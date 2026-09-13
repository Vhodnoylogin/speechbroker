#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Envoy
{
	struct Alternative
	{
		std::string text;
		float       score{ 0.0f };
	};

	struct BidRecord
	{
		std::string  ns;
		float        confidence{ 0.0f };
		std::int32_t costClass{ 0 };   // 0 - reversible, 1 - expensive
		// Greed is a claim to exclusivity. It only fires if the one claiming it won;
		// when somebody else wins, a greedy bidder drops out of the share by its own
		// condition, "mine alone or not at all".
		bool         greedy{ false };
		// The vocabulary phrase the bidder recognised in the utterance. The bridge
		// works it out itself, because the vocabularies belong to it. It is needed
		// because a tie comes in two different kinds: two understood DIFFERENT things
		// with equal confidence - the utterance is ambiguous; two understood THE SAME
		// thing - an argument about whose command it is. They cannot be settled alike.
		std::string  phrase;
	};

	// An utterance lives in the game under a number of its own: an event brings
	// the subscriber only the number, and everything else it fetches by function.
	struct Utterance
	{
		std::int32_t             id{ 0 };
		bool                     isFinal{ false };
		std::string              text;
		std::vector<Alternative> alternatives;
		float                    score{ 0.0f };
		float                    margin{ 0.0f };
		std::string              language;
		std::string              engine;
		std::string              channel;
		std::int32_t             latencyMs{ 0 };
		std::int32_t             durationMs{ 0 };
		bool                     wakeWord{ false };

		// The chance that the sentence ENDED on this piece. It comes from the
		// recognition engine: that is the one thing that hears the pause, the
		// intonation and the tone this is judged by.
		//
		// The default of one is not optimism but compatibility: an adapter that knows
		// nothing about completeness must not be given a hold it never asked for -
		// for it, everything still arrives finished.
		float                    complete{ 1.0f };

		// The utterance is held: announcing it is put off until it is clear whether
		// the phrase has ended. A held utterance may never be announced at all - if
		// the continuation arrives before the ceiling runs out.
		bool                     held{ false };
		std::string              holdReason;

		std::string              topic;
		// Length is not a property of the text but a way of cutting: short pieces are
		// cut on a short pause, long ones are glued out of them on a long one.
		std::int32_t             lengthClass{ 0 };   // 0 short, 1 middle, 2 long
		// The number of the piece at the sound source; 0 - nothing to relate it to.
		std::int32_t             sliceId{ 0 };
		// The number of the long utterance that absorbed this one; 0 - not yet absorbed.
		std::int32_t             supersededBy{ 0 };
		std::vector<BidRecord>             bids;
		std::vector<std::string>           winners;
		// Who was refused and why. An ordered map on purpose: the traversal order of a
		// hash map depends on the order of insertion, and any rearrangement of the
		// code that changes no meaning would shuffle the refusal lines in the log and
		// in the report of the host - the safety net would fire falsely.
		std::map<std::string, std::string> denied;
		std::string                        outcome;  // the outcome in words, for an observer
		bool                               awarded{ false };

		std::chrono::steady_clock::time_point born{ std::chrono::steady_clock::now() };
		// When the utterance was announced to the subscribers. The mark is needed
		// after the settling as well: a late bid has to be able to say how late it
		// was. It used to live in a map of its own with a mutex of its own and a
		// sweep of its own - though it is an ordinary field of the utterance.
		std::chrono::steady_clock::time_point offeredAt{};

		// How many milliseconds have passed since the announcement; -1 - not announced
		// yet. Wanted so that the log shows whether Papyrus manages to answer inside
		// the bid window: in the first live run not one bid arrived, and there was
		// nothing to tell "the script kept quiet" from "the script was late" with.
		std::int64_t MsSinceOffer() const
		{
			if (offeredAt.time_since_epoch().count() == 0) {
				return -1;
			}
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - offeredAt).count();
		}
	};
}
