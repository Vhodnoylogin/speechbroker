#pragma once

#include <RE/Skyrim.h>

namespace Envoy
{
	// Подписки и словари участников живут в памяти плагина и гибнут вместе
	// с процессом, а RegisterForModEvent, наоборот, переживает сохранение.
	// Значит после каждой загрузки игры мост обязан позвать всех перечислиться
	// заново - иначе он ничего о них не знает, хотя события им доходят.
	//
	// Сообщение SKSE kPostLoadGame для этого не годится: в VR оно не приходит
	// вовсе. В живом прогоне 03.09.2026 мост получил ровно четыре сообщения -
	// kPostLoad, kPostPostLoad, kInputLoaded, kDataLoaded, - хотя сохранение
	// загружалось. Поэтому слушаем событие самого движка: оно есть в любой
	// редакции игры и от набора сообщений SKSE не зависит.
	class GameLoadWatch : public RE::BSTEventSink<RE::TESLoadGameEvent>
	{
	public:
		static GameLoadWatch& Get();

		// Вызывать после kDataLoaded: раньше держателя событий не существует.
		static bool Install();

		RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent* a_event,
			RE::BSTEventSource<RE::TESLoadGameEvent>* a_source) override;

	private:
		GameLoadWatch() = default;
		GameLoadWatch(const GameLoadWatch&) = delete;
		GameLoadWatch& operator=(const GameLoadWatch&) = delete;
	};
}
