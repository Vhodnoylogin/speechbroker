#include "Settings.h"

#include "Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace WhisperRu
{
	namespace
	{
		using json = nlohmann::json;

		// The two models this mod ships, as code. They are the source of the
		// files written on a first run, and they are also what a field left out
		// of an edited file falls back to - so a person may delete a line
		// instead of having to remember what was in it.
		ModelSettings DefaultTurbo()
		{
			ModelSettings model;
			model.id = "whisper-ru-turbo";
			model.name = "Whisper large-v3-turbo, Russian";
			model.declaredClass = "accurate";
			model.budgetMs = 2500;
			model.weightsRelative = "weights/whisper-ru-turbo";
			return model;
		}

		ModelSettings DefaultSmall()
		{
			ModelSettings model;
			model.id = "whisper-ru-small";
			model.name = "Whisper small, Russian";
			model.declaredClass = "fast";
			model.budgetMs = 700;
			model.weightsRelative = "weights/whisper-ru-small";
			return model;
		}

		std::string Str(const json& a_at, const char* a_key, const std::string& a_default)
		{
			const auto found = a_at.find(a_key);
			return (found != a_at.end() && found->is_string()) ? found->get<std::string>() : a_default;
		}

		bool Bool(const json& a_at, const char* a_key, bool a_default)
		{
			const auto found = a_at.find(a_key);
			return (found != a_at.end() && found->is_boolean()) ? found->get<bool>() : a_default;
		}

		std::int32_t Int(const json& a_at, const char* a_key, std::int32_t a_default)
		{
			const auto found = a_at.find(a_key);
			return (found != a_at.end() && found->is_number_integer()) ? found->get<std::int32_t>() : a_default;
		}

		json ToJson(const ModelSettings& a_model)
		{
			json child;
			child["exec"] = a_model.child.exec;
			child["library"] = a_model.child.library;
			child["preload"] = a_model.child.preload;
			child["device"] = a_model.child.device;
			child["beamSize"] = a_model.child.beamSize;
			child["threads"] = a_model.child.threads;
			child["promptPhrases"] = a_model.child.promptPhrases;

			json out;
			out["_"] = "The settings of one model of Speech Broker - Voice Model - Whisper RU. "
			           "This file is PRIVATE to this mod: the contract with the adapter is the C ABI "
			           "in speechbroker-voice-model.h, and nothing outside this DLL reads this file. "
			           "Every relative path is taken from the folder this file is in and may not leave it.";
			out["id"] = a_model.id;
			out["name"] = a_model.name;
			out["enabled"] = a_model.enabled;
			out["language"] = a_model.language;
			out["provides"] = a_model.provides;
			out["class"] = a_model.declaredClass;
			out["finalOnly"] = a_model.finalOnly;
			out["budgetMs"] = a_model.budgetMs;
			out["startupMs"] = a_model.startupMs;
			out["stopMs"] = a_model.stopMs;
			out["maxInFlight"] = a_model.maxInFlight;
			out["weights"] = a_model.weightsRelative;
			out["requireSums"] = a_model.requireSums;
			out["child"] = child;
			return out;
		}

		bool Write(const std::filesystem::path& a_path, const json& a_json)
		{
			std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
			if (!file) {
				return false;
			}
			file << a_json.dump(2) << '\n';
			return file.good();
		}

		ModelSettings Parse(const json& a_json, const ModelSettings& a_fallback,
			const std::filesystem::path& a_source)
		{
			ModelSettings model = a_fallback;
			model.source = a_source;
			model.id = Str(a_json, "id", model.id);
			model.name = Str(a_json, "name", model.name);
			model.language = Str(a_json, "language", model.language);
			model.provides = Str(a_json, "provides", model.provides);
			model.enabled = Bool(a_json, "enabled", model.enabled);
			model.declaredClass = Str(a_json, "class", model.declaredClass);
			model.finalOnly = Bool(a_json, "finalOnly", model.finalOnly);
			model.budgetMs = Int(a_json, "budgetMs", model.budgetMs);
			model.startupMs = Int(a_json, "startupMs", model.startupMs);
			model.stopMs = Int(a_json, "stopMs", model.stopMs);
			model.maxInFlight = Int(a_json, "maxInFlight", model.maxInFlight);
			model.weightsRelative = Str(a_json, "weights", model.weightsRelative);
			model.requireSums = Bool(a_json, "requireSums", model.requireSums);

			if (const auto child = a_json.find("child"); child != a_json.end() && child->is_object()) {
				model.child.exec = Str(*child, "exec", model.child.exec);
				model.child.library = Str(*child, "library", model.child.library);
				model.child.device = Str(*child, "device", model.child.device);
				model.child.beamSize = Int(*child, "beamSize", model.child.beamSize);
				model.child.threads = Int(*child, "threads", model.child.threads);
				model.child.promptPhrases = Int(*child, "promptPhrases", model.child.promptPhrases);
				if (const auto preload = child->find("preload");
					preload != child->end() && preload->is_array()) {
					model.child.preload.clear();
					for (const auto& one : *preload) {
						if (one.is_string()) {
							model.child.preload.push_back(one.get<std::string>());
						}
					}
				}
			}
			return model;
		}
	}

	bool IsContained(const std::filesystem::path& a_root, const std::string& a_relative,
		std::filesystem::path& a_resolved)
	{
		if (a_relative.empty()) {
			return false;
		}
		std::filesystem::path relative(a_relative);
		if (relative.is_absolute() || relative.has_root_name()) {
			return false;
		}
		// lexically_normal collapses "a/../.." into something that can be
		// compared; the string search alone would miss "a/b/../../.." and the
		// comparison alone would miss nothing but is worth doing on the
		// normalised form rather than on what was typed.
		const auto combined = (a_root / relative).lexically_normal();
		const auto root = a_root.lexically_normal();
		const auto rootText = root.native();
		const auto combinedText = combined.native();
		if (combinedText.size() <= rootText.size() || combinedText.compare(0, rootText.size(), rootText) != 0) {
			return false;
		}
		const auto separator = combinedText[rootText.size()];
		if (separator != L'\\' && separator != L'/') {
			return false;
		}
		a_resolved = combined;
		return true;
	}

	Settings LoadSettings(const std::filesystem::path& a_root)
	{
		Settings settings;
		settings.root = a_root;

		std::error_code ec;
		std::filesystem::create_directories(a_root, ec);

		std::vector<std::filesystem::path> files;
		for (const auto& entry : std::filesystem::directory_iterator(a_root, ec)) {
			if (!entry.is_regular_file(ec)) {
				continue;
			}
			const auto& path = entry.path();
			if (path.extension() != ".json") {
				continue;
			}
			files.push_back(path);
		}
		std::sort(files.begin(), files.end());

		if (files.empty()) {
			// The first run. Two files, and then we read them back rather than
			// using the structs we just wrote: if writing failed, the log has to
			// say so now, not at the next launch.
			const auto turbo = a_root / "whisper-ru.json";
			const auto small = a_root / "whisper-ru-small.json";
			if (Write(turbo, ToJson(DefaultTurbo())) && Write(small, ToJson(DefaultSmall()))) {
				Log::Info("$SBWHISPERRU_LOG_SETTINGS_WRITTEN", a_root.string());
				files.push_back(small);
				files.push_back(turbo);
				std::sort(files.begin(), files.end());
			} else {
				Log::Error("$SBWHISPERRU_LOG_SETTINGS_UNWRITABLE", a_root.string());
				settings.models.push_back(DefaultTurbo());
				settings.models.push_back(DefaultSmall());
				return settings;
			}
		}

		for (const auto& path : files) {
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				Log::Warn("$SBWHISPERRU_LOG_SETTINGS_UNREADABLE", path.string());
				continue;
			}
			json parsed;
			try {
				parsed = json::parse(file, nullptr, true, true);  // comments allowed
			} catch (const std::exception& e) {
				Log::Warn("$SBWHISPERRU_LOG_SETTINGS_BROKEN", path.string(), e.what());
				continue;
			}
			if (!parsed.is_object()) {
				Log::Warn("$SBWHISPERRU_LOG_SETTINGS_BROKEN", path.string(), "not an object");
				continue;
			}

			// One file may also carry the settings of the shim itself. They are
			// read from whichever file states them; a shim that demanded a third
			// file would be a third thing to install wrongly.
			settings.language = Str(parsed, "shimLanguage", settings.language);
			settings.logLevel = Str(parsed, "logLevel", settings.logLevel);

			const auto id = Str(parsed, "id", {});
			const auto fallback = (id == "whisper-ru-small") ? DefaultSmall() : DefaultTurbo();
			auto model = Parse(parsed, fallback, path);
			if (model.id.empty()) {
				Log::Warn("$SBWHISPERRU_LOG_SETTINGS_NO_ID", path.string());
				continue;
			}
			const auto twice = std::find_if(settings.models.begin(), settings.models.end(),
				[&model](const ModelSettings& a_other) { return a_other.id == model.id; });
			if (twice != settings.models.end()) {
				// The adapter would answer DUPLICATE and the second registration
				// would be refused with a line in ITS log. Saying it here as well
				// costs one line and points at the two files by name.
				Log::Warn("$SBWHISPERRU_LOG_SETTINGS_TWICE", model.id, twice->source.string(), path.string());
				continue;
			}
			settings.models.push_back(std::move(model));
		}
		return settings;
	}
}
