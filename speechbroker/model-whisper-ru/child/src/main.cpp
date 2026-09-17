// The child: one process, one model, one pipe.
//
// It exists because of a measured number. A GPU runtime that cannot reach one of
// its own sub-libraries ends its process with exit code 127 - no exception of
// any kind, past every handler and past the player's crash logger, leaving no
// log at all. Inside SkyrimVR.exe that is the game gone with nothing to read. In
// here it is a number GetExitCodeProcess returns to the shim, which turns it
// into an ordinary failure and lets the other models carry on.
//
// SO THE ONE THING THIS PROGRAM MUST NEVER DO IS OUTLIVE THE GAME. Two
// mechanisms hold it, and both are wanted: the shim puts this process in a Job
// Object with kill-on-close, and --parent-pid arms the watchdog below. The job
// is the guarantee; the watchdog covers the case where assigning to the job
// failed, which the shim logs rather than treats as fatal.
//
// IT RENDERS NO TEXT. Every diagnostic is a localisation key of this module's
// own table and its arguments, sent up the pipe as a Log frame; the shim hands
// them to the adapter's Host::Log verbatim and the adapter writes them down. A
// console this process does not have is not a place to put a message.

#include "Recogniser.h"

#include "Wire.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <Windows.h>

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace WhisperRu;

	// The endings, and the shim knows every one of them by number. They are
	// written down in docs/child-protocol.md because an exit code nobody has
	// written down is a number in a log that means nothing.
	constexpr int kExitOk = 0;
	constexpr int kExitBadArguments = 2;
	constexpr int kExitNoBackend = 3;
	constexpr int kExitNoWeights = 4;

	HANDLE g_out = nullptr;
	HANDLE g_in = nullptr;

	bool WriteAll(const std::vector<std::uint8_t>& a_frame)
	{
		std::size_t sent = 0;
		while (sent < a_frame.size()) {
			DWORD written = 0;
			const DWORD want = static_cast<DWORD>(
				(a_frame.size() - sent) > 0x10000 ? 0x10000 : (a_frame.size() - sent));
			if (!::WriteFile(g_out, a_frame.data() + sent, want, &written, nullptr) || written == 0) {
				return false;
			}
			sent += written;
		}
		return true;
	}

	bool ReadAll(std::uint8_t* a_into, std::size_t a_count)
	{
		std::size_t got = 0;
		while (got < a_count) {
			DWORD read = 0;
			const DWORD want = static_cast<DWORD>(
				(a_count - got) > 0x10000 ? 0x10000 : (a_count - got));
			if (!::ReadFile(g_in, a_into + got, want, &read, nullptr) || read == 0) {
				return false;  // end of pipe: the shim has gone, and so do we
			}
			got += read;
		}
		return true;
	}

	void Tell(std::int32_t a_level, const std::string& a_key, std::vector<std::string> a_args)
	{
		Wire::LogLine line;
		line.level = a_level;
		line.key = a_key;
		line.args = std::move(a_args);
		WriteAll(Wire::EncodeLog(line));
	}

	// The watchdog. It waits on the parent and leaves when it goes - no polling,
	// no timer, one blocking wait on a handle the kernel signals.
	void WatchParent(DWORD a_pid)
	{
		HANDLE parent = ::OpenProcess(SYNCHRONIZE, FALSE, a_pid);
		if (!parent) {
			// The parent is already gone, or we may not wait on it. Either way
			// there is nothing here to serve.
			::ExitProcess(kExitOk);
		}
		::WaitForSingleObject(parent, INFINITE);
		// ExitProcess and not a return: this runs on a thread of its own while
		// the main loop is blocked reading a pipe whose far end has just died,
		// and the point is to leave NOW rather than to unwind tidily.
		::ExitProcess(kExitOk);
	}

	struct Arguments
	{
		std::string modelId;
		std::string device;
		std::string computeType;
		std::string language{ "ru" };
		std::filesystem::path weights;
		std::filesystem::path library;
		std::vector<std::filesystem::path> preload;
		std::int32_t beamSize{ 5 };
		std::int32_t threads{ 0 };
		std::uint32_t maxSamples{ 0 };
		DWORD parentPid{ 0 };
	};

	bool Parse(int a_count, char** a_values, Arguments& a_out)
	{
		for (int i = 1; i < a_count; ++i) {
			const std::string name = a_values[i];
			const bool hasValue = (i + 1) < a_count;
			const std::string value = hasValue ? a_values[i + 1] : std::string{};
			if (!hasValue) {
				return false;
			}
			++i;
			if (name == "--model-id") {
				a_out.modelId = value;
			} else if (name == "--weights") {
				a_out.weights = std::filesystem::path(value);
			} else if (name == "--library") {
				a_out.library = std::filesystem::path(value);
			} else if (name == "--preload") {
				a_out.preload.emplace_back(value);
			} else if (name == "--device") {
				a_out.device = value;
			} else if (name == "--compute-type") {
				a_out.computeType = value;
			} else if (name == "--language") {
				a_out.language = value;
			} else if (name == "--beam-size") {
				a_out.beamSize = std::atoi(value.c_str());
			} else if (name == "--threads") {
				a_out.threads = std::atoi(value.c_str());
			} else if (name == "--max-samples") {
				a_out.maxSamples = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
			} else if (name == "--parent-pid") {
				a_out.parentPid = static_cast<DWORD>(std::strtoul(value.c_str(), nullptr, 10));
			} else {
				// An argument we do not know is not fatal: the shim and the
				// child ship together, but a person debugging by hand should be
				// able to pass something harmless without being thrown out.
			}
		}
		return !a_out.weights.empty() && !a_out.library.empty();
	}
}

int main(int a_count, char** a_values)
{
	// stdin and stdout are a BINARY pipe. Without this the C runtime turns every
	// 0x0A inside a float into 0x0D 0x0A on the way out, which corrupts roughly
	// one byte in two hundred of the samples and is invisible until the answers
	// are wrong.
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	g_in = ::GetStdHandle(STD_INPUT_HANDLE);
	g_out = ::GetStdHandle(STD_OUTPUT_HANDLE);

	Arguments arguments;
	if (!Parse(a_count, a_values, arguments)) {
		Tell(2, "$SBWHISPERRU_LOG_CHILD_BAD_ARGUMENTS", {});
		return kExitBadArguments;
	}
	if (arguments.parentPid != 0) {
		std::thread(WatchParent, arguments.parentPid).detach();
	}

	auto recogniser = Child::MakeWhisperRecogniser();
	Child::Recogniser::Options options;
	options.weights = arguments.weights;
	options.library = arguments.library;
	options.preload = arguments.preload;
	options.device = arguments.device;
	options.computeType = arguments.computeType;
	options.language = arguments.language;
	options.beamSize = arguments.beamSize;
	options.threads = arguments.threads;

	Child::Refusal why;
	if (!recogniser->Open(options, why)) {
		// The clean refusal. One line naming the file that is missing, and an
		// exit code the shim turns into a permanent refusal rather than a retry
		// - because no amount of waiting installs a DLL.
		Tell(3, why.key, why.args);
		const bool aboutWeights = why.key == "$SBWHISPERRU_LOG_WEIGHTS_MISSING";
		return aboutWeights ? kExitNoWeights : kExitNoBackend;
	}

	Wire::Hello hello;
	hello.protocolVersion = Wire::kVersion;
	hello.maxSamples = arguments.maxSamples;
	hello.backend = "whisper.cpp";
	hello.modelId = arguments.modelId;
	if (!WriteAll(Wire::EncodeHello(hello))) {
		return kExitOk;
	}

	std::vector<std::uint8_t> body;
	for (;;) {
		std::uint8_t header[Wire::kHeaderBytes]{};
		if (!ReadAll(header, sizeof(header))) {
			return kExitOk;  // the shim closed our stdin, or it has gone
		}
		Wire::Header parsed{};
		if (!Wire::DecodeHeader(header, sizeof(header), parsed)) {
			// There is no resynchronising from a frame we cannot identify: the
			// next bytes would be read as a length. Leaving is the honest
			// answer, and the shim sees the exit code.
			Tell(3, "$SBWHISPERRU_LOG_CHILD_FRAME_BROKEN", { "header" });
			return kExitBadArguments;
		}
		body.resize(parsed.bodyBytes);
		if (parsed.bodyBytes != 0 && !ReadAll(body.data(), body.size())) {
			return kExitOk;
		}

		switch (static_cast<Wire::Type>(parsed.type)) {
		case Wire::Type::Bye:
			return kExitOk;

		case Wire::Type::Vocabulary:
			{
				std::vector<std::string> phrases;
				if (Wire::DecodeVocabulary(body.data(), body.size(), phrases)) {
					recogniser->SetVocabulary(phrases);
				}
				break;
			}

		case Wire::Type::Cancel:
			// Advisory, and this child cannot honour it: one inference runs to
			// the end on this thread. Answering it would mean a second thread
			// and an abortable backend, and the shim already pays the debt from
			// the reply that follows. The frame is read and dropped rather than
			// treated as unknown, so the log does not fill with a message about
			// something working as intended.
			break;

		case Wire::Type::Request:
			{
				Wire::Request request;
				if (!Wire::DecodeRequest(body.data(), body.size(), request)) {
					Tell(2, "$SBWHISPERRU_LOG_CHILD_FRAME_BROKEN", { "request" });
					break;
				}
				const auto began = std::chrono::steady_clock::now();
				std::vector<Child::Piece> pieces;
				std::string failed;
				const bool ok = recogniser->Recognise(request.samples, 16000, pieces, failed);

				Wire::Reply reply;
				reply.utteranceId = request.utteranceId;
				reply.serial = request.serial;
				reply.status = ok ? Wire::kStatusOk : Wire::kStatusFailed;
				reply.latencyMs = static_cast<std::int32_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::steady_clock::now() - began).count());
				reply.lostSamples = 0;
				reply.failed = failed;
				reply.fragments.reserve(pieces.size());
				for (auto& piece : pieces) {
					Wire::Fragment fragment;
					fragment.startMs = piece.startMs;
					fragment.endMs = piece.endMs;
					fragment.score = piece.score;
					fragment.endsSentence = piece.endsSentence;
					fragment.lastWordProb = piece.lastWordProb;
					fragment.noSpeechProb = piece.noSpeechProb;
					fragment.medianGapMs = piece.medianGapMs;
					fragment.words = piece.words;
					fragment.text = std::move(piece.text);
					reply.fragments.push_back(std::move(fragment));
				}
				// Exactly one reply per request, always, whatever happened. The
				// shim owes the adapter exactly one Complete per accepted
				// utterance and this is where it comes from.
				if (!WriteAll(Wire::EncodeReply(reply))) {
					return kExitOk;
				}
				break;
			}

		default:
			Tell(2, "$SBWHISPERRU_LOG_CHILD_FRAME_UNKNOWN",
				{ std::to_string(static_cast<int>(parsed.type)) });
			break;
		}
	}
}
