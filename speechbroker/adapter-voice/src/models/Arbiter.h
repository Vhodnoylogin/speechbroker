#pragma once

#include "models/Collector.h"
#include "models/Reputation.h"

#include "turn/Completeness.h"
#include "turn/SpeechTurn.h"  // LengthClass, SegmentSettings, TurnSettings

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Voice::Models
{
	// SEVERAL MODELS ARGUING OVER ONE AND THE SAME SOUND.
	//
	// Only two things are comparable here, and naming them is most of the design
	// (engine/arbiter.py:1-12):
	//
	//   AGREEMENT - two models independently arriving at the same string is the
	//   best evidence this system ever gets. It is not "two votes"; it is one
	//   stronger reason.
	//
	//   A MODEL'S PLACE IN ITS OWN DISTRIBUTION - because raw scores across
	//   models are not comparable at all. 0.9 from one and 0.9 from another mean
	//   different things; "higher than four cases out of five" means the same
	//   thing from anybody.
	//
	// AND TRUST IS NOT ONE OF THEM. It decides whose voice weighs more IN AN
	// ARGUMENT, and it must never multiply the score itself: doing so lowered a
	// model's confidence for being OFTEN RIGHT, and a phrase recognised word for
	// word stopped reaching the threshold (engine/arbiter.py:76-86).

	// One guess at what was said.
	struct Hypothesis
	{
		std::string  text;
		float        score{ 0.0f };   // normalised into the model's own distribution
		std::string  model;           // whose guess won this slot after the fold
		std::int32_t agreed{ 1 };     // how many models said the same thing

		// The key two models are compared by: lower case, letters digits and
		// spaces only, trimmed. Punctuation and case are not disagreements about
		// what was heard (engine/parts.py:20-22).
		//
		// THREAD: any - it reads nothing but this object.
		std::string Key() const;
	};

	// The lane: the stretches of the buffer, in order, each with its hypotheses
	// best first.
	using Lane = std::vector<std::vector<Hypothesis>>;

	// WHAT GOES OUT, AND NOTHING ELSE GOES OUT. engine/parts.py:68-104.
	struct Slice
	{
		// The adapter's own number for this piece of speech, monotone for the
		// session. It is NOT an utteranceId and NOT a model's index: those name a
		// pass and a reading, this names a piece a person said.
		std::int32_t id{ 0 };

		// Milliseconds from turn sample zero, after snapping onto the anchors.
		std::uint32_t startMs{ 0 };
		std::uint32_t endMs{ 0 };

		std::vector<Hypothesis> hypotheses;

		// How sure we are the sentence ended here, in [0, 1]. The bridge holds
		// the unfinished back - but only if it was told, and only the side that
		// heard the pause can tell it.
		float complete{ 1.0f };

		// Pieces this one swallowed: a later, longer reading absorbing the short
		// ones already sent. Ids of THIS list, never a model's numbering.
		std::vector<std::int32_t> supersedes;

		// How far into the turn the speech of this piece ended.
		std::uint32_t speechElapsedMs{ 0 };

		LengthClass lengthClass{ LengthClass::Short };

		// WHEN it was handed out, from turn sample zero - not the same as endMs.
		// Between the end of the speech and the handing out lies the pause the
		// pass was fired by, and the difference between two pieces' emission is
		// exactly the time the bridge may spend holding the first one back
		// (engine/parts.py:80-84).
		std::uint32_t emittedMs{ 0 };

		const std::string& Text() const;
		std::uint32_t      DurationMs() const noexcept
		{
			return endMs > startMs ? endMs - startMs : 0U;
		}
	};

	// The tolerances of the argument. Settings, not constants, like every other
	// tuned number in this module.
	struct ArbiterSettings
	{
		// How much of a lane stretch a foreign fragment must cover before its
		// text is counted as being about that stretch. A weak overlap means the
		// model was talking about the neighbouring phrase and its text does not
		// belong here (engine/arbiter.py:131-135).
		//
		// TWO NUMBERS, AND THE STRICTER WINS, exactly as the reference did with
		// `max(slack_ms, target.duration_ms // 2)`: a floor in milliseconds for
		// short stretches, and a fraction for long ones.
		int overlapMs{ 250 };
		int overlapPercent{ 50 };
	};

	// Folding several readings of one buffer into one lane.
	//
	// THREAD: any, and several at once. It holds nothing but its settings and a
	// const reference to the standings, so one instance serves every worker. The
	// standings take their own locks; nothing here does.
	class Arbiter
	{
	public:
		Arbiter(ArbiterSettings a_settings, Reputations& a_standings);

		// WHOSE READING DEFINES THE LANE.
		//
		// Not the one that cut finest - that was a mistake and it is worth the
		// sentence: fineness says nothing about where the boundaries are, and
		// "finest of all" changed from pass to pass, so the lane slid and the
		// hypotheses of one phrase glued themselves to the stretch of another.
		//
		// The lane is defined by a model WITH WORD TIMINGS, because its
		// boundaries came from an alignment to the audio rather than from
		// dividing a segment proportionally by string length, and a lane must be
		// reproducible or nothing can be recognised on it as already sent. Among
		// those, the one with the most fragments; among equals, the FIRST, so
		// that two runs over the same answers give the same lane
		// (engine/arbiter.py:22-44).
		//
		// nullptr when no reading is usable.
		const Reading* Base(const std::vector<Reading>& a_readings) const;

		// The stretches of the lane, in the order Merge returns their hypotheses.
		std::vector<Fragment> Spans(const std::vector<Reading>& a_readings) const;

		// The lane itself. One bucket per stretch of Base, each folded and sorted
		// AGREEMENT FIRST, then weighted score: two models beat one confident
		// one, and between two lone voices the one we trust more wins
		// (engine/arbiter.py:100-118).
		//
		// Identical text from different models is not doubled, it is STRENGTHENED
		// - merged into one hypothesis whose agreed count rises. The winner of a
		// fold is chosen by score TIMES weight; the score stored is the score as
		// it was. Trust picks the winner of an argument, it does not shrink the
		// confidence the winner leaves with.
		Lane Merge(const std::vector<Reading>& a_readings) const;

		// Feed the fold's outcome back into the standings, so that "these two
		// always agree" is a measured thing rather than an impression.
		// THREAD: the assembling worker, after Merge.
		void NoteAgreement(const Lane& a_lane) const;

		const ArbiterSettings& Settings() const noexcept { return _settings; }

	private:
		ArbiterSettings _settings;

		// Not const: Normalize and Weight are const on the record, but
		// NoteAgreement writes. One reference, so there is exactly one holder of
		// the standings in the process.
		Reputations* _standings;
	};

	// ------------------------------------------------------------------------ //

	// ONE TURN'S LEDGER: WHAT HAS ALREADY GONE OUT, AND THE ORDER GUARD.
	//
	// The passes of a turn are ordered by serial, each one a re-read from sample
	// zero, and the reconciliation is APPEND-ONLY: feeding it serial 3 after
	// serial 4 has been reconciled would emit corrections pointing backwards in
	// time. So the guard lives here, at close, under this object's own lock, and
	// the last-reconciled serial is written nowhere else in the process.
	//
	// IT CANNOT LIVE AT Complete, WHERE AN EARLIER DRAFT PUT IT. An answer is
	// admitted to its slot while its own pass is still legitimately open, and the
	// reconciliation happens later. Two collectors of one turn have independent
	// timers armed at different moments, so pass 4 can close all-answered while
	// pass 3 is still waiting out a slow model. Only the close order matters, and
	// only close can see it (contract, "ARBITRATION RUNS EXACTLY ONCE PER
	// COLLECTOR").
	//
	// A piece is identified by TIME inside the turn and never by text and never
	// by index. Model timestamps wander by hundreds of milliseconds over the same
	// audio, so the times have already been snapped onto the turn's own anchors -
	// which come out of energy alone and do not move between passes - before they
	// reach here (engine/turn.py:97-109, slack TurnSettings::snapSlackMs).
	//
	// THREAD: one worker at a time, by the lock. Several workers may hold
	// different turns' ledgers at once, which is the point of a ledger per turn.
	class TurnLedger
	{
	public:
		TurnLedger(std::int64_t a_turnId, TurnSettings a_turn, SegmentSettings a_segments);

		std::int64_t TurnId() const noexcept { return _turnId; }

		// THE WHOLE CLOSE PATH OF ONE PASS, and it is one call on purpose: the
		// serial guard and the reconciliation must happen under one hold of one
		// lock, or a second worker slips between them.
		//
		//   BEHIND the last reconciled serial - the collector is ABANDONED WHOLE.
		//   No arbitration, no reconciliation, every answer in it treated as
		//   STALE, one log line, and an empty result.
		//
		//   AHEAD of it - the arbitration runs, the ledger advances to this
		//   serial, and the slices to hand onward come back.
		//
		// a_judge is the ears' own judge, shared and const: there is exactly one
		// holder of the completeness weights in the process, and a worker that
		// built its own from its own settings would be the second holder
		// (Ears::Judge()).
		//
		// a_terminalFall is our own measurement of the tone over the last stretch
		// of the SNAPSHOT, empty when the prosody had no opinion. Punctuation is
		// a model's and is a property of a language; tone is ours and is not
		// (engine/engine.py:144-151).
		//
		// THREAD: the assembling worker, and only after Collector::Close()
		// returned true to it.
		std::vector<Slice> Reconcile(const Collector& a_closed, const Arbiter& a_arbiter,
			const CompletenessJudge& a_judge, std::optional<float> a_terminalFall);

		// Has anything of this turn been reconciled at all. For the log line that
		// says a turn ended without ever producing a slice.
		bool Emitted() const;

	private:
		const std::int64_t _turnId;
		TurnSettings       _turn;
		SegmentSettings    _segments;

		// THE TURN'S OWN LOCK. It guards the serial below and the pieces already
		// sent, and it is held for the whole of Reconcile.
		mutable std::mutex _lock;

		// The order guard. Written in Reconcile and nowhere else.
		std::int32_t _lastReconciled{ 0 };

		// What has already gone out, so that a later reading can be matched
		// against it by time and can say which pieces it swallowed. Two pieces
		// within TurnSettings::matchSlackMs of each other are the same piece
		// (engine/turn.py:185-201).
		std::vector<Slice> _sent;
	};
}
