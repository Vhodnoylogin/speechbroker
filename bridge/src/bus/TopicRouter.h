#pragma once

#include <cstdint>
#include <string>

namespace Envoy
{
	struct Utterance;

	// The topic is chosen by the fact of the state of the game, not by the sense
	// of the phrase: an utterance made outside the dialogue window will never
	// reach the dialogue subscribers.
	class TopicRouter
	{
	public:
		// Call from the main thread only: it reads the state of the game.
		static std::string Pick(const Utterance& a_utterance);
		static std::string EventName(const std::string& a_topic);
	};
}
