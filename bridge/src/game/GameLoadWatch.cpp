#include "GameLoadWatch.h"

#include "core/Events.h"
#include "envoy-adapter.h"

#include <SKSE/SKSE.h>

namespace Envoy
{
	GameLoadWatch& GameLoadWatch::Get()
	{
		static GameLoadWatch instance;
		return instance;
	}

	bool GameLoadWatch::Install()
	{
		auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
		if (!holder) {
			SKSE::log::error("the holder of the game events is unavailable: "
			                 "there will be nobody to call the participants after a load");
			return false;
		}
		holder->AddEventSink<RE::TESLoadGameEvent>(std::addressof(Get()));
		SKSE::log::info("watching for a game load through the own event of the engine");
		return true;
	}

	RE::BSEventNotifyControl GameLoadWatch::ProcessEvent(const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		Events::Send("Envoy_Ready", "", static_cast<float>(EnvoyAPI::kInterfaceVersion));
		SKSE::log::info("game loaded: calling the participants to declare themselves again");
		return RE::BSEventNotifyControl::kContinue;
	}
}
