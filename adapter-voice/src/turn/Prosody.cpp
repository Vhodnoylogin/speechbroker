#include "turn/Prosody.h"

#include "Loc.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Voice
{
	namespace
	{
		// The step of a 16-bit capture, as a float. It is NOT a tuning knob and must
		// not become one: it is a property of the sample format, on the same footing
		// as kTargetSampleRate. It exists for one purpose - to tell a window that
		// holds no signal at all from one that holds a very quiet voice - because the
		// peak test below is a RATIO, and a ratio of zero to zero would report a
		// pitch in digital silence.
		//
		// The reference wrote that guard as an absolute `energy < 1e-6`
		// (engine/prosody.py:44-46), a number that only means anything for one window
		// length. Ours is that same magnitude derived rather than written down: one
		// quantisation step spread over the whole window, which at 40 ms and 16 kHz
		// comes to 6e-7. Everything below it is dither and rounding.
		constexpr double kQuantum = 1.0 / 32768.0;

		// The value a one-hertz bin stands for: its middle. Taking the lower edge
		// would bias every percentile down by half a hertz, and half a hertz is
		// nothing beside a voice - but it would be a bias, and biases add up.
		constexpr double kBinCentre = 0.5;
	}

	// ---------------------------------------------------------------- the track

	PitchTracker::PitchTracker(const ProsodySettings& a_settings) :
		_settings(a_settings)
	{
		// Sized here as well as in Prepare, so that a tracker is usable the moment it
		// is built: a worker that measures one terminal fall builds its scratch and
		// calls TerminalFall, and must not have to know about a second step.
		Prepare();
	}

	void PitchTracker::Prepare()
	{
		const auto frameMs = _settings.frameMs > 0 ? static_cast<std::uint64_t>(_settings.frameMs) : 0ULL;
		const auto hopMs = _settings.hopMs > 0 ? static_cast<std::uint64_t>(_settings.hopMs) : 0ULL;

		_frame = static_cast<std::size_t>(MsToSamples(frameMs));
		_hop = static_cast<std::size_t>(MsToSamples(hopMs));

		// The band, as lags rather than as hertz, converted once here and never
		// again - the inner loop compares lags with lags (engine/prosody.py:35-36).
		// _lagHigh is the EXCLUSIVE end, because the reference searched corr[lo:hi]
		// and python leaves the right-hand end out (engine/prosody.py:50).
		_lagLow = _settings.maxHz > 0.0 ?
		              static_cast<std::size_t>(static_cast<double>(kTargetSampleRate) / _settings.maxHz) :
		              0;
		const auto lagFromMin = _settings.minHz > 0.0 ?
		                            static_cast<std::size_t>(static_cast<double>(kTargetSampleRate) / _settings.minHz) :
		                            0;
		_lagHigh = _frame > 0 ? std::min(lagFromMin, _frame - 1) : 0;

		if (_frame == 0 || _hop == 0 || _lagLow == 0 || _lagHigh <= _lagLow) {
			// Settings that leave nothing to search in. The reference returned an
			// empty list and said nothing (engine/prosody.py:37-38); silence here
			// would mean a player who mistyped a number gets "the tone is never
			// measured" and no reason for it. _frame is zeroed so that Track answers
			// nothing rather than half of something.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_PROSODY_NO_BAND", _settings.frameMs, _settings.hopMs,
				_settings.minHz, _settings.maxHz);
			_frame = 0;
			_window.clear();
			return;
		}

		// The whole of the allocation of this class, and it happens here. The inner
		// loops below touch nothing but this window and the caller's buffer.
		_window.assign(_frame, 0.0);

		Loc::Debug("$SPEECHBROKERVOICE_LOG_PROSODY_BAND", _settings.frameMs, _settings.hopMs, _lagLow,
			_lagHigh, static_cast<double>(kTargetSampleRate) / static_cast<double>(_lagHigh),
			static_cast<double>(kTargetSampleRate) / static_cast<double>(_lagLow));
	}

	std::size_t PitchTracker::Track(std::span<const Sample> a_pcm, std::vector<float>& a_into)
	{
		if (_frame == 0 || a_pcm.size() < _frame) {
			return 0;  // engine/prosody.py:32-33
		}

		// Hoisted out of the frame loop: it depends on nothing but the window length,
		// and work that can be done once is done once.
		const double silent = static_cast<double>(_frame) * kQuantum * kQuantum;
		const double ratio = _settings.peakRatio;

		std::size_t made = 0;

		// DEFECT OF THE REFERENCE, FIXED. It stepped `range(0, len(pcm) - frame, hop)`
		// (engine/prosody.py:40), which leaves out the last frame that fits exactly -
		// and when a block is exactly one frame long it leaves out ALL of them, so a
		// 40 ms block was tracked as no frames at all. The end of a block is the part
		// a terminal fall is made of, so the loss fell precisely where it hurt.
		for (std::size_t at = 0; at + _frame <= a_pcm.size(); at += _hop) {
			// The mean is removed INTO THE SCRATCH, never into the caller's samples:
			// they belong to a turn and may already be inside a shared snapshot
			// (engine/prosody.py:42-43 wrote into a copy numpy had just made).
			double sum = 0.0;
			for (std::size_t i = 0; i < _frame; ++i) {
				sum += static_cast<double>(a_pcm[at + i]);
			}
			const double mean = sum / static_cast<double>(_frame);

			double energy = 0.0;
			for (std::size_t i = 0; i < _frame; ++i) {
				const double value = static_cast<double>(a_pcm[at + i]) - mean;
				_window[i] = value;
				energy += value * value;
			}

			if (energy <= silent) {
				a_into.push_back(0.0f);  // engine/prosody.py:45-47
				++made;
				continue;
			}

			// Only the lags of the band are correlated. The reference built the whole
			// correlation and then looked at 183 of its 640 lags
			// (engine/prosody.py:49-50); the rest of it was never read. The peak is
			// taken with a strict comparison, so the FIRST of equal peaks wins, which
			// is what np.argmax does - and the first is the higher pitch, which is the
			// fundamental rather than its octave below.
			double      best = std::numeric_limits<double>::lowest();
			std::size_t bestLag = _lagLow;
			for (std::size_t lag = _lagLow; lag < _lagHigh; ++lag) {
				double correlation = 0.0;
				for (std::size_t n = 0; n + lag < _frame; ++n) {
					correlation += _window[n] * _window[n + lag];
				}
				if (correlation > best) {
					best = correlation;
					bestLag = lag;
				}
			}

			// A weak peak is noise or a whisper: there is no tone there, and inventing
			// one is worse than admitting there is none. corr[0] is the energy
			// (engine/prosody.py:51-52).
			if (best < ratio * energy) {
				a_into.push_back(0.0f);
			} else {
				a_into.push_back(
					static_cast<float>(static_cast<double>(kTargetSampleRate) / static_cast<double>(bestLag)));
			}
			++made;
		}

		return made;
	}

	// ---------------------------------------------------------------- the range

	PitchRange::PitchRange(const ProsodySettings& a_settings) :
		_settings(a_settings)
	{
		// The histogram covers everything the tracker can actually produce, not
		// [minHz, maxHz] as written. The lowest lag is `rate / maxHz` TRUNCATED, so
		// the highest pitch that can come back is rate divided by that whole lag -
		// 355.6 Hz at the shipped 350, not 350. Binning to the written ceiling would
		// pile every high frame into the last bin and pull the upper percentile down.
		const auto lagLow = _settings.maxHz > 0.0 ?
		                        static_cast<std::size_t>(static_cast<double>(kTargetSampleRate) / _settings.maxHz) :
		                        std::size_t{ 0 };
		const auto reachable = lagLow > 0 ?
		                           static_cast<double>(kTargetSampleRate) / static_cast<double>(lagLow) :
		                           _settings.maxHz;
		const auto top = std::max(_settings.maxHz, reachable);

		if (_settings.minHz > 0.0 && top > _settings.minHz) {
			const auto base = static_cast<std::int64_t>(std::floor(_settings.minHz));
			const auto ceiling = static_cast<std::int64_t>(std::ceil(top));
			_bins.assign(static_cast<std::size_t>(ceiling - base) + 1, 0U);
		}
	}

	void PitchRange::Add(std::span<const float> a_f0) noexcept
	{
		if (_bins.empty()) {
			return;
		}

		const auto base = static_cast<double>(static_cast<std::int64_t>(std::floor(_settings.minHz)));

		for (const auto value : a_f0) {
			if (value <= 0.0f) {
				continue;  // the unvoiced frames, skipped as in engine/prosody.py:74
			}
			// Clamping is a net, not a policy: the histogram is sized above to hold
			// everything the tracker can hand back, so nothing should reach an edge.
			const auto offset = static_cast<double>(value) - base;
			const auto index = offset <= 0.0 ? std::size_t{ 0 } :
			                                   std::min(_bins.size() - 1, static_cast<std::size_t>(offset));
			++_bins[index];
			++_voiced;
		}
	}

	PitchSpan PitchRange::Resolve() const noexcept
	{
		PitchSpan out;
		out.voiced = _voiced;

		const auto needed = _settings.voicedNeeded > 0 ? static_cast<std::uint32_t>(_settings.voicedNeeded) : 1U;
		if (_bins.empty() || _voiced < needed) {
			// Not enough voice to call anything a range yet. Zeroes, and Known() will
			// say so - the reference left low and high at zero for the same reason
			// (engine/prosody.py:75-79).
			return out;
		}

		// The percentiles, computed HERE and not on every block. numpy places the
		// p-th percentile at rank p/100*(n-1) of the sorted values and interpolates
		// between its neighbours (engine/prosody.py:77-78); with one-hertz bins there
		// is nothing to interpolate inside a bin, so the rank is floored and the bin
		// holding it answers. The difference against the reference is under a hertz,
		// which is far finer than anything this number is used for.
		const auto rankOf = [count = static_cast<double>(_voiced)](int a_percent) {
			const auto percent = static_cast<double>(std::clamp(a_percent, 0, 100));
			return static_cast<std::uint64_t>(percent / 100.0 * (count - 1.0));
		};

		const auto lowRank = rankOf(_settings.lowPercentile);
		const auto highRank = rankOf(_settings.highPercentile);
		const auto base = static_cast<double>(static_cast<std::int64_t>(std::floor(_settings.minHz)));

		std::uint64_t seen = 0;
		bool          haveLow = false;
		bool          haveHigh = false;
		for (std::size_t i = 0; i < _bins.size(); ++i) {
			if (_bins[i] == 0U) {
				continue;
			}
			const auto next = seen + _bins[i];
			const auto hertz = static_cast<float>(base + static_cast<double>(i) + kBinCentre);
			if (!haveLow && lowRank < next) {
				out.low = hertz;
				haveLow = true;
			}
			if (!haveHigh && highRank < next) {
				out.high = hertz;
				haveHigh = true;
			}
			if (haveLow && haveHigh) {
				break;
			}
			seen = next;
		}

		return out;
	}

	void PitchRange::Reset() noexcept
	{
		// The counts go, the memory stays: a turn is restarted, never rebuilt.
		std::fill(_bins.begin(), _bins.end(), 0U);
		_voiced = 0;
	}

	// ------------------------------------------------------------- the fall

	std::optional<float> TerminalFall(PitchTracker& a_scratch, std::span<const Sample> a_tail,
		const PitchSpan& a_span)
	{
		const auto& settings = a_scratch.Settings();

		if (!a_span.Known(settings)) {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_PROSODY_NO_RANGE", a_span.voiced, settings.voicedNeeded);
			return std::nullopt;  // engine/prosody.py:93-94
		}

		// The floor under the piece of sound worth measuring at all. The reference
		// kept this test in its CALLER, as `if to - at > self.sample_rate // 10`
		// (engine/engine.py:150), where a second caller would simply have forgotten
		// it. It belongs to the measurement, so it lives here: the answer of a fall
		// measured over 30 ms is noise wearing the shape of a number.
		const auto minSpan = MsToSamples(settings.minSpanMs > 0 ? static_cast<std::uint64_t>(settings.minSpanMs) : 0ULL);
		if (a_tail.size() <= minSpan) {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_PROSODY_SHORT", SamplesToMs(a_tail.size()), settings.minSpanMs);
			return std::nullopt;
		}

		// The last tailMs of what was given, or all of it when it is shorter
		// (engine/prosody.py:96).
		const auto want = MsToSamples(settings.tailMs > 0 ? static_cast<std::uint64_t>(settings.tailMs) : 0ULL);
		const auto from = a_tail.size() > want ? a_tail.size() - static_cast<std::size_t>(want) : std::size_t{ 0 };
		const auto tail = a_tail.subspan(from);

		// This vector is the one allocation of the listening half that is not made in
		// a constructor, and it is allowed because this is not a per-block path: a
		// terminal fall is measured once per pass, on a worker, while the consumer
		// goes on cutting. It is sized up front so it does not grow while it fills.
		const auto step = MsToSamples(settings.hopMs > 0 ? static_cast<std::uint64_t>(settings.hopMs) : 1ULL);
		std::vector<float> f0;
		f0.reserve(tail.size() / std::max<std::size_t>(1U, static_cast<std::size_t>(step)) + 1U);
		a_scratch.Track(tail, f0);

		// Only the voiced frames count; the zeros are the silence between words
		// (engine/prosody.py:98).
		f0.erase(std::remove_if(f0.begin(), f0.end(), [](float a_value) { return a_value <= 0.0f; }), f0.end());

		const auto inTail = settings.voicedInTail > 0 ? static_cast<std::size_t>(settings.voicedInTail) : 1U;
		if (f0.size() < inTail) {
			// The third answer, and it is not a zero: a judge handed 0.0 for "I could
			// not tell" reads it as "they are going to carry on"
			// (engine/prosody.py:99-100).
			Loc::Debug("$SPEECHBROKERVOICE_LOG_PROSODY_NO_VOICE", f0.size(), settings.voicedInTail);
			return std::nullopt;
		}

		// The median of the last third of the tail: the very end of an utterance often
		// falls into creak, where autocorrelation lies (engine/prosody.py:102-105).
		// The reference sorted the slice whole; the median alone is wanted, so the
		// slice is only partitioned - same element, less work.
		const auto take = std::max(inTail, f0.size() / 3U);
		const auto begin = f0.end() - static_cast<std::ptrdiff_t>(take);
		const auto middle = begin + static_cast<std::ptrdiff_t>(take / 2);
		std::nth_element(begin, middle, f0.end());
		const auto value = *middle;

		// Where that sits in the speaker's own range, turned upside down: on the floor
		// is 1.0, still up is 0.0 (engine/prosody.py:107-108). Known() has already
		// promised high > low, so no guard against dividing by zero is needed here -
		// the reference's `max(1e-6, ...)` was guarding a case its own `known` had
		// ruled out - and the clamp catches a range so narrow the division runs away.
		const auto place = (value - a_span.low) / (a_span.high - a_span.low);
		const auto fall = std::clamp(1.0f - place, 0.0f, 1.0f);

		Loc::Debug("$SPEECHBROKERVOICE_LOG_PROSODY_FALL", fall, f0.size(), a_span.low, a_span.high);
		return fall;
	}
}
