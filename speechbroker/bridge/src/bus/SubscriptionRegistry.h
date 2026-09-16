#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace SpeechBroker
{
	struct VocabularyMatch
	{
		std::string phrase;
		float       score{ 0.0f };
		float       margin{ 0.0f };

		// How confidently a subscriber would bid on an utterance heard with such a
		// score. The product of two different quantities: how well the utterance was
		// heard and how close it is to the declared phrase. A match against the
		// vocabulary alone is not enough: a command said indistinctly matches it
		// exactly as well as one said clearly.
		//
		// The bridge does not know the exact number - the subscriber works that out
		// itself. But this estimate is close enough to tell whether it would reach
		// the threshold of its class, and two parties use it: holding, when deciding
		// who is in the room, and the test host, when bidding on behalf of the
		// subscribers. Each of them used to have a formula of its own.
		float Confidence(float a_utteranceScore) const { return score * a_utteranceScore; }
	};

	// A subscriber that recognised the phrase, together with what it declared
	// about itself.
	//
	// The kind is declared when subscribing and not in the bid: the decision to
	// hold is taken BEFORE anybody has managed to bid, and it cannot lean on bids.
	struct Listener
	{
		std::string     ns;
		VocabularyMatch match;
		std::int32_t    costClass{ 0 };   // 0 - a reversible action, 1 - an expensive one
		// Whether it can undo what it did. A revocable one may be given things
		// earlier: it can put itself right if the phrase turns out unfinished.
		bool            revocable{ false };
	};

	// A vocabulary phrase together with its folded form. The folding is done once,
	// when the vocabulary is declared: it used to be repeated on every question
	// about a match, that is once per subscriber per utterance.
	struct Phrase
	{
		std::string    text;        // as the mod declared it - this is what goes back out
		std::u32string normalized;  // this is what we compare by
	};

	// Who is subscribed to what and which phrases they declared. The vocabularies
	// belong to the bridge: the matching is done here, in C++, so as not to make
	// Papyrus fiddle with strings - it does that badly and slowly.
	class SubscriptionRegistry
	{
	public:
		static SubscriptionRegistry& Get();

		void Subscribe(const std::string& a_ns, std::vector<std::string> a_topics);
		// What a subscriber declared about itself. As a call of its own rather than
		// parameters of the subscription: nothing changes today for whoever called
		// Subscribe yesterday, and a declaration can be refined without
		// resubscribing.
		void Declare(const std::string& a_ns, std::int32_t a_costClass, bool a_revocable);
		void Unsubscribe(const std::string& a_ns);
		void SetActive(const std::string& a_ns, bool a_active);
		void SetVocabulary(const std::string& a_ns, std::vector<std::string> a_phrases);
		void ClearVocabulary(const std::string& a_ns);

		VocabularyMatch          Match(const std::string& a_ns, const std::string& a_text) const;
		// Who is in the room: the subscribers of this topic whose vocabularies
		// recognised the phrase.
		std::vector<Listener>    Audience(const std::string& a_topic,
			const std::string& a_text) const;
		std::vector<std::string> MergedVocabulary() const;
		// Who declared themselves and on which topics - so that a participant can
		// show it to the player.
		std::vector<std::string> Namespaces() const;
		std::vector<std::string> TopicsOf(const std::string& a_ns) const;
		std::size_t              Count() const;

	private:
		SubscriptionRegistry() = default;

		struct Entry
		{
			std::vector<std::string> topics;
			std::vector<Phrase>      vocabulary;
			bool                     active{ true };
			std::int32_t             costClass{ 0 };
			bool                     revocable{ false };
		};

		// A match against the vocabulary of one record. Pulled out because it is asked
		// about in two ways - about one subscriber and about the whole room - and two
		// copies of one loop would have found time to drift apart.
		static VocabularyMatch BestOf(const Entry& a_entry, const std::u32string& a_text);

		// Whether the announcement of such a topic reaches this subscriber.
		static bool Hears(const Entry& a_entry, const std::string& a_topic);

		mutable std::mutex                     _mutex;
		std::unordered_map<std::string, Entry> _entries;
	};
}
