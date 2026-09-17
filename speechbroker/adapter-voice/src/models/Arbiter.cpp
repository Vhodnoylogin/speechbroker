// The argument between models, and the ledger of what has already left because
// of it.
//
// The two macros are set before anything else is included, for the reason
// audio/Capture.cpp gives: spdlog reaches for <Windows.h> on its own and
// whoever gets there first decides what the header defines. The fold below
// calls std::min and std::max in the same file as CharLowerBuffW, which is
// exactly the collision NOMINMAX exists for. They are guarded rather than
// defined outright, because an identical redefinition is silent while a
// different one is C4005, which /WX turns into a failure.
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif

#include "models/Arbiter.h"

#include "Loc.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

// For the key alone. Lower case is a property of a LANGUAGE and not of a byte:
// "Ä" is not "ä" by any arithmetic over bytes, and two models spelling the same
// word with different cases must fold into one hypothesis whatever the language
// of the game. The one table on this machine that knows that is the operating
// system's, so the key goes out to UTF-16, is lowered there and comes back.
#include <Windows.h>

namespace Voice::Models
{
	namespace
	{
		// ------------------------------------------------------------ the key

		// Python's str.lower() then a filter on isalnum/isspace
		// (engine/parts.py:22-24). The two differences from it are written down
		// rather than discovered later:
		//
		//   - the lowering is per UTF-16 CODE UNIT, so a letter outside the basic
		//     plane (which has no case anyway in any script a recogniser of speech
		//     will produce) is dropped rather than kept. The note of the design
		//     asks for exactly this spelling, and the alternative - a full Unicode
		//     table of our own - is a library we are not going to carry for a
		//     comparison of two transcriptions;
		//   - whitespace is KEPT AS IT CAME and not turned into a plain space,
		//     because that is what the reference did: it filtered characters, it
		//     did not rewrite them.
		bool KeepsIt(wchar_t a_unit) noexcept
		{
			return IsCharAlphaNumericW(a_unit) != FALSE || std::iswspace(a_unit) != 0;
		}

		std::wstring Lowered(const std::string& a_utf8)
		{
			if (a_utf8.empty() || a_utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
				return {};
			}

			const int bytes = static_cast<int>(a_utf8.size());
			const int units = MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(), bytes, nullptr, 0);
			if (units <= 0) {
				return {};  // not UTF-8 at all; a key we cannot build is an empty key
			}

			std::wstring wide(static_cast<std::size_t>(units), L'\0');
			if (MultiByteToWideChar(CP_UTF8, 0, a_utf8.c_str(), bytes, wide.data(), units) != units) {
				return {};
			}

			CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
			return wide;
		}

		std::string ToUtf8(const std::wstring& a_wide)
		{
			if (a_wide.empty() || a_wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
				return {};
			}

			const int units = static_cast<int>(a_wide.size());
			const int bytes = WideCharToMultiByte(CP_UTF8, 0, a_wide.c_str(), units,
				nullptr, 0, nullptr, nullptr);
			if (bytes <= 0) {
				return {};
			}

			std::string out(static_cast<std::size_t>(bytes), '\0');
			if (WideCharToMultiByte(CP_UTF8, 0, a_wide.c_str(), units, out.data(), bytes,
					nullptr, nullptr) != bytes) {
				return {};
			}
			return out;
		}

		// --------------------------------------------------------- the overlap

		// engine/arbiter.py:126-137. The fragment of a foreign reading that covers
		// the same stretch of time, or nothing at all.
		//
		// TWO NUMBERS AND THE STRICTER WINS, as the reference's
		// `max(slack_ms, target.duration_ms // 2)` did: a floor in milliseconds so
		// that a short stretch cannot be claimed by a passing touch, and a
		// fraction so that a long one cannot be claimed by a quarter of a second
		// at its edge.
		const Fragment* Overlapping(const std::vector<Fragment>& a_fragments,
			const Fragment& a_target, const ArbiterSettings& a_settings)
		{
			const Fragment* best = nullptr;
			std::int64_t    bestOverlap = 0;

			for (const auto& piece : a_fragments) {
				// Signed, deliberately: two stretches that do not touch produce a
				// NEGATIVE overlap, and the unsigned subtraction of the fields would
				// turn it into an enormous positive one and match everything.
				const std::int64_t overlap =
					static_cast<std::int64_t>(std::min(piece.endMs, a_target.endMs)) -
					static_cast<std::int64_t>(std::max(piece.startMs, a_target.startMs));
				if (overlap > bestOverlap) {
					best = &piece;
					bestOverlap = overlap;
				}
			}

			const std::int64_t byMs = static_cast<std::int64_t>(a_settings.overlapMs);
			const std::int64_t byShare = static_cast<std::int64_t>(a_target.DurationMs()) *
			                             static_cast<std::int64_t>(a_settings.overlapPercent) / 100;
			const std::int64_t need = std::max(byMs, byShare);

			return (best != nullptr && bestOverlap >= need) ? best : nullptr;
		}

		// ------------------------------------------------------------ the fold

		double Weighted(const Hypothesis& a_guess, Reputations& a_standings)
		{
			// The score TIMES the weight decides who wins; the score STORED is the
			// score as it was. Trust picks the winner of an argument, it does not
			// shrink the confidence the winner leaves with
			// (engine/arbiter.py:112-117).
			return static_cast<double>(a_guess.score) * a_standings.Of(a_guess.model).Weight();
		}

		std::vector<Hypothesis> Fold(std::vector<Hypothesis> a_bucket, Reputations& a_standings)
		{
			// The keys run parallel to the output rather than living in a map,
			// because the ORDER OF FIRST ARRIVAL has to survive the fold: the sort
			// below is stable, and a hash map would hand the equal-ranking
			// hypotheses to it in whatever order it liked - so two runs over the
			// same answers would produce two different lanes.
			std::vector<Hypothesis> out;
			std::vector<std::string> keys;
			out.reserve(a_bucket.size());
			keys.reserve(a_bucket.size());

			for (auto& guess : a_bucket) {
				std::string key = guess.Key();
				if (key.empty()) {
					continue;  // engine/arbiter.py:106-107 - nothing to compare by
				}

				const auto at = std::find(keys.begin(), keys.end(), key);
				if (at == keys.end()) {
					keys.push_back(std::move(key));
					out.push_back(std::move(guess));
					continue;
				}

				// Identical text from different models is not doubled, it is
				// STRENGTHENED (engine/arbiter.py:94-101).
				Hypothesis& seen = out[static_cast<std::size_t>(at - keys.begin())];
				++seen.agreed;

				if (Weighted(guess, a_standings) > Weighted(seen, a_standings)) {
					seen.text = std::move(guess.text);
					seen.score = guess.score;
					seen.voters.insert(seen.voters.begin(), guess.model);  // the winner first
					seen.model = std::move(guess.model);
				} else {
					seen.voters.push_back(std::move(guess.model));
				}
			}

			// AGREEMENT FIRST, THEN THE WEIGHTED SCORE: two models beat one
			// confident one, and between two lone voices the one we trust more
			// wins (engine/arbiter.py:119-122).
			//
			// The weights are taken once, before the sort, and not inside the
			// comparison: a comparator that takes a lock per call would take it
			// O(n log n) times, and - worse - a weight that changed while the sort
			// ran would make the comparison inconsistent and the sort undefined.
			std::vector<double> weighted(out.size(), 0.0);
			for (std::size_t i = 0; i < out.size(); ++i) {
				weighted[i] = Weighted(out[i], a_standings);
			}

			std::vector<std::size_t> order(out.size());
			std::iota(order.begin(), order.end(), std::size_t{ 0 });
			std::stable_sort(order.begin(), order.end(),
				[&](std::size_t a_left, std::size_t a_right) {
					if (out[a_left].agreed != out[a_right].agreed) {
						return out[a_left].agreed > out[a_right].agreed;
					}
					return weighted[a_left] > weighted[a_right];
				});

			std::vector<Hypothesis> sorted;
			sorted.reserve(out.size());
			for (const std::size_t index : order) {
				sorted.push_back(std::move(out[index]));
			}
			return sorted;
		}

		// ------------------------------------------------------- the reconciler

		// engine/turn.py:97-109, over the pass's OWN copy of the anchors. The
		// worker never calls into a live turn: by the time it runs, the turn
		// belongs to another thread and has moved on.
		//
		// The comparison is in milliseconds here and in samples inside SpeechTurn,
		// and the two agree exactly: MsToSamples is a multiplication by a
		// constant, so `gap <= slack` is the same question in either unit.
		std::uint32_t Snap(const Anchors& a_anchors, std::uint32_t a_elapsed, int a_slackMs,
			std::uint32_t a_ms) noexcept
		{
			const std::int64_t want = static_cast<std::int64_t>(a_ms);

			std::uint32_t best = a_ms;
			std::int64_t  bestGap = static_cast<std::int64_t>(a_slackMs) + 1;  // engine/turn.py:104

			if (a_anchors) {
				for (const std::uint32_t anchor : *a_anchors) {
					const std::int64_t gap = std::abs(static_cast<std::int64_t>(anchor) - want);
					if (gap < bestGap) {
						best = anchor;
						bestGap = gap;
					}
				}
			}

			// The end of the sound is a candidate too (engine/turn.py:105): the last
			// fragment of a reading ends where the speech does, and a model that
			// says so a few tens of milliseconds late must land on the same number
			// twice.
			const std::int64_t endGap = std::abs(static_cast<std::int64_t>(a_elapsed) - want);
			if (endGap < bestGap) {
				best = a_elapsed;
			}
			return best;
		}

		// engine/turn.py:177 - `len(hypotheses[0].text.split())`, used only when
		// the model did not say how many words it heard.
		//
		// ASCII whitespace, and that is the whole of it: a byte of a UTF-8
		// sequence is never below 0x80, so no letter of any language can be
		// mistaken for a separator here, and a script that separates its words by
		// something else would need a word counter, not a space table.
		std::uint32_t WordsIn(const std::string& a_text) noexcept
		{
			std::uint32_t words = 0;
			bool          inside = false;

			for (const char raw : a_text) {
				const auto unit = static_cast<unsigned char>(raw);
				const bool space = unit < 0x80U && std::isspace(static_cast<int>(unit)) != 0;
				if (space) {
					inside = false;
				} else if (!inside) {
					inside = true;
					++words;
				}
			}
			return words;
		}

		bool SameKey(const std::vector<Hypothesis>& a_old, const std::vector<Hypothesis>& a_fresh)
		{
			// engine/turn.py:205-208. Two empty readings are NOT the same reading:
			// there is nothing to compare, so there is no news to suppress.
			if (a_old.empty() || a_fresh.empty()) {
				return false;
			}
			return a_old.front().Key() == a_fresh.front().Key();
		}
	}

	// ----------------------------------------------------------------- pieces

	std::string Hypothesis::Key() const
	{
		const std::wstring wide = Lowered(text);
		if (wide.empty()) {
			return {};
		}

		std::wstring kept;
		kept.reserve(wide.size());
		for (const wchar_t unit : wide) {
			if (KeepsIt(unit)) {
				kept.push_back(unit);
			}
		}

		// .strip(): the edges only, never the middle. Two models differing by a
		// trailing space said the same thing; two models differing by a space in
		// the middle did not. The test is iswspace and not a table of six
		// characters, so that whatever KeepsIt let through as whitespace is
		// exactly what is trimmed here.
		std::size_t first = 0;
		while (first < kept.size() && std::iswspace(kept[first]) != 0) {
			++first;
		}
		if (first == kept.size()) {
			return {};
		}
		std::size_t last = kept.size();
		while (last > first && std::iswspace(kept[last - 1U]) != 0) {
			--last;
		}

		return ToUtf8(kept.substr(first, last - first));
	}

	const std::string& Slice::Text() const
	{
		// A slice with no hypotheses is never emitted, but Text() is also what a
		// log line reaches for, and a log line must not be the thing that crashes
		// the game.
		static const std::string kNothing;
		return hypotheses.empty() ? kNothing : hypotheses.front().text;
	}

	// ---------------------------------------------------------------- Arbiter

	Arbiter::Arbiter(ArbiterSettings a_settings, Reputations& a_standings) :
		_settings(std::move(a_settings)),
		_standings(&a_standings)
	{}

	const Reading* Arbiter::Base(const std::vector<Reading>& a_readings) const
	{
		// engine/arbiter.py:38-44, in the order the reference had it: the usable
		// readings, then those of them that carry word timings, then the most
		// fragments, then the FIRST among equals - so that two runs over the same
		// answers give the same lane.
		const Reading* best = nullptr;
		bool           bestTimed = false;

		for (const auto& reading : a_readings) {
			if (!reading.Usable()) {
				continue;
			}
			const bool timed = reading.CarriesWordTimings();

			if (best == nullptr) {
				best = &reading;
				bestTimed = timed;
				continue;
			}

			// A reading with word timings beats one without, whatever the counts:
			// `pool = timed or alive` is a choice of POOL and not a tie-break.
			if (timed != bestTimed) {
				if (timed) {
					best = &reading;
					bestTimed = true;
				}
				continue;
			}

			if (reading.fragments.size() > best->fragments.size()) {
				best = &reading;
			}
		}
		return best;
	}

	std::vector<Fragment> Arbiter::Spans(const std::vector<Reading>& a_readings) const
	{
		const Reading* base = Base(a_readings);
		return base != nullptr ? base->fragments : std::vector<Fragment>{};
	}

	Lane Arbiter::Merge(const std::vector<Reading>& a_readings) const
	{
		const Reading* base = Base(a_readings);
		if (base == nullptr) {
			return {};
		}

		Lane lane;
		lane.reserve(base->fragments.size());

		for (const auto& target : base->fragments) {
			std::vector<Hypothesis> bucket;
			bucket.reserve(a_readings.size());

			for (const auto& reading : a_readings) {
				if (!reading.Usable()) {
					continue;
				}
				const Fragment* match = Overlapping(reading.fragments, target, _settings);
				if (match == nullptr) {
					continue;  // it was talking about the neighbouring phrase
				}

				Hypothesis guess;
				guess.text = match->text;
				// The place in the model's OWN distribution, which is the only
				// number of two models that is comparable at all
				// (engine/arbiter.py:72-86).
				guess.score = _standings->Of(reading.modelId).Normalize(match->score);
				guess.model = reading.modelId;
				guess.agreed = 1;
				guess.voters.push_back(reading.modelId);
				bucket.push_back(std::move(guess));
			}

			lane.push_back(Fold(std::move(bucket), *_standings));
		}
		return lane;
	}

	void Arbiter::NoteAgreement(const Lane& a_lane) const
	{
		for (const auto& stretch : a_lane) {
			// WHO WAS IN THE ARGUMENT AT ALL. One model alone over a stretch
			// agreed with nobody and disagreed with nobody, and charging it a
			// disagreement would make "the only model installed" the worst-standing
			// model in the process.
			std::vector<std::string> everyone;
			for (const auto& guess : stretch) {
				for (const auto& voter : guess.voters) {
					if (std::find(everyone.begin(), everyone.end(), voter) == everyone.end()) {
						everyone.push_back(voter);
					}
				}
			}
			if (everyone.size() < 2U) {
				continue;
			}

			for (const auto& guess : stretch) {
				// Two or more models at one text is the agreement; everybody else
				// in the same stretch said something different, and that is the
				// disagreement. Both are facts about THIS stretch and neither is a
				// judgement of who was right - nobody here knows that.
				const bool together = guess.agreed >= 2;
				for (const auto& voter : guess.voters) {
					_standings->Of(voter).NoteAgreement(together);
				}
			}
		}
	}

	// -------------------------------------------------------------- TurnLedger

	TurnLedger::TurnLedger(std::int64_t a_turnId, TurnSettings a_turn, SegmentSettings a_segments,
		std::atomic<std::int32_t>& a_nextSliceId) :
		_turnId(a_turnId),
		_turn(a_turn),
		_segments(a_segments),
		_nextSliceId(a_nextSliceId)
	{}

	bool TurnLedger::Emitted() const
	{
		const std::lock_guard hold(_lock);
		return !_sent.empty();
	}

	std::vector<Slice> TurnLedger::Reconcile(const Collector& a_closed, const Arbiter& a_arbiter,
		const CompletenessJudge& a_judge, std::optional<float> a_terminalFall)
	{
		const std::lock_guard hold(_lock);

		std::vector<Slice> out;

		// THE ORDER GUARD, AND IT IS THE FIRST THING. A pass behind the last
		// reconciled one is abandoned WHOLE: its answers describe a buffer this
		// turn has already moved past, and emitting them would send the bridge a
		// correction pointing backwards in time.
		const std::int32_t serial = a_closed.Serial();
		if (serial <= _lastReconciled) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_LEDGER_BEHIND", _turnId, serial, _lastReconciled);
			return out;
		}

		const std::vector<Reading>& answers = a_closed.Answers();
		const std::vector<Fragment> spans = a_arbiter.Spans(answers);
		const Lane                  lane = a_arbiter.Merge(answers);

		// The standings learn from the fold once per pass, here, where the whole
		// lane is in hand - never per stretch inside Merge, which several workers
		// run at once over readings that may yet be thrown away by this guard.
		a_arbiter.NoteAgreement(lane);

		const Pass& subject = a_closed.Subject();

		// HOW FAR INTO THE TURN THIS PASS REACHES. The audio was trimmed of the
		// silence that fired the pass, so the sound and the silence together are
		// the clock the anchors are measured against and the moment a piece is
		// handed out (engine/turn.py:105, engine/parts.py:77-81).
		const std::uint32_t elapsed = subject.DurationMs() + subject.tailSilenceMs;

		const std::int64_t slack = static_cast<std::int64_t>(_turn.matchSlackMs);

		for (std::size_t i = 0; i < spans.size(); ++i) {
			// Merge returns one bucket per stretch of the same base, so the two
			// run together - and the guard is here anyway, because a lane shorter
			// than its spans must skip the stretch rather than index past the end.
			if (i >= lane.size() || lane[i].empty()) {
				continue;  // nothing was said about this stretch by anybody usable
			}
			const std::vector<Hypothesis>& hypotheses = lane[i];

			const Fragment& span = spans[i];

			const std::uint32_t start = Snap(subject.anchors, elapsed, _turn.snapSlackMs, span.startMs);
			const std::uint32_t end = Snap(subject.anchors, elapsed, _turn.snapSlackMs, span.endMs);
			if (end <= start) {
				continue;  // the snap folded it onto one anchor: it is not a piece
			}

			const bool hasAfter = i + 1U < spans.size();

			// The RAW start of the next stretch against the SNAPPED end of this
			// one, which is what the reference compared: the next stretch has not
			// been snapped yet when this one is judged. Both are unsigned and
			// fragments of one reading may overlap, so the subtraction is floored
			// at zero rather than allowed to wrap into an enormous pause.
			std::uint32_t silenceAfter = subject.tailSilenceMs;
			if (hasAfter) {
				silenceAfter = spans[i + 1U].startMs > end ? spans[i + 1U].startMs - end : 0U;
			}

			// The measured tone belongs to the END of the pass and to nothing
			// else: it was measured on the last stretch of the snapshot.
			const float complete = a_judge.Judge(span.signs, hasAfter, silenceAfter,
				hasAfter ? std::nullopt : a_terminalFall);

			// --- is this stretch one we have already sent? ----------------------
			Slice* same = nullptr;
			for (auto& sent : _sent) {
				const std::int64_t atStart =
					std::abs(static_cast<std::int64_t>(sent.startMs) - static_cast<std::int64_t>(start));
				const std::int64_t atEnd =
					std::abs(static_cast<std::int64_t>(sent.endMs) - static_cast<std::int64_t>(end));
				if (atStart <= slack && atEnd <= slack) {
					same = &sent;  // engine/turn.py:185-192, the first match wins
					break;
				}
			}

			const std::uint32_t words = span.signs.words > 0U ?
				span.signs.words :
				WordsIn(hypotheses.front().text);
			const LengthClass lengthClass = Classify(words, _segments);

			if (same != nullptr) {
				if (SameKey(same->hypotheses, hypotheses)) {
					continue;  // no news, and no news is not a message
				}

				// A BETTER READING OF A PIECE ALREADY OUT. Its id does not change -
				// the bridge knows the piece by it - and refines says which piece
				// the new hypotheses belong to.
				//
				// The last three fields are filled although the design note names
				// only the first six, because the publisher reads them on EVERY
				// slice it hands the bridge: a refinement that left lengthClass at
				// its default would tell the bridge a long phrase had become a
				// short one, and an emittedMs of zero would say it was handed out
				// before the turn began.
				Slice refined;
				refined.id = same->id;
				refined.refines = same->id;
				refined.startMs = start;
				refined.endMs = end;
				refined.hypotheses = hypotheses;
				refined.complete = complete;
				refined.speechElapsedMs = end;
				refined.lengthClass = lengthClass;
				refined.emittedMs = elapsed;

				// The ledger remembers the new hypotheses and NOT the new edges:
				// the piece keeps the place it was first recognised at, or a
				// sequence of small corrections would walk it across the turn a
				// slack at a time.
				same->hypotheses = hypotheses;

				Loc::Debug("$SPEECHBROKERVOICE_LOG_LEDGER_REFINED", _turnId, serial, refined.id,
					refined.Text());
				out.push_back(std::move(refined));
				continue;
			}

			// --- a new piece, and what it swallowed -----------------------------
			//
			// engine/turn.py:194-202: the pieces lying INSIDE this one and
			// shorter than it. The ledger says only what it absorbed; whether an
			// absorbed piece is spent is the bridge's to decide, because the state
			// of the auction is there and not here.
			Slice piece;
			const std::int64_t duration = static_cast<std::int64_t>(end) - static_cast<std::int64_t>(start);
			for (const auto& sent : _sent) {
				const std::int64_t from = static_cast<std::int64_t>(start) - slack;
				const std::int64_t to = static_cast<std::int64_t>(end) + slack;
				if (static_cast<std::int64_t>(sent.startMs) >= from &&
					static_cast<std::int64_t>(sent.endMs) <= to &&
					static_cast<std::int64_t>(sent.DurationMs()) < duration - slack) {
					piece.supersedes.push_back(sent.id);
				}
			}

			// The counter is the SESSION's, not this turn's, and it is taken
			// exactly once per piece.
			piece.id = _nextSliceId.fetch_add(1, std::memory_order_relaxed);
			piece.startMs = start;
			piece.endMs = end;
			piece.hypotheses = hypotheses;
			piece.complete = complete;
			piece.speechElapsedMs = end;
			piece.lengthClass = lengthClass;
			piece.emittedMs = elapsed;

			Loc::Debug("$SPEECHBROKERVOICE_LOG_LEDGER_SLICE", _turnId, serial, piece.id,
				piece.startMs, piece.endMs, piece.complete, piece.supersedes.size(), piece.Text());

			_sent.push_back(piece);
			out.push_back(std::move(piece));
		}

		// LAST, AND ONLY ON THE PATH THAT ACTUALLY RECONCILED. The guard above is
		// the only other writer of this number in the process.
		_lastReconciled = serial;
		return out;
	}
}
