#pragma once

#include "state/StateCollector.h"

namespace SpeechBroker
{
	// The first collector, and deliberately a poor one: it reports whether the game
	// was running, in a dialogue or paused when the line was spoken.
	//
	// It is admitted to be pointless as information. A subscriber already knows that
	// much - the topic of the utterance carries it. What it is for is the CHANNEL:
	// a reading taken at the moment of the utterance, carried in the packet and read
	// by a mod is the whole path this part exists to provide, and here it can be
	// proved end to end without writing a model for it first.
	//
	// It reads through the GameState seam and the same setting TopicRouter uses for
	// the dialogue menus, so outside the game it answers "game", and a check can make
	// it answer anything by installing a source of its own.
	class GameModeCollector final : public StateCollector
	{
	public:
		// "core", because this is the reading of the broker itself rather than of
		// anybody subject matter. The key is therefore "core.mode".
		[[nodiscard]] std::string Name() const override { return "core"; }

		void Collect(std::map<std::string, std::string>& a_into) const override;
	};
}
