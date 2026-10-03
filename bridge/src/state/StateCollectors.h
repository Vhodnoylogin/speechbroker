#pragma once

#include "state/StateCollector.h"

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SpeechBroker
{
	// Every collector there is. Not a market: this hands out no utterance and picks
	// no winner, it only lets each collector add its own reading to the packet.
	//
	// Two collectors cannot tread on one another. Keys are written as "<name>.<key>",
	// and a second collector claiming a name already taken is refused, which is the
	// mechanical form of "non-competing": there is no rule for who wins a clash
	// because a clash cannot happen.
	class StateCollectors
	{
	public:
		static StateCollectors& Get();

		// False when the collector is null, unnamed, or its name is already taken.
		bool Add(std::shared_ptr<StateCollector> a_collector);

		[[nodiscard]] std::size_t Count() const;

		// Ask everybody and write the readings into the packet. A collector that
		// throws loses its own reading and nothing else: the packet is the utterance
		// of a player, and dropping it because some reader of the world misbehaved
		// would trade the thing we have for the thing we merely wanted.
		void CollectInto(std::map<std::string, std::string>& a_into) const;

		// For the checks. It removes the built-in collector too, which is the point:
		// a check that wants an empty world has to be able to have one.
		void Clear();

	private:
		StateCollectors();

		mutable std::mutex                           _mutex;
		std::vector<std::shared_ptr<StateCollector>> _collectors;
	};
}
