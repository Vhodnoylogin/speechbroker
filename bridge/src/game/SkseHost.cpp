#include "SkseHost.h"

#include "ModEventBus.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace Envoy
{
	void SkseHost::Tasks::Post(MainThread::Task a_task)
	{
		if (auto* task = SKSE::GetTaskInterface()) {
			task->AddTask([work = std::move(a_task)]() { work(); });
			return;
		}

		// Интерфейса задач нет - делаем на месте. Хуже, чем в главном потоке,
		// но лучше, чем потерять работу молча.
		SKSE::log::warn("интерфейс задач SKSE недоступен, работа сделана на месте");
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
		// Живут до конца процесса: ядро держит на них указатели, а снимать швы
		// при выгрузке некому - плагин SKSE не выгружается.
		static Tasks   tasks;
		static Skyrim  state;
		static Papyrus events;

		MainThread::Install(&tasks);
		GameState::Install(&state);
		Events::Install(&events);

		SKSE::log::info("швы ядра подключены к игре: задачи, состояние, события");
	}
}
