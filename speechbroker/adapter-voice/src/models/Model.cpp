#include "models/Model.h"

#include "models/ModelHost.h"

#include "Loc.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Voice::Models
{
	namespace
	{
		// WHY A MODEL WAS PUT OUT, IN WORDS. The loud line that accompanies an
		// ejection has to name the reason, because working quietly without the
		// accurate model turns every later question about quality into guesswork
		// (Model.h, enum Ejection).
		const char* EjectionWord(Ejection a_why)
		{
			switch (a_why) {
			case Ejection::StartRefused:   return Loc::Get("$SPEECHBROKERVOICE_WORD_EJECT_START_REFUSED");
			case Ejection::StartExhausted: return Loc::Get("$SPEECHBROKERVOICE_WORD_EJECT_START_EXHAUSTED");
			case Ejection::Faulted:        return Loc::Get("$SPEECHBROKERVOICE_WORD_EJECT_FAULTED");
			case Ejection::BusyOnFinal:    return Loc::Get("$SPEECHBROKERVOICE_WORD_EJECT_BUSY_FINAL");
			case Ejection::ProtocolBroken: return Loc::Get("$SPEECHBROKERVOICE_WORD_EJECT_PROTOCOL");
			default:                       return Loc::Get("$SPEECHBROKERVOICE_WORD_EJECT_PROTOCOL");
			}
		}

		// The name of the crossing that faulted. It is a DATUM and not a message -
		// it is the name of a function in the contract, the same kind of thing as
		// the fault code beside it - so it goes into the localised line as an
		// argument and is not itself translated (Guard.h, FaultName).
		constexpr const char* kCallStart         = "Start";
		constexpr const char* kCallStop          = "Stop";
		constexpr const char* kCallSetVocabulary = "SetVocabulary";
		constexpr const char* kCallSubmit        = "Submit";
		constexpr const char* kCallCancel        = "Cancel";

		std::int64_t ElapsedMs(Clock::time_point a_from) noexcept
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - a_from).count();
		}
	}

	// ---------------------------------------------------------------- BusyWindow

	BusyWindow::BusyWindow(int a_size) :
		// AT LEAST ONE SLOT, because a window of nothing would make Rate() a
		// constant zero and quietly disable the whole demotion path. The settings
		// are obeyed where they can be obeyed; a nonsense value is raised to the
		// smallest thing that still measures something.
		_ring(static_cast<std::size_t>(std::max(1, a_size)), std::uint8_t{ 0 })
	{}

	void BusyWindow::Note(bool a_busy)
	{
		// The slot about to be overwritten leaves the count before the new
		// observation enters it. Doing it the other way round loses a busy call
		// every time the ring wraps onto one.
		if (_filled == _ring.size()) {
			if (_ring[_at] != 0u) {
				--_busy;
			}
		} else {
			++_filled;
		}

		_ring[_at] = a_busy ? std::uint8_t{ 1 } : std::uint8_t{ 0 };
		if (a_busy) {
			++_busy;
		}

		_at = (_at + 1u) % _ring.size();
	}

	double BusyWindow::Rate() const noexcept
	{
		// Zero while the window is empty, and NO "not enough samples" fallback:
		// the window is small on purpose, and a model that answers BUSY to its
		// first twenty requests is exactly the case this exists to catch
		// (Model.h, BusyWindow).
		return _filled == 0 ? 0.0 : static_cast<double>(_busy) / static_cast<double>(_filled);
	}

	// --------------------------------------------------------------------- Model

	std::shared_ptr<Model> Model::Make(SpeechBrokerVoiceHandle a_handle, ModelInfo a_info,
		const SpeechBrokerVoiceModel& a_table, DispatchSettings a_settings,
		Reputation& a_standing, Host& a_host)
	{
		// THE DELETER DOES NOTHING, and that is the rule at the head of this class
		// expressed in the type rather than in a comment somebody may not read: an
		// abandoned dispatch thread may still return into this object at any later
		// moment, so the per-model state is never freed. The destructor is private,
		// so the ordinary shared_ptr - the one that would call delete - does not
		// compile and nobody can undo this by accident.
		return std::shared_ptr<Model>(
			new Model(a_handle, std::move(a_info), a_table, a_settings, a_standing, a_host),
			[](Model*) noexcept {});
	}

	Model::Model(SpeechBrokerVoiceHandle a_handle, ModelInfo a_info,
		const SpeechBrokerVoiceModel& a_table, DispatchSettings a_settings,
		Reputation& a_standing, Host& a_host) :
		_handle(a_handle),
		_info(std::move(a_info)),
		_table(a_table),
		_settings(a_settings),
		_standing(a_standing),
		_host(a_host),
		_busy(a_settings.busyWindow)
	{}

	Model::~Model() = default;

	// --- where it is ---------------------------------------------------------

	bool Model::TakesPass(bool a_final) const noexcept
	{
		if (Where() != Life::Live || !Proven()) {
			return false;
		}

		if (a_final) {
			// The final pass of a turn asks everybody: there is nothing left to
			// hurry for and quality is what is left to want.
			return true;
		}

		// finalOnly IS BINDING and is the one declaration measurement does not
		// override, because it is about COST and not about speed - a model may
		// answer in forty milliseconds and still be one that must not be asked five
		// times a turn (contract, ModelInfo::finalOnly).
		if (_info.finalOnly) {
			return false;
		}

		// Demoted by its own busy rate: it is offered final passes only, and
		// keeping it on the interim roster would mean paying for the copy and the
		// wait to be told BUSY again.
		if (_busyDemoted.load(std::memory_order_acquire)) {
			return false;
		}

		return Class() == Speed::Fast;
	}

	Speed Model::Class() const
	{
		// Measured once the samples exist, declared before that - and the record
		// itself knows which of the two it is holding, so there is no second copy
		// of that threshold here (Reputation::MeasuredClass).
		return _standing.MeasuredClass();
	}

	std::int32_t Model::BudgetMs() const
	{
		return _standing.DeadlineMs(_settings.defaultBudgetMs, _settings.bootstrapSlackMs);
	}

	// --- the queue -----------------------------------------------------------

	Queued Model::Offer(DispatchEntry&& a_entry)
	{
		// The entry that loses is moved out here and dies at the end of this
		// function - OUTSIDE the lock. Its destructor releases a claim on a pass
		// snapshot, and a claim released under _lock would put an allocator on a
		// path that is entered from the thread the microphone is being drained on.
		DispatchEntry dropped;
		bool          haveDropped = false;
		Queued        outcome = Queued::Refused;

		{
			std::lock_guard<std::mutex> lock(_lock);

			if (!_pending.has_value()) {
				_pending = std::move(a_entry);
				outcome = Queued::Accepted;
			} else if (a_entry.probe) {
				// A PROBE IS NEVER REFUSED. Its slot is empty when it is offered -
				// the probe goes out before the model is proven and nothing else is
				// offered to an unproven model - so this branch is the day that
				// stops being true, and the probe wins it: the one check in this
				// system that does not rest on trusting a model must not be the
				// thing that gets dropped.
				dropped = std::move(*_pending);
				haveDropped = true;
				_pending = std::move(a_entry);
				outcome = Queued::AcceptedReplacing;
			} else if (!_pending->probe && !_pending->final &&
				_pending->turnId == a_entry.turnId && a_entry.serial > _pending->serial) {
				// A REPLACEMENT NEVER CROSSES A TURN and NEVER REPLACES A FINAL
				// PASS. Both bounds are the contract rather than niceties: turn 6's
				// first pass is different speech from turn 5's, and a final pass is
				// the only one whose loss no later pass recovers (Model.h, Offer).
				dropped = std::move(*_pending);
				haveDropped = true;
				_pending = std::move(a_entry);
				outcome = Queued::AcceptedReplacing;
			} else {
				dropped = std::move(a_entry);
				haveDropped = true;
				outcome = Queued::Refused;
			}
		}

		_wake.notify_all();

		if (haveDropped) {
			// TWICE, NOT ONCE, and this is the only place that knows which entry it
			// was: out of that pass's expected set, so the pass does not wait for a
			// request that was never sent, AND against the model in the same ledger
			// as a timeout, so a model that is always too slow to be submitted to
			// does not look statistically perfect (Model.h, Offer).
			_host.Drop(dropped.collector, _handle, Removal::DroppedUnsent);
			_standing.NoteDropped();
		}

		return outcome;
	}

	// --- the debt ------------------------------------------------------------

	bool Model::DebtHasRoom() const
	{
		std::lock_guard<std::mutex> lock(_lock);
		return _debt.size() < static_cast<std::size_t>(std::max(1u, _info.maxInFlight));
	}

	void Model::TakeDebt(std::int64_t a_utteranceId)
	{
		std::lock_guard<std::mutex> lock(_lock);
		_debt.push_back(a_utteranceId);
	}

	bool Model::ReleaseDebt(std::int64_t a_utteranceId)
	{
		bool had = false;
		{
			std::lock_guard<std::mutex> lock(_lock);
			const auto at = std::find(_debt.begin(), _debt.end(), a_utteranceId);
			if (at != _debt.end()) {
				_debt.erase(at);
				had = true;
			}
		}

		// The dispatch thread may be waiting for room; a discharged debt is the
		// only thing that ever makes room.
		if (had) {
			_wake.notify_all();
		}
		return had;
	}

	// --- what the host tells it ----------------------------------------------

	void Model::SetVocabulary(std::vector<std::string> a_phrases)
	{
		{
			std::lock_guard<std::mutex> lock(_lock);
			Control control;
			control.kind = ControlKind::Vocabulary;
			control.phrases = std::move(a_phrases);
			_control.push_back(std::move(control));
		}
		_wake.notify_all();
	}

	void Model::Cancel(std::int64_t a_utteranceId)
	{
		// Nothing to queue when the table has no Cancel: it may be NULL, and a
		// control message nobody will ever deliver is a message that would sit in
		// the queue in front of real work.
		if (_table.Cancel == nullptr) {
			return;
		}

		{
			std::lock_guard<std::mutex> lock(_lock);
			Control control;
			control.kind = ControlKind::Cancel;
			control.utteranceId = a_utteranceId;
			_control.push_back(std::move(control));
		}
		_wake.notify_all();
	}

	void Model::ProbeAnswered(bool a_invented)
	{
		// The outcome feeds the invention record ONLY - never a latency sample,
		// never a timeout - because the first inference of a session pays for
		// workspace allocation and autotune (contract, "The probe").
		_standing.NoteSilenceProbe(a_invented);

		_proven.store(true, std::memory_order_release);

		// It was held out of every pass until this arrived. Now it may take part -
		// unless something else has happened to it in the meantime, which is why
		// this is a compare and not a store.
		Life expected = Life::Probing;
		_life.compare_exchange_strong(expected, Life::Live, std::memory_order_acq_rel);

		if (a_invented) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_INVENTS", _info.id);
		} else {
			Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL_PROBE_PASSED", _info.id);
		}
	}

	void Model::NoteReady(bool a_ready)
	{
		if (!a_ready) {
			Life was = _life.load(std::memory_order_acquire);
			if (was == Life::Draining || was == Life::Ejected) {
				return;
			}
			_life.store(Life::NotReady, std::memory_order_release);

			// GOING NOT-READY REMOVES IT FROM EVERY PASS STILL OPEN, as a removal
			// and not a timeout: it said so itself, in time, and a model is not
			// charged for the honesty (contract, Ready).
			_host.RemoveEverywhere(_handle, Removal::NotReady);
			return;
		}

		Life expected = Life::NotReady;
		if (_life.compare_exchange_strong(expected, Life::Live, std::memory_order_acq_rel)) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL_READY_AGAIN", _info.id);
		}
	}

	void Model::Eject(Ejection a_why)
	{
		const Life was = _life.exchange(Life::Ejected, std::memory_order_acq_rel);
		if (was == Life::Ejected) {
			return;  // idempotent: one ejection, one line
		}

		Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_EJECTED", _info.id, EjectionWord(a_why));

		// Out for the session means its thread has nothing left to do. It leaves
		// its loop, calls Stop if there is anything to stop, and the others carry
		// on untouched.
		_stopping.store(true, std::memory_order_release);
		_wake.notify_all();
	}

	double Model::BusyRate() const
	{
		std::lock_guard<std::mutex> lock(_lock);
		return _busy.Rate();
	}

	bool Model::FirstTruncation() noexcept
	{
		return !_saidTruncated.exchange(true, std::memory_order_acq_rel);
	}

	bool Model::FirstClamp() noexcept
	{
		return !_saidClamped.exchange(true, std::memory_order_acq_rel);
	}

	// --- its thread ----------------------------------------------------------

	void Model::Begin()
	{
		_done.store(false, std::memory_order_release);

		// DETACHED. See Begin's note in the header: a Model is never destroyed, so
		// nothing will ever join this, and a joinable member nobody joins is a
		// std::terminate waiting for the day somebody writes a destructor.
		_thread = std::thread([this] {
			// A catch(...) AT THE TOP OF THE THREAD ENTRY FUNCTION. An exception
			// escaping a thread procedure is std::terminate, and that is a
			// fail-fast nothing in the process observes.
			try {
				Run();
			} catch (const std::exception& e) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_THREAD_THREW", _info.id, e.what());
			} catch (...) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_THREAD_THREW", _info.id,
					Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
			}

			_done.store(true, std::memory_order_release);
		});
		_thread.detach();
	}

	bool Model::BeginDrain()
	{
		Life was = _life.load(std::memory_order_acquire);
		for (;;) {
			if (was == Life::Draining || was == Life::Ejected) {
				return false;
			}
			if (_life.compare_exchange_weak(was, Life::Draining,
					std::memory_order_acq_rel, std::memory_order_acquire)) {
				break;
			}
		}

		// The dispatch thread is told to leave, and the registry lock is NOT held
		// while it is: Complete, Ready and Log on a draining handle read one atomic
		// and return at once, which is what makes Complete unable to deadlock
		// against Unregister.
		_stopping.store(true, std::memory_order_release);
		_wake.notify_all();
		return true;
	}

	bool Model::WaitDrained(std::uint32_t a_stopMs)
	{
		// POLLED, NOT JOINED. A join has no bound, and nothing in the contract
		// bounds how long one of a model's own calls may take; this wait does have
		// a bound, and past it the model is abandoned rather than killed.
		const auto until = Clock::now() + std::chrono::milliseconds(a_stopMs);
		for (;;) {
			if (Done() && !_inside.load(std::memory_order_acquire)) {
				return true;
			}
			if (Clock::now() >= until) {
				return false;
			}

			// A wall-clock sleep, which is allowed here and nowhere near the
			// cutting: this number reaches no gate, no pacer and no turn.
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	// --- the dispatch thread -------------------------------------------------

	void Model::Run()
	{
		// SetThreadStackGuarantee, so that a stack overflow inside third-party code
		// still leaves room for the filter and for the line that names it. Without
		// it the overflow that ejects a model takes the process with it and the
		// guard is decoration (Guard.h, PrepareDispatchThread).
		PrepareDispatchThread(_settings.stackGuaranteeBytes);

		// Kept for the exit path: Stop is owed only by a model whose Start actually
		// returned OK, and it is NOT owed by one that faulted - __except unwound
		// without running a destructor in any frame between, so calling back into
		// that state is the adapter choosing to keep talking to code it has just
		// watched go wrong.
		bool started = false;
		bool faulted = false;

		if (RunStart(faulted)) {
			started = true;

			// The probe goes out at once and gates the roster: a model takes part
			// in no pass until its probe answer has arrived (contract, "The probe").
			_life.store(Life::Probing, std::memory_order_release);
			_host.SubmitProbe(_host.Find(_handle));

			RunQueue(faulted);
		}

		// Whatever is still queued belongs to a pass that is still waiting for it.
		// Drop it properly rather than letting the destructor swallow it, or the
		// pass sits to its deadline for a request nobody will ever send.
		DropPending();

		if (started && !faulted) {
			Fault fault;
			bool  cameBack = true;
			try {
				cameBack = GuardedStop(_table.Stop, _table.user, &fault);
			} catch (const std::exception& e) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallStop, e.what());
			} catch (...) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallStop,
					Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
			}

			if (!cameBack) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_FAULTED", _info.id, kCallStop,
					FaultName(fault.code), fault.code, fault.address);
			}
		}
	}

	bool Model::RunStart(bool& a_faulted)
	{
		_life.store(Life::Starting, std::memory_order_release);

		// How long past its own startupMs a Start may run before one line says so.
		// There is NO TIMEOUT on a hung Start: the adapter cannot kill a thread
		// inside third-party code and will not pretend it can. It waits here, on
		// this model's own dispatch thread, where nothing else is waiting.
		const auto complainMs = _settings.startComplaintMs > 0
			? static_cast<std::int64_t>(_settings.startComplaintMs)
			: static_cast<std::int64_t>(_info.startupMs);

		const int attempts = std::max(1, _settings.startAttempts);
		for (int attempt = 1; attempt <= attempts; ++attempt) {
			if (_stopping.load(std::memory_order_acquire)) {
				return false;
			}

			const auto   at = Clock::now();
			std::int32_t status = SPEECHBROKERVOICE_REFUSED;
			Fault        fault;
			bool         cameBack = false;
			bool         threw = false;

			try {
				cameBack = GuardedStart(_table.Start, _table.user, &status, &fault);
			} catch (const std::exception& e) {
				threw = true;
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallStart, e.what());
			} catch (...) {
				threw = true;
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallStart,
					Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
			}

			const auto took = ElapsedMs(at);
			if (complainMs > 0 && took > complainMs) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_START_SLOW", _info.id, took, complainMs);
			}

			if (!cameBack && !threw) {
				// A fault: the state on the far side is of unknown shape, so there
				// is no second attempt and there is no Stop.
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_FAULTED", _info.id, kCallStart,
					FaultName(fault.code), fault.code, fault.address);
				a_faulted = true;
				Eject(Ejection::Faulted);
				return false;
			}

			if (!threw && status == SPEECHBROKERVOICE_OK) {
				// A Start that returns OK long afterwards is honoured if the model
				// has not been ejected, and ignored with one line if it has.
				if (Where() == Life::Ejected) {
					Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_START_LATE", _info.id, took);
					return false;
				}
				Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL_STARTED", _info.id, took, attempt);
				return true;
			}

			// RETRY AND NOT_READY ARE TREATED ALIKE and are both recoverable: an
			// earlier draft of the contract ejected a model for the session for
			// saying NOT_READY, which is the obvious code for "my model is not up"
			// (contract, Model::Start). A throw is read the same way a REFUSED is.
			if (!threw && status != SPEECHBROKERVOICE_RETRY && status != SPEECHBROKERVOICE_NOT_READY) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_START_REFUSED", _info.id, status);
				Eject(Ejection::StartRefused);
				return false;
			}

			if (attempt < attempts) {
				Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL_START_RETRY", _info.id, status, attempt,
					attempts, _info.startupMs);
				WaitBetweenStarts();
			}
		}

		Eject(Ejection::StartExhausted);
		return false;
	}

	void Model::WaitBetweenStarts()
	{
		// The wait between attempts is the model's OWN startupMs, and it is waited
		// out on the condition variable rather than slept through, so that a model
		// asked to stop between two attempts does not sit out the whole of it.
		std::unique_lock<std::mutex> lock(_lock);
		_wake.wait_for(lock, std::chrono::milliseconds(_info.startupMs),
			[this] { return _stopping.load(std::memory_order_acquire); });
	}

	void Model::RunQueue(bool& a_faulted)
	{
		// How many times this model has answered and then refused the same
		// utterance. ProtocolBroken is for the one that cannot be talked out of it,
		// so the first is a line and the second is the ejection.
		int protocolBreaks = 0;

		for (;;) {
			Control                      control;
			bool                         haveControl = false;
			std::optional<DispatchEntry> entry;

			{
				std::unique_lock<std::mutex> lock(_lock);
				_wake.wait(lock, [this] {
					return _stopping.load(std::memory_order_acquire) || !_control.empty() ||
						_pending.has_value();
				});

				if (_stopping.load(std::memory_order_acquire)) {
					return;
				}

				// CONTROL FIRST. A Cancel or a vocabulary is ordered against the
				// work of this thread and against nothing else, and both are small
				// and rare; letting a request overtake them would deliver a Cancel
				// after the pass it names has already been reconciled.
				if (!_control.empty()) {
					control = std::move(_control.front());
					_control.pop_front();
					haveControl = true;
				} else {
					entry = std::move(_pending);
					_pending.reset();
				}
			}

			if (haveControl) {
				if (!RunControl(control, a_faulted)) {
					return;
				}
				continue;
			}

			if (entry.has_value()) {
				RunEntry(std::move(*entry), a_faulted, protocolBreaks);
				if (a_faulted) {
					return;
				}
			}
		}
	}

	bool Model::RunControl(Control& a_control, bool& a_faulted)
	{
		switch (a_control.kind) {
		case ControlKind::Vocabulary:
			{
				// Does nothing when the table has none: SetVocabulary may be NULL,
				// and the list is advisory by contract.
				if (_table.SetVocabulary == nullptr) {
					return true;
				}

				// The array of pointers is built here, out of strings this object
				// owns for the length of the call, which is exactly what the
				// contract promises the model: the array and the strings live for
				// the length of the call only.
				std::vector<const char*> raw;
				raw.reserve(a_control.phrases.size());
				for (const auto& phrase : a_control.phrases) {
					raw.push_back(phrase.c_str());
				}

				Fault fault;
				bool  cameBack = true;
				_inside.store(true, std::memory_order_release);
				try {
					cameBack = GuardedSetVocabulary(_table.SetVocabulary, _table.user,
						raw.empty() ? nullptr : raw.data(), static_cast<std::int32_t>(raw.size()),
						&fault);
				} catch (const std::exception& e) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id,
						kCallSetVocabulary, e.what());
				} catch (...) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id,
						kCallSetVocabulary, Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
				}
				_inside.store(false, std::memory_order_release);

				if (!cameBack) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_FAULTED", _info.id, kCallSetVocabulary,
						FaultName(fault.code), fault.code, fault.address);
					a_faulted = true;
					Eject(Ejection::Faulted);
					return false;
				}
				return true;
			}

		case ControlKind::Cancel:
			{
				if (_table.Cancel == nullptr) {
					return true;
				}

				Fault fault;
				bool  cameBack = true;
				_inside.store(true, std::memory_order_release);
				try {
					cameBack = GuardedCancel(_table.Cancel, _table.user, a_control.utteranceId, &fault);
				} catch (const std::exception& e) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallCancel, e.what());
				} catch (...) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallCancel,
						Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
				}
				_inside.store(false, std::memory_order_release);

				if (!cameBack) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_FAULTED", _info.id, kCallCancel,
						FaultName(fault.code), fault.code, fault.address);
					a_faulted = true;
					Eject(Ejection::Faulted);
					return false;
				}
				return true;
			}

		case ControlKind::Stop:
		default:
			return false;
		}
	}

	void Model::RunEntry(DispatchEntry&& a_entry, bool& a_faulted, int& a_protocolBreaks)
	{
		DispatchEntry entry = std::move(a_entry);

		// THE ADAPTER NEVER ENTERS Submit FOR A PASS THAT HAS ALREADY CLOSED.
		// Sending it would buy nothing but a guaranteed STALE and, for a model
		// across a network, a paid upload and a paid inference for an answer that
		// cannot be used.
		if (entry.collector && entry.collector->IsClosed()) {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_MODEL_PASS_CLOSED", _info.id, entry.utteranceId);
			_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
			_standing.NoteDropped();
			return;
		}

		// THE ONE ABSOLUTE DEADLINE OF THE PASS, stamped when the pass was created,
		// minus now. A deadline measured from the model's own call while the timer
		// enforcing it was armed at pass creation is two different clocks wearing
		// one name (docs/model-host.md, "The deadline").
		std::optional<Clock::time_point> deadline;
		if (entry.collector) {
			deadline = entry.collector->Deadline();
		}

		std::int64_t leftMs = 0;
		if (deadline.has_value()) {
			leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				*deadline - Clock::now()).count();
			if (leftMs <= 0) {
				// A REQUEST WHOSE BUDGET IS ALREADY SPENT IS NEVER SUBMITTED, the
				// model is not told, and it owes nothing - but the drop is recorded
				// twice all the same (contract, deadlineMs).
				Loc::Debug("$SPEECHBROKERVOICE_LOG_MODEL_BUDGET_SPENT", _info.id, entry.utteranceId);
				_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
				_standing.NoteDropped();
				return;
			}
		}

		// maxInFlight IS READ AGAINST THE DEBT AND NOT AGAINST THE QUEUE: Submit is
		// not entered while that many Completes are already outstanding. The wait is
		// bounded by the same deadline, because a pass that closes while we wait is
		// a pass there is no point entering Submit for.
		{
			const auto room = [this] {
				return _stopping.load(std::memory_order_acquire) ||
					_debt.size() < static_cast<std::size_t>(std::max(1u, _info.maxInFlight));
			};

			std::unique_lock<std::mutex> lock(_lock);
			if (deadline.has_value()) {
				_wake.wait_until(lock, *deadline, room);
			} else {
				_wake.wait(lock, room);
			}
		}

		if (_stopping.load(std::memory_order_acquire)) {
			_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
			_standing.NoteDropped();
			return;
		}

		if (!DebtHasRoom()) {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_MODEL_BUDGET_SPENT", _info.id, entry.utteranceId);
			_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
			_standing.NoteDropped();
			return;
		}

		if (deadline.has_value()) {
			leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				*deadline - Clock::now()).count();
			if (leftMs <= 0) {
				Loc::Debug("$SPEECHBROKERVOICE_LOG_MODEL_BUDGET_SPENT", _info.id, entry.utteranceId);
				_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
				_standing.NoteDropped();
				return;
			}
		}

		SpeechBrokerVoiceRequest request{};
		request.structBytes = static_cast<std::uint32_t>(sizeof(SpeechBrokerVoiceRequest));
		request.serial = entry.serial;
		request.turnId = entry.turnId;
		request.utteranceId = entry.utteranceId;
		request.final = entry.final ? 1 : 0;
		request.sampleCount = entry.SampleCount();
		request.samples = entry.audio ? entry.audio->data() : nullptr;
		request.lostSamples = entry.lostSamples;

		// 0 MEANS NO DEADLINE IS STATED, and the probe is the one request that
		// carries it: it belongs to no turn and is retired only when the model
		// unregisters or the session ends.
		request.deadlineMs = entry.probe || !deadline.has_value()
			? 0u
			: static_cast<std::uint32_t>(leftMs);

		request.format.structBytes = static_cast<std::uint32_t>(sizeof(SpeechBrokerVoiceFormat));
		request.format.sampleRate = kTargetSampleRate;
		request.format.channels = 1u;
		request.format.sampleFormat = SPEECHBROKERVOICE_FMT_FLOAT32;

		// THE DEBT IS RECORDED BEFORE THE CALL, NOT BY THE RETURN VALUE. That
		// ordering is what makes Complete legal from inside Submit: the answer then
		// always arrives against an utterance the adapter already knows.
		TakeDebt(entry.utteranceId);

		const auto   at = Clock::now();
		std::int32_t status = SPEECHBROKERVOICE_REFUSED;
		Fault        fault;
		bool         cameBack = false;
		bool         threw = false;

		_inside.store(true, std::memory_order_release);
		try {
			cameBack = GuardedSubmit(_table.Submit, _table.user, &request, &status, &fault);
		} catch (const std::exception& e) {
			threw = true;
			Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallSubmit, e.what());
		} catch (...) {
			threw = true;
			Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_CALL_THREW", _info.id, kCallSubmit,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
		}
		_inside.store(false, std::memory_order_release);

		const auto took = ElapsedMs(at);
		if (_settings.submitOverrunMs > 0 && took > static_cast<std::int64_t>(_settings.submitOverrunMs)) {
			// NOT a failure - the contract only promises Submit is measured - but a
			// shim that keeps overrunning has to be distinguishable from a slow
			// model (Model.h, DispatchSettings::submitOverrunMs).
			Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_SUBMIT_SLOW", _info.id, took,
				_settings.submitOverrunMs, entry.utteranceId);
		}

		const auto busyNote = [this](bool a_busy) {
			std::lock_guard<std::mutex> lock(_lock);
			_busy.Note(a_busy);
		};

		if (!cameBack && !threw) {
			// A fault retracts exactly what a non-OK return retracts, and ejects on
			// top of it: the model's locks are still held and the state on the far
			// side is of unknown shape (Guard.h, the outward guards).
			Loc::Error("$SPEECHBROKERVOICE_LOG_MODEL_FAULTED", _info.id, kCallSubmit,
				FaultName(fault.code), fault.code, fault.address);
			busyNote(false);
			ReleaseDebt(entry.utteranceId);
			_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
			a_faulted = true;
			Eject(Ejection::Faulted);
			return;
		}

		if (threw) {
			// A throw is a REFUSAL and nothing more - the contract says so in as
			// many words - so it costs a failure and not the session.
			busyNote(false);
			ReleaseDebt(entry.utteranceId);
			_standing.NoteFailure(false);
			_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
			return;
		}

		switch (status) {
		case SPEECHBROKERVOICE_OK:
			// Nothing more here: the model owes exactly one Complete and it will
			// come, or the deadline will close the pass without it.
			busyNote(false);
			return;

		case SPEECHBROKERVOICE_BUSY:
			{
				const bool owed = ReleaseDebt(entry.utteranceId);
				busyNote(true);

				// BUSY COSTS NOTHING AS A FAILURE - no NoteDropped either: the model
				// owes nothing, nothing is counted against it, and its weight in an
				// argument is untouched (Model.h, BusyWindow).
				_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);

				if (!owed) {
					NoteProtocolBreak(entry.utteranceId, a_protocolBreaks);
				}

				const double rate = BusyRate();
				if (rate > _settings.busyRateLimit) {
					if (!_busyDemoted.exchange(true, std::memory_order_acq_rel)) {
						Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_BUSY_DEMOTED", _info.id, rate,
							_settings.busyRateLimit);
					} else if (entry.final) {
						// Demoted to final passes only and still busy on those.
						// There is nothing further to take away from it.
						Eject(Ejection::BusyOnFinal);
					}
				}
				return;
			}

		case SPEECHBROKERVOICE_NOT_READY:
			{
				const bool owed = ReleaseDebt(entry.utteranceId);
				busyNote(false);
				_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
				if (!owed) {
					NoteProtocolBreak(entry.utteranceId, a_protocolBreaks);
				}
				Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL_SAID_NOT_READY", _info.id);
				NoteReady(false);
				return;
			}

		default:
			// REFUSED, or anything outside the enum, which the contract says is
			// read as REFUSED and logged.
			busyNote(false);
			ReleaseDebt(entry.utteranceId);
			_standing.NoteFailure(false);
			_host.Drop(entry.collector, _handle, Removal::DroppedUnsent);
			Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_SUBMIT_REFUSED", _info.id, status,
				entry.utteranceId);
			return;
		}
	}

	void Model::NoteProtocolBreak(std::int64_t a_utteranceId, int& a_protocolBreaks)
	{
		// The debt was not owed any more, which means the model called Complete and
		// then returned BUSY or NOT_READY for the same utterance. The contract
		// forbids that in as many words: it is logged, and it is counted.
		Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_PAID_THEN_REFUSED", _info.id, a_utteranceId);
		_standing.NoteFailure(false);

		// Once is a defect; twice is a model that cannot be talked out of it.
		if (++a_protocolBreaks >= 2) {
			Eject(Ejection::ProtocolBroken);
		}
	}

	void Model::DropPending()
	{
		std::optional<DispatchEntry> entry;
		{
			std::lock_guard<std::mutex> lock(_lock);
			entry = std::move(_pending);
			_pending.reset();
		}

		if (entry.has_value()) {
			_host.Drop(entry->collector, _handle, Removal::DroppedUnsent);
			_standing.NoteDropped();
		}
	}
}
