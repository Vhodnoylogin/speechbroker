#pragma once

#include "envoy-loc.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <filesystem>
#include <string>
#include <utility>

namespace Voice
{
	// The text of the adapter, which is to say its log: it has no window and no
	// notices of its own.
	//
	// Not one line is written in the code. A line is a key, and the text behind it
	// comes out of Interface\Translations\EnvoyVoiceAdapter_<language>.txt - the
	// same mechanism and the same format as everywhere else in Envoy; the reading
	// is done by envoy-loc.h out of the SDK of the bridge.
	//
	// The adapter keeps a table of its own rather than borrowing one. It is a DLL
	// of its own, and it writes its first lines before it has even met the bridge:
	// the settings are read at load, and a mistake in them has to be legible.
	//
	// The one thing that is never translated is the recognised speech. What a
	// person said is data, not a message; it goes into the log as it came.
	//
	// Placeholders are numbered, {0} and {1} rather than a bare {}, because another
	// language puts the words in another order.
	class Loc
	{
	public:
		// Called once at load. a_language is the language the game runs in, in
		// Bethesda's spelling; empty or "auto" is resolved by the caller.
		static void Load(const std::filesystem::path& a_dir, const std::string& a_language);

		static const char*       Get(const char* a_key);
		static const std::string& Language();

		// One line of the log, by key. A pattern comes out of a file anybody may
		// edit, so a wrong number of placeholders in it is a question of when and
		// not of whether. It must not silence the line: a broken pattern falls back
		// to the bare key, because an adapter that cannot be diagnosed is worse than
		// one whose log reads badly.
		template <class... Args>
		static void Say(spdlog::level::level_enum a_level, const char* a_key, Args&&... a_args)
		{
			try {
				spdlog::log(a_level, fmt::runtime(Get(a_key)), std::forward<Args>(a_args)...);
			} catch (const std::exception&) {
				spdlog::log(a_level, "{}", a_key);
			}
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
