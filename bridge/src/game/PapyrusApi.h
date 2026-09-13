#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cstdint>
#include <vector>

namespace Envoy
{
	// Игровая поверхность моста. Имя скрипта совпадает с contract/Envoy.psc.
	//
	// Событие приносит подписчику только номер реплики - больше в механизм SKSE
	// не влезает. Всё остальное он забирает этими функциями.
	class PapyrusApi
	{
	public:
		static bool Register(RE::BSScript::IVirtualMachine* a_vm);

		static inline constexpr const char* kScriptName = "Envoy";

	private:
		using Str = RE::BSFixedString;
		using Tag = RE::StaticFunctionTag*;

		static std::int32_t GetInterfaceVersion(Tag);
		static bool         IsAvailable(Tag);

		static void Subscribe(Tag, Str a_ns, std::vector<Str> a_topics);
		static void Unsubscribe(Tag, Str a_ns);
		static void SetActive(Tag, Str a_ns, bool a_active);
		// Что участник о себе объявляет: во что обходится его действие
		// и умеет ли он его отменить. От этого зависит, придержит ли мост
		// незаконченную фразу или отдаст сразу.
		static void Declare(Tag, Str a_ns, std::int32_t a_costClass, bool a_revocable);
		static void RegisterVocabulary(Tag, Str a_ns, std::vector<Str> a_phrases);
		static void ClearVocabulary(Tag, Str a_ns);

		static Str          GetText(Tag, std::int32_t a_id);
		static float        GetScore(Tag, std::int32_t a_id);
		static float        GetMargin(Tag, std::int32_t a_id);
		// Насколько мост уверен, что на этой реплике фраза закончилась.
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

		// Кто объявился и на какие темы - чтобы участник мог показать это игроку.
		static std::vector<Str> GetNamespaces(Tag);
		static std::vector<Str> GetTopicsOf(Tag, Str a_ns);
		// Самопроверка рассылки. SelfTest посылает Envoy_Ping, Pong - ответ скрипта.
		static void             SelfTest(Tag);
		static void             Pong(Tag, std::int32_t a_token);
		static Str              GetSpeechResult(Tag, std::int32_t a_speechId);
		static Str              GetTopic(Tag, std::int32_t a_id);

		// Второе направление моста: из игры в модель.
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
	};
}
