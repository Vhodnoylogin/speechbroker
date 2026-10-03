#pragma once

#include "../../contract/speechbroker-voice-model.h"

#include "models/Collector.h"
#include "models/Guard.h"
#include "models/Reputation.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Voice::Models
{
	class Host;

	// WHAT THE ADAPTER KNOWS HOW TO ASK FOR. At contract version 1 there is
	// exactly one value and "asr" is it; anything else in a model's `provides` is
	// logged and ignored, and a registration whose provides names nothing we know
	// is refused MALFORMED. NULL provides is read as "asr".
	//
	// A BITMASK AND NOT A bool, because the day a second value appears the shape
	// of the field must not have to change - and because the refusal above has to
	// be able to say "nothing I know", which needs a zero that means zero.
	enum Provides : std::uint32_t
	{
		kProvidesNothing = 0,
		kProvidesAsr = 1u << 0
	};

	// The tuned numbers of one model's life. Settings, every one of them, with
	// defaults that are the shipping behaviour.
	struct DispatchSettings
	{
		// How many times Start is tried before the model is out for the session.
		// RETRY and NOT_READY are both recoverable and are treated alike; the
		// wait between attempts is the model's own startupMs. Five is the
		// contract's stated shipping default and the contract says a player may
		// change it, so it is here (contract, Model::Start).
		int startAttempts{ 5 };

		// What budgetMs 0 means, and what maxInFlight 0 means - the contract
		// fixes the second at 1 and leaves the first to us.
		int defaultBudgetMs{ 2000 };

		// The slack added to the declared budget until the measured p90 exists.
		// engine/engine.py:126.
		int bootstrapSlackMs{ 1000 };

		// How much digital silence the probe carries. One second, as the contract
		// states it (contract, "The probe, and why it carries no flag").
		double probeSeconds{ 1.0 };

		// The sliding window the busy rate is measured over, and the rate above
		// which a model is demoted.
		int    busyWindow{ 20 };
		double busyRateLimit{ 0.5 };

		// SetThreadStackGuarantee on this model's dispatch thread, so that a
		// stack overflow inside third-party code still leaves room for the filter
		// and the log line.
		std::uint32_t stackGuaranteeBytes{ 64u * 1024u };

		// A Submit that takes longer than this is logged. It is NOT failed - the
		// contract only promises it is measured - but a shim that keeps
		// overrunning has to be distinguishable from a slow model.
		int submitOverrunMs{ 5 };

		// How long past startupMs a Start may run before one line says so. There
		// is NO timeout on a hung Start: the adapter cannot kill a thread inside
		// third-party code and will not pretend it can, so it waits on this
		// model's own dispatch thread, where nothing else is waiting.
		int startComplaintMs{ 0 };  // 0 - use the model's own startupMs
	};

	// THE ADAPTER'S OWN COPY OF WHAT A MODEL DECLARED.
	//
	// Every string here was read out of the model's memory with strnlen against
	// SPEECHBROKERVOICE_MAX_STRING_BYTES and truncated rather than refused, inside
	// the Register call, because what was handed over is borrowed for the length
	// of that call and no longer. The scalars are already normalised: budgetMs 0
	// has become the adapter's default, maxInFlight 0 has become 1, declaredClass
	// 0 has become Accurate - the cautious reading, which costs the model interim
	// passes rather than costing the player a late draft.
	struct ModelInfo
	{
		std::string id;        // short, no spaces; every answer is signed with it
		std::string name;      // for a person, in the log; id when the model sent none
		std::string language;  // first BCP-47 subtag. LOGGED AND ROUTED ON BY NOTHING at version 1
		std::string providesRaw;  // as sent, for the log, so an unknown value is visible

		std::uint32_t provides{ kProvidesNothing };
		std::uint32_t abiVersion{ 0 };  // the version settled at Register: min(theirs, ours)
		std::uint32_t kind{ 0 };        // SpeechBrokerVoiceKind, declared and unverifiable

		std::uint32_t budgetMs{ 0 };
		std::uint32_t startupMs{ 0 };
		std::uint32_t stopMs{ 0 };
		std::uint32_t maxInFlight{ 1 };

		bool  finalOnly{ false };
		Speed declaredClass{ Speed::Accurate };

		// Does the sound leave this machine. Derived once from kind through the
		// contract's own macro, so that a kind added later cannot answer the
		// player's question by accident.
		bool LeavesMachine() const noexcept
		{
			// Through the contract's own macro and never by comparing to
			// SPEECHBROKERVOICE_KIND_REMOTE here, so that a kind added in a later
			// version cannot answer the player's question by accident. The cast
			// is only so that /W4 /WX does not see an unsigned field compared
			// against an int enumerator.
			return SPEECHBROKERVOICE_KIND_LEAVES_MACHINE(static_cast<int>(kind)) != 0;
		}
	};

	// WHERE A MODEL IS IN ITS LIFE. One atomic, because Complete, Ready and Log
	// on a draining, dead or unknown handle must read ONE ATOMIC AND RETURN AT
	// ONCE - that is what makes Complete unable to deadlock against Unregister.
	enum class Life : std::int32_t
	{
		Registered = 0,  // accepted; its thread has not reached Start yet
		Starting = 1,    // inside Start, or waiting out startupMs between attempts
		Probing = 2,     // Start returned OK; the silence probe is out and unanswered
		Live = 3,        // proven by the probe; takes part in passes
		NotReady = 4,    // it said Ready(handle, 0); nothing is submitted until Ready(1)
		Draining = 5,    // Unregister is running, or gave up and left it draining forever
		Ejected = 6      // out for the session: a fault, a Start that failed, busy past tolerance
	};

	// Why a model was put out, for the one loud log line that has to accompany
	// it. Working quietly without the accurate model turns every later question
	// about quality into guesswork.
	enum class Ejection : std::int32_t
	{
		StartRefused = 0,   // Start returned something other than OK, RETRY or NOT_READY
		StartExhausted = 1, // DispatchSettings::startAttempts went by
		Faulted = 2,        // __except caught it: locks held, state of unknown shape
		BusyOnFinal = 3,    // demoted to final-only and still busy past the limit
		ProtocolBroken = 4  // Complete and then BUSY, or another rule it cannot be talked out of
	};

	// THE BUSY RATE, IN A WINDOW, AND WHAT IT MAY NOT TOUCH.
	//
	// BUSY costs a model nothing as a failure: it owes nothing, nothing is
	// counted against it, and its weight in an argument is untouched. It is not
	// free either. A model whose busy rate stays above the limit is taken off the
	// INTERIM roster and offered final passes only; a model that is busy on those
	// too is dropped for the session. One log line at each step.
	//
	// Otherwise a model that is busy on every pass looks healthy for ever: it
	// never fails, never times out, never accumulates the latency samples that
	// would reclassify it, and contributes nothing.
	//
	// AND IT MUST NOT TOUCH THE FAILURE NUMERATOR OR DENOMINATOR. Counting the
	// calls alone would RAISE a busy model's weight, because Weight() divides
	// failures by calls. That is why this window lives here, in the dispatch
	// side, and not in Reputation - the separation is the guarantee
	// (docs/model-host.md, "The busy rate").
	//
	// THREAD: the model's own dispatch thread writes it; anything may read it.
	// Guarded by Model::_lock, not by a lock of its own.
	class BusyWindow
	{
	public:
		explicit BusyWindow(int a_size);

		// One Submit outcome. a_busy is true only for SPEECHBROKERVOICE_BUSY;
		// every other return, and a fault, is false.
		void Note(bool a_busy);

		// Busy calls over observations, 0 while the window is empty. There is no
		// "not enough samples" fallback and there must not be one: the window is
		// small on purpose, and a model that answers BUSY to its first twenty
		// requests is exactly the case this exists to catch.
		double Rate() const noexcept;

		std::size_t Observations() const noexcept { return _filled; }

	private:
		std::vector<std::uint8_t> _ring;
		std::size_t               _at{ 0 };
		std::size_t               _filled{ 0 };
		std::uint32_t             _busy{ 0 };
	};

	// ONE QUEUED REQUEST: A PASS WAITING TO BE HANDED TO THIS MODEL.
	//
	// THE SNAPSHOT IS THE POINT OF THIS TYPE. The contract promises a model that
	// `samples` is an immutable snapshot valid for the whole of the Submit call,
	// and that its lifetime IS A CLAIM, NOT AN EVENT: the moment a pass is queued
	// for a model, that entry takes a claim on the snapshot and releases it
	// either when Submit returns or when the entry is dropped unsent. The buffer
	// dies when the last claim does. The earlier wording - "alive until the last
	// Submit of this utteranceId has returned" - named an event that never occurs
	// for a queued copy that is replaced before it is ever sent.
	//
	// So `audio` is held here BY VALUE and not read out of the collector at the
	// moment of the call. Holding the collector would keep the snapshot alive
	// transitively, which is true and invisible; this is the claim the contract
	// talks about, in the type, where it cannot be lost by accident.
	struct DispatchEntry
	{
		// THE CLAIM. Released when this entry is destroyed, and by then either
		// Submit has returned or the entry was dropped.
		Snapshot audio;

		// The pass this belongs to. Held so that the dispatch thread can ask
		// IsClosed() before entering Submit, take the one absolute deadline out
		// of it, and Remove() itself from the expected set when it drops the
		// entry.
		std::shared_ptr<Collector> collector;

		// Copied out of the collector so that the request can be filled without
		// touching it again - and so that a dropped entry can still be logged
		// with the numbers it would have carried.
		std::int64_t  utteranceId{ 0 };
		std::int64_t  turnId{ 0 };
		std::int32_t  serial{ 0 };
		bool          final{ false };
		std::uint32_t lostSamples{ 0 };

		// The probe belongs to no turn, carries deadlineMs 0 and is never retired
		// by anything another pass does. It is an ORDINARY request in every field
		// a shim can see - there is no probe flag in the contract and there must
		// never be one - and this flag exists only so that our own bookkeeping
		// can keep its outcome out of the latency samples and the timeouts.
		bool probe{ false };

		std::uint32_t SampleCount() const noexcept
		{
			return audio ? static_cast<std::uint32_t>(audio->size()) : 0U;
		}
	};

	// What Offer did with what it was given, so that the caller's log line names
	// the right thing.
	enum class Queued : std::int32_t
	{
		Accepted = 0,          // the slot was empty
		AcceptedReplacing = 1, // a lower serial of the same turn was dropped for it
		Refused = 2            // the new entry itself was dropped
	};

	// ONE REGISTERED MODEL: ITS DECLARATION, ITS TABLE, ITS THREAD, ITS DEBT, ITS
	// BUSY WINDOW AND ITS STANDING.
	//
	// ONE DISPATCH THREAD PER MODEL, AND THAT IS THE WHOLE CONCURRENCY DESIGN OF
	// THIS HALF. Every call into the table - Start, Stop, SetVocabulary, Submit,
	// Cancel - is made on this thread and on no other, so calls to one model are
	// serialised against each other and a shim that misbehaves in one of them
	// STARVES ONLY ITSELF. That is the structural form of the rule that one
	// broken model mod must not cost a person the other two.
	//
	// NOTHING HERE EVER TOUCHES THE THREAD OF THE GAME. Not Start, not Submit,
	// not Stop. The one call the game thread makes anywhere in this half is
	// Host::Register, which is deliberately trivial.
	//
	// A MODEL OBJECT IS NEVER DESTROYED, AND THAT IS DELIBERATE. When Unregister
	// or Stop gives up waiting, the adapter does not kill the thread - killing a
	// thread inside third-party code leaves every lock it held permanently held,
	// including CRT and loader locks, which turns a hung recogniser into a hung
	// game and, at exit, into the case where MO2 still believes the game is
	// running. It stops waiting, writes one line, and abandons it. The per-model
	// state an abandoned thread can still reach is therefore never freed, so
	// returning late into it is not a use-after-free. The registry holds a
	// shared_ptr it never drops.
	class Model
	{
	public:
		// THE ONLY WAY TO MAKE ONE, and it hands back a shared_ptr WHOSE DELETER
		// DOES NOTHING. That is not a leak that slipped in; it is the rule above
		// expressed in the type. The destructor is private, so the ordinary
		// shared_ptr - the one that would call delete - does not compile, and
		// nobody can quietly free a model whose abandoned thread may still return
		// into it.
		//
		// Built inside Register, on the game thread, out of the copies made
		// there. It starts NO thread: Start runs on a worker, later, because
		// weights take seconds and Register may not.
		static std::shared_ptr<Model> Make(SpeechBrokerVoiceHandle a_handle, ModelInfo a_info,
			const SpeechBrokerVoiceModel& a_table, DispatchSettings a_settings,
			Reputation& a_standing, Host& a_host);

		Model(const Model&) = delete;
		Model(Model&&) = delete;
		Model& operator=(const Model&) = delete;
		Model& operator=(Model&&) = delete;

		// --- what it is, all immutable after construction ----------------------

		SpeechBrokerVoiceHandle Handle() const noexcept { return _handle; }
		const ModelInfo&        Info() const noexcept { return _info; }
		const std::string&      Id() const noexcept { return _info.id; }

		// --- where it is ------------------------------------------------------

		// ONE ATOMIC. Complete, Ready and Log read this and return at once on a
		// draining, dead or unknown handle.
		// THREAD: any.
		Life Where() const noexcept { return _life.load(std::memory_order_acquire); }

		// Has the probe been answered. A MODEL TAKES PART IN NO PASS UNTIL ITS
		// PROBE ANSWER HAS ARRIVED - a model that never answers is never asked
		// for anything, with one log line. Otherwise ignoring the probe would
		// leave the invention count at zero out of zero, which reads as a perfect
		// record, while the unproven model voted at full weight.
		// THREAD: any.
		bool Proven() const noexcept { return _proven.load(std::memory_order_acquire); }

		// MAY THIS MODEL BE PUT IN THE ROSTER OF THIS PASS.
		//
		//   Live, and proven, and not draining or ejected; and
		//   for an INTERIM pass additionally: not finalOnly - which is BINDING
		//   and is the one declaration measurement does not override, because it
		//   is about cost and not about speed - and not demoted by the busy rate,
		//   and Class() == Fast.
		//
		// The final pass of a turn asks everybody.
		// THREAD: the ears' consumer thread, inside the pass sink.
		bool TakesPass(bool a_final) const noexcept;

		// Measured once ReputationSettings::latencySamplesNeeded samples exist,
		// declared before that - and since latencies are not carried between
		// runs, "before that" is the start of every session.
		// THREAD: any.
		Speed Class() const;

		// The per-model deadline for a timed pass: budgetMs + slack until the
		// samples exist, then the measured p90.
		// THREAD: any.
		std::int32_t BudgetMs() const;

		// --- the queue of at most one unsent request ---------------------------

		// OFFER A PASS. At most one unsent request is queued for a model, and the
		// two bounds on replacing it are the contract, not niceties:
		//
		//   A REPLACEMENT NEVER CROSSES A TURN. Turn 6's first pass is different
		//   speech from turn 5's, not a longer reading of it, and replacing one
		//   with the other would throw away a turn nobody ever answered. Without
		//   the turnId condition the replacement silently discards the final pass
		//   of one turn in favour of the first pass of the next.
		//
		//   A PASS WITH final == 1 IS NEVER REPLACED. It is the only pass whose
		//   loss is not recoverable by a later one.
		//
		// So: accepted into an empty slot; accepted over a HIGHER SERIAL OF THE
		// SAME TURN whose final is not set; refused otherwise, and then the NEW
		// entry is the one dropped.
		//
		// WHICHEVER ENTRY IS DROPPED IS RECORDED TWICE, NOT ONCE, and this
		// function does both records itself because it is the only place that
		// knows which entry that was: against the model in the same ledger as a
		// timeout (Reputation::NoteDropped), AND removed from that pass's
		// expected set (Collector::Remove, Removal::DroppedUnsent). Record only
		// the first and the pass waits for a request that was never sent; record
		// only the second and a model that is always too slow to be submitted to
		// looks statistically perfect.
		//
		// The model is not told and owes nothing for a request it never received.
		//
		// IT NEVER BLOCKS AND NEVER CALLS INTO THE MODEL. It takes _lock, moves a
		// shared_ptr or two, and signals. That is what lets it be called from the
		// thread the microphone is being drained on.
		//
		// THREAD: the ears' consumer thread, inside the pass sink; and the host
		// worker for the probe.
		Queued Offer(DispatchEntry&& a_entry);

		// --- the debt ---------------------------------------------------------

		// maxInFlight IS READ AGAINST THE DEBT AND NOT AGAINST THE QUEUE: Submit
		// is not entered while that many Completes are already outstanding.
		// Without this the field is a promise backed by nothing, and a shim that
		// declared its true depth would still have to answer BUSY.
		// THREAD: any.
		bool DebtHasRoom() const;

		// THE DEBT IS RECORDED BEFORE THE CALL, NOT BY THE RETURN VALUE. That
		// ordering is what makes Complete legal from inside Submit - an
		// in-process model that answers at once should not have to own a thread
		// to do it - because the answer then always arrives against an utterance
		// the adapter already knows.
		//
		// The consequence is a rule the caller enforces: DO NOT CALL Complete AND
		// THEN RETURN BUSY OR NOT_READY. That is a protocol violation, it is
		// logged, and it is counted.
		//
		// THREAD: this model's dispatch thread only.
		void TakeDebt(std::int64_t a_utteranceId);

		// Discharge it. Four things do: a Complete of any status; a non-OK return
		// from Submit; a fault inside Submit; and the close of the pass at its
		// deadline, after which the answer is STALE and the model may stop
		// carrying it - without the model doing anything at all.
		// Returns false when that utterance was not owed, which is how a second
		// Complete for one utteranceId is caught.
		// THREAD: any.
		bool ReleaseDebt(std::int64_t a_utteranceId);

		// --- what the host tells it -------------------------------------------

		// Queue a SetVocabulary. Delivered ON THIS MODEL'S OWN DISPATCH THREAD,
		// never before Start returned OK and never after Stop. The list has
		// already been clipped to SPEECHBROKERVOICE_MAX_VOCABULARY by the host,
		// because it is merged across every installed subscriber and would
		// otherwise have no bound at all.
		// THREAD: any.
		void SetVocabulary(std::vector<std::string> a_phrases);

		// Queue an advisory Cancel. The model STILL owes the Complete. It is
		// delivered on the serialised dispatch thread, so it queues behind
		// whatever that thread is already doing and arrives, in the normal case,
		// after the model has already paid. For a model across a network it is
		// close to useless, and that is a reason finalOnly exists rather than a
		// defect to be fixed by making Cancel re-entrant.
		// Does nothing when the table has no Cancel - it may be NULL.
		// THREAD: any.
		void Cancel(std::int64_t a_utteranceId);

		// The probe answer arrived. Its outcome feeds the invention record ONLY -
		// never a latency sample, never a timeout - because the first inference
		// of a session pays for workspace allocation and autotune and can take
		// seconds where the steady state is a tenth of one.
		// THREAD: the assembling worker.
		void ProbeAnswered(bool a_invented);

		// The model said Ready(handle, ready, reason). Going not-ready REMOVES it
		// from every pass still open, as a removal and not a timeout. There is no
		// re-registration, ever: a model that never comes back is a model that is
		// never asked, and recognition carries on with whatever else is
		// installed.
		// THREAD: any model thread, from inside Host::Ready.
		void NoteReady(bool a_ready);

		// Out for the session, loudly, with its id and the reason. The others
		// carry on untouched. Idempotent.
		// THREAD: any.
		void Eject(Ejection a_why);

		// --- its thread -------------------------------------------------------

		// Start the dispatch thread. It does, in this order and on itself:
		// PrepareDispatchThread; Start up to startAttempts times with the model's
		// own startupMs between them, with NO TIMEOUT on a hung Start and one log
		// line once it has run longer than startupMs; then the silence probe;
		// then the queue, forever.
		//
		// A Start that returns OK long afterwards is honoured if the model has
		// not been ejected, and ignored with one log line if it has.
		//
		// THE THREAD ENTRY FUNCTION CARRIES A catch(...) AT ITS TOP. An exception
		// escaping a thread procedure is std::terminate, and that is a fail-fast
		// nothing in the process observes.
		//
		// IT DETACHES THE THREAD, and that follows from the rule at the head of
		// this class rather than being a separate decision: a Model is never
		// destroyed, so there is no destructor that could join, and the one place
		// that waits - WaitDrained - is explicitly allowed to give up and abandon
		// the thread. A joinable std::thread member that nobody may ever join is a
		// std::terminate waiting for the day somebody adds a destructor.
		//
		// THREAD: the host's bring-up worker. Never the game thread.
		void Begin();

		// HAS THE DISPATCH THREAD LEFT ITS LOOP. WaitDrained polls this with a
		// bounded sleep rather than joining, because joining is exactly what the
		// rule above forbids: the thread may be inside third-party code that never
		// returns, and then the wait has to end while the thread does not.
		//
		// TRUE BEFORE Begin, deliberately. A model whose thread was never started
		// has trivially left a loop it never entered, and without this an
		// Unregister before bring-up would sit out the whole of stopMs waiting for
		// a thread that does not exist.
		// THREAD: any.
		bool Done() const noexcept { return _done.load(std::memory_order_acquire); }

		// Mark the handle draining. THE REGISTRY LOCK IS RELEASED BEFORE THE
		// WAIT, always: Complete, Ready and Log on a draining handle read one
		// atomic and return at once, so Complete never blocks on Unregister and
		// the two cannot deadlock against each other.
		// Returns false when it was already draining or dead.
		// THREAD: any, but NEVER the game thread - see WaitDrained.
		bool BeginDrain();

		// Wait at most a_stopMs for the dispatch thread to leave the model's
		// code. After that the handle is left draining FOREVER and the model is
		// abandoned: not killed, not joined, not freed.
		//
		// NEVER ON THE GAME THREAD. Nothing in the contract bounds how long one
		// of a model's own calls may take, so an SKSE message handler may only
		// START an unregistration, on a thread of its own.
		// THREAD: any worker.
		bool WaitDrained(std::uint32_t a_stopMs);

		// --- for the log -------------------------------------------------------

		const Reputation& Standing() const noexcept { return _standing; }
		double            BusyRate() const;

		// THE TWO "ONCE PER MODEL" LATCHES OF Host::Complete, and they are here and
		// not there because "once" is a property of the MODEL and not of the call:
		// docs/model-host.md asks for the truncation to be logged "once per model,
		// not once per string", and the clamping the same way. A model that sends a
		// hundred over-long fragments in one answer, and another hundred in the
		// next, is worth exactly one line either way.
		//
		// Each returns true the FIRST time and false ever after.
		// THREAD: any model's thread, from inside Host::Complete.
		bool FirstTruncation() noexcept;
		bool FirstClamp() noexcept;

	private:
		Model(SpeechBrokerVoiceHandle a_handle, ModelInfo a_info,
			const SpeechBrokerVoiceModel& a_table, DispatchSettings a_settings,
			Reputation& a_standing, Host& a_host);

		// PRIVATE AND NEVER CALLED. A model object is not destroyed for the life
		// of the process - see the note at the head of this class. It is declared
		// so that an accidental `delete` fails here, with this line in the
		// diagnostic, rather than deep inside a template.
		~Model();

		// What the dispatch thread finds on its queue besides a request.
		enum class ControlKind : std::int32_t
		{
			Vocabulary = 0,
			Cancel = 1,
			Stop = 2
		};

		struct Control
		{
			ControlKind              kind{ ControlKind::Stop };
			std::vector<std::string> phrases;      // Vocabulary
			std::int64_t             utteranceId{ 0 };  // Cancel
		};

		// THE DISPATCH THREAD, IN PIECES. It is one thread and one story - Start,
		// the probe, then the queue for ever - and it is cut into these because
		// every one of them holds a crossing into third-party code inside its own
		// try/catch, and a single function carrying five of them would be a page
		// where the retraction of one outcome could be read as the retraction of
		// another. They are called from Run and from nowhere else.
		//
		// a_faulted travels down and back rather than being read off _life,
		// because "ejected" does not say WHY, and Stop is owed by a model that was
		// ejected for refusing to start and not by one that faulted.
		void Run();
		bool RunStart(bool& a_faulted);   // true when Start returned OK
		void WaitBetweenStarts();
		void RunQueue(bool& a_faulted);
		bool RunControl(Control& a_control, bool& a_faulted);  // false - leave the loop
		void RunEntry(DispatchEntry&& a_entry, bool& a_faulted, int& a_protocolBreaks);

		// Complete came and then Submit said BUSY or NOT_READY for the same
		// utterance. One is a defect; two is Ejection::ProtocolBroken.
		void NoteProtocolBreak(std::int64_t a_utteranceId, int& a_protocolBreaks);

		// On the way out: whatever is still queued belongs to a pass that is still
		// waiting for it, and a pass must not sit to its deadline for a request
		// nobody will ever send.
		void DropPending();

		const SpeechBrokerVoiceHandle _handle;
		const ModelInfo               _info;

		// THE TABLE IS COPIED, never pointed at: the contract lets a shim build
		// it on the stack and says the adapter copies it. The user pointer inside
		// it is handed back untouched in every call and is never looked into.
		const SpeechBrokerVoiceModel _table;

		DispatchSettings _settings;

		// The standing is a reference into Reputations, which never erases and
		// never rehashes a record out from under this. One holder, one record.
		Reputation& _standing;

		// The owner. A back-pointer rather than a callback table because there is
		// exactly one host in the process and a Model outlives nothing it could
		// dangle on. The body includes ModelHost.h; this header does not, which
		// is what keeps the two out of an include cycle.
		Host& _host;

		std::atomic<Life> _life{ Life::Registered };
		std::atomic_bool  _proven{ false };

		// Latched, so that the two demotion steps each say their line once.
		std::atomic_bool _busyDemoted{ false };

		// The two latches behind FirstTruncation and FirstClamp. Atomics rather
		// than fields of _lock's estate because Complete arrives on the model's own
		// threads, several at once is ordinary, and a log line must never be the
		// reason a lock is taken.
		std::atomic_bool _saidTruncated{ false };
		std::atomic_bool _saidClamped{ false };

		// See Done(). It starts TRUE and Begin clears it before it launches the
		// thread, so that "never started" and "has left" read alike to a waiter.
		std::atomic_bool _done{ true };

		// Guards the queue, the debt and the busy window. NEVER HELD ACROSS A
		// CALL INTO THE MODEL - the adapter holds no lock of its own across any
		// crossing, which is what lets a shim answer from inside the very call it
		// was given.
		mutable std::mutex      _lock;
		std::condition_variable _wake;

		// AT MOST ONE UNSENT REQUEST. Not a queue: a slot.
		std::optional<DispatchEntry> _pending;

		// Control messages are a queue, because they are ordered against each
		// other and against nothing else. Small and rare.
		std::deque<Control> _control;

		// The outstanding utteranceIds. A vector because maxInFlight is small -
		// one for almost every shim - and a linear scan beats a hash at that
		// size under a lock taken from several threads.
		std::vector<std::int64_t> _debt;

		BusyWindow _busy;

		std::thread      _thread;
		std::atomic_bool _stopping{ false };

		// True while the thread is inside the model's code. Read by WaitDrained,
		// which is why it is an atomic and not a field of _lock's estate.
		std::atomic_bool _inside{ false };
	};
}
