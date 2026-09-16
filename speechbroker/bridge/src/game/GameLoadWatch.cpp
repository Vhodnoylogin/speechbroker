#include "GameLoadWatch.h"
#include "core/Log.h"

#include "core/Events.h"
#include "speechbroker-adapter.h"

#include <SKSE/SKSE.h>

namespace SpeechBroker
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
			Log::Error("$SPEECHBROKER_LOG_LOADWATCH_NO_HOLDER");
			return false;
		}
		holder->AddEventSink<RE::TESLoadGameEvent>(std::addressof(Get()));
		Log::Info("$SPEECHBROKER_LOG_LOADWATCH_ON");
		return true;
	}

	RE::BSEventNotifyControl GameLoadWatch::ProcessEvent(const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		Events::Send("SpeechBroker_Ready", "", static_cast<float>(SpeechBrokerAPI::kInterfaceVersion));
		Log::Info("$SPEECHBROKER_LOG_LOADWATCH_LOADED");
		return RE::BSEventNotifyControl::kContinue;
	}
}
