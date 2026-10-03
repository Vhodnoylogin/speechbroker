#pragma once

#include "audio/Capture.h"
#include "audio/Ring.h"
#include "turn/Completeness.h"
#include "turn/Gate.h"
#include "turn/Pacer.h"
#include "turn/Prosody.h"
#include "turn/Resample.h"
#include "turn/SpeechTurn.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace Voice
{
	// Everything this half is tuned by, in one place, so that it can be handed over
	// in one argument and so that a test can build it without a settings file and
	// without a game.
	//
	// THE EARS NEVER READ Config. They are given their settings and keep a copy.
	// That is what makes the whole half buildable and runnable outside SkyrimVR:
	// nothing under audio/ or turn/ includes Config.h, Loc.h is used only by the
	// bodies for their log lines, and not one file here includes RE/Skyrim.h,
	// SKSE.h or the model contract. Config owns one field of this type and fills it
	// from speechbroker-voice.json; a wav test builds one by hand.
	struct EarsSettings
	{
		CaptureSettings      capture;
		VadSettings          vad;
		PacerSettings        pacer;
		TurnSettings         turn;
		ProsodySettings      prosody;
		CompletenessSettings completeness;
		SegmentSettings      segments;

		// How long the consumer sleeps when the ring is empty.
		//
		// This is the whole cost of the decision that the capture callback NEVER
		// SIGNALS: no condition variable, no atomic notify, nothing that can enter
		// the kernel on the thread that owns the microphone. The consumer polls
		// instead and pays up to this much latency for it, on a cadence measured in
		// hundreds of milliseconds. It is a wall-clock sleep and it is one of the
		// two places one is allowed in this half - it must never produce a number
		// that reaches the gate, the pacer or the turn.
		int consumerPollMs{ 5 };
	};

	// What the ears have done so far. Read from any thread, so that logging and
	// reporting never reach into the working state.
	struct EarsStats
	{
		std::uint64_t captured{ 0 };      // samples at kTargetSampleRate
		std::uint64_t lost{ 0 };          // of those, missing: overruns and device changes
		std::uint64_t turns{ 0 };
		std::uint64_t passes{ 0 };        // handed to the sink
		std::uint64_t passesTooShort{ 0 };
		std::uint64_t passesTooQuiet{ 0 };
		std::uint32_t opens{ 0 };         // how many times a device has been opened
		float         floor{ 0.0f };
		float         trigger{ 0.0f };
	};

	// THE FRONT DOOR OF THE LISTENING HALF.
	//
	// Above this line nobody knows there is a ring, a gate, a pacer or a resampler.
	// What the rest of the plugin sees is: start it, and passes arrive; stop it,
	// and they stop. The dispatch half - registration, rosters, deadlines,
	// collectors, everything docs/model-host.md describes - takes each pass and
	// hands it to the models. It never looks inside this class and never reaches
	// past it into a turn.
	//
	// THE THREADS, WHOLE, BECAUSE EVERY FILE IN THIS HALF STATES ITS PART OF THEM
	// AND THIS IS WHERE THEY MEET:
	//
	//   THE GAME THREAD DOES NOTHING HERE. It does not construct the ears, it does
	//   not start them, it does not stop them and it never sees a pass. Start opens
	//   a device and may sit through the reopen delays; Stop joins two threads. The
	//   plugin starts the ears from a worker, exactly as it already starts its
	//   polling thread (Listen.cpp), and SKSE sends no shutdown message that would
	//   need the other direction.
	//
	//   THE CAPTURE PUMP is Capture's, and the delivery step inside it copies into
	//   the ring and does nothing else - no allocation, no lock, no log.
	//
	//   THE CONSUMER is this class's own thread and is where the whole of the
	//   cutting happens: resample, gate, pace, feed the turn, cut the pass, call
	//   the sink. One thread, therefore no lock anywhere in Resampler, Gate, Pacer
	//   or SpeechTurn - and the discipline that buys that is simply that no other
	//   thread may call into them.
	//
	// THE SINK IS ENTERED ON THE CONSUMER THREAD, so it must not block: it copies
	// the Pass - which is a handful of scalars and two shared pointers - onto its
	// own queue and returns. It must not call back into the ears, and the body must
	// wrap the call in a catch(...): an exception escaping a thread procedure is
	// std::terminate, and that is a fail-fast nothing in the process observes
	// (docs/model-host.md, "Supporting pieces").
	//
	// ONE CLOCK: THE SAMPLE COUNT. There is no steady_clock in the cutting. The
	// only wall clock in this half is the consumer's idle sleep and the optional
	// pacing of a file, and neither may hand a number to anything that decides
	// where speech begins or ends.
	//
	// THE ORDER OF THE CONSUMER LOOP IS PART OF THE DESIGN, not an implementation
	// detail, because it is the order the reference established at
	// engine/engine.py:61-92 and every threshold was tuned against it:
	//
	//   1. read from the ring; then read the lost counter and fill the hole with
	//      exactly that many samples of silence, so the clock stays the clock;
	//   2. if the capture epoch changed - a device was reopened - throw away what
	//      is left of the old device, reset the resampler, close any open turn with
	//      a final pass and start the noise floor again;
	//   3. resample to 16 kHz, carrying the remainder;
	//   4. gate the block: while the floor is being measured, the block is thrown
	//      away and nothing else happens;
	//   5. no turn open: the gate keeps the pre-roll and counts the debounce; when
	//      it says `opens`, start a turn and put the pre-roll in first - it is the
	//      turn's sample zero;
	//   6. turn open: feed the block with its verdict, arm the pacer, add to the
	//      silence count or clear it;
	//   7. `over` first, `run` second - the turn is over if the silence passed the
	//      pacer's end, or if this block would take the turn past the ceiling; a
	//      pass runs if the turn is over or the pacer fires;
	//   8. cut with final = over. The serial is spent either way;
	//   9. if the turn is over, close it and take the next turn id.
	//
	// The ceiling is the contract's own: when a turn would outgrow
	// maxRequestSamples the turn ENDS and the next sample starts a new turnId - the
	// window never slides, because buffer sample zero must stay turn sample zero.
	class Ears
	{
	public:
		// A pass is ready. The snapshot, the turn it belongs to, which pass it is
		// and whether it is the last - all of it is in the Pass, and all of it is
		// either a value or immutable and shared.
		using PassReady = std::function<void(const Pass&)>;

		explicit Ears(EarsSettings a_settings);
		~Ears();

		Ears(const Ears&) = delete;
		Ears(Ears&&) = delete;
		Ears& operator=(const Ears&) = delete;
		Ears& operator=(Ears&&) = delete;

		// Set BEFORE Start and never again: it is read by the consumer thread with
		// no lock, and a sink that could change under it would need one for no
		// reason - there is exactly one owner of these ears and it knows what it
		// wants before it starts them.
		void OnPass(PassReady a_sink);

		// Opens the sound source and starts both threads. False means the source
		// did not open; the body has already said which candidates it tried.
		//
		// NEVER FROM THE THREAD OF THE GAME.
		bool Start();

		// Idempotent, called by the destructor, never from the thread of the game
		// and never from inside the sink. Stops the pump, stops the consumer,
		// joins both. The join is bounded - the consumer's longest wait is
		// consumerPollMs and the pump's is one device period - which is precisely
		// what this adapter is NOT allowed to assume about a model's Stop.
		void Stop() noexcept;

		bool Running() const noexcept { return _running.load(std::memory_order_acquire); }

		// The number the dispatch half must publish as
		// SpeechBrokerVoiceSession::maxRequestSamples. It is VadSettings::maxUttSec
		// in samples and it is the SAME number that ends a turn: a ceiling that was
		// announced and a ceiling that was enforced must not be two fields that can
		// drift apart.
		std::uint32_t MaxRequestSamples() const noexcept;

		// What every pass will be in, for SpeechBrokerVoiceSession::format:
		// kTargetSampleRate, one channel. Said out loud rather than assumed.
		CaptureFormat OutputFormat() const noexcept;

		// What the microphone is actually giving us, before the resampler. For the
		// log, and for a report that a headset came up at some unexpected rate.
		CaptureFormat InputFormat() const noexcept;

		EarsStats Numbers() const noexcept;

		// The judge, shared rather than copied: it is stateless and const, so every
		// worker may use this one, and there is then exactly one holder of the
		// completeness weights in the process. A worker that built its own from its
		// own settings would be the second holder, and the two would differ on the
		// day somebody edits the file.
		const CompletenessJudge& Judge() const noexcept { return _judge; }

		const EarsSettings& Settings() const noexcept { return _settings; }

		// Classification with the settings these ears were built with
		// (engine/turn.py:124-129).
		LengthClass Classify(std::uint32_t a_words) const noexcept;

	private:
		EarsSettings _settings;

		// Resolved once, at construction, out of vad and turn together.
		CutRules _rules;

		// Declaration order is construction order and it matters: the ring must
		// exist before the capture that writes into it.
		Ring    _ring;
		Capture _capture;

		Resampler         _resampler;
		Gate              _gate;
		Pacer             _pacer;
		SpeechTurn        _turn;
		PitchTracker      _pitch;
		CompletenessJudge _judge;

		// The consumer's buffers, owned for the life of the session so that nothing
		// in the loop allocates: what comes out of the ring, and what comes out of
		// the resampler.
		std::vector<Sample> _raw;
		std::vector<Sample> _block;
		std::vector<Sample> _silence;   // zeros, to fill a hole without allocating one

		PassReady        _sink;
		std::thread      _consumer;
		std::atomic_bool _running{ false };
		std::atomic_bool _stopping{ false };

		// The turn ids and the pass serials. Both live here and nowhere else: a
		// turnId is monotone for the session and never reused, and a serial starts
		// again at 1 in every turn (contract, SpeechBrokerVoiceRequest).
		std::int64_t _nextTurnId{ 1 };
		std::int32_t _serial{ 0 };

		// What the consumer has already accounted for out of Ring::Lost, and which
		// capture epoch it is working under.
		std::uint64_t _seenLost{ 0 };
		std::uint32_t _epoch{ 0 };

		// The clock of the open turn, in samples: since the last loud block, and
		// since the last pass was fired (engine/engine.py:75-76, in milliseconds
		// there and in samples here).
		SampleIndex _silenceRun{ 0 };
		SampleIndex _sincePass{ 0 };

		// Read from any thread through Numbers(); written only by the consumer.
		std::atomic<std::uint64_t> _captured{ 0 };
		std::atomic<std::uint64_t> _lost{ 0 };
		std::atomic<std::uint64_t> _turns{ 0 };
		std::atomic<std::uint64_t> _passes{ 0 };
		std::atomic<std::uint64_t> _tooShort{ 0 };
		std::atomic<std::uint64_t> _tooQuiet{ 0 };
	};
}
