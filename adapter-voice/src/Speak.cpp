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

		// Who to ask is decided by Config: whether the speaking model is named in the
		// settings or the first suitable one was taken is not our concern here.
		const auto* model = config.SpeakingModel();
		if (!model) {
			SKSE::log::warn("speech {}: not one installed model can speak", a_speechId);
			bridge.PushSpeechDone(a_speechId, false, false);
			return;
		}

		const Service   service;
		const auto&     ep = service.Where();
		httplib::Client client(ep.host, ep.port);
		client.set_read_timeout(config.sayTimeoutSec, 0);
		client.set_default_headers({ { Service::kPass, config.service.token } });
		// Who is to speak is decided by the adapter, not by the service: the service
		// may have several models, and the choice is our business, because we are the
		// ones that know what each installed mod declared.
		nlohmann::json payload{ { "text", a_text }, { "model", model->id } };
		auto           res = client.Post("/say", payload.dump(), "application/json");

		const bool ok = res && res->status == 200;
		SKSE::log::info("speech {} by model {}: {}", a_speechId, model->id,
			ok ? "said" : "did not work");
		bridge.PushSpeechDone(a_speechId, ok, false);
	}
}
