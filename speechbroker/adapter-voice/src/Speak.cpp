#include "Speak.h"
#include "Loc.h"

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
			Loc::Warn("$SPEECHBROKERVOICE_LOG_NO_SPEAKER", a_speechId);
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
		Loc::Info("$SPEECHBROKERVOICE_LOG_SPOKE", a_speechId, model->id,
			Loc::Get(ok ? "$SPEECHBROKERVOICE_WORD_SAID" : "$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
		bridge.PushSpeechDone(a_speechId, ok, false);
	}
}
