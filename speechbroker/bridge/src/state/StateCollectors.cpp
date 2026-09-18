#include "state/StateCollectors.h"

#include "core/Log.h"
#include "state/GameModeCollector.h"

#include <algorithm>

namespace SpeechBroker
{
	StateCollectors::StateCollectors()
	{
		// The built-in collector is installed here rather than by whoever starts the
		// broker, because a channel that works only when somebody remembered to wire
		// it up is a channel that will one day be reported as broken. A check that
		// wants an empty world calls Clear().
		_collectors.push_back(std::make_shared<GameModeCollector>());
	}

	StateCollectors& StateCollectors::Get()
	{
		static StateCollectors instance;
		return instance;
	}

	bool StateCollectors::Add(std::shared_ptr<StateCollector> a_collector)
	{
		if (!a_collector) {
			return false;
		}
		const auto name = a_collector->Name();
		if (name.empty()) {
			return false;
		}

		std::scoped_lock lock(_mutex);
		const auto taken = std::any_of(_collectors.begin(), _collectors.end(),
			[&](const auto& a_other) { return a_other->Name() == name; });
		if (taken) {
			return false;
		}
		_collectors.push_back(std::move(a_collector));
		return true;
	}

	std::size_t StateCollectors::Count() const
	{
		std::scoped_lock lock(_mutex);
		return _collectors.size();
	}

	void StateCollectors::CollectInto(std::map<std::string, std::string>& a_into) const
	{
		std::vector<std::shared_ptr<StateCollector>> collectors;
		{
			// Copied out under the lock and asked outside it: a collector reads the
			// world, the world is the game, and holding a lock across that would put
			// the mutex of the broker on the far side of somebody elses code.
			std::scoped_lock lock(_mutex);
			collectors = _collectors;
		}

		for (const auto& collector : collectors) {
			std::map<std::string, std::string> mine;
			try {
				collector->Collect(mine);
			} catch (...) {
				Log::Warn("$SPEECHBROKER_LOG_COLLECTOR_THREW", collector->Name());
				continue;
			}
			for (auto& entry : mine) {
				a_into[collector->Name() + "." + entry.first] = std::move(entry.second);
			}
		}
	}

	void StateCollectors::Clear()
	{
		std::scoped_lock lock(_mutex);
		_collectors.clear();
	}
}
