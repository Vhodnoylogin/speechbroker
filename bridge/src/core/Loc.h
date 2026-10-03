#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace SpeechBroker
{
	// The text of the bridge: the window on screen, the notices, and the log.
	//
	// Not one line is written in the code. A line is a key, and the text behind it
	// comes out of the translation files - how that works and why the format is
	// Skyrim's own is described in contract/speechbroker-loc.h, which does the reading and
	// which the SDK publishes so that an adapter can do exactly the same.
	//
	// The one thing that is never translated is the recognised speech: what a
	// person said is data, not a message, and it stays in the language it was said
	// in wherever it appears.
	//
	// This is the table of the bridge, seeded with the set compiled into the
	// plugin. It is loaded once, while the plugin loads and no thread of ours is
	// running yet; ReloadSettings deliberately leaves it alone, so the lines are
	// fixed for the life of the process and cost no lock to read.
	class Loc
	{
	public:
		// a_dir is the Interface\Translations folder; a_language is the language
		// the game runs with, in Bethesda's spelling ("english", "russian").
		static void Load(const std::filesystem::path& a_dir, const std::string& a_language);

		// An unknown key comes back as itself. A missing line has to be visible on
		// screen: answering with an empty string turns a forgotten key into a blank
		// window that nobody ever reports.
		//
		// Takes and returns a plain pointer because both callers - ImGui and
		// Papyrus - hand over strings that are already null-terminated and stay
		// alive, and because the answer is read every frame.
		static const char* Get(const char* a_key);

		// The language actually in force, which is not always the one asked for.
		static const std::string& Language();

		// How many lines are loaded, and out of how many files. Both go to the log:
		// "0 files" is the difference between a broken translation and one that was
		// never installed.
		static std::size_t Count();
		static std::size_t Files();
	};
}
