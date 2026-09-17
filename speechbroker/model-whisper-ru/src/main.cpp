// The SKSE half of the model mod, and deliberately the thin one.
//
// Everything that is hard - the debt of exactly one Complete, the queue that
// must never wait, the child tied to the process, the sentinels - is in
// src/core, which contains no RE/ and no SKSE/ and can be exercised without a
// game. What is left here is the plumbing that only SKSE can do: the three entry
// points, the log directory, the language the game runs in, and catching one
// broadcast.
//
// THE HANDSHAKE, WHICH IS FOUR FACTS AND NOT ONE OF THEM IS GUESSABLE. The
// adapter broadcasts SPEECHBROKERVOICE_MESSAGE_HOST under its own plugin name at
// kDataLoaded. THE PAYLOAD IS A POINTER TO THE POINTER - dataLen is
// sizeof(void*) and the table is reached by dereferencing once - which is the
// house convention and not a slip. And THERE IS NO SECOND BROADCAST: a model
// that was not listening is simply never registered, never asked for anything,
// and recognition carries on with whatever else is installed. Which is why the
// listener goes up in SKSEPlugin_Load, the only place guaranteed to be early
// enough.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "speechbroker-voice-model.h"

#include "core/ChildProcess.h"
#include "core/Log.h"
#include "core/ModelShim.h"
#include "core/Settings.h"

#include <string>

namespace
{
	using namespace WhisperRu;

	// Where this mod keeps its own things, relative to the folder the DLL was
	// loaded from - which is Data\SKSE\Plugins. It is not a setting and cannot
	// be one: it is the path the settings file itself is found at, and a
	// setting that says where the settings are is a joke with a long tail. It is
	// not an absolute path either; the folder of the DLL is asked of the loader.
	constexpr auto kOwnFolder = L"speechbroker/models/whisper-ru";
	constexpr auto kLocalizationFolder = L"localization";
	constexpr auto kTableName = "speechbrokermodelwhisperru";

	// Which language to render OUR OWN log in. "auto" is the language the game
	// runs in. Asking the engine rather than reading Skyrim.ini ourselves
	// matters: which of the several ini files wins is decided by the launcher,
	// by MO2 and by whichever the player last edited, and the engine has settled
	// all of that by the time we ask.
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

	void OnMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message || a_message->type != SPEECHBROKERVOICE_MESSAGE_HOST) {
			return;
		}
		// Refuse the message if dataLen is not sizeof(void*). The contract says
		// so in as many words, and the reason is that the alternative is
		// dereferencing whatever a future sender put there.
		if (a_message->dataLen != sizeof(void*)) {
			Log::Error("$SBWHISPERRU_LOG_HOST_PAYLOAD", static_cast<std::uint32_t>(a_message->dataLen));
			return;
		}
		const auto* host = *static_cast<const SpeechBrokerVoiceHost**>(a_message->data);
		if (!host) {
			return;
		}

		// Register is the ONE call a shim makes from its SKSE message handler,
		// and it is deliberately trivial. Nothing here brings a model up: Start
		// comes later, on a worker of the adapter's, and a model whose kind the
		// player has forbidden is refused before Start is ever reached - so a
		// forbidden model must not have started a process by now.
		int registered = 0;
		for (auto* model : Models()) {
			if (!model->Enabled()) {
				Log::Info("$SBWHISPERRU_LOG_MODEL_OFF", model->Id());
				continue;
			}
			if (model->Register(host)) {
				++registered;
			}
		}
		Log::Info("$SBWHISPERRU_LOG_HANDSHAKE", static_cast<std::int32_t>(registered),
			static_cast<std::int32_t>(Models().size()), host->abiVersion);
	}
}

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse,
	SKSE::PluginInfo* a_info)
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

	const auto own = ThisModuleFolder() / kOwnFolder;

	if (auto directory = SKSE::log::log_directory(); directory) {
		*directory /= PLUGIN_NAME;
		*directory += ".log";
		Log::OpenFile(*directory);
	}

	// The text first, so that a mistake in the settings is legible. At this
	// point the language can only be the one the game runs in - the file that
	// could name another has not been read yet - so the table is laid again
	// below if it turns out to name one.
	Log::LoadTable(own / kLocalizationFolder, kTableName, ResolveLanguage("auto"));
	Log::Info("$SBWHISPERRU_LOG_PLUGIN_LOADED", PLUGIN_NAME, PLUGIN_VERSION, own);

	auto settings = LoadSettings(own);
	Log::SetLevel(settings.logLevel);
	if (settings.language != "auto") {
		Log::LoadTable(own / kLocalizationFolder, kTableName, ResolveLanguage(settings.language));
	}

	for (auto& model : settings.models) {
		// Never freed, on purpose: the adapter may abandon a model's dispatch
		// thread after stopMs rather than kill it, and the state an abandoned
		// thread can still reach must outlive everything.
		Models().push_back(new ModelShim(model));
		Log::Info("$SBWHISPERRU_LOG_MODEL", model.id, model.name, model.declaredClass,
			model.weightsRelative, model.source);
	}
	if (settings.models.empty()) {
		Log::Warn("$SBWHISPERRU_LOG_NO_MODELS", own);
	}

	// The adapter broadcasts under its own plugin name; that is what we listen
	// for. There is no second broadcast and no entry point to ask for the table,
	// so a listener registered any later than here is a model that never exists.
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("SpeechBrokerVoiceAdapter", OnMessage);
	} else {
		Log::Error("$SBWHISPERRU_LOG_NO_MESSAGING");
	}
	return true;
}
