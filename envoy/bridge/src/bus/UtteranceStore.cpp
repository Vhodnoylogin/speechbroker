#include "UtteranceStore.h"

#include <algorithm>
#include <vector>

namespace Envoy
{
	UtteranceStore& UtteranceStore::Get()
	{
		static UtteranceStore instance;
		return instance;
	}

	std::int32_t UtteranceStore::Add(Utterance a_utterance)
	{
		std::scoped_lock lock(_mutex);

		// The number has to fit into a Papyrus integer without losing precision, so
		// we roll over to one rather than into negative values.
		if (_next >= 16'000'000) {
			_next = 1;
		}

		a_utterance.id = _next++;
		const auto id = a_utterance.id;
		_items.emplace(id, std::make_shared<const Utterance>(std::move(a_utterance)));
		return id;
	}

	bool UtteranceStore::Update(std::int32_t a_id, const Utterance& a_utterance)
	{
		std::scoped_lock lock(_mutex);
		return Mutate(a_id, [&](Utterance& item) {
			// Bids outlive an update: they belong to the utterance, not to the text. The
			// topic, on the other hand, is ASSIGNED here - the auctioneer writes it when
			// the utterance is taken in, and keeping the previous one instead is wrong:
			// there is no previous one, and the topic would stay empty forever. Refining
			// the text does not touch the topic at all, because that goes through Refine.
			auto bids = std::move(item.bids);
			item = a_utterance;
			item.id = a_id;
			item.bids = std::move(bids);
			return true;
		});
	}

	std::shared_ptr<const Utterance> UtteranceStore::Find(std::int32_t a_id) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _items.find(a_id);
		return it == _items.end() ? nullptr : it->second;
	}

	bool UtteranceStore::Refine(std::int32_t a_id, const Utterance& a_utterance)
	{
		std::scoped_lock lock(_mutex);
		return Mutate(a_id, [&](Utterance& item) {
			item.text = a_utterance.text;
			item.alternatives = a_utterance.alternatives;
			item.score = a_utterance.score;
			item.margin = a_utterance.margin;
			item.engine = a_utterance.engine;
			item.latencyMs = a_utterance.latencyMs;
			item.isFinal = a_utterance.isFinal;
			return true;
		});
	}

	bool UtteranceStore::AddBid(std::int32_t a_id, BidRecord a_bid)
	{
		std::scoped_lock lock(_mutex);
		return Mutate(a_id, [&](Utterance& item) {
			if (item.awarded) {
				return false;
			}
			auto same = std::find_if(item.bids.begin(), item.bids.end(),
				[&](const BidRecord& b) { return b.ns == a_bid.ns; });
			if (same != item.bids.end()) {
				*same = std::move(a_bid);
			} else {
				item.bids.push_back(std::move(a_bid));
			}
			return true;
		});
	}

	bool UtteranceStore::SetOutcome(std::int32_t a_id, std::vector<std::string> a_winners,
		std::map<std::string, std::string> a_denied, std::string a_outcome)
	{
		std::scoped_lock lock(_mutex);
		return Mutate(a_id, [&](Utterance& item) {
			item.awarded = true;
			item.winners = std::move(a_winners);
			item.denied = std::move(a_denied);
			item.outcome = std::move(a_outcome);
			if (!item.winners.empty()) {
				_lastAwardedTo = item.winners.front();
				_lastAwardAt = std::chrono::steady_clock::now();
			}
			return true;
		});
	}

	void UtteranceStore::PruneIfDue(double a_ttlSec, std::size_t a_maxStored)
	{
		{
			std::scoped_lock lock(_mutex);
			const auto now = std::chrono::steady_clock::now();
			const auto due = _lastPrune + std::chrono::duration<double>(a_ttlSec / 4.0);
			if (_items.size() <= a_maxStored && now < due) {
				return;
			}
			_lastPrune = now;
		}
		Prune(a_ttlSec, a_maxStored);
	}

	void UtteranceStore::Prune(double a_ttlSec, std::size_t a_maxStored)
	{
		std::scoped_lock lock(_mutex);
		const auto now = std::chrono::steady_clock::now();

		for (auto it = _items.begin(); it != _items.end();) {
			const std::chrono::duration<double> age = now - it->second->born;
			it = (age.count() > a_ttlSec) ? _items.erase(it) : std::next(it);
		}

		if (_items.size() <= a_maxStored) {
			return;
		}

		std::vector<std::pair<std::int32_t, std::chrono::steady_clock::time_point>> byAge;
		byAge.reserve(_items.size());
		for (const auto& [id, item] : _items) {
			byAge.emplace_back(id, item->born);
		}
		std::sort(byAge.begin(), byAge.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

		const auto excess = _items.size() - a_maxStored;
		for (std::size_t i = 0; i < excess; ++i) {
			_items.erase(byAge[i].first);
		}
	}

	std::size_t UtteranceStore::Count() const
	{
		std::scoped_lock lock(_mutex);
		return _items.size();
	}

	bool UtteranceStore::WonPrevious(const std::string& a_namespace) const
	{
		std::scoped_lock lock(_mutex);
		return _lastAwardedTo == a_namespace;
	}

	float UtteranceStore::SecondsSinceWin(const std::string& a_namespace) const
	{
		std::scoped_lock lock(_mutex);
		if (_lastAwardedTo != a_namespace) {
			return -1.0f;
		}
		const std::chrono::duration<float> age = std::chrono::steady_clock::now() - _lastAwardAt;
		return age.count();
	}
}
