#pragma once

#include "envoy-adapter.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	// The reception desk of the bridge for adapters. The bridge knows no external
	// program at all: adapters are mods like any other, they come by themselves
	// and name themselves.
	//
	// Jobs are handed over by calling a callback right in the thread of the caller,
	// so an adapter is obliged only to put the job into a queue of its own and
	// return.
	//
	// The lock is always let go by then: jobs are gathered under it and sent out
	// after. Otherwise an adapter that answered the bridge from its handler would
	// seize up for good.
	class AdapterHost final : public EnvoyAPI::IEnvoy
	{
	public:
		static AdapterHost& Get();

		std::uint32_t Version() const override { return EnvoyAPI::kInterfaceVersion; }

		bool         Register(const EnvoyAPI::AdapterInfo& a_info, EnvoyAPI::JobCallback a_onJob, void* a_user) override;
		void         Unregister(const char* a_id) override;
		std::int32_t PushUtterance(const char* a_adapterId, const EnvoyAPI::UtteranceIn& a_utterance) override;
		bool         SourceOf(const char* a_capability, char* a_out,
		                 std::int32_t a_outSize) const override;

		// For the menu and the scripts.
		bool                     SetSource(const std::string& a_capability, const std::string& a_adapter);
		std::string              Source(const std::string& a_capability) const;
		std::vector<std::string> AdapterIds() const;
		void                     ReloadConfig();

		// The answer of a model and the outcome of speaking: the event carries only a
		// number, so the subscriber comes here for the content.
		std::string Answer(std::int32_t a_requestId) const;
		std::string SpeechResult(std::int32_t a_speechId) const;

		void PushAnswer(const char* a_adapterId, std::int32_t a_requestId, bool a_ok,
			const char* a_payload) override;
		void PushSpeechDone(const char* a_adapterId, std::int32_t a_speechId, bool a_ok,
			bool a_interrupted) override;

		void         SendVocabulary(const std::vector<std::string>& a_phrases);
		std::int32_t SendSpeak(const std::string& a_text, const std::string& a_voice, std::int32_t a_priority);
		void         SendStop(std::int32_t a_speechId);
		std::int32_t SendAsk(const std::string& a_service, const std::string& a_payload);

	private:
		AdapterHost() = default;

		struct Entry
		{
			std::string              name;
			std::vector<std::string> provides;
			EnvoyAPI::JobCallback    onJob{ nullptr };
			void*                    user{ nullptr };
			std::uint64_t            order{ 0 };
			bool                     active{ false };
			// The contract version declared at the handshake. The bridge does not read
			// fields that did not yet exist in that version.
			std::uint32_t            contract{ 0 };
		};

		// A job gathered under the lock and sent out without it. The strings belong to
		// the job itself: the pointers inside EnvoyAPI::Job live only for the length
		// of the call, and the call happens after the lock has been let go.
		struct Outgoing
		{
			EnvoyAPI::JobCallback    onJob{ nullptr };
			void*                    user{ nullptr };
			std::int32_t             kind{ 0 };
			bool                     active{ false };
			std::string              text;
			std::string              service;
			std::string              payload;
			std::int32_t             speechId{ 0 };
			std::int32_t             requestId{ 0 };
			std::vector<std::string> phrases;

			void Send() const;
		};

		static void Dispatch(const std::vector<Outgoing>& a_jobs);

		// The rule "who is the source for a capability", separated from its
		// consequences. A pure function of which adapters there are, of what the menu
		// forced and of the names from the settings: it can be read and checked
		// without thinking about locks, jobs and the log. The rule used to be
		// tangled up with them.
		static std::unordered_map<std::string, std::string> Choose(
			const std::unordered_map<std::string, Entry>&       a_adapters,
			const std::unordered_map<std::string, std::string>& a_overrides);

		// Applies Choose: switches who is active and hands back the jobs instead of
		// sending them. The caller is obliged to let the lock go and only then call
		// Dispatch.
		[[nodiscard]] std::vector<Outgoing> RecomputeSources();

		mutable std::mutex                           _mutex;
		std::unordered_map<std::string, Entry>       _adapters;
		std::unordered_map<std::string, std::string> _sources;
		std::unordered_map<std::string, std::string> _overrides;

		// The event carries only a number, so the answer itself and the outcome of
		// speaking have to lie somewhere until the subscriber comes for them.
		std::unordered_map<std::int32_t, std::string> _answers;
		std::unordered_map<std::int32_t, std::string> _speechResults;
		std::uint64_t                                _order{ 0 };
		std::int32_t                                 _nextSpeech{ 1 };
		std::int32_t                                 _nextRequest{ 1 };
	};
}
