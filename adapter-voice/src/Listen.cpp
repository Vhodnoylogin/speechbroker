#include "Listen.h"

#include "Bridge.h"
#include "Service.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <nlohmann/json.hpp>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Voice
{
	namespace
	{
		// Relating the results of different models to each other and translating the
		// numbers between the service and the bridge. One piece of state under one
		// lock: there is only one polling thread now, but speaking comes from its own,
		// and the lock is still needed.
		//
		// Until the sound source has a common number for a piece, the results of
		// different models are related by time: an accurate answer that came shortly
		// after a preliminary one counts as its refinement. That is temporary and is
		// honestly marked as such.
		//
		// The number of an utterance at the service and at the bridge are different
		// numbers. The service says which of ITS OWN pieces the new one swallowed; the
		// bridge understands only its own. The translation lives here: the adapter is
		// the only one that knows both sides, and keeping this map anywhere else would
		// mean making one of the sides know about the other.
		//
		// The translation is separate per model. Every model has its own counter of
		// pieces, and with two models at once - "a fast one plus an accurate one",
		// which is what the adapter exists for - their numbers would collide in one
		// map: an entry of one would cover an entry of the other, and the "oldest" one
		// would be whichever had the smaller counter. An absorption always refers to
		// pieces of the same model, so there is no need to look in anybody else map.
		class Correlation
		{
		public:
			// The numbers of the service -> the numbers of the bridge. An unknown number
			// IS the case of "the absorption will not happen", so it is not passed over
			// in silence.
			std::vector<std::int32_t> Translate(const std::string& a_modelId,
				const nlohmann::json& a_serviceIds)
			{
				std::vector<std::int32_t> out;
				std::scoped_lock          lock(_lock);
				const auto& map = _serviceToBridge[a_modelId];
				for (const auto& mark : a_serviceIds) {
					const auto serviceId = mark.get<std::int32_t>();
					const auto found = map.find(serviceId);
					if (found != map.end()) {
						out.push_back(found->second);
					} else {
						SKSE::log::warn("model {}: piece {} was not found in the translation - "
						                "its absorption will not reach the bridge", a_modelId, serviceId);
					}
				}
				return out;
			}

			// For an accurate answer: the number of the draft it refines, or 0. A draft is
			// given out once - a second accurate answer will not get it any more.
			std::int32_t TakePreliminary(std::chrono::milliseconds a_window)
			{
				std::scoped_lock lock(_lock);
				if (_lastPreliminary != 0 &&
					std::chrono::steady_clock::now() - _lastPreliminaryAt < a_window) {
					return std::exchange(_lastPreliminary, 0);
				}
				return 0;
			}

			void Remember(const std::string& a_modelId, bool a_fast, std::int32_t a_serviceId,
				std::int32_t a_bridgeId)
			{
				std::scoped_lock lock(_lock);
				if (a_fast) {
					_lastPreliminary = a_bridgeId;
					_lastPreliminaryAt = std::chrono::steady_clock::now();
				}
				// The translation is remembered so that the next piece can name what it
				// swallowed. The map grows by one entry per piece of speech, and it needs a
				// limit. But it must not be dropped whole: a long piece arriving right after a
				// drop would not find its short ones, the absorption would silently not
				// happen, and the bridge would announce both the pieces and the whole phrase.
				// So only the oldest are thrown out. The numbers of the service run across and
				// grow, so in an ordered map the oldest is always the first. A limit of zero
				// or less means no limit.
				if (a_serviceId != 0) {
					auto& map = _serviceToBridge[a_modelId];
					map[a_serviceId] = a_bridgeId;
					const auto limit = Config::Get().idMapLimit;
					while (limit > 0 && map.size() > static_cast<std::size_t>(limit)) {
						map.erase(map.begin());
					}
				}
			}

		private:
			std::mutex                            _lock;
			std::int32_t                          _lastPreliminary{ 0 };
			std::chrono::steady_clock::time_point _lastPreliminaryAt{};
			// The name of the model -> (the number of the service -> the number of the
			// bridge).
			std::map<std::string, std::map<std::int32_t, std::int32_t>> _serviceToBridge;
		};

		Correlation g_correlation;

		void PushResult(const nlohmann::json& a_item)
		{
			auto& bridge = Bridge::Get();
			if (!bridge.Ready()) {
				return;
			}

			const auto text = a_item.value("text", std::string{});
			const auto engine = a_item.value("engine", std::string{});

			// Who recognised the utterance is said by the service; whether it is a draft
			// or a final answer is known from the listing of that model. An unknown name
			// is not a silent case: it means the service is loading a model that has no
			// mod in the build, and that has to be looked at by eye.
			const auto* model = Config::Get().Find(engine);
			if (!model) {
				SKSE::log::warn("the service returned an answer from model '{}', which is not among the "
				                "installed ones - taking it as final", engine);
			}
			const bool fast = model && model->fast;

			EnvoyAPI::UtteranceIn in{};
			in.text = text.c_str();
			in.language = model ? model->language.c_str() : "";
			in.engine = engine.c_str();
			in.channel = "";
			in.score = a_item.value("score", 0.0f);
			in.margin = a_item.value("margin", 0.0f);
			in.latencyMs = a_item.value("ms", 0);
			in.durationMs = 0;
			in.isFinal = !fast;

			// --- the third version of the contract -------------------------------
			// A service on the new engine hands over not a whole phrase after a silence
			// but pieces as the speech goes, and about each of them it says how sure it is
			// that the phrase ended on it. The bridge holds the unfinished back - but only
			// if it was told, and only the one that hears the pause can tell it.
			//
			// A service that knows nothing about this will send no such fields, and the
			// values stay as they were: completeness one, nothing to absorb.
			in.complete = a_item.value("complete", 1.0f);
			in.lengthClass = a_item.value("lengthClass", 0);

			std::vector<std::int32_t> swallowed;
			if (a_item.contains("supersedes")) {
				swallowed = g_correlation.Translate(engine, a_item["supersedes"]);
			}
			if (!swallowed.empty()) {
				in.supersedes = swallowed.data();
				in.supersedesCount = static_cast<std::int32_t>(swallowed.size());
			}

			if (!fast) {
				in.refinesId = g_correlation.TakePreliminary(
					std::chrono::milliseconds(Config::Get().correlateMs));
			}

			const auto id = bridge.PushUtterance(in);
			if (id == 0) {
				SKSE::log::warn("the bridge did not take the utterance from model {}", engine);
				return;
			}

			g_correlation.Remember(engine, fast, a_item.value("id", 0), id);

			if (!swallowed.empty()) {
				SKSE::log::info("utterance {} absorbs {} earlier ones, completeness {:.2f}",
					id, swallowed.size(), in.complete);
			}
		}
	}

	void PollService()
	{
		const auto&   config = Config::Get();
		const Service service;
		const auto&   ep = service.Where();
		const auto    timeout = config.service.listenTimeoutSec;
		int           since = 0;

		// Two different outcomes, and they are obliged to differ in the log: otherwise
		// there is no telling from it whether we checked that the service starts on
		// its own or connected to one brought up by hand in advance.
		if (service.Alive()) {
			SKSE::log::info("the service is already up, connecting to it");
		} else {
			service.Launch();
		}

		// There is deliberately no way out of the loop: the thread is detached and
		// dies together with the process of the game, and the adapter never gets the
		// chance to shut itself down.
		for (;;) {
			if (!Bridge::Get().Listening()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(config.idleSleepMs));
				continue;
			}

			httplib::Client client(ep.host, ep.port);
			// The service holds /listen for its whole deadline, until speech has gathered;
			// it has to be waited for longer than it holds, or the client cuts off a
			// lawful answer on the doorstep.
			client.set_read_timeout(timeout + config.listenGraceSec, 0);
			const auto path = "/listen?since=" + std::to_string(since) +
			                  "&timeout=" + std::to_string(timeout);
			client.set_default_headers({ { Service::kPass, config.service.token } });
			auto res = client.Get(path);
			if (!res || res->status != 200) {
				std::this_thread::sleep_for(std::chrono::milliseconds(config.retryDelayMs));
				continue;
			}

			try {
				auto body = nlohmann::json::parse(res->body);
				for (const auto& item : body.value("utterances", nlohmann::json::array())) {
					since = std::max(since, item.value("id", 0));
					PushResult(item);
				}
			} catch (const std::exception& e) {
				// The pause here is compulsory. It was on the "not 200" path and was not on
				// the "200 with a body that does not parse" path, and somebody else program
				// that took the port and answered instantly drove this loop to the limit: the
				// core under load and thousands of lines a second into the log, right in the
				// middle of play.
				SKSE::log::warn("the answer of the service did not parse - {}", e.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(config.retryDelayMs));
			}
		}
	}
}
