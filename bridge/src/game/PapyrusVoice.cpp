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

// The voice going out, the choice of adapter and the self-test of delivery:
// everything that leaves the game for the models, and the loop that proves the
// events come back.
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
		// The log obeys the settings at once. A reread file used to change the
		// thresholds of the auction but not the level of the log, and whoever was
		// tuning it saw the old lines while certain they had turned the knob.
		const auto& settings = Settings::Get();
		Log::SetLevel(settings.logLevel);
		Log::SetShowSpeech(settings.logSpeechText);
		SKSE::log::info("settings reread, log: level {}, the words of the player {}",
			Log::Level(), settings.logSpeechText ? "written down" : "not written down");
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

	// The loop closes like this: the bridge sends an event, the script of the
	// carrier quest catches it and calls Pong. An answer proves the events reach
	// Papyrus; silence proves the opposite. Neither is visible from the log of
	// the bridge any other way.
	void PapyrusApi::SelfTest(Tag)
	{
		const auto token = ++g_pingToken;
		g_pongHeard.store(false);
		SKSE::log::info("self-test of delivery: token {}, ringing in 2 s", token);

		// The ring is deliberately put off. A script subscribes to Envoy_Ping in the
		// same line where it asks for the self-test, and an instant broadcast would
		// outrun its subscription - the result would be a false "does not arrive".
		// Two seconds with room to spare.
		Scheduler::Get().After(std::chrono::seconds(2), [token]() {
			MainThread::Post([token]() {
				g_pingSentAt.store(std::chrono::steady_clock::now());
				SKSE::log::info("self-test of delivery: sending Envoy_Ping, token {}", token);
				Events::Send("Envoy_Ping", "", static_cast<float>(token));
			});
		});

		// The verdict is passed on a deadline of its own rather than by sleeping in
		// the same thread: there is one scheduler for everybody and it must not be
		// taken up with waiting.
		Scheduler::Get().After(std::chrono::seconds(7), [token]() {
			if (!g_pongHeard.load()) {
				SKSE::log::error("self-test of delivery: no answer on token {} within 5 s - "
				                 "the events of the bridge DO NOT REACH the Papyrus scripts", token);
			}
		});
	}

	void PapyrusApi::Pong(Tag, std::int32_t a_token)
	{
		g_pongHeard.store(true);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - g_pingSentAt.load()).count();
		SKSE::log::info("self-test of delivery: the answer on token {} came in {} ms - "
		                "the events of the bridge do reach the Papyrus scripts", a_token, ms);
	}
}
