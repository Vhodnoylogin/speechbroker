#pragma once

#include "Loc.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace SpeechBroker
{
	// The bridge's log. The level comes from the settings; it is not in the code.
	//
	// Where to write is decided by whoever starts the core: in the game that is
	// SKSE's log folder, outside it a folder next to the test host. The core knows
	// no paths and writes through spdlog, which by then already has somewhere to
	// put things.
	//
	// The log is steered while the game runs: from the in-game menu and by
	// SpeechBroker.ReloadSettings. That is why the level and the permission to write the
	// player's words live here and not only in the settings file - otherwise the
	// menu would change one thing and the log would obey another.
	//
	// The lines themselves are translated like everything else the module puts
	// out. Not one of them is written in the code: a line is a key, and the text
	// behind it lives in the same translation files the window on screen uses.
	// The one thing that stays as it came is the recognised speech - translating
	// what a person said is meaningless, it is data and not a message.
	//
	// The placeholders are numbered, {0} and {1} rather than a bare {}, because
	// another language puts the words in another order and a translator has to be
	// able to move them.
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

		// One line of the log, by key.
		//
		// A pattern comes out of a file anybody may edit, so a wrong number of
		// placeholders in it is a question of when and not of whether. It must not
		// silence the line: a mod that cannot be diagnosed is worse than one whose
		// log reads badly, so a broken pattern falls back to the bare key.
		template <class... Args>
		static void Say(spdlog::level::level_enum a_level, const char* a_key, Args&&... a_args)
		{
			try {
				spdlog::log(a_level, fmt::runtime(Loc::Get(a_key)), std::forward<Args>(a_args)...);
			} catch (const std::exception&) {
				spdlog::log(a_level, "{}", a_key);
			}
		}

		template <class... Args>
		static void Trace(const char* a_key, Args&&... a_args)
		{
			Say(spdlog::level::trace, a_key, std::forward<Args>(a_args)...);
		}

		template <class... Args>
		static void Debug(const char* a_key, Args&&... a_args)
		{
			Say(spdlog::level::debug, a_key, std::forward<Args>(a_args)...);
		}

		template <class... Args>
		static void Info(const char* a_key, Args&&... a_args)
		{
			Say(spdlog::level::info, a_key, std::forward<Args>(a_args)...);
		}

		template <class... Args>
		static void Warn(const char* a_key, Args&&... a_args)
		{
			Say(spdlog::level::warn, a_key, std::forward<Args>(a_args)...);
		}

		template <class... Args>
		static void Error(const char* a_key, Args&&... a_args)
		{
			Say(spdlog::level::err, a_key, std::forward<Args>(a_args)...);
		}
	};
}
