#pragma once

#include "speechbroker-market.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace SpeechBroker
{
	struct Utterance;

	// The markets that are installed, and the one place a packet is offered to them.
	//
	// It is deliberately thin. Everything a market does with a packet - who gets it,
	// how long it is on offer, what events it raises - happens on the far side of
	// the callback and the bridge never learns any of it. What the bridge keeps is
	// the packet itself and one fact: whether anybody has handed it out.
	//
	// THE ORDER OF OFFERING IS FIXED, sorted by id, and the handedOut field of the
	// packet is a SNAPSHOT taken before the first market is asked. Both are the same
	// rule: no argument is settled by the order in which mods happened to load. A
	// market that would rather have the live value calls HandOutState, and then the
	// order does matter - but it asked for that, and it asked knowingly.
	class MarketRegistry
	{
	public:
		static MarketRegistry& Get();

		bool Register(const SpeechBrokerMarketAPI::MarketInfo& a_info,
			SpeechBrokerMarketAPI::OfferCallback a_onOffer, void* a_user);
		void Unregister(const std::string& a_id);

		[[nodiscard]] std::size_t Count() const;

		// Offer one utterance to every market. Called on the main thread; the strings
		// in the packet live only for the duration of this call.
		void Offer(const Utterance& a_utterance);

		// 1 - some market handed this packet out, 0 - none yet, -1 - the bridge no
		// longer has the packet. The last is not "free": it means the question is
		// too late to answer.
		[[nodiscard]] std::int32_t HandOutState(std::int32_t a_id) const;

		// For the checks.
		void Clear();

	private:
		MarketRegistry() = default;

		struct Entry
		{
			std::string                             id;
			std::string                             name;
			SpeechBrokerMarketAPI::OfferCallback    onOffer{ nullptr };
			void*                                   user{ nullptr };
		};

		mutable std::mutex _mutex;
		std::vector<Entry> _markets;   // sorted by id, so the offering order is fixed
	};
}
