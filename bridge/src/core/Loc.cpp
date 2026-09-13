#include "Loc.h"

#include "LocStrings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	namespace
	{
		std::unordered_map<std::string, std::string> g_strings;
		std::string                                  g_language{ "english" };
		std::size_t                                  g_files{ 0 };

		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_text;
		}

		// The file the game reads is UTF-16LE with a byte order mark, but a
		// translator saving it out of an ordinary editor may well hand us UTF-8.
		// Both are accepted: refusing the second would mean an empty window whose
		// cause is invisible from inside the game.
		std::string ToUtf8(const std::vector<char>& a_bytes)
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

				// A surrogate pair is one character written as two units. Joining
				// them here is what keeps a rare glyph from turning into two pieces
				// of rubbish.
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

		// One line per string, so a line break inside a string has to be written
		// down rather than typed.
		std::string Unescape(std::string_view a_value)
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

		std::size_t Absorb(std::string_view a_text)
		{
			std::size_t taken = 0;
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
				g_strings[std::string{ line.substr(0, tab) }] = Unescape(line.substr(tab + 1));
				++taken;
			}
			return taken;
		}

		void AbsorbFile(const std::filesystem::path& a_path)
		{
			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) {
				spdlog::warn("localisation: cannot open {}", a_path.string());
				return;
			}
			const std::vector<char> bytes{ std::istreambuf_iterator<char>(stream),
				std::istreambuf_iterator<char>() };
			Absorb(ToUtf8(bytes));
		}

		// Envoy*_<language>.txt, compared without regard to case: what a translator
		// names the file on disk is not something we can insist on.
		bool Matches(const std::string& a_name, const std::string& a_suffix)
		{
			const auto lower = Lower(a_name);
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

			// By name, so that two mods bringing files always land in the same
			// order: which of them wins a shared key must not depend on how the file
			// system feels like listing the folder today.
			std::sort(found.begin(), found.end());
			for (const auto& path : found) {
				AbsorbFile(path);
			}
			return found.size();
		}
	}

	void Loc::Load(const std::filesystem::path& a_dir, const std::string& a_language)
	{
		g_strings.clear();
		g_files = 0;
		g_language = a_language.empty() ? std::string{ "english" } : Lower(a_language);

		for (const auto& entry : kDefaultStrings) {
			g_strings[entry.key] = Unescape(entry.text);
		}

		g_files += AbsorbLanguage(a_dir, "english");
		if (g_language != "english") {
			g_files += AbsorbLanguage(a_dir, g_language);
		}

		spdlog::info("localisation: {}, {} strings from {} files in {}", g_language,
			g_strings.size(), g_files, a_dir.string());
		if (g_files == 0) {
			spdlog::warn("localisation: no Envoy*_{}.txt found, running on the built-in English",
				g_language);
		}
	}

	const char* Loc::Get(const char* a_key)
	{
		if (!a_key) {
			return "";
		}
		const auto it = g_strings.find(a_key);
		return it == g_strings.end() ? a_key : it->second.c_str();
	}

	const std::string& Loc::Language()
	{
		return g_language;
	}

	std::size_t Loc::Count()
	{
		return g_strings.size();
	}

	std::size_t Loc::Files()
	{
		return g_files;
	}
}
