#include "Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>

namespace SpeechBroker
{
	namespace
	{
		// The player's words are read from the adapter's threads and switched from
		// the game's thread: the value has to be atomic or it is a race.
		std::atomic_bool g_showSpeech{ false };

		spdlog::level::level_enum ToLevel(std::string_view a_level)
		{
			if (a_level == "trace") {
				return spdlog::level::trace;
			}
			if (a_level == "debug") {
				return spdlog::level::debug;
			}
			if (a_level == "warning" || a_level == "warn") {
				return spdlog::level::warn;
			}
			if (a_level == "error") {
				return spdlog::level::err;
			}
			return spdlog::level::info;
		}

		void Adopt(std::shared_ptr<spdlog::logger> a_logger, std::string_view a_level)
		{
			const auto level = ToLevel(a_level);
			a_logger->set_level(level);
			a_logger->flush_on(level);
			spdlog::set_default_logger(std::move(a_logger));
		}
	}

	void Log::Init(std::string_view a_level, const std::filesystem::path& a_file, int a_maxSizeKb)
	{
		if (a_file.empty()) {
			ToConsole(a_level);
			return;
		}

		spdlog::sink_ptr sink;
		if (a_maxSizeKb > 0) {
			// Two files, not ten: more than that is wanted when digging into an
			// old fault, and a mod's log is read while the trail is fresh.
			sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
				a_file.string(), static_cast<std::size_t>(a_maxSizeKb) * 1024, 1);
		} else {
			sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(a_file.string(), true);
		}
		Adopt(std::make_shared<spdlog::logger>("global log", std::move(sink)), a_level);
	}

	void Log::ToConsole(std::string_view a_level)
	{
		Adopt(spdlog::stdout_color_mt("global log"), a_level);
	}

	void Log::SetLevel(std::string_view a_level)
	{
		auto logger = spdlog::default_logger();
		if (!logger) {
			return;
		}
		const auto level = ToLevel(a_level);
		logger->set_level(level);
		logger->flush_on(level);
	}

	std::string Log::Level()
	{
		auto logger = spdlog::default_logger();
		if (!logger) {
			return "info";
		}
		switch (logger->level()) {
		case spdlog::level::trace:
			return "trace";
		case spdlog::level::debug:
			return "debug";
		case spdlog::level::warn:
			return "warning";
		case spdlog::level::err:
			return "error";
		default:
			return "info";
		}
	}

	bool Log::ShowSpeech()
	{
		return g_showSpeech.load(std::memory_order_relaxed);
	}

	void Log::SetShowSpeech(bool a_show)
	{
		g_showSpeech.store(a_show, std::memory_order_relaxed);
	}
}
