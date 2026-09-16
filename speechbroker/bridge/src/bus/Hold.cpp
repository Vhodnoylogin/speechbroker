#include "Hold.h"
#include "core/Loc.h"

#include "SubscriptionRegistry.h"
#include "Utterance.h"
#include "core/Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace SpeechBroker
{
	Hold::Decision Hold::Judge(const Utterance& a_utterance)
	{
		Decision decision;
		const auto& settings = Settings::Get();
		decision.ceilingMs = settings.HoldCeilingMs(a_utterance.lengthClass);

		// The room is those who WOULD act: the subscribers of this topic whose
		// vocabularies recognise the phrase. The bridge can answer that itself
		// without asking anybody: the vocabularies are its own, and so are the
		// thresholds of the classes.
		const auto heard = SubscriptionRegistry::Get().Audience(a_utterance.topic,
			a_utterance.text);

		for (const auto& who : heard) {
			// Whether it would reach the threshold of its class - by the same estimate
			// the test host bids with on its behalf.
			const float likely = who.match.Confidence(a_utterance.score);
			if (likely < settings.MinConfidence(who.costClass)) {
				continue;
			}
			++decision.audience;
			decision.damage += settings.HoldWeight(who.costClass, who.revocable);
		}

		if (decision.audience == 0) {
			decision.reason = Loc::Get("$SPEECHBROKER_REASON_ROOM_EMPTY");
			return decision;
		}

		const float unsure = 1.0f - std::clamp(a_utterance.complete, 0.0f, 1.0f);
		decision.risk = unsure * decision.damage;
		decision.hold = decision.risk > settings.holdTolerance;

		decision.reason = fmt::format(fmt::runtime(Loc::Get("$SPEECHBROKER_REASON_HOLD_WEIGHED")),
			a_utterance.complete, decision.audience, decision.damage, decision.risk,
			settings.holdTolerance);
		return decision;
	}
}
