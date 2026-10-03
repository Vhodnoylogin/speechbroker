#include "TopicRouter.h"

#include "Utterance.h"
#include "core/GameState.h"
#include "core/Settings.h"

namespace SpeechBroker
{
	namespace
	{
		bool DialogueOpen()
		{
			for (const auto& name : Settings::Get().dialogueMenuNames) {
				if (GameState::Get().IsMenuOpen(name)) {
					return true;
				}
			}
			return false;
		}
	}

	std::string TopicRouter::Pick(const Utterance& a_utterance)
	{
		for (const auto& name : Settings::Get().topicOrder) {
			if (name == "channel") {
				if (!a_utterance.channel.empty()) {
					return "channel:" + a_utterance.channel;
				}
			} else if (name == "dialogue") {
				if (DialogueOpen()) {
					return "dialogue";
				}
			} else if (name == "menu") {
				if (GameState::Get().IsPaused()) {
					return "menu";
				}
			} else if (name == "combat") {
				if (GameState::Get().IsInCombat()) {
					return "combat";
				}
			} else if (name == "world") {
				return "world";
			}
		}

		return "world";
	}

	std::string TopicRouter::EventName(const std::string& a_topic)
	{
		if (a_topic.rfind("channel:", 0) == 0) {
			return "SpeechBroker_Speech_Channel";
		}
		if (a_topic == "dialogue") {
			return "SpeechBroker_Speech_Dialogue";
		}
		if (a_topic == "menu") {
			return "SpeechBroker_Speech_Menu";
		}
		if (a_topic == "combat") {
			return "SpeechBroker_Speech_Combat";
		}
		return "SpeechBroker_Speech_World";
	}
}
