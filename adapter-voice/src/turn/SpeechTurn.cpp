#include "turn/SpeechTurn.h"
#include "Loc.h"

// Gate is here for its two static measurements only - Peak on the trimmed buffer
// has to be the very number the gate would have produced on it, which is why that
// arithmetic lives in one place and is not written a second time here.
#include "turn/Gate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Voice
{
	namespace
	{
		// A setting is a signed number a person may have typed; the clock is
		// unsigned. One place for the guard, so that the "ms to samples once, in a
		// constructor" rule is not spelled out five times with five different
		// treatments of a negative.
		SampleIndex MsSetting(int a_ms) noexcept
		{
			return a_ms > 0 ? MsToSamples(static_cast<std::uint64_t>(a_ms)) : 0ULL;
		}

		SampleIndex AbsGap(SampleIndex a_left, SampleIndex a_right) noexcept
		{
			return a_left > a_right ? a_left - a_right : a_right - a_left;
		}

		// engine/turn.py:92-95. The edges of a pause are not sharp, so two anchors
		// closer together than the slack are one anchor; without the thinning the
		// list fills with near-duplicates and the snap stops meaning anything.
		//
		// THE LIST IS IN MILLISECONDS - that is what a worker above compares model
		// times against, and it is what Anchors promises - but THE COMPARISON IS IN
		// SAMPLES, like every other comparison in this half. The stored millisecond
		// is converted the one direction that is allowed, ms to samples; the result
		// of SamplesToMs never reaches a comparison here.
		//
		// The capacity guard is what keeps the promise that nothing in the
		// per-block path allocates. With any non-zero anchorSlackMs it can never
		// fire - the reserve is computed from the ceiling and that slack - and a
		// player who sets the slack to zero loses a little snap accuracy at the end
		// of a very long turn rather than an allocation while somebody is speaking.
		bool PushAnchor(std::vector<std::uint32_t>& a_anchors, SampleIndex a_at, SampleIndex a_slack)
		{
			if (!a_anchors.empty() && AbsGap(MsToSamples(a_anchors.back()), a_at) <= a_slack) {
				return false;
			}
			if (a_anchors.size() >= a_anchors.capacity()) {
				return false;
			}
			a_anchors.push_back(SamplesToMs(a_at));
			return true;
		}
	}

	// engine/turn.py:124-129. Free and pure: the worker that has the words uses the
	// same two numbers the ears were built with instead of keeping a second copy of
	// them. Zero words is "short", exactly as the reference had it - a reading with
	// no words at all is not a long one.
	LengthClass Classify(std::uint32_t a_words, const SegmentSettings& a_settings) noexcept
	{
		const auto shortMax = static_cast<std::uint32_t>(std::max(0, a_settings.shortMaxWords));
		const auto middleMax = static_cast<std::uint32_t>(std::max(0, a_settings.middleMaxWords));
		if (a_words <= shortMax) {
			return LengthClass::Short;
		}
		if (a_words <= middleMax) {
			return LengthClass::Middle;
		}
		return LengthClass::Long;
	}

	SpeechTurn::SpeechTurn(const TurnSettings& a_settings, const CutRules& a_rules,
		const ProsodySettings& a_prosody) :
		_settings(a_settings),
		_rules(a_rules),
		_prosody(a_prosody),
		_anchorSlack(MsSetting(a_settings.anchorSlackMs)),
		_snapSlack(MsSetting(a_settings.snapSlackMs)),
		_resumeQuiet(MsSetting(a_settings.resumeQuietMs)),
		_pitch(a_prosody)
	{
		// EVERY ALLOCATION THIS CLASS EVER MAKES OUTSIDE A CUT IS MADE HERE. The
		// turn is restarted and never rebuilt, so the sound buffer is taken at the
		// ceiling once - 1.28 MB at the shipped 20 s - and never grows again while
		// the microphone is running.
		if (_rules.maxSamples > 0) {
			_pcm.reserve(static_cast<std::size_t>(_rules.maxSamples));
		} else {
			// The ceiling is also SpeechBrokerVoiceSession::maxRequestSamples, so a
			// zero here is a settings fault worth saying out loud once. The turn
			// still works; it simply never ends on length, and WouldOverflow says
			// so honestly rather than refusing every block.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_TURN_NO_CEILING");
		}

		// An anchor is at least one slack away from the one before it, so the
		// ceiling divided by the slack is the most a turn can hold. At the shipped
		// numbers that is 20 s / 60 ms - a third of a kilobyte, taken once.
		const SampleIndex spacing = std::max(_anchorSlack, MsToSamples(1));
		_anchors.reserve(static_cast<std::size_t>(_rules.maxSamples / spacing) + 2U);

		// One f0 per hop, and the longest block that can be offered is the whole
		// ceiling. Reserved so that Track never reallocates inside Feed.
		const SampleIndex hop = std::max(MsSetting(_prosody.hopMs), SampleIndex{ 1 });
		_f0.reserve(static_cast<std::size_t>(_rules.maxSamples / hop) + 2U);

		_anchors.push_back(0);  // engine/turn.py:66 - turn sample zero is always an anchor
	}

	void SpeechTurn::Restart(std::int64_t a_id)
	{
		_id = a_id;
		_open = true;
		_emitted = false;

		_pcm.clear();  // keeps the capacity: clear, never shrink_to_fit
		_samples = 0;
		_lost = 0;

		_anchors.clear();
		_anchors.push_back(0);
		_anchorsShared.reset();
		_anchorsDirty = true;

		_quiet = 0;
		_wasQuiet = true;  // engine/turn.py:69 - a turn begins in the quiet state

		_pitch.Reset();
		_f0.clear();

		// The last pass of the turn that just ended is released here and not in
		// Close: the header promises that, and it is what lets a turn end with a
		// repeat of its own last snapshot without the audio being freed under it.
		_lastAudio.reset();
		_lastAnchors.reset();
		_lastPitch = PitchSpan{};
		_lastTailSilenceMs = 0;
		_lastLost = 0;
	}

	void SpeechTurn::Close()
	{
		_open = false;
		_pcm.clear();
		_samples = 0;
	}

	void SpeechTurn::Feed(std::span<const Sample> a_block, bool a_loud, PitchTracker& a_pitch)
	{
		if (!_open) {
			return;
		}

		// THE CEILING IS NOT A SLIDING WINDOW. The caller asks WouldOverflow before
		// it feeds, so this clamp is unreachable in a correct consumer; it is here
		// because the alternative to clamping is a reallocation of the sound buffer
		// while somebody is speaking, and because a silent truncation would move
		// every later time in the turn.
		std::size_t take = a_block.size();
		if (_rules.maxSamples > 0 && _samples + take > _rules.maxSamples) {
			take = static_cast<std::size_t>(_rules.maxSamples - _samples);
			Loc::Warn("$SPEECHBROKERVOICE_LOG_TURN_OVERFULL", _id,
				static_cast<std::uint64_t>(a_block.size() - take), _rules.maxSamples);
		}

		// The edge of a pause is the START of the block, not its end
		// (engine/turn.py:81 and :88, `at - block_ms`, where `at` was read after the
		// block had already been appended). Taken before the buffer grows, which is
		// the same number without the subtraction - and without the reference's
		// assumption that every block is the same length.
		const SampleIndex start = _samples;

		if (take > 0) {
			_pcm.insert(_pcm.end(), a_block.begin(), a_block.begin() + static_cast<std::ptrdiff_t>(take));
			_samples += take;
		}

		// Every clock in the turn counts what actually went into the buffer, so the
		// silence run and the anchors cannot drift away from the sound they
		// describe even in the clamped case above.
		const auto kept = a_block.first(take);

		if (a_loud) {
			// engine/turn.py:80-81: speech coming back after a real pause is an
			// anchor; speech coming back after a single quiet block is the same run
			// of speech and is not.
			if (_wasQuiet && _quiet >= _resumeQuiet && PushAnchor(_anchors, start, _anchorSlack)) {
				_anchorsDirty = true;
			}
			_quiet = 0;
			_wasQuiet = false;

			// engine/turn.py:84-85 - the tone is measured on loud blocks only. Track
			// appends, so the scratch is cleared and not reallocated, and the
			// caller's tracker is borrowed rather than owned: a worker measuring a
			// terminal fall on a finished snapshot brings its own.
			if (!kept.empty()) {
				_f0.clear();
				a_pitch.Track(kept, _f0);
				_pitch.Add(_f0);
			}
		} else {
			// engine/turn.py:87-88 - speech stopping is an anchor at once, with no
			// debounce: the far side of the pause is what needs one.
			if (!_wasQuiet && PushAnchor(_anchors, start, _anchorSlack)) {
				_anchorsDirty = true;
			}
			_quiet += take;
			_wasQuiet = true;
		}
	}

	void SpeechTurn::NoteLost(std::size_t a_samples) noexcept
	{
		// The samples are already in the buffer as silence - the consumer filled the
		// hole before it resampled - and this is how many of them are not real. It
		// accumulates for the turn and rides on every pass of it, because a hole is
		// a property of the audio and every pass carries the whole turn.
		constexpr auto      kCeiling = (std::numeric_limits<std::uint32_t>::max)();
		const std::uint64_t grown = static_cast<std::uint64_t>(_lost) + a_samples;
		_lost = grown > kCeiling ? kCeiling : static_cast<std::uint32_t>(grown);
	}

	bool SpeechTurn::WouldOverflow(std::size_t a_count) const noexcept
	{
		// A zero ceiling was reported at construction; there is nothing to overflow.
		return _rules.maxSamples > 0 && _samples + a_count > _rules.maxSamples;
	}

	CutOutcome SpeechTurn::Cut(SampleIndex a_tailSilence, bool a_final, std::int32_t a_serial)
	{
		// A TURN THAT EMITTED ANYTHING MUST EMIT A FINAL PASS, whatever refused this
		// cut. One pointer copy of the last snapshot with a fresh serial: it is
		// immutable and shared, so nothing is copied and nothing can be seen to
		// change. Without it a turn could end with every pass interim, and the
		// dispatch half - which may not replace an entry whose final is set, and
		// which closes a turn on it - would wait for a pass that is never coming.
		const auto refuse = [&](CutVerdict a_why) -> CutOutcome {
			CutOutcome out;
			out.verdict = a_why;
			if (!a_final || !_emitted || !_lastAudio) {
				return out;
			}

			out.verdict = CutVerdict::Made;
			out.pass.audio = _lastAudio;
			out.pass.turnId = _id;
			out.pass.serial = a_serial;
			out.pass.final = true;
			out.pass.lostSamples = _lastLost;
			// The stored number and not a freshly measured one: the field says how
			// much silence was cut off THIS buffer, and this buffer is the one that
			// went out before. That the pause has since grown is said by final == 1.
			out.pass.tailSilenceMs = _lastTailSilenceMs;
			out.pass.anchors = _lastAnchors;
			out.pass.pitch = _lastPitch;
			Loc::Debug("$SPEECHBROKERVOICE_LOG_TURN_FINAL_REPEAT", _id, a_serial);
			return out;
		};

		// A closed turn is cut no further, not even by the rule above: the header
		// says nothing may be cut between Close and the next Restart, and the
		// consumer cuts BEFORE it closes (the loop order, step 8 then step 9). A
		// repeat handed out here would be a second final pass for a turn the half
		// above has already closed on the first one.
		if (!_open) {
			CutOutcome closed;
			closed.verdict = CutVerdict::Empty;
			return closed;
		}

		if (_samples == 0) {
			return refuse(CutVerdict::Empty);
		}

		// TRAILING SILENCE IS ALWAYS CUT, AND BY US. The adapter is the side that
		// heard the pause; a model must never be handed it - calibration caught
		// Whisper inventing a whole sentence over nothing at all, with its own
		// silence filter switched on (engine/engine.py:102-109).
		//
		// WHERE THIS DIFFERS FROM THE REFERENCE: python trimmed only when
		// `0 < drop < len(audio)` (engine/engine.py:107), so a pass whose silence
		// covered the whole turn was left UNTRIMMED and went on to be refused by the
		// length test a few lines later - the right outcome reached by the wrong
		// road, and only because that test existed. Here the trim is a clamp, so the
		// buffer can never contain more silence than it was told to cut, whatever
		// the floor is set to.
		const SampleIndex drop = std::min(a_tailSilence, _samples);
		const SampleIndex keep = _samples - drop;

		// ONE FLOOR, NOT TWO: _rules.minSamples was resolved once out of
		// TurnSettings::minPassMs and VadSettings::minUttSec, and neither of those
		// two is ever compared against here (engine/engine.py:110-111; contract,
		// SpeechBrokerVoiceRequest::sampleCount).
		if (keep < _rules.minSamples) {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_TURN_PASS_SHORT", _id, a_serial,
				SamplesToMs(keep), SamplesToMs(_rules.minSamples));
			return refuse(CutVerdict::TooShort);
		}

		// The one judgement about "was there speech at all" this half is allowed to
		// make, and it is a measurement rather than an opinion about content
		// (voice-service.py:296-298). Measured before the snapshot is taken, so a
		// refusal costs no allocation.
		const std::span<const Sample> trimmed{ _pcm.data(), static_cast<std::size_t>(keep) };
		const float                   peak = Gate::Peak(trimmed);
		if (peak < _rules.minPeak) {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_TURN_PASS_QUIET", _id, a_serial, peak, _rules.minPeak);
			return refuse(CutVerdict::TooQuiet);
		}

		// THE SNAPSHOT, AND THIS IS THE ONE PLACE IT IS MADE. A fresh, complete,
		// immutable copy of the trimmed turn, owned by whoever holds a pointer to it
		// and by nobody else. Never a span over _pcm: _pcm grows under the consumer
		// while a worker still has the pass, and a vector that grows moves.
		//
		// The reference took a fresh array per pass too (engine/turn.py:111-113),
		// but by accident of numpy rather than by rule; here the immutability is in
		// the type and cannot be lost by somebody writing one line differently.
		auto fresh = std::make_shared<std::vector<Sample>>(
			_pcm.begin(), _pcm.begin() + static_cast<std::ptrdiff_t>(keep));

		// The anchors as they stood AT THE CUT. Remade only when the list has moved
		// since the copy was taken: in an ordinary turn most passes share one, and
		// an allocation avoided in the cutting path is an allocation avoided while
		// somebody is speaking.
		if (_anchorsDirty || !_anchorsShared) {
			_anchorsShared = std::make_shared<const std::vector<std::uint32_t>>(_anchors);
			_anchorsDirty = false;
		}

		CutOutcome out;
		out.verdict = CutVerdict::Made;
		out.pass.audio = std::move(fresh);
		out.pass.turnId = _id;
		out.pass.serial = a_serial;
		out.pass.final = a_final;
		out.pass.lostSamples = _lost;
		out.pass.tailSilenceMs = SamplesToMs(drop);
		out.pass.anchors = _anchorsShared;
		// By value, resolved here rather than in the worker: the percentiles are
		// then computed once per pass instead of once per block, and the worker
		// never reaches into a turn that the consumer is still filling.
		out.pass.pitch = _pitch.Resolve();

		_lastAudio = out.pass.audio;
		_lastAnchors = out.pass.anchors;
		_lastPitch = out.pass.pitch;
		_lastTailSilenceMs = out.pass.tailSilenceMs;
		_lastLost = out.pass.lostSamples;
		_emitted = true;

		return out;
	}

	std::uint32_t SpeechTurn::Snap(std::uint32_t a_ms) const noexcept
	{
		// engine/turn.py:97-109. The model decides WHERE a boundary falls; the
		// adapter decides what numbers it is called by. Without this the same
		// "fireball" arrived as [0..842], [0..1305] and [0..580] and counted as
		// three different pieces every time.
		//
		// In samples, like every other comparison here: the milliseconds that come
		// in and the milliseconds that are stored are converted the allowed
		// direction, and the answer is given back in the unit it was asked in.
		const SampleIndex want = MsToSamples(a_ms);

		std::uint32_t best = a_ms;
		SampleIndex   bestGap = _snapSlack + 1;  // engine/turn.py:104 - `slack_ms + 1`

		for (const auto anchor : _anchors) {
			const SampleIndex gap = AbsGap(MsToSamples(anchor), want);
			if (gap < bestGap) {
				best = anchor;
				bestGap = gap;
			}
		}

		// The end of the turn is a candidate as well (engine/turn.py:105): the last
		// fragment of a reading ends where the sound does, and a model that says so
		// a few tens of milliseconds late must land on the same number twice.
		const SampleIndex endGap = AbsGap(_samples, want);
		if (endGap < bestGap) {
			best = ElapsedMs();
		}

		return best;
	}
}
