#include "Hold.h"

#include "SubscriptionRegistry.h"
#include "Utterance.h"
#include "core/Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace Envoy
{
	Hold::Decision Hold::Judge(const Utterance& a_utterance)
	{
		Decision decision;
		const auto& settings = Settings::Get();
		decision.ceilingMs = settings.HoldCeilingMs(a_utterance.lengthClass);

		// Зал - те, кто СТАЛ БЫ действовать: подписчики этой темы, чьи словари
		// узнают фразу. Мост может ответить на это сам, никого не спрашивая:
		// словари принадлежат ему, пороги классов - тоже.
		const auto heard = SubscriptionRegistry::Get().Audience(a_utterance.topic,
			a_utterance.text);

		for (const auto& who : heard) {
			// Дошёл бы он до порога своего класса - по той же оценке, по которой
			// за него ставит хост проверки.
			const float likely = who.match.Confidence(a_utterance.score);
			if (likely < settings.MinConfidence(who.costClass)) {
				continue;
			}
			++decision.audience;
			decision.damage += settings.HoldWeight(who.costClass, who.revocable);
		}

		if (decision.audience == 0) {
			decision.reason = "зал пуст - придерживать не для кого";
			return decision;
		}

		const float unsure = 1.0f - std::clamp(a_utterance.complete, 0.0f, 1.0f);
		decision.risk = unsure * decision.damage;
		decision.hold = decision.risk > settings.holdTolerance;

		decision.reason = fmt::format(
			"завершённость {:.2f}, в зале {}, цена ошибки {:.2f}, риск {:.2f} против допуска {:.2f}",
			a_utterance.complete, decision.audience, decision.damage, decision.risk,
			settings.holdTolerance);
		return decision;
	}
}
