#include "state/GameModeCollector.h"

#include "core/GameState.h"
#include "core/Settings.h"

namespace SpeechBroker
{
	void GameModeCollector::Collect(std::map<std::string, std::string>& a_into) const
	{
		// THIS ORDER IS THE ORDER OF THE COLLECTOR, AND IT MAY DISAGREE WITH THE
		// TOPIC OF THE SAME UTTERANCE. Measured in tests/scenarios/world-state.json:
		// with the dialogue menu open AND the game paused, the reading is "paused"
		// while the topic is "dialogue", because the router walks topicOrder, which
		// belongs to the player. They answer different questions - the topic is
		// where the utterance should be offered, the reading is what the world was
		// like - and a mod that wants the routing should read the topic rather than
		// rebuild it out of this.
		//
		// The order matters, and it is the one a person would say out loud: paused
		// beats everything, because a menu that stops the world stops the dialogue
		// with it.
		if (GameState::Get().IsPaused()) {
			a_into["mode"] = "paused";
			return;
		}
		for (const auto& name : Settings::Get().dialogueMenuNames) {
			if (GameState::Get().IsMenuOpen(name)) {
				a_into["mode"] = "dialogue";
				return;
			}
		}
		a_into["mode"] = "game";
	}
}
