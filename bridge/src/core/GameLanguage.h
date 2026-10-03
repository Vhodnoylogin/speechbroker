#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace SpeechBroker
{
	// WHICH LANGUAGE THE GAME IS RUNNING IN, and why it is not a one-line question.
	//
	// The bridge needs it for one purpose only, and that purpose is not decoration:
	// a subscriber's vocabulary is words, and the words have to be in the language
	// the player actually speaks to the microphone. Ask for the wrong language and
	// the table loads a neighbouring file, the subscriber registers "close the
	// door", and a Russian game listens for English all evening.
	//
	// RE::INISettingCollection was the only source here until 20.09.2026 and it
	// answered nothing in Skyrim VR: the bridge fell back to English while the
	// game itself was reading Interface\Translations\*_RUSSIAN.txt. So the files
	// are read the way the ENGINE reads them - the one beside the executable
	// first, the player's own in Documents after it - with ordinary file reads,
	// which work under Mod Organizer exactly as the settings read does.
	class GameLanguage
	{
	public:
		// sLanguage out of the text of one ini, lower-cased, or empty when the text
		// says nothing about it. Pure on purpose: it is checked without a game.
		static std::string FromIniText(std::string_view a_text);

		// The same out of a file. An absent file is not an error - most machines
		// have only one of the two.
		static std::string FromIniFile(const std::filesystem::path& a_path);

		// The answer for a pair of files, in Bethesda's own spelling brought down
		// to lower case ("russian", "english"). The second file wins when both
		// name a language, because that is the order the engine applies them in.
		// Empty when neither says anything - the caller decides what to do then,
		// and the caller has a default.
		static std::string Of(const std::filesystem::path& a_besideTheExe,
			const std::filesystem::path& a_inDocuments);
	};
}
