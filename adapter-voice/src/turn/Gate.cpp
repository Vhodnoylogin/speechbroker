#include "turn/Gate.h"

#include "Loc.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace Voice
{
	namespace
	{
		// THE SCRATCH FOR THE MEASUREMENT IS SIZED FOR A BLOCK OF ONE MILLISECOND.
		//
		// The reference knew its block size and could say how many levels it would
		// collect (`need = int(seconds * 1000 / self.block_ms)`, engine/audio.py:129).
		// We do not: the blocks arrive at whatever size WASAPI, a headset or a test
		// file hands us, and the gate is forbidden to allocate once it is running.
		// So the buffer is reserved for the smallest block anybody could plausibly
		// deliver and then never grows. At four bytes an entry a whole second of
		// one-millisecond blocks costs four kilobytes, once per device.
		//
		// It is NOT a tunable and it is not a threshold: a block shorter still is not
		// refused, its level simply does not join the sample - by then a thousand
		// others have already decided the median.
		constexpr std::uint64_t kShortestAccountedBlockMs = 1;

		// np.median (engine/audio.py:134, voice-service.py:250), and worth copying
		// exactly: with an even count numpy averages the two central values, and a
		// floor that differed from the reference's by half a step would mean every
		// threshold in voice.json was tuned against a slightly different number.
		//
		// It reorders what it is given, which is why it takes the buffer by
		// reference: the levels are dead the moment the floor is known.
		float Median(std::vector<float>& a_values) noexcept
		{
			if (a_values.empty()) {
				return 0.0f;
			}
			const auto middle = a_values.size() / 2;
			std::nth_element(a_values.begin(), std::next(a_values.begin(), static_cast<std::ptrdiff_t>(middle)),
				a_values.end());
			const auto upper = a_values[middle];
			if (a_values.size() % 2 != 0) {
				return upper;
			}
			// nth_element has already put everything below the middle to its left, so
			// the other central value is the largest of that left part.
			const auto lower = *std::max_element(a_values.begin(),
				std::next(a_values.begin(), static_cast<std::ptrdiff_t>(middle)));
			return 0.5f * (lower + upper);
		}

		// The pre-roll ring. It is a free function and not a method because the
		// header declares the members and nothing else, and the header is not ours to
		// change.
		//
		// Two memcpys at most and no branch that depends on the sound - the cost of a
		// block is the same block after block for the whole session.
		void PushRing(std::vector<Sample>& a_ring, std::size_t& a_at, std::size_t& a_held,
			std::span<const Sample> a_block) noexcept
		{
			const auto capacity = a_ring.size();
			if (capacity == 0) {
				return;
			}
			auto from = a_block;
			if (from.size() > capacity) {
				// A block longer than the whole pre-roll: only its newest end can be
				// kept, and the older part of it would have been thrown away on the
				// next block anyway.
				from = from.last(capacity);
			}
			const auto first = std::min(from.size(), capacity - a_at);
			std::memcpy(a_ring.data() + a_at, from.data(), first * sizeof(Sample));
			if (first < from.size()) {
				std::memcpy(a_ring.data(), from.data() + first, (from.size() - first) * sizeof(Sample));
			}
			a_at = (a_at + from.size()) % capacity;
			a_held = std::min(capacity, a_held + from.size());
		}
	}

	Gate::Gate(const VadSettings& a_settings) :
		_settings(a_settings)
	{
		// EVERY THRESHOLD IS TURNED INTO SAMPLES HERE AND NOWHERE ELSE. Offer runs
		// fifty times a second for the whole session and never divides by a thousand;
		// the reference compared milliseconds it had added up block by block
		// (voice-service.py:278, :287), which is exact only while every block is the
		// same size, and ours are not promised to be.
		//
		// The wall clock of the reference goes the same way. measure_floor sat on
		// time.time() twice (voice-service.py:242, :246); a sample count cannot
		// disagree with the sound, and a machine that stalls for a second here gets
		// the floor it asked for rather than a floor measured over less sound than it
		// ordered.
		_warmUp = MsToSamples(static_cast<std::uint64_t>(std::max(0, _settings.warmUpMs)));
		_floorWindow = SecondsToSamples(_settings.noiseFloorSec);
		_startNeeded = MsToSamples(static_cast<std::uint64_t>(std::max(0, _settings.startMs)));
		_preRollCapacity = static_cast<std::size_t>(
			MsToSamples(static_cast<std::uint64_t>(std::max(0, _settings.preRollMs))));

		Reset();
	}

	void Gate::Reset()
	{
		_seen = 0;
		_above = 0;
		_ready = false;
		_open = false;
		_floor = 0.0f;
		_trigger = 0.0f;

		// THE ONLY PLACE IN THE CLASS THAT ALLOCATES, and after the first device it
		// does not even do that: both buffers keep the size they had, and assign over
		// a vector of the same length neither grows nor frees.
		_levels.clear();
		_levels.reserve(static_cast<std::size_t>(_floorWindow / MsToSamples(kShortestAccountedBlockMs)) + 1);
		_preRoll.assign(_preRollCapacity, Sample{ 0.0f });
		_preRollAt = 0;
		_preRollHeld = 0;

		Loc::Debug("$SPEECHBROKERVOICE_LOG_FLOOR_MEASURING", SamplesToMs(_warmUp), SamplesToMs(_floorWindow));
	}

	Heard Gate::Offer(std::span<const Sample> a_block) noexcept
	{
		Heard heard;
		// Read before anything is decided: the block that COMPLETES the measurement
		// is still a block of the measurement, exactly as it was in the reference,
		// where measure_floor consumed it and capture_loop never saw it
		// (voice-service.py:247-249). The caller throws it away; the person was asked
		// not to speak.
		heard.ready = _ready;

		if (a_block.empty()) {
			return heard;
		}

		heard.rms = Rms(a_block);
		_seen += a_block.size();

		if (!_ready) {
			// THE WARM-UP DISCARD. The first packets after a stream opens are digital
			// silence, and a floor measured on them is zero - which is to say a
			// threshold that means nothing (voice-service.py:242-244, where the half
			// second was a constant in the code; here it is warmUpMs, because a
			// headset that comes up slower is a thing a player can meet and a
			// constant is the one number they cannot reach).
			if (_seen > _warmUp) {
				if (_levels.size() < _levels.capacity()) {
					// One level per block, the same measurement as the trigger's
					// (engine/audio.py:131, voice-service.py:249).
					_levels.push_back(heard.rms);
				}
				if (_seen >= _warmUp + _floorWindow) {
					// voice-service.py:250-251 and engine/audio.py:134-135, which are
					// the same two lines twice. An empty sample means a floor of zero,
					// and the trigger then falls back to minRmsFloor on its own - the
					// gate is never left without one.
					_floor = Median(_levels);
					_trigger = std::max(static_cast<float>(_settings.minRmsFloor),
						_floor * static_cast<float>(_settings.startFactor));

					// NOT IN THE REFERENCE, and it costs one comparison once per
					// device. A single bad sample out of a driver - a NaN or an
					// infinity - would poison the median, and every comparison against
					// a NaN trigger is false: the gate would never open again and
					// would say nothing about it. Falling back to the settings' own
					// floor is the one answer that still works.
					if (!std::isfinite(_trigger)) {
						_floor = 0.0f;
						_trigger = static_cast<float>(_settings.minRmsFloor);
					}
					_ready = true;
				}
			}
			return heard;
		}

		// engine/audio.py:138-139. THE TRIGGER IS FROZEN: it was computed once above
		// and nothing after this line touches _floor or _trigger. Speech raises the
		// level, so a threshold that followed the level would climb after the speaker
		// and stop telling speech from silence at all (engine/audio.py:82-86 is the
		// whole argument). Only Reset - a new device, a new file - starts again.
		heard.loud = heard.rms >= _trigger;

		if (_open) {
			// A turn is open: the rolling pre-roll is not kept and the debounce is not
			// counted. The reference did both only in its not-speaking branch
			// (voice-service.py:277-286), and the sound of the open turn is the
			// caller's to feed - it does not pass through here twice.
			return heard;
		}

		// THE ROLLING PRE-ROLL, including this very block: the words spoken before
		// the gate noticed them (voice-service.py:280-282). The reference counted it
		// in BLOCKS - `len(buf) > preRollMs / block_ms`, voice-service.py:280 - so it
		// held preRollMs plus one block, and held the wrong amount entirely the
		// moment the device changed its block size. Ours is a count of samples, like
		// every other length in this module.
		PushRing(_preRoll, _preRollAt, _preRollHeld, a_block);

		// voice-service.py:278: the debounce counts only while the sound stays above
		// the trigger and falls back to nothing the moment it does not.
		_above = heard.loud ? _above + a_block.size() : 0;

		// voice-service.py:283 is `above_ms >= v["startMs"]` alone. The `loud &&` in
		// front of it is a defect fixed: with startMs set to 0 - which the settings
		// offer as the way back to the reference's engine path, engine.py:64-70 - the
		// reference's test is true for a silent block as well, and a turn would open
		// on the first block of the session and every block after it. With the
		// shipped 120 ms the two tests are identical, because _above is only ever
		// non-zero after a loud block.
		if (heard.loud && _above >= _startNeeded) {
			heard.opens = true;
			_open = true;
			_above = 0;
		}
		return heard;
	}

	void Gate::TakePreRoll(std::vector<Sample>& a_into)
	{
		// APPENDS, and does not clear: the caller owns one buffer for the session.
		// What comes out is in the order it was heard, oldest first, and it is the
		// new turn's sample zero - the block that fired the trigger is the last of it
		// and must NOT be fed to the turn a second time.
		const auto capacity = _preRoll.size();
		if (capacity != 0 && _preRollHeld != 0) {
			const auto start = (_preRollAt + capacity - _preRollHeld) % capacity;
			const auto first = std::min(_preRollHeld, capacity - start);
			a_into.insert(a_into.end(), std::next(_preRoll.begin(), static_cast<std::ptrdiff_t>(start)),
				std::next(_preRoll.begin(), static_cast<std::ptrdiff_t>(start + first)));
			if (first < _preRollHeld) {
				a_into.insert(a_into.end(), _preRoll.begin(),
					std::next(_preRoll.begin(), static_cast<std::ptrdiff_t>(_preRollHeld - first)));
			}
		}
		// Emptied, not merely read: what has been handed over belongs to the turn now,
		// and a second call before the turn closes must not hand out the same sound
		// again.
		_preRollAt = 0;
		_preRollHeld = 0;
	}

	void Gate::Close() noexcept
	{
		// voice-service.py:291 clears the debounce together with the phrase. The
		// pre-roll goes with it: nothing was kept while the turn was open, and
		// whatever is still in the ring was handed to that turn when it opened.
		_open = false;
		_above = 0;
		_preRollAt = 0;
		_preRollHeld = 0;
	}

	float Gate::Rms(std::span<const Sample> a_block) noexcept
	{
		if (a_block.empty()) {
			return 0.0f;
		}
		// sqrt(mean(square(block))) * 32768, engine/audio.py:131 and :139 - a float
		// sample expressed in the numbers of a 16-bit one, which is the scale every
		// threshold in the settings is written in (kRmsScale).
		//
		// The sum is a double where the reference had numpy's: a block of a second of
		// quiet room is thousands of squares of numbers around 1e-3, and in a float
		// accumulator the later ones stop arriving. That is a floor measured low and
		// a trigger set low with it.
		double sum = 0.0;
		for (const auto sample : a_block) {
			const auto value = static_cast<double>(sample);
			sum += value * value;
		}
		return static_cast<float>(std::sqrt(sum / static_cast<double>(a_block.size())) *
			static_cast<double>(kRmsScale));
	}

	float Gate::Peak(std::span<const Sample> a_block) noexcept
	{
		// max(abs(block)) * 32768, voice-service.py:293. The gate itself never uses
		// it - the peak judges a finished buffer, not a block - but it is stated here
		// so that the worker above and the gate measure the same thing with the same
		// code, which is the only way the number in a log and the number in a
		// comparison can be trusted to agree.
		float peak = 0.0f;
		for (const auto sample : a_block) {
			peak = std::max(peak, std::fabs(sample));
		}
		return peak * kRmsScale;
	}
}
