#include "Text.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace Envoy
{
	// Code points, not bytes. Cyrillic takes two bytes in UTF-8, std::tolower does
	// not touch them, and an edit distance over bytes prices a capital letter by
	// which half of the block it sits in: one Russian capital cost 1 byte out of
	// 23 and another cost 2 out of 19. Case-insensitivity did not work for Russian
	// at all, and every threshold meant something other than it seemed to.
	std::u32string Text::Decode(const std::string& a_text)
	{
		std::u32string out;
		out.reserve(a_text.size());
		for (std::size_t i = 0; i < a_text.size();) {
			const auto lead = static_cast<unsigned char>(a_text[i]);
			std::size_t extra = 0;
			char32_t    point = lead;
			if (lead >= 0xF0) {
				extra = 3;
				point = lead & 0x07u;
			} else if (lead >= 0xE0) {
				extra = 2;
				point = lead & 0x0Fu;
			} else if (lead >= 0xC0) {
				extra = 1;
				point = lead & 0x1Fu;
			} else if (lead >= 0x80) {
				// A lone continuation byte - the string is broken; skip it.
				++i;
				continue;
			}
			if (i + extra >= a_text.size()) {
				break;
			}
			for (std::size_t k = 1; k <= extra; ++k) {
				point = (point << 6) | (static_cast<unsigned char>(a_text[i + k]) & 0x3Fu);
			}
			out.push_back(point);
			i += extra + 1;
		}
		return out;
	}

	char32_t Text::Lower(char32_t a_point)
	{
		if (a_point >= U'A' && a_point <= U'Z') {
			return a_point + 0x20;
		}
		if (a_point >= 0x0410 && a_point <= 0x042F) {   // Cyrillic A-Ya
			return a_point + 0x20;
		}
		if (a_point >= 0x0400 && a_point <= 0x040F) {   // Cyrillic Ie-Dzhe, Yo among them
			return a_point + 0x50;
		}
		return a_point;
	}

	bool Text::IsSpace(char32_t a_point)
	{
		// The whitespace ones by number: tab, line feed, carriage return and the
		// non-breaking space.
		return a_point == U' ' || a_point == 0x09 || a_point == 0x0A ||
		       a_point == 0x0D || a_point == 0x00A0;
	}

	bool Text::IsPunct(char32_t a_point)
	{
		if (a_point < 0x80) {
			return std::ispunct(static_cast<int>(a_point)) != 0;
		}
		// The marks that really do arrive from recognition: guillemets, typographic
		// quotes and dashes, the ellipsis.
		return a_point == 0x00AB || a_point == 0x00BB ||
		       (a_point >= 0x2010 && a_point <= 0x2015) ||
		       (a_point >= 0x2018 && a_point <= 0x201F) ||
		       a_point == 0x2026;
	}

	std::u32string Text::Normalize(const std::string& a_text)
	{
		std::u32string out;
		for (const auto point : Decode(a_text)) {
			if (IsSpace(point)) {
				if (!out.empty() && out.back() != U' ') {
					out.push_back(U' ');
				}
			} else if (!IsPunct(point)) {
				out.push_back(Lower(point));
			}
		}
		while (!out.empty() && out.back() == U' ') {
			out.pop_back();
		}
		return out;
	}

	// Likeness of strings without outside libraries: edit distance taken as a
	// share of the length. For commands and short phrases that is enough.
	float Text::Similarity(const std::u32string& a_left, const std::u32string& a_right)
	{
		if (a_left.empty() || a_right.empty()) {
			return 0.0f;
		}
		if (a_left == a_right) {
			return 1.0f;
		}

		std::vector<std::size_t> previous(a_right.size() + 1);
		std::vector<std::size_t> current(a_right.size() + 1);
		for (std::size_t j = 0; j <= a_right.size(); ++j) {
			previous[j] = j;
		}

		for (std::size_t i = 1; i <= a_left.size(); ++i) {
			current[0] = i;
			for (std::size_t j = 1; j <= a_right.size(); ++j) {
				const std::size_t cost = (a_left[i - 1] == a_right[j - 1]) ? 0u : 1u;
				current[j] = std::min({ current[j - 1] + 1, previous[j] + 1, previous[j - 1] + cost });
			}
			previous.swap(current);
		}

		const auto distance = static_cast<float>(previous[a_right.size()]);
		const auto longest = static_cast<float>(std::max(a_left.size(), a_right.size()));
		return std::max(0.0f, 1.0f - distance / longest);
		}
}
