#pragma once

#include "../../contract/speechbroker-voice-model.h"

#include "turn/Completeness.h"  // FragmentSigns, and nothing else
#include "turn/SpeechTurn.h"    // Pass, Snapshot, Anchors

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Voice::Models
{
	using Clock = std::chrono::steady_clock;

	// ONE PIECE OF ONE MODEL'S READING, IN OUR OWN MEMORY.
	//
	// It is not the contract's SpeechBrokerVoiceFragment and must never become a
	// pointer to one. Everything a model hands over - the struct, the array,
	// every string - is borrowed for the length of the Complete call and may be a
	// buffer it reuses for the next request. What is kept is copied inside that
	// call, with strnlen against SPEECHBROKERVOICE_MAX_STRING_BYTES, truncating
	// rather than refusing.
	struct Fragment
	{
		// Milliseconds from sample zero of the submitted buffer, which is also
		// sample zero of the turn. Already clamped into the buffer and already
		// snapped onto the turn's anchors by the worker that built this.
		std::uint32_t startMs{ 0 };
		std::uint32_t endMs{ 0 };

		std::string text;  // UTF-8. Empty is a lawful answer.

		// The model's own confidence, as it sent it: a probability in [0, 1],
		// NOT multiplied by anything. Ranking it inside the model's own
		// distribution is the arbiter's job and happens later
		// (contract, SpeechBrokerVoiceFragment::score).
		float score{ 0.0f };

		// Everything the completeness judge needs. It is FragmentSigns out of
		// turn/Completeness.h rather than a second copy of those five fields,
		// which is what lets the judge stay in the half that owns the microphone
		// and never see this header.
		FragmentSigns signs;

		std::uint32_t DurationMs() const noexcept
		{
			return endMs > startMs ? endMs - startMs : 0U;
		}
	};

	// ONE MODEL'S WHOLE READING OF ONE BUFFER.
	//
	// Whole, always: a model re-reads the buffer from sample zero and gives back
	// its own full segmentation of it, which is what makes any two answers
	// comparable with each other and with what has already gone out. The question
	// "did it give me B, or A and B" must not be askable (contract preamble).
	struct Reading
	{
		// Who answered. BOTH are kept: the handle is the identity the ledger and
		// the roster use, and the id is what the arbiter folds by and the log
		// prints. The handle may be dead by the time the worker reads this - a
		// handle is never reused, so it stays a safe name for a dead model - and
		// looking the id up from a dead handle later would fail exactly when the
		// log line is most wanted.
		SpeechBrokerVoiceHandle handle{ 0 };
		std::string             modelId;

		// OK, CANCELLED or FAILED, and nothing else; the validation that admits
		// it has already refused everything outside that set.
		std::int32_t status{ SPEECHBROKERVOICE_REFUSED };

		// Measured by the model, wall clock, Submit to Complete. This is the
		// number the adapter learns what a model IS from, rather than what it
		// declared.
		std::int32_t latencyMs{ 0 };

		// Samples the model lost or refused out of the buffer it was given.
		// Anything but zero says this reading speaks about sound with a hole in
		// it, and it is weighed accordingly rather than merged as an equal
		// opinion.
		std::uint32_t lostSamples{ 0 };

		// Only when status is FAILED, and a string rather than a code because it
		// goes into the log for a person to read.
		std::string failed;

		std::vector<Fragment> fragments;

		bool Usable() const noexcept
		{
			return status == SPEECHBROKERVOICE_OK && !fragments.empty();
		}

		// MAY THIS READING DEFINE THE LANE.
		//
		// The arbiter picks one model's segmentation as the lane every other
		// model's text is matched onto, and it picks from among the models that
		// carry WORD TIMINGS - tested as exactly these two fields
		// (engine/arbiter.py:38-44). Filling either of them is therefore a claim
		// that the boundaries came from an alignment to the audio; a shim that
		// divides a segment's time proportionally by string length leaves both
		// sentinels, or it wins the lane with boundaries that move between passes
		// and re-creates the drifting lane arbiter.base() was written to fix.
		//
		// The zero of lastWordProb is the dangerous one: the test is `>= 0`, so a
		// value-initialised field passes it. The adapter fills fields a model did
		// not send with the contract's SENTINELS and never with zero, and this
		// function is the one place that matters.
		bool CarriesWordTimings() const noexcept
		{
			for (const auto& piece : fragments) {
				if (piece.signs.medianGapMs > 0u || piece.signs.lastWordProb >= 0.0f) {
					return true;
				}
			}
			return false;
		}
	};

	// Why a model left the expected set of a pass. NONE OF THE THREE IS A TIMEOUT
	// AND NONE OF THEM COSTS THE MODEL ITS STANDING; each is logged as a removal
	// (contract, "WHAT CLOSES A PASS"; docs/model-host.md, "The deadline").
	//
	// DroppedUnsent is the one with a second half: the entry is ALSO recorded
	// against that model in the same ledger as a timeout, so that a model which
	// is always too slow to be submitted to does not look statistically perfect.
	// The two records are not the same record - see Reputation::NoteDropped.
	enum class Removal : std::int32_t
	{
		DroppedUnsent = 0,  // replaced by a later serial of the same turn, or its budget was spent
		Unregistered = 1,
		NotReady = 2        // the model said Ready(handle, 0)
	};

	// THE OPEN PASS: ONE BUFFER OFFERED TO EVERY MODEL TAKING PART IN IT.
	//
	// The arbitration above needs a CLOSED SET of answers - it picks a lane from
	// among them and counts how many agree - so something has to decide that no
	// more are coming. In the python that was a blocking wait per model and the
	// set was closed by arithmetic (engine/engine.py:126). Nothing blocks any
	// more, so this object is that decision, and it is part of the contract
	// because it is what STALE means.
	//
	// IT HAS FOUR PIECES OF STATE AND NO MORE: the expected set, the answers, the
	// deadline, the closed flag. The Pass rides along as the SUBJECT of the pass -
	// the audio, the anchors, the tail silence, the range - and is not a fifth,
	// because nothing ever mutates it: it is a handful of scalars and two
	// immutable shared pointers, copied in at construction and read by everybody
	// afterwards without a lock.
	//
	// THE EXPECTED SET IS FIXED WHEN THE PASS IS CREATED AND ONLY EVER SHRINKS.
	// Without that rule the set is either fixed and charges timeouts to models
	// that were never asked, or floating and can go "complete" before a model's
	// Submit has even been entered.
	//
	// THREADS, ALL OF THEM, BECAUSE FIVE DIFFERENT ONES TOUCH THIS OBJECT:
	//
	//   THE CONSUMER THREAD OF THE EARS constructs it, inside the pass sink, and
	//   then hands a shared_ptr to every model's queue and to the scheduler. It
	//   does nothing else here and must not: it is the thread the microphone is
	//   being drained on.
	//
	//   EVERY MODEL'S OWN DISPATCH THREAD calls Remove() when it drops a queued
	//   entry, and IsClosed() before it enters Submit.
	//
	//   EVERY MODEL'S OWN THREADS, inside Host::Complete, call Deliver(). Several
	//   at once is ordinary.
	//
	//   THE SCHEDULER THREAD calls Close() when the deadline fires, AND NOTHING
	//   ELSE. It marks and it posts. Folding, snapping, judging and reconciling
	//   all run on a worker, because the scheduler runs everything that waits in
	//   one thread, and anything heavy there delays every other armed deadline
	//   and manufactures the out-of-order closes the serial guard then has to
	//   catch.
	//
	//   ONE WORKER THREAD, after the close, reads Answers(), Missing() and
	//   Subject(). By then nothing writes any of them.
	class Collector
	{
	public:
		// a_deadline empty means A PASS THAT IS NOT TIMED: today that is the
		// silence probe, which carries deadlineMs 0, belongs to no turn, is
		// retired only when the model unregisters or the session ends, and lives
		// in a map of its own for exactly that reason (contract, "A REQUEST WITH
		// deadlineMs 0 IS NOT RETIRED BY ANY OF THIS").
		Collector(std::int64_t a_utteranceId, Pass a_subject,
			std::vector<SpeechBrokerVoiceHandle> a_expected,
			std::optional<Clock::time_point> a_deadline);

		Collector(const Collector&) = delete;
		Collector(Collector&&) = delete;
		Collector& operator=(const Collector&) = delete;
		Collector& operator=(Collector&&) = delete;

		// --- identity, all immutable, readable from any thread with no lock ----

		std::int64_t UtteranceId() const noexcept { return _utteranceId; }
		std::int64_t TurnId() const noexcept { return _subject.turnId; }
		std::int32_t Serial() const noexcept { return _subject.serial; }
		bool         Final() const noexcept { return _subject.final; }

		// The audio, the anchors, the tail silence and the speaker's range as
		// they stood at the cut. The worker measures the terminal fall on THIS
		// and never on a live turn: by the time it runs, the turn belongs to
		// another thread and has moved on (engine/engine.py:144-151).
		const Pass& Subject() const noexcept { return _subject; }

		// Empty for the probe. One absolute moment for every model in the pass;
		// the per-model deadlineMs is this minus now, computed at the instant
		// Submit is entered and nowhere else.
		const std::optional<Clock::time_point>& Deadline() const noexcept { return _deadline; }

		// --- the closed flag ---------------------------------------------------

		// Read before entering Submit. THE ADAPTER NEVER ENTERS Submit FOR A PASS
		// THAT HAS ALREADY CLOSED: sending it would buy nothing but a guaranteed
		// STALE and, for a model across a network, a paid upload and a paid
		// inference for an answer that cannot be used.
		//
		// THREAD: any. Acquire, so that whoever sees the flag also sees the
		// writes that came before it.
		bool IsClosed() const noexcept { return _closed.load(std::memory_order_acquire); }

		// Seal it. Returns true to EXACTLY ONE CALLER, and that caller is the one
		// that posts the collector to a worker - so a pass is assembled once even
		// when the deadline fires at the same instant as the last answer arrives.
		//
		// THREAD: the scheduler, when the deadline fires; or whichever thread
		// delivered the last expected answer. Whoever wins does two things and
		// two only: mark, and post.
		bool Close() noexcept;

		// --- the expected set and the answers ----------------------------------

		// Admit an answer. Returns false when the collector is already closed or
		// the handle is not in the expected set - the caller then answers the
		// model STALE, which is not an accusation of anything.
		//
		// It copies into the slot and returns. It does not fold, does not snap
		// and does not reconcile: several models calling Complete at once must
		// not queue behind each other's merging.
		//
		// WHEN THIS FILLS THE EXPECTED SET the caller must go on to Close(), and
		// if Close() returns true, post to a worker - here, on the model's own
		// thread, because there is no other thread that knows it happened.
		//
		// THREAD: any model's thread, from inside Host::Complete.
		bool Deliver(SpeechBrokerVoiceHandle a_handle, Reading&& a_reading);

		// Take a model out of the expected set. Idempotent; false when it was not
		// there. Logged as a removal by the caller, with a_why in the line.
		//
		// IF THIS EMPTIES WHAT WAS LEFT UNANSWERED the collector is complete, and
		// the caller closes and posts exactly as Deliver's note says: a pass whose
		// last two participants both went away must not sit until its deadline.
		//
		// THREAD: any. A dispatch thread dropping its own queued entry; any
		// model's thread inside Unregister or Ready(0).
		bool Remove(SpeechBrokerVoiceHandle a_handle, Removal a_why);

		// Is every model in the expected set answered. THREAD: any.
		bool AllAnswered() const;

		// --- what the worker reads after the close -----------------------------

		// The answers, in arrival order. THREAD: the assembling worker, after
		// Close() returned true to somebody. Reading it before the close is a
		// defect, not a race to be papered over with a lock.
		const std::vector<Reading>& Answers() const noexcept { return _answers; }

		// Everyone still expected and still silent at the close. EVERY ONE OF
		// THEM IS RECORDED AS A TIMEOUT FAILURE against its standing - the same
		// bookkeeping engine/engine.py:128 did on an expired future - which is
		// what keeps a permanently stalled third-party model from holding its
		// full voting weight forever while contributing nothing.
		//
		// Their debt is discharged at the same moment and without their doing
		// anything: after the close their answer is STALE and they may stop
		// carrying it (contract, Submit).
		//
		// THREAD: the assembling worker.
		std::vector<SpeechBrokerVoiceHandle> Missing() const;

		// Everyone still expected, answered or not. For Cancel, which is
		// advisory: the model still owes its Complete.
		// THREAD: any.
		std::vector<SpeechBrokerVoiceHandle> Expected() const;

	private:
		const std::int64_t                     _utteranceId;
		const Pass                             _subject;
		const std::optional<Clock::time_point> _deadline;

		// Set once, never cleared. Separate from _lock so that IsClosed() costs
		// one load on the hot path in front of every Submit.
		std::atomic_bool _closed{ false };

		// Guards the two vectors below and nothing else. Never held across a call
		// into a model, and never held while a log line is written.
		mutable std::mutex _lock;

		// Fixed at construction, shrink-only. A vector and not a set: it holds
		// the models of one pass, which is a handful, and a linear scan over a
		// handful beats a hash under a lock that is taken from four threads.
		std::vector<SpeechBrokerVoiceHandle> _expected;

		std::vector<Reading> _answers;
	};
}
