#pragma once

#include <RE/Skyrim.h>

namespace SpeechBroker
{
	// The subscriptions and vocabularies of the participants live in the memory of
	// the plugin and die with the process, whereas RegisterForModEvent, on the
	// contrary, outlives a save. So after every game load the bridge is obliged
	// to call everyone to list themselves again - otherwise it knows nothing
	// about them even though the events do reach them.
	//
	// The SKSE message kPostLoadGame is no good for this: in VR it does not arrive
	// at all. In the live run of 03.09.2026 the bridge got exactly four messages -
	// kPostLoad, kPostPostLoad, kInputLoaded, kDataLoaded - even though a save was
	// being loaded. So we listen to the own event of the engine: it exists in
	// every edition of the game and does not depend on which SKSE messages arrive.
	class GameLoadWatch : public RE::BSTEventSink<RE::TESLoadGameEvent>
	{
	public:
		static GameLoadWatch& Get();

		// Call after kDataLoaded: before that the holder of the events does not exist.
		static bool Install();

		RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent* a_event,
			RE::BSTEventSource<RE::TESLoadGameEvent>* a_source) override;

	private:
		GameLoadWatch() = default;
		GameLoadWatch(const GameLoadWatch&) = delete;
		GameLoadWatch& operator=(const GameLoadWatch&) = delete;
	};
}
