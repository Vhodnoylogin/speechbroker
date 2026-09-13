#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace Envoy
{
	// Every line of text a player can read.
	//
	// The log is deliberately not here. Its lines travel into other people's bug
	// reports, and a translated log cannot be read by the author who has to
	// answer them; so the log stays English and the screen gets translated.
	//
	// The store is Skyrim's own translation file - Interface\Translations\
	// Envoy_<language>.txt, UTF-16LE, one "$KEY<tab>text" line each. Choosing the
	// engine's format rather than inventing one buys two things. The game resolves
	// $-prefixed strings in Papyrus and in menus out of that same file, so one
	// file serves both the window we draw ourselves and the notifications the
	// engine draws for us. And a translation becomes an ordinary mod with a single
	// folder in it: nothing to compile, nothing to patch, and the load order
	// decides which one wins, exactly as with any other asset.
	//
	// Three layers are laid over each other, each allowed to override the last:
	// the table compiled into the plugin, then every English file, then every file
	// of the language in force. A half-finished translation therefore shows its
	// own lines where it has them and English where it has not, instead of turning
	// the window into a list of keys.
	//
	// Files are picked up by name: Envoy*_<language>.txt. A mod that wants its own
	// strings in this table - the demo subscriber does - names its file that way
	// and they appear. Reading every translation file in the folder would mean
	// holding the whole build's text in memory for the sake of six lines.
	//
	// Loading happens once, while the plugin loads and no other thread of ours is
	// running yet. Changing the language needs the game restarted, which is what
	// the engine demands of itself as well; ReloadSettings deliberately leaves the
	// table alone, so the lines are fixed for the life of the process and cost no
	// lock to read.
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
