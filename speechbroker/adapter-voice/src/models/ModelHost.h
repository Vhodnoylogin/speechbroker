#pragma once

#include "../../contract/speechbroker-voice-model.h"

#include "models/Arbiter.h"
#include "models/Collector.h"
#include "models/Guard.h"
#include "models/Model.h"
#include "models/Reputation.h"

#include "turn/Ears.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Voice::Models
{
	// THE HOST: THE SIDE OF THE CONTRACT THE ADAPTER OWNS.
	//
	// It is the only object in this half that knows all the others. It owns the
	// ears, the registry of models, the open collectors, the ledgers of the
	// turns, the one thread that waits and the few that assemble. Everything
	// docs/model-host.md calls an obligation of the adapter is a member of this
	// class or of something it owns.
	//
	// THE GAME THREAD ENTERS THIS CLASS EXACTLY TWICE AND NOWHERE ELSE: Broadcast,
	// out of the SKSE message handler at kDataLoaded, and Register, which a shim
	// calls back out of its own handler. Both are deliberately trivial. Bring-up,
	// Start, Submit, Stop, the deadlines and the assembling all run on threads of
	// ours.

	// What the player may allow. A kind the player has forbidden is refused AT
	// Register AND NOWHERE LATER, before Start is ever called, so a forbidden
	// model never resolves a name, never opens a socket, never starts a process
	// and never contacts anybody: the refusal has to happen before bring-up or it
	// is not a refusal at all.
	//
	// The defaults say what the shipped settings say. Remote is the one a player
	// is entitled to be asked about, because it is the one where the sound leaves
	// the machine - and the declaration is unverifiable, so the log writes the
	// kind out in words at every registration.
	struct KindPolicy
	{
		bool inProcess{ true };
		bool child{ true };
		bool attached{ true };
		bool remote{ false };

		bool Allows(std::uint32_t a_kind) const noexcept
		{
			switch (a_kind) {
			case SPEECHBROKERVOICE_KIND_INPROCESS: return inProcess;
			case SPEECHBROKERVOICE_KIND_CHILD:     return child;
			case SPEECHBROKERVOICE_KIND_ATTACHED:  return attached;
			case SPEECHBROKERVOICE_KIND_REMOTE:    return remote;
			default:                               return false;
			}
		}
	};

	// Everything this half is tuned by, in one place, so that it can be handed
	// over in one argument and so that a test can build it without a settings
	// file and without a game - the same shape and the same reason as
	// EarsSettings (turn/Ears.h).
	//
	// CONFIG OWNS ONE FIELD OF THIS TYPE and fills it from speechbroker-voice.json
	// under a "models" key, beside the existing "ears". NOTHING UNDER models/
	// INCLUDES Config.h, exactly as nothing under audio/ or turn/ does.
	struct HostSettings
	{
		KindPolicy         allow;
		DispatchSettings   dispatch;
		ReputationSettings standing;
		ArbiterSettings    arbiter;

		// How many threads assemble closed passes. Folding, snapping, judging and
		// reconciling all run here; the scheduler only seals. Two is enough for
		// the shipping settings - a turn has at most a handful of passes in
		// flight - and it must be at least one.
		int workers{ 2 };

		// Every dispatch thread, the scheduler and every worker get this. It is
		// DispatchSettings::stackGuaranteeBytes for the model threads; this is
		// the same number for ours, kept separate so that the two can be moved
		// apart the day one of them needs it.
		std::uint32_t stackGuaranteeBytes{ 64u * 1024u };

		// Never ask a model twice for a buffer we would not ask anybody for. It
		// is the same floor CutRules::minSamples enforces inside the ears, and it
		// is stated here only so that the log line that refuses a pass names a
		// number a person can find.
		//
		// IT IS NOT A SECOND COPY OF THE THRESHOLD. The ears refuse first; this
		// is the belt, and a body author must derive it from the ears rather than
		// read it from the file, or the two will disagree on the day somebody
		// edits one of them. See Ears::Settings().
		bool trustEarsFloor{ true };
	};

	// ------------------------------------------------------------------------ //

	// ONE THREAD FOR EVERYTHING THAT WAITS.
	//
	// It holds the armed deadlines and nothing else. When one fires it does
	// EXACTLY TWO THINGS - mark the collector closed, and post it to a worker -
	// and then goes back to waiting. Folding, snapping, judging, reconciling and
	// handing text onward all run on a worker.
	//
	// THE REASON IS NOT TIDINESS. Anything heavy on this thread delays every
	// other armed deadline behind it, which manufactures out-of-order closes as a
	// matter of course - which is exactly what the serial guard in TurnLedger
	// then has to catch. Keeping this thread empty is what keeps that guard a
	// safety net rather than a working part.
	//
	// A collector is held WEAKLY here. A pass that was closed early by its last
	// answer, and assembled, and finished with, must not be kept alive to its
	// deadline by a timer nobody cancelled.
	class Scheduler
	{
	public:
		Scheduler();

		// THREAD: the ears' consumer thread, inside the pass sink. It takes one
		// lock, pushes one entry and signals - it never waits.
		void Arm(Clock::time_point a_when, const std::shared_ptr<Collector>& a_collector);

		// Start and stop the waiting thread. Stop is bounded: the thread's
		// longest wait is to the nearest armed deadline, and it is woken.
		// THREAD: the host's bring-up worker; never the game thread.
		void Begin(std::uint32_t a_stackGuaranteeBytes, std::function<void(std::shared_ptr<Collector>)> a_post);
		void End() noexcept;

	private:
		struct Armed
		{
			Clock::time_point        when;
			std::weak_ptr<Collector> collector;
		};

		std::function<void(std::shared_ptr<Collector>)> _post;

		mutable std::mutex      _lock;
		std::condition_variable _wake;

		// Ordered by time, so the front is always the next to fire. A multimap
		// and not a heap because a heap of weak_ptr cannot be pruned in the
		// middle, and expired entries are the ordinary case here.
		std::multimap<Clock::time_point, std::weak_ptr<Collector>> _armed;

		std::thread      _thread;
		std::atomic_bool _stopping{ false };
	};

	// ------------------------------------------------------------------------ //

	class Host
	{
	public:
		// One per process. The table it publishes is a function-local static with
		// process lifetime and is never replaced, so a shim may keep the pointer
		// forever - which it must, because Complete comes from its own thread
		// hundreds of milliseconds later, Ready at any time and Log after Stop.
		static Host& Get();

		// --- bringing the half up ---------------------------------------------

		// Settings in, and the one thing this half hands outward: a finished
		// slice of speech. A std::function and not a call into Bridge.h, because
		// the half that owns the microphone and the models must not know which
		// bridge it is feeding - the same discipline that keeps Config.h out of
		// audio/ and turn/.
		//
		// THREAD: the game thread, once, before Broadcast. It starts nothing.
		using Publish = std::function<void(const Slice&)>;
		void Configure(HostSettings a_settings, EarsSettings a_ears, Publish a_publish);

		// Build the ears, read the calibration, start the scheduler, start the
		// workers, open the microphone.
		//
		// NEVER FROM THE THREAD OF THE GAME. Ears::Start opens a device and may
		// sit through the reopen delays; reading reputations.json touches the
		// disk. The plugin starts this from a worker, exactly as it already
		// starts its polling thread.
		//
		// False means the sound source did not open. The models are registered
		// and started anyway: a microphone that failed is a reason to say so
		// loudly, not a reason to leave three model mods unloaded.
		bool Begin();

		// --- the handshake ----------------------------------------------------

		// The table. A function-local static, process lifetime, never replaced.
		// THREAD: any.
		static const SpeechBrokerVoiceHost* Table() noexcept;

		// Broadcast it at SKSE's kDataLoaded, dispatching THE ADDRESS OF THE
		// TABLE POINTER with dataLen == sizeof(void*) - the house convention, and
		// exactly what the bridge does towards its own adapters. There is no
		// second broadcast and no entry point to ask for the table: a model that
		// was not listening is simply not registered, is never asked for
		// anything, and costs nobody anything.
		//
		// kDataLoaded and not earlier, on purpose: by then every model plugin has
		// certainly been loaded and has had its chance to subscribe.
		//
		// THREAD: THE GAME THREAD, from the SKSE message handler. This is one of
		// the two places in this half where that is true.
		void Broadcast();

		// --- the registry ------------------------------------------------------

		// REGISTER REFUSES IN THIS ORDER, and the order is the specification:
		//
		//   1. NULL a_info, a_model or a_session                      -> MALFORMED
		//   2. structBytes of all three: a multiple of 8 AND at least
		//      that struct's version-1 size                           -> MALFORMED
		//   3. a_info->abiVersion >= 1                                -> VERSION
		//   4. reserved0 == 0 in BOTH info and model                  -> MALFORMED
		//   5. a non-empty id                                         -> MALFORMED
		//   6. that id is not registered already (FIRST WINS)         -> DUPLICATE
		//   7. Start, Stop and Submit are all non-NULL                -> MALFORMED
		//   8. provides parses to something we know ("asr")           -> MALFORMED
		//   9. the kind is allowed by the player's policy             -> REFUSED
		//
		// WRITE NOTHING INTO a_session ON ANY STATUS BUT OK. A failed Register
		// leaves whatever the shim put there, and reading session.handle after
		// one is the shim's own bug rather than an undefined one.
		//
		// AND THE OUT-PARAMETER RULE IS INVERTED: a_session->structBytes is the
		// size of the buffer THE SHIM allocated. The adapter writes
		// min(that, its own sizeof) bytes and never its own sizeof. A larger
		// buffer than it knows about is perfectly legal.
		//
		// The version settled here is min(theirs, ours) and it governs every
		// struct in the contract that carries no abiVersion of its own - which is
		// what a fragmentStride is later checked against, exactly.
		//
		// THREAD: THE GAME THREAD, during plugin load, from the shim's own SKSE
		// message handler. It copies the info, the table and the strings and
		// returns. It does not bring the model up: Start comes later, on a
		// worker, because weights take seconds and a frame is 11.1 ms.
		std::int32_t Register(const SpeechBrokerVoiceModelInfo* a_info,
			const SpeechBrokerVoiceModel* a_model, SpeechBrokerVoiceSession* a_session);

		// THREAD: any thread of a model's, never the game thread - it blocks
		// until the calls already inside the shim have come back.
		//
		// Marks the handle draining, RELEASES THE REGISTRY LOCK, and only then
		// waits at most stopMs. After that the handle is left draining forever
		// and the model is abandoned. Unregistering also removes the model from
		// every pass still open, which is a removal and not a timeout.
		void Unregister(SpeechBrokerVoiceHandle a_handle);

		// The model whose handle this is, or nullptr. HANDLES ARE MONOTONE AND
		// ARE NEVER REUSED for the life of the process, which is what makes a
		// four-hundred-millisecond answer landing on a dead handle safe rather
		// than a lottery: with reuse it would land on some other model's
		// bookkeeping and the wrong model would be credited or blamed.
		// THREAD: any.
		std::shared_ptr<Model> Find(SpeechBrokerVoiceHandle a_handle) const;

		// --- what a model calls ------------------------------------------------

		// Copy the answer into the slot of its pass, signal, return. It does NOT
		// run the arbitration: the pass is sealed by the timer and assembled
		// afterwards on a worker, so several models calling it at once do not
		// queue behind each other's merging.
		//
		// AN ANSWER IS VALIDATED IN THIS ORDER, and the count and the stride are
		// checked BEFORE `fragments` is dereferenced even once:
		//   structBytes -> abiVersion -> status in {OK, CANCELLED, FAILED} ->
		//   fragmentCount in [0, MAX_FRAGMENTS] -> fragments non-NULL when the
		//   count is above zero -> fragmentStride EXACTLY the size of a fragment
		//   at the model's declared version -> and only then index.
		// Then per fragment: startMs <= endMs, non-decreasing, non-overlapping;
		// times outside the buffer are CLAMPED and logged once rather than
		// refusing a whole reading.
		//
		// THREAD: any thread of a model's, including from inside its own Submit.
		// Never the game thread.
		std::int32_t Complete(SpeechBrokerVoiceHandle a_handle,
			const SpeechBrokerVoiceAnswer* a_answer);

		// THREAD: any thread of a model's, never the game thread.
		void Ready(SpeechBrokerVoiceHandle a_handle, std::int32_t a_ready, const char* a_reason);

		// WRITES THE ID, THEN THE KEY VERBATIM, THEN THE ARGUMENTS IN ORDER, TAB
		// SEPARATED, AND SUBSTITUTES NOTHING. The key belongs to the model's own
		// localisation table with its own prefix; the adapter neither parses nor
		// translates it, because it does not have that table and because
		// otherwise every new model mod would mean an edit to the adapter.
		//
		// A call whose argument count is out of range, or that carries a NULL
		// element, is DROPPED with one line of the adapter's own.
		//
		// THREAD: any thread of a model's, never the game thread - it formats and
		// allocates.
		void Log(SpeechBrokerVoiceHandle a_handle, std::int32_t a_level, const char* a_key,
			const char* const* a_args, std::int32_t a_argCount);

		// --- the passes --------------------------------------------------------

		// THE SINK THE EARS CALL. This is where a pass becomes a collector.
		//
		// It does exactly this and nothing else:
		//   take the next utteranceId; build the collector with the roster of
		//   this pass as its expected set and ONE ABSOLUTE DEADLINE stamped now;
		//   put it in the open map; arm the scheduler; offer one entry to each
		//   model in the roster; return.
		//
		// NO Submit HAPPENS HERE. Every Submit is made on that model's own
		// dispatch thread, which is what lets this function be entered on the
		// thread the microphone is being drained on: every step above is a lock
		// held for a few instructions and a shared_ptr copied.
		//
		// THE DEADLINE IS STAMPED HERE, AT PASS CREATION, and the deadlineMs a
		// model is handed is this minus now, computed at the instant Submit is
		// entered. A deadline measured from the model's own call while the timer
		// enforcing it was armed at pass creation is two different clocks wearing
		// one name.
		//
		// THE ROSTER, in order:
		//   - the pass is refused outright if nothing is registered, or if no
		//     model is in the roster; the serial is spent either way and the gap
		//     in the numbers is the record that it happened;
		//   - a FINAL pass asks everybody who is live and proven;
		//   - an INTERIM pass asks the fast ones. finalOnly models are ALWAYS
		//     excluded. IF THE INTERIM ROSTER WOULD BE EMPTY BECAUSE EVERY
		//     INSTALLED MODEL IS finalOnly, run no interim passes at all and say
		//     so ONCE - do not ask a finalOnly model anyway for want of anybody
		//     else, which would break the one declaration measurement does not
		//     override.
		//
		// THREAD: the ears' consumer thread. It must not block, must not call
		// back into the ears, and the body must wrap the whole of it in a
		// catch(...): an exception escaping a thread procedure is std::terminate.
		void OnPass(const Pass& a_pass);

		// RETIREMENT IS MEMBERSHIP IN THIS MAP AND NEVER A HIGH-WATER MARK. An
		// utteranceId that names no open collector is retired, and that is one
		// lookup. A high-water mark is wrong in two ways at once: a pass that
		// closes early would retire the answers of passes still legitimately
		// open, and it would retire the probe, whose utteranceId is issued before
		// any real turn and whose collector is meant to outlive them all.
		//
		// The set cannot grow without bound, because every timed collector has a
		// deadline and a timer that fires it.
		// THREAD: any.
		std::shared_ptr<Collector> Collecting(std::int64_t a_utteranceId) const;

		// --- the vocabulary ----------------------------------------------------

		// Merged across every installed subscriber by the bridge, clipped HERE to
		// SPEECHBROKERVOICE_MAX_VOCABULARY - the list is otherwise unbounded -
		// and delivered on each model's own dispatch thread, never before its
		// Start returned OK and never after its Stop. The order is the merge
		// order and means nothing; it is not a ranking.
		// THREAD: any. The game thread may call it - it only copies strings and
		// signals - but it is better on a worker.
		void SetVocabulary(std::vector<std::string> a_phrases);

		// --- what the rest of the plugin may ask -------------------------------

		// The ears, for the log and for the two numbers the session publishes.
		// nullptr before Begin.
		// THREAD: any.
		const Ears* Listening() const noexcept { return _ears.get(); }

		const HostSettings& Settings() const noexcept { return _settings; }

		// One line's worth of every standing. THREAD: any.
		std::vector<Reputation::Numbers> Report() const;

	private:
		Host();

		Host(const Host&) = delete;
		Host(Host&&) = delete;
		Host& operator=(const Host&) = delete;
		Host& operator=(Host&&) = delete;

		// THE FIVE ENTRY POINTS OF THE TABLE.
		//
		// They are static and carry the contract's own calling convention, which
		// is stated and not assumed: on x64 the switch that genuinely changes how
		// a call is made is /Gv (__vectorcall), and a model built with it would
		// hand this contract's float arguments to the wrong place. Under MSVC
		// language linkage is not part of a function type, so a static member
		// function assigns to these C function pointers without a cast.
		//
		// EACH ONE CARRIES ITS OWN POD GUARD, because this direction runs on the
		// MODEL'S thread over pointers the shim supplied, and a stale char* from
		// third-party code is the most probable in-process fault in the whole
		// design. Each builds a POD context on its stack, runs the body behind
		// GuardedHostCall, and wraps that call in catch(...). A fault ejects that
		// model for the session and returns the status the contract names for
		// that call - it never returns as if nothing had happened.
		static std::int32_t SPEECHBROKERVOICE_CALL OnRegister(
			const SpeechBrokerVoiceModelInfo* a_info,
			const SpeechBrokerVoiceModel* a_model, SpeechBrokerVoiceSession* a_session);
		static void SPEECHBROKERVOICE_CALL OnUnregister(SpeechBrokerVoiceHandle a_handle);
		static std::int32_t SPEECHBROKERVOICE_CALL OnComplete(SpeechBrokerVoiceHandle a_handle,
			const SpeechBrokerVoiceAnswer* a_answer);
		static void SPEECHBROKERVOICE_CALL OnReady(SpeechBrokerVoiceHandle a_handle,
			std::int32_t a_ready, const char* a_reason);
		static void SPEECHBROKERVOICE_CALL OnLog(SpeechBrokerVoiceHandle a_handle,
			std::int32_t a_level, const char* a_key,
			const char* const* a_args, std::int32_t a_argCount);

		// THE PROBE. One second of digital silence submitted as an ORDINARY
		// request straight after a successful Start: its own turnId and serial,
		// final == 1 because the buffer will not grow, and deadlineMs 0 because
		// it is not timed.
		//
		// ITS COLLECTOR LIVES IN _probes AND NOT IN _open, which is the whole
		// reason there are two maps: a probe is retired only when its model
		// unregisters or the session ends, and a high-water mark over utterance
		// ids would retire it the moment any later pass closed - silently
		// disabling the one check in this system that does not rest on trusting a
		// model.
		//
		// THREAD: the bring-up worker, or the model's own dispatch thread right
		// after Start returned OK.
		void SubmitProbe(const std::shared_ptr<Model>& a_model);

		// ASSEMBLE A CLOSED PASS. Everything heavy is here and nowhere else:
		// measure the terminal fall on the snapshot, fold the readings, take the
		// turn's ledger, run the serial guard and the reconciliation under its
		// lock, release the debts, charge the timeouts, and publish.
		// THREAD: a worker.
		void Assemble(std::shared_ptr<Collector> a_collector);

		// The ledger of this turn, made on first sight. Old ledgers are dropped
		// when their turn is finished and no collector of it is open.
		// THREAD: a worker.
		std::shared_ptr<TurnLedger> Ledger(std::int64_t a_turnId);

		HostSettings _settings;
		EarsSettings _earsSettings;
		Publish      _publish;

		// Built in Begin, never on the game thread. THE HOST OWNS THE EARS - one
		// microphone per game, and the side that publishes maxRequestSamples and
		// the format at Register has to be the same side that enforces them.
		std::unique_ptr<Ears> _ears;

		// One holder of the standings in the process.
		std::unique_ptr<Reputations> _standings;
		std::unique_ptr<Arbiter>     _arbiter;

		// --- the registry -----------------------------------------------------

		// Guards _byHandle and _byId, and NOTHING SLOW EVER RUNS UNDER IT. In
		// particular Unregister releases it before it waits.
		mutable std::mutex _registry;

		// Never erased. A model that unregistered is left draining in place, for
		// the reason at the head of class Model.
		std::unordered_map<SpeechBrokerVoiceHandle, std::shared_ptr<Model>> _byHandle;

		// For the duplicate-id refusal. First registration wins.
		std::unordered_map<std::string, SpeechBrokerVoiceHandle> _byId;

		// 0 IS NOT A HANDLE, and handles are never reused.
		std::atomic<std::uint32_t> _nextHandle{ 1 };

		// Monotone, never 0, never reused. It identifies an answer and nothing
		// else does.
		std::atomic<std::int64_t> _nextUtterance{ 1 };

		// The probe's turn ids, which belong to no speaking turn. Negative, so
		// that they can never collide with a turnId out of the ears - which
		// start at 1 and only rise - and so that a log line naming one is
		// recognisable at a glance.
		std::atomic<std::int64_t> _nextProbeTurn{ -1 };

		// --- the passes -------------------------------------------------------

		mutable std::mutex _collectors;

		// Timed passes. Retirement is membership here.
		std::unordered_map<std::int64_t, std::shared_ptr<Collector>> _open;

		// Untimed ones: at most one per model, so a small explicit set costs
		// nothing and cannot grow. Erased only on Unregister or at the end of the
		// session.
		std::unordered_map<std::int64_t, std::shared_ptr<Collector>> _probes;

		mutable std::mutex _ledgers;
		std::unordered_map<std::int64_t, std::shared_ptr<TurnLedger>> _byTurn;

		// One second of digital silence, made once and shared by every probe of
		// the session. It is an immutable Snapshot like any other, so every model
		// gets the same pointer and the claim rules apply to it unchanged.
		Snapshot _probeAudio;

		// --- the threads ------------------------------------------------------

		Scheduler _scheduler;

		mutable std::mutex                      _work;
		std::condition_variable                 _workWake;
		std::vector<std::shared_ptr<Collector>> _queue;
		std::vector<std::thread>                _workers;
		std::atomic_bool                        _stopping{ false };
	};
}
