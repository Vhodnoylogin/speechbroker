#pragma once

#include "audio/Ring.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Voice
{
	// THE RATE EVERYTHING ABOVE THIS LINE IS MEASURED IN.
	//
	// It is not a setting and must not become one. The contract states the format
	// in SpeechBrokerVoiceFormat and every model is built against it; a player who
	// changed this number would not be tuning the adapter, they would be handing
	// every installed model a buffer it declared it could not take. The reference
	// spells it the same way, as a module constant (engine/audio.py:20 `RATE =
	// 16000`).
	inline constexpr std::uint32_t kTargetSampleRate = 16000;

	// THE ONLY CLOCK IN THIS MODULE.
	//
	// Every length, every threshold, every position - the noise floor window, the
	// start debounce, the pre-roll, the silence that arms a pass, the silence that
	// ends a turn, the ceiling on a turn, the anchors, the trim - is a count of
	// samples at kTargetSampleRate and nothing else. There is no steady_clock and
	// no system_clock anywhere in audio/ or turn/, and the two places a wall clock
	// is allowed - the consumer's idle sleep and the optional pacing of a file -
	// are forbidden to hand a number to anything that cuts.
	//
	// WHY, in one sentence: the sound is the only thing that knows how long the
	// sound was. Mix in a wall clock and the two disagree the moment a block is
	// late, a device is reopened or a frame takes 40 ms instead of 11, and they
	// disagree by a little - which is drift, which is the kind of defect that is
	// found months later by a mismatch of a few hundred milliseconds in a log.
	//
	// The reference measured in milliseconds because it added block_ms per block
	// (voice-service.py:278, :287, engine/engine.py:75-76), which is exact only
	// while the blocks are all the same size. Ours are not promised to be.
	using SampleIndex = std::uint64_t;

	// The settings speak milliseconds and seconds, because that is what a person
	// tuning them can think in, and the contract speaks milliseconds on the way
	// out. These two functions are the whole of the boundary between that and the
	// clock. They are the only place in the module where the two units meet.
	constexpr SampleIndex MsToSamples(std::uint64_t a_ms) noexcept
	{
		return a_ms * kTargetSampleRate / 1000ULL;
	}

	constexpr SampleIndex SecondsToSamples(double a_seconds) noexcept
	{
		return a_seconds <= 0.0 ? 0ULL
		                        : static_cast<SampleIndex>(a_seconds * static_cast<double>(kTargetSampleRate));
	}

	// Only for the log and for the millisecond fields of the contract. Never feed
	// the result back into a comparison against a threshold: convert the threshold
	// into samples once, at construction, and compare samples with samples.
	constexpr std::uint32_t SamplesToMs(SampleIndex a_samples) noexcept
	{
		return static_cast<std::uint32_t>(a_samples * 1000ULL / kTargetSampleRate);
	}

	// Bringing the device rate down to kTargetSampleRate, mono in and mono out -
	// the channels are already folded by the capture callback.
	//
	// STATEFUL, AND THAT IS THE WHOLE POINT OF THE CLASS. The reference resampled
	// each block on its own (engine/audio.py:53-65) and therefore carried two
	// defects that only show over time:
	//
	//   - on the integer path it kept `len(block) // step * step` samples and threw
	//     the remainder of EVERY block away (engine/audio.py:61). At 48 kHz and
	//     1024-frame blocks that is one sample in 1024 - about a millisecond a
	//     second, three seconds an hour, all of it invisible and all of it moving
	//     the anchors away from the sound;
	//
	//   - on the fractional path np.interp restarted its phase at every block
	//     (engine/audio.py:63-65), putting a small discontinuity at every block
	//     boundary - which is a click at the block rate, in the band the pitch
	//     tracker works in.
	//
	// So the remainder of the integer path and the phase of the fractional one are
	// members, carried across blocks, and the only thing that may clear them is
	// Reset.
	//
	// WHICH PATH. When the device rate divides exactly - 48000, 32000 - whole
	// groups of samples are averaged. Averaging rather than picking every third
	// sample is not decoration: it is a crude low-pass, and without it everything
	// above 8 kHz folds back down into the middle of speech (engine/audio.py:53-56
	// says exactly this). Otherwise - 44100, and whatever a headset invents -
	// linear interpolation, phase carried.
	//
	// THREAD: the consumer's, and only the consumer's. One resampler, one thread,
	// no lock, because it has state.
	class Resampler
	{
	public:
		Resampler() = default;

		// Point it at a device rate and clear every carried sample. Called once per
		// capture epoch - which is to say once at the start and once more every time
		// a device is reopened. A rate equal to kTargetSampleRate is legal and makes
		// Feed a copy.
		void Reset(std::uint32_t a_sourceRate);

		std::uint32_t SourceRate() const noexcept { return _sourceRate; }

		// Appends the converted samples to a_into and returns how many it appended.
		// It APPENDS: the caller owns one buffer for the life of the session,
		// clears it when it has consumed it and never lets it reallocate - work
		// that can be done once is done once, and a resampler that allocated per
		// block would allocate fifty times a second for ever.
		std::size_t Feed(std::span<const Sample> a_from, std::vector<Sample>& a_into);

		// The most a_count source samples can produce, so the caller can reserve
		// the output buffer once and know it will never grow. An upper bound, not a
		// promise of the exact number: the exact number depends on the carried
		// remainder.
		std::size_t Expect(std::size_t a_count) const noexcept;

	private:
		std::uint32_t _sourceRate{ 0 };

		// Non-zero on the integer path: how many source samples make one output
		// sample. Zero means the fractional path.
		std::uint32_t _step{ 0 };

		// The integer path: the part-built group that straddles the block boundary.
		float         _sum{ 0.0f };
		std::uint32_t _have{ 0 };

		// The fractional path: where in the source stream the next output sample
		// falls, and the last source sample of the previous block, which is the
		// left-hand end of the first interpolation of the next one.
		double _phase{ 0.0 };
		Sample _last{ 0.0f };
		bool   _primed{ false };
	};
}
