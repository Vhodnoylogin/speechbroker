#pragma once

#include "turn/Resample.h"

namespace Voice
{
	// When to ask the models, and when the speaker has finished.
	//
	// The shipped values are voice.json:69-73 and they are read in the reference at
	// engine/turn.py:26-28.
	struct PacerSettings
	{
		// A short pause fires a pass. It is the main rule and it is free when it
		// does not happen: somebody speaking without pauses gets exactly one pass,
		// at the end, which is what the whole-phrase path used to do
		// (engine/turn.py:16-22). voice.json:70, engine/turn.py:26.
		int sliceSilenceMs{ 300 };

		// The silence that says the speaker has stopped. THIS is what ends a turn -
		// engine/engine.py:78 asks the pacer, not the gate - and it is the field
		// the contract means when it says a player may change it
		// (docs/model-host.md, "Numbers that are settings, not constants").
		// voice.json:71, engine/turn.py:27.
		int endSilenceMs{ 1600 };

		// The ceiling on how long a single pass may cover. Without it somebody who
		// never pauses would get one pass at the end of a long speech and the whole
		// delay in one lump (engine/turn.py:19-22). voice.json:72,
		// engine/turn.py:28.
		int maxSpanMs{ 4000 };
	};

	// The pacer holds one bit of state and it is the bit that matters: whether
	// speech has been heard since the last pass.
	//
	// WITHOUT THE ARMING THE FIRST PAUSE FIRES FOR EVER. "Silence longer than the
	// threshold" stays true for the whole rest of the turn, so the first pass would
	// become twenty - engine/turn.py:32-35 says exactly this, and it is the kind of
	// defect that shows as a load spike rather than as a wrong answer.
	//
	// EVERY THRESHOLD IS CONVERTED TO SAMPLES IN THE CONSTRUCTOR. Nothing in the
	// three calls below divides, multiplies or looks at a clock; they compare one
	// sample count with another. Work that can be done once is done once, and here
	// "once" is fifty times a second saved for the whole session.
	//
	// THREAD: the consumer's only.
	class Pacer
	{
	public:
		explicit Pacer(const PacerSettings& a_settings);

		// Speech arms the pacer. Called for every block, with the gate's verdict
		// (engine/turn.py:32-36).
		void Note(bool a_loud) noexcept;

		// a_silence is how many samples have passed since the last loud block;
		// a_sinceLastPass is how many since the last pass was fired.
		//
		// IT CONSUMES THE ARMING when it says yes on the pause rule, so the caller
		// must call it once per block and must not ask it twice about the same
		// block (engine/turn.py:39-43).
		bool ShouldFire(SampleIndex a_silence, SampleIndex a_sinceLastPass) noexcept;

		// The speaker has stopped (engine/turn.py:45-46). Pure: it consumes
		// nothing, and the caller asks it BEFORE ShouldFire, because a turn that is
		// over fires its pass as the final one rather than as another interim
		// (engine/engine.py:78-79 computes `over` first and passes it as `final`).
		bool TurnOver(SampleIndex a_silence) const noexcept;

		// A new turn, or a new device. Disarms.
		void Reset() noexcept;

		SampleIndex SliceSilence() const noexcept { return _sliceSilence; }
		SampleIndex EndSilence() const noexcept { return _endSilence; }
		SampleIndex MaxSpan() const noexcept { return _maxSpan; }

	private:
		SampleIndex _sliceSilence{ 0 };
		SampleIndex _endSilence{ 0 };
		SampleIndex _maxSpan{ 0 };
		bool        _armed{ false };
	};
}
