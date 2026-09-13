#pragma once

#include "core/Events.h"
#include "core/GameState.h"
#include "core/MainThread.h"

namespace Envoy
{
	// Слой SKSE: три ответа игры на три вопроса ядра.
	//
	// Ядро не знает ни одного из этих типов и знать не должно. Оно спрашивает
	// «положи работу в главный поток», «открыто ли меню», «разошли событие» -
	// а здесь на эти вопросы отвечают Skyrim и SKSE. Уберите этот файл, и ядро
	// продолжит работать: без игры вопросы получают ответы по умолчанию.
	class SkseHost
	{
	public:
		// Поставить игру источником ответов. Зовётся один раз при загрузке.
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
