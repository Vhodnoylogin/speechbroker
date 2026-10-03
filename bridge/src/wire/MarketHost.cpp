#include "wire/MarketHost.h"

#include "bus/UtteranceStore.h"
#include "market/MarketRegistry.h"

#include <cstring>
#include <string>

namespace SpeechBroker
{
	MarketHost& MarketHost::Get()
	{
		static MarketHost instance;
		return instance;
	}

	std::uint32_t MarketHost::Version() const
	{
		return SpeechBrokerMarketAPI::kInterfaceVersion;
	}

	bool MarketHost::Register(const SpeechBrokerMarketAPI::MarketInfo& a_info,
		SpeechBrokerMarketAPI::OfferCallback a_onOffer, void* a_user)
	{
		return MarketRegistry::Get().Register(a_info, a_onOffer, a_user);
	}

	void MarketHost::Unregister(const char* a_id)
	{
		MarketRegistry::Get().Unregister(a_id ? a_id : "");
	}

	void MarketHost::HandedOut(const char* a_marketId, std::int32_t a_id)
	{
		if (!a_marketId || !*a_marketId) {
			return;
		}
		UtteranceStore::Get().MarkHandedOut(a_id, a_marketId);
	}

	std::int32_t MarketHost::HandOutState(std::int32_t a_id) const
	{
		return MarketRegistry::Get().HandOutState(a_id);
	}

	bool MarketHost::WorldState(std::int32_t a_id, const char* a_key, char* a_out,
		std::int32_t a_outSize) const
	{
		if (!a_key || !*a_key || !a_out || a_outSize <= 0) {
			return false;
		}
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return false;
		}
		const auto found = item->state.find(std::string{ a_key });
		if (found == item->state.end()) {
			return false;
		}
		// The whole reading or nothing. Half of "dialogue" is "dial", and a market
		// comparing that against a token would take a truncation for a difference.
		const auto& value = found->second;
		if (value.size() + 1 > static_cast<std::size_t>(a_outSize)) {
			return false;
		}
		std::memcpy(a_out, value.c_str(), value.size() + 1);
		return true;
	}
}
