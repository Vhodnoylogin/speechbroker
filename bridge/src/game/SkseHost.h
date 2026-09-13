#pragma once

#include "core/Events.h"
#include "core/GameState.h"
#include "core/MainThread.h"

namespace Envoy
{
	// The SKSE layer: the three answers of the game to the three questions of the
	// core.
	//
	// The core knows none of these types and is not meant to. It asks "put this
	// work into the main thread", "is a menu open", "send this event out" - and
	// here Skyrim and SKSE answer. Take this file away and the core keeps
	// working: without the game the questions get their default answers.
	class SkseHost
	{
	public:
		// Make the game the source of the answers. Called once, at load.
		static void Install();

	private:
		class Tasks final : public MainThread::Dispatcher
		{
		public:
			void Post(MainThread::Task a_task) override;
		};

		class Skyrim final : public GameState::Source
		{
		public:
			bool IsMenuOpen(const std::string& a_name) const override;
			bool IsPaused() const override;
			bool IsInCombat() const override;
		};

		class Papyrus final : public Events::Sink
		{
		public:
			void Send(const std::string& a_event, const std::string& a_string,
				float a_number) override;
		};
	};
}
