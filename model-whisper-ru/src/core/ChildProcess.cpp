#include "ChildProcess.h"

#include "Log.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <Windows.h>

namespace WhisperRu
{
	namespace
	{
		// One job for every child this DLL ever raises. It is a function-local
		// static so that it is created on first use and never destroyed: the
		// handle is closed by the kernel when the process ends, which is exactly
		// the moment the children must die, and running a destructor for it
		// during static teardown would only give us a chance to close it EARLY
		// and kill a child that is still wanted.
		HANDLE Job()
		{
			static HANDLE job = []() -> HANDLE {
				HANDLE created = ::CreateJobObjectW(nullptr, nullptr);
				if (!created) {
					return nullptr;
				}
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (!::SetInformationJobObject(created, JobObjectExtendedLimitInformation,
					    &limits, sizeof(limits))) {
					::CloseHandle(created);
					return nullptr;
				}
				return created;
			}();
			return job;
		}

		std::wstring Widen(const std::string& a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int wanted = ::MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(),
				static_cast<int>(a_text.size()), nullptr, 0);
			if (wanted <= 0) {
				return {};
			}
			std::wstring out(static_cast<std::size_t>(wanted), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), static_cast<int>(a_text.size()),
				out.data(), wanted);
			return out;
		}

		// The command line is built by the rules CommandLineToArgvW parses back,
		// because the child reads its arguments with the ordinary runtime and a
		// path with a space in it is the normal case here: mods live under
		// "Skyrim VR Modding".
		void AppendArgument(std::wstring& a_line, const std::wstring& a_argument)
		{
			if (!a_line.empty()) {
				a_line.push_back(L' ');
			}
			const bool needsQuotes = a_argument.empty() ||
				a_argument.find_first_of(L" \t\"") != std::wstring::npos;
			if (!needsQuotes) {
				a_line.append(a_argument);
				return;
			}
			a_line.push_back(L'"');
			std::size_t backslashes = 0;
			for (const wchar_t c : a_argument) {
				if (c == L'\\') {
					++backslashes;
					continue;
				}
				if (c == L'"') {
					a_line.append(backslashes * 2 + 1, L'\\');
				} else {
					a_line.append(backslashes, L'\\');
				}
				backslashes = 0;
				a_line.push_back(c);
			}
			a_line.append(backslashes * 2, L'\\');
			a_line.push_back(L'"');
		}

		bool ReadExactly(HANDLE a_pipe, std::uint8_t* a_into, std::size_t a_count)
		{
			std::size_t got = 0;
			while (got < a_count) {
				DWORD read = 0;
				const DWORD want = static_cast<DWORD>(
					(a_count - got) > 0x10000 ? 0x10000 : (a_count - got));
				if (!::ReadFile(a_pipe, a_into + got, want, &read, nullptr) || read == 0) {
					return false;
				}
				got += read;
			}
			return true;
		}

		bool WriteExactly(HANDLE a_pipe, const std::uint8_t* a_from, std::size_t a_count)
		{
			std::size_t sent = 0;
			while (sent < a_count) {
				DWORD written = 0;
				const DWORD want = static_cast<DWORD>(
					(a_count - sent) > 0x10000 ? 0x10000 : (a_count - sent));
				if (!::WriteFile(a_pipe, a_from + sent, want, &written, nullptr) || written == 0) {
					return false;
				}
				sent += written;
			}
			return true;
		}
	}

	std::filesystem::path PreloadByFullPath(const std::vector<std::filesystem::path>& a_files)
	{
		for (const auto& file : a_files) {
			std::error_code ec;
			if (!std::filesystem::exists(file, ec)) {
				return file;
			}
			if (!::LoadLibraryW(file.c_str())) {
				return file;
			}
		}
		return {};
	}

	std::filesystem::path ThisModuleFolder()
	{
		HMODULE self = nullptr;
		if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&ThisModuleFolder), &self)) {
			return {};
		}
		std::wstring path(MAX_PATH, L'\0');
		for (;;) {
			const DWORD got = ::GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
			if (got == 0) {
				return {};
			}
			if (got < path.size()) {
				path.resize(got);
				break;
			}
			// A path longer than MAX_PATH is not exotic under MO2, whose virtual
			// tree adds a prefix of its own to everything.
			path.resize(path.size() * 2);
		}
		return std::filesystem::path(path).parent_path();
	}

	ChildProcess::~ChildProcess()
	{
		Close();
	}

	bool ChildProcess::Start(const Options& a_options, std::string& a_failReason)
	{
		Close();

		SECURITY_ATTRIBUTES inherit{};
		inherit.nLength = sizeof(inherit);
		inherit.bInheritHandle = TRUE;

		HANDLE childReads = nullptr;   // the child's stdin
		HANDLE weWrite = nullptr;
		HANDLE weRead = nullptr;
		HANDLE childWrites = nullptr;  // the child's stdout
		if (!::CreatePipe(&childReads, &weWrite, &inherit, 0) ||
			!::CreatePipe(&weRead, &childWrites, &inherit, 0)) {
			a_failReason = std::to_string(::GetLastError());
			return false;
		}
		// Only the child's ends are inheritable. Ours must not be, or the child
		// holds a copy of its own read end and never sees end of file when we
		// close ours - which is the orderly stop failing silently.
		::SetHandleInformation(weWrite, HANDLE_FLAG_INHERIT, 0);
		::SetHandleInformation(weRead, HANDLE_FLAG_INHERIT, 0);

		std::wstring line;
		AppendArgument(line, a_options.exe.wstring());
		for (const auto& argument : a_options.args) {
			AppendArgument(line, Widen(argument));
		}
		AppendArgument(line, L"--parent-pid");
		AppendArgument(line, std::to_wstring(::GetCurrentProcessId()));

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		startup.hStdInput = childReads;
		startup.hStdOutput = childWrites;
		// The child's own diagnostics do NOT go down stderr to a console nobody
		// will see: they are Log frames on stdout, keys and arguments, which the
		// shim relays through Host::Log into the adapter's log. So stderr is
		// simply the one the game had.
		startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

		PROCESS_INFORMATION process{};
		// CREATE_SUSPENDED and not otherwise: the child must be in the job
		// BEFORE its first instruction, or a child that dies in its own
		// start-up between CreateProcess and AssignProcessToJobObject was never
		// in the job at all. CREATE_NO_WINDOW keeps a console from flashing over
		// a game that is running full screen in a headset.
		const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW;
		std::wstring workingDir = a_options.workingDir.wstring();
		if (!::CreateProcessW(a_options.exe.c_str(), line.data(), nullptr, nullptr, TRUE, flags,
			    nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &startup, &process)) {
			a_failReason = std::to_string(::GetLastError());
			::CloseHandle(childReads);
			::CloseHandle(childWrites);
			::CloseHandle(weWrite);
			::CloseHandle(weRead);
			return false;
		}

		if (HANDLE job = Job(); job != nullptr) {
			if (!::AssignProcessToJobObject(job, process.hProcess)) {
				// Not fatal, and said out loud rather than swallowed: the
				// --parent-pid watchdog still covers the child, but the
				// guarantee is now only as good as the child's own code.
				Log::Warn("$SBWHISPERRU_LOG_CHILD_NO_JOB", static_cast<std::int32_t>(::GetLastError()));
			}
		} else {
			Log::Warn("$SBWHISPERRU_LOG_CHILD_NO_JOB", static_cast<std::int32_t>(::GetLastError()));
		}

		::ResumeThread(process.hThread);
		::CloseHandle(process.hThread);
		::CloseHandle(childReads);
		::CloseHandle(childWrites);

		m_process = process.hProcess;
		m_stdIn = weWrite;
		m_stdOut = weRead;
		return true;
	}

	bool ChildProcess::ReadFrame(Wire::Header& a_header, std::vector<std::uint8_t>& a_body)
	{
		if (!m_stdOut) {
			return false;
		}
		std::uint8_t header[Wire::kHeaderBytes]{};
		if (!ReadExactly(m_stdOut, header, sizeof(header))) {
			return false;
		}
		if (!Wire::DecodeHeader(header, sizeof(header), a_header)) {
			// A frame we cannot even identify means the stream is lost. There is
			// no resynchronising from here and pretending otherwise would have
			// us reading a length out of the middle of somebody's samples.
			return false;
		}
		a_body.resize(a_header.bodyBytes);
		return a_header.bodyBytes == 0 || ReadExactly(m_stdOut, a_body.data(), a_body.size());
	}

	bool ChildProcess::WriteFrame(const std::vector<std::uint8_t>& a_frame)
	{
		std::lock_guard guard(m_writeLock);
		if (!m_stdIn) {
			return false;
		}
		return WriteExactly(m_stdIn, a_frame.data(), a_frame.size());
	}

	bool ChildProcess::Alive() const
	{
		if (!m_process) {
			return false;
		}
		return ::WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
	}

	bool ChildProcess::ExitCode(std::uint32_t& a_code) const
	{
		if (!m_process) {
			return false;
		}
		DWORD code = 0;
		if (!::GetExitCodeProcess(m_process, &code) || code == STILL_ACTIVE) {
			return false;
		}
		a_code = code;
		return true;
	}

	void ChildProcess::CloseInput()
	{
		std::lock_guard guard(m_writeLock);
		if (m_stdIn) {
			::CloseHandle(m_stdIn);
			m_stdIn = nullptr;
		}
	}

	void ChildProcess::Terminate()
	{
		if (m_process) {
			::TerminateProcess(m_process, 1);
		}
	}

	void ChildProcess::Close()
	{
		CloseInput();
		if (m_stdOut) {
			::CloseHandle(m_stdOut);
			m_stdOut = nullptr;
		}
		if (m_process) {
			::CloseHandle(m_process);
			m_process = nullptr;
		}
	}
}
