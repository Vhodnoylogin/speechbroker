#pragma once

#include <map>
#include <string>

namespace SpeechBroker
{
	// A reader of some part of the state of the world, whose reading travels with
	// the utterance.
	//
	// WHY THE READING RIDES IN THE PACKET. A subscriber has to decide by the state
	// the world was in AT THE MOMENT the line was spoken. By the time it acts, that
	// moment is gone: the menu has closed, the fight has ended, the player has moved.
	// Asking afterwards answers a different question, and answers it confidently.
	//
	// THE PART IS MULTI-INSTANCE AND NON-COMPETING, and that is what separates it
	// from a market. Every collector contributes; nobody is chosen and nothing is
	// awarded. A collector does not know what the others read and cannot stop them.
	// Whoever receives the packet decides whether any of it interests them and which
	// part, so a collector never needs to know who its readings are for.
	//
	// VALUES ARE TOKENS, NOT SENTENCES. A reading is compared by a mod, not shown to
	// a player: "dialogue" is a value in the protocol and stays "dialogue" in every
	// language. Anything a person actually reads goes through a localisation key at
	// the place it is displayed, and that place is not here.
	class StateCollector
	{
	public:
		virtual ~StateCollector() = default;

		// The name the readings are filed under. It is a namespace rather than a
		// label: the registry writes every key as "<name>.<key>", which is what
		// makes it impossible for one collector to overwrite the reading of another.
		[[nodiscard]] virtual std::string Name() const = 0;

		// Read the world as it is now. Called once per utterance, before anything is
		// announced to anybody. Keys are written WITHOUT the name in front - the
		// registry puts it there.
		//
		// Reading nothing is an ordinary answer: a collector with nothing to say
		// writes nothing and the packet simply has no key of its own. It must not
		// invent a value to fill the gap, because a mod cannot tell an invented
		// value from a read one.
		virtual void Collect(std::map<std::string, std::string>& a_into) const = 0;
	};
}
