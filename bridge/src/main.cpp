// Envoy Framework - the bridge between Skyrim and outside models.
//
// The bridge is static: it starts nothing and knows no external program. Talking
// to a particular model is the adapter's job, and the adapter knows both sides.
//
// One file works in SE, AE and VR: what it can do is decided by whether a
// provider for the data exists, not by which edition of the game is running.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <filesystem>
#include <string>

#include "core/Config.h"
#include "core/Loc.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "game/GameLoadWatch.h"
#include "game/PapyrusApi.h"
#include "game/MenuPanel.h"
#include "game/SkseHost.h"
#include "envoy-adapter.h"

#include "wire/AdapterHost.h"

namespace
{
	// The only paths written into the code: where the settings file lives and
	// where the game keeps its translations. Neither is a setting - the first is
	// the place everything else is read from, and the second is fixed by the
	// engine.
	constexpr auto kConfigPath = LR"(Data\SKSE\Plugins\envoy\envoy.json)";
	constexpr auto kTranslations = LR"(Data\Interface\Translations)";

	// Which language the text on screen is in. "auto" means the one the game
	// itself runs in, which is what a player expects and never has to set.
	//
	// Asking the engine rather than reading Skyrim.ini ourselves matters: the file
	// that wins is decided by the launcher, by MO2 and by which of the several
	// ini files the player last edited, and the engine has already settled all of
	// that by the time we ask.
	std::string ResolveLanguage(const std::string& a_asked)
	{
		if (!a_asked.empty() && a_asked != "auto") {
			return a_asked;
		}
		if (auto* ini = RE::INISettingCollection::GetSingleton()) {
			if (auto* setting = ini->GetSetting("sLanguage:General")) {
				if (setting->GetType() == RE::Setting::Type::kString) {
					if (const auto* value = setting->GetString(); value && *value) {
						return value;
					}
				}
			}
		}
		return "english";
	}

	// Which SKSE messages actually reach us is not a theoretical question: in the
	// first live run the bridge never got the message about the game loading and
	// so never called the participants to declare themselves again. Until the
	// cause is known, every message is named in the log.
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
		default:                                      return Envoy::Loc::Get("$ENVOY_WORD_UNKNOWN");
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message) {
			return;
		}

		Envoy::Log::Info("$ENVOY_LOG_SKSE_MESSAGE", MessageName(a_message->type), a_message->type);

		// The interface is handed out the way HIGGS and PLANCK hand theirs out: by
		// broadcasting an SKSE message. Adapters are ordinary plugins, they catch
		// it and call the functions directly. No ports, no addresses.
		if (a_message->type == SKSE::MessagingInterface::kPostPostLoad) {
			auto* api = static_cast<EnvoyAPI::IEnvoy*>(&Envoy::AdapterHost::Get());
			SKSE::GetMessagingInterface()->Dispatch(EnvoyAPI::kMessageInterface, &api,
				static_cast<std::uint32_t>(sizeof(api)), nullptr);
			Envoy::Log::Info("$ENVOY_LOG_INTERFACE_BROADCAST");
		}

		// The bridge learns about a game being loaded from the engine, not from
		// SKSE: in VR there is no kPostLoadGame message at all, and this branch
		// would be silent forever. The holder of the game events appears by
		// kDataLoaded, and that is when we subscribe.
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
	// The address library plus the modern struct layout: this pair is what makes
	// SKSE accept the plugin for newer builds of the game as well. VR does not
	// read this data at all - there SKSEPlugin_Query above does the work.
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

	// Where to write the log is SKSE's business, not the core's: the path is
	// handed to it from outside.
	std::filesystem::path logFile;
	if (auto dir = SKSE::log::log_directory()) {
		logFile = *dir / (std::string(PLUGIN_NAME) + ".log");
	}

	// Parsed settings rather than the raw document: the in-game menu and
	// ReloadSettings read the same fields, and the value has to be one.
	//
	// The order of these three is not a matter of taste. The settings hold the
	// level of the log, so they are read first and say nothing while they do it.
	// The log is set up next, because the translation reports whether it loaded.
	// And only then are the settings named out loud, in the language the player
	// reads.
	const auto& settings = Envoy::Settings::Get();
	Envoy::Log::Init(settings.logLevel, logFile, settings.logMaxSizeKb);
	Envoy::Log::SetShowSpeech(settings.logSpeechText);

	// All the text of the module. Loaded once, before anything can draw or speak:
	// changing the language needs the game restarted, which is what the engine
	// demands of itself too.
	Envoy::Loc::Load(kTranslations, ResolveLanguage(settings.language));
	Envoy::Settings::Report();

	// Up to this line the core answers its own three questions: work is done on
	// the spot, nothing happens in the game, events go to the log. Here the game
	// starts answering all three.
	Envoy::SkseHost::Install();

	// The window in the mod menu. A soft dependency: no SKSE Menu Framework, no
	// window, and the log is still set from the file.
	Envoy::MenuPanel::Install();

	// The contract version is a property of the binary, not a setting. This line
	// used to print the "interfaceVersion" field out of the settings file, where a
	// stale 1 had been sitting for ages: two different numbers under one name, and
	// the wrong one reached the log. Sorting out the run of 07.09 started here.
	Envoy::Log::Info("$ENVOY_LOG_PLUGIN_LOADED", PLUGIN_NAME, PLUGIN_VERSION,
		EnvoyAPI::kInterfaceVersion);
	Envoy::Log::Info("$ENVOY_LOG_CONFIG_SOURCE",
		Envoy::Loc::Get(Envoy::Config::Describe(config.Source())), config.Path().string());

	if (!config.Error().empty()) {
		Envoy::Log::Warn("$ENVOY_LOG_CONFIG_ERROR", config.Error());
	}

	if (auto* papyrus = SKSE::GetPapyrusInterface()) {
		papyrus->Register(Envoy::PapyrusApi::Register);
	}

	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}

	return true;
}
