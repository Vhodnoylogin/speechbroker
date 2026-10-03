#pragma once

#include "turn/Prosody.h"

#include <cstdint>
#include <optional>

namespace Voice
{
	// The weights and thresholds of "has the sentence finished". The shipped values
	// are voice.json:78-83, read in the reference at engine/judge.py:19-23.
	struct CompletenessSettings
	{
		// A pause counts as the end of a phrase RELATIVE TO THE SPEAKER'S OWN
		// TEMPO. An absolute threshold would be right for exactly one speaking
		// speed (engine/judge.py:17-19). voice.json:79, engine/judge.py:19.
		double pauseFactor{ 2.5 };

		// What a full stop, a question mark or an exclamation mark from the model
		// is worth on its own. voice.json:80, engine/judge.py:20.
		double withTerminalMark{ 0.80 };

		// And what their absence is worth. voice.json:81, engine/judge.py:21.
		double withoutTerminalMark{ 0.20 };

		// Above this no-speech probability the model was inventing over silence and
		// its punctuation means nothing at all. voice.json:82,
		// engine/judge.py:22 and :34-36.
		double junkNoSpeechProb{ 0.6 };

		// How much of the verdict the measured tone carries against the model's
		// punctuation. Not in voice.json - it is defaulted in the code of the
		// reference, engine/judge.py:23 - and it is a settings field here for the
		// same reason warmUpMs is: a number that is tuned is a number a player can
		// reach.
		double pitchWeight{ 0.5 };
	};

	// What the judge needs to know about one fragment of a model's reading.
	//
	// IT IS DELIBERATELY NOT THE CONTRACT'S FRAGMENT. Nothing in turn/ or audio/
	// includes speechbroker-voice-model.h: the half that owns the microphone must
	// build, run and be tested with no model, no bridge and no game anywhere near
	// it. The dispatch half fills this in from a model's answer, which is a handful
	// of assignments, and in exchange this whole half stays testable from a wav.
	//
	// The fields are engine/parts.py:36-43.
	struct FragmentSigns
	{
		// The model ended it with . ! or ? (engine/parts.py:39).
		bool endsSentence{ false };

		// The confidence of the last word - the available stand-in for "the
		// probability of the full stop itself", which no recogniser hands out
		// (engine/parts.py:40, engine/judge.py:50-53). Below zero means "not
		// given", and that is a third state, not a zero confidence.
		float lastWordProb{ -1.0f };

		// engine/parts.py:41.
		float noSpeechProb{ 0.0f };

		// The median gap between words inside the fragment: the speaker's own tempo,
		// which the pause after it is measured against (engine/parts.py:42).
		std::uint32_t medianGapMs{ 0 };

		// engine/parts.py:43.
		std::uint32_t words{ 0 };
	};

	// The only place the weights and thresholds of completeness live
	// (engine/judge.py:12-13).
	//
	// HOW LITTLE OF IT IS NEEDED IS THE POINT. Inside one reading every fragment
	// but the last is finished BY CONSTRUCTION - speech follows it, so the speaker
	// finished it - and all the arithmetic below applies to one fragment per pass,
	// the tail (engine/judge.py:3-7).
	//
	// THREAD: any. The judge is its settings and nothing else - no state, no
	// allocation, no logging - so one instance is shared by every worker and none
	// of them needs a lock. That is also why it is const throughout: a judge that
	// learned something between two passes would make two passes over one turn
	// incomparable, which is the property the whole contract is built on.
	class CompletenessJudge
	{
	public:
		explicit CompletenessJudge(const CompletenessSettings& a_settings) :
			_settings(a_settings)
		{}

		// The probability that the sentence ended on this fragment, in [0, 1].
		//
		// a_hasSpeechAfter - there is another fragment after it in the SAME
		// reading. Then the answer is 1.0 and nothing is computed: a fact beats
		// every sign (engine/judge.py:30-33).
		//
		// a_silenceAfterMs - the gap to the next fragment, or, for the last one,
		// the silence that fired this pass (engine/turn.py:157).
		//
		// a_terminalFall - the measured tone, when there is one. Empty means the
		// prosody had no opinion, and the judge then weighs the punctuation alone
		// rather than treating "no opinion" as "did not fall"
		// (engine/judge.py:47-49).
		float Judge(const FragmentSigns& a_signs, bool a_hasSpeechAfter,
			std::uint32_t a_silenceAfterMs, std::optional<float> a_terminalFall) const;

		const CompletenessSettings& Settings() const noexcept { return _settings; }

	private:
		CompletenessSettings _settings;
	};
}
