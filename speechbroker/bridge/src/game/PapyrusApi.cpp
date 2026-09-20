#include "PapyrusApi.h"

#include "speechbroker-adapter.h"
#include "speechbroker-loc.h"

#include "bus/SubscriptionRegistry.h"
#include "bus/UtteranceStore.h"
#include "core/Loc.h"
#include "core/Log.h"
#include "wire/AdapterHost.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace SpeechBroker
{
	namespace
	{
		std::vector<std::string> ToStrings(const std::vector<RE::BSFixedString>& a_items)
		{
			std::vector<std::string> out;
			out.reserve(a_items.size());
			for (const auto& item : a_items) {
				out.emplace_back(item.c_str());
			}
			return out;
		}

		// The engine's own translation table: $-key to line, in the language the
		// game is running. Reached through the Scaleform loader because that is
		// where the engine keeps it; everything here is null-checked, since a
		// script may call before the menus exist and none of this is worth a crash.
		//
		// The answer comes back as UTF-16, and is turned into UTF-8 by the very
		// decoder that reads the files - the bytes are handed over with a byte
		// order mark in front so that the one tested routine does the work.
		bool FromTheEngine(const char* a_key, std::string& a_out)
		{
			if (a_key == nullptr || *a_key != '$') {
				return false;  // not a key at all - the engine has nothing to say
			}

			auto* const manager = RE::BSScaleformManager::GetSingleton();
			auto* const loader = manager ? manager->loader : nullptr;
			if (loader == nullptr) {
				return false;
			}
			const auto translator =
				loader->GetState<RE::BSScaleformTranslator>(RE::GFxState::StateType::kTranslator);
			if (!translator) {
				return false;
			}

			// The keys are ASCII by construction, so widening one character at a
			// time is exact here and needs no conversion table.
			const std::wstring wide(a_key, a_key + std::strlen(a_key));

			const auto& map = translator->translator.translationMap;
			const auto  at = map.find(RE::BSFixedStringW(wide.c_str()));
			if (at == map.end() || at->second.empty()) {
				return false;
			}

			const wchar_t* const line = at->second.c_str();
			const auto           units = std::wcslen(line);

			std::vector<char> bytes;
			bytes.reserve(units * 2 + 2);
			bytes.push_back(static_cast<char>(0xFF));
			bytes.push_back(static_cast<char>(0xFE));
			const auto* const raw = reinterpret_cast<const char*>(line);
			bytes.insert(bytes.end(), raw, raw + units * 2);

			a_out = SpeechBrokerLoc::detail::ToUtf8(bytes);
			return !a_out.empty();
		}
	}

	std::int32_t PapyrusApi::GetInterfaceVersion(Tag)
	{
		// Out of the binary, not out of the settings: a subscriber is asking which
		// contract the bridge CAN do, not the one somebody wrote into its json. This
		// used to hand back a 1 out of the settings file, and every script that
		// checked the version was told a lie.
		return static_cast<std::int32_t>(SpeechBrokerAPI::kInterfaceVersion);
	}

	bool PapyrusApi::IsAvailable(Tag)
	{
		// If the library is absent the function is not registered, the call does not
		// go through and the mod gets false. So "yes" here means exactly that.
		return true;
	}

	void PapyrusApi::Subscribe(Tag, Str a_ns, std::vector<Str> a_topics)
	{
		// BSFixedString keeps strings in one shared pool without regard to case: the
		// engine already holds "Dialogue", so a "dialogue" declared by a script comes
		// back out of the pool in somebody else's spelling. Topics cannot be compared
		// after that, so they are lowered on the way in - the name of a topic is ours,
		// not the engine's.
		auto topics = ToStrings(a_topics);
		for (auto& t : topics) {
			std::transform(t.begin(), t.end(), t.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		}
		SubscriptionRegistry::Get().Subscribe(a_ns.c_str(), topics);
		std::string joined;
		for (const auto& t : topics) { joined += joined.empty() ? t : ", " + t; }
		Log::Info("$SPEECHBROKER_LOG_SUBSCRIBED", a_ns.c_str(), joined);
	}

	void PapyrusApi::Unsubscribe(Tag, Str a_ns)
	{
		SubscriptionRegistry::Get().Unsubscribe(a_ns.c_str());
	}

	void PapyrusApi::SetActive(Tag, Str a_ns, bool a_active)
	{
		SubscriptionRegistry::Get().SetActive(a_ns.c_str(), a_active);
	}

	void PapyrusApi::Declare(Tag, Str a_ns, std::int32_t a_costClass, bool a_revocable)
	{
		SubscriptionRegistry::Get().Declare(a_ns.c_str(), a_costClass, a_revocable);
		Log::Info("$SPEECHBROKER_LOG_DECLARED", a_ns.c_str(),
			Loc::Get(a_costClass >= 1 ? "$SPEECHBROKER_WORD_EXPENSIVE" : "$SPEECHBROKER_WORD_REVERSIBLE"),
			Loc::Get(a_revocable ? "$SPEECHBROKER_WORD_CAN_UNDO" : "$SPEECHBROKER_WORD_CANNOT_UNDO"));
	}

	void PapyrusApi::RegisterVocabulary(Tag, Str a_ns, std::vector<Str> a_phrases)
	{
		const auto phrases = ToStrings(a_phrases);
		SubscriptionRegistry::Get().SetVocabulary(a_ns.c_str(), phrases);
		Log::Info("$SPEECHBROKER_LOG_VOCABULARY", a_ns.c_str(), phrases.size());
		// The side that listens is the one that needs the vocabulary: let the adapters
		// hear about it straight away.
		AdapterHost::Get().SendVocabulary(SubscriptionRegistry::Get().MergedVocabulary());
	}

	void PapyrusApi::ClearVocabulary(Tag, Str a_ns)
	{
		SubscriptionRegistry::Get().ClearVocabulary(a_ns.c_str());
		AdapterHost::Get().SendVocabulary(SubscriptionRegistry::Get().MergedVocabulary());
	}

	RE::BSFixedString PapyrusApi::GetText(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? RE::BSFixedString{ item->text } : RE::BSFixedString{};
	}

	float PapyrusApi::GetScore(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? item->score : 0.0f;
	}

	float PapyrusApi::GetMargin(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? item->margin : 0.0f;
	}

	float PapyrusApi::GetComplete(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? item->complete : 1.0f;
	}

	bool PapyrusApi::IsFinal(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item && item->isFinal;
	}

	RE::BSFixedString PapyrusApi::GetEngineId(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? RE::BSFixedString{ item->engine } : RE::BSFixedString{};
	}

	RE::BSFixedString PapyrusApi::GetLanguage(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? RE::BSFixedString{ item->language } : RE::BSFixedString{};
	}

	RE::BSFixedString PapyrusApi::GetChannel(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? RE::BSFixedString{ item->channel } : RE::BSFixedString{};
	}

	std::int32_t PapyrusApi::GetLatencyMs(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? item->latencyMs : 0;
	}

		RE::BSFixedString PapyrusApi::GetWorldState(Tag, std::int32_t a_id, Str a_key)
	{
		const auto* key = a_key.c_str();
		if (!key || !*key) {
			return RE::BSFixedString{};
		}
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return RE::BSFixedString{};
		}
		const auto found = item->state.find(std::string{ key });
		return found == item->state.end() ? RE::BSFixedString{} : RE::BSFixedString{ found->second };
	}

	std::vector<RE::BSFixedString> PapyrusApi::GetWorldStateKeys(Tag, std::int32_t a_id)
	{
		std::vector<RE::BSFixedString> keys;
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return keys;
		}
		// The order is the order of the map, which is sorted: a subscriber that walks
		// the keys gets the same walk every time, and a log made of it can be compared
		// between two runs.
		keys.reserve(item->state.size());
		for (const auto& entry : item->state) {
			keys.push_back(RE::BSFixedString{ entry.first });
		}
		return keys;
	}

std::vector<RE::BSFixedString> PapyrusApi::GetAlternatives(Tag, std::int32_t a_id)
	{
		std::vector<RE::BSFixedString> out;
		auto item = UtteranceStore::Get().Find(a_id);
		if (item) {
			for (const auto& alt : item->alternatives) {
				out.emplace_back(alt.text);
			}
		}
		return out;
	}

	std::vector<float> PapyrusApi::GetAlternativeScores(Tag, std::int32_t a_id)
	{
		std::vector<float> out;
		auto item = UtteranceStore::Get().Find(a_id);
		if (item) {
			for (const auto& alt : item->alternatives) {
				out.push_back(alt.score);
			}
		}
		return out;
	}

	RE::BSFixedString PapyrusApi::GetVocabularyMatch(Tag, std::int32_t a_id, Str a_ns)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return {};
		}
		return RE::BSFixedString{ SubscriptionRegistry::Get().Match(a_ns.c_str(), item->text).phrase };
	}

	float PapyrusApi::GetVocabularyScore(Tag, std::int32_t a_id, Str a_ns)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			Log::Info("$SPEECHBROKER_LOG_MATCH_NO_UTTERANCE", a_ns.c_str(), a_id);
			return 0.0f;
		}
		const auto match = SubscriptionRegistry::Get().Match(a_ns.c_str(), item->text);
		// The most important line in the log: it proves the event reached the script.
		// Without it "the script kept quiet" and "the event never arrived" look alike.
		Log::Info("$SPEECHBROKER_LOG_MATCH",
			a_ns.c_str(), a_id, match.score, match.phrase);
		return match.score;
	}

	float PapyrusApi::GetVocabularyMargin(Tag, std::int32_t a_id, Str a_ns)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? SubscriptionRegistry::Get().Match(a_ns.c_str(), item->text).margin : 0.0f;
	}

	void PapyrusApi::Bid(Tag, std::int32_t a_id, Str a_ns, float a_confidence, std::int32_t a_costClass,
		bool a_greedy)
	{
		// Which phrase the bidder recognised is worked out by the bridge rather than
		// taken on trust: the vocabularies belong to it, and sorting out a tie hangs on
		// this answer. An empty phrase would mean "unknown", and every tie would end in
		// a refusal to everybody - which is why it is in the log too, so that the
		// silence is visible.
		auto        stored = UtteranceStore::Get().Find(a_id);
		std::string phrase;
		if (stored) {
			phrase = SubscriptionRegistry::Get().Match(a_ns.c_str(), stored->text).phrase;
		}

		const bool accepted = UtteranceStore::Get().AddBid(a_id,
			BidRecord{ a_ns.c_str(), a_confidence, a_costClass, a_greedy, phrase });

		// There is no other way to tell "the subscriber kept quiet" from "the subscriber
		// was late" out of the log, and the difference decides everything: in the first
		// case the event never reached it, in the second the bid window is shorter than
		// Papyrus's own delay.
		Log::Info("$SPEECHBROKER_LOG_BID", a_ns.c_str(), a_id, a_confidence,
			Loc::Get(a_greedy ? "$SPEECHBROKER_WORD_GREEDY" : "$SPEECHBROKER_WORD_SHARING"), phrase,
			stored ? stored->MsSinceOffer() : -1,
			accepted ? "" : Loc::Get("$SPEECHBROKER_WORD_TOO_LATE"));
	}

	void PapyrusApi::Done(Tag, std::int32_t a_id, Str a_ns, bool a_succeeded)
	{
		Log::Debug("$SPEECHBROKER_LOG_DONE", a_id, a_ns.c_str(), a_succeeded);
	}

	RE::BSFixedString PapyrusApi::GetWinner(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return (item && !item->winners.empty()) ? RE::BSFixedString{ item->winners.front() }
		                                       : RE::BSFixedString{};
	}

	std::vector<RE::BSFixedString> PapyrusApi::GetWinners(Tag, std::int32_t a_id)
	{
		std::vector<RE::BSFixedString> out;
		auto item = UtteranceStore::Get().Find(a_id);
		if (item) {
			for (const auto& winner : item->winners) {
				out.emplace_back(winner);
			}
		}
		return out;
	}

	bool PapyrusApi::IsWinner(Tag, std::int32_t a_id, Str a_ns)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return false;
		}
		const std::string ns{ a_ns.c_str() };
		return std::find(item->winners.begin(), item->winners.end(), ns) != item->winners.end();
	}

	RE::BSFixedString PapyrusApi::GetDenyReason(Tag, std::int32_t a_id, Str a_ns)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		if (!item) {
			return {};
		}
		auto it = item->denied.find(a_ns.c_str());
		return it == item->denied.end() ? RE::BSFixedString{} : RE::BSFixedString{ it->second };
	}

	std::vector<RE::BSFixedString> PapyrusApi::GetNamespaces(Tag)
	{
		std::vector<RE::BSFixedString> out;
		for (const auto& ns : SubscriptionRegistry::Get().Namespaces()) {
			out.emplace_back(ns);
		}
		return out;
	}

	std::vector<RE::BSFixedString> PapyrusApi::GetTopicsOf(Tag, Str a_ns)
	{
		std::vector<RE::BSFixedString> out;
		for (const auto& t : SubscriptionRegistry::Get().TopicsOf(a_ns.c_str())) {
			out.emplace_back(t);
		}
		return out;
	}

	RE::BSFixedString PapyrusApi::GetOutcome(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? RE::BSFixedString{ item->outcome } : RE::BSFixedString{};
	}

	RE::BSFixedString PapyrusApi::GetTopic(Tag, std::int32_t a_id)
	{
		auto item = UtteranceStore::Get().Find(a_id);
		return item ? RE::BSFixedString{ item->topic } : RE::BSFixedString{};
	}

	// The snapshot of the world. So far it holds only what the script providers have
	// published: the core and the outside providers arrive separately, and until then
	// their keys honestly answer "nobody to ask" instead of making something up.

	bool PapyrusApi::WonPrevious(Tag, Str a_ns)
	{
		return UtteranceStore::Get().WonPrevious(a_ns.c_str());
	}

	float PapyrusApi::SecondsSinceWin(Tag, Str a_ns)
	{
		return UtteranceStore::Get().SecondsSinceWin(a_ns.c_str());
	}

	// The line the key stands for, in the language the game runs in. The engine
	// resolves a $-string on its own only when the whole string is shown as it is;
	// a line glued together out of a translated part and a number, and text that is
	// never shown at all - a subscriber's vocabulary - have to come through here.
	//
	// THE ENGINE IS ASKED FIRST, and that is the whole fix of 20.09.2026. A
	// subscriber's keys live in the SUBSCRIBER's file, which this plugin has never
	// heard of and has no business scanning for; and the language is the game's to
	// decide, not ours to guess out of an ini. The engine has already read every
	// Interface\Translations\*_<language>.txt on the load order by the time any
	// script runs, so it holds exactly the right line in exactly the right
	// language. Our own table answers only when the engine has nothing - which is
	// the case for keys of ours that no translator has covered.
	RE::BSFixedString PapyrusApi::Translate(Tag, Str a_key)
	{
		if (std::string line; FromTheEngine(a_key.c_str(), line)) {
			return RE::BSFixedString{ line.c_str() };
		}
		return RE::BSFixedString{ Loc::Get(a_key.c_str()) };
	}

	bool PapyrusApi::Register(RE::BSScript::IVirtualMachine* a_vm)
	{
		if (!a_vm) {
			return false;
		}

		a_vm->RegisterFunction("GetInterfaceVersion", kScriptName, GetInterfaceVersion);
		a_vm->RegisterFunction("IsAvailable", kScriptName, IsAvailable);

		a_vm->RegisterFunction("Subscribe", kScriptName, Subscribe);
		a_vm->RegisterFunction("Unsubscribe", kScriptName, Unsubscribe);
		a_vm->RegisterFunction("SetActive", kScriptName, SetActive);
		a_vm->RegisterFunction("RegisterVocabulary", kScriptName, RegisterVocabulary);
		a_vm->RegisterFunction("ClearVocabulary", kScriptName, ClearVocabulary);

		a_vm->RegisterFunction("GetText", kScriptName, GetText);
		a_vm->RegisterFunction("GetScore", kScriptName, GetScore);
		a_vm->RegisterFunction("GetMargin", kScriptName, GetMargin);
		a_vm->RegisterFunction("IsFinal", kScriptName, IsFinal);
		a_vm->RegisterFunction("GetEngineId", kScriptName, GetEngineId);
		a_vm->RegisterFunction("GetLanguage", kScriptName, GetLanguage);
		a_vm->RegisterFunction("GetChannel", kScriptName, GetChannel);
		a_vm->RegisterFunction("GetLatencyMs", kScriptName, GetLatencyMs);
		a_vm->RegisterFunction("GetWorldState", kScriptName, GetWorldState);
		a_vm->RegisterFunction("GetWorldStateKeys", kScriptName, GetWorldStateKeys);
		a_vm->RegisterFunction("GetAlternatives", kScriptName, GetAlternatives);
		a_vm->RegisterFunction("GetAlternativeScores", kScriptName, GetAlternativeScores);

		a_vm->RegisterFunction("Declare", kScriptName, Declare);
		a_vm->RegisterFunction("GetComplete", kScriptName, GetComplete);
		a_vm->RegisterFunction("GetVocabularyMatch", kScriptName, GetVocabularyMatch);
		a_vm->RegisterFunction("GetVocabularyScore", kScriptName, GetVocabularyScore);
		a_vm->RegisterFunction("GetVocabularyMargin", kScriptName, GetVocabularyMargin);

		a_vm->RegisterFunction("Bid", kScriptName, Bid);
		a_vm->RegisterFunction("Done", kScriptName, Done);
		a_vm->RegisterFunction("GetWinner", kScriptName, GetWinner);
		a_vm->RegisterFunction("GetWinners", kScriptName, GetWinners);
		a_vm->RegisterFunction("IsWinner", kScriptName, IsWinner);
		a_vm->RegisterFunction("GetDenyReason", kScriptName, GetDenyReason);
		a_vm->RegisterFunction("GetOutcome", kScriptName, GetOutcome);
		a_vm->RegisterFunction("GetAnswer", kScriptName, GetAnswer);
		a_vm->RegisterFunction("GetNamespaces", kScriptName, GetNamespaces);
		a_vm->RegisterFunction("GetTopicsOf", kScriptName, GetTopicsOf);
		a_vm->RegisterFunction("SelfTest", kScriptName, SelfTest);
		a_vm->RegisterFunction("Pong", kScriptName, Pong);
		a_vm->RegisterFunction("GetSpeechResult", kScriptName, GetSpeechResult);
		a_vm->RegisterFunction("GetTopic", kScriptName, GetTopic);
		a_vm->RegisterFunction("Say", kScriptName, Say);
		a_vm->RegisterFunction("StopSpeech", kScriptName, StopSpeech);
		a_vm->RegisterFunction("Ask", kScriptName, Ask);

		a_vm->RegisterFunction("GetStateStatus", kScriptName, GetStateStatus);
		a_vm->RegisterFunction("GetStateAge", kScriptName, GetStateAge);
		a_vm->RegisterFunction("GetStateBool", kScriptName, GetStateBool);
		a_vm->RegisterFunction("GetStateInt", kScriptName, GetStateInt);
		a_vm->RegisterFunction("GetStateFloat", kScriptName, GetStateFloat);
		a_vm->RegisterFunction("GetStateString", kScriptName, GetStateString);
		a_vm->RegisterFunction("GetStateForm", kScriptName, GetStateForm);
		a_vm->RegisterFunction("GetKeys", kScriptName, GetKeys);
		a_vm->RegisterFunction("WonPrevious", kScriptName, WonPrevious);
		a_vm->RegisterFunction("SecondsSinceWin", kScriptName, SecondsSinceWin);
		a_vm->RegisterFunction("DeclareKey", kScriptName, DeclareKey);
		a_vm->RegisterFunction("RetractKey", kScriptName, RetractKey);
		a_vm->RegisterFunction("PublishBool", kScriptName, PublishBool);
		a_vm->RegisterFunction("PublishInt", kScriptName, PublishInt);
		a_vm->RegisterFunction("PublishFloat", kScriptName, PublishFloat);
		a_vm->RegisterFunction("PublishString", kScriptName, PublishString);
		a_vm->RegisterFunction("PublishForm", kScriptName, PublishForm);

		a_vm->RegisterFunction("GetAdapters", kScriptName, GetAdapters);
		a_vm->RegisterFunction("GetSource", kScriptName, GetSource);
		a_vm->RegisterFunction("SetSource", kScriptName, SetSource);
		a_vm->RegisterFunction("ReloadSettings", kScriptName, ReloadSettings);

		a_vm->RegisterFunction("Translate", kScriptName, Translate);

		Log::Info("$SPEECHBROKER_LOG_PAPYRUS_REGISTERED", kScriptName);
		return true;
	}
}
