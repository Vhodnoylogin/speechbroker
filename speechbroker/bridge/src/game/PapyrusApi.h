#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cstdint>
#include <vector>

namespace SpeechBroker
{
	// The bridge's in-game surface. The script name matches contract/SpeechBroker.psc.
	//
	// An event brings the subscriber nothing but the number of an utterance -
	// nothing more fits into SKSE's mechanism. Everything else it fetches with
	// these functions.
	class PapyrusApi
	{
	public:
		static bool Register(RE::BSScript::IVirtualMachine* a_vm);

		static inline constexpr const char* kScriptName = "SpeechBroker";

	private:
		using Str = RE::BSFixedString;
		using Tag = RE::StaticFunctionTag*;

		static std::int32_t GetInterfaceVersion(Tag);
		static bool         IsAvailable(Tag);

		static void Subscribe(Tag, Str a_ns, std::vector<Str> a_topics);
		static void Unsubscribe(Tag, Str a_ns);
		static void SetActive(Tag, Str a_ns, bool a_active);
		// What a participant declares about itself: what its action costs and
		// whether it can undo it. Whether the bridge holds an unfinished phrase
		// back or hands it over at once depends on this.
		static void Declare(Tag, Str a_ns, std::int32_t a_costClass, bool a_revocable);
		static void RegisterVocabulary(Tag, Str a_ns, std::vector<Str> a_phrases);
		static void ClearVocabulary(Tag, Str a_ns);

		static Str          GetText(Tag, std::int32_t a_id);
		static float        GetScore(Tag, std::int32_t a_id);
		static float        GetMargin(Tag, std::int32_t a_id);
		// How sure the bridge is that the phrase ended on this utterance.
		static float        GetComplete(Tag, std::int32_t a_id);
		static bool         IsFinal(Tag, std::int32_t a_id);
		static Str          GetEngineId(Tag, std::int32_t a_id);
		static Str          GetLanguage(Tag, std::int32_t a_id);
		static Str          GetChannel(Tag, std::int32_t a_id);
		static std::int32_t GetLatencyMs(Tag, std::int32_t a_id);

		static std::vector<Str>   GetAlternatives(Tag, std::int32_t a_id);
		static std::vector<float> GetAlternativeScores(Tag, std::int32_t a_id);

		static Str   GetVocabularyMatch(Tag, std::int32_t a_id, Str a_ns);
		static float GetVocabularyScore(Tag, std::int32_t a_id, Str a_ns);
		static float GetVocabularyMargin(Tag, std::int32_t a_id, Str a_ns);

		static void Bid(Tag, std::int32_t a_id, Str a_ns, float a_confidence, std::int32_t a_costClass, bool a_greedy);
		static void Done(Tag, std::int32_t a_id, Str a_ns, bool a_succeeded);
		static Str              GetWinner(Tag, std::int32_t a_id);
		static std::vector<Str> GetWinners(Tag, std::int32_t a_id);
		static bool             IsWinner(Tag, std::int32_t a_id, Str a_ns);
		static Str              GetDenyReason(Tag, std::int32_t a_id, Str a_ns);
		static Str              GetOutcome(Tag, std::int32_t a_id);
		static Str              GetAnswer(Tag, std::int32_t a_requestId);

		// Who declared themselves and on which topics - so that a participant can
		// show it to the player.
		static std::vector<Str> GetNamespaces(Tag);
		static std::vector<Str> GetTopicsOf(Tag, Str a_ns);
		// A self-test of delivery. SelfTest sends SpeechBroker_Ping; Pong is the script's answer.
		static void             SelfTest(Tag);
		static void             Pong(Tag, std::int32_t a_token);
		static Str              GetSpeechResult(Tag, std::int32_t a_speechId);
		static Str              GetTopic(Tag, std::int32_t a_id);

		// The bridge's second direction: from the game to the model.
		static std::int32_t Say(Tag, Str a_text, Str a_voice, std::int32_t a_priority);
		static void         StopSpeech(Tag, std::int32_t a_speechId);
		static std::int32_t Ask(Tag, Str a_service, Str a_payload);

		static std::int32_t     GetStateStatus(Tag, std::int32_t a_id, Str a_key);
		static float            GetStateAge(Tag, std::int32_t a_id, Str a_key);
		static bool             GetStateBool(Tag, std::int32_t a_id, Str a_key, bool a_default);
		static std::int32_t     GetStateInt(Tag, std::int32_t a_id, Str a_key, std::int32_t a_default);
		static float            GetStateFloat(Tag, std::int32_t a_id, Str a_key, float a_default);
		static Str              GetStateString(Tag, std::int32_t a_id, Str a_key, Str a_default);
		static RE::TESForm*     GetStateForm(Tag, std::int32_t a_id, Str a_key);
		static std::vector<Str> GetKeys(Tag);

		static bool  WonPrevious(Tag, Str a_ns);
		static float SecondsSinceWin(Tag, Str a_ns);

		static void DeclareKey(Tag, Str a_key, Str a_type, float a_ttlSec, Str a_description);
		static void RetractKey(Tag, Str a_key);
		static void PublishBool(Tag, Str a_key, bool a_value);
		static void PublishInt(Tag, Str a_key, std::int32_t a_value);
		static void PublishFloat(Tag, Str a_key, float a_value);
		static void PublishString(Tag, Str a_key, Str a_value);
		static void PublishForm(Tag, Str a_key, RE::TESForm* a_value);

		static std::vector<Str> GetAdapters(Tag);
		static Str              GetSource(Tag, Str a_capability);
		static bool             SetSource(Tag, Str a_capability, Str a_adapter);
		static void             ReloadSettings(Tag);

		// Text for the player. The engine resolves a $-string by itself only when
		// the whole string is shown as it is; anything glued together, and the
		// vocabulary a subscriber registers, comes through here.
		static Str              Translate(Tag, Str a_key);
	};
}
