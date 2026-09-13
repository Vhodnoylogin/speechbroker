// Адаптер голоса для Envoy.
//
// Это мод: обычный плагин SKSE, живущий в процессе игры. С мостом он говорит
// вызовом функции, а наружу - к своей службе, которая владеет микрофоном и
// частью игры быть не может, - ходит по HTTP на петлю. Мост об этом не знает.
//
// Микрофон принадлежит АДАПТЕРУ, а не моделям: служба едет внутри этого же
// мода и поднимается сама при загрузке - игроку запускать нечего. Модель же -
// это распознаватель: чужой мод кладёт листок в папку models рядом с нашими
// настройками, и служба грузит её веса. Частный случай "быстрая плюс точная" -
// это два таких мода, а какая из них узнала реплику, сказано в ответе службы.
//
// Здесь - только точки входа SKSE и рукопожатие с мостом. Настройки в Config,
// жизнь службы в Service, перенос реплик в Listen, синтез в Speak, наша
// сторона моста в Bridge.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "envoy-adapter.h"

#include "Bridge.h"
#include "Config.h"
#include "Listen.h"
#include "Speak.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <string>
#include <thread>

namespace
{
	using namespace Voice;

	void InitLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= PLUGIN_NAME;
		*path += ".log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto logger = std::make_shared<spdlog::logger>("global log", std::move(sink));
		logger->set_level(spdlog::level::info);
		logger->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(logger));
	}

	void OnJob(const EnvoyAPI::Job& a_job, void*)
	{
		switch (a_job.kind) {
		case EnvoyAPI::kJobListen:
			Bridge::Get().SetListening(a_job.active);
			SKSE::log::info("мост: {} ({})", a_job.active ? "я источник" : "я в запасе",
				a_job.text ? a_job.text : "");
			break;
		case EnvoyAPI::kJobVocabulary:
			SKSE::log::info("словарь подписчиков: {} фраз", a_job.phraseCount);
			break;
		case EnvoyAPI::kJobSpeak:
			if (a_job.text) {
				// Озвучка идёт в своём потоке: держать поток игры на время
				// синтеза нельзя.
				std::string text = a_job.text;
				const auto  speechId = a_job.speechId;
				std::thread([text, speechId]() { Speak(text, speechId); }).detach();
			}
			break;
		case EnvoyAPI::kJobAsk:
			// Голосовой адаптер языковых моделей не держит: эта способность
			// объявляется другим адаптером, и мост направит запрос ему.
			SKSE::log::info("запрос к {} мне не адресован", a_job.service ? a_job.service : "?");
			break;
		default:
			break;
		}
	}

	void Start()
	{
		auto& bridge = Bridge::Get();
		if (!bridge.Ready()) {
			return;
		}
		const auto& config = Config::Get();

		EnvoyAPI::AdapterInfo info{};
		info.id = config.adapterId.c_str();
		info.name = config.adapterName.c_str();
		info.provides = config.adapterProvides.c_str();
		info.contract = EnvoyAPI::kInterfaceVersion;

		if (!bridge.Register(info, OnJob, nullptr)) {
			SKSE::log::error("мост отказал в регистрации");
			return;
		}
		SKSE::log::info("зарегистрирован в мосту как {}", config.adapterId);

		if (config.models.empty()) {
			// Это не поломка: модель ставится отдельным модом, и человек мог
			// поставить только адаптер. Сказать об этом надо прямо, иначе
			// молчащий микрофон выглядит как наша ошибка.
			SKSE::log::warn("не установлено ни одной модели - распознавать нечем. "
			                "Модель ставится отдельным модом и кладёт свой листок "
			                "в Data/SKSE/Plugins/envoy/adapters/voice/models");
			return;
		}

		for (const auto& model : config.models) {
			SKSE::log::info("модель {} ({}) из {}: {}{}", model.id, model.name, model.source,
				model.enabled ? (model.fast ? "черновая" : "точная") : "выключена",
				model.speaks ? ", умеет говорить" : "");
		}

		// Поток один: микрофон принадлежит адаптеру, служба у него своя и одна,
		// а какая модель узнала реплику - сказано в её ответе.
		std::thread([]() { PollService(); }).detach();
		const auto started = std::count_if(config.models.begin(), config.models.end(),
			[](const Voice::Model& a_model) { return a_model.enabled && a_model.hears; });
		SKSE::log::info("моделей объявлено: {}, объявляю мосту: {}", started,
			config.adapterProvides.empty() ? "ничего" : config.adapterProvides);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message || a_message->type != EnvoyAPI::kMessageInterface) {
			return;
		}
		if (a_message->dataLen != sizeof(EnvoyAPI::IEnvoy*)) {
			return;
		}
		auto* envoy = *static_cast<EnvoyAPI::IEnvoy**>(a_message->data);
		if (!envoy) {
			return;
		}
		auto& bridge = Bridge::Get();
		bridge.Attach(envoy);

		// Мост НОВЕЕ себя принимать обязаны: он не читает у нас полей, которых
		// в объявленной нами версии ещё не было, и старый адаптер для него
		// остаётся исправным. Мост СТАРШЕ себя принимать нельзя: он не поймёт
		// того, что мы шлём.
		//
		// Прежде здесь стояло строгое равенство, и переработка моста до третьей
		// версии выбила адаптер целиком: прогон 07.09 не состоялся, потому что
		// речь в мост не попадала вовсе. Совместимость, сделанная с одной
		// стороны, совместимостью не является.
		if (bridge.Version() < EnvoyAPI::kInterfaceVersion) {
			SKSE::log::error("мост старше меня: он понимает контракт версии {}, "
			                 "а я говорю на {} - работать не буду",
				bridge.Version(), EnvoyAPI::kInterfaceVersion);
			bridge.Detach();
			return;
		}
		if (bridge.Version() > EnvoyAPI::kInterfaceVersion) {
			SKSE::log::info("мост новее меня: контракт версии {} против моих {} - "
			                "работаю по своей, новых полей он у меня не спросит",
				bridge.Version(), EnvoyAPI::kInterfaceVersion);
		}
		SKSE::log::info("интерфейс моста получен, версия {}", bridge.Version());
		Start();
	}
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = PLUGIN_NAME;
	a_info->version = 1;
	return !a_skse->IsEditor();
}

extern "C" __declspec(dllexport) constinit auto SKSEPlugin_Version = []() {
	SKSE::PluginVersionData v;
	v.PluginVersion(REL::Version{ 0, 1, 0 });
	v.PluginName(PLUGIN_NAME);
	v.AuthorName(PLUGIN_AUTHOR);
	v.UsesAddressLibrary(true);
	v.UsesStructsPost629(true);
	v.CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST });
	return v;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	InitLog();

	SKSE::log::info("{} v{} загружен", PLUGIN_NAME, PLUGIN_VERSION);
	if (!Config::Load()) {
		SKSE::log::error("без настроек работать не могу");
		return true;
	}

	// Мост рассылает интерфейс от своего имени - слушаем именно его.
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("Envoy", OnMessage);
	}
	return true;
}
