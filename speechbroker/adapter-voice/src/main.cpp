// The voice adapter for SpeechBroker.
//
// This is a mod: an ordinary SKSE plugin living in the process of the game. It
// talks to the bridge by calling a function, and outward - to its own service,
// which owns the microphone and cannot be part of the game - it goes over HTTP
// on the loopback. The bridge knows nothing about that.
//
// The microphone belongs to the ADAPTER, not to the models: the service rides
// inside this very mod and comes up by itself at load - there is nothing for a
// player to start. A model, on the other hand, is a recogniser: somebody else
// mod puts a listing into the models folder next to our settings, and the
// service loads its weights. The particular case of "a fast one plus an
// accurate one" is two such mods, and which of them recognised an utterance is
// said in the answer of the service.
//
// Here there are only the SKSE entry points and the handshake with the bridge.
// The settings are in Config, the life of the service in Service, carrying the
// utterances over in Listen, the synthesis in Speak, our side of the bridge in
// Bridge.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "speechbroker-adapter.h"

#include "Bridge.h"
#include "Config.h"
#include "Loc.h"
#include "Listen.h"
#include "Speak.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <string>
#include <thread>

namespace
{
	using namespace Voice;

	// The only path written into the code besides the settings file: where the game
	// keeps its translations. It is not a setting - the engine fixes it.
	constexpr auto kTranslations = LR"(Data\Interface\Translations)";

	// Which language to write in. "auto" means the one the game itself runs in.
	//
	// Asking the engine rather than reading Skyrim.ini ourselves matters: which of
	// the several ini files wins is decided by the launcher, by MO2 and by whichever
	// the player last edited, and the engine has settled all of that by the time we
	// ask.
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

	void OnJob(const SpeechBrokerAPI::Job& a_job, void*)
	{
		switch (a_job.kind) {
		case SpeechBrokerAPI::kJobListen:
			Bridge::Get().SetListening(a_job.active);
			Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_BRIDGE_ROLE",
				Voice::Loc::Get(a_job.active ? "$SPEECHBROKERVOICE_WORD_I_AM_SOURCE"
				                             : "$SPEECHBROKERVOICE_WORD_I_AM_RESERVE"),
				a_job.text ? a_job.text : "");
			break;
		case SpeechBrokerAPI::kJobVocabulary:
			Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_VOCABULARY", a_job.phraseCount);
			break;
		case SpeechBrokerAPI::kJobSpeak:
			if (a_job.text) {
				// Speaking goes in a thread of its own: the thread of the game must not be
				// held for the length of the synthesis.
				std::string text = a_job.text;
				const auto  speechId = a_job.speechId;
				std::thread([text, speechId]() { Speak(text, speechId); }).detach();
			}
			break;
		case SpeechBrokerAPI::kJobAsk:
			// The voice adapter holds no language models: that capability is declared by
			// another adapter, and the bridge will send the request there.
			Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_NOT_FOR_ME", a_job.service ? a_job.service : "?");
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

		SpeechBrokerAPI::AdapterInfo info{};
		info.id = config.adapterId.c_str();
		info.name = config.adapterName.c_str();
		info.provides = config.adapterProvides.c_str();
		info.contract = SpeechBrokerAPI::kInterfaceVersion;

		if (!bridge.Register(info, OnJob, nullptr)) {
			Voice::Loc::Error("$SPEECHBROKERVOICE_LOG_REGISTER_REFUSED");
			return;
		}
		Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_REGISTERED", config.adapterId);

		if (config.models.empty()) {
			// This is not a breakage: a model is installed as a separate mod, and a
			// person may have installed only the adapter. It has to be said plainly, or a
			// silent microphone looks like a fault of ours.
			Voice::Loc::Warn("$SPEECHBROKERVOICE_LOG_NO_MODELS");
			return;
		}

		for (const auto& model : config.models) {
			Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL", model.id, model.name, model.source,
				Voice::Loc::Get(model.enabled
				                    ? (model.fast ? "$SPEECHBROKERVOICE_WORD_DRAFT" : "$SPEECHBROKERVOICE_WORD_ACCURATE")
				                    : "$SPEECHBROKERVOICE_WORD_SWITCHED_OFF"),
				model.speaks ? Voice::Loc::Get("$SPEECHBROKERVOICE_WORD_CAN_SPEAK") : "");
		}

		// One thread: the microphone belongs to the adapter, its service is its own
		// and single, and which model recognised an utterance is said in its answer.
		std::thread([]() { PollService(); }).detach();
		const auto started = std::count_if(config.models.begin(), config.models.end(),
			[](const Voice::Model& a_model) { return a_model.enabled && a_model.hears; });
		Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_DECLARED", started,
			config.adapterProvides.empty() ? Voice::Loc::Get("$SPEECHBROKERVOICE_WORD_NOTHING")
			                               : config.adapterProvides);
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message || a_message->type != SpeechBrokerAPI::kMessageInterface) {
			return;
		}
		if (a_message->dataLen != sizeof(SpeechBrokerAPI::ISpeechBroker*)) {
			return;
		}
		auto* speechbroker = *static_cast<SpeechBrokerAPI::ISpeechBroker**>(a_message->data);
		if (!speechbroker) {
			return;
		}
		auto& bridge = Bridge::Get();
		bridge.Attach(speechbroker);

		// A bridge NEWER than us we are obliged to accept: it does not read fields of
		// ours that did not yet exist in the version we declared, and an old adapter
		// stays sound as far as it is concerned. A bridge OLDER than us must not be
		// accepted: it will not understand what we send.
		//
		// There used to be a strict equality here, and reworking the bridge up to
		// version three knocked the adapter out entirely: the run of 07.09 never
		// happened, because speech did not reach the bridge at all. Compatibility
		// made from one side is not compatibility.
		if (bridge.Version() < SpeechBrokerAPI::kInterfaceVersion) {
			Voice::Loc::Error("$SPEECHBROKERVOICE_LOG_BRIDGE_TOO_OLD",
				bridge.Version(), SpeechBrokerAPI::kInterfaceVersion);
			bridge.Detach();
			return;
		}
		if (bridge.Version() > SpeechBrokerAPI::kInterfaceVersion) {
			Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_BRIDGE_NEWER",
				bridge.Version(), SpeechBrokerAPI::kInterfaceVersion);
		}
		Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_INTERFACE_RECEIVED", bridge.Version());
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

	// The text first, so that a mistake in the settings is legible. At this point
	// the language can only be the one the game runs in - the file that could pin
	// another one has not been read yet - so the table is laid again below if it
	// turns out to name one.
	Voice::Loc::Load(kTranslations, ResolveLanguage("auto"));

	Voice::Loc::Info("$SPEECHBROKERVOICE_LOG_PLUGIN_LOADED", PLUGIN_NAME, PLUGIN_VERSION);
	if (!Config::Load()) {
		Voice::Loc::Error("$SPEECHBROKERVOICE_LOG_NO_SETTINGS_FATAL");
		return true;
	}

	if (const auto& asked = Config::Get().language; !asked.empty() && asked != "auto") {
		Voice::Loc::Load(kTranslations, ResolveLanguage(asked));
	}

	// The bridge broadcasts the interface under its own name - that is what we
	// listen for.
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("SpeechBroker", OnMessage);
	}
	return true;
}
