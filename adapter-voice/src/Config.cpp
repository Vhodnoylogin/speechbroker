#include "Config.h"

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
		constexpr auto kHome = LR"(Data\SKSE\Plugins\envoy\adapters\voice)";
		constexpr auto kConfigPath = LR"(Data\SKSE\Plugins\envoy\adapters\voice\envoy-voice.json)";

		// Папка, куда каждый мод-модель кладёт свой листок. Имя файла значения
		// не имеет - важен только ключ id внутри; папка читается в порядке имён,
		// чтобы список моделей не зависел от того, как их вернула файловая
		// система, и два запуска давали один и тот же порядок.
		constexpr auto kModelsDir = LR"(Data\SKSE\Plugins\envoy\adapters\voice\models)";

		// Путь из настроек, если он относительный, считается от папки адаптера,
		// и выйти за неё нельзя. Адаптер запускает СВОЮ службу, которая едет
		// внутри его же мода; возможность указать сюда что угодно означала бы,
		// что подменивший файл настроек запускает у игрока любую программу.
		//
		// Пустая строка на выходе означает отказ.
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
			// lexically_relative даёт ".." в начале ровно тогда, когда путь ушёл
			// выше корня. Сравнение строк здесь не годится: "voice-evil"
			// начинается с "voice".
			const auto rel = full.lexically_relative(root);
			if (rel.empty() || *rel.begin() == "..") {
				return {};
			}
			return full.string();
		}

		// Служба обязана жить на этой же машине. Через неё проходит всё, что
		// игрок говорит и слышит; чужой хост в настройках означал бы, что
		// установка голосового мода молча включает пересылку сказанного наружу.
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

		// Секрет на сессию. Случайность нужна не ради стойкости шифра, а ради
		// того, чтобы значение нельзя было угадать заранее и зашить в чужую
		// программу, занявшую порт.
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
				SKSE::log::error("служба: exec «{}» выходит за папку адаптера - запускать не буду",
					exec);
				return std::nullopt;
			}

			const auto dir = a_doc.value("workingDir", std::string{});
			out.workingDir = ResolveInside(dir, home);
			if (!dir.empty() && out.workingDir.empty()) {
				SKSE::log::error("служба: workingDir «{}» выходит за папку адаптера", dir);
				return std::nullopt;
			}

			out.parentPidArg = a_doc.value("parentPidArg", out.parentPidArg);
			out.waitSec = a_doc.value("waitSec", out.waitSec);
			out.pollSec = a_doc.value("pollSec", out.pollSec);
			// Доводы не трогаем. Их разрешает сама служба от своей рабочей
			// папки, а среди них бывают не пути вовсе: "--port", "8931".
			for (const auto& arg : a_doc.value("args", nlohmann::json::array())) {
				out.args.push_back(arg.get<std::string>());
			}
			return out;
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

			// Что модель умеет, она объявляет сама. Умолчание - только слух:
			// распознавание есть у всякой модели, ради которой этот адаптер
			// написан, а озвучка - нет.
			const auto provides = a_doc.value("provides", std::string{ "asr" });
			out.hears = provides.find("asr") != std::string::npos;
			out.speaks = provides.find("tts") != std::string::npos;
			return out;
		}

		// Читает папку моделей. Отказ одного листка не отменяет остальных:
		// листок привозит ЧУЖОЙ мод, и его ошибка не должна лишать человека
		// моделей, которые в порядке. Своим файлом настроек адаптер по-прежнему
		// строг - там ошибка наша.
		//
		// Веса моделей адаптер не читает и не проверяет: их грузит служба,
		// и правило «веса лежат внутри своего мода» стережёт она же. Двух
		// проверяющих у одного правила быть не должно.
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
						SKSE::log::error("модель из {}: нет ключа id - пропускаю",
							file.filename().string());
						continue;
					}
					const auto twin = std::find_if(out.begin(), out.end(),
						[&](const Model& a_seen) { return a_seen.id == model.id; });
					if (twin != out.end()) {
						SKSE::log::error("модель {} объявлена дважды: {} и {} - беру первую",
							model.id, twin->source, model.source);
						continue;
					}
					out.push_back(std::move(model));
				} catch (const std::exception& e) {
					SKSE::log::error("модель из {} не разобрана: {} - пропускаю",
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
			SKSE::log::error("нет файла настроек: {}", std::filesystem::path{ kConfigPath }.string());
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
			self.correlateMs = doc.value("correlateMs", self.correlateMs);
			self.retryDelayMs = doc.value("retryDelayMs", self.retryDelayMs);
			self.healthTimeoutSec = doc.value("healthTimeoutSec", self.healthTimeoutSec);
			self.listenGraceSec = doc.value("listenGraceSec", self.listenGraceSec);
			self.idleSleepMs = doc.value("idleSleepMs", self.idleSleepMs);
			self.sayTimeoutSec = doc.value("sayTimeoutSec", self.sayTimeoutSec);
			self.idMapLimit = doc.value("idMapLimit", self.idMapLimit);
		} catch (const std::exception& e) {
			SKSE::log::error("настройки не разобраны: {}", e.what());
			return false;
		}

		if (!Loopback(self.service.url)) {
			SKSE::log::error("служба по адресу «{}» ведёт не на эту машину - работать не буду",
				self.service.url);
			return false;
		}

		self.models = ReadModels();

		// Способности адаптер не объявляет, а считает: обещать мосту озвучку,
		// когда ни одна установленная модель не говорит, значит забрать работу
		// у адаптера, который её умеет. Прежде это была строка в файле
		// настроек - то есть обещание, ничем не подкреплённое.
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
