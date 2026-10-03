#include "turn/Resample.h"

#include "Loc.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Voice
{
	// WHAT THE REFERENCE DID AND WHAT IT COST.
	//
	// engine/audio.py:53-65 converted every block on its own, with no memory
	// between calls, and the callback handed the result straight to the queue
	// (engine/audio.py:115). Three consequences, and two of them are defects this
	// file exists to remove:
	//
	//   THE DRIFT (engine/audio.py:61). On the integer path it kept
	//   `len(block) // step * step` samples and threw away what was left over -
	//   every block, for ever. At 48 kHz, step 3 and the 1024-frame blocks the
	//   service actually asked for (voice-service.py:206), that is 1024 = 341*3 + 1:
	//   one source sample lost per block, 46.875 blocks a second, 46.875 source
	//   samples a second, 15.625 output samples a second. The stream therefore came
	//   out at 15984 samples per second of sound instead of 16000 - a tenth of a
	//   percent, which is a second and a half per hour of open microphone, and which
	//   moves every anchor away from the sound it names. It is invisible in a short
	//   test and permanent in a long one.
	//
	//   THE CLICK AT EVERY BLOCK BOUNDARY (engine/audio.py:63-65). np.interp was
	//   given `linspace(0, len-1, want)`: the phase restarted at zero in each block,
	//   the block's first source sample was forced onto the first output sample and
	//   its last onto the last, so the spacing inside a block was (len-1)/(want-1)
	//   rather than the true ratio, and the seam between two blocks was a small
	//   discontinuity fifty times a second - in the band the pitch tracker reads.
	//
	//   THE ONE THING IT GOT RIGHT, and which is kept here: it AVERAGED whole groups
	//   rather than picking every Nth sample (engine/audio.py:53-56 says exactly
	//   why). Picking would fold everything above 8 kHz back down into the middle of
	//   speech; the average is a crude low-pass in front of the decimation.
	//
	// So the remainder of the integer path and the phase of the fractional one are
	// members, carried from block to block, and only Reset clears them. Nothing here
	// counts time: the sample count is the clock, and this class is the one place
	// where the number of samples legitimately changes.
	//
	// NOTHING IN HERE IS TUNABLE AND NOTHING IS READ FROM THE SETTINGS. The output
	// rate is kTargetSampleRate, which is the contract's and not a preference
	// (engine/audio.py:20 spells it the same way, as a module constant); the source
	// rate is what the device announced, and it arrives through Reset. There is no
	// third number to tune - a filter length or a quality switch would be one, and
	// there is none, which is why this file takes no settings at all.

	void Resampler::Reset(std::uint32_t a_sourceRate)
	{
		// EVERY CARRIED THING GOES, AND THIS IS THE ONLY PLACE IT MAY. A reset in the
		// middle of a stream is exactly the reference's defect written by hand.
		_sourceRate = a_sourceRate;
		_step = 0;
		_sum = 0.0f;
		_have = 0;
		_phase = 0.0;
		_last = 0.0f;
		_primed = false;

		if (_sourceRate == 0) {
			// Nobody asked for a rate. Passing the samples through keeps the sound
			// audible and the clock honest for a device that is already at the target
			// rate, and says so out loud rather than going silent: a resampler that
			// quietly produced nothing would look exactly like a microphone that hears
			// nothing.
			_sourceRate = kTargetSampleRate;
			Loc::Warn("$SPEECHBROKERVOICE_LOG_RESAMPLE_NO_RATE", kTargetSampleRate);
		}

		if (_sourceRate == kTargetSampleRate) {
			// A group of one. Kept as its own step so that Feed can copy the block
			// instead of walking it sample by sample to divide each one by one.
			_step = 1;
			Loc::Info("$SPEECHBROKERVOICE_LOG_RESAMPLE_DIRECT", _sourceRate);
			return;
		}

		if (_sourceRate % kTargetSampleRate == 0) {
			// 48000, 32000, and the 96000 a studio interface comes up at: whole groups
			// of samples are averaged (engine/audio.py:59-62).
			_step = _sourceRate / kTargetSampleRate;
			Loc::Info("$SPEECHBROKERVOICE_LOG_RESAMPLE_AVERAGE", _sourceRate, kTargetSampleRate, _step);
			return;
		}

		// 44100, 22050, and whatever a headset invents. Linear interpolation with the
		// phase carried across the block boundary (engine/audio.py:63-65, without its
		// per-block restart).
		_step = 0;
		Loc::Info("$SPEECHBROKERVOICE_LOG_RESAMPLE_INTERPOLATE", _sourceRate, kTargetSampleRate);
	}

	std::size_t Resampler::Feed(std::span<const Sample> a_from, std::vector<Sample>& a_into)
	{
		const std::size_t count = a_from.size();
		if (count == 0 || _sourceRate == 0) {
			// _sourceRate is zero only before the first Reset, which the owner does at
			// Start and at every capture epoch. Nothing to say and nothing to do.
			return 0;
		}

		// IT APPENDS AND IT NEVER CLEARS: the caller owns the buffer for the life of
		// the session. The room is taken once, for the whole block, and taken back to
		// the true length once at the end - so the loops below have no capacity check
		// in them and there is exactly one call that could ever allocate. It does not:
		// the caller reserves with Expect, and resize over spare capacity only moves
		// the size. Shrinking a vector never reallocates.
		const std::size_t base = a_into.size();
		a_into.resize(base + Expect(count));

		Sample* const out = a_into.data() + base;
		std::size_t   made = 0;

		if (_step == 1) {
			// The device is already at the target rate. There is nothing to carry, so
			// the block goes over whole.
			std::memcpy(out, a_from.data(), count * sizeof(Sample));
			made = count;
		} else if (_step > 1) {
			// THE INTEGER PATH. The part-built group is the whole of the fix: the
			// reference dropped it (engine/audio.py:61) and we finish it with the first
			// samples of the next block, so not one source sample is ever thrown away
			// and the output rate is exactly kTargetSampleRate.
			const std::uint32_t step = _step;
			const float         scale = 1.0f / static_cast<float>(step);

			// Pulled into locals so that the carried state is written once per block
			// rather than once per sample.
			float         sum = _sum;
			std::uint32_t have = _have;

			for (std::size_t i = 0; i < count; ++i) {
				sum += a_from[i];
				if (++have == step) {
					out[made++] = sum * scale;
					sum = 0.0f;
					have = 0;
				}
			}

			_sum = sum;
			_have = have;
		} else {
			// THE FRACTIONAL PATH. One continuous position in the source stream rather
			// than one per block: _phase is where the next output sample falls,
			// measured from the LAST SAMPLE OF THE PREVIOUS BLOCK, and _last is that
			// sample - the left-hand end of the first interpolation of this block.
			// Position 0 is _last, position 1 is a_from[0], position j is a_from[j-1].
			const double stride = static_cast<double>(_sourceRate) / static_cast<double>(kTargetSampleRate);

			if (!_primed) {
				// The first output sample of a stream is its first input sample, so the
				// very beginning of a turn is the sound itself and not a blend with
				// silence. Phase 1 with _last set to a_from[0] gives exactly that.
				_last = a_from[0];
				_phase = 1.0;
				_primed = true;
			}

			const double limit = static_cast<double>(count);
			while (_phase < limit) {
				const double      whole = std::floor(_phase);
				const std::size_t left = static_cast<std::size_t>(whole);
				const float       part = static_cast<float>(_phase - whole);

				const Sample a = (left == 0) ? _last : a_from[left - 1];
				const Sample b = a_from[left];

				out[made++] = a + (b - a) * part;
				_phase += stride;
			}

			// Re-anchor on the end of this block. The subtraction is by a whole number
			// of samples, so it moves the origin without touching the phase itself -
			// which is what keeps the stride exact across a block boundary instead of
			// restarting it there.
			_last = a_from[count - 1];
			_phase -= limit;
		}

		a_into.resize(base + made);
		return made;
	}

	std::size_t Resampler::Expect(std::size_t a_count) const noexcept
	{
		if (a_count == 0 || _sourceRate == 0) {
			return 0;
		}

		if (_step == 1) {
			return a_count;
		}

		if (_step > 1) {
			// At most one group is already part-built, so the worst case is that the
			// block completes it and every group after it: (_have + a_count) / _step
			// with _have at its largest, which is _step - 1.
			return (a_count + _step - 1) / _step;
		}

		// The fractional path produces one output for every `stride` source samples,
		// and at most one more than that because the carried phase may sit just before
		// the start of the block. One spare on top of that one, so that a rounding of
		// the ratio can never cost the caller a sample.
		const double stride = static_cast<double>(_sourceRate) / static_cast<double>(kTargetSampleRate);
		return static_cast<std::size_t>(static_cast<double>(a_count) / stride) + 2;
	}
}
