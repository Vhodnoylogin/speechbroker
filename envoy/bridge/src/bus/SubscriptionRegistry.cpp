#include "SubscriptionRegistry.h"

#include "core/Text.h"

#include <algorithm>
#include <cctype>

namespace Envoy
{
	SubscriptionRegistry& SubscriptionRegistry::Get()
	{
		static SubscriptionRegistry instance;
		return instance;
	}

	void SubscriptionRegistry::Subscribe(const std::string& a_ns, std::vector<std::string> a_topics)
	{
		std::scoped_lock lock(_mutex);
		_entries[a_ns].topics = std::move(a_topics);
	}

	void SubscriptionRegistry::Declare(const std::string& a_ns, std::int32_t a_costClass,
		bool a_revocable)
	{
		std::scoped_lock lock(_mutex);
		auto& entry = _entries[a_ns];
		entry.costClass = a_costClass;
		entry.revocable = a_revocable;
	}

	void SubscriptionRegistry::Unsubscribe(const std::string& a_ns)
	{
		std::scoped_lock lock(_mutex);
		_entries.erase(a_ns);
	}

	void SubscriptionRegistry::SetActive(const std::string& a_ns, bool a_active)
	{
		std::scoped_lock lock(_mutex);
		_entries[a_ns].active = a_active;
	}

	void SubscriptionRegistry::SetVocabulary(const std::string& a_ns, std::vector<std::string> a_phrases)
	{
		std::vector<Phrase> prepared;
		prepared.reserve(a_phrases.size());
		for (auto& phrase : a_phrases) {
			prepared.push_back(Phrase{ phrase, Text::Normalize(phrase) });
		}

		std::scoped_lock lock(_mutex);
		_entries[a_ns].vocabulary = std::move(prepared);
	}

	void SubscriptionRegistry::ClearVocabulary(const std::string& a_ns)
	{
		std::scoped_lock lock(_mutex);
		_entries[a_ns].vocabulary.clear();
	}

	VocabularyMatch SubscriptionRegistry::BestOf(const Entry& a_entry,
		const std::u32string& a_text)
	{
		VocabularyMatch best;
		float           second = 0.0f;

		for (const auto& phrase : a_entry.vocabulary) {
			const auto score = Text::Similarity(a_text, phrase.normalized);
			if (score > best.score) {
				second = best.score;
				best.score = score;
				best.phrase = phrase.text;
			} else if (score > second) {
				second = score;
			}
		}

		best.margin = best.score - second;
		return best;
	}

	bool SubscriptionRegistry::Hears(const Entry& a_entry, const std::string& a_topic)
	{
		for (const auto& mine : a_entry.topics) {
			if (mine == "any" || mine == a_topic) {
				return true;
			}
			// A channel arrives as a topic of the form "channel:<name>".
			if (mine == "channel" && a_topic.rfind("channel:", 0) == 0) {
				return true;
			}
		}
		return false;
	}

	VocabularyMatch SubscriptionRegistry::Match(const std::string& a_ns, const std::string& a_text) const
	{
		std::scoped_lock lock(_mutex);

		auto it = _entries.find(a_ns);
		if (it == _entries.end()) {
			return VocabularyMatch{};
		}
		return BestOf(it->second, Text::Normalize(a_text));
	}

	std::vector<Listener> SubscriptionRegistry::Audience(const std::string& a_topic,
		const std::string& a_text) const
	{
		std::vector<Listener> out;
		if (a_text.empty()) {
			return out;
		}

		std::scoped_lock lock(_mutex);
		const auto text = Text::Normalize(a_text);

		for (const auto& [ns, entry] : _entries) {
			if (!entry.active || entry.vocabulary.empty() || !Hears(entry, a_topic)) {
				continue;
			}
			auto match = BestOf(entry, text);
			if (match.score <= 0.0f) {
				continue;
			}
			out.push_back(Listener{ ns, std::move(match), entry.costClass, entry.revocable });
		}

		std::sort(out.begin(), out.end(),
			[](const Listener& a, const Listener& b) { return a.ns < b.ns; });
		return out;
	}

	std::vector<std::string> SubscriptionRegistry::Namespaces() const
	{
		std::scoped_lock lock(_mutex);
		std::vector<std::string> out;
		out.reserve(_entries.size());
		for (const auto& entry : _entries) {
			out.push_back(entry.first);
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	std::vector<std::string> SubscriptionRegistry::TopicsOf(const std::string& a_ns) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _entries.find(a_ns);
		return it == _entries.end() ? std::vector<std::string>{} : it->second.topics;
	}

	std::vector<std::string> SubscriptionRegistry::MergedVocabulary() const
	{
		std::scoped_lock lock(_mutex);
		std::vector<std::string> out;
		for (const auto& entry : _entries) {
			if (!entry.second.active) {
				continue;
			}
			for (const auto& phrase : entry.second.vocabulary) {
				out.push_back(phrase.text);
			}
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	std::size_t SubscriptionRegistry::Count() const
	{
		std::scoped_lock lock(_mutex);
		return _entries.size();
	}
}
