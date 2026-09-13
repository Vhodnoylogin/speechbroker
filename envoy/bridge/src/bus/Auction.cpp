#include "Auction.h"
#include "core/Loc.h"
#include "core/Log.h"

#include "TopicRouter.h"
#include "UtteranceStore.h"
#include "Hold.h"
#include "core/Events.h"
#include "core/MainThread.h"
#include "core/Scheduler.h"
#include "core/Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace Envoy
{
	Auction::Result Auction::Share(const std::vector<BidRecord>& a_survivors,
		const BidRecord& a_top) const
	{
		Result result;

		const char* stranger = a_top.phrase.empty()
		                           ? Loc::Get("$ENVOY_REASON_WINNER_UNKNOWN_COMMAND")
		                           : Loc::Get("$ENVOY_REASON_OTHER_COMMAND");

		for (const auto& bid : a_survivors) {
			const bool same = bid.ns == a_top.ns ||
			                  (!a_top.phrase.empty() && bid.phrase == a_top.phrase);
			if (!same) {
				result.denied[bid.ns] = stranger;
			} else if (bid.greedy) {
				result.denied[bid.ns] = Loc::Get("$ENVOY_REASON_LOST_AND_GREEDY");
			} else {
				result.winners.push_back(bid.ns);
			}
		}
		return result;
	}

	Auction::Result Auction::Exclusive(const std::string& a_ns, const std::string& a_reason,
		const std::vector<BidRecord>& a_scope) const
	{
		Result result;
		result.winners.push_back(a_ns);
		result.reason = a_reason;
		for (const auto& bid : a_scope) {
			if (bid.ns != a_ns) {
				result.denied[bid.ns] = a_reason;
			}
		}
		return result;
	}

	std::vector<BidRecord> Auction::Survivors(Result& a_result) const
	{
		std::vector<BidRecord> survivors;
		for (const auto& bid : _utterance.bids) {
			if (bid.confidence >= Settings::Get().MinConfidence(bid.costClass)) {
				survivors.push_back(bid);
			} else {
				a_result.denied[bid.ns] = Loc::Get("$ENVOY_REASON_BELOW_THRESHOLD");
			}
		}
		return survivors;
	}

	std::vector<BidRecord> Auction::Tied(const std::vector<BidRecord>& a_survivors, float a_need) const
	{
		std::vector<BidRecord> tied;
		for (const auto& bid : a_survivors) {
			if (a_survivors.front().confidence - bid.confidence < a_need) {
				tied.push_back(bid);
			}
		}
		return tied;
	}

	bool Auction::SameCommand(const std::vector<BidRecord>& a_tied) const
	{
		if (a_tied.front().phrase.empty()) {
			return false;
		}
		for (const auto& bid : a_tied) {
			if (bid.phrase != a_tied.front().phrase) {
				return false;
			}
		}
		return true;
	}

	Auction::Result Auction::BreakTie(const std::vector<BidRecord>& a_survivors, float a_need) const
	{
		Result     result;
		const auto tied = Tied(a_survivors, a_need);

		// The order from the settings is a direct instruction from the player, and it
		// outranks any reasoning of ours: if they named who is in charge here, there
		// is nothing to argue about.
		auto             best = Settings::kNoPriority;
		std::size_t      count = 0;
		const BidRecord* chosen = nullptr;
		for (const auto& bid : tied) {
			const auto place = Settings::Get().PriorityIndex(bid.ns);
			if (place < best) {
				best = place;
				count = 1;
				chosen = &bid;
			} else if (place == best) {
				++count;
			}
		}
		if (best != Settings::kNoPriority && count == 1) {
			return Exclusive(chosen->ns, Loc::Get("$ENVOY_REASON_PRIORITY"),
				a_survivors);
		}

		// The order says nothing. What happens next depends on whether the argument is
		// about one thing. Different commands at indistinguishable confidence mean an
		// ambiguous utterance: it can be understood two ways and both readings are
		// equally plausible. Doing anything at all about it amounts to guessing.
		if (!SameCommand(tied)) {
			result.reason = Loc::Get("$ENVOY_REASON_AMBIGUOUS");
			for (const auto& bid : a_survivors) {
				result.denied[bid.ns] = result.reason;
			}
			return result;
		}

		// There is no ambiguity: everybody recognised one and the same command, and
		// the argument is not about what was said but about whose command it is.
		// Confidence will never settle that, so it is settled by the declared
		// willingness to share. A greedy bidder loses nothing by this: it declared
		// "mine alone or not at all" itself and drops out by its own condition when
		// somebody else wins.
		if (Settings::Get().sharedWinsTie) {
			// The arguers were already checked for one command above, but the sharing
			// runs over all the survivors: one left behind by a whole margin may also
			// recognise the same phrase, and there is nothing to refuse it for. One that
			// recognised something else gets no prize even if it passed the threshold.
			auto shared = Share(a_survivors, tied.front());
			if (!shared.winners.empty()) {
				shared.reason = Loc::Get("$ENVOY_REASON_SHARED_TAKES_TIE");
				return shared;
			}
		}

		result.reason = Loc::Get("$ENVOY_REASON_ALL_GREEDY");
		for (const auto& bid : a_survivors) {
			result.denied[bid.ns] = result.reason;
		}
		return result;
	}

	Auction::Result Auction::Decide() const
	{
		Result result;

		if (_utterance.score < Settings::Get().minUtteranceScore) {
			result.reason = Loc::Get("$ENVOY_REASON_UTTERANCE_TOO_WEAK");
			for (const auto& bid : _utterance.bids) {
				result.denied[bid.ns] = result.reason;
			}
			return result;
		}

		// No bids at all and bids that failed are different things, and boiling them
		// down to one line in the log means lying in the diagnosis: in the run of
		// 04.09 that line was given to 34 utterances out of 36, and not one of them
		// had any bids.
		if (_utterance.bids.empty()) {
			result.reason = Loc::Get("$ENVOY_REASON_NO_BIDS");
			return result;
		}

		auto survivors = Survivors(result);
		if (survivors.empty()) {
			result.reason = Loc::Get("$ENVOY_REASON_NO_BID_PASSED");
			return result;
		}

		std::sort(survivors.begin(), survivors.end(), [](const BidRecord& a, const BidRecord& b) {
			if (a.confidence != b.confidence) {
				return a.confidence > b.confidence;
			}
			const auto pa = Settings::Get().PriorityIndex(a.ns);
			const auto pb = Settings::Get().PriorityIndex(b.ns);
			return pa != pb ? pa < pb : a.ns < b.ns;
		});

		const auto top = survivors.front();

		// From here only the survivors argue, and the verdict concerns only them.
		Result verdict;
		const auto need = Settings::Get().MinMargin(top.costClass);
		if (survivors.size() > 1 && top.confidence - survivors[1].confidence < need) {
			verdict = BreakTie(survivors, need);
		} else if (top.greedy) {
			verdict = Exclusive(top.ns, Loc::Get("$ENVOY_REASON_GREEDY_WON"), survivors);
		} else {
			verdict = Share(survivors, top);
			verdict.reason = verdict.winners.size() > 1
			                     ? Loc::Get("$ENVOY_REASON_SHARED_WON")
			                     : Loc::Get("$ENVOY_REASON_SHARED_ALONE");
		}

		// Those who dropped out on the threshold were never in the circle of the
		// argument and are not in the verdict: their reason was written down earlier
		// and is simply added. That makes the merge one for every outcome; there
		// used to be two of them with different meanings, and in the branch of the
		// order somebody who dropped out on the threshold got the wrong reason.
		verdict.denied.insert(result.denied.begin(), result.denied.end());
		return verdict;
	}

	Auctioneer& Auctioneer::Get()
	{
		static Auctioneer instance;
		return instance;
	}

	void Auctioneer::Receive(std::int32_t a_id)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored) {
			return;
		}

		// The topic is chosen BEFORE the decision to hold: the room depends on it,
		// and what is said in combat is heard by different people than what is said
		// out in the world.
		//
		// And it is chosen once. The topic belongs to the moment the phrase was said,
		// not to the moment it is announced: a held utterance is let go seconds
		// later, by which time the combat may have ended or a menu opened. Offer
		// used to choose the topic afresh, and in the live run of 07.09 a fireball
		// said in combat and held by the combat room went out as an
		// Envoy_Speech_World event - holding counted the risk by one room while the
		// announcement went to another.
		auto item = *stored;
		item.topic = TopicRouter::Pick(item);
		UtteranceStore::Get().Update(a_id, item);

		const auto verdict = Hold::Judge(item);
		if (!verdict.hold) {
			if (verdict.audience > 0) {
				Log::Info("$ENVOY_LOG_HANDED_AT_ONCE", a_id, verdict.reason);
			}
			Offer(a_id);
			return;
		}

		item.held = true;
		item.holdReason = verdict.reason;
		UtteranceStore::Get().Update(a_id, item);
		Log::Info("$ENVOY_LOG_HELD", a_id, verdict.reason);

		// The ceiling is not the main path but insurance. Usually the hold ends
		// earlier: either the continuation arrives and the fragment is thrown away
		// altogether, or the person falls silent and the engine sends a finished
		// utterance itself.
		if (verdict.ceilingMs > 0) {
			Scheduler::Get().After(std::chrono::milliseconds(verdict.ceilingMs), [a_id]() {
				MainThread::Post([a_id]() {
					Auctioneer::Get().Release(a_id, Loc::Get("$ENVOY_REASON_CEILING"));
				});
			});
		}
	}

	void Auctioneer::Release(std::int32_t a_id, const std::string& a_why)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored || !stored->held) {
			return;
		}
		if (stored->supersededBy != 0) {
			// The continuation got here in time: the fragment is no longer played out.
			return;
		}

		auto item = *stored;
		item.held = false;
		UtteranceStore::Get().Update(a_id, item);
		Log::Info("$ENVOY_LOG_LET_GO", a_id, a_why);
		Offer(a_id);
	}

	void Auctioneer::Supersede(std::int32_t a_newId, const std::vector<std::int32_t>& a_older)
	{
		for (const auto older : a_older) {
			auto stored = UtteranceStore::Get().Find(older);
			if (!stored || stored->supersededBy != 0) {
				continue;
			}

			auto item = *stored;
			item.supersededBy = a_newId;
			const bool wasHeld = item.held;
			item.held = false;
			UtteranceStore::Get().Update(older, item);

			if (wasHeld) {
				// Held, and rightly so: the phrase went on and the fragment never went
				// anywhere. This is the case the whole thing was built for.
				Log::Info("$ENVOY_LOG_DROPPED",
					older, a_newId);
				continue;
			}

			if (!item.winners.empty()) {
				// Handed over in time. The bridge cannot undo what was done - it does not
				// know what exactly the subscriber did - but it is obliged to say so. Only
				// the winner itself knows how to put things right.
				std::string who;
				for (const auto& winner : item.winners) {
					who += who.empty() ? winner : ", " + winner;
				}
				Log::Warn("$ENVOY_LOG_REVOKED",
					older, who, a_newId);
				Events::Send("Envoy_Revoked", "", static_cast<float>(older));
			}
		}
	}

	void Auctioneer::Offer(std::int32_t a_id)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored) {
			return;
		}

		// The topic was chosen when the utterance was taken in and is not revisited
		// here: see Receive.
		auto item = *stored;
		item.offeredAt = std::chrono::steady_clock::now();
		UtteranceStore::Get().Update(a_id, item);

		// The tidying happens right here: nothing happens in the store more often
		// than an utterance does, and a sweeper thread of its own would also have to
		// be stopped on exit.
		UtteranceStore::Get().PruneIfDue(Settings::Get().utteranceTtlSec,
			Settings::Get().utteranceMaxStored);

		// Observers see every utterance whatever the topic - that is exactly how a mod
		// can show that something did not reach it, and why.
		// An event is a doorbell: there is only the number of the utterance in it.
		// The subscriber learns the topic from the name of the event, and for
		// Envoy_Speech_Any by calling GetTopic. The text is always taken from the
		// bridge: the accurate model may refine it after the broadcast, and a copy
		// inside the event would part company with the truth.
		Events::Send("Envoy_Speech_Any", "", static_cast<float>(a_id));
		Events::Send(TopicRouter::EventName(item.topic), "", static_cast<float>(a_id));

		Scheduler::Get().After(std::chrono::milliseconds(Settings::Get().bidWindowMs), [a_id]() {
			MainThread::Post([a_id]() { Auctioneer::Get().Settle(a_id); });
		});
	}

	void Auctioneer::Settle(std::int32_t a_id)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored || stored->awarded) {
			return;
		}

		const auto result = Auction{ *stored }.Decide();

		std::string outcome;
		for (const auto& winner : result.winners) {
			outcome += outcome.empty() ? winner : ", " + winner;
		}
		if (outcome.empty()) {
			outcome = Loc::Get("$ENVOY_WORD_NOBODY");
		}

		UtteranceStore::Get().SetOutcome(a_id, result.winners, result.denied,
			outcome + " - " + result.reason);

		Log::Info("$ENVOY_LOG_SETTLED", a_id, stored->topic,
			stored->bids.size(), outcome, result.reason);

		// One broadcast per outcome rather than per recipient: there is no name in the
		// event any more, and each participant asks IsWinner or GetDenyReason for
		// itself. The three names are kept not for their content - it is the same
		// for everybody - but for their condition: Envoy_Award stays silent when
		// nobody won, Envoy_Denied when nobody was refused, and Envoy_Settled always
		// sounds.
		if (!result.winners.empty()) {
			Events::Send("Envoy_Award", "", static_cast<float>(a_id));
		}
		if (!result.denied.empty()) {
			Events::Send("Envoy_Denied", "", static_cast<float>(a_id));
		}
		Events::Send("Envoy_Settled", "", static_cast<float>(a_id));
	}
}
