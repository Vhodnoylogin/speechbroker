#include "Config.h"
#include "Loc.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

namespace Voice
{
	namespace
	{
		constexpr auto kHome = LR"(Data\SKSE\Plugins\speechbroker\adapters\voice)";
		constexpr auto kConfigPath = LR"(Data\SKSE\Plugins\speechbroker\adapters\voice\speechbroker-voice.json)";

		// The folder every model mod puts its listing into. The name of the file does
		// not matter - only the id key inside it does; the folder is read in the order
		// of the names, so that the list of models does not depend on how the file
		// system handed them back and two launches give one and the same order.
		constexpr auto kModelsDir = LR"(Data\SKSE\Plugins\speechbroker\adapters\voice\models)";

		// A path from the settings, if it is relative, is taken from the folder of the
		// adapter, and it cannot leave it. The adapter starts ITS OWN service, which
		// rides inside its own mod; being able to write anything here would mean that
		// whoever replaced the settings file starts any program they like on the
		// machine of the player.
		//
		// An empty string on the way out means a refusal.
		std::string ResolveInside(const std::string& a_path, const std::filesystem::path& a_base)
		{
			if (a_path.empty()) {
				return a_path;
			}
			std::filesystem::path given{ a_path };
			if (given.is_absolute() || given.has_root_name()) {
				return {};
			}
			const auto full = (a_base / given).lexically_normal();
			const auto root = a_base.lexically_normal();
			// lexically_relative gives a ".." at the front exactly when the path went
			// above the root. Comparing strings will not do here: "voice-evil" begins
			// with "voice".
			const auto rel = full.lexically_relative(root);
			if (rel.empty() || *rel.begin() == "..") {
				return {};
			}
			return full.string();
		}

		// The service has to live on this very machine. Everything the player says
		// and hears goes through it; a foreign host in the settings would mean that
		// installing a voice mod silently switches on the sending of what was said
		// out of the house.
		bool Loopback(const std::string& a_url)
		{
			auto       rest = a_url;
			const auto scheme = rest.find("://");
			if (scheme != std::string::npos) {
				rest = rest.substr(scheme + 3);
			}
			const auto slash = rest.find('/');
			if (slash != std::string::npos) {
				rest = rest.substr(0, slash);
			}
			const auto colon = rest.rfind(':');
			if (colon != std::string::npos && rest.find(']') == std::string::npos) {
				rest = rest.substr(0, colon);
			}
			return rest == "127.0.0.1" || rest == "localhost" || rest == "::1" || rest == "[::1]";
		}

		// A secret per session. The randomness is wanted not for the strength of a
		// cipher but so that the value cannot be guessed in advance and wired into
		// somebody else program that took the port.
		std::string MakeToken()
		{
			std::random_device                 source;
			std::uniform_int_distribution<int> digit(0, 15);
			constexpr char                     alphabet[] = "0123456789abcdef";
			std::string                        out;
			out.reserve(32);
			for (int i = 0; i < 32; ++i) {
				out.push_back(alphabet[digit(source)]);
			}
			return out;
		}

		std::optional<AutoStart> ReadAutoStart(const nlohmann::json& a_doc)
		{
			const std::filesystem::path home{ kHome };
			AutoStart                   out;
			out.enabled = a_doc.value("enabled", out.enabled);

			const auto exec = a_doc.value("exec", std::string{});
			out.exec = ResolveInside(exec, home);
			if (!exec.empty() && out.exec.empty()) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_EXEC_OUTSIDE",
					exec);
				return std::nullopt;
			}

			const auto dir = a_doc.value("workingDir", std::string{});
			out.workingDir = ResolveInside(dir, home);
			if (!dir.empty() && out.workingDir.empty()) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_WORKDIR_OUTSIDE", dir);
				return std::nullopt;
			}

			out.parentPidArg = a_doc.value("parentPidArg", out.parentPidArg);
			out.waitSec = a_doc.value("waitSec", out.waitSec);
			out.pollSec = a_doc.value("pollSec", out.pollSec);
			// The arguments are left alone. The service itself resolves them from its own
			// working folder, and some of them are not paths at all: "--port", "8931".
			for (const auto& arg : a_doc.value("args", nlohmann::json::array())) {
				out.args.push_back(arg.get<std::string>());
			}
			return out;
		}

		// ------------------------------------------------------------------ the ears
		// The microphone and the cutting of the stream into passes.
		//
		// Every reader below obeys the rule of this file without an exception: a key
		// that is missing leaves the field at the value its struct was built with,
		// and those values are the ones the python shipped. A settings file written
		// before this block existed behaves exactly as it did.
		//
		// THE READING GOES ONE WAY ONLY. These structs live in src/audio and
		// src/turn, and not one file there includes this one - which is what lets
		// the whole listening half be built and run from a wav, outside the game.
		// Config fills them; nothing fills Config back.
		void ReadDevice(const nlohmann::json& a_doc, DeviceSettings& a_out)
		{
			a_out.input = a_doc.value("input", a_out.input);
			a_out.inputApi = a_doc.value("inputApi", a_out.inputApi);
			a_out.allowDefault = a_doc.value("allowDefault", a_out.allowDefault);

			const auto reopen = a_doc.value("reopen", nlohmann::json::object());
			a_out.reopen.tries = reopen.value("tries", a_out.reopen.tries);
			a_out.reopen.delayMs = reopen.value("delayMs", a_out.reopen.delayMs);
		}

		void ReadCapture(const nlohmann::json& a_doc, CaptureSettings& a_out)
		{
			const auto source = a_doc.value("source", std::string{});
			if (source == "file") {
				a_out.source = Source::File;
			} else if (!source.empty() && source != "device") {
				// A word this build does not know is not obeyed and is not passed
				// over either: the microphone is taken and the player is told which
				// of the two words they meant to write.
				Loc::Warn("$SPEECHBROKERVOICE_LOG_SETTINGS_SOURCE_UNKNOWN", source);
			}

			// The wav is pinned to the folder of the adapter HERE, because the
			// capture cannot do it: audio/ may not include this file and therefore
			// does not know where that folder is. It resolves what it is handed
			// against the working directory and refuses anything that climbs out;
			// what it is handed is already relative to the adapter.
			const std::filesystem::path home{ kHome };
			const auto                  file = a_doc.value("file", std::string{});
			a_out.file = ResolveInside(file, home);
			if (!file.empty() && a_out.file.empty()) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_CAPTURE_FILE_OUTSIDE", file);
			}

			a_out.filePaced = a_doc.value("filePaced", a_out.filePaced);
			a_out.fileLoop = a_doc.value("fileLoop", a_out.fileLoop);
			a_out.blockMs = a_doc.value("blockMs", a_out.blockMs);
			a_out.ringMs = a_doc.value("ringMs", a_out.ringMs);

			if (a_doc.contains("device")) {
				ReadDevice(a_doc["device"], a_out.device);
			}
		}

		void ReadVad(const nlohmann::json& a_doc, VadSettings& a_out)
		{
			a_out.noiseFloorSec = a_doc.value("noiseFloorSec", a_out.noiseFloorSec);
			a_out.warmUpMs = a_doc.value("warmUpMs", a_out.warmUpMs);
			a_out.startFactor = a_doc.value("startFactor", a_out.startFactor);
			a_out.minRmsFloor = a_doc.value("minRmsFloor", a_out.minRmsFloor);
			a_out.startMs = a_doc.value("startMs", a_out.startMs);
			a_out.preRollMs = a_doc.value("preRollMs", a_out.preRollMs);
			a_out.endSilenceMs = a_doc.value("endSilenceMs", a_out.endSilenceMs);
			a_out.maxUttSec = a_doc.value("maxUttSec", a_out.maxUttSec);
			a_out.minUttSec = a_doc.value("minUttSec", a_out.minUttSec);
			a_out.minPeak = a_doc.value("minPeak", a_out.minPeak);
		}

		void ReadPacer(const nlohmann::json& a_doc, PacerSettings& a_out)
		{
			a_out.sliceSilenceMs = a_doc.value("sliceSilenceMs", a_out.sliceSilenceMs);
			a_out.endSilenceMs = a_doc.value("endSilenceMs", a_out.endSilenceMs);
			a_out.maxSpanMs = a_doc.value("maxSpanMs", a_out.maxSpanMs);
		}

		void ReadTurn(const nlohmann::json& a_doc, TurnSettings& a_out)
		{
			a_out.anchorSlackMs = a_doc.value("anchorSlackMs", a_out.anchorSlackMs);
			a_out.snapSlackMs = a_doc.value("snapSlackMs", a_out.snapSlackMs);
			a_out.resumeQuietMs = a_doc.value("resumeQuietMs", a_out.resumeQuietMs);
			a_out.matchSlackMs = a_doc.value("matchSlackMs", a_out.matchSlackMs);
			a_out.minPassMs = a_doc.value("minPassMs", a_out.minPassMs);
		}

		void ReadProsody(const nlohmann::json& a_doc, ProsodySettings& a_out)
		{
			a_out.minHz = a_doc.value("minHz", a_out.minHz);
			a_out.maxHz = a_doc.value("maxHz", a_out.maxHz);
			a_out.frameMs = a_doc.value("frameMs", a_out.frameMs);
			a_out.hopMs = a_doc.value("hopMs", a_out.hopMs);
			a_out.tailMs = a_doc.value("tailMs", a_out.tailMs);
			a_out.voicedNeeded = a_doc.value("voicedNeeded", a_out.voicedNeeded);
			a_out.voicedInTail = a_doc.value("voicedInTail", a_out.voicedInTail);
			a_out.peakRatio = a_doc.value("peakRatio", a_out.peakRatio);
			a_out.lowPercentile = a_doc.value("lowPercentile", a_out.lowPercentile);
			a_out.highPercentile = a_doc.value("highPercentile", a_out.highPercentile);
			a_out.minSpanMs = a_doc.value("minSpanMs", a_out.minSpanMs);
		}

		void ReadCompleteness(const nlohmann::json& a_doc, CompletenessSettings& a_out)
		{
			a_out.pauseFactor = a_doc.value("pauseFactor", a_out.pauseFactor);
			a_out.withTerminalMark = a_doc.value("withTerminalMark", a_out.withTerminalMark);
			a_out.withoutTerminalMark = a_doc.value("withoutTerminalMark", a_out.withoutTerminalMark);
			a_out.junkNoSpeechProb = a_doc.value("junkNoSpeechProb", a_out.junkNoSpeechProb);
			a_out.pitchWeight = a_doc.value("pitchWeight", a_out.pitchWeight);
		}

		void ReadSegments(const nlohmann::json& a_doc, SegmentSettings& a_out)
		{
			a_out.shortMaxWords = a_doc.value("shortMaxWords", a_out.shortMaxWords);
			a_out.middleMaxWords = a_doc.value("middleMaxWords", a_out.middleMaxWords);
		}

		void ReadEars(const nlohmann::json& a_doc, EarsSettings& a_out)
		{
			if (a_doc.contains("capture")) {
				ReadCapture(a_doc["capture"], a_out.capture);
			}
			if (a_doc.contains("vad")) {
				ReadVad(a_doc["vad"], a_out.vad);
			}
			if (a_doc.contains("pacer")) {
				ReadPacer(a_doc["pacer"], a_out.pacer);
			}
			if (a_doc.contains("turn")) {
				ReadTurn(a_doc["turn"], a_out.turn);
			}
			if (a_doc.contains("prosody")) {
				ReadProsody(a_doc["prosody"], a_out.prosody);
			}
			if (a_doc.contains("completeness")) {
				ReadCompleteness(a_doc["completeness"], a_out.completeness);
			}
			if (a_doc.contains("segments")) {
				ReadSegments(a_doc["segments"], a_out.segments);
			}
			a_out.consumerPollMs = a_doc.value("consumerPollMs", a_out.consumerPollMs);
		}

		Model ReadModel(const nlohmann::json& a_doc, const std::filesystem::path& a_file)
		{
			Model out;
			out.source = a_file.filename().string();
			out.id = a_doc.value("id", out.id);
			out.name = a_doc.value("name", out.id);
			out.enabled = a_doc.value("enabled", out.enabled);
			if (!out.enabled) {
				return out;
			}
			out.fast = a_doc.value("class", std::string{}) == "fast";
			out.language = a_doc.value("language", out.language);

			// What a model can do it declares itself. The default is hearing only:
			// recognition is in every model this adapter was written for, and speaking is
			// not.
			const auto provides = a_doc.value("provides", std::string{ "asr" });
			out.hears = provides.find("asr") != std::string::npos;
			out.speaks = provides.find("tts") != std::string::npos;
			return out;
		}

		// Reads the folder of models. A refusal of one listing does not cancel the
		// rest: a listing is brought along by SOMEBODY ELSE mod, and its mistake must
		// not leave a person without the models that are fine. With its own settings
		// file the adapter is still strict - a mistake there is ours.
		//
		// The adapter neither reads nor checks the weights of the models: they are
		// loaded by the service, and the rule "the weights lie inside their own mod"
		// is guarded by the service as well. One rule must not have two guards.
		std::vector<Model> ReadModels()
		{
			std::error_code ec;
			if (!std::filesystem::exists(kModelsDir, ec)) {
				return {};
			}

			std::vector<std::filesystem::path> files;
			for (const auto& entry : std::filesystem::directory_iterator(kModelsDir, ec)) {
				if (!entry.is_regular_file(ec)) {
					continue;
				}
				auto path = entry.path();
				if (path.extension() == ".json") {
					files.push_back(std::move(path));
				}
			}
			std::sort(files.begin(), files.end());

			std::vector<Model> out;
			for (const auto& file : files) {
				try {
					nlohmann::json doc;
					std::ifstream  stream(file);
					stream >> doc;
					auto model = ReadModel(doc, file);
					if (model.id.empty()) {
						Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_NO_ID",
							file.filename().string());
						continue;
					}
					const auto twin = std::find_if(out.begin(), out.end(),
						[&](const Model& a_seen) { return a_seen.id == model.id; });
					if (twin != out.end()) {
						Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_TWICE",
							model.id, twin->source, model.source);
						continue;
					}
					out.push_back(std::move(model));
				} catch (const std::exception& e) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_BROKEN",
						file.filename().string(), e.what());
				}
			}
			return out;
		}
	}

	Config& Config::Mutable()
	{
		static Config instance;
		return instance;
	}

	const Config& Config::Get()
	{
		return Mutable();
	}

	const Model* Config::Find(const std::string& a_engineId) const
	{
		for (const auto& model : models) {
			if (model.id == a_engineId) {
				return &model;
			}
		}
		return nullptr;
	}

	const Model* Config::SpeakingModel() const
	{
		for (const auto& model : models) {
			if (!model.enabled || !model.speaks) {
				continue;
			}
			if (speakModel.empty() || model.id == speakModel) {
				return &model;
			}
		}
		return nullptr;
	}

	bool Config::Load()
	{
		std::error_code ec;
		if (!std::filesystem::exists(kConfigPath, ec)) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_NO_SETTINGS", std::filesystem::path{ kConfigPath }.string());
			return false;
		}

		nlohmann::json doc;
		auto&          self = Mutable();
		try {
			std::ifstream stream(kConfigPath);
			stream >> doc;

			const auto adapter = doc.value("adapter", nlohmann::json::object());
			self.adapterId = adapter.value("id", self.adapterId);
			self.adapterName = adapter.value("name", self.adapterName);

			const auto service = doc.value("service", nlohmann::json::object());
			self.service.url = service.value("url", self.service.url);
			self.service.listenTimeoutSec =
				service.value("listenTimeoutSec", self.service.listenTimeoutSec);
			if (service.contains("autoStart")) {
				self.service.autoStart = ReadAutoStart(service["autoStart"]);
			}
			self.service.token = MakeToken();

			self.speakModel = doc.value("speakModel", self.speakModel);
			self.language = doc.value("language", self.language);
			self.correlateMs = doc.value("correlateMs", self.correlateMs);
			self.retryDelayMs = doc.value("retryDelayMs", self.retryDelayMs);
			self.healthTimeoutSec = doc.value("healthTimeoutSec", self.healthTimeoutSec);
			self.listenGraceSec = doc.value("listenGraceSec", self.listenGraceSec);
			self.idleSleepMs = doc.value("idleSleepMs", self.idleSleepMs);
			self.sayTimeoutSec = doc.value("sayTimeoutSec", self.sayTimeoutSec);
			self.idMapLimit = doc.value("idMapLimit", self.idMapLimit);

			if (doc.contains("ears")) {
				ReadEars(doc["ears"], self.ears);
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_SETTINGS_BROKEN", e.what());
			return false;
		}

		if (!Loopback(self.service.url)) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_NOT_LOOPBACK",
				self.service.url);
			return false;
		}

		// The judge of completeness does not police its own weights, and must not:
		// it is const, it holds no state and it runs once per fragment of an answer.
		// So the one weight that can be set to a value which quietly disables a whole
		// branch is checked here, where the number is read. Below one, the rule that
		// forgives a hurried pause can never fire; at or below nothing, every pause
		// reads as the end of a sentence. Neither shows up anywhere as an error - the
		// verdicts simply come out wrong.
		if (self.ears.completeness.pauseFactor < 1.0) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_SETTINGS_PAUSE_FACTOR",
				self.ears.completeness.pauseFactor);
		}

		self.models = ReadModels();

		// The adapter does not declare its capabilities but works them out: promising
		// the bridge speech when not one installed model speaks means taking the work
		// away from an adapter that can do it. This used to be a line in the settings
		// file - that is, a promise backed by nothing.
		const bool hears = std::any_of(self.models.begin(), self.models.end(),
			[](const Model& a_model) { return a_model.enabled && a_model.hears; });
		const bool speaks = self.SpeakingModel() != nullptr;
		self.adapterProvides.clear();
		if (hears) {
			self.adapterProvides = "asr";
		}
		if (speaks) {
			self.adapterProvides += self.adapterProvides.empty() ? "tts" : ",tts";
		}

		return true;
	}
}
