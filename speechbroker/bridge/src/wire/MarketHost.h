#pragma once

#include "speechbroker-market.h"

namespace SpeechBroker
{
	// The market contract as the mods see it. It holds nothing of its own: the
	// register of markets and the store of packets already exist, and this is the
	// face they show across the ABI - C strings in, buffers out, no exceptions
	// crossing the line.
	//
	// It lives beside AdapterHost for the same reason AdapterHost exists: the shape
	// a stranger calls through is not the shape the core is written in, and mixing
	// the two makes both worse.
	class MarketHost final : public SpeechBrokerMarketAPI::ISpeechBrokerMarkets
	{
	public:
		static MarketHost& Get();

		std::uint32_t Version() const override;

		bool Register(const SpeechBrokerMarketAPI::MarketInfo& a_info,
			SpeechBrokerMarketAPI::OfferCallback a_onOffer, void* a_user) override;
		void Unregister(const char* a_id) override;

		void         HandedOut(const char* a_marketId, std::int32_t a_id) override;
		std::int32_t HandOutState(std::int32_t a_id) const override;

		bool WorldState(std::int32_t a_id, const char* a_key, char* a_out,
			std::int32_t a_outSize) const override;

	private:
		MarketHost() = default;
	};
}
