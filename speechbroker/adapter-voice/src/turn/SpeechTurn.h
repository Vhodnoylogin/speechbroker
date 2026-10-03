#pragma once

#include "turn/Prosody.h"
#include "turn/Resample.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Voice
{
	// THE AUDIO OF A PASS, AND THE WHOLE CONTRACT TURNS ON THE SHAPE OF THIS TYPE.
	//
	// A std::shared_ptr<const std::vector<Sample>>: made once per pass, already
	// trimmed, and never touched again by anybody. Not a pointer into the turn, not
	// a span over the turn, not a reference to a buffer that is still being filled.
	//
	// WHY, in the words of the side that is owed the guarantee. The contract
	// promises a model that `samples` is valid for the whole of the Submit call and
	// that it is an immutable snapshot rather than a window into a live buffer
	// (contract, SpeechBrokerVoiceRequest::samples). That promise is derivable from
	// this shape and from no other. And the lifetime is A CLAIM, NOT AN EVENT: the
	// moment a pass is queued for a model, that queue entry takes a copy of this
	// shared_ptr and releases it either when Submit returns or when the entry is
	// dropped unsent. The buffer dies when the last claim does - which is why the
	// earlier wording, "alive until the last Submit has returned", was wrong: for a
	// queued copy that is replaced before it is ever sent, that event never happens
	// (docs/model-host.md, "The audio of a pass").
	//
	// The reference had no hole here, but it had no rule either: turn.audio()
	// concatenated a fresh array per pass (engine/turn.py:111-113), so what it
	// actually guaranteed was OWNERSHIP PER PASS rather than immutability. This is
	// the C++ spelling of the same thing, with the immutability written into the
	// type so that it cannot be lost by accident.
	//
	// const, therefore: a model that casts the const away spoils the sound for
	// every other model in the same pass, and nothing on our side can detect it.
	using Snapshot = std::shared_ptr<const std::vector<Sample>>;

	// The edges of the pauses, in milliseconds from turn sample zero, measured FROM
	// THE SOUND ALONE.
	//
	// They are the most stable numbers this module has: they come out of energy,
	// they depend on no model and they do not move between passes. Model timestamps
	// wander by hundreds of milliseconds over the same audio, so a piece of speech
	// is identified by snapping model times onto these and never by text and never
	// by index (engine/turn.py:62-65, :97-109).
	//
	// Shared and const for the same reason the audio is: the worker that reconciles
	// a reading runs long after the pass was cut, on another thread, while the turn
	// has moved on. It gets the anchors as they were AT THE CUT, not as they are.
	using Anchors = std::shared_ptr<const std::vector<std::uint32_t>>;

	// How long a piece of speech is, in words. The numbers are the ones the bridge
	// receives (voice-service.py:381 `LENGTH_CLASS = {"short": 0, "middle": 1,
	// "long": 2}`), so they are fixed by that and not free to renumber.
	enum class LengthClass : std::int32_t
	{
		Short = 0,
		Middle = 1,
		Long = 2
	};

	// Where the boundaries between those three fall. voice.json:74-77, read at
	// engine/turn.py:55-56.
	struct SegmentSettings
	{
		int shortMaxWords{ 3 };
		int middleMaxWords{ 8 };
	};

	// engine/turn.py:124-129. Free and pure, so the worker that has the words
	// classifies with the same numbers the settings hold, rather than keeping a
	// second copy of them.
	LengthClass Classify(std::uint32_t a_words, const SegmentSettings& a_settings) noexcept;

	// The slacks and the tolerances of a turn. Every one of them is a constant in
	// the reference and a settings field here, because every one of them was tuned
	// against real speech and will be tuned again.
	struct TurnSettings
	{
		// Two anchors closer together than this are one anchor: the edges of a
		// pause are not sharp, and a list full of near-duplicates makes the snap
		// meaningless. engine/turn.py:94 (`abs(self.anchors[-1] - ms) > 60`).
		int anchorSlackMs{ 60 };

		// How far a model's time may be from an anchor and still be pulled onto it.
		// The reference's own note is the best statement of why it exists: without
		// it one and the same "fireball" arrived as [0..842], [0..1305] and
		// [0..580], and each time counted as a new piece (engine/turn.py:97-109).
		int snapSlackMs{ 350 };

		// How long the quiet has to have lasted before speech starting again counts
		// as a new anchor rather than as the same run of speech.
		// engine/turn.py:80 (`self._quiet_ms >= 150`).
		int resumeQuietMs{ 150 };

		// How far apart two pieces may be and still be called the same piece when a
		// fresh reading is matched against what has already gone out. Read by the
		// reconciler above this half, not here; it lives with the other slacks
		// because they are one family and a body author must not declare a second
		// copy of it. engine/turn.py:185-190 and :194-201 (`slack_ms: int = 200`).
		int matchSlackMs{ 200 };

		// The floor the contract itself sets: no buffer shorter than a quarter of a
		// second is ever submitted, because a model cannot align one and Whisper
		// invents over it (engine/engine.py:110-111; contract,
		// SpeechBrokerVoiceRequest::sampleCount).
		//
		// VadSettings::minUttSec is the same rule at another scale, and the body
		// takes the stricter of the two - see CutRules, which is where the two stop
		// being two numbers.
		int minPassMs{ 250 };
	};

	// The three numbers the cutting actually compares against, resolved ONCE when
	// the ears start, out of VadSettings and TurnSettings together.
	//
	// It exists so that the turn does not have to hold two settings structs and so
	// that nobody has to decide, in the hot path, which of two floors is in force.
	// One number, decided once, at the only moment when both structs are in hand.
	struct CutRules
	{
		// max(TurnSettings::minPassMs, VadSettings::minUttSec).
		SampleIndex minSamples{ 0 };

		// VadSettings::maxUttSec. The turn ends when it would pass this, and the
		// pass that ends it carries final == 1. It is also the number the dispatch
		// half must publish as SpeechBrokerVoiceSession::maxRequestSamples - the
		// same number, because the contract's ceiling and our own are one ceiling.
		SampleIndex maxSamples{ 0 };

		// VadSettings::minPeak, in the unit of kRmsScale.
		float minPeak{ 0.0f };
	};

	// One pass, as it leaves this half. Everything in it is either a value or an
	// immutable shared thing; there is not one reference into live state, and that
	// is what lets it be handed to a worker and outlive the turn it came from.
	struct Pass
	{
		// Never null when the pass was made. Trimmed, mono, from turn sample zero.
		Snapshot audio;

		// Monotone for the session, never reused - the contract requires both
		// (SpeechBrokerVoiceRequest::turnId).
		std::int64_t turnId{ 0 };

		// Which pass this is inside the turn, from 1.
		//
		// SERIALS SKIP AND NOBODY IS OWED AN ACCOUNT OF IT: a serial is spent when
		// a pass is ARMED, and the trimmed buffer may then fall below the floor or
		// be too quiet to be worth sending (engine/engine.py:110-111; contract,
		// SpeechBrokerVoiceRequest::serial). The gap in the numbers is the record
		// that it happened.
		std::int32_t serial{ 0 };

		// The last pass of this turn; the buffer will not be extended again.
		//
		// NOT a promise that it is longer than the pass before it. Every pass is
		// trimmed of the silence that fired it, and the silence that fires the last
		// one is the longest, so in the ordinary one-phrase turn the final buffer
		// is the previous buffer, sample for sample (contract preamble, "NO
		// EARLIER, NOT LONGER"; the arithmetic is engine/engine.py:106-109 against
		// the two thresholds at engine/turn.py:26-27). Nothing anywhere may test a
		// pass for being longer than its predecessor.
		bool final{ false };

		// Said out loud in every pass although it never changes, for the reason the
		// contract gives: a constant in a header is what the adapter was compiled
		// with, a field is what it caught, and only the second is a fact.
		std::uint32_t sampleRate{ kTargetSampleRate };

		// Samples missing INSIDE this buffer - an overrun, a device that went away.
		// They are present in the audio as silence, so the timeline is intact and
		// this is how long of it is not real. Goes straight to
		// SpeechBrokerVoiceRequest::lostSamples.
		std::uint32_t lostSamples{ 0 };

		// The silence that fired this pass, already cut off the end of the audio.
		// The worker needs it: it is the "silence after" of the last fragment when
		// the judge is asked whether the sentence finished (engine/turn.py:157,
		// engine/engine.py:100 and :156).
		std::uint32_t tailSilenceMs{ 0 };

		// The anchors as they stood at the cut. Never null.
		Anchors anchors;

		// The speaker's range as it stood at the cut, by value. With it the worker
		// measures the terminal fall on the snapshot without touching the turn
		// (engine/engine.py:144-151 does this on the live turn's PitchRange; here
		// it cannot, because by then the turn belongs to another thread).
		PitchSpan pitch;

		std::uint32_t SampleCount() const noexcept
		{
			return audio ? static_cast<std::uint32_t>(audio->size()) : 0U;
		}

		std::uint32_t DurationMs() const noexcept
		{
			return SamplesToMs(SampleCount());
		}
	};

	// Why a cut produced nothing, so the log can say it once and the numbers can
	// count it. A refusal here is ordinary - it is the system declining to hand a
	// model something it could only invent over.
	enum class CutVerdict : std::int32_t
	{
		Made = 0,
		TooShort = 1,   // below CutRules::minSamples after trimming
		TooQuiet = 2,   // peak below CutRules::minPeak: silence, and Whisper invents over silence
		Empty = 3       // nothing in the turn at all
	};

	struct CutOutcome
	{
		CutVerdict verdict{ CutVerdict::Empty };
		Pass       pass;  // filled only when verdict is Made
	};

	// One speaking turn: its sound, its clock, its anchors, its range and the
	// passes it has already handed out.
	//
	// A turn lives from the block that opened the gate to the long silence that
	// closes it. All that time it accumulates sound FROM ZERO, because every pass
	// is a complete re-reading of the turn: that is what makes any two answers
	// comparable with each other and with what has already gone out, and it is why
	// the question "did it give me B, or A and B" cannot be asked (engine/turn.py:1-7,
	// contract preamble).
	//
	// THREAD: the consumer's, from Restart to Close. Nothing else may touch it.
	// What crosses to another thread is a Pass, which shares nothing mutable with
	// it.
	//
	// IT IS REUSED, NOT REBUILT. One SpeechTurn lives for the session and Restart
	// gives it a new id; the sample buffer is reserved to CutRules::maxSamples at
	// construction and never grows again. That is 1.28 MB allocated once at the
	// shipping settings, against 1.28 MB allocated per turn otherwise - and, more
	// to the point, no reallocation ever happens while the microphone is running.
	class SpeechTurn
	{
	public:
		SpeechTurn(const TurnSettings& a_settings, const CutRules& a_rules,
			const ProsodySettings& a_prosody);

		// Begin a turn with this id. Clears everything, keeps every allocation.
		void Restart(std::int64_t a_id);

		// The turn is finished with. After it, Samples() is zero and nothing may be
		// cut until the next Restart.
		void Close();

		std::int64_t Id() const noexcept { return _id; }
		bool         Open() const noexcept { return _open; }

		// One block of 16 kHz mono, with the gate's verdict about it. This is the
		// reference's append and note_level in one call (engine/turn.py:72-90),
		// deliberately merged: two calls over the same block are two chances to
		// append it without noting it, and the anchors would then be measured
		// against a clock that had already moved.
		//
		// a_pitch is the consumer's own tracker, lent for the duration. The tone is
		// measured only on loud blocks, as in the reference (engine/turn.py:85).
		void Feed(std::span<const Sample> a_block, bool a_loud, PitchTracker& a_pitch);

		// Samples that were lost inside the stretch just fed - they are already in
		// the buffer as silence, and this is how many of them are not real. Counted
		// per turn and handed on in every pass of it.
		void NoteLost(std::size_t a_samples) noexcept;

		// THE CLOCK. How much sound the turn holds, and the same thing in
		// milliseconds for the log and for the contract (engine/turn.py:115-117).
		SampleIndex   Samples() const noexcept { return _samples; }
		std::uint32_t ElapsedMs() const noexcept { return SamplesToMs(_samples); }

		// Would another a_count samples pass the ceiling? Asked BEFORE the block is
		// fed, because the answer decides whether this block belongs to this turn
		// or to the next one (contract, maxRequestSamples: the turn ends, the
		// window never slides).
		bool WouldOverflow(std::size_t a_count) const noexcept;

		// MAKE THE PASS. This is the one place a Snapshot is created.
		//
		// a_tailSilence is how many samples of silence stand at the end of the
		// turn; exactly that many are cut off the end of the snapshot and the rest
		// is copied. THE ADAPTER NEVER SENDS TRAILING SILENCE IT DETECTED - it is
		// the one that heard the pause, and a model is not to be trusted with that:
		// Whisper invents filler over trailing silence with its own silence filter
		// switched on, and the very first run of this system produced a whole
		// sentence over nothing (engine/engine.py:102-109; contract,
		// SpeechBrokerVoiceRequest::samples).
		//
		// a_serial is spent whether or not a pass comes out of it.
		//
		// A FINAL PASS IS ALWAYS MADE ONCE ANY PASS HAS BEEN MADE. When a_final is
		// set and the trimmed buffer would be refused by the floor or the peak, the
		// turn hands back the LAST SNAPSHOT IT ALREADY MADE, with a fresh serial
		// and final set - which costs one pointer copy, because the snapshot is
		// immutable and shared. Without this rule a turn could end with every pass
		// interim, and the dispatch half above - which may never replace an entry
		// whose final is set, and which closes a turn on it - would wait for a pass
		// that is never coming. The arithmetic says it cannot happen (the final
		// buffer is never shorter than the last interim one), and the rule is here
		// for the day the arithmetic is wrong.
		CutOutcome Cut(SampleIndex a_tailSilence, bool a_final, std::int32_t a_serial);

		// Has anything gone out for this turn.
		bool Emitted() const noexcept { return _emitted; }

		// Pull a model's millisecond onto the nearest anchor, or leave it where it
		// is when no anchor is near enough (engine/turn.py:97-109). The model
		// decides WHERE a boundary falls; the adapter decides what numbers it is
		// called by.
		//
		// The worker above uses Pass::anchors and its own copy of this arithmetic
		// rather than calling into a live turn - this one is for the consumer.
		std::uint32_t Snap(std::uint32_t a_ms) const noexcept;

		const CutRules& Rules() const noexcept { return _rules; }

	private:
		TurnSettings    _settings;
		CutRules        _rules;
		ProsodySettings _prosody;

		// The slacks, in samples, converted once.
		SampleIndex _anchorSlack{ 0 };
		SampleIndex _snapSlack{ 0 };
		SampleIndex _resumeQuiet{ 0 };

		std::int64_t _id{ 0 };
		bool         _open{ false };
		bool         _emitted{ false };

		std::vector<Sample> _pcm;       // reserved to _rules.maxSamples, never grown
		SampleIndex         _samples{ 0 };
		std::uint32_t       _lost{ 0 };

		// The anchors, and the immutable copy handed to the last pass. The copy is
		// remade only when the list has changed since it was taken - most passes
		// take the same one, and an allocation avoided in the cutting path is an
		// allocation avoided while somebody is speaking.
		std::vector<std::uint32_t> _anchors;
		Anchors                    _anchorsShared;
		bool                       _anchorsDirty{ true };

		// The run of quiet, for the anchors (engine/turn.py:68-69, :76-90).
		SampleIndex _quiet{ 0 };
		bool        _wasQuiet{ true };

		PitchRange         _pitch;
		std::vector<float> _f0;   // scratch for one block, reused

		// The last snapshot handed out, kept for the final-pass rule above. It
		// holds the audio alive no longer than the turn does, and the turn is
		// reused, so it is released at the next Restart.
		Snapshot      _lastAudio;
		Anchors       _lastAnchors;
		PitchSpan     _lastPitch;
		std::uint32_t _lastTailSilenceMs{ 0 };
		std::uint32_t _lastLost{ 0 };
	};
}
