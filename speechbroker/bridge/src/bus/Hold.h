#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace SpeechBroker
{
	struct Utterance;

	// Hold an utterance back or hand it over at once.
	//
	// The point of holding is that NOBODY starts acting until it is clear whether
	// the phrase has ended. So the decision is taken once for the utterance and
	// not separately for each subscriber: a scheme where the impatient get a
	// fragment and the demanding wait permits exactly what holding was set up to
	// prevent. Acting on "close the door" when what was said was "close the
	// door when you leave" stays a mistake no matter who it was that acted.
	//
	// The boundary is neither in the code nor in the settings. It is worked out
	// from who is in the room right now: how many subscribers would have heard
	// this phrase and of what kind they are. One revocable subscriber with a
	// reversible action and five with expensive irreversible ones are a different
	// cost of a mistake, and one threshold for both would mean that in one case
	// we stall for nothing and in the other we risk for nothing. What is
	// configured is the TOLERANCE - how much risk we agree to carry - and the
	// threshold follows by itself.
	class Hold
	{
	public:
		struct Decision
		{
			bool         hold{ false };
			float        damage{ 0.0f };   // cost of a mistake, summed over the whole room
			float        risk{ 0.0f };     // (1 - completeness) * damage
			std::size_t  audience{ 0 };    // how many subscribers recognised the phrase
			std::int32_t ceilingMs{ 0 };   // longer than this we never hold, whatever happens
			std::string  reason;
		};

		// Called from the main thread: it looks at the topic, and the topic looks at
		// the state of the game.
		static Decision Judge(const Utterance& a_utterance);
	};
}
