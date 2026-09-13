/* Envoy Framework - the adapter interface. The contract version is
 * kInterfaceVersion below.
 *
  * An adapter is a mod just like the bridge: an ordinary SKSE plugin living in
  * the same process of the game. That is why they talk by calling a function
  * rather than over a network.
 *
  * Outward - towards a model, which cannot be part of the game - only the
  * adapter looks, and it chooses the transport itself: HTTP, a pipe, a library
  * built in. There is nothing in that for the bridge to know.
 *
  * The handshake is the same as in HIGGS and PLANCK: the bridge broadcasts an
  * SKSE message with a pointer to the interface and the adapter catches it.
 */
#pragma once

#include <cstdint>

namespace EnvoyAPI
{
	constexpr std::uint32_t kInterfaceVersion = 3;

	// The type of the SKSE message the bridge hands the interface over with. The
	// sender is "Envoy".
	constexpr std::uint32_t kMessageInterface = 'ENVY';

	// What the adapter sent. The strings live only for the length of the call:
	// the bridge copies them.
	struct UtteranceIn
	{
		const char*  text{ nullptr };
		const char*  language{ nullptr };
		const char*  engine{ nullptr };
		const char*  channel{ nullptr };
		float        score{ 0.0f };
		float        margin{ 0.0f };
		std::int32_t latencyMs{ 0 };
		std::int32_t durationMs{ 0 };
		bool         isFinal{ true };

		// 0 - a new utterance; anything else refines one already given out. That is
		// how the pair "a fast model plus an accurate one" works: the second refines
		// the result of the first.
		std::int32_t refinesId{ 0 };

		const char* const* altText{ nullptr };
		const float*       altScore{ nullptr };
		std::int32_t       altCount{ 0 };

		// --- contract version 3 ---------------------------------------------
		// The bridge does not read these fields from an adapter that declared an
		// older version.

		// The chance that the sentence ENDED on this piece. Only the adapter knows
		// this: it is the one that hears the pause, the intonation and the tone,
		// while the bridge gets text already. One means "finished", and it is also
		// the default: an adapter that knows nothing about this behaves as before.
		float        complete{ 1.0f };

		// 0 short, 1 middle, 2 long. The bridge holds pieces of different classes for
		// different lengths of time: for a short one the continuation comes quickly,
		// for a long one there is nothing left to wait for.
		std::int32_t lengthClass{ 0 };

		// The numbers of the utterances this piece has taken into itself. The numbers
		// are the ones PushUtterance gave back: the bridge does not know the
		// numbering of pieces and is not meant to. Those of them that are held will
		// be thrown away unannounced, and those already handed over will be revoked.
		const std::int32_t* supersedes{ nullptr };
		std::int32_t        supersedesCount{ 0 };
	};

	struct AdapterInfo
	{
		const char*   id{ nullptr };        // "voice"
		const char*   name{ nullptr };      // human readable
		const char*   provides{ nullptr };  // "asr,tts" - comma separated
		std::uint32_t contract{ kInterfaceVersion };
	};

	enum JobKind : std::int32_t
	{
		kJobListen     = 1,  // be the source, or fall silent
		kJobVocabulary = 2,  // the merged vocabulary of the subscribers has changed
		kJobSpeak      = 3,  // speak the text
		kJobStop       = 4,  // stop speaking
		kJobAsk        = 5   // any request to the model: its content is opaque to the bridge
	};

	struct Job
	{
		std::int32_t       kind{ 0 };
		bool               active{ false };    // for kJobListen
		const char*        text{ nullptr };    // text for kJobSpeak, the reason for kJobListen
		const char* const* phrases{ nullptr };  // for kJobVocabulary
		std::int32_t       phraseCount{ 0 };
		std::int32_t       speechId{ 0 };

		// For kJobAsk. The bridge neither parses nor checks the payload: otherwise
		// every new model would mean an edit to the bridge.
		std::int32_t       requestId{ 0 };
		const char*        service{ nullptr };
		const char*        payload{ nullptr };
	};

	using JobCallback = void (*)(const Job& a_job, void* a_user);

	class IEnvoy
	{
	public:
		virtual ~IEnvoy() = default;

		virtual std::uint32_t Version() const = 0;

		// The bridge keeps one active source per capability at a time; the other
		// adapters get kJobListen with active=false and are obliged to let go of
		// their device.
		virtual bool Register(const AdapterInfo& a_info, JobCallback a_onJob, void* a_user) = 0;
		virtual void Unregister(const char* a_id) = 0;

		// Gives back the number of the utterance in the game, or 0 on a refusal.
		virtual std::int32_t PushUtterance(const char* a_adapterId, const UtteranceIn& a_utterance) = 0;

		// Who is the source for a capability right now. The name is copied into the
		// buffer of the caller: handing back a pointer to an internal string of the
		// bridge is not allowed - the next one to ask would overwrite it, and the
		// asking comes from different threads. false - there is no source, or the
		// buffer is too small.
		virtual bool SourceOf(const char* a_capability, char* a_out,
			std::int32_t a_outSize) const = 0;

		// The other direction: the adapter reports how a job ended.
		virtual void PushAnswer(const char* a_adapterId, std::int32_t a_requestId, bool a_ok,
			const char* a_payload) = 0;
		virtual void PushSpeechDone(const char* a_adapterId, std::int32_t a_speechId, bool a_ok,
			bool a_interrupted) = 0;
	};
}
