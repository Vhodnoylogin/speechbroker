#include "turn/Completeness.h"

#include "Loc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace Voice
{
	namespace
	{
		// THE THREE HALVES OF THE REFERENCE, NAMED.
		//
		// They are not tuned weights and they are not settings, which is why the
		// header stops at five fields. Each of them is a piece of the SHAPE of the
		// formula rather than a number somebody measured:
		//
		//   - the confidence of the last word is mapped onto the upper half of the
		//     range, so that a model which is sure of nothing still leaves half the
		//     verdict standing instead of erasing it (engine/judge.py:53);
		//   - a pause that says "finished" moves the verdict halfway to one, and a
		//     pause too short even for this speaker's own tempo moves it halfway to
		//     zero. The reference wrote the second as `value *= 0.5`
		//     (engine/judge.py:59 and :61); halving is exactly a move halfway to
		//     zero, and in binary floating point the two are the same bits, so
		//     saying it the same way in both branches costs no number;
		//   - the ratio the second branch compares against is one, because one IS
		//     the unit of that ratio: a pause shorter than the speaker's own median
		//     gap between words is not a pause at all (engine/judge.py:60).
		//
		// If any of them ever has to be moved, it becomes a settings field and the
		// header changes with it - not a literal edited here.
		constexpr double kLastWordFloor = 0.5;   // engine/judge.py:53
		constexpr double kPauseShift = 0.5;      // engine/judge.py:59, :61
		constexpr double kOwnTempo = 1.0;        // engine/judge.py:60

		// A probability is a finite number in [0, 1] or it is not a probability.
		bool IsProbability(double a_value) noexcept
		{
			return std::isfinite(a_value) && a_value >= 0.0 && a_value <= 1.0;
		}

		// Said once for the life of the process, and then never again.
		//
		// The header promises that the judge has no state, no allocation and no
		// logging, and that promise is what lets one const instance be shared by
		// every worker without a lock. This does not break it: the latch is a
		// function-local flag and not a member, so two judges cannot differ by it;
		// it is lock free; nothing here is on the per-block path - the judge runs
		// once per fragment of one model's answer, off the capture thread entirely;
		// and a fragment arriving with a number that is not a probability is a fault
		// in whoever filled FragmentSigns in, which would otherwise be invisible
		// forever. One line is the smallest way to make it visible.
		void SayOnceAboutBadInput(double a_noSpeech, double a_lastWord, double a_fall)
		{
			static std::atomic_flag said = ATOMIC_FLAG_INIT;
			if (!said.test_and_set(std::memory_order_relaxed)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_JUDGE_BAD_INPUT",
					a_noSpeech, a_lastWord, a_fall);
			}
		}
	}

	float CompletenessJudge::Judge(const FragmentSigns& a_signs, bool a_hasSpeechAfter,
		std::uint32_t a_silenceAfterMs, std::optional<float> a_terminalFall) const
	{
		// A fact beats every sign. If there is more speech after this fragment in
		// the SAME reading, the speaker finished it by construction, and nothing
		// below is computed at all (engine/judge.py:29-32).
		if (a_hasSpeechAfter) {
			return 1.0f;
		}

		// WHAT THE REFERENCE DID WITH A NUMBER THAT IS NOT A PROBABILITY, AND WHY
		// THIS DOES NOT DO IT.
		//
		// Every comparison against a NaN is false, so a NaN walked past the junk
		// test (engine/judge.py:35), past the last-word test (:52) and out of
		// `max(0.0, min(1.0, value))` at :63 as 1.0 - because Python's min returns
		// its first argument when the comparison is false. A model that handed out
		// one broken float therefore closed a phrase the player was still speaking,
		// with the highest confidence the judge can express, and said nothing. Here
		// a number that is not a probability is "not given", which is a state this
		// judge already has for two of these three fields, a finite number outside
		// the range is brought into it, and the fault is reported once.
		const double rawNoSpeech = static_cast<double>(a_signs.noSpeechProb);
		const double rawLastWord = static_cast<double>(a_signs.lastWordProb);
		const double rawFall = a_terminalFall ? static_cast<double>(*a_terminalFall) : -1.0;

		// Below zero is the documented "not given" for the last word, not a broken
		// number (Completeness.h:56-57, engine/parts.py:40), so it is excluded from
		// the complaint and from the clamp alike.
		const bool lastWordGiven = std::isfinite(rawLastWord) && rawLastWord >= 0.0;

		const bool inputSound = IsProbability(rawNoSpeech) &&
		                        (!lastWordGiven || rawLastWord <= 1.0) &&
		                        (!a_terminalFall || IsProbability(rawFall));
		if (!inputSound) {
			SayOnceAboutBadInput(rawNoSpeech, rawLastWord, rawFall);
		}

		// A non-finite no-speech probability becomes zero rather than one: the
		// reference's effective answer was "not junk", and turning an unreadable
		// number into "junk" would throw away speech that was actually there.
		const double noSpeech = std::isfinite(rawNoSpeech) ? std::clamp(rawNoSpeech, 0.0, 1.0) : 0.0;
		const double lastWord = lastWordGiven ? std::clamp(rawLastWord, 0.0, 1.0) : 0.0;

		// PROSODY HAS THREE ANSWERS, NOT TWO. Empty is "cannot tell", and it must
		// not become 0.0, which reads as "they will carry on"
		// (engine/prosody.py:90-93, engine/judge.py:47).
		const std::optional<double> fall =
			(a_terminalFall && std::isfinite(rawFall)) ?
				std::optional<double>{ std::clamp(rawFall, 0.0, 1.0) } :
				std::nullopt;

		// Invention over silence: the model was transcribing a pause, so its
		// punctuation means nothing at all (engine/judge.py:34-36).
		if (noSpeech >= _settings.junkNoSpeechProb) {
			return 0.0f;
		}

		// The punctuation is ONE input and not the main one. A recogniser's full
		// stop is three quarters language: it has seen millions of texts in which a
		// lone noun stands with a full stop, and it will put one after "Fireball"
		// however that was said (engine/judge.py:38-42).
		//
		// The whole sum is carried in double, as it was in the reference where a
		// Python float is a double, and narrowed exactly once on the way out. The
		// dispatch half compares this number against a threshold, and a verdict that
		// moved because the arithmetic was done a bit wider or a bit narrower would
		// be a difference nobody could account for.
		double value = a_signs.endsSentence ?
			_settings.withTerminalMark :
			_settings.withoutTerminalMark;

		// A measured tone carries no language prior: the last word sitting down on
		// the bottom of the speaker's own range is a measurement, not a guess about
		// how people usually write. That is why it outweighs the mark
		// (engine/judge.py:44-48).
		if (fall) {
			value = (1.0 - _settings.pitchWeight) * value + _settings.pitchWeight * *fall;
		}

		// The confidence of the last word is the available stand-in for "the
		// probability of the full stop itself", which no recogniser hands out
		// (engine/judge.py:50-53).
		//
		// THIS IS THE DANGEROUS SENTINEL OF THE TWO. The test is `>= 0`, so a
		// FragmentSigns that was zero-filled rather than filled in PASSES it and
		// halves every verdict for the rest of the session - quietly, because 0.0 is
		// also a legal confidence and there is no way to tell the two apart from in
		// here. Whoever fills this in from a model's answer must write -1 when the
		// model gave no per-word probability; that is what Completeness.h's default
		// is, and it is the reason for the default (engine/models.py:166 does the
		// same thing on the Python side). The other sentinel is safe: medianGapMs is
		// tested `> 0` by both of its readers (engine/judge.py:56,
		// engine/arbiter.py:42), so a zero there simply means "no tempo known" and
		// the pause is left out of the verdict.
		if (lastWordGiven) {
			value *= kLastWordFloor + (1.0 - kLastWordFloor) * lastWord;
		}

		// The pause after the fragment, weighed against the tempo of the speech
		// inside it. An absolute threshold in milliseconds would be right for
		// exactly one speaking speed (engine/judge.py:17-18, :55-61).
		//
		// The reference divided by `max(1, median_gap_ms)` at :57, which could never
		// fire: the same line's guard had already required the gap to be above zero.
		// The guard is what makes the division safe, here as there, and the dead
		// max is not carried over.
		//
		// Both of these are unsigned, so the "negative silence" the reference could
		// produce from overlapping fragments (engine/turn.py:157 subtracts one
		// boundary from another) cannot arrive as a small negative number here - it
		// would arrive as an enormous positive one. The caller subtracts, and the
		// caller must not let that subtraction go below zero.
		if (a_silenceAfterMs > 0 && a_signs.medianGapMs > 0) {
			const double ratio = static_cast<double>(a_silenceAfterMs) /
			                     static_cast<double>(a_signs.medianGapMs);
			if (ratio >= _settings.pauseFactor) {
				value += (1.0 - value) * kPauseShift;   // halfway to "finished"
			} else if (ratio < kOwnTempo) {
				value -= value * kPauseShift;           // halfway to "carrying on"
			}
		}

		// engine/judge.py:63. std::clamp is given the arguments in the order that
		// makes a NaN impossible to pass through as certainty; nothing above can
		// produce one any more, and this is the last line that could hide it.
		return static_cast<float>(std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0);
	}
}
