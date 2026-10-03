#pragma once

#include "turn/Resample.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Voice
{
	// Why the adapter measures the tone of the speaker itself, when a model already
	// returns punctuation: because the punctuation of a recogniser is three
	// quarters LANGUAGE. It has seen millions of texts in which a lone noun stands
	// with a full stop, and it will put one after "Fireball" no matter how that was
	// said - precisely where it matters most whether the speaker has finished or is
	// about to go on (engine/prosody.py:1-11).
	//
	// A terminal fall to the bottom of the speaker's own range has no such prior.
	// It is a measurement, not a guess about how people usually write, which is why
	// the judge weighs it above the punctuation mark (engine/judge.py:44-49).
	struct ProsodySettings
	{
		// The band a human voice is looked for in. engine/prosody.py:22-23.
		double minHz{ 70.0 };
		double maxHz{ 350.0 };

		// The analysis window and its step. engine/prosody.py:24-25.
		int frameMs{ 40 };
		int hopMs{ 20 };

		// How much of the end of a fragment the fall is measured over.
		// engine/prosody.py:87 (`tail_ms: int = 250`).
		int tailMs{ 250 };

		// How many voiced frames a range needs before it is worth believing.
		// engine/prosody.py:79 and :83 (`>= 8`).
		int voicedNeeded{ 8 };

		// How many voiced frames the tail itself needs. Below this there is nothing
		// to judge and the answer is "no opinion", which is a third outcome and not
		// a zero. engine/prosody.py:99-100 (`if len(voiced) < 3`).
		int voicedInTail{ 3 };

		// How strong the autocorrelation peak has to be against the zero lag before
		// it is called a pitch at all. A weak peak is noise or a whisper: there is
		// no tone there and inventing one is worse than admitting it.
		// engine/prosody.py:51-52 (`corr[peak] < 0.3 * corr[0]`).
		double peakRatio{ 0.3 };

		// The ends of the speaker's range, as percentiles rather than extremes: one
		// creak or one shout must not become the bottom or the top of a voice.
		// engine/prosody.py:77-78.
		int lowPercentile{ 10 };
		int highPercentile{ 90 };

		// The shortest fragment worth measuring a fall on. engine/engine.py:150
		// (`if to - at > self.sample_rate // 10`).
		int minSpanMs{ 100 };
	};

	// The range of the speaker, resolved: three numbers and nothing else.
	//
	// THERE CAN BE NO ABSOLUTE THRESHOLD HERE. Every voice is its own, and one
	// person speaks higher in a fight than in a menu, so the measurement is against
	// that speaker, in this turn (engine/prosody.py:62-66).
	//
	// It is a value type on purpose: it is COPIED INTO EVERY PASS. The worker that
	// measures the terminal fall long after the pass was cut must not reach into a
	// turn that is still being filled on another thread, and three floats copied at
	// cut time cost nothing beside the alternative, which is a lock around a live
	// turn.
	struct PitchSpan
	{
		float         low{ 0.0f };
		float         high{ 0.0f };
		std::uint32_t voiced{ 0 };

		// engine/prosody.py:81-83.
		bool Known(const ProsodySettings& a_settings) const noexcept
		{
			return voiced >= static_cast<std::uint32_t>(a_settings.voicedNeeded) && high > low;
		}
	};

	// The fundamental frequency, frame by frame, by autocorrelation - no library
	// and almost free. Accuracy to the hertz is not wanted: the only question is
	// whether the last word sat down on the bottom of its own range
	// (engine/prosody.py:12-15).
	//
	// IT OWNS ITS SCRATCH AND ALLOCATES ONLY IN Prepare. The reference allocated a
	// full correlation per frame (engine/prosody.py:49) fifty times a second; here
	// the windows are sized once and reused. Two more economies that change no
	// number:
	//
	//   - only the lags between rate/maxHz and rate/minHz are computed, not the
	//     whole correlation. The rest of it was never looked at
	//     (engine/prosody.py:50);
	//   - the mean is subtracted into the scratch window, not into the caller's
	//     samples. The caller's samples belong to a turn and may already be inside
	//     a shared snapshot.
	//
	// THREAD: one tracker belongs to one thread. The consumer has its own, for the
	// live turn; a worker that measures a terminal fall brings its own. They share
	// nothing, which is why neither needs a lock.
	class PitchTracker
	{
	public:
		explicit PitchTracker(const ProsodySettings& a_settings);

		// Size the scratch. Called once; calling it again is legal and reallocates.
		void Prepare();

		// Appends one f0 per frame to a_into - zero where there is no voice - and
		// returns how many frames it appended. Appends, and the caller reuses its
		// vector, for the usual reason (engine/prosody.py:28-56).
		std::size_t Track(std::span<const Sample> a_pcm, std::vector<float>& a_into);

		const ProsodySettings& Settings() const noexcept { return _settings; }

	private:
		ProsodySettings _settings;

		std::size_t _frame{ 0 };   // samples, from frameMs
		std::size_t _hop{ 0 };     // samples, from hopMs
		std::size_t _lagLow{ 0 };  // rate / maxHz
		std::size_t _lagHigh{ 0 }; // rate / minHz

		std::vector<double> _window;  // one frame, mean removed
	};

	// The speaker's range as it accumulates through a turn.
	//
	// It keeps a HISTOGRAM rather than every value. The reference kept a list that
	// grew for the whole turn and recomputed two percentiles over the whole of it
	// on every block (engine/prosody.py:73-79) - which is an unbounded allocation
	// and an O(n log n) in a path that runs fifty times a second. A histogram over
	// [minHz, maxHz] in one-hertz bins is fixed memory, allocated once, and gives
	// the same two percentiles to within the bin width, which is far finer than
	// anything this is used for.
	//
	// Resolve is where the percentiles are computed, so they are computed once per
	// pass instead of once per block.
	//
	// THREAD: the consumer's, through the live turn. What crosses to a worker is
	// the PitchSpan that Resolve returns, by value.
	class PitchRange
	{
	public:
		explicit PitchRange(const ProsodySettings& a_settings);

		// Every non-zero f0 of a frame counts; the zeros are the unvoiced frames
		// and are skipped (engine/prosody.py:74).
		void Add(std::span<const float> a_f0) noexcept;

		PitchSpan Resolve() const noexcept;

		// A new turn. Clears the counts, keeps the memory.
		void Reset() noexcept;

	private:
		ProsodySettings          _settings;
		std::vector<std::uint32_t> _bins;   // one per hertz across the band, allocated once
		std::uint32_t            _voiced{ 0 };
	};

	// How far the last word sat down onto the bottom of the speaker's own range.
	//
	//   1.0 - on the floor: the phrase sounds finished.
	//   0.0 - still up: that is what a continuation sounds like.
	//   nothing - no opinion: no voice was found, or the range is not known yet.
	//
	// The three outcomes are three, and the third is not a zero. A judge handed 0.0
	// for "I could not tell" would read it as "they are going to carry on"
	// (engine/prosody.py:86-108).
	//
	// The median of the last third of the tail is what is taken: the very end of an
	// utterance often falls into creak, where autocorrelation lies
	// (engine/prosody.py:102-105).
	//
	// THREAD: any, and that is the point - it is given a scratch tracker of the
	// caller's own and a PitchSpan by value, so a worker can call it on a snapshot
	// while the consumer is filling the next turn.
	std::optional<float> TerminalFall(PitchTracker& a_scratch, std::span<const Sample> a_tail,
		const PitchSpan& a_span);
}
