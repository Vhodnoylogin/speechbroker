#include "SkseHost.h"
#include "core/Log.h"

#include "ModEventBus.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace SpeechBroker
{
	void SkseHost::Tasks::Post(MainThread::Task a_task)
	{
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([work = std::move(a_task)]() { work(); });
			return;
		}

		// No task interface, so we do it on the spot. Worse than the main thread, but
		// better than silently losing the work.
		Log::Warn("$SPEECHBROKER_LOG_NO_TASK_INTERFACE");
		a_task();
	}

	bool SkseHost::Skyrim::IsMenuOpen(const std::string& a_name) const
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->IsMenuOpen(a_name);
	}

	bool SkseHost::Skyrim::IsPaused() const
	{
		auto* ui = RE::UI::GetSingleton();
		return ui && ui->GameIsPaused();
	}

	bool SkseHost::Skyrim::IsInCombat() const
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player && player->IsInCombat();
	}

	void SkseHost::Papyrus::Send(const std::string& a_event, const std::string& a_string,
		float a_number)
	{
		ModEventBus::Send(a_event, a_string, a_number);
	}

	void SkseHost::Install()
	{
		// They live until the process ends: the core holds pointers to them, and there
		// is nobody to take the seams down at unload - an SKSE plugin is not
		// unloaded.
		static Tasks   tasks;
		static Skyrim  state;
		static Papyrus events;

		MainThread::Install(&tasks);
		GameState::Install(&state);
		Events::Install(&events);

		Log::Info("$SPEECHBROKER_LOG_SEAMS_WIRED");
	}
}
