#include "GameLoadWatch.h"
#include "core/Log.h"

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
			Log::Error("$ENVOY_LOG_LOADWATCH_NO_HOLDER");
			return false;
		}
		holder->AddEventSink<RE::TESLoadGameEvent>(std::addressof(Get()));
		Log::Info("$ENVOY_LOG_LOADWATCH_ON");
		return true;
	}

	RE::BSEventNotifyControl GameLoadWatch::ProcessEvent(const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		Events::Send("Envoy_Ready", "", static_cast<float>(EnvoyAPI::kInterfaceVersion));
		Log::Info("$ENVOY_LOG_LOADWATCH_LOADED");
		return RE::BSEventNotifyControl::kContinue;
	}
}
