// The microphone, and the wav that stands in for it.
//
// These two macros are set before anything else is included, because spdlog
// reaches for <Windows.h> on its own and whoever gets there first decides what
// the header defines. They are guarded rather than defined outright: the build
// may already pass them on the command line, and an identical redefinition is
// silent while a different one is C4005, which /WX turns into a failure.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif

#include "audio/Capture.h"

#include "Loc.h"
#include "audio/Devices.h"

#include <Windows.h>
// mmreg.h by name: WIN32_LEAN_AND_MEAN drops mmsystem.h, and WAVEFORMATEX with
// it. Naming it here means the capture does not depend on which of the audio
// headers happens to pull it in this year.
#include <mmreg.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Voice
{
	namespace
	{
		// ------------------------------------------------------- sample formats
		//
		// These are not tunables and must never move into the settings: they are
		// the names and the arithmetic of the formats themselves. A player has
		// nothing to choose here - the endpoint says what it gives, and we either
		// can convert it or cannot.
		constexpr std::uint32_t kWaveFormatPcm = 0x0001;
		constexpr std::uint32_t kWaveFormatFloat = 0x0003;
		constexpr std::uint32_t kWaveFormatExtensible = 0xFFFE;

		// A WAVE_FORMAT_EXTENSIBLE sub-format is {0000000X-0000-0010-8000-00AA00389B71},
		// and the X is the plain format tag. Comparing Data1 rather than the whole
		// GUID is what keeps <ksmedia.h> - which brings the whole kernel streaming
		// tree with it - out of this file, and it is the same test for the mix
		// format of an endpoint and for the fmt chunk of a wav.
		constexpr std::uint32_t kExtensibleTail = 22;  // cbSize of a full extensible block

		constexpr float kInt16Scale = 1.0f / 32768.0f;
		constexpr float kInt32Scale = 1.0f / 2147483648.0f;

		// One millisecond in the hundred-nanosecond units WASAPI asks its
		// durations in.
		constexpr std::int64_t kHundredNsPerMs = 10000;

		enum class PcmKind
		{
			Unsupported,
			Float32,
			Float64,
			Int16,
			Int24,
			Int32
		};

		// What is arriving, resolved ONCE when a source opens. Nothing in the
		// delivery step decides anything about a format: it switches on a value
		// that was worked out at open and does arithmetic.
		struct StreamFormat
		{
			PcmKind       kind{ PcmKind::Unsupported };
			std::uint32_t rate{ 0 };
			std::uint32_t channels{ 0 };
			std::uint32_t frameBytes{ 0 };  // nBlockAlign: one frame, every channel
			std::uint32_t bits{ 0 };
			std::uint32_t tag{ 0 };
		};

		PcmKind KindOf(std::uint32_t a_tag, std::uint32_t a_bits) noexcept
		{
			if (a_tag == kWaveFormatFloat) {
				if (a_bits == 32) {
					return PcmKind::Float32;
				}
				if (a_bits == 64) {
					return PcmKind::Float64;
				}
				return PcmKind::Unsupported;
			}
			if (a_tag == kWaveFormatPcm) {
				switch (a_bits) {
				case 16:
					return PcmKind::Int16;
				case 24:
					return PcmKind::Int24;
				case 32:
					return PcmKind::Int32;
				default:
					// Eight-bit unsigned PCM is left out on purpose rather than by
					// oversight: no capture endpoint of this decade offers it, and a
					// path nothing exercises is a path that is wrong when it is
					// finally taken.
					return PcmKind::Unsupported;
				}
			}
			return PcmKind::Unsupported;
		}

		// THE CONVERSION THE DELIVERY STEP IS ALLOWED, and the only arithmetic in
		// it: a straight loop over the frames with no memory of its own and no
		// branch inside the loop - the kind is switched on once, outside.
		//
		// CHANNEL ZERO, NOT AN AVERAGE. The reference took column zero
		// (engine/audio.py:115, voice-service.py:196) and every level in the
		// settings was tuned against what that produced. An average would look
		// tidier and would be wrong twice over: on an endpoint whose second
		// channel is dead - which headsets do ship - it halves the level, and
		// vad.minRmsFloor is an absolute number, not a relative one.
		void ToMono(const std::byte* a_from, std::uint32_t a_frames, const StreamFormat& a_format,
			Sample* a_into) noexcept
		{
			const std::size_t stride = a_format.frameBytes;

			switch (a_format.kind) {
			case PcmKind::Float32:
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					float value = 0.0f;
					// memcpy rather than a cast: a packet is not promised to be
					// aligned for a float, and a misaligned load is undefined even
					// where the processor tolerates it.
					std::memcpy(&value, a_from + i * stride, sizeof(value));
					a_into[i] = value;
				}
				break;

			case PcmKind::Float64:
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					double value = 0.0;
					std::memcpy(&value, a_from + i * stride, sizeof(value));
					a_into[i] = static_cast<Sample>(value);
				}
				break;

			case PcmKind::Int16:
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					std::int16_t value = 0;
					std::memcpy(&value, a_from + i * stride, sizeof(value));
					a_into[i] = static_cast<Sample>(value) * kInt16Scale;
				}
				break;

			case PcmKind::Int24:
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					const auto* bytes = reinterpret_cast<const unsigned char*>(a_from + i * stride);
					// The three bytes are shifted into the TOP of a 32-bit word, so
					// the sign extends itself and one scale serves both integer
					// widths. Reading them into the bottom would need a sign fix-up
					// per sample, which is a branch in the one loop that must not
					// have one.
					const auto raw = (static_cast<std::uint32_t>(bytes[0]) << 8) |
					                 (static_cast<std::uint32_t>(bytes[1]) << 16) |
					                 (static_cast<std::uint32_t>(bytes[2]) << 24);
					a_into[i] = static_cast<Sample>(static_cast<std::int32_t>(raw)) * kInt32Scale;
				}
				break;

			case PcmKind::Int32:
				for (std::uint32_t i = 0; i < a_frames; ++i) {
					std::int32_t value = 0;
					std::memcpy(&value, a_from + i * stride, sizeof(value));
					a_into[i] = static_cast<Sample>(value) * kInt32Scale;
				}
				break;

			case PcmKind::Unsupported:
			default:
				// Unreachable: a source whose kind did not resolve is refused at
				// open and never opened. Silence rather than noise if it ever is.
				std::fill_n(a_into, a_frames, Sample{ 0 });
				break;
			}
		}

		// A code, not a sentence. An HRESULT carries no words, so it does not go
		// through the translation table - it goes into one as a value, in the
		// notation anybody looking it up will type.
		std::string Code(HRESULT a_hr)
		{
			return fmt::format("0x{:08X}", static_cast<std::uint32_t>(a_hr));
		}

		// A path out of the settings is relative and stays where it was pointed.
		// This is the rule ResolveInside applies in Config.cpp, written out again
		// rather than shared, because NOTHING UNDER audio/ MAY INCLUDE Config.h -
		// that is exactly what lets this half build and run from a wav with no
		// game around it. The base is the working directory, which is the folder
		// the game runs from and the one every other relative path in this adapter
		// is already taken from (Config.cpp, kHome).
		//
		// An empty string on the way out means a refusal.
		std::string ResolveInside(const std::string& a_path)
		{
			if (a_path.empty()) {
				return {};
			}
			std::error_code       oops;
			std::filesystem::path given{ a_path };
			if (given.is_absolute() || given.has_root_name()) {
				return {};
			}
			const auto base = std::filesystem::current_path(oops);
			if (oops) {
				return {};
			}
			const auto full = (base / given).lexically_normal();
			const auto root = base.lexically_normal();
			// lexically_relative puts a ".." at the front exactly when the path
			// climbed above the root. Comparing strings will not do: "voice-evil"
			// begins with "voice".
			const auto rel = full.lexically_relative(root);
			if (rel.empty() || *rel.begin() == "..") {
				return {};
			}
			return full.string();
		}

		std::uint32_t ReadU32(const std::byte* a_from) noexcept
		{
			std::uint32_t value = 0;
			std::memcpy(&value, a_from, sizeof(value));
			return value;
		}

		std::uint16_t ReadU16(const std::byte* a_from) noexcept
		{
			std::uint16_t value = 0;
			std::memcpy(&value, a_from, sizeof(value));
			return value;
		}

		bool SameTag(const std::byte* a_four, const char* a_tag) noexcept
		{
			return std::memcmp(a_four, a_tag, 4) == 0;
		}

		// ------------------------------------------------------------ the wav
		//
		// A recording standing in for the microphone. It is not a debugging
		// afterthought: without it this whole half can only be tried by putting on
		// a headset and speaking, and no two attempts are alike (Capture.h).
		//
		// It reads the file in blocks rather than swallowing it: a five minute
		// take at 48 kHz stereo float is 230 MB, and a test that needs a quarter
		// of a gigabyte of resident memory is a test nobody runs twice.
		class WavSource
		{
		public:
			// Opens and parses. Says in the log why it refused, because "the file
			// did not open" without the reason is a report nobody can act on.
			bool Open(const std::string& a_setting)
			{
				if (a_setting.empty()) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_NOT_SET");
					return false;
				}
				_path = ResolveInside(a_setting);
				if (_path.empty()) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_OUTSIDE", a_setting);
					return false;
				}

				_file.open(_path, std::ios::binary);
				if (!_file) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_MISSING", _path);
					return false;
				}

				_file.seekg(0, std::ios::end);
				const auto size = static_cast<std::uint64_t>(static_cast<std::streamoff>(_file.tellg()));
				_file.seekg(0, std::ios::beg);

				std::byte header[12]{};
				if (!Take(header, sizeof(header)) || !SameTag(header, "RIFF") ||
					!SameTag(header + 8, "WAVE")) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_BROKEN", _path, "RIFF");
					return false;
				}

				bool haveFormat = false;
				while (_file) {
					std::byte chunk[8]{};
					if (!Take(chunk, sizeof(chunk))) {
						break;
					}
					const auto length = ReadU32(chunk + 4);

					if (SameTag(chunk, "fmt ")) {
						// Enough for a full WAVE_FORMAT_EXTENSIBLE and not a byte
						// more; anything longer is skipped rather than refused.
						std::byte body[40]{};
						const auto want = std::min<std::uint32_t>(length, static_cast<std::uint32_t>(sizeof(body)));
						if (!Take(body, want)) {
							break;
						}
						Skip(length - want);
						if (want < 16) {
							Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_BROKEN", _path, "fmt ");
							return false;
						}
						auto tag = static_cast<std::uint32_t>(ReadU16(body));
						_format.channels = ReadU16(body + 2);
						_format.rate = ReadU32(body + 4);
						_format.frameBytes = ReadU16(body + 12);
						_format.bits = ReadU16(body + 14);
						if (tag == kWaveFormatExtensible && want >= 16 + 2 + kExtensibleTail) {
							// The sub-format GUID begins at cbSize + validBits +
							// channelMask; its Data1 is the plain tag.
							tag = ReadU32(body + 24);
						}
						_format.tag = tag;
						_format.kind = KindOf(tag, _format.bits);
						haveFormat = true;
					} else if (SameTag(chunk, "data")) {
						_dataAt = static_cast<std::uint64_t>(static_cast<std::streamoff>(_file.tellg()));
						// A writer that could not seek back leaves 0xFFFFFFFF here,
						// and a take cut short by a crash leaves a length longer
						// than the file. What is on the disk decides.
						_dataBytes = std::min<std::uint64_t>(length, size - _dataAt);
						Skip(length);
					} else {
						Skip(length);
					}

					if ((length & 1u) != 0) {
						Skip(1);  // RIFF pads every odd chunk to an even boundary
					}
					if (haveFormat && _dataBytes != 0) {
						break;
					}
				}

				if (!haveFormat || _dataAt == 0) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_BROKEN", _path, "data");
					return false;
				}
				if (_format.kind == PcmKind::Unsupported || _format.channels == 0 ||
					_format.rate == 0 || _format.frameBytes == 0) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_UNSUPPORTED", _path, _format.bits,
						_format.tag, _format.channels);
					return false;
				}
				if (_dataBytes < _format.frameBytes) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_EMPTY", _path);
					return false;
				}

				Rewind();
				return true;
			}

			// Frames actually read, which is short only at the end of the take.
			std::uint32_t Read(std::byte* a_into, std::uint32_t a_frames)
			{
				const auto want = std::min<std::uint64_t>(
					static_cast<std::uint64_t>(a_frames) * _format.frameBytes, _left);
				if (want == 0) {
					return 0;
				}
				if (!Take(a_into, static_cast<std::size_t>(want))) {
					_left = 0;
					return 0;
				}
				_left -= want;
				return static_cast<std::uint32_t>(want / _format.frameBytes);
			}

			void Rewind()
			{
				_file.clear();
				_file.seekg(static_cast<std::streamoff>(_dataAt), std::ios::beg);
				_left = _dataBytes;
			}

			const StreamFormat& Format() const noexcept { return _format; }
			const std::string&  Path() const noexcept { return _path; }
			std::uint64_t       Frames() const noexcept { return _dataBytes / _format.frameBytes; }

		private:
			bool Take(std::byte* a_into, std::size_t a_bytes)
			{
				_file.read(reinterpret_cast<char*>(a_into), static_cast<std::streamsize>(a_bytes));
				return static_cast<std::size_t>(_file.gcount()) == a_bytes;
			}

			void Skip(std::uint64_t a_bytes)
			{
				if (a_bytes != 0) {
					_file.seekg(static_cast<std::streamoff>(a_bytes), std::ios::cur);
				}
			}

			std::ifstream _file;
			std::string   _path;
			StreamFormat  _format;
			std::uint64_t _dataAt{ 0 };
			std::uint64_t _dataBytes{ 0 };
			std::uint64_t _left{ 0 };
		};
	}

	// ---------------------------------------------------------------- the backend
	//
	// Everything WASAPI, everything COM and the wav reader, named in no header at
	// all. The members below divide by thread and the division is the whole of the
	// safety here: what the pump touches, the owner does not, and the other way
	// about.
	struct Capture::Backend
	{
		Backend(const CaptureSettings& a_settings, Ring& a_ring) :
			settings(a_settings),
			ring(a_ring)
		{}

		// ---- immutable for the life of the capture
		const CaptureSettings& settings;
		Ring&                  ring;

		// ---- published: written by the pump, read by anybody
		//
		// The rate and the channel count live in ONE 64-bit value because a
		// consumer that read them as two atomics could catch the rate of the new
		// device with the channel count of the old one, which is a wrong
		// resampling ratio and a silent one (Capture.h).
		std::atomic<std::uint64_t> format{ 0 };
		std::atomic<std::uint32_t> epoch{ 0 };
		std::atomic<std::uint64_t> opens{ 0 };
		std::atomic_bool           running{ false };
		std::atomic_bool           stopping{ false };
		std::atomic_bool           finished{ false };

		// The one lock this class has. It is taken when a device opens and by
		// Name(), and never by the delivery step.
		mutable std::mutex nameLock;
		std::string        name;

		// ---- owner thread
		std::thread        pump;
		std::promise<bool> firstOpen;

		// Signalled by WASAPI when a packet is ready, and by Stop to break every
		// wait in this file at once. Two handles rather than one: the capture
		// event is auto-reset and fires constantly, so a stop that shared it could
		// not be told from a packet, and the reopen delay would end on a leftover
		// signal instead of on time.
		HANDLE wake{ nullptr };
		HANDLE quit{ nullptr };

		// ---- pump thread only, from here down
		bool         announced{ false };
		StreamFormat source;

		Microsoft::WRL::ComPtr<IAudioClient>        client;
		Microsoft::WRL::ComPtr<IAudioCaptureClient> reader;

		// Sized when a source opens and reused until it closes: the delivery step
		// must not allocate, and a buffer that grew would allocate fifty times a
		// second for ever.
		std::vector<Sample>    mono;
		std::vector<std::byte> raw;  // the file feeder's block, in the file's own format

		// The device's own frame counter, which is the same clock this whole
		// module counts in. A gap in it is missing sound by definition.
		std::uint64_t expected{ 0 };
		bool          havePosition{ false };

		std::uint64_t delivered{ 0 };
		std::uint64_t missing{ 0 };
		std::uint64_t holes{ 0 };
		std::uint64_t capped{ 0 };  // of `missing`, what was bigger than the ring

		// ------------------------------------------------------------ the pump
		void Proc()
		{
			// An exception escaping a thread procedure is std::terminate, and that
			// is a fail-fast nothing in the process observes
			// (docs/model-host.md, "Supporting pieces").
			try {
				if (settings.source == Source::File) {
					FileProc();
				} else {
					DeviceProc();
				}
			} catch (...) {
				try {
					Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_CRASHED");
				} catch (...) {
				}
			}
			running.store(false, std::memory_order_release);
			// Harmless after a good run - the promise is spent - and the whole of
			// the answer after a bad one.
			Announce(false);
		}

		// The owner is waiting on this and on nothing else. It is set exactly
		// once, whatever happens afterwards.
		void Announce(bool a_opened)
		{
			if (!announced) {
				announced = true;
				try {
					firstOpen.set_value(a_opened);
				} catch (...) {
				}
			}
		}

		// The format is published FIRST and the epoch SECOND, because the consumer
		// reads them the other way round: epoch, then format. That order is what
		// makes a device change cross to the consumer without a lock (Capture.h).
		void Publish(const std::string& a_name)
		{
			{
				std::scoped_lock lock(nameLock);
				name = a_name;
			}
			const auto packed = (static_cast<std::uint64_t>(source.rate) << 32) |
			                    static_cast<std::uint64_t>(source.channels);
			format.store(packed, std::memory_order_release);
			epoch.fetch_add(1, std::memory_order_release);
			opens.fetch_add(1, std::memory_order_relaxed);

			expected = 0;
			havePosition = false;
			delivered = 0;
			missing = 0;
			holes = 0;
		}

		// THE DELIVERY STEP (engine/audio.py:114-115). It does one thing: the
		// packet becomes mono float and is copied into the ring. No allocation, no
		// lock, no log, and nothing that might do any of the three.
		//
		// The chunk loop is not caution for its own sake: a packet is not promised
		// to fit the scratch, and the alternative to a loop is an allocation in
		// the one place that may never have one.
		void Deliver(const std::byte* a_from, std::uint32_t a_frames, bool a_silent) noexcept
		{
			const auto room = static_cast<std::uint32_t>(mono.size());
			if (room == 0) {
				return;
			}
			for (std::uint32_t done = 0; done < a_frames;) {
				const auto take = std::min(room, a_frames - done);
				if (a_silent) {
					// AUDCLNT_BUFFERFLAGS_SILENT: the memory is there and its
					// contents mean nothing. Zeros, not whatever was left in it.
					std::fill_n(mono.data(), take, Sample{ 0 });
				} else {
					ToMono(a_from + static_cast<std::size_t>(done) * source.frameBytes, take, source,
						mono.data());
				}
				ring.Write(mono.data(), take);
				done += take;
			}
			delivered += a_frames;
		}

		// A hole the device itself reported, measured in its own frames. It is
		// counted, never skipped: the consumer fills exactly this many samples of
		// silence before resampling, so the sample clock does not compress and
		// every later time in the turn stays where the sound put it (Ring.h).
		//
		// The size is capped at what the ring can hold. That is not a guess about
		// the device: a hole longer than the ring is longer than any turn can
		// survive - the ceiling ends the turn several times over - and an
		// endpoint whose position counter is nonsense would otherwise hand the
		// consumer an hour of silence to chew through.
		void NoteHole(std::uint64_t a_frames)
		{
			if (a_frames == 0) {
				return;
			}
			const auto fit = std::min<std::uint64_t>(a_frames, ring.Capacity());
			ring.NoteLost(static_cast<std::size_t>(fit));
			missing += fit;
			++holes;
			if (fit < a_frames) {
				capped += a_frames - fit;
			}
		}

		// ------------------------------------------------------- the device path
		void DeviceProc()
		{
			// COM BELONGS TO THE PUMP THREAD. An apartment belongs to a thread, and
			// the apartment of the game is not ours to assume - nor to leave
			// uninitialised behind us.
			const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			if (FAILED(com)) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_COM", Code(com));
				Announce(false);
				return;
			}

			// The multimedia class scheduler. Without it the pump is an ordinary
			// thread inside a game that is using every core it can reach, and a
			// starved pump is an overrun in the ring rather than a slow frame.
			DWORD      task = 0;
			const auto mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
			if (mmcss == nullptr) {
				Loc::Debug("$SPEECHBROKERVOICE_LOG_CAPTURE_NO_MMCSS", Code(HRESULT_FROM_WIN32(GetLastError())));
			}

			while (!stopping.load(std::memory_order_acquire)) {
				const bool opened = OpenDevice();
				if (opened) {
					running.store(true, std::memory_order_release);
				}
				Announce(opened);
				if (!opened) {
					break;
				}

				RunDevice();
				CloseDevice();
				// A device that went away while we were listening: round again.
				// Opens() climbing through a session is a microphone that keeps
				// leaving, and that belongs in the log rather than in a guess about
				// why recognition got worse (Capture.h).
			}

			if (mmcss != nullptr) {
				AvRevertMmThreadCharacteristics(mmcss);
			}
			CoUninitialize();
		}

		// Every candidate, in order, as many rounds as the policy allows. The
		// pause between the rounds is a sleep and nothing else: it is a wall clock
		// and not one number derived from it ever reaches the gate, the pacer or
		// the turn (voice-service.py:199, :218 - "a just-killed process can still
		// hold the endpoint").
		bool OpenDevice()
		{
			const auto  rounds = std::max(1, settings.device.reopen.tries);
			const auto  delay = static_cast<DWORD>(std::max(0, settings.device.reopen.delayMs));

			for (int round = 0; round < rounds; ++round) {
				if (stopping.load(std::memory_order_acquire)) {
					return false;
				}

				// Devices does the naming and the ordering, and says both in the
				// log itself - the empty machine, every candidate it ranked and the
				// inputApi key this build does not have. Saying any of it again
				// here would only put the same line in the log twice.
				const auto ranked = Devices::Rank(Devices::Inputs(), settings.device);

				for (const auto& one : ranked) {
					if (stopping.load(std::memory_order_acquire)) {
						return false;
					}
					if (OpenOne(one)) {
						return true;
					}
				}

				if (round + 1 < rounds) {
					Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_ROUND_FAILED", round + 1, rounds, delay);
					// Waiting on the stop handle rather than sleeping: a Stop during
					// the pause ends it at once, and the owner's join stays bounded.
					WaitForSingleObject(quit, delay);
				}
			}

			Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_NONE_OPENED");
			return false;
		}

		bool OpenOne(const Device& a_device)
		{
			using Microsoft::WRL::ComPtr;

			ComPtr<IMMDeviceEnumerator> endpoints;
			auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				IID_PPV_ARGS(&endpoints));
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "CoCreateInstance",
					Code(hr));
				return false;
			}

			ComPtr<IMMDevice> endpoint;
			if (a_device.id.empty()) {
				// Devices fills the id for everything it names; an empty one can
				// only be the system default asked for by policy, and the system is
				// the only one that knows which that is right now.
				hr = endpoints->GetDefaultAudioEndpoint(eCapture, eConsole, endpoint.GetAddressOf());
			} else {
				hr = endpoints->GetDevice(a_device.id.c_str(), endpoint.GetAddressOf());
			}
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "GetDevice", Code(hr));
				return false;
			}

			ComPtr<IAudioClient> opening;
			hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
				reinterpret_cast<void**>(opening.GetAddressOf()));
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "Activate", Code(hr));
				return false;
			}

			// SHARED MODE HANDS OUT THE MIX FORMAT AND DOES NOT NEGOTIATE. The
			// reference asked for rates in order and took the first that opened
			// (engine/audio.py:91, voice-service.py:203); that is a PortAudio
			// liberty we do not have and do not need, because everything
			// downstream is resampled anyway (Capture.h).
			WAVEFORMATEX* mix = nullptr;
			hr = opening->GetMixFormat(&mix);
			if (FAILED(hr) || mix == nullptr) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "GetMixFormat",
					Code(hr));
				return false;
			}

			StreamFormat asked;
			asked.rate = mix->nSamplesPerSec;
			asked.channels = mix->nChannels;
			asked.frameBytes = mix->nBlockAlign;
			asked.bits = mix->wBitsPerSample;
			asked.tag = mix->wFormatTag;
			if (asked.tag == kWaveFormatExtensible && mix->cbSize >= kExtensibleTail) {
				const auto* wide = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
				asked.tag = wide->SubFormat.Data1;
			}
			asked.kind = KindOf(asked.tag, asked.bits);

			if (asked.kind == PcmKind::Unsupported || asked.channels == 0 || asked.rate == 0 ||
				asked.frameBytes == 0) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_FORMAT_UNSUPPORTED", a_device.name, asked.bits,
					asked.tag, asked.channels);
				CoTaskMemFree(mix);
				return false;
			}

			// A REQUEST, not a promise: in shared mode the audio engine gives the
			// period it likes - ten milliseconds on most machines - and the body
			// works with what it got (Capture.h). Nothing downstream measures time
			// in blocks, so a period other than this one costs nothing.
			const auto wanted = static_cast<REFERENCE_TIME>(std::max(1, settings.blockMs)) *
			                    kHundredNsPerMs;
			hr = opening->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				wanted, 0, mix, nullptr);
			CoTaskMemFree(mix);
			mix = nullptr;
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "Initialize", Code(hr));
				return false;
			}

			hr = opening->SetEventHandle(wake);
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "SetEventHandle",
					Code(hr));
				return false;
			}

			UINT32 held = 0;
			hr = opening->GetBufferSize(&held);
			if (FAILED(hr) || held == 0) {
				held = static_cast<UINT32>(std::max<std::uint32_t>(1u,
					asked.rate * static_cast<std::uint32_t>(std::max(1, settings.blockMs)) / 1000u));
			}

			ComPtr<IAudioCaptureClient> reading;
			hr = opening->GetService(__uuidof(IAudioCaptureClient),
				reinterpret_cast<void**>(reading.GetAddressOf()));
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "GetService", Code(hr));
				return false;
			}

			hr = opening->Start();
			if (FAILED(hr)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_REFUSED", a_device.name, "Start", Code(hr));
				return false;
			}

			client = opening;
			reader = reading;
			source = asked;
			// Sized once, here, and never again until the next device: a packet
			// cannot be larger than the client's own buffer, and the chunk loop in
			// Deliver covers the case where a driver disagrees.
			mono.assign(held, Sample{ 0 });

			Publish(a_device.name);
			Loc::Info("$SPEECHBROKERVOICE_LOG_CAPTURE_OPENED", a_device.name, source.rate,
				source.channels, settings.blockMs, opens.load(std::memory_order_relaxed));
			return true;
		}

		void RunDevice()
		{
			// A device that has produced nothing for as long as the ring can hold
			// is gone, whatever it says about itself. The number is the ring's own
			// depth rather than one invented here, and like every wall clock in
			// this file it decides only when to give up on a device - never where
			// speech begins or ends.
			const DWORD  patience = static_cast<DWORD>(std::max(1, settings.ringMs));
			const HANDLE both[2] = { wake, quit };

			while (!stopping.load(std::memory_order_acquire)) {
				const auto woken = WaitForMultipleObjects(2, both, FALSE, patience);
				if (woken == WAIT_OBJECT_0 + 1 || woken == WAIT_FAILED) {
					return;
				}
				if (woken == WAIT_TIMEOUT) {
					Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_SILENT", Named(), patience);
					return;
				}

				UINT32 packet = 0;
				auto   hr = reader->GetNextPacketSize(&packet);
				while (SUCCEEDED(hr) && packet != 0) {
					BYTE*  bytes = nullptr;
					UINT32 frames = 0;
					DWORD  flags = 0;
					UINT64 position = 0;
					hr = reader->GetBuffer(&bytes, &frames, &flags, &position, nullptr);
					if (hr == AUDCLNT_S_BUFFER_EMPTY) {
						hr = S_OK;
						break;
					}
					if (FAILED(hr)) {
						break;
					}

					// The position is the stream's own frame counter. A gap in it
					// is sound that never reached us, and its SIZE is known
					// exactly - which is why this is measured rather than guessed
					// from AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, a flag that says
					// there was a hole and never says how big.
					if (havePosition && position > expected) {
						NoteHole(position - expected);
					}

					Deliver(reinterpret_cast<const std::byte*>(bytes), frames,
						(flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);

					expected = position + frames;
					havePosition = true;

					reader->ReleaseBuffer(frames);
					hr = reader->GetNextPacketSize(&packet);
				}

				if (FAILED(hr)) {
					Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_LOST", Named(), Code(hr));
					return;
				}
			}
		}

		void CloseDevice()
		{
			if (client) {
				client->Stop();
			}
			reader.Reset();
			client.Reset();

			Loc::Info("$SPEECHBROKERVOICE_LOG_CAPTURE_CLOSED", Named(), delivered, missing, holes);
			if (capped != 0) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_CAPTURE_HOLE_CAPPED", capped, ring.Capacity());
				capped = 0;
			}

			// THE RING IS NOT TOUCHED HERE, and that is deliberate. Ring::Reset
			// wants both sides stopped and the consumer is still running; the
			// samples left in it belong to the old device and it is the consumer
			// that throws them away and counts them, the moment it sees the epoch
			// change (Ears, step 2). Capture publishes the change and nothing else.
		}

		// --------------------------------------------------------- the file path
		void FileProc()
		{
			// No COM here, and no WASAPI. The file source is what makes this half
			// buildable and runnable with no game and no microphone around it, and
			// a COM apartment it does not need would be one more thing that can
			// refuse.
			WavSource wav;
			if (!wav.Open(settings.file)) {
				Announce(false);
				return;
			}

			source = wav.Format();
			// The same arithmetic the device period gets, so that the two paths
			// produce blocks of the same size and a defect found on one is the
			// same defect on the other. Clamped to the ring because the feeder
			// waits for room: a block the ring could never hold would be a wait
			// that never ends.
			const auto asked = std::max<std::uint32_t>(1u,
				source.rate * static_cast<std::uint32_t>(std::max(1, settings.blockMs)) / 1000u);
			const auto block = std::min<std::uint32_t>(asked,
				std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(ring.Capacity())));
			const auto pace = static_cast<DWORD>(std::max(1, settings.blockMs));

			// Both sized once, before a sample moves: the feeder allocates nothing
			// per block either, for the same reason the device pump does not - the
			// two paths must be comparable, and a path that allocates is not
			// comparable with one that does not.
			mono.assign(block, Sample{ 0 });
			raw.assign(static_cast<std::size_t>(block) * source.frameBytes, std::byte{ 0 });

			Publish(wav.Path());
			Loc::Info("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_OPENED", wav.Path(), source.rate,
				source.channels, wav.Frames() * 1000ULL / source.rate, settings.blockMs,
				settings.filePaced);

			running.store(true, std::memory_order_release);
			Announce(true);

			while (!stopping.load(std::memory_order_acquire)) {
				const auto frames = wav.Read(raw.data(), block);
				if (frames == 0) {
					if (settings.fileLoop) {
						// A splice, not a hole: no sample is missing, so nothing is
						// counted and the epoch does not move. Bumping it would make
						// the consumer throw the ring away and measure the noise
						// floor again in the middle of a recording.
						wav.Rewind();
						Loc::Debug("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_LOOP", wav.Path());
						continue;
					}
					finished.store(true, std::memory_order_release);
					Loc::Info("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_END", wav.Path(), delivered);
					break;
				}

				// THE FILE FEEDER WAITS FOR ROOM, and the asymmetry with the device
				// is the point (Capture.h): a device callback that waited would
				// glitch the audio engine of the whole machine, while a file has no
				// real time to keep - so a test never loses a sample and never has
				// to explain a hole.
				//
				// The room is computed by the producer, which is safe in exactly
				// one direction and this is that direction: the consumer only ever
				// makes MORE room, so what is read here is a lower bound and the
				// wait is conservative. The poll is a block long because a block is
				// what we are waiting for the consumer to take.
				while (!stopping.load(std::memory_order_acquire) &&
					ring.Capacity() - ring.Available() < frames) {
					WaitForSingleObject(quit, pace);
				}
				if (stopping.load(std::memory_order_acquire)) {
					break;
				}

				Deliver(raw.data(), frames, false);

				if (settings.filePaced) {
					// THE PACING NEVER REACHES SEGMENTATION. It is a sleep in this
					// thread and nothing else; not one number derived from it is
					// handed to the gate, the pacer or the turn (Capture.h).
					WaitForSingleObject(quit, pace);
				}
			}
		}

		std::string Named() const
		{
			std::scoped_lock lock(nameLock);
			return name;
		}
	};

	// ------------------------------------------------------------------ Capture
	Capture::Capture(CaptureSettings a_settings, Ring& a_ring) :
		_settings(std::move(a_settings)),
		_ring(a_ring),
		_backend(std::make_unique<Backend>(_settings, a_ring))
	{}

	// Out of line because Backend is incomplete in the header, and incomplete is
	// what keeps <audioclient.h> out of the rest of the adapter.
	Capture::~Capture()
	{
		Stop();
	}

	bool Capture::Start()
	{
		if (_backend->running.load(std::memory_order_acquire)) {
			return true;
		}
		// A pump left over from a source that stopped by itself - a file that ran
		// out, a device that never came back - is joined before another starts.
		Stop();

		_backend->stopping.store(false, std::memory_order_release);
		_backend->finished.store(false, std::memory_order_release);
		_backend->announced = false;
		_backend->firstOpen = std::promise<bool>{};
		auto opened = _backend->firstOpen.get_future();

		_backend->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset: WASAPI's
		_backend->quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);    // manual: once stopped, stopped
		if (_backend->wake == nullptr || _backend->quit == nullptr) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_EVENTS",
				Code(HRESULT_FROM_WIN32(GetLastError())));
			Stop();
			return false;
		}

		try {
			_backend->pump = std::thread([backend = _backend.get()]() { backend->Proc(); });
		} catch (const std::exception& why) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_THREAD", why.what());
			Stop();
			return false;
		}

		// THE OWNER WAITS HERE, and this is why Start is never called from the
		// thread of the game: the pump walks the endpoints and may sit through the
		// reopen delays, which is hundreds of milliseconds in the bad case
		// (Capture.h). The enumeration belongs to the pump because that is the
		// thread with a COM apartment of its own.
		bool ok = false;
		try {
			ok = opened.get();
		} catch (...) {
			ok = false;
		}
		if (!ok) {
			Stop();
			return false;
		}
		return true;
	}

	void Capture::Stop() noexcept
	{
		try {
			_backend->stopping.store(true, std::memory_order_release);
			if (_backend->quit != nullptr) {
				// Every wait in the pump is on this handle, so the join below is
				// bounded by whatever the pump is in the middle of and not by a
				// timeout anybody has to choose.
				SetEvent(_backend->quit);
			}
			if (_backend->pump.joinable()) {
				_backend->pump.join();
			}
			_backend->running.store(false, std::memory_order_release);

			if (_backend->wake != nullptr) {
				CloseHandle(_backend->wake);
				_backend->wake = nullptr;
			}
			if (_backend->quit != nullptr) {
				CloseHandle(_backend->quit);
				_backend->quit = nullptr;
				Loc::Info("$SPEECHBROKERVOICE_LOG_CAPTURE_STOPPED");
			}
		} catch (...) {
			// Stop is noexcept and is called from the destructor: join throws on a
			// thread that is no longer there, and there is nothing to do about it
			// but not make it worse.
		}
	}

	bool Capture::Running() const noexcept
	{
		return _backend->running.load(std::memory_order_acquire);
	}

	CaptureFormat Capture::Format() const noexcept
	{
		const auto packed = _backend->format.load(std::memory_order_acquire);
		return CaptureFormat{ static_cast<std::uint32_t>(packed >> 32),
			static_cast<std::uint32_t>(packed & 0xFFFFFFFFULL) };
	}

	std::uint32_t Capture::Epoch() const noexcept
	{
		return _backend->epoch.load(std::memory_order_acquire);
	}

	std::string Capture::Name() const
	{
		return _backend->Named();
	}

	bool Capture::Finished() const noexcept
	{
		return _backend->finished.load(std::memory_order_acquire);
	}

	std::uint64_t Capture::Opens() const noexcept
	{
		return _backend->opens.load(std::memory_order_relaxed);
	}
}
