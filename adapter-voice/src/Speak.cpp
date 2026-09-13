#include "Speak.h"

#include "Bridge.h"
#include "Config.h"
#include "Service.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <nlohmann/json.hpp>
#include <httplib.h>

namespace Voice
{
	void Speak(const std::string& a_text, std::int32_t a_speechId)
	{
		const auto& config = Config::Get();
		auto&       bridge = Bridge::Get();

		// Кого спрашивать, решает Config: здесь нас не касается, названа
		// говорящая модель в настройках или взята первая подходящая.
		const auto* model = config.SpeakingModel();
		if (!model) {
			SKSE::log::warn("озвучка {}: ни одна установленная модель не умеет говорить", a_speechId);
			bridge.PushSpeechDone(a_speechId, false, false);
			return;
		}

		const Service   service;
		const auto&     ep = service.Where();
		httplib::Client client(ep.host, ep.port);
		client.set_read_timeout(config.sayTimeoutSec, 0);
		client.set_default_headers({ { Service::kPass, config.service.token } });
		// Кем говорить, решает адаптер, а не служба: у неё моделей может быть
		// несколько, и выбор - наше дело, потому что это мы знаем, что объявил
		// каждый установленный мод.
		nlohmann::json payload{ { "text", a_text }, { "model", model->id } };
		auto           res = client.Post("/say", payload.dump(), "application/json");

		const bool ok = res && res->status == 200;
		SKSE::log::info("озвучка {} моделью {}: {}", a_speechId, model->id,
			ok ? "сказано" : "не вышло");
		bridge.PushSpeechDone(a_speechId, ok, false);
	}
}
