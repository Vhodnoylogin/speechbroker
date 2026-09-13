#include "Log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>

namespace Envoy
{
	namespace
	{
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

	void Log::Init(std::string_view a_level, const std::filesystem::path& a_file)
	{
		if (a_file.empty()) {
			ToConsole(a_level);
			return;
		}

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(a_file.string(), true);
		Adopt(std::make_shared<spdlog::logger>("global log", std::move(sink)), a_level);
	}

	void Log::ToConsole(std::string_view a_level)
	{
		Adopt(spdlog::stdout_color_mt("global log"), a_level);
	}
}
