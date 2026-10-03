#include "Config.h"
#include "Loc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Voice
{
	namespace
	{
		constexpr auto kHome = LR"(Data\SKSE\Plugins\speechbroker\adapters\voice)";
		constexpr auto kConfigPath = LR"(Data\SKSE\Plugins\speechbroker\adapters\voice\speechbroker-voice.json)";

		// A path from the settings, if it is relative, is taken from the folder of
		// the adapter, and it cannot leave it. Whoever can replace the settings file
		// could otherwise make the adapter read - or one day write - anywhere on the
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

		// ------------------------------------------------------------------ the ears
		// The microphone and the cutting of the stream into passes.
		//
		// Every reader below obeys the rule of this file without an exception: a key
		// that is missing leaves the field at the value its struct was built with,
		// and those values are the shipping behaviour. A settings file written
		// before a block existed behaves exactly as it did.
		//
		// THE READING GOES ONE WAY ONLY. These structs live in src/audio, src/turn
		// and src/models, and not one file there includes this one - which is what
		// lets both halves be built and run outside the game. Config fills them;
		// nothing fills Config back.
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
			// does not know where that folder is.
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

		// -------------------------------------------------------------- the models
		// Who may register, how a model's life is run, and how an argument between
		// two of them is settled.
		//
		// THERE IS NO MODEL IN HERE, only the rules that apply to all of them. A
		// model is an SKSE plugin the player installed; it declares itself through
		// the C ABI and the adapter learns of it at kDataLoaded. Naming one in this
		// file would tie the adapter back to somebody else's mod, which is exactly
		// what the listing folder used to do.
		void ReadKindPolicy(const nlohmann::json& a_doc, Models::KindPolicy& a_out)
		{
			// The one a player is entitled to be asked about is `remote`, because it
			// is the one where the sound of their room leaves the machine. It ships
			// off, and the gate is applied at Register - before the model has
			// resolved a name or opened a socket.
			a_out.inProcess = a_doc.value("inProcess", a_out.inProcess);
			a_out.child = a_doc.value("child", a_out.child);
			a_out.attached = a_doc.value("attached", a_out.attached);
			a_out.remote = a_doc.value("remote", a_out.remote);
		}

		void ReadDispatch(const nlohmann::json& a_doc, Models::DispatchSettings& a_out)
		{
			a_out.startAttempts = a_doc.value("startAttempts", a_out.startAttempts);
			a_out.defaultBudgetMs = a_doc.value("defaultBudgetMs", a_out.defaultBudgetMs);
			a_out.bootstrapSlackMs = a_doc.value("bootstrapSlackMs", a_out.bootstrapSlackMs);
			a_out.probeSeconds = a_doc.value("probeSeconds", a_out.probeSeconds);
			a_out.busyWindow = a_doc.value("busyWindow", a_out.busyWindow);
			a_out.busyRateLimit = a_doc.value("busyRateLimit", a_out.busyRateLimit);
			a_out.stackGuaranteeBytes = a_doc.value("stackGuaranteeBytes", a_out.stackGuaranteeBytes);
			a_out.submitOverrunMs = a_doc.value("submitOverrunMs", a_out.submitOverrunMs);
			a_out.startComplaintMs = a_doc.value("startComplaintMs", a_out.startComplaintMs);
		}

		void ReadStanding(const nlohmann::json& a_doc, Models::ReputationSettings& a_out)
		{
			// The path is left as it was written and is resolved inside the folder
			// of the adapter by whoever opens it - the same rule the wav goes
			// through, applied where the file is actually read.
			a_out.file = a_doc.value("file", a_out.file);
			a_out.keep = a_doc.value("keep", a_out.keep);
			a_out.defaultTrust = a_doc.value("defaultTrust", a_out.defaultTrust);
			a_out.minSample = a_doc.value("minSample", a_out.minSample);
			a_out.latencyPercentile = a_doc.value("latencyPercentile", a_out.latencyPercentile);
			a_out.fastBelowMs = a_doc.value("fastBelowMs", a_out.fastBelowMs);
			a_out.latencySamplesNeeded = a_doc.value("latencySamplesNeeded", a_out.latencySamplesNeeded);
			a_out.maxFailurePenalty = a_doc.value("maxFailurePenalty", a_out.maxFailurePenalty);
			a_out.maxInventionPenalty = a_doc.value("maxInventionPenalty", a_out.maxInventionPenalty);
			a_out.minWeight = a_doc.value("minWeight", a_out.minWeight);
		}

		void ReadArbiter(const nlohmann::json& a_doc, Models::ArbiterSettings& a_out)
		{
			a_out.overlapMs = a_doc.value("overlapMs", a_out.overlapMs);
			a_out.overlapPercent = a_doc.value("overlapPercent", a_out.overlapPercent);
		}

		void ReadHost(const nlohmann::json& a_doc, Models::HostSettings& a_out)
		{
			if (a_doc.contains("allow")) {
				ReadKindPolicy(a_doc["allow"], a_out.allow);
			}
			if (a_doc.contains("dispatch")) {
				ReadDispatch(a_doc["dispatch"], a_out.dispatch);
			}
			if (a_doc.contains("standing")) {
				ReadStanding(a_doc["standing"], a_out.standing);
			}
			if (a_doc.contains("arbiter")) {
				ReadArbiter(a_doc["arbiter"], a_out.arbiter);
			}
			a_out.workers = a_doc.value("workers", a_out.workers);
			a_out.stackGuaranteeBytes = a_doc.value("stackGuaranteeBytes", a_out.stackGuaranteeBytes);
			a_out.trustEarsFloor = a_doc.value("trustEarsFloor", a_out.trustEarsFloor);
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

	bool Config::Load()
	{
		std::error_code ec;
		if (!std::filesystem::exists(kConfigPath, ec)) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_NO_SETTINGS",
				std::filesystem::path{ kConfigPath }.string());
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

			self.language = doc.value("language", self.language);
			self.idMapLimit = doc.value("idMapLimit", self.idMapLimit);

			if (doc.contains("ears")) {
				ReadEars(doc["ears"], self.ears);
			}
			if (doc.contains("models")) {
				ReadHost(doc["models"], self.models);
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_SETTINGS_BROKEN", e.what());
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

		// At least one worker, or a closed pass is sealed by the timer and then
		// assembled by nobody: every utterance of the session would be lost
		// silently, which is the worst of the two failures this module knows.
		self.models.workers = std::max(1, self.models.workers);

		return true;
	}
}
