// One model, as the adapter sees it: the whole of our side of
// speechbroker-voice-model.h.
//
// There is no RE/ and no SKSE/ in this file or in the one beside it, and that is
// a rule rather than an accident. Everything hard here - the debt of exactly one
// Complete per accepted utterance, the queue that must never wait, the child
// that dies with the process, the sentinels that must not be zero - is testable
// without a game, and a core that needs SkyrimVR.exe to be exercised is a core
// that is exercised by launching a game in a headset. The SKSE plumbing is
// main.cpp and is four small functions.
//
// ONE INSTANCE PER MODEL, NOT PER MOD. This mod ships two - the draft and the
// accurate one - and each registers under its own id and raises its own child,
// because that is what they are: two different sets of weights, with two
// different budgets, which the adapter is meant to race against each other.
//
// THE INSTANCE IS NEVER DESTROYED, and that is also the contract's doing. Stop
// may never be called; after stopMs the adapter abandons the model's dispatch
// thread rather than killing it; and the per-model state an abandoned thread can
// still reach must therefore never be freed, or the abandonment it promises is a
// use-after-free. So these live in a static list for the life of the process and
// have no destructor worth writing.
#pragma once

#include "ChildProcess.h"
#include "Log.h"
#include "Settings.h"
#include "Wire.h"

#include "speechbroker-voice-model.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace WhisperRu
{
	class ModelShim
	{
	public:
		explicit ModelShim(ModelSettings a_settings);
		ModelShim(const ModelShim&) = delete;
		ModelShim& operator=(const ModelShim&) = delete;

		// THE MINIMUM VERSION OF THE ADAPTER THIS SHIM NEEDS, and it is 1
		// because this code reads no field that a later version added. The
		// contract is explicit that this is the number to check - not our own
		// version - so that a shim built against version 5 keeps working against
		// every adapter ever shipped. Raise it on the day a field of a later
		// version is actually read, and not before.
		static constexpr std::uint32_t kMinimumHostVersion = 1;

		// Called once, from the SKSE message handler, on the thread of the game.
		// It does nothing slow and it does NOT bring the model up: the contract
		// forbids that here, and a model refused for its kind must never have
		// opened anything.
		bool Register(const SpeechBrokerVoiceHost* a_host);

		const std::string& Id() const { return m_settings.id; }
		bool Enabled() const { return m_settings.enabled; }

	private:
		// What one accepted utterance is, on our side of the debt. The samples
		// are a COPY: the contract lends the buffer for the length of Submit and
		// not one instruction longer, and a shim that queued the pointer would
		// be reading the adapter's dead stack four hundred milliseconds later.
		struct Job
		{
			bool               vocabulary{ false };  // a control job, not an utterance
			std::int64_t       utteranceId{ 0 };
			std::int64_t       turnId{ 0 };
			std::int32_t       serial{ 0 };
			std::int32_t       final{ 0 };
			std::uint32_t      lostSamples{ 0 };
			std::uint32_t      deadlineMs{ 0 };
			std::vector<float> samples;
			std::vector<std::string> phrases;
		};

		// What we owe, from the moment Submit accepted it until the one Complete
		// that discharges it.
		struct Debt
		{
			std::int32_t                          serial{ 0 };
			std::chrono::steady_clock::time_point acceptedAt{};
			bool                                  cancelled{ false };
			bool                                  sent{ false };
		};

		// ------------------------------------------------- the contract's table
		// Plain C thunks, because the table is a struct of function pointers and
		// a member function is not one. Each finds its instance in a_user, which
		// is the pointer we put there at Register.
		static std::int32_t SPEECHBROKERVOICE_CALL StartThunk(void* a_user);
		static void SPEECHBROKERVOICE_CALL StopThunk(void* a_user);
		static std::int32_t SPEECHBROKERVOICE_CALL SubmitThunk(void* a_user,
			const SpeechBrokerVoiceRequest* a_request);
		static void SPEECHBROKERVOICE_CALL CancelThunk(void* a_user, std::int64_t a_utteranceId);
		static void SPEECHBROKERVOICE_CALL VocabularyThunk(void* a_user,
			const char* const* a_phrases, std::int32_t a_count);

		std::int32_t Start();
		void         Stop();
		std::int32_t Submit(const SpeechBrokerVoiceRequest* a_request);
		void         Cancel(std::int64_t a_utteranceId);
		void         SetVocabulary(const char* const* a_phrases, std::int32_t a_count);

		// ------------------------------------------------------- our own threads
		void WriterLoop();  // takes jobs off the queue and puts them on the pipe
		void ReaderLoop();  // takes frames off the pipe and pays the debts

		// The child has gone. Read the number, say it, fail everything
		// outstanding, and go not-ready. This is the ordinary failure the whole
		// child-process design exists to turn a vanished game into.
		void OnChildGone();

		bool VerifyWeights(std::string& a_failedFile, std::string& a_reason);
		bool Raise();

		// Claims a debt: at most one caller ever gets it, and only that caller
		// calls Complete. Three different paths can arrive at one utteranceId -
		// a reply, a cancel before it was sent, and a dead child - and "exactly
		// one Complete" is this function and nothing else.
		bool TakeDebt(std::int64_t a_utteranceId, Debt& a_out);

		void Answer(std::int64_t a_utteranceId, const Debt& a_debt, const Wire::Reply& a_reply);
		void AnswerFailure(std::int64_t a_utteranceId, const Debt& a_debt, std::int32_t a_status,
			const std::string& a_failed);

		// Both destinations at once: rendered into our own file, and handed to
		// the adapter as the key and its arguments, unrendered, which is what the
		// contract says Host::Log does with them.
		void Say(std::int32_t a_level, const std::string& a_key,
			const std::vector<std::string>& a_args);

		template <class... Args>
		void Tell(std::int32_t a_level, const std::string& a_key, Args&&... a_args)
		{
			Say(a_level, a_key, Log::Pack(std::forward<Args>(a_args)...));
		}

		ModelSettings m_settings;

		// Borrowed for the life of the process: the contract says the host table
		// is a static inside the adapter and is never replaced, and that the
		// pointer is the one thing a shim must keep.
		const SpeechBrokerVoiceHost* m_host{ nullptr };
		SpeechBrokerVoiceSession     m_session{};
		SpeechBrokerVoiceModel       m_table{};
		bool                         m_registered{ false };

		// Resolved once at Register, from the settings, and checked for
		// containment there so that Start has nothing left to refuse for.
		std::filesystem::path m_weights;
		std::filesystem::path m_exec;

		ChildProcess m_child;
		bool         m_raised{ false };  // only ever touched under m_startLock

		// Read from Submit on the adapter's dispatch thread and written from the
		// reader thread, so they are atomics rather than plain bools under a
		// lock: Submit must return within a few milliseconds and may not wait on
		// anything, a lock the reader can hold included.
		std::atomic_bool m_hello{ false };
		std::atomic_bool m_ready{ false };
		std::atomic_bool m_stopping{ false };

		std::mutex              m_lock;
		std::condition_variable m_wake;
		std::deque<Job>         m_queue;
		std::unordered_map<std::int64_t, Debt> m_debts;

		std::mutex              m_startLock;
		std::condition_variable m_started;

		std::thread m_writer;
		std::thread m_reader;

		std::vector<std::string> m_vocabulary;
	};

	// Every model this mod runs. A static list, never emptied, for the reason at
	// the head of ModelShim.
	std::vector<ModelShim*>& Models();
}
