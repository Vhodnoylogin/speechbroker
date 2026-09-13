/* Envoy Framework - reading the text a module puts out. Header only.
 *
 * Every line a module shows or writes down is a key, and the text behind the key
 * lives in Skyrim's own translation files: Interface\Translations\Envoy*_<language>.txt,
 * UTF-16LE, one "$KEY<tab>text" line each. Choosing the format of the engine rather
 * than inventing one buys two things. The game resolves $-prefixed strings in Papyrus
 * and in menus out of that same file, so one file serves both the window a plugin
 * draws itself and the notices the engine draws for it. And a translation becomes an
 * ordinary mod with a single folder in it - nothing to compile, nothing to patch, and
 * the load order decides which one wins, exactly as with any other asset.
 *
 * This header is published in the SDK because every part needs it and each of them is
 * a DLL of its own: the bridge cannot lend its table to an adapter, and an adapter
 * writes lines before it has even met the bridge. One implementation in a header
 * costs less than a copy in every module.
 *
 * Three layers are laid over each other, each allowed to override the last: the table
 * compiled into the binary, then every English file, then every file of the language
 * in force. A half-finished translation therefore shows its own lines where it has
 * them and English where it has not, instead of turning into a list of keys.
 *
 * Files are picked up by name: Envoy*_<language>.txt. A mod that wants its own strings
 * in the table names its file that way and they appear. Reading every translation file
 * in the folder would mean holding the whole text of the build in memory for the sake
 * of a few lines.
 *
 * A table is meant to be loaded once, while the plugin loads and no thread of its own
 * is running yet. Changing the language needs the game restarted, which is what the
 * engine demands of itself as well; that is why reading costs no lock.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace EnvoyLoc
{
	// One line of the table compiled into a binary. build-localization.py writes an
	// array of these out of the source language.
	struct Entry
	{
		const char* key;
		const char* text;
	};

	namespace detail
	{
		inline std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_text;
		}

		// The file the game reads is UTF-16LE with a byte order mark, but a translator
		// saving it out of an ordinary editor may well hand over UTF-8. Both are
		// accepted: refusing the second would mean empty text whose cause is invisible
		// from inside the game.
		inline std::string ToUtf8(const std::vector<char>& a_bytes)
		{
			const auto size = a_bytes.size();
			const auto byte = [&a_bytes](std::size_t i) {
				return static_cast<std::uint32_t>(static_cast<unsigned char>(a_bytes[i]));
			};

			if (size >= 3 && byte(0) == 0xEF && byte(1) == 0xBB && byte(2) == 0xBF) {
				return std::string(a_bytes.begin() + 3, a_bytes.end());
			}
			if (size < 2 || byte(0) != 0xFF || byte(1) != 0xFE) {
				return std::string(a_bytes.begin(), a_bytes.end());
			}

			std::string out;
			out.reserve(size / 2);
			for (std::size_t i = 2; i + 1 < size; i += 2) {
				std::uint32_t unit = byte(i) | (byte(i + 1) << 8);

				// A surrogate pair is one character written as two units. Joining them
				// here is what keeps a rare glyph from turning into two pieces of
				// rubbish.
				if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < size) {
					const std::uint32_t low = byte(i + 2) | (byte(i + 3) << 8);
					if (low >= 0xDC00 && low <= 0xDFFF) {
						unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
						i += 2;
					}
				}

				if (unit < 0x80) {
					out.push_back(static_cast<char>(unit));
				} else if (unit < 0x800) {
					out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
					out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
				} else if (unit < 0x10000) {
					out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
					out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
				} else {
					out.push_back(static_cast<char>(0xF0 | (unit >> 18)));
					out.push_back(static_cast<char>(0x80 | ((unit >> 12) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
				}
			}
			return out;
		}

		// One line per string, so a line break inside a string has to be written down
		// rather than typed.
		inline std::string Unescape(std::string_view a_value)
		{
			std::string out;
			out.reserve(a_value.size());
			for (std::size_t i = 0; i < a_value.size(); ++i) {
				if (a_value[i] == 0x5C && i + 1 < a_value.size()) {
					const auto next = a_value[i + 1];
					if (next == 'n') {
						out.push_back('\n');
						++i;
						continue;
					}
					if (next == 't') {
						out.push_back('\t');
						++i;
						continue;
					}
					if (next == 0x5C) {
						out.push_back(static_cast<char>(0x5C));
						++i;
						continue;
					}
				}
				out.push_back(a_value[i]);
			}
			return out;
		}
	}

	// The strings of one module. Made once and read from anywhere afterwards.
	class Table
	{
	public:
		// a_dir is the Interface\Translations folder; a_language is the language the
		// game runs in, in Bethesda's spelling ("english", "russian"). The baseline is
		// the table compiled into the binary - what is shown when no file is found.
		void Load(const std::filesystem::path& a_dir, const std::string& a_language,
			const Entry* a_baseline = nullptr, std::size_t a_baselineCount = 0)
		{
			_strings.clear();
			_files = 0;
			_language = a_language.empty() ? std::string{ "english" } : detail::Lower(a_language);

			for (std::size_t i = 0; i < a_baselineCount; ++i) {
				_strings[a_baseline[i].key] = detail::Unescape(a_baseline[i].text);
			}

			_files += AbsorbLanguage(a_dir, "english");
			if (_language != "english") {
				_files += AbsorbLanguage(a_dir, _language);
			}
		}

		// An unknown key comes back as itself. A missing line has to be visible: an
		// empty string turns a forgotten key into a blank nobody ever reports.
		[[nodiscard]] const char* Get(const char* a_key) const
		{
			if (!a_key) {
				return "";
			}
			const auto it = _strings.find(a_key);
			return it == _strings.end() ? a_key : it->second.c_str();
		}

		// The language actually in force, which is not always the one asked for.
		[[nodiscard]] const std::string& Language() const { return _language; }

		// How many lines are loaded, and out of how many files. Both belong in the
		// log: "0 files" is the difference between a broken translation and one that
		// was never installed.
		[[nodiscard]] std::size_t Count() const { return _strings.size(); }
		[[nodiscard]] std::size_t Files() const { return _files; }

	private:
		void Absorb(std::string_view a_text)
		{
			std::size_t from = 0;
			while (from <= a_text.size()) {
				auto to = a_text.find('\n', from);
				if (to == std::string_view::npos) {
					to = a_text.size();
				}
				auto line = a_text.substr(from, to - from);
				from = to + 1;

				if (!line.empty() && line.back() == '\r') {
					line.remove_suffix(1);
				}
				if (line.empty() || line.front() != '$') {
					continue;  // notes, blank lines, anything that is not a key
				}
				const auto tab = line.find('\t');
				if (tab == std::string_view::npos) {
					continue;
				}
				_strings[std::string{ line.substr(0, tab) }] = detail::Unescape(line.substr(tab + 1));
			}
		}

		void AbsorbFile(const std::filesystem::path& a_path)
		{
			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) {
				return;
			}
			const std::vector<char> bytes{ std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>() };
			Absorb(detail::ToUtf8(bytes));
		}

		// Envoy*_<language>.txt, compared without regard to case: what a translator
		// names the file on disk is not something we can insist on.
		static bool Matches(const std::string& a_name, const std::string& a_suffix)
		{
			const auto lower = detail::Lower(a_name);
			return lower.size() > a_suffix.size() + 5 && lower.compare(0, 5, "envoy") == 0 &&
			       lower.compare(lower.size() - a_suffix.size(), a_suffix.size(), a_suffix) == 0;
		}

		std::size_t AbsorbLanguage(const std::filesystem::path& a_dir, const std::string& a_language)
		{
			const auto                         suffix = "_" + a_language + ".txt";
			std::vector<std::filesystem::path> found;

			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(a_dir, ec)) {
				if (entry.is_regular_file(ec) && Matches(entry.path().filename().string(), suffix)) {
					found.push_back(entry.path());
				}
			}

			// By name, so that two mods bringing files always land in the same order:
			// which of them wins a shared key must not depend on how the file system
			// feels like listing the folder today.
			std::sort(found.begin(), found.end());
			for (const auto& path : found) {
				AbsorbFile(path);
			}
			return found.size();
		}

		std::unordered_map<std::string, std::string> _strings;
		std::string                                  _language{ "english" };
		std::size_t                                  _files{ 0 };
	};
}
