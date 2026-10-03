#include "turn/Pacer.h"

#include "Loc.h"

#include <algorithm>
#include <cstdint>

namespace Voice
{
	namespace
	{
		// A threshold out of the settings file, turned into the only clock this
		// module has. It happens three times per pacer and never again: everything
		// below compares one sample count with another, which is the whole reason
		// the three calls of this class carry no arithmetic at all.
		//
		// The clamp is not decoration. The settings file is a text file a player
		// edits by hand, and a minus sign in it would reach MsToSamples, which
		// takes an unsigned: -300 ms would come out as some sixteen million years
		// of silence and the rule holding that threshold would simply never fire
		// again. A negative pause is read as no pause.
		SampleIndex Threshold(int a_ms) noexcept
		{
			return MsToSamples(static_cast<std::uint64_t>(std::max(0, a_ms)));
		}
	}

	Pacer::Pacer(const PacerSettings& a_settings) :
		_sliceSilence(Threshold(a_settings.sliceSilenceMs)),
		_endSilence(Threshold(a_settings.endSilenceMs)),
		_maxSpan(Threshold(a_settings.maxSpanMs))
	{
		Loc::Debug("$SPEECHBROKERVOICE_LOG_PACER_READY",
			SamplesToMs(_sliceSilence), SamplesToMs(_endSilence), SamplesToMs(_maxSpan));

		// The ceiling is meant for somebody who never pauses (engine/turn.py:19-22),
		// so it sits far above the pause rule - 4000 ms against 300. Set the other
		// way round it does not break anything, it quietly takes the pause rule's
		// job away: the ceiling would come up before the speaker ever drew breath,
		// and every pass would be cut mid-word. The reference said nothing about
		// this; we say it once, at the start, rather than leaving somebody to
		// wonder why their pauses stopped mattering.
		if (_maxSpan <= _sliceSilence) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_PACER_SPAN_TOO_SHORT",
				SamplesToMs(_maxSpan), SamplesToMs(_sliceSilence));
		}
	}

	void Pacer::Note(bool a_loud) noexcept
	{
		// engine/turn.py:31-36. Speech arms the pacer, silence does not disarm it -
		// only a pass that fired on the pause rule does. That asymmetry IS the
		// latch: without it "silence longer than the threshold" stays true for the
		// whole rest of the turn and the first pause fires a pass on every block
		// after it, which is fifty runs of a model a second.
		if (a_loud) {
			_armed = true;
		}
	}

	bool Pacer::ShouldFire(SampleIndex a_silence, SampleIndex a_sinceLastPass) noexcept
	{
		// THE PAUSE RULE, and it is the main one (engine/turn.py:39-41). It costs
		// nothing when there is no pause: somebody speaking without a break never
		// arms and disarms anything, they simply get one pass at the end.
		if (_armed && a_silence >= _sliceSilence) {
			_armed = false;  // one pause, one pass - engine/turn.py:40
			return true;
		}

		// THE CEILING (engine/turn.py:42). It does NOT consume the arming, and that
		// is deliberate rather than an oversight of the reference: the latch is
		// about pauses, and a pause that arrives shortly after a ceiling pass is
		// still a real boundary of speech and still deserves its own pass.
		if (a_sinceLastPass < _maxSpan) {
			return false;
		}

		// A DEFECT OF THE REFERENCE, FIXED HERE.
		//
		// engine/turn.py:42 fires the ceiling on the span alone, without asking
		// whether anything was actually said during it. With the shipped numbers
		// that can never happen - a turn is closed by 1600 ms of silence long
		// before 4000 ms of it can pass - but all three of these are settings a
		// player may edit, and endSilenceMs above maxSpanMs turns the ceiling into
		// a metronome: every maxSpanMs of one long pause it fires another pass,
		// each one byte for byte the pass before it (the trailing silence is cut
		// off, so there is literally nothing new in it), each one a spent serial
		// and a whole run of a model on a GPU for an answer that is already known.
		//
		// The test needs no new state, which is what makes it safe to add to a
		// class whose one bit of state is the latch: if the silence run is as long
		// as the whole span since the last pass, then every sample since that pass
		// was silence. A single loud block anywhere in the span puts the silence
		// run back to zero and the ceiling fires exactly as the reference does.
		if (a_silence >= a_sinceLastPass) {
			return false;
		}

		Loc::Debug("$SPEECHBROKERVOICE_LOG_PACER_SPAN", SamplesToMs(a_sinceLastPass));
		return true;
	}

	bool Pacer::TurnOver(SampleIndex a_silence) const noexcept
	{
		// engine/turn.py:45-46, asked by engine/engine.py:78 BEFORE the pass rule,
		// because a turn that is over fires its pass as the final one.
		//
		// Pure, and it has to stay pure: the caller asks it once per block and then
		// asks ShouldFire, and a TurnOver that consumed the latch would eat the
		// arming of a pass that had not been fired yet.
		//
		// endSilenceMs at 0 ends every turn on its first block. It is left as the
		// player wrote it - a floor invented here would be a constant in the code
		// standing over a number in the settings file, and the number in the file
		// is the one that is supposed to win.
		return a_silence >= _endSilence;
	}

	void Pacer::Reset() noexcept
	{
		// A NEW TURN, OR A NEW DEVICE, STARTS DISARMED - AND THE REFERENCE DID NOT.
		//
		// engine/engine.py:58 builds one pacer for the whole session and never
		// clears its latch, so arming survived the end of a turn. There it was
		// invisible, because a turn can only begin on a loud block and a loud block
		// arms the pacer anyway. Here a turn also ends at the ceiling of
		// CutRules::maxSamples, in the middle of a word, and a device change closes
		// a turn without any silence at all - so the latch can outlive its turn
		// carrying an arming earned by speech that belongs to a turn already sent.
		// The next turn would then fire its first pass on the first pause it met,
		// however early, on the strength of somebody else's speech.
		_armed = false;
	}
}
