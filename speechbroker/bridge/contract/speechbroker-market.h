/* Speech Broker - the market interface. The contract version is kInterfaceVersion
 * below, and it is counted apart from the adapter contract on purpose: the two
 * change for different reasons and a market must not be refused because an
 * adapter gained a field.
 *
 * A MARKET IS A MOD OF ITS OWN. It receives the finished packet of recognised
 * text and decides who gets it - and that is the whole of its business. The rule
 * by which it decides is its own: everyone who asked, only those whose vocabulary
 * matches, the winner of an auction, or anything a third party invents. Changing
 * the rule means installing a different market, not editing this one.
 *
 * WHY THE INTERFACE EXISTS AT ALL. It is a small safety device. The auction is
 * complicated machinery, and complicated machinery can turn out to work wrongly
 * and opaquely; a mod that cannot afford to depend on an argument it cannot watch
 * installs a plain market instead. The plain implementations are not lesser
 * auctions - they are the way out of one.
 *
 * WHAT A MARKET OWNS AND WHAT IT DOES NOT
 *
 *   owns   its subscribers, its events, its rule, and the LIFETIME of the packet:
 *          it may drop a packet after a while no matter who is asking, and a
 *          subscriber that comes late gets nothing.
 *
 *   knows  exactly one thing about the other markets: whether one of them has
 *          already handed this packet to somebody. HandOutState says so.
 *
 *   cannot do anything to another market. There is no call here that reaches a
 *          neighbour, and there will not be one: a market that could hold a
 *          packet open on a neighbour behalf would be arbitrating, and the part
 *          that arbitrates is the market itself, one level down.
 *
 * So two markets may hand the same utterance to two different mods. That is not
 * a hole: the guarantee "one mod acts on one utterance" belongs to the auction,
 * INSIDE the auction, and a player who installs a second market has asked for the
 * second opinion. A market that would rather not double up reads HandOutState and
 * decides for itself - which is a choice, never a promise the broker enforces.
 *
 * The handshake is the one the adapters already use: the bridge broadcasts an
 * SKSE message with a pointer to the interface, and the market catches it.
 */
#pragma once

#include <cstdint>

namespace SpeechBrokerMarketAPI
{
	constexpr std::uint32_t kInterfaceVersion = 1;

	// The type of the SKSE message the bridge hands the interface over with. The
	// sender is "SpeechBroker".
	constexpr std::uint32_t kMessageInterface = 'ENVM';

	struct MarketInfo
	{
		const char*   id{ nullptr };     // "auction", "open", "dictionary" - unique
		const char*   name{ nullptr };   // human readable, for the log
		std::uint32_t contract{ kInterfaceVersion };
	};

	// The packet as a market sees it. Every string lives ONLY for the length of the
	// call: a market that wants to keep something copies it, because the lifetime
	// is the business of the market and the bridge will not hold anything open on
	// its behalf.
	struct Packet
	{
		std::int32_t id{ 0 };
		const char*  text{ nullptr };
		float        score{ 0.0f };
		float        margin{ 0.0f };
		const char*  language{ nullptr };
		const char*  engine{ nullptr };
		const char*  channel{ nullptr };
		const char*  topic{ nullptr };
		bool         isFinal{ false };

		// The chance that the sentence ENDED here, from the recognition engine. A
		// market that acts on an unfinished phrase acts on half of one.
		float        complete{ 1.0f };

		// THE ONE FACT ABOUT THE NEIGHBOURS, as of the moment of this call: 1 - some
		// market has handed this packet out, 0 - nobody has, -1 - the bridge no
		// longer remembers this packet. -1 is NOT "nobody took it": it means the
		// question came too late, and a market that reads it as a free packet will
		// double up precisely on the old ones.
		std::int32_t handedOut{ 0 };

		// What the collectors of world state read AT THE MOMENT this was spoken.
		// Two arrays of the same length, keys sorted, keys shaped "<collector>.<key>".
		const char* const* stateKeys{ nullptr };
		const char* const* stateValues{ nullptr };
		std::int32_t       stateCount{ 0 };
	};

	// Called on the main thread of the game, once per market per packet. A market
	// that wants to think about it longer than a frame returns at once and thinks
	// on its own time - the bridge does not wait and does not ask again.
	using OfferCallback = void (*)(const Packet& a_packet, void* a_user);

	class ISpeechBrokerMarkets
	{
	public:
		virtual ~ISpeechBrokerMarkets() = default;

		virtual std::uint32_t Version() const = 0;

		// False when the id is empty, already taken, or the contract does not match.
		// A refused market is not a broken game: it simply never receives a packet,
		// and the bridge says by name in the log which one it refused and why.
		virtual bool Register(const MarketInfo& a_info, OfferCallback a_onOffer, void* a_user) = 0;
		virtual void Unregister(const char* a_id) = 0;

		// The market reports that it gave this packet to somebody. This is the only
		// thing a market ever tells the bridge about a packet, and the only thing
		// its neighbours can learn about it. Reporting is not asking permission:
		// nothing refuses, and the market has already acted by the time it calls.
		virtual void HandedOut(const char* a_marketId, std::int32_t a_id) = 0;

		// 1 - handed out, 0 - not yet, -1 - the bridge no longer remembers. Same
		// sentinel as in the packet, and the same trap: -1 is not a free packet.
		virtual std::int32_t HandOutState(std::int32_t a_id) const = 0;

		// The reading of the world for one key, copied into the buffer of the
		// caller. Handing back a pointer into the bridge is not allowed: the next
		// caller would overwrite it, and the asking comes from different threads.
		// False - no such reading, the packet is forgotten, or the buffer is small.
		virtual bool WorldState(std::int32_t a_id, const char* a_key, char* a_out,
			std::int32_t a_outSize) const = 0;
	};
}
