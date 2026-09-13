#pragma once

#include <filesystem>
#include <string_view>

namespace Envoy
{
	// Журнал моста. Уровень берётся из конфигурации, в коде его нет.
	//
	// Куда писать, решает тот, кто заводит ядро: в игре это папка журналов SKSE,
	// вне игры - папка рядом с хостом проверки. Само ядро путей не знает и
	// пишет через spdlog, у которого к этому времени уже есть куда.
	class Log
	{
	public:
		static void Init(std::string_view a_level, const std::filesystem::path& a_file);

		// Без файла - только на экран. Так удобнее гонять проверки.
		static void ToConsole(std::string_view a_level);
	};
}
