// The voice adapter of Speech Broker.
//
// It is a mod: an ordinary SKSE plugin living in the process of the game, and it
// owns the microphone for the whole build. Two halves lie under it and this file
// is the only thing that knows both.
//
//   THE EARS - src/audio and src/turn. The microphone, the noise floor, the
//   cutting of the stream into passes. They know nothing of models or of the
//   bridge and can be run from a wav with no game at all.
//
//   THE DISPATCH - src/models. The registry of model mods, one thread per model,
//   the deadlines, the arbitration between two readings of one sound, the
//   standings. It knows nothing of the bridge or of the settings file.
//
// Here there are only the SKSE entry points, the two handshakes and the
// translation of a finished slice of speech into what the bridge understands.
//
// WHAT USED TO BE HERE AND IS GONE. Until 17.09 this file started a python
// service, polled it over HTTP for utterances and asked it to speak, and a
// "model" was a json listing that service read. The microphone may have exactly
// one owner, and by then it is the adapter; a model is now an SKSE plugin that
// registers through contract/speechbroker-voice-model.h. Service.cpp, Listen.cpp
// and Speak.cpp went with that change, and nothing in this module opens a socket
// any more.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "speechbroker-adapter.h"

#include "Bridge.h"
#include "Config.h"
#include "Loc.h"

#include "models/ModelHost.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace Voice;

	// The only path written into the code besides the settings file: where the game
	// keeps its translations. It is not a setting - the engine fixes it.
	constexpr auto kTranslations = LR"(Data\Interface\Translations)";

	// ----------------------------------------------------------------- the numbers
	//
	// OUR SLICE IDS AND THE BRIDGE'S UTTERANCE IDS ARE DIFFERENT NUMBERS, and the
	// translation between them lives here because the adapter is the only side that
	// knows both. A slice says which earlier slices it swallowed, in OUR numbering;
	// the bridge understands only its own, and would silently fail to revoke a
	// piece a person has already acted on.
	//
	// It is bounded, and it is trimmed from the OLDEST rather than dropped whole.
	// A long piece arriving just after a drop would not find the short ones it
	// swallowed, the absorption would not happen, and the bridge would announce
	// both the pieces and the whole phrase. Our ids rise, so in an ordered map the
	// oldest is always the first.
	class Numbering
	{
	public:
		void Remember(std::int32_t a_slice, std::int32_t a_bridge)
		{
			if (a_slice == 0 || a_bridge == 0) {
				return;
			}
			const std::scoped_lock lock(_lock);
			_toBridge[a_slice] = a_bridge;

			const auto limit = Config::Get().idMapLimit;
			while (limit > 0 && _toBridge.size() > static_cast<std::size_t>(limit)) {
				_toBridge.erase(_toBridge.begin());
			}
		}

		// 0 when we no longer hold that translation, which IS the case of "the
		// absorption will not happen" and so is never passed over in silence.
		std::int32_t Of(std::int32_t a_slice) const
		{
			const std::scoped_lock lock(_lock);
			const auto             found = _toBridge.find(a_slice);
			return found == _toBridge.end() ? 0 : found->second;
		}

	private:
		mutable std::mutex                        _lock;
		std::map<std::int32_t, std::int32_t>      _toBridge;
	};

	Numbering g_numbering;

	// --------------------------------------------------------------- what goes out
	//
	// One finished slice of speech, in the words of the bridge's own contract.
	//
	// THREAD: an assembling worker of the dispatch half. It must not be the game
	// thread and it never is; the bridge copies every string inside the call.
	void PublishToBridge(const Models::Slice& a_slice)
	{
		auto& bridge = Bridge::Get();
		if (!bridge.Ready() || !bridge.Listening()) {
			// The bridge has put us in reserve: another adapter is the source, and
			// handing it speech anyway would be two microphones talking at once.
			return;
		}
		if (a_slice.hypotheses.empty()) {
			return;
		}

		const auto& best = a_slice.hypotheses.front();

		// The alternatives are what SEVERAL MODELS DISAGREEING produced, not an
		// n-best list of one model: a model returns one reading, and the argument
		// between them is built above it. The strings must outlive the call, so
		// the vectors are held here rather than pointed at a temporary.
		std::vector<const char*> altText;
		std::vector<float>       altScore;
		for (std::size_t i = 1; i < a_slice.hypotheses.size(); ++i) {
			altText.push_back(a_slice.hypotheses[i].text.c_str());
			altScore.push_back(a_slice.hypotheses[i].score);
		}

		std::vector<std::int32_t> swallowed;
		for (const auto mine : a_slice.supersedes) {
			if (const auto theirs = g_numbering.Of(mine); theirs != 0) {
				swallowed.push_back(theirs);
			} else {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_PIECE_UNKNOWN", best.model, mine);
			}
		}

		SpeechBrokerAPI::UtteranceIn in{};
		in.text = best.text.c_str();
		in.language = "";
		in.engine = best.model.c_str();
		in.channel = "";
		in.score = best.score;
		// The margin over the second hypothesis, and 0 when there is no second one:
		// one model agreeing with itself is not a margin.
		in.margin = altScore.empty() ? 0.0f : best.score - altScore.front();
		in.latencyMs = 0;
		in.durationMs = static_cast<std::int32_t>(a_slice.DurationMs());

		// EVERY SLICE IS FINAL AS THE BRIDGE COUNTS IT. What used to be an interim
		// utterance here - a draft one model gave before another refined it - is
		// settled inside the adapter now, by the arbitration, before anything
		// leaves. What the bridge still gets is a REFINEMENT of a piece it already
		// holds, and that is refinesId below, not isFinal.
		in.isFinal = true;
		in.complete = a_slice.complete;
		in.lengthClass = static_cast<std::int32_t>(a_slice.lengthClass);

		if (!altText.empty()) {
			in.altText = altText.data();
			in.altScore = altScore.data();
			in.altCount = static_cast<std::int32_t>(altText.size());
		}
		if (!swallowed.empty()) {
			in.supersedes = swallowed.data();
			in.supersedesCount = static_cast<std::int32_t>(swallowed.size());
		}

		// A slice that re-reads a stretch already sent carries the id of that
		// stretch. The bridge is told by number, so it can correct what it holds
		// instead of announcing the same speech twice.
		const auto refines = a_slice.refines != 0 ? g_numbering.Of(a_slice.refines) : 0;
		in.refinesId = refines;

		const auto given = bridge.PushUtterance(in);
		if (given == 0) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_BRIDGE_REFUSED_UTTERANCE", best.model);
			return;
		}

		// A refinement keeps the number it refined; only a new piece earns one.
		if (refines == 0) {
			g_numbering.Remember(a_slice.id, given);
		}
		if (!swallowed.empty()) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_ABSORBS", given, swallowed.size(), a_slice.complete);
		}
	}

	// ------------------------------------------------------------ what comes in
	//
	// THREAD: the bridge's, inside our own callback. Everything here either sets an
	// atomic or copies and returns - the bridge gathers its jobs under a lock and
	// sends them out without it, and an adapter that worked inside this call would
	// hold every other adapter behind it.
	void OnJob(const SpeechBrokerAPI::Job& a_job, void*)
	{
		switch (a_job.kind) {
		case SpeechBrokerAPI::kJobListen:
			Bridge::Get().SetListening(a_job.active);
			Loc::Info("$SPEECHBROKERVOICE_LOG_BRIDGE_ROLE",
				Loc::Get(a_job.active ? "$SPEECHBROKERVOICE_WORD_I_AM_SOURCE"
				                      : "$SPEECHBROKERVOICE_WORD_I_AM_RESERVE"),
				a_job.text ? a_job.text : "");
			break;

		case SpeechBrokerAPI::kJobVocabulary:
			{
				// A hint, never a grammar: a model is free to ignore it. The list is
				// merged across every subscriber, so the dispatch half clips it to
				// what the contract allows before it reaches a model.
				std::vector<std::string> phrases;
				for (std::int32_t i = 0; a_job.phrases && i < a_job.phraseCount; ++i) {
					if (a_job.phrases[i]) {
						phrases.emplace_back(a_job.phrases[i]);
					}
				}
				Loc::Info("$SPEECHBROKERVOICE_LOG_VOCABULARY", phrases.size());
				Models::Host::Get().SetVocabulary(std::move(phrases));
			}
			break;

		case SpeechBrokerAPI::kJobSpeak:
			// THIS ADAPTER HEARS AND DOES NOT SPEAK, and it says so at once rather
			// than leaving the bridge waiting on a report that is never coming.
			// Speech is a model mod of its own and an adapter of its own; when one
			// exists, the bridge will send this job there instead.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_NO_SPEAKER", a_job.speechId);
			Bridge::Get().PushSpeechDone(a_job.speechId, false, false);
			break;

		case SpeechBrokerAPI::kJobAsk:
			Loc::Info("$SPEECHBROKERVOICE_LOG_NOT_FOR_ME", a_job.service ? a_job.service : "?");
			break;

		default:
			break;
		}
	}

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

	// Introduce ourselves to the bridge. It is the only thing the bridge is told:
	// what we are called and that we hear.
	void RegisterWithBridge()
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
			Loc::Error("$SPEECHBROKERVOICE_LOG_REGISTER_REFUSED");
			return;
		}
		Loc::Info("$SPEECHBROKERVOICE_LOG_REGISTERED", config.adapterId);
	}

	void OnBridgeMessage(SKSE::MessagingInterface::Message* a_message)
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
			Loc::Error("$SPEECHBROKERVOICE_LOG_BRIDGE_TOO_OLD",
				bridge.Version(), SpeechBrokerAPI::kInterfaceVersion);
			bridge.Detach();
			return;
		}
		if (bridge.Version() > SpeechBrokerAPI::kInterfaceVersion) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_BRIDGE_NEWER",
				bridge.Version(), SpeechBrokerAPI::kInterfaceVersion);
		}
		Loc::Info("$SPEECHBROKERVOICE_LOG_INTERFACE_RECEIVED", bridge.Version());
		RegisterWithBridge();
	}

	// Our own SKSE messages, and there is exactly one moment that matters.
	//
	// kDataLoaded, and not earlier, is when every model plugin has certainly been
	// loaded and has had its chance to subscribe. There is no second broadcast and
	// no entry point to ask for the table: a model that was not listening is simply
	// never registered and costs nobody anything.
	void OnSkseMessage(SKSE::MessagingInterface::Message* a_message)
	{
		if (!a_message || a_message->type != SKSE::MessagingInterface::kDataLoaded) {
			return;
		}

		// THE ADDRESS OF THE POINTER, with dataLen == sizeof(void*). It is the house
		// convention and it is what the bridge does towards its own adapters; the
		// contract states it in as many words, so that a shim author copying the
		// line is right. The table is a static with process lifetime and is never
		// replaced, so a static here is honest.
		static const SpeechBrokerVoiceHost* table = Models::Host::Table();
		if (auto* messaging = SKSE::GetMessagingInterface()) {
			messaging->Dispatch(SPEECHBROKERVOICE_MESSAGE_HOST, &table,
				static_cast<std::uint32_t>(sizeof(table)), nullptr);
			Loc::Info("$SPEECHBROKERVOICE_LOG_HOST_BROADCAST");
		}

		// NEVER ON THE THREAD OF THE GAME. Begin opens a capture device, which may
		// sit through the reopen delays, and reads the calibration off the disk.
		// A frame is 11.1 ms.
		std::thread([]() {
			if (!Models::Host::Get().Begin()) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_EARS_NO_SOURCE");
			}
		}).detach();
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
	Loc::Load(kTranslations, ResolveLanguage("auto"));

	Loc::Info("$SPEECHBROKERVOICE_LOG_PLUGIN_LOADED", PLUGIN_NAME, PLUGIN_VERSION);
	if (!Config::Load()) {
		Loc::Error("$SPEECHBROKERVOICE_LOG_NO_SETTINGS_FATAL");
		return true;
	}

	if (const auto& asked = Config::Get().language; !asked.empty() && asked != "auto") {
		Loc::Load(kTranslations, ResolveLanguage(asked));
	}

	// The dispatch half is told its settings BEFORE anything can register with it.
	// Register runs on the game thread during plugin load, and a model plugin
	// loaded after us may reach it within the same message cycle; it starts
	// nothing, which is why it is safe here and Begin is not.
	Models::Host::Get().Configure(Config::Get().models, Config::Get().ears, PublishToBridge);

	// The bridge broadcasts its interface under its own name - that is what we
	// listen for. Our own broadcast, to the model mods, waits for kDataLoaded.
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener("SpeechBroker", OnBridgeMessage);
		messaging->RegisterListener(OnSkseMessage);
	}
	return true;
}
