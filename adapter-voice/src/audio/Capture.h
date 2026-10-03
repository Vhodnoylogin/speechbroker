#pragma once

#include "audio/Devices.h"
#include "audio/Ring.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Voice
{
	// Where the sound comes from. Two sources, one interface, and the second one is
	// not a debugging afterthought bolted on later: without it the whole of this
	// half can only be tried by putting on a headset and speaking, which is minutes
	// per attempt and no two attempts alike. With it a wav stands in for the
	// microphone, the gate, the pacer and the cutting run over identical input
	// every time, and a defect that took a run in the game to see becomes a test.
	enum class Source
	{
		Device,  // a capture endpoint of the machine, chosen by DeviceSettings
		File     // a wav standing in for it
	};

	// What the adapter is actually capturing, as opposed to what it asked for.
	// WASAPI in shared mode hands out the mix format of the endpoint - usually
	// 48000 Hz and often two channels - and does not negotiate; the reference asked
	// for rates in order and took the first that opened (engine/audio.py:91,
	// voice-service.py:203), which is a PortAudio liberty we do not have and do not
	// need, because everything downstream is resampled anyway.
	struct CaptureFormat
	{
		std::uint32_t sampleRate{ 0 };
		std::uint32_t channels{ 0 };
	};

	struct CaptureSettings
	{
		Source source{ Source::Device };

		// Where the wav is, when source is File. Relative to the folder of the
		// adapter; a path that leaves it is refused, exactly as the settings of the
		// service are (see ResolveInside in Config.cpp). No absolute path is ever
		// written into the code or into the shipped settings.
		std::string file;

		// Feed the file at the speed of sound rather than as fast as the ring
		// drains. Off by default, and that default is the useful one: a test that
		// runs the file flat out is deterministic and finishes in a moment.
		//
		// THE PACING NEVER REACHES SEGMENTATION. It is a sleep in the feeding
		// thread and nothing else; not one number derived from it may be handed to
		// the gate, the pacer or the turn, which count samples and only samples.
		bool filePaced{ false };

		// Start the file again at its end instead of stopping. For a long soak of
		// the consumer side over one recording.
		bool fileLoop{ false };

		DeviceSettings device;

		// What to ask WASAPI for as a period. It is a REQUEST: in shared mode the
		// audio engine gives the period it likes - ten milliseconds on most
		// machines - and the body must work with what it got and never assume this
		// number. The reference asked PortAudio for 1024 frames, which is 21 ms at
		// 48 kHz (voice-service.py:206 and the arithmetic at :225); the older path
		// asked for 64 ms (engine/audio.py:72). Twenty is between them and is the
		// period Windows actually uses.
		//
		// NOTHING DOWNSTREAM MAY MEASURE TIME IN BLOCKS. The reference added
		// block_ms to its counters (voice-service.py:278, :287), which is exact
		// only while every block is the same size; here the counters are sample
		// counts, so a short block or a device that changes its period costs
		// nothing.
		int blockMs{ 20 };

		// How much sound the ring holds. Four seconds is deliberately far more than
		// the consumer can fall behind by in ordinary play: the ring exists to
		// survive a stall - a shader compile, a cell load - without a hole, and
		// 4 s of 48 kHz mono float is 768 kB, which is nothing beside the 1.28 MB
		// a single full turn already costs.
		int ringMs{ 4000 };
	};

	// The microphone: the one irreversible resource in the whole mod, which is why
	// it is a class of its own and why exactly one of these exists. Two owners of
	// one microphone cut the stream in two different ways and produce two versions
	// of one phrase that nothing can reconcile afterwards (engine/audio.py:2-8).
	//
	// THREE THREADS TOUCH THIS CLASS AND THEY MAY DO DIFFERENT THINGS.
	//
	//   The OWNER thread - the ears' own - builds it, calls Start and Stop, and
	//   nothing else. Start opens COM, walks the endpoints and may sit through the
	//   reopen delays, so it takes hundreds of milliseconds in the bad case: IT IS
	//   NEVER CALLED FROM THE THREAD OF THE GAME. Stop joins the pump; the pump's
	//   longest wait is one event timeout, so the join is bounded, which is exactly
	//   what the model side of this adapter is NOT allowed to assume about somebody
	//   else's code (docs/model-host.md, "Teardown is not promised as a join").
	//
	//   The PUMP thread is ours and lives inside this class. It initialises COM for
	//   itself - an apartment belongs to a thread, and the apartment of the game is
	//   not ours to assume - waits on the capture event, and for every packet calls
	//   the delivery step below. It is also the thread that reopens a lost device
	//   and the thread that reads the file in the File source.
	//
	//   THE DELIVERY STEP is what the reference calls the callback
	//   (engine/audio.py:114-115). It does one thing: turn the packet into mono
	//   float and copy it into the ring. It MUST NOT allocate, must not take a
	//   lock, must not log and must not call anything that might. The one piece of
	//   arithmetic it is allowed is that conversion - a straight loop over the
	//   frames with no branches and no memory of its own - and it is allowed
	//   because the alternative is a ring of raw bytes whose reader would have to
	//   redo the same work anyway, one thread further from the format that produced
	//   it. Resampling is NOT done here: it is stateful, and state in a callback is
	//   how a callback grows.
	//
	// THE PUMP NEVER BLOCKS, THE FILE FEEDER DOES, and the asymmetry is deliberate.
	// A device callback that waits for room glitches the audio engine of the whole
	// machine, so it drops and counts instead (see Ring). A file has no real time
	// to keep, so its feeder waits for room and a test therefore never loses a
	// sample and never has to explain a hole.
	class Capture
	{
	public:
		// The ring is not owned here: it outlives the capture, because a device may
		// be reopened several times inside one session and the consumer must not
		// notice the difference.
		Capture(CaptureSettings a_settings, Ring& a_ring);

		// Out of line in the .cpp: the backend is an incomplete type here, and a
		// unique_ptr to an incomplete type cannot be destroyed in this header. That
		// incompleteness is the point - not one COM header reaches the rest of the
		// adapter through this file.
		~Capture();

		Capture(const Capture&) = delete;
		Capture(Capture&&) = delete;
		Capture& operator=(const Capture&) = delete;
		Capture& operator=(Capture&&) = delete;

		// OWNER THREAD. Opens the source and starts the pump. False means not one
		// candidate opened; the body has already said in the log which ones it
		// tried and why each refused, because "the microphone did not open" without
		// the list is a report nobody can act on (engine/audio.py:103,
		// voice-service.py:212, :220).
		bool Start();

		// OWNER THREAD. Idempotent, and safe to call from the destructor. Stops the
		// pump, joins it, closes the device.
		void Stop() noexcept;

		bool Running() const noexcept;

		// ANY THREAD. What is being captured right now.
		//
		// It is one atomic read, not two: the rate and the channel count are
		// published together in a single 64-bit value, because a consumer that read
		// them as two atomics could catch the rate of the new device with the
		// channel count of the old one - which is a wrong resampling ratio and a
		// silent one.
		CaptureFormat Format() const noexcept;

		// ANY THREAD. Bumped every time a device is opened, and published AFTER the
		// format it belongs to. Zero means nothing has opened yet.
		//
		// This is how a format change crosses to the consumer without a lock: read
		// the epoch, then the format; if the epoch differs from the one the
		// consumer is working under, the samples still in the ring belong to the
		// old device, and the consumer throws them away, resets its resampler and
		// starts the noise floor again - a different microphone has a different
		// floor. Capture counts what it discarded into Ring::NoteLost, because a
		// device change is a hole in the sound like any other.
		std::uint32_t Epoch() const noexcept;

		// ANY THREAD. What the open endpoint is called, for the log. Copied out
		// under the one lock this class has; the lock is taken when a device opens
		// and never by the delivery step.
		std::string Name() const;

		// ANY THREAD. The file source reached the end and did not loop. For a test
		// harness: it is how the harness knows the recording is spent without
		// guessing at a duration. Always false for a device.
		bool Finished() const noexcept;

		// ANY THREAD. How many times a device has been opened this session. One is
		// the healthy number; a number that keeps climbing is a microphone that
		// keeps going away, and that belongs in the log rather than in a guess
		// about why recognition got worse.
		std::uint64_t Opens() const noexcept;

	private:
		// Everything WASAPI, everything COM and the wav reader. Defined in the
		// .cpp and nowhere named in a header - that is what keeps <audioclient.h>
		// out of the rest of the adapter and what lets the file source exist
		// without a second interface.
		struct Backend;

		CaptureSettings          _settings;
		Ring&                    _ring;
		std::unique_ptr<Backend> _backend;
	};
}
