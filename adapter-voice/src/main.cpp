// The voice adapter for Envoy.
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
			SKSE::log::info("bridge: {} ({})", a_job.active ? "I am the source" : "I am in reserve",
				a_job.text ? a_job.text : "");
			break;
		case EnvoyAPI::kJobVocabulary:
			SKSE::log::info("vocabulary of the subscribers: {} phrases", a_job.phraseCount);
			break;
		case EnvoyAPI::kJobSpeak:
			if (a_job.text) {
				// Speaking goes in a thread of its own: the thread of the game must not be
				// held for the length of the synthesis.
				std::string text = a_job.text;
				const auto  speechId = a_job.speechId;
				std::thread([text, speechId]() { Speak(text, speechId); }).detach();
			}
			break;
		case EnvoyAPI::kJobAsk:
			// The voice adapter holds no language models: that capability is declared by
			// another adapter, and the bridge will send the request there.
			SKSE::log::info("a request to {} is not addressed to me", a_job.service ? a_job.service : "?");
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
			SKSE::log::error("the bridge refused to register us");
			return;
		}
		SKSE::log::info("registered at the bridge as {}", config.adapterId);

		if (config.models.empty()) {
			// This is not a breakage: a model is installed as a separate mod, and a
			// person may have installed only the adapter. It has to be said plainly, or a
			// silent microphone looks like a fault of ours.
			SKSE::log::warn("not a single model is installed - there is nothing to recognise with. "
			                "A model is installed as a separate mod and puts its listing "
			                "into Data/SKSE/Plugins/envoy/adapters/voice/models");
			return;
		}

		for (const auto& model : config.models) {
			SKSE::log::info("model {} ({}) out of {}: {}{}", model.id, model.name, model.source,
				model.enabled ? (model.fast ? "draft" : "accurate") : "switched off",
				model.speaks ? ", can speak" : "");
		}

		// One thread: the microphone belongs to the adapter, its service is its own
		// and single, and which model recognised an utterance is said in its answer.
		std::thread([]() { PollService(); }).detach();
		const auto started = std::count_if(config.models.begin(), config.models.end(),
			[](const Voice::Model& a_model) { return a_model.enabled && a_model.hears; });
		SKSE::log::info("models declared: {}, declaring to the bridge: {}", started,
			config.adapterProvides.empty() ? "nothing" : config.adapterProvides);
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

		// A bridge NEWER than us we are obliged to accept: it does not read fields of
		// ours that did not yet exist in the version we declared, and an old adapter
		// stays sound as far as it is concerned. A bridge OLDER than us must not be
		// accepted: it will not understand what we send.
		//
		// There used to be a strict equality here, and reworking the bridge up to
		// version three knocked the adapter out entirely: the run of 07.09 never
		// happened, because speech did not reach the bridge at all. Compatibility
		// made from one side is not compatibility.
		if (bridge.Version() < EnvoyAPI::kInterfaceVersion) {
			SKSE::log::error("the bridge is older than me: it understands contract version {}, "
			                 "and I speak {} - I will not work",
				bridge.Version(), EnvoyAPI::kInterfaceVersion);
			bridge.Detach();
			return;
		}
		if (bridge.Version() > EnvoyAPI::kInterfaceVersion) {
			SKSE::log::info("the bridge is newer than me: contract version {} against my {} - "
			                "working by mine, it will not ask me for the new fields",
				bridge.Version(), EnvoyAPI::kInterfaceVersion);
		}
		SKSE::log::info("the interface of the bridge received, version {}", bridge.Version());
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

	SKSE::log::info("{} v{} loaded", PLUGIN_NAME, PLUGIN_VERSION);
	if (!Config::Load()) {
		SKSE::log::error("I cannot work without settings");
		return true;
	}

	// The bridge broadcasts the interface under its own name - that is what we
	// listen for.
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("Envoy", OnMessage);
	}
	return true;
}
