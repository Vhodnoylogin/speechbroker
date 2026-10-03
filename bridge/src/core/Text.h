#pragma once

#include <string>

namespace SpeechBroker
{
	// Comparing what was said against a vocabulary. This is a concern of its own
	// and not part of the subscription registry: the registry answers "who is
	// subscribed to what", and the question that lives here is "how close is this
	// to that phrase".
	//
	// Everything is counted in code points, not in bytes. Cyrillic takes two
	// bytes in UTF-8, and per-character work over bytes gives nonsense: the case
	// is not folded at all, and the price of a capital letter depends on which
	// half of the block it sits in.
	class Text
	{
	public:
		// Splitting UTF-8 into code points. A broken byte is skipped, not fatal.
		static std::u32string Decode(const std::string& a_text);

		// Folding: drop the case, throw out the punctuation, squeeze the spaces. To
		// be done once - when the vocabulary is declared, not on every question.
		static std::u32string Normalize(const std::string& a_text);

		// Likeness without outside libraries: edit distance taken as a share of the
		// length. For commands and short phrases that is enough.
		static float Similarity(const std::u32string& a_left, const std::u32string& a_right);

	private:
		static char32_t Lower(char32_t a_point);
		static bool     IsSpace(char32_t a_point);
		static bool     IsPunct(char32_t a_point);
	};
}
