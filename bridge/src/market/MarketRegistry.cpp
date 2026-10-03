#include "market/MarketRegistry.h"

#include "bus/Utterance.h"
#include "bus/UtteranceStore.h"
#include "core/Log.h"

#include <algorithm>

namespace SpeechBroker
{
	MarketRegistry& MarketRegistry::Get()
	{
		static MarketRegistry instance;
		return instance;
	}

	bool MarketRegistry::Register(const SpeechBrokerMarketAPI::MarketInfo& a_info,
		SpeechBrokerMarketAPI::OfferCallback a_onOffer, void* a_user)
	{
		const std::string id = a_info.id ? a_info.id : "";
		if (id.empty() || !a_onOffer) {
			Log::Warn("$SPEECHBROKER_LOG_MARKET_REFUSED", id, "id");
			return false;
		}
		if (a_info.contract != SpeechBrokerMarketAPI::kInterfaceVersion) {
			// Refused by NAME and by reason. A market built against another version
			// of the contract would read the packet at the wrong offsets, and the
			// failure of that is a market quietly handing out nonsense.
			Log::Warn("$SPEECHBROKER_LOG_MARKET_REFUSED", id, "contract");
			return false;
		}

		std::scoped_lock lock(_mutex);
		const auto taken = std::any_of(_markets.begin(), _markets.end(),
			[&](const auto& a_other) { return a_other.id == id; });
		if (taken) {
			Log::Warn("$SPEECHBROKER_LOG_MARKET_REFUSED", id, "name");
			return false;
		}

		Entry entry;
		entry.id = id;
		entry.name = a_info.name ? a_info.name : id;
		entry.onOffer = a_onOffer;
		entry.user = a_user;
		_markets.push_back(std::move(entry));
		std::sort(_markets.begin(), _markets.end(),
			[](const auto& a_left, const auto& a_right) { return a_left.id < a_right.id; });

		Log::Info("$SPEECHBROKER_LOG_MARKET_REGISTERED", id, _markets.size());
		return true;
	}

	void MarketRegistry::Unregister(const std::string& a_id)
	{
		std::scoped_lock lock(_mutex);
		_markets.erase(std::remove_if(_markets.begin(), _markets.end(),
			[&](const auto& a_entry) { return a_entry.id == a_id; }), _markets.end());
	}

	std::size_t MarketRegistry::Count() const
	{
		std::scoped_lock lock(_mutex);
		return _markets.size();
	}

	std::int32_t MarketRegistry::HandOutState(std::int32_t a_id) const
	{
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return -1;
		}
		return item->handedOutBy.empty() ? 0 : 1;
	}

	void MarketRegistry::Offer(const Utterance& a_utterance)
	{
		std::vector<Entry> markets;
		{
			// Copied out and called outside the lock: a market is somebody else code
			// and may call back into the bridge from inside its own callback.
			std::scoped_lock lock(_mutex);
			markets = _markets;
		}
		if (markets.empty()) {
			return;
		}

		// The readings are flattened once for all the markets. The strings must
		// outlive every call, so they are kept here and nowhere else - a market that
		// wants to keep one copies it, because the lifetime of a packet is the
		// business of the market and the bridge holds nothing open for it.
		std::vector<std::string> keys;
		std::vector<std::string> values;
		keys.reserve(a_utterance.state.size());
		values.reserve(a_utterance.state.size());
		for (const auto& entry : a_utterance.state) {
			keys.push_back(entry.first);
			values.push_back(entry.second);
		}
		std::vector<const char*> keyPtrs;
		std::vector<const char*> valuePtrs;
		keyPtrs.reserve(keys.size());
		valuePtrs.reserve(values.size());
		for (std::size_t i = 0; i < keys.size(); ++i) {
			keyPtrs.push_back(keys[i].c_str());
			valuePtrs.push_back(values[i].c_str());
		}

		SpeechBrokerMarketAPI::Packet packet;
		packet.id = a_utterance.id;
		packet.text = a_utterance.text.c_str();
		packet.score = a_utterance.score;
		packet.margin = a_utterance.margin;
		packet.language = a_utterance.language.c_str();
		packet.engine = a_utterance.engine.c_str();
		packet.channel = a_utterance.channel.c_str();
		packet.topic = a_utterance.topic.c_str();
		packet.isFinal = a_utterance.isFinal;
		packet.complete = a_utterance.complete;
		// The SNAPSHOT, taken once: every market in this round sees the same answer,
		// so which of them happened to load first settles nothing.
		packet.handedOut = a_utterance.handedOutBy.empty() ? 0 : 1;
		packet.stateKeys = keyPtrs.empty() ? nullptr : keyPtrs.data();
		packet.stateValues = valuePtrs.empty() ? nullptr : valuePtrs.data();
		packet.stateCount = static_cast<std::int32_t>(keyPtrs.size());

		for (const auto& market : markets) {
			try {
				market.onOffer(packet, market.user);
			} catch (...) {
				// One market that throws loses this packet and nothing else. It is not
				// unregistered for it: a market that misbehaves once is still the one
				// the player installed, and taking it out from under them silently
				// would be the bridge deciding which markets deserve to exist.
				Log::Warn("$SPEECHBROKER_LOG_MARKET_THREW", market.id, a_utterance.id);
			}
		}
	}

	void MarketRegistry::Clear()
	{
		std::scoped_lock lock(_mutex);
		_markets.clear();
	}
}
