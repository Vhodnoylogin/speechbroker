#pragma once

#include "Utterance.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Envoy
{
	// Utterances arrive from the thread of the server and are read by scripts from
	// the main thread, so everything is under a lock. Copies are handed outside:
	// holding a reference to a record that may be dropped on its deadline at any
	// moment is not allowed.
	class UtteranceStore
	{
	public:
		static UtteranceStore& Get();

		std::int32_t             Add(Utterance a_utterance);
		bool                     Update(std::int32_t a_id, const Utterance& a_utterance);
		// A refinement from the accurate model: the text and the scores are replaced,
		// the bids and the topic are kept - the utterance has already gone out and is
		// already played out, or is being played out.
		bool                     Refine(std::int32_t a_id, const Utterance& a_utterance);
		// An utterance is handed over as a pointer to an immutable snapshot rather
		// than by value. Every question about it - text, score, topic, match, outcome
		// - used to copy the whole thing along with the bids, the winners and the map
		// of refusals; and there are a dozen questions per utterance.
		std::shared_ptr<const Utterance> Find(std::int32_t a_id) const;

		bool AddBid(std::int32_t a_id, BidRecord a_bid);
		bool SetOutcome(std::int32_t a_id, std::vector<std::string> a_winners,
			std::map<std::string, std::string> a_denied, std::string a_outcome);

		void        Prune(double a_ttlSec, std::size_t a_maxStored);
		// Tidying on two cheap conditions instead of a sweep on every utterance:
		// either more than the limit has piled up, or a quarter of the keeping time
		// has passed since last time. Both checks take constant time.
		void        PruneIfDue(double a_ttlSec, std::size_t a_maxStored);
		std::size_t Count() const;

		bool  WonPrevious(const std::string& a_namespace) const;
		float SecondsSinceWin(const std::string& a_namespace) const;

	private:
		UtteranceStore() = default;

		// Copy on write: readers hold a snapshot and live with it for as long as they
		// like, while a writer makes a copy of its own and swaps the pointer. That is
		// one copy per write instead of one per question.
		template <class Fn>
		bool Mutate(std::int32_t a_id, Fn a_change)
		{
			auto it = _items.find(a_id);
			if (it == _items.end()) {
				return false;
			}
			auto copy = std::make_shared<Utterance>(*it->second);
			if (!a_change(*copy)) {
				return false;
			}
			it->second = std::move(copy);
			return true;
		}

		mutable std::mutex                                              _mutex;
		std::unordered_map<std::int32_t, std::shared_ptr<const Utterance>> _items;
		std::string                                    _lastAwardedTo;
		std::chrono::steady_clock::time_point          _lastAwardAt{};
		std::int32_t                                   _next{ 1 };
		// When the last tidy-up was - so as not to sweep on every utterance.
		std::chrono::steady_clock::time_point           _lastPrune{};
	};
}
