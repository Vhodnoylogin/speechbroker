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
			SKSE::log::error("держатель игровых событий недоступен: "
			                 "участников после загрузки будет некому позвать");
			return false;
		}
		holder->AddEventSink<RE::TESLoadGameEvent>(std::addressof(Get()));
		SKSE::log::info("слежу за загрузкой игры событием движка");
		return true;
	}

	RE::BSEventNotifyControl GameLoadWatch::ProcessEvent(const RE::TESLoadGameEvent*,
		RE::BSTEventSource<RE::TESLoadGameEvent>*)
	{
		Events::Send("Envoy_Ready", "", static_cast<float>(EnvoyAPI::kInterfaceVersion));
		SKSE::log::info("игра загружена: зову участников объявиться заново");
		return RE::BSEventNotifyControl::kContinue;
	}
}
