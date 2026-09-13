// Envoy Framework - мост между Skyrim и внешними моделями.
//
// Мост статичен: он ничего не инициирует и не знает ни одной внешней программы.
// Связь с конкретной моделью делает адаптер, знающий обе стороны.
//
// Один файл работает в SE, AE и VR: возможности определяются не редакцией игры,
// а тем, есть ли поставщик нужных данных.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <filesystem>

#include "core/Config.h"
#include "core/Log.h"
#include "game/GameLoadWatch.h"
#include "game/PapyrusApi.h"
#include "game/SkseHost.h"
#include "envoy-adapter.h"

#include "wire/AdapterHost.h"

namespace
{
	// Единственный путь, заданный в коде: местоположение файла настроек,
	// а не настройка. Всё остальное читается из него.
	constexpr auto kConfigPath = LR"(Data\SKSE\Plugins\envoy\envoy.json)";

	// Какие сообщения SKSE до нас доходят - вопрос не теоретический: в первом
	// живом прогоне мост не получил сообщения о загрузке игры и потому не позвал
	// участников объявиться заново. Пока причина не ясна, каждое сообщение
	// называется в журнале по имени.
	const char* MessageName(std::uint32_t a_type)
	{
		switch (a_type) {
		case SKSE::MessagingInterface::kPostLoad:     return "kPostLoad";
		case SKSE::MessagingInterface::kPostPostLoad: return "kPostPostLoad";
		case SKSE::MessagingInterface::kPreLoadGame:  return "kPreLoadGame";
		case SKSE::MessagingInterface::kPostLoadGame: return "kPostLoadGame";
		case SKSE::MessagingInterface::kSaveGame:     return "kSaveGame";
		case SKSE::MessagingInterface::kDeleteGame:   return "kDeleteGame";
		case SKSE::MessagingInterface::kInputLoaded:  return "kInputLoaded";
		case SKSE::MessagingInterface::kNewGame:      return "kNewGame";
		case SKSE::MessagingInterface::kDataLoaded:   return "kDataLoaded";
		default:                                      return "неизвестное";
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		SKSE::log::info("сообщение SKSE: {} ({})", MessageName(a_message->type), a_message->type);

		// Интерфейс отдаётся так же, как это делают HIGGS и PLANCK: рассылкой
		// сообщения SKSE. Адаптеры - обычные плагины, они его ловят и вызывают
		// функции напрямую. Ни портов, ни адресов.
		if (a_message->type == SKSE::MessagingInterface::kPostPostLoad) {
			auto* api = static_cast<EnvoyAPI::IEnvoy*>(&Envoy::AdapterHost::Get());
			SKSE::GetMessagingInterface()->Dispatch(EnvoyAPI::kMessageInterface, &api,
				static_cast<std::uint32_t>(sizeof(api)), nullptr);
			SKSE::log::info("интерфейс моста разослан адаптерам");
		}

		// Загрузку игры мост узнаёт не от SKSE, а от самого движка: в VR
		// сообщения kPostLoadGame нет вовсе, и эта ветка молчала бы всегда.
		// Держатель игровых событий появляется к kDataLoaded, тогда и подписываемся.
		if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
			Envoy::GameLoadWatch::Install();
		}
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
	// Адресная библиотека плюс современная раскладка структур: этой парой SKSE
	// признаёт плагин пригодным и для новых сборок игры. VR эти данные не читает
	// вовсе - там работает SKSEPlugin_Query выше.
	v.UsesAddressLibrary(true);
	v.UsesStructsPost629(true);
	v.CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST });
	return v;
}();

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	auto& config = Envoy::Config::Get();
	config.Load(kConfigPath);

	// Куда писать журнал, знает SKSE, а не ядро: путь отдаётся ему снаружи.
	std::filesystem::path logFile;
	if (auto dir = SKSE::log::log_directory()) {
		logFile = *dir / (std::string(PLUGIN_NAME) + ".log");
	}

	// "info" здесь - не настройка, а запасной вариант на случай, когда не удалось
	// разобрать даже встроенный набор. В норме значение приходит из файла.
	Envoy::Log::Init(config.Value<std::string>("/log/level").value_or("info"), logFile);

	// Ядро до этой строки отвечает себе само: работа делается на месте, в игре
	// ничего не происходит, события уходят в журнал. Здесь на все три вопроса
	// начинает отвечать игра.
	Envoy::SkseHost::Install();

	// Версия контракта - свойство двоичного файла, а не настройка. Раньше сюда
	// печаталось поле "interfaceVersion" из файла настроек, где с давних пор
	// лежала единица: два разных числа под одним именем, и в журнал попадало
	// не то. Разбор прогона 07.09 начался именно с этой строки.
	SKSE::log::info("{} v{} загружен, контракт версии {}", PLUGIN_NAME, PLUGIN_VERSION,
		EnvoyAPI::kInterfaceVersion);
	SKSE::log::info("{}: {}", Envoy::Config::Describe(config.Source()), config.Path().string());

	if (!config.Error().empty()) {
		SKSE::log::warn("настройки: {}", config.Error());
	}

	if (auto* papyrus = SKSE::GetPapyrusInterface()) {
		papyrus->Register(Envoy::PapyrusApi::Register);
	}

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}

	return true;
}
