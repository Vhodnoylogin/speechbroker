#pragma once

#include "Utterance.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Envoy
{
	// One auction - one utterance.
	//
	// The object stores nothing and changes nothing: it only answers who gets the
	// lot and why. The argument is settled by declared quantities and not by the
	// order in which the engine happened to wake the scripts - otherwise one and
	// the same phrase would behave differently from launch to launch.
	//
	// This used to be a namespace pretending to be a class: four static methods,
	// with the whole substance in free functions of an anonymous namespace next
	// door. The data sat in the utterance and the behaviour apart from it.
	class Auction
	{
	public:
		struct Result
		{
			std::vector<std::string>           winners;  // empty - nobody acts
			std::map<std::string, std::string> denied;   // who was refused and why
			std::string                        reason;
		};

		explicit Auction(const Utterance& a_utterance) :
			_utterance(a_utterance)
		{}

		Result Decide() const;

	private:
		// Who passed the confidence threshold of their class. Those who did not get
		// their refusal written down straight away - it will be wanted whatever the
		// outcome, and it is more precise than any other: it names the reason the
		// participant dropped out BEFORE the argument began. So the survivors are
		// sorted out separately after this, and these refusals are simply added to
		// the verdict at the end.
		std::vector<BidRecord> Survivors(Result& a_result) const;

		// Not every survivor argues, only those the best one is not clear of by a
		// margin.
		std::vector<BidRecord> Tied(const std::vector<BidRecord>& a_survivors, float a_need) const;

		// Whether it is one command they recognised. An empty phrase means
		// "unknown", and then it is safer to assume they understood different
		// things: guessing costs more than keeping quiet.
		bool SameCommand(const std::vector<BidRecord>& a_tied) const;

		// A tie: confidence will not separate the arguers any further. The same
		// vocabulary entry always gives the same number, so waiting for somebody to
		// come out ahead next time is pointless - a rule is needed.
		//
		// The verdict goes to EVERY survivor and not only to the arguers: one left
		// behind by a margin also passed the threshold and is waiting for an answer.
		// Without an entry in the map of refusals its script cannot tell "I was
		// refused" from "the event never arrived".
		Result BreakTie(const std::vector<BidRecord>& a_survivors, float a_need) const;

		// The lot to one, and to everybody else IN THE CIRCLE OF THE ARGUMENT a
		// refusal with the same reason. The circle is passed in explicitly: whoever
		// dropped out on the threshold took no part in the argument, and their
		// reason is their own, written down earlier. This method used to refuse
		// every bid in a row and the callers overwrote what was too much - and in
		// one of the branches the overwriting was forgotten: those who dropped out
		// on the threshold were told "the argument was settled by the order".
		Result Exclusive(const std::string& a_ns, const std::string& a_reason,
			const std::vector<BidRecord>& a_scope) const;

		// Who shares the prize with the winner.
		//
		// The prize is given for a COMMAND, not for a place in the list of bids. One
		// that recognised ANOTHER phrase has nothing to do with this command, however
		// much confidence it gathered: sharing the prize with it would make the
		// bridge force it to do something other than what was asked. It gets a reason
		// of its own for the refusal, different from losing on confidence - otherwise
		// the log cannot tell "fell short" from "heard the wrong thing".
		Result Share(const std::vector<BidRecord>& a_survivors, const BidRecord& a_top) const;

		const Utterance& _utterance;
	};

	// The auctioneer lives all the time and touches the world: the store, the
	// events, the clock. That is what sets it apart from the auction, which only
	// works things out.
	class Auctioneer
	{
	public:
		static Auctioneer& Get();

		// Take an utterance in: decide whether to announce it now or hold it until
		// it becomes clear whether the phrase has ended. The single entry point for
		// a new utterance. From the main thread.
		void Receive(std::int32_t a_id);

		// Let a held one go. From the main thread.
		void Release(std::int32_t a_id, const std::string& a_why);

		// A long utterance absorbs the short ones it is made of. The held ones it
		// throws away, the ones already handed over it revokes. From the main thread.
		void Supersede(std::int32_t a_newId, const std::vector<std::int32_t>& a_older);

		// Announce an utterance to the subscribers and open the bid window. By this
		// point the topic has already been chosen on the way in. From the main thread.
		void Offer(std::int32_t a_id);

		// Settle once the window has run out. From the main thread.
		void Settle(std::int32_t a_id);

	private:
		Auctioneer() = default;
	};
}
