// The text of the shim, which is to say its log: it has no window and shows the
// player nothing.
//
// NOT ONE LINE IS WRITTEN IN THE CODE. A line is a key, and the text behind it
// comes out of localization/speechbrokermodelwhisperru.<language>.txt, a
// tab-separated key<TAB>text file shipped inside the mod. Placeholders are
// numbered - {0}, {1} - because another language puts the words in another
// order.
//
// WHY THIS MODULE READS ITS OWN TABLE instead of going through the engine's
// translation files the way the bridge and the adapter do. Those two turn their
// tables into Interface\Translations\*.txt with a script the BRIDGE publishes in
// its SDK, and the engine renders them for text on screen. A model mod depends
// on the adapter and not on the bridge, it shows nothing on screen, and making
// it build against the bridge's SDK to render a log line would add a dependency
// that exists for no other reason. The file format is the same one, so a
// translator meets nothing new.
//
// THE TWO DESTINATIONS ARE NOT THE SAME LINE, and that is the contract's doing.
// What goes into the ADAPTER'S log goes as a key and its arguments, verbatim and
// unrendered: the adapter neither parses nor translates a model's key, it writes
// down the id, then the key, then the arguments, tab separated. What goes into
// OUR OWN file is rendered here, because nobody else will render it. So a line
// worth telling the adapter about is sent both ways and the two look different
// on purpose - see ModelShim::Say.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace WhisperRu::Log
{
	// The same four the contract uses, with the same numbers, so that a level
	// crossing to Host::Log needs no translation.
	inline constexpr std::int32_t kDebug = 0;
	inline constexpr std::int32_t kInfo = 1;
	inline constexpr std::int32_t kWarn = 2;
	inline constexpr std::int32_t kError = 3;

	// a_language is Bethesda's spelling - "english", "russian". A table that is
	// not there is not an error: the keys are then written bare, which is ugly
	// and perfectly diagnosable, and a shim that fell silent because a text file
	// was missing would be worse in every way.
	void LoadTable(const std::filesystem::path& a_folder, const std::string& a_name,
		const std::string& a_language);

	// Where our own lines go. Opening it is the whole of the sink policy: if it
	// cannot be opened we say nothing and carry on, because a log that takes the
	// program down with it is not a log.
	void OpenFile(const std::filesystem::path& a_path);

	void SetLevel(const std::string& a_level);

	// The text behind a key, or the key itself when there is none.
	const std::string& Text(const std::string& a_key);

	// Renders {0}, {1}, ... out of a_args. A placeholder with no argument is
	// left as it stands rather than silencing the line - a table anybody may
	// edit will eventually have the wrong number of them, and the question is
	// only what happens then.
	std::string Render(const std::string& a_key, const std::vector<std::string>& a_args);

	void Say(std::int32_t a_level, const std::string& a_key, const std::vector<std::string>& a_args);

	// ------------------------------------------------------------ convenience
	// Arguments arrive as whatever they are and leave as strings, because that
	// is what crosses to the adapter: Host::Log takes const char* const*, so a
	// number has to become text on this side anyway.
	std::string ToText(const std::string& a_value);
	std::string ToText(const char* a_value);
	std::string ToText(const std::filesystem::path& a_value);
	std::string ToText(std::int32_t a_value);
	std::string ToText(std::uint32_t a_value);
	std::string ToText(std::int64_t a_value);
	std::string ToText(double a_value);
	std::string ToText(bool a_value);

	template <class... Args>
	std::vector<std::string> Pack(Args&&... a_args)
	{
		return { ToText(std::forward<Args>(a_args))... };
	}

	template <class... Args>
	void Debug(const std::string& a_key, Args&&... a_args)
	{
		Say(kDebug, a_key, Pack(std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Info(const std::string& a_key, Args&&... a_args)
	{
		Say(kInfo, a_key, Pack(std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Warn(const std::string& a_key, Args&&... a_args)
	{
		Say(kWarn, a_key, Pack(std::forward<Args>(a_args)...));
	}

	template <class... Args>
	void Error(const std::string& a_key, Args&&... a_args)
	{
		Say(kError, a_key, Pack(std::forward<Args>(a_args)...));
	}
}
