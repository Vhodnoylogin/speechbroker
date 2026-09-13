#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Envoy
{
	// The bridge's log. The level comes from the settings; it is not in the code.
	//
	// Where to write is decided by whoever starts the core: in the game that is
	// SKSE's log folder, outside it a folder next to the test host. The core knows
	// no paths and writes through spdlog, which by then already has somewhere to
	// put things.
	//
	// The log is steered while the game runs: from the in-game menu and by
	// Envoy.ReloadSettings. That is why the level and the permission to write the
	// player's words live here and not only in the settings file - otherwise the
	// menu would change one thing and the log would obey another.
	//
	// The lines themselves stay English and are not translated. They travel into
	// other people's bug reports, and a log in a language the author cannot read
	// is a log nobody can answer.
	class Log
	{
	public:
		// The size limit is in kilobytes; zero means "do not rotate".
		// Without rotation the file grows without bound over a long session: the
		// log.maxSizeKb key had been in the reference settings from the start and
		// was read by nobody, which is to say it promised what it did not do.
		static void Init(std::string_view a_level, const std::filesystem::path& a_file,
			int a_maxSizeKb = 0);

		// No file, screen only. Handier for running checks.
		static void ToConsole(std::string_view a_level);

		// Change the level of a log that is already running. Takes effect at once
		// and until the end of the session; the lasting value is set in the
		// settings file.
		static void SetLevel(std::string_view a_level);

		// The current level as a word - the menu shows it.
		static std::string Level();

		// Whether the player's own words may go into the log. Not by default:
		// otherwise a transcript of everything a person said aloud at home piles up
		// in the log folder, and they were warned about it nowhere.
		static bool ShowSpeech();
		static void SetShowSpeech(bool a_show);
	};
}
