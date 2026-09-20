#include "GameLanguage.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>

namespace SpeechBroker
{
	namespace
	{
		std::string Lower(std::string a_text)
		{
			std::transform(a_text.begin(), a_text.end(), a_text.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return a_text;
		}

		std::string_view Trim(std::string_view a_text)
		{
			const auto space = [](char c) {
				return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"';
			};
			while (!a_text.empty() && space(a_text.front())) {
				a_text.remove_prefix(1);
			}
			while (!a_text.empty() && space(a_text.back())) {
				a_text.remove_suffix(1);
			}
			return a_text;
		}
	}

	std::string GameLanguage::FromIniText(std::string_view a_text)
	{
		std::string section;
		std::string answer;

		std::size_t at = 0;
		while (at <= a_text.size()) {
			const auto end = a_text.find('\n', at);
			auto       line = Trim(a_text.substr(at, end == std::string_view::npos ? end : end - at));
			at = (end == std::string_view::npos) ? a_text.size() + 1 : end + 1;

			// A comment is the rest of the line, and an ini of a player's is full
			// of them - including commented-out copies of this very key.
			if (const auto hash = line.find_first_of(";#"); hash != std::string_view::npos) {
				line = Trim(line.substr(0, hash));
			}
			if (line.empty()) {
				continue;
			}
			if (line.front() == '[') {
				const auto close = line.find(']');
				section = Lower(std::string(Trim(line.substr(1, close == std::string_view::npos ? close : close - 1))));
				continue;
			}

			const auto equals = line.find('=');
			if (equals == std::string_view::npos) {
				continue;
			}
			const auto key = Lower(std::string(Trim(line.substr(0, equals))));
			if (key != "slanguage") {
				continue;
			}
			// [General] is where it belongs, but an ini written by hand may have
			// lost the section header; a key with this name means the same thing
			// wherever it stands, and taking it is better than ignoring it.
			if (!section.empty() && section != "general") {
				continue;
			}

			const auto value = Trim(line.substr(equals + 1));
			if (!value.empty()) {
				answer = Lower(std::string(value));  // the LAST one in the file wins
			}
		}

		return answer;
	}

	std::string GameLanguage::FromIniFile(const std::filesystem::path& a_path)
	{
		std::ifstream file(a_path, std::ios::binary);
		if (!file) {
			return {};
		}
		const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		return FromIniText(text);
	}

	std::string GameLanguage::Of(const std::filesystem::path& a_besideTheExe,
		const std::filesystem::path& a_inDocuments)
	{
		const auto beside = FromIniFile(a_besideTheExe);
		const auto mine = FromIniFile(a_inDocuments);
		return mine.empty() ? beside : mine;
	}
}
