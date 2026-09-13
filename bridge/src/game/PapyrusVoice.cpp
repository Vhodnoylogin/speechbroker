#include "PapyrusApi.h"

#include "core/Events.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/MainThread.h"
#include "core/Scheduler.h"
#include "wire/AdapterHost.h"

#include <SKSE/SKSE.h>

#include <atomic>
#include <chrono>

// Голос наружу, выбор адаптера и самопроверка рассылки: всё, что уходит из
// игры к моделям, и круг, доказывающий, что события доходят обратно.
namespace Envoy
{
	namespace
	{
		std::atomic_int32_t                                g_pingToken{ 0 };
		std::atomic<std::chrono::steady_clock::time_point> g_pingSentAt{};
		std::atomic_bool                                   g_pongHeard{ false };
	}

	std::vector<RE::BSFixedString> PapyrusApi::GetAdapters(Tag)
	{
		std::vector<RE::BSFixedString> out;
		for (const auto& id : AdapterHost::Get().AdapterIds()) {
			out.emplace_back(id);
		}
		return out;
	}

	RE::BSFixedString PapyrusApi::GetSource(Tag, Str a_capability)
	{
		return RE::BSFixedString{ AdapterHost::Get().Source(a_capability.c_str()) };
	}

	bool PapyrusApi::SetSource(Tag, Str a_capability, Str a_adapter)
	{
		return AdapterHost::Get().SetSource(a_capability.c_str(), a_adapter.c_str());
	}

	void PapyrusApi::ReloadSettings(Tag)
	{
		AdapterHost::Get().ReloadConfig();
		// Журнал слушается настроек сразу. Прежде перечитанный файл менял
		// пороги аукциона, но не уровень журнала, и наладчик видел прежние
		// строки, будучи уверен, что подкрутил ручку.
		const auto& settings = Settings::Get();
		Log::SetLevel(settings.logLevel);
		Log::SetShowSpeech(settings.logSpeechText);
		SKSE::log::info("настройки перечитаны, журнал: уровень {}, слова игрока {}",
			Log::Level(), settings.logSpeechText ? "пишем" : "не пишем");
	}

	std::int32_t PapyrusApi::Say(Tag, Str a_text, Str a_voice, std::int32_t a_priority)
	{
		return AdapterHost::Get().SendSpeak(a_text.c_str(), a_voice.c_str(), a_priority);
	}

	void PapyrusApi::StopSpeech(Tag, std::int32_t a_speechId)
	{
		AdapterHost::Get().SendStop(a_speechId);
	}

	std::int32_t PapyrusApi::Ask(Tag, Str a_service, Str a_payload)
	{
		return AdapterHost::Get().SendAsk(a_service.c_str(), a_payload.c_str());
	}

	RE::BSFixedString PapyrusApi::GetAnswer(Tag, std::int32_t a_requestId)
	{
		return RE::BSFixedString{ AdapterHost::Get().Answer(a_requestId) };
	}

	RE::BSFixedString PapyrusApi::GetSpeechResult(Tag, std::int32_t a_speechId)
	{
		return RE::BSFixedString{ AdapterHost::Get().SpeechResult(a_speechId) };
	}

	// Круг замыкается так: мост шлёт событие, скрипт квеста-носителя его ловит
	// и зовёт Pong. Ответ доказывает, что события доходят до Papyrus; молчание
	// доказывает обратное. Ни то, ни другое иначе из журнала моста не видно.
	void PapyrusApi::SelfTest(Tag)
	{
		const auto token = ++g_pingToken;
		g_pongHeard.store(false);
		SKSE::log::info("самопроверка рассылки: метка {}, звонок через 2 с", token);

		// Звонок отложен нарочно. Скрипт подписывается на Envoy_Ping в той же
		// строке, где просит самопроверку, и мгновенная рассылка обогнала бы
		// его подписку - получилось бы ложное "не доходит". Две секунды с запасом.
		Scheduler::Get().After(std::chrono::seconds(2), [token]() {
			MainThread::Post([token]() {
				g_pingSentAt.store(std::chrono::steady_clock::now());
				SKSE::log::info("самопроверка рассылки: посылаю Envoy_Ping, метка {}", token);
				Events::Send("Envoy_Ping", "", static_cast<float>(token));
			});
		});

		// Приговор выносится отдельным сроком, а не сном в том же потоке:
		// планировщик один на всех, и занимать его ожиданием нельзя.
		Scheduler::Get().After(std::chrono::seconds(7), [token]() {
			if (!g_pongHeard.load()) {
				SKSE::log::error("самопроверка рассылки: ответа на метку {} нет за 5 с - "
				                 "события моста до скриптов Papyrus НЕ ДОХОДЯТ", token);
			}
		});
	}

	void PapyrusApi::Pong(Tag, std::int32_t a_token)
	{
		g_pongHeard.store(true);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - g_pingSentAt.load()).count();
		SKSE::log::info("самопроверка рассылки: ответ на метку {} пришёл через {} мс - "
		                "события моста до скриптов Papyrus доходят", a_token, ms);
	}
}
