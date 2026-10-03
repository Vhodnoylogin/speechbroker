// The child, and why there is one at all.
//
// The contract lets a shim BE the model, RAISE one, or ATTACH to one. Ours
// raises one, and the reason is a measured number rather than caution: on this
// machine, 17.09.2026, loading cudnn64_9.dll with a sub-library unreachable ENDS
// THE PROCESS WITH EXIT CODE 127 - no exception of any kind, past every handler
// in the process and past the player's crash logger, leaving no log at all. In
// SkyrimVR.exe that is the game gone with nothing to read. In a child it is a
// number GetExitCodeProcess returns, which the shim turns into an ordinary
// failure. The argument is against dragging a third-party GPU runtime into the
// game's process, not against in-process model mods in general.
//
// THE CHILD IS TIED TO THE PROCESS, NOT TO Stop, and this is the part that must
// not be simplified away. The contract says plainly that Stop MAY NEVER BE
// CALLED AT ALL - SKSE sends no shutdown message and a plugin dies with the
// process - so anything that must not outlive the game cannot be tied to it. A
// surviving child keeps the graphics card and, worse in daily use, keeps MO2
// believing the game is still running, which means the build cannot be edited.
// Two mechanisms, and BOTH are here because each covers what the other does not:
//
//   - a Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE. The handle is a
//     static of this DLL, so the kernel closes it when SkyrimVR.exe ends, by any
//     means at all - a clean exit, a crash, Task Manager - and every child in
//     the job dies with it. This is the guarantee.
//   - --parent-pid on the child's command line. The child watches that pid and
//     leaves when it goes. This is the belt to the job's braces, and it is what
//     covers the one case the job does not: a machine or a container where
//     assigning to the job failed, which is logged rather than fatal.
#pragma once

#include "Wire.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace WhisperRu
{
	// Loads each of a_files by FULL PATH, in order. Returns the first that could
	// not be loaded, or an empty path when all of them came up.
	//
	// FULL PATH IS THE WHOLE POINT and the contract spends a paragraph on it:
	// changing PATH or calling SetDefaultDllDirectories alters bare-name
	// resolution for every later LoadLibrary in the process, including other
	// plugins' delay-loaded imports and the game's own lazy loads, and it cannot
	// be scoped to one caller. A module already loaded by full path is found by a
	// later bare-name load with no policy change at all. The shim keeps this rule
	// even though its own preload list ships empty, because a fork of this folder
	// that runs its model INPROCESS needs the mechanism to be here and right.
	std::filesystem::path PreloadByFullPath(const std::vector<std::filesystem::path>& a_files);

	// The folder this very DLL was loaded from. Asked of the loader rather than
	// built out of the game's folder, because MO2 shows the plugin a virtual
	// tree and the answer has to be the path the plugin is actually living at.
	// It is also the only way a module gets its own location without a constant
	// naming somebody's disk.
	std::filesystem::path ThisModuleFolder();

	// One raised child. Not copyable: it owns three kernel handles and a pipe
	// whose far end is a process.
	class ChildProcess
	{
	public:
		ChildProcess() = default;
		ChildProcess(const ChildProcess&) = delete;
		ChildProcess& operator=(const ChildProcess&) = delete;
		~ChildProcess();

		struct Options
		{
			std::filesystem::path exe;
			std::filesystem::path workingDir;
			std::vector<std::string> args;  // --parent-pid is appended by Start
		};

		// False when the child could not be raised at all; a_failReason then
		// holds the Win32 error number as text, for the log line.
		bool Start(const Options& a_options, std::string& a_failReason);

		// Reads one whole frame. Blocks until it has one, or answers false at
		// end of pipe - which is what a dead child looks like from here.
		bool ReadFrame(Wire::Header& a_header, std::vector<std::uint8_t>& a_body);

		// Serialised against itself: the worker writes requests and Stop writes
		// the farewell, and a torn frame would desynchronise the child for good.
		bool WriteFrame(const std::vector<std::uint8_t>& a_frame);

		bool Alive() const;

		// The number this whole design exists to obtain. False while the child
		// is still running.
		bool ExitCode(std::uint32_t& a_code) const;

		// Closes our end of the child's stdin. The child sees end of file and
		// leaves of its own accord - the orderly ending, which costs nothing and
		// is tried before the abrupt one.
		void CloseInput();

		// The abrupt ending, for our OWN child and nobody else's. Safe here in a
		// way TerminateThread never is: a whole process dies with its locks, and
		// nothing of ours is inside it.
		void Terminate();

	private:
		void Close();

		void*      m_process{ nullptr };
		void*      m_stdIn{ nullptr };
		void*      m_stdOut{ nullptr };
		std::mutex m_writeLock;
	};
}
