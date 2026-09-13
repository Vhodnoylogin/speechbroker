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
		// Связь между результатами разных моделей и перевод номеров между
		// службой и мостом. Состояние одно, под одним замком: поток опроса
		// теперь один, но озвучка ходит из своего, и замок остаётся нужен.
		//
		// Пока у источника звука нет общего номера куска, результаты разных моделей
		// связываются по времени: точный ответ, пришедший вскоре после
		// предварительного, считается его уточнением. Это временно и честно помечено.
		//
		// Номер реплики у службы и у моста - разные числа. Служба говорит, какие
		// СВОИ куски вобрал новый; мост понимает только свои. Перевод живёт здесь:
		// адаптер - единственный, кто знает обе стороны, и держать эту карту
		// где-либо ещё значило бы заставить одну из сторон знать про другую.
		//
		// Перевод - отдельный на модель. Счётчик кусков у каждой модели свой,
		// и при двух моделях сразу - «быстрая плюс точная», ради чего адаптер
		// и заведён, - их номера сталкивались бы в одной карте: запись одной
		// перекрывала бы запись другой, а «старейшей» считалась бы та, чей
		// счётчик меньше. Поглощение всегда ссылается на куски той же модели,
		// так что чужую карту искать не надо.
		class Correlation
		{
		public:
			// Номера службы -> номера моста. Неизвестный номер - это и есть
			// «поглощение не случится», поэтому он не пропускается молча.
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
						SKSE::log::warn("модель {}: кусок {} в переводе не найден - "
						                "его поглощение до моста не дойдёт", a_modelId, serviceId);
					}
				}
				return out;
			}

			// Для точного ответа: номер черновика, который он уточняет, либо 0.
			// Черновик отдаётся один раз - второй точный ответ его уже не получит.
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
				// Запоминаем перевод, чтобы следующий кусок мог назвать поглощённые.
				// Карта растёт на реплику за кусок речи, и предел ей нужен. Но
				// сбрасывать её целиком нельзя: длинный кусок, пришедший сразу после
				// сброса, не нашёл бы своих коротких, и поглощение молча не
				// случилось бы - мост огласил бы и куски, и фразу целиком. Поэтому
				// выбрасываются только самые старые. Номера у службы сквозные и
				// растут, так что в упорядоченной карте старейший всегда первый.
				// Предел ноль и меньше - без предела.
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
			// Имя модели -> (номер службы -> номер моста).
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

			// Кто узнал реплику, говорит служба; черновик это или окончательный
			// ответ, знает листок этой модели. Неизвестное имя - не молчаливый
			// случай: значит, служба грузит модель, о которой в сборке нет мода,
			// и разбираться с этим надо глазами.
			const auto* model = Config::Get().Find(engine);
			if (!model) {
				SKSE::log::warn("служба вернула ответ модели «{}», которой нет среди "
				                "установленных - считаю окончательным", engine);
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

			// --- третья версия контракта ----------------------------------------
			// Служба на новом движке отдаёт не целую фразу после молчания, а куски
			// по ходу речи, и о каждом говорит, насколько уверена, что фраза на нём
			// кончилась. Мост придерживает незаконченное - но только если ему это
			// сказали, а сказать может лишь тот, кто слышит паузу.
			//
			// Служба, которая об этом ничего не знает, полей не пришлёт, и значения
			// останутся прежними: завершённость единица, поглощать нечего.
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
				SKSE::log::warn("мост не принял реплику от модели {}", engine);
				return;
			}

			g_correlation.Remember(engine, fast, a_item.value("id", 0), id);

			if (!swallowed.empty()) {
				SKSE::log::info("реплика {} поглощает {} прежних, завершённость {:.2f}",
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

		// Два разных исхода, и в журнале они обязаны различаться: иначе по нему
		// не понять, проверили мы самостоятельный запуск службы или подключились
		// к поднятой заранее руками.
		if (service.Alive()) {
			SKSE::log::info("служба уже поднята, подключаюсь к ней");
		} else {
			service.Launch();
		}

		// Выхода из цикла нет намеренно: поток отсоединён и умирает вместе с
		// процессом игры, а своего завершения адаптер выполнить не успевает.
		for (;;) {
			if (!Bridge::Get().Listening()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(config.idleSleepMs));
				continue;
			}

			httplib::Client client(ep.host, ep.port);
			// Служба держит /listen весь свой срок, пока не наберётся речи; ждать её
			// надо дольше, чем она держит, иначе клиент оборвёт законный ответ на пороге.
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
				// Пауза здесь обязательна. На пути "не 200" она была, а на пути
				// "200 с неразобранным телом" - нет, и чужая программа, занявшая
				// порт и отвечающая мгновенно, разгоняла этот цикл до предела:
				// ядро под нагрузкой и тысячи строк в журнал в секунду, прямо
				// во время игры.
				SKSE::log::warn("ответ службы не разобран - {}", e.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(config.retryDelayMs));
			}
		}
	}
}
