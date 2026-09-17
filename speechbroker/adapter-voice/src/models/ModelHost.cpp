#include "models/ModelHost.h"

#include "Loc.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// windows.h LAST, and with both guards. NOMINMAX because it otherwise defines
// min and max as macros and every std::min in this file stops compiling;
// WIN32_LEAN_AND_MEAN because this file wants two functions out of it and not the
// whole of OLE. It is here for ONE thing: finding the folder this DLL lives in, so
// that the calibration file can be resolved inside it and refused if it leaves
// (Reputation.h, ReputationSettings::file).
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Voice::Models
{
	namespace
	{
		// ------------------------------------------------------------ the strings
		//
		// ONE BOUNDED READ FOR EVERY STRING THAT COMES OUT OF A MODEL'S MEMORY -
		// the id, the name, the language, the provides, every fragment's text, the
		// failure text, a log key and every one of its arguments. Never strlen:
		// every one of these pointers belongs to third-party code, a missing
		// terminator is a defect that happens, and the contract's answer to it is
		// to TRUNCATE rather than to refuse (contract, "EVERY STRING THAT CROSSES
		// THIS LINE").
		struct Copied
		{
			std::string text;
			bool        truncated{ false };
		};

		Copied CopyString(const char* a_from)
		{
			Copied out;
			if (a_from == nullptr) {
				// A NULL becomes an empty string rather than a crash - the contract
				// says so for `failed`, `name` and `reason` in as many words.
				return out;
			}

			const std::size_t n = ::strnlen(a_from, SPEECHBROKERVOICE_MAX_STRING_BYTES);
			out.text.assign(a_from, n);
			out.truncated = n == SPEECHBROKERVOICE_MAX_STRING_BYTES;
			return out;
		}

		// THERE IS NO SUCH THING AS A PARTIAL VERSION-1 STRUCT. A multiple of 8 AND
		// at least the version-1 size, or the sender could declare 68 bytes for a
		// 72-byte ModelInfo and put its last field out of reach of the very check
		// that guards it (contract, "How a struct grows").
		bool StructOk(std::uint32_t a_bytes, std::uint32_t a_v1) noexcept
		{
			return a_bytes % 8u == 0u && a_bytes >= a_v1;
		}

		// ------------------------------------------------------------- the words

		const char* KindWord(std::uint32_t a_kind)
		{
			switch (a_kind) {
			case SPEECHBROKERVOICE_KIND_INPROCESS: return Loc::Get("$SPEECHBROKERVOICE_WORD_KIND_INPROCESS");
			case SPEECHBROKERVOICE_KIND_CHILD:     return Loc::Get("$SPEECHBROKERVOICE_WORD_KIND_CHILD");
			case SPEECHBROKERVOICE_KIND_ATTACHED:  return Loc::Get("$SPEECHBROKERVOICE_WORD_KIND_ATTACHED");
			case SPEECHBROKERVOICE_KIND_REMOTE:    return Loc::Get("$SPEECHBROKERVOICE_WORD_KIND_REMOTE");
			default:                               return Loc::Get("$SPEECHBROKERVOICE_WORD_KIND_UNKNOWN");
			}
		}

		const char* RemovalWord(Removal a_why)
		{
			switch (a_why) {
			case Removal::DroppedUnsent: return Loc::Get("$SPEECHBROKERVOICE_WORD_REMOVAL_UNSENT");
			case Removal::Unregistered:  return Loc::Get("$SPEECHBROKERVOICE_WORD_REMOVAL_UNREGISTERED");
			case Removal::NotReady:      return Loc::Get("$SPEECHBROKERVOICE_WORD_REMOVAL_NOT_READY");
			default:                     return Loc::Get("$SPEECHBROKERVOICE_WORD_REMOVAL_UNSENT");
			}
		}

		const char* QueuedWord(Queued a_outcome)
		{
			switch (a_outcome) {
			case Queued::Accepted:          return Loc::Get("$SPEECHBROKERVOICE_WORD_QUEUED_ACCEPTED");
			case Queued::AcceptedReplacing: return Loc::Get("$SPEECHBROKERVOICE_WORD_QUEUED_REPLACING");
			case Queued::Refused:           return Loc::Get("$SPEECHBROKERVOICE_WORD_QUEUED_REFUSED");
			default:                        return Loc::Get("$SPEECHBROKERVOICE_WORD_QUEUED_REFUSED");
			}
		}

		// The level of a model's own log line, mapped onto ours. An unknown level
		// becomes info rather than being dropped: the line is the only diagnostic
		// channel a remote model has, and losing it over a number is the wrong
		// trade (contract, Host::Log).
		spdlog::level::level_enum LevelOf(std::int32_t a_level) noexcept
		{
			switch (a_level) {
			case SPEECHBROKERVOICE_LOG_DEBUG: return spdlog::level::debug;
			case SPEECHBROKERVOICE_LOG_WARN:  return spdlog::level::warn;
			case SPEECHBROKERVOICE_LOG_ERROR: return spdlog::level::err;
			default:                          return spdlog::level::info;
			}
		}

		// --------------------------------------------------------- what it says

		// AT VERSION 1 "asr" IS THE ONLY VALUE THE ADAPTER KNOWS, and a
		// registration whose provides names nothing we know is refused MALFORMED.
		// NULL is read as "asr". A bitmask and not a bool, so that the refusal can
		// say "nothing I know", which needs a zero that means zero (Model.h,
		// enum Provides).
		std::uint32_t ParseProvides(const std::string& a_raw, std::string& a_unknown)
		{
			if (a_raw.empty()) {
				return kProvidesAsr;
			}

			std::uint32_t known = kProvidesNothing;
			std::size_t   at = 0;
			while (at <= a_raw.size()) {
				const auto comma = a_raw.find(',', at);
				const auto end = comma == std::string::npos ? a_raw.size() : comma;

				auto from = at;
				auto to = end;
				while (from < to && (a_raw[from] == ' ' || a_raw[from] == '\t')) {
					++from;
				}
				while (to > from && (a_raw[to - 1] == ' ' || a_raw[to - 1] == '\t')) {
					--to;
				}

				const auto value = a_raw.substr(from, to - from);
				if (value == "asr") {
					known |= kProvidesAsr;
				} else if (!value.empty()) {
					// Logged and ignored, never fatal on its own: a model that does
					// one thing we know and one we do not is a model we can use.
					if (!a_unknown.empty()) {
						a_unknown += ',';
					}
					a_unknown += value;
				}

				if (comma == std::string::npos) {
					break;
				}
				at = comma + 1;
			}

			return known;
		}

		// ONE LANGUAGE PER REGISTRATION and only the first BCP-47 subtag is kept.
		// It is LOGGED AND ROUTED ON BY NOTHING at version 1 (contract,
		// ModelInfo::language).
		std::string FirstSubtag(const std::string& a_language)
		{
			const auto dash = a_language.find('-');
			return dash == std::string::npos ? a_language : a_language.substr(0, dash);
		}

		// ------------------------------------------------------------- the folder

		// The folder this DLL lives in, found from an address INSIDE this module
		// rather than from the module of the process: the adapter is a plugin, and
		// the process is SkyrimVR.exe, whose folder is not ours.
		std::filesystem::path AdapterFolder()
		{
			HMODULE self = nullptr;
			if (::GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&AdapterFolder), &self) == 0 ||
				self == nullptr) {
				return {};
			}

			std::wstring buffer(MAX_PATH, L'\0');
			for (;;) {
				const DWORD got = ::GetModuleFileNameW(self, buffer.data(),
					static_cast<DWORD>(buffer.size()));
				if (got == 0) {
					return {};
				}
				if (static_cast<std::size_t>(got) < buffer.size()) {
					buffer.resize(got);
					break;
				}
				buffer.resize(buffer.size() * 2u);
			}

			return std::filesystem::path(buffer).parent_path();
		}

		// A path that leaves the folder of the adapter is refused rather than
		// followed - the same rule the service settings go through, and for the
		// same reason: a settings file may be edited by anybody, and a relative
		// path with two dots in it is the cheapest way to make a mod read
		// something that is not its own. Empty means refused.
		std::filesystem::path ResolveInside(const std::filesystem::path& a_base,
			const std::string& a_relative)
		{
			if (a_base.empty() || a_relative.empty()) {
				return {};
			}

			std::filesystem::path want(a_relative);
			if (!want.is_absolute()) {
				want = a_base / want;
			}

			const auto normal = want.lexically_normal();
			const auto base = a_base.lexically_normal();
			const auto inside = normal.lexically_relative(base);
			if (inside.empty() || inside.native().rfind(L"..", 0) == 0) {
				return {};
			}
			return normal;
		}

		// ------------------------------------------------ the POD guard contexts
		//
		// EACH ENTRY POINT BUILDS ONE OF THESE ON ITS STACK and hands it to
		// GuardedHostCall. They hold nothing but scalars and pointers, because the
		// guarded frame is where MSVC refuses anything with a destructor, and the
		// result of the call travels back inside the context rather than through a
		// return value the guard has no way to carry (Guard.h, HostBody).

		struct RegisterContext
		{
			const SpeechBrokerVoiceModelInfo* info{ nullptr };
			const SpeechBrokerVoiceModel*     model{ nullptr };
			SpeechBrokerVoiceSession*         session{ nullptr };
			std::int32_t                      status{ SPEECHBROKERVOICE_REFUSED };
		};

		struct CompleteContext
		{
			SpeechBrokerVoiceHandle        handle{ 0 };
			const SpeechBrokerVoiceAnswer* answer{ nullptr };
			std::int32_t                   status{ SPEECHBROKERVOICE_REFUSED };
		};

		struct HandleContext
		{
			SpeechBrokerVoiceHandle handle{ 0 };
		};

		struct ReadyContext
		{
			SpeechBrokerVoiceHandle handle{ 0 };
			std::int32_t            ready{ 0 };
			const char*             reason{ nullptr };
		};

		struct LogContext
		{
			SpeechBrokerVoiceHandle handle{ 0 };
			std::int32_t            level{ 0 };
			const char*             key{ nullptr };
			const char* const*      args{ nullptr };
			std::int32_t            argCount{ 0 };
		};

		void RegisterBody(void* a_context)
		{
			auto* ctx = static_cast<RegisterContext*>(a_context);
			ctx->status = Host::Get().Register(ctx->info, ctx->model, ctx->session);
		}

		void CompleteBody(void* a_context)
		{
			auto* ctx = static_cast<CompleteContext*>(a_context);
			ctx->status = Host::Get().Complete(ctx->handle, ctx->answer);
		}

		void UnregisterBody(void* a_context)
		{
			Host::Get().Unregister(static_cast<HandleContext*>(a_context)->handle);
		}

		void ReadyBody(void* a_context)
		{
			auto* ctx = static_cast<ReadyContext*>(a_context);
			Host::Get().Ready(ctx->handle, ctx->ready, ctx->reason);
		}

		void LogBody(void* a_context)
		{
			auto* ctx = static_cast<LogContext*>(a_context);
			Host::Get().Log(ctx->handle, ctx->level, ctx->key, ctx->args, ctx->argCount);
		}

		// The one place the inward direction says what a fault costs. A fault here
		// ran on the MODEL's thread, inside a call the adapter never made, and the
		// unwind ran no destructor of theirs: the game survived, the model did not.
		void EjectForFault(SpeechBrokerVoiceHandle a_handle, const char* a_call, const Fault& a_fault)
		{
			const auto model = Host::Get().Find(a_handle);
			const char* id = model ? model->Id().c_str() : "?";
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_FAULTED", id, a_call,
				FaultName(a_fault.code), a_fault.code, a_fault.address);
			if (model) {
				model->Eject(Ejection::Faulted);
			}
		}

		constexpr const char* kEntryRegister   = "Register";
		constexpr const char* kEntryUnregister = "Unregister";
		constexpr const char* kEntryComplete   = "Complete";
		constexpr const char* kEntryReady      = "Ready";
		constexpr const char* kEntryLog        = "Log";
	}

	// ----------------------------------------------------------------- Scheduler

	Scheduler::Scheduler() = default;

	void Scheduler::Arm(Clock::time_point a_when, const std::shared_ptr<Collector>& a_collector)
	{
		{
			std::lock_guard<std::mutex> lock(_lock);
			_armed.emplace(a_when, a_collector);
		}

		// One lock held for a few instructions and one signal. It never waits,
		// which is what lets it be called on the thread the microphone is being
		// drained on.
		_wake.notify_all();
	}

	void Scheduler::Begin(std::uint32_t a_stackGuaranteeBytes,
		std::function<void(std::shared_ptr<Collector>)> a_post)
	{
		_post = std::move(a_post);
		_stopping.store(false, std::memory_order_release);

		_thread = std::thread([this, a_stackGuaranteeBytes] {
			try {
				PrepareDispatchThread(a_stackGuaranteeBytes);

				for (;;) {
					std::shared_ptr<Collector> due;
					{
						std::unique_lock<std::mutex> lock(_lock);
						if (_armed.empty()) {
							_wake.wait(lock, [this] {
								return _stopping.load(std::memory_order_acquire) || !_armed.empty();
							});
						} else {
							// The longest this thread ever waits is to the nearest
							// armed deadline, which is what makes End bounded.
							_wake.wait_until(lock, _armed.begin()->first, [this] {
								return _stopping.load(std::memory_order_acquire);
							});
						}

						if (_stopping.load(std::memory_order_acquire)) {
							return;
						}
						if (_armed.empty()) {
							continue;
						}

						const auto at = _armed.begin();
						if (at->first > Clock::now()) {
							continue;  // something nearer was armed while we waited
						}

						// WEAKLY HELD. A pass that was closed early by its last
						// answer, assembled and finished with, must not be kept
						// alive to its deadline by a timer nobody cancelled - and
						// an expired entry is the ORDINARY case here, not an error.
						due = at->second.lock();
						_armed.erase(at);
					}

					// EXACTLY TWO THINGS, and outside the lock: mark it closed, and
					// post it. Anything heavy here would delay every other armed
					// deadline behind it and manufacture the out-of-order closes
					// the serial guard then has to catch (ModelHost.h, Scheduler).
					if (due && due->Close() && _post) {
						_post(std::move(due));
					}
				}
			} catch (...) {
				// An exception escaping a thread procedure is std::terminate.
			}
		});
	}

	void Scheduler::End() noexcept
	{
		_stopping.store(true, std::memory_order_release);
		_wake.notify_all();
		if (_thread.joinable()) {
			// Bounded, and that is the difference between this thread and a
			// model's: its longest wait is to the nearest armed deadline and it has
			// just been woken. Nothing of a third party's runs on it.
			_thread.join();
		}
	}

	// ---------------------------------------------------------------------- Host

	Host::Host() = default;

	Host& Host::Get()
	{
		// ONE PER PROCESS, and a function-local static so that it is built on first
		// use rather than in whatever order the loader felt like.
		static Host host;
		return host;
	}

	const SpeechBrokerVoiceHost* Host::Table() noexcept
	{
		// A FUNCTION-LOCAL STATIC WITH PROCESS LIFETIME, NEVER REPLACED. A shim may
		// keep this pointer for ever, and it must: Complete comes from its own
		// thread hundreds of milliseconds later, Ready at any time, Log after Stop.
		static const SpeechBrokerVoiceHost table = [] {
			SpeechBrokerVoiceHost built{};
			built.structBytes = static_cast<std::uint32_t>(sizeof(SpeechBrokerVoiceHost));
			built.abiVersion = SPEECHBROKERVOICE_MODEL_ABI_VERSION;
			built.Register = &Host::OnRegister;
			built.Unregister = &Host::OnUnregister;
			built.Complete = &Host::OnComplete;
			built.Ready = &Host::OnReady;
			built.Log = &Host::OnLog;
			return built;
		}();

		return &table;
	}

	void Host::Configure(HostSettings a_settings, EarsSettings a_ears, Publish a_publish)
	{
		_settings = std::move(a_settings);
		_earsSettings = std::move(a_ears);
		_publish = std::move(a_publish);

		// THE STANDINGS AND THE ARBITER ARE BUILT HERE AND NOT IN Begin, and that
		// is not tidiness: Register runs at plugin load, long before Begin, and it
		// has to hand a model its record. Building them later would mean either a
		// null dereference on the game thread or a second, lazy construction path
		// racing every registration. The CALIBRATION is still read in Begin - it
		// touches the disk, which the game thread may not.
		_standings = std::make_unique<Reputations>(_settings.standing);
		_arbiter = std::make_unique<Arbiter>(_settings.arbiter, *_standings);
	}

	bool Host::Begin()
	{
		if (!_standings || !_arbiter) {
			// Configure was never called. Say so and do nothing: starting a
			// microphone nothing will listen to helps nobody.
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_NOT_CONFIGURED");
			return false;
		}

		// The calibration, and FAILURE IS NOT AN ERROR: no file, a file that does
		// not parse, a file from an older version - all of them leave every
		// standing uncalibrated, which is a state the arithmetic already handles.
		const auto folder = AdapterFolder();
		const auto calibration = ResolveInside(folder, _settings.standing.file);
		if (_settings.standing.file.empty()) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_HOST_NO_CALIBRATION_SET");
		} else if (calibration.empty()) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_HOST_CALIBRATION_OUTSIDE", _settings.standing.file);
		} else if (!_standings->Load(calibration)) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_HOST_NO_CALIBRATION", calibration.string());
		} else {
			Loc::Info("$SPEECHBROKERVOICE_LOG_HOST_CALIBRATION", calibration.string());
		}

		// ONE SECOND OF DIGITAL SILENCE, MADE ONCE AND SHARED BY EVERY PROBE OF THE
		// SESSION. It is an immutable Snapshot like any other, so every model gets
		// the same pointer and the claim rules apply to it unchanged.
		{
			const auto samples = static_cast<std::size_t>(
				SecondsToSamples(std::max(0.0, _settings.dispatch.probeSeconds)));
			std::lock_guard<std::mutex> lock(_collectors);
			_probeAudio = std::make_shared<const std::vector<Sample>>(samples, 0.0f);
		}

		_ears = std::make_unique<Ears>(_earsSettings);
		_ears->OnPass([this](const Pass& a_pass) { OnPass(a_pass); });

		_stopping.store(false, std::memory_order_release);
		_scheduler.Begin(_settings.stackGuaranteeBytes, [this](std::shared_ptr<Collector> a_closed) {
			PostClosed(std::move(a_closed));
		});

		const int workers = std::max(1, _settings.workers);
		for (int i = 0; i < workers; ++i) {
			_workers.emplace_back([this] {
				try {
					PrepareDispatchThread(_settings.stackGuaranteeBytes);

					// THE WORKER'S OWN TRACKER, built once and lent to every
					// Assemble it runs. One tracker belongs to one thread
					// (Prosody.h, PitchTracker).
					PitchTracker scratch(_earsSettings.prosody);
					scratch.Prepare();

					for (;;) {
						std::shared_ptr<Collector> job;
						{
							std::unique_lock<std::mutex> lock(_work);
							_workWake.wait(lock, [this] {
								return _stopping.load(std::memory_order_acquire) || !_queue.empty();
							});
							if (_queue.empty()) {
								return;  // stopping, and nothing left to assemble
							}

							// FIRST IN, FIRST ASSEMBLED. The serial guard catches an
							// out-of-order close, but it is a safety net and not a
							// working part, and taking the newest first would make it
							// one. The queue holds a handful of passes, so the erase
							// from the front costs nothing worth measuring.
							job = std::move(_queue.front());
							_queue.erase(_queue.begin());
						}

						try {
							Assemble(job, scratch);
						} catch (const std::exception& e) {
							Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_WORKER_THREW",
								job ? job->UtteranceId() : 0, e.what());
						} catch (...) {
							Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_WORKER_THREW",
								job ? job->UtteranceId() : 0,
								Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
						}
					}
				} catch (...) {
					// An exception escaping a thread procedure is std::terminate.
				}
			});
		}

		// EVERY MODEL THAT REGISTERED BEFORE NOW GETS ITS THREAD HERE, and the
		// latch is set under the same lock that took the list: a model that
		// registers between the two would otherwise be started by neither.
		std::vector<std::shared_ptr<Model>> registered;
		{
			std::lock_guard<std::mutex> lock(_registry);
			registered.reserve(_byHandle.size());
			for (const auto& entry : _byHandle) {
				registered.push_back(entry.second);
			}
			_begun.store(true, std::memory_order_release);
		}

		// Started BEFORE the microphone, deliberately: the probe is also the
		// warm-up, and it is meant to pay for workspace allocation and autotune
		// while the game is still loading rather than on the first thing the player
		// says (contract, "The probe").
		for (const auto& model : registered) {
			model->Begin();
		}

		const bool heard = _ears->Start();
		if (!heard) {
			// A microphone that failed is a reason to say so loudly, not a reason
			// to leave three model mods unloaded.
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_NO_EARS");
		}

		Loc::Info("$SPEECHBROKERVOICE_LOG_HOST_UP", registered.size(), workers,
			_ears->MaxRequestSamples());
		return heard;
	}

	// --- the registry --------------------------------------------------------

	std::int32_t Host::Register(const SpeechBrokerVoiceModelInfo* a_info,
		const SpeechBrokerVoiceModel* a_model, SpeechBrokerVoiceSession* a_session)
	{
		// THE ORDER BELOW IS THE SPECIFICATION (ModelHost.h, Register). Every step
		// is numbered there and here, so that a reader can check one against the
		// other without reconstructing it.

		// 1.
		if (a_info == nullptr || a_model == nullptr || a_session == nullptr) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "NULL");
			return SPEECHBROKERVOICE_MALFORMED;
		}

		// 2.
		if (!StructOk(a_info->structBytes, SPEECHBROKERVOICE_MODELINFO_BYTES_V1)) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "info.structBytes");
			return SPEECHBROKERVOICE_MALFORMED;
		}
		if (!StructOk(a_model->structBytes, SPEECHBROKERVOICE_MODEL_BYTES_V1)) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "model.structBytes");
			return SPEECHBROKERVOICE_MALFORMED;
		}
		if (!StructOk(a_session->structBytes, SPEECHBROKERVOICE_SESSION_BYTES_V1)) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "session.structBytes");
			return SPEECHBROKERVOICE_MALFORMED;
		}

		// 3. Not ==, and not > either: compatibility made from one side is not
		// compatibility, and strict equality one level up knocked the bridge's own
		// adapter out entirely (contract, Host::Register).
		if (a_info->abiVersion < 1u) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_VERSION", a_info->abiVersion);
			return SPEECHBROKERVOICE_VERSION;
		}

		// 4.
		if (a_info->reserved0 != 0u) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "info.reserved0");
			return SPEECHBROKERVOICE_MALFORMED;
		}
		if (a_model->reserved0 != 0u) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "model.reserved0");
			return SPEECHBROKERVOICE_MALFORMED;
		}

		// 5. Every string is copied HERE, inside the call, because what was handed
		// over is borrowed for the length of it and no longer.
		const auto id = CopyString(a_info->id);
		if (id.text.empty()) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "info.id");
			return SPEECHBROKERVOICE_MALFORMED;
		}

		const auto name = CopyString(a_info->name);
		const auto language = CopyString(a_info->language);
		const auto provides = CopyString(a_info->provides);
		if (id.truncated || name.truncated || language.truncated || provides.truncated) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_TRUNCATED", id.text,
				SPEECHBROKERVOICE_MAX_STRING_BYTES);
		}

		if (!_standings || !_arbiter) {
			// REFUSED and not MALFORMED: the adapter is in a state that cannot take
			// a registration, and that is not the shim's defect. It is never
			// counted against a model.
			Loc::Error("$SPEECHBROKERVOICE_LOG_REGISTER_NOT_CONFIGURED", id.text);
			return SPEECHBROKERVOICE_REFUSED;
		}

		// 6. FIRST WINS. The lookup and the insertion are under ONE hold of the
		// registry lock, with steps 7 to 9 inside it: two shims registering the
		// same id from two message handlers would otherwise both pass the lookup.
		// Nothing slow runs under it - by here every string is already copied.
		{
			std::lock_guard<std::mutex> lock(_registry);

			if (_byId.find(id.text) != _byId.end()) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_DUPLICATE", id.text);
				return SPEECHBROKERVOICE_DUPLICATE;
			}

			// 7. Silence about a mandatory entry reads as permission, so the line
			// names which one is missing.
			if (a_model->Start == nullptr) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "model.Start");
				return SPEECHBROKERVOICE_MALFORMED;
			}
			if (a_model->Stop == nullptr) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "model.Stop");
				return SPEECHBROKERVOICE_MALFORMED;
			}
			if (a_model->Submit == nullptr) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "model.Submit");
				return SPEECHBROKERVOICE_MALFORMED;
			}

			// 8.
			std::string  unknown;
			const auto   known = ParseProvides(provides.text, unknown);
			if (!unknown.empty()) {
				Loc::Info("$SPEECHBROKERVOICE_LOG_REGISTER_PROVIDES_UNKNOWN", id.text, unknown);
			}
			if (known == kProvidesNothing) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_MALFORMED", "info.provides");
				return SPEECHBROKERVOICE_MALFORMED;
			}

			// 9. REFUSED HERE AND NOWHERE LATER, before Start is ever called, so a
			// forbidden model never resolves a name, never opens a socket, never
			// starts a process and never contacts anybody.
			if (!_settings.allow.Allows(a_info->kind)) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_KIND_REFUSED", id.text,
					KindWord(a_info->kind), a_info->kind);
				return SPEECHBROKERVOICE_REFUSED;
			}

			ModelInfo info;
			info.id = id.text;
			info.name = name.text.empty() ? id.text : name.text;
			info.language = FirstSubtag(language.text);
			info.providesRaw = provides.text;
			info.provides = known;
			info.kind = a_info->kind;

			// THE VERSION SETTLED HERE IS min(theirs, ours) and it governs every
			// struct in the contract that carries no abiVersion of its own - which
			// is what a fragmentStride is later checked against, exactly.
			info.abiVersion = std::min<std::uint32_t>(a_info->abiVersion,
				SPEECHBROKERVOICE_MODEL_ABI_VERSION);

			// The scalars are normalised once, here, so that nothing below ever has
			// to remember what a zero meant.
			info.budgetMs = a_info->budgetMs != 0u
				? a_info->budgetMs
				: static_cast<std::uint32_t>(std::max(0, _settings.dispatch.defaultBudgetMs));
			info.startupMs = a_info->startupMs;
			info.stopMs = a_info->stopMs;
			info.maxInFlight = a_info->maxInFlight != 0u ? a_info->maxInFlight : 1u;
			info.finalOnly = a_info->finalOnly != 0;

			// 0 IS READ AS ACCURATE - the cautious reading, which costs the model
			// interim passes rather than costing the player a late draft.
			info.declaredClass = a_info->declaredClass == SPEECHBROKERVOICE_CLASS_FAST
				? Speed::Fast
				: Speed::Accurate;

			const auto handle = static_cast<SpeechBrokerVoiceHandle>(
				_nextHandle.fetch_add(1, std::memory_order_acq_rel));

			auto& standing = _standings->Of(info.id);
			standing.Declare(info.declaredClass, info.budgetMs);

			auto model = Model::Make(handle, info, *a_model, _settings.dispatch, standing, *this);
			_byHandle.emplace(handle, model);
			_byId.emplace(info.id, handle);

			// WRITE NOTHING INTO a_session ON ANY STATUS BUT OK - which is why this
			// is the last thing that happens and not the first.
			//
			// AND THE OUT-PARAMETER RULE IS INVERTED: structBytes is the size of the
			// buffer THE SHIM allocated, we write min(that, our own sizeof) bytes,
			// and we never write our own sizeof into it. An adapter one version
			// newer than a shim is accepted by design, and without this rule that
			// acceptance would put its extra fields past the end of the shim's
			// stack object.
			SpeechBrokerVoiceSession filled{};
			filled.structBytes = a_session->structBytes;
			filled.abiVersion = info.abiVersion;
			filled.handle = handle;
			filled.maxRequestSamples = MaxRequestSamples();
			filled.format.structBytes = static_cast<std::uint32_t>(sizeof(SpeechBrokerVoiceFormat));
			filled.format.sampleRate = kTargetSampleRate;
			filled.format.channels = 1u;
			filled.format.sampleFormat = SPEECHBROKERVOICE_FMT_FLOAT32;

			const auto bytes = std::min<std::size_t>(a_session->structBytes,
				sizeof(SpeechBrokerVoiceSession));
			std::memcpy(a_session, &filled, bytes);

			Loc::Info("$SPEECHBROKERVOICE_LOG_REGISTER_OK", info.id, info.name, handle,
				KindWord(info.kind), info.language, info.budgetMs, info.maxInFlight,
				info.abiVersion);

			// THE DECLARATION IS UNVERIFIABLE, so the log writes it out in words at
			// every registration - that is the whole of what the player gets in
			// exchange for a field nothing can check (contract, SpeechBrokerVoiceKind).
			if (info.LeavesMachine()) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REGISTER_LEAVES_MACHINE", info.id);
			}

			// A model that registered after Begin has missed the pass where every
			// thread was started, so it gets its own here and now.
			if (_begun.load(std::memory_order_acquire)) {
				model->Begin();
			}
		}

		return SPEECHBROKERVOICE_OK;
	}

	void Host::Unregister(SpeechBrokerVoiceHandle a_handle)
	{
		const auto model = Find(a_handle);
		if (!model) {
			return;
		}

		Loc::Info("$SPEECHBROKERVOICE_LOG_UNREGISTER", model->Id());

		// THE REGISTRY LOCK IS RELEASED BEFORE THE WAIT, always - Find took it and
		// gave it back. Complete, Ready and Log on a draining handle read one
		// atomic and return at once, so Complete never blocks on this and the two
		// cannot deadlock against each other.
		model->BeginDrain();

		// A REMOVAL AND NOT A TIMEOUT: it does not cost the model its standing.
		RemoveEverywhere(a_handle, Removal::Unregistered);

		// Its probe is retired now, and only now: a probe belongs to no turn and no
		// high-water mark may retire it (contract, "A REQUEST WITH deadlineMs 0").
		{
			std::lock_guard<std::mutex> lock(_collectors);
			for (auto at = _probes.begin(); at != _probes.end();) {
				const auto expected = at->second->Expected();
				if (std::find(expected.begin(), expected.end(), a_handle) != expected.end()) {
					at = _probes.erase(at);
				} else {
					++at;
				}
			}
		}

		if (!model->WaitDrained(model->Info().stopMs)) {
			// NOT KILLED, NOT JOINED, NOT FREED. Killing a thread inside
			// third-party code leaves every lock it held permanently held,
			// including CRT and loader locks, which turns a hung recogniser into a
			// hung game and, at exit, into the case where MO2 still believes the
			// game is running.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_UNREGISTER_ABANDONED", model->Id(),
				model->Info().stopMs);
		}
	}

	std::shared_ptr<Model> Host::Find(SpeechBrokerVoiceHandle a_handle) const
	{
		std::lock_guard<std::mutex> lock(_registry);
		const auto at = _byHandle.find(a_handle);
		return at == _byHandle.end() ? nullptr : at->second;
	}

	// --- what a model calls --------------------------------------------------

	std::int32_t Host::Complete(SpeechBrokerVoiceHandle a_handle,
		const SpeechBrokerVoiceAnswer* a_answer)
	{
		const auto model = Find(a_handle);
		if (!model) {
			return SPEECHBROKERVOICE_STALE;
		}

		// ONE ATOMIC AND RETURN AT ONCE on a draining or dead handle. That is what
		// makes Complete unable to deadlock against Unregister.
		const auto where = model->Where();
		if (where == Life::Draining || where == Life::Ejected) {
			return SPEECHBROKERVOICE_STALE;
		}

		// THE ORDER BELOW IS THE SPECIFICATION, and the count and the stride are
		// checked BEFORE `fragments` is dereferenced even once (ModelHost.h,
		// Complete).
		if (a_answer == nullptr) {
			// Nothing is counted against it here: a NULL answer names no utterance,
			// so there is no reading to blame it for.
			Loc::Warn("$SPEECHBROKERVOICE_LOG_ANSWER_MALFORMED", model->Id(), "NULL");
			return SPEECHBROKERVOICE_MALFORMED;
		}

		const auto malformed = [&](const char* a_field) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_ANSWER_MALFORMED", model->Id(), a_field);
			_standings->Of(model->Id()).NoteFailure(false);
			return SPEECHBROKERVOICE_MALFORMED;
		};

		if (!StructOk(a_answer->structBytes, SPEECHBROKERVOICE_ANSWER_BYTES_V1)) {
			return malformed("answer.structBytes");
		}
		if (a_answer->abiVersion < 1u || a_answer->abiVersion > model->Info().abiVersion) {
			return malformed("answer.abiVersion");
		}
		if (a_answer->status != SPEECHBROKERVOICE_OK &&
			a_answer->status != SPEECHBROKERVOICE_CANCELLED &&
			a_answer->status != SPEECHBROKERVOICE_FAILED) {
			// A zero here is not "success by default", it is REFUSED wearing the
			// clothes of an uninitialised field.
			return malformed("answer.status");
		}
		if (a_answer->fragmentCount < 0 || a_answer->fragmentCount > SPEECHBROKERVOICE_MAX_FRAGMENTS) {
			return malformed("answer.fragmentCount");
		}
		if (a_answer->fragmentCount > 0 && a_answer->fragments == nullptr) {
			return malformed("answer.fragments");
		}
		if (a_answer->fragmentCount > 0 &&
			a_answer->fragmentStride != static_cast<std::uint32_t>(sizeof(SpeechBrokerVoiceFragment))) {
			// AN EQUALITY, NOT A RANGE. A stride of 0 makes every element alias
			// element 0, and fragmentCount copies of one fragment would walk into
			// the arbiter as independent evidence with nothing looking wrong.
			return malformed("answer.fragmentStride");
		}

		const auto* first = a_answer->fragments;
		const auto  count = static_cast<std::size_t>(a_answer->fragmentCount);
		const auto  stride = static_cast<std::size_t>(a_answer->fragmentStride);
		const auto  fragmentAt = [first, stride](std::size_t a_index) {
			return reinterpret_cast<const SpeechBrokerVoiceFragment*>(
				reinterpret_cast<const char*>(first) + a_index * stride);
		};

		// The order and the bounds are part of the contract, because the adapter
		// matches by TIME and has no defined behaviour on a tangle.
		std::int32_t previousEnd = 0;
		for (std::size_t i = 0; i < count; ++i) {
			const auto* piece = fragmentAt(i);
			if (piece->startMs > piece->endMs) {
				return malformed("fragment.startMs");
			}
			if (i > 0 && piece->startMs < previousEnd) {
				return malformed("fragment.order");
			}
			previousEnd = piece->endMs;
		}

		// THE COLLECTOR IS FOUND BEFORE THE COPY and not after it, because the copy
		// CLAMPS every time into the buffer and the length of the buffer is the
		// collector's to state. Retirement is membership in this map and never a
		// high-water mark: an utteranceId that names no open collector is retired,
		// and that is one lookup.
		const auto collector = Collecting(a_answer->utteranceId);
		if (!collector) {
			model->ReleaseDebt(a_answer->utteranceId);
			return SPEECHBROKERVOICE_STALE;
		}

		const auto duration = collector->Subject().DurationMs();

		Reading reading;
		reading.handle = a_handle;
		reading.modelId = model->Id();
		reading.status = a_answer->status;
		reading.latencyMs = a_answer->latencyMs;
		reading.lostSamples = a_answer->lostSamples;

		bool truncated = false;
		bool clamped = false;

		if (a_answer->status == SPEECHBROKERVOICE_FAILED) {
			const auto failed = CopyString(a_answer->failed);
			reading.failed = failed.text;
			truncated = truncated || failed.truncated;
		}

		reading.fragments.reserve(count);
		for (std::size_t i = 0; i < count; ++i) {
			const auto* piece = fragmentAt(i);

			Fragment kept;
			const auto text = CopyString(piece->text);
			truncated = truncated || text.truncated;
			kept.text = text.text;
			kept.score = piece->score;

			// Times outside the buffer are CLAMPED and logged once rather than
			// refusing a whole reading: a model's last timestamp genuinely overruns
			// by a few tens of milliseconds and refusing for that would be absurd.
			const auto clampMs = [&](std::int32_t a_ms) {
				if (a_ms < 0) {
					clamped = true;
					return 0u;
				}
				if (static_cast<std::uint32_t>(a_ms) > duration) {
					clamped = true;
					return duration;
				}
				return static_cast<std::uint32_t>(a_ms);
			};
			kept.startMs = clampMs(piece->startMs);
			kept.endMs = clampMs(piece->endMs);

			// THE SENTINELS, AND NEVER ZERO. The dangerous one is lastWordProb: the
			// test that promotes a model to the lane is `>= 0`, so a value-
			// initialised field passes it and a model that sent no word timings at
			// all would win the lane (Collector.h, CarriesWordTimings).
			kept.signs.endsSentence = piece->endsSentence == 1;
			kept.signs.lastWordProb = piece->lastWordProb < 0.0f ? -1.0f : piece->lastWordProb;
			kept.signs.noSpeechProb = piece->noSpeechProb < 0.0f ? 0.0f : piece->noSpeechProb;
			kept.signs.medianGapMs = piece->medianGapMs < 0
				? 0u
				: static_cast<std::uint32_t>(piece->medianGapMs);
			kept.signs.words = piece->words < 0 ? 0u : static_cast<std::uint32_t>(piece->words);

			reading.fragments.push_back(std::move(kept));
		}

		// ONCE PER MODEL, not once per string and not once per fragment.
		if (truncated && model->FirstTruncation()) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_ANSWER_TRUNCATED", model->Id(),
				SPEECHBROKERVOICE_MAX_STRING_BYTES);
		}
		if (clamped && model->FirstClamp()) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_ANSWER_CLAMPED", model->Id(), duration);
		}

		// A PROBE COLLECTOR - the one without a deadline. Its outcome feeds the
		// invention record only, it is never assembled and it never reaches a
		// worker: there is no turn for it to be reconciled against.
		if (!collector->Deadline().has_value()) {
			bool invented = false;
			for (const auto& piece : reading.fragments) {
				if (piece.text.find_first_not_of(" \t\r\n") != std::string::npos) {
					invented = true;
					break;
				}
			}

			model->ReleaseDebt(a_answer->utteranceId);
			model->ProbeAnswered(invented);
			collector->Deliver(a_handle, std::move(reading));
			collector->Close();
			return SPEECHBROKERVOICE_OK;
		}

		// The debt is discharged by any status, and it is discharged whether or not
		// the answer was admitted: a model that answered is a model that is no
		// longer carrying the utterance, and leaving the debt standing would hold
		// maxInFlight against it for ever.
		const bool owed = model->ReleaseDebt(a_answer->utteranceId);
		if (!owed) {
			// A second Complete for one utteranceId, which is how the debt catches
			// it (Model.h, ReleaseDebt).
			Loc::Warn("$SPEECHBROKERVOICE_LOG_ANSWER_TWICE", model->Id(), a_answer->utteranceId);
		}

		if (!collector->Deliver(a_handle, std::move(reading))) {
			// Already closed, or not in the expected set. STALE is not an
			// accusation of anything.
			return SPEECHBROKERVOICE_STALE;
		}

		// WHEN THIS FILLS THE EXPECTED SET the post happens HERE, on the model's
		// own thread, because there is no other thread that knows it happened.
		if (collector->AllAnswered() && collector->Close()) {
			PostClosed(collector);
		}

		return SPEECHBROKERVOICE_OK;
	}

	void Host::Ready(SpeechBrokerVoiceHandle a_handle, std::int32_t a_ready, const char* a_reason)
	{
		const auto model = Find(a_handle);
		if (!model) {
			return;
		}

		const auto where = model->Where();
		if (where == Life::Draining || where == Life::Ejected) {
			return;
		}

		const auto reason = CopyString(a_reason);
		if (a_ready != 0) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_MODEL_SAYS_READY", model->Id());
		} else {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_SAYS_NOT_READY", model->Id(),
				reason.text.empty() ? Loc::Get("$SPEECHBROKERVOICE_WORD_NOTHING") : reason.text.c_str());
		}

		model->NoteReady(a_ready != 0);
	}

	void Host::Log(SpeechBrokerVoiceHandle a_handle, std::int32_t a_level, const char* a_key,
		const char* const* a_args, std::int32_t a_argCount)
	{
		const auto model = Find(a_handle);
		if (!model) {
			return;
		}

		const auto where = model->Where();
		if (where == Life::Draining || where == Life::Ejected) {
			return;
		}

		if (a_key == nullptr || a_argCount < 0 || a_argCount > SPEECHBROKERVOICE_MAX_LOG_ARGS ||
			(a_args == nullptr && a_argCount > 0)) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_LINE_DROPPED", model->Id(), a_argCount);
			return;
		}

		// THE KEY VERBATIM, THEN THE ARGUMENTS IN ORDER, TAB SEPARATED, AND
		// NOTHING SUBSTITUTED. The key belongs to the model's own table with its
		// own prefix; the adapter neither parses nor translates it, because it does
		// not have that table and because otherwise every new model mod would mean
		// an edit to the adapter. So this line does NOT go through Loc - the text
		// is the model's, not ours.
		std::string line = model->Id();
		line += '\t';
		line += CopyString(a_key).text;

		for (std::int32_t i = 0; i < a_argCount; ++i) {
			if (a_args[i] == nullptr) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_MODEL_LINE_DROPPED", model->Id(), a_argCount);
				return;
			}
			line += '\t';
			line += CopyString(a_args[i]).text;
		}

		spdlog::log(LevelOf(a_level), "{}", line);
	}

	// --- the passes ----------------------------------------------------------

	void Host::OnPass(const Pass& a_pass)
	{
		// THE WHOLE BODY IN A catch(...): this runs on the ears' consumer thread,
		// and an exception escaping a thread procedure is std::terminate.
		try {
			std::vector<std::shared_ptr<Model>> roster;
			std::size_t                         registered = 0;
			bool                                everyoneFinalOnly = true;

			{
				std::lock_guard<std::mutex> lock(_registry);
				registered = _byHandle.size();
				for (const auto& entry : _byHandle) {
					const auto& model = entry.second;
					if (!model->Info().finalOnly) {
						everyoneFinalOnly = false;
					}
					if (model->TakesPass(a_pass.final)) {
						roster.push_back(model);
					}
				}
			}

			if (roster.empty()) {
				// THE SERIAL IS SPENT EITHER WAY and the gap in the numbers is the
				// record that it happened.
				if (!a_pass.final && registered > 0 && everyoneFinalOnly) {
					// SAID ONCE. It is a property of the installation and not of
					// this pause between phrases, and the adapter does NOT ask a
					// finalOnly model anyway for want of anybody else - that would
					// break the one declaration measurement does not override.
					if (!_saidFinalOnly.exchange(true, std::memory_order_acq_rel)) {
						Loc::Info("$SPEECHBROKERVOICE_LOG_PASS_ALL_FINAL_ONLY");
					}
				} else {
					Loc::Debug("$SPEECHBROKERVOICE_LOG_PASS_NO_ROSTER", a_pass.turnId, a_pass.serial);
				}
				return;
			}

			const auto utteranceId = _nextUtterance.fetch_add(1, std::memory_order_acq_rel);

			// ONE ABSOLUTE DEADLINE, STAMPED HERE, AT PASS CREATION, and it is the
			// LONGEST of the roster's: a pass sealed on the quickest model's budget
			// would close under the accurate one every single time.
			std::int32_t budgetMs = 0;
			std::vector<SpeechBrokerVoiceHandle> expected;
			expected.reserve(roster.size());
			for (const auto& model : roster) {
				budgetMs = std::max(budgetMs, model->BudgetMs());
				expected.push_back(model->Handle());
			}

			const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(1, budgetMs));
			auto collector = std::make_shared<Collector>(utteranceId, a_pass, std::move(expected),
				std::optional<Clock::time_point>(deadline));

			{
				std::lock_guard<std::mutex> lock(_collectors);
				_open.emplace(utteranceId, collector);
			}

			_scheduler.Arm(deadline, collector);

			Loc::Debug("$SPEECHBROKERVOICE_LOG_PASS_OPENED", a_pass.turnId, a_pass.serial,
				utteranceId, roster.size(), budgetMs);

			// NO Submit HAPPENS HERE. Every Submit is made on that model's own
			// dispatch thread, which is what lets this function be entered on the
			// thread the microphone is being drained on: every step above is a lock
			// held for a few instructions and a shared_ptr copied.
			for (const auto& model : roster) {
				DispatchEntry entry;
				entry.audio = a_pass.audio;
				entry.collector = collector;
				entry.utteranceId = utteranceId;
				entry.turnId = a_pass.turnId;
				entry.serial = a_pass.serial;
				entry.final = a_pass.final;
				entry.lostSamples = a_pass.lostSamples;
				entry.probe = false;

				const auto outcome = model->Offer(std::move(entry));
				Loc::Debug("$SPEECHBROKERVOICE_LOG_PASS_QUEUED", utteranceId, model->Id(),
					QueuedWord(outcome));
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_PASS_THREW", a_pass.turnId, a_pass.serial, e.what());
		} catch (...) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_PASS_THREW", a_pass.turnId, a_pass.serial,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
		}
	}

	std::shared_ptr<Collector> Host::Collecting(std::int64_t a_utteranceId) const
	{
		std::lock_guard<std::mutex> lock(_collectors);

		const auto timed = _open.find(a_utteranceId);
		if (timed != _open.end()) {
			return timed->second;
		}

		// THE WHOLE REASON THERE ARE TWO MAPS. A probe is retired only when its
		// model unregisters or the session ends; a high-water mark over utterance
		// ids would retire it the moment any later pass closed, silently disabling
		// the one check in this system that does not rest on trusting a model.
		const auto probe = _probes.find(a_utteranceId);
		return probe == _probes.end() ? nullptr : probe->second;
	}

	// --- taking a model out of a pass ----------------------------------------

	void Host::Drop(const std::shared_ptr<Collector>& a_collector, SpeechBrokerVoiceHandle a_handle,
		Removal a_why)
	{
		if (!a_collector) {
			return;
		}

		if (!a_collector->Remove(a_handle, a_why)) {
			return;  // idempotent: it was not there, and that is not an event
		}

		Loc::Debug("$SPEECHBROKERVOICE_LOG_REMOVED", a_collector->UtteranceId(), a_handle,
			RemovalWord(a_why));

		// IF THIS EMPTIES WHAT WAS LEFT UNANSWERED the pass is complete, and a pass
		// whose last two participants both went away must not sit until its
		// deadline. A probe is never posted: it belongs to no turn, so there is
		// nothing for a worker to reconcile it against.
		if (a_collector->Deadline().has_value() && a_collector->AllAnswered() &&
			a_collector->Close()) {
			PostClosed(a_collector);
		}
	}

	void Host::RemoveEverywhere(SpeechBrokerVoiceHandle a_handle, Removal a_why)
	{
		// The list is taken under the lock and the dropping happens outside it:
		// Drop writes a log line and may post to a worker, and neither belongs
		// under the lock that every Complete takes.
		std::vector<std::shared_ptr<Collector>> open;
		{
			std::lock_guard<std::mutex> lock(_collectors);
			open.reserve(_open.size());
			for (const auto& entry : _open) {
				open.push_back(entry.second);
			}
		}

		for (const auto& collector : open) {
			Drop(collector, a_handle, a_why);
		}
	}

	void Host::PostClosed(std::shared_ptr<Collector> a_collector)
	{
		if (!a_collector) {
			return;
		}

		{
			std::lock_guard<std::mutex> lock(_work);
			_queue.push_back(std::move(a_collector));
		}
		_workWake.notify_one();
	}

	// --- the vocabulary ------------------------------------------------------

	void Host::SetVocabulary(std::vector<std::string> a_phrases)
	{
		// CLIPPED HERE, because the list is merged across every installed
		// subscriber by the bridge and is otherwise unbounded. The order is the
		// merge order and means nothing; it is not a ranking.
		if (a_phrases.size() > SPEECHBROKERVOICE_MAX_VOCABULARY) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_VOCABULARY_CLIPPED", a_phrases.size(),
				SPEECHBROKERVOICE_MAX_VOCABULARY);
			a_phrases.resize(SPEECHBROKERVOICE_MAX_VOCABULARY);
		}

		std::vector<std::shared_ptr<Model>> models;
		{
			std::lock_guard<std::mutex> lock(_registry);
			models.reserve(_byHandle.size());
			for (const auto& entry : _byHandle) {
				models.push_back(entry.second);
			}
		}

		for (const auto& model : models) {
			// Each gets its own copy: it is delivered on that model's own dispatch
			// thread, at a moment of that thread's choosing, and the strings must
			// outlive this call by exactly that much.
			model->SetVocabulary(a_phrases);
		}
	}

	std::vector<Reputation::Numbers> Host::Report() const
	{
		return _standings ? _standings->Report() : std::vector<Reputation::Numbers>{};
	}

	// --- the probe -----------------------------------------------------------

	void Host::SubmitProbe(const std::shared_ptr<Model>& a_model)
	{
		if (!a_model) {
			return;
		}

		Snapshot audio;
		{
			std::lock_guard<std::mutex> lock(_collectors);
			if (!_probeAudio) {
				const auto samples = static_cast<std::size_t>(
					SecondsToSamples(std::max(0.0, _settings.dispatch.probeSeconds)));
				_probeAudio = std::make_shared<const std::vector<Sample>>(samples, 0.0f);
			}
			audio = _probeAudio;
		}

		const auto utteranceId = _nextUtterance.fetch_add(1, std::memory_order_acq_rel);

		// NEGATIVE, so that it can never collide with a turnId out of the ears -
		// which start at 1 and only rise - and so that a log line naming one is
		// recognisable at a glance.
		const auto turnId = _nextProbeTurn.fetch_sub(1, std::memory_order_acq_rel);

		// AN ORDINARY REQUEST IN EVERY FIELD A SHIM CAN SEE. There is no probe flag
		// in the contract and there must never be one: a flag would make cheating
		// free and obvious and turn the one check that does not rest on believing a
		// model into a check a model opts out of.
		Pass subject;
		subject.audio = audio;
		subject.turnId = turnId;
		subject.serial = 1;
		subject.final = true;  // the buffer will not grow
		subject.sampleRate = kTargetSampleRate;
		subject.lostSamples = 0;
		subject.tailSilenceMs = 0;
		subject.anchors = std::make_shared<const std::vector<std::uint32_t>>();

		// NO DEADLINE: it is not timed, and it is retired only when its model
		// unregisters or the session ends.
		auto collector = std::make_shared<Collector>(utteranceId, subject,
			std::vector<SpeechBrokerVoiceHandle>{ a_model->Handle() },
			std::optional<Clock::time_point>{});

		{
			std::lock_guard<std::mutex> lock(_collectors);
			_probes.emplace(utteranceId, collector);
		}

		DispatchEntry entry;
		entry.audio = audio;
		entry.collector = collector;
		entry.utteranceId = utteranceId;
		entry.turnId = turnId;
		entry.serial = 1;
		entry.final = true;
		entry.lostSamples = 0;
		entry.probe = true;

		a_model->Offer(std::move(entry));

		Loc::Info("$SPEECHBROKERVOICE_LOG_PROBE_SENT", a_model->Id(), utteranceId,
			subject.DurationMs());
	}

	// --- the workers ---------------------------------------------------------

	void Host::Assemble(const std::shared_ptr<Collector>& a_collector, PitchTracker& a_scratch)
	{
		if (!a_collector || !_arbiter || !_standings) {
			return;
		}

		const auto& subject = a_collector->Subject();

		// (1) EVERYONE STILL EXPECTED AND STILL SILENT AT THE CLOSE. Their debt is
		// discharged at the same moment and without their doing anything: after the
		// close their answer is STALE and they may stop carrying it.
		for (const auto handle : a_collector->Missing()) {
			const auto model = Find(handle);
			if (!model) {
				continue;
			}
			model->ReleaseDebt(a_collector->UtteranceId());
			_standings->Of(model->Id()).NoteFailure(true);
			Loc::Debug("$SPEECHBROKERVOICE_LOG_TIMEOUT", a_collector->UtteranceId(), model->Id());
		}

		// (2) What did arrive.
		const auto& answers = a_collector->Answers();
		for (const auto& reading : answers) {
			auto& standing = _standings->Of(reading.modelId);
			if (reading.status == SPEECHBROKERVOICE_OK) {
				std::vector<float> scores;
				scores.reserve(reading.fragments.size());
				for (const auto& piece : reading.fragments) {
					scores.push_back(piece.score);
				}
				standing.NoteCall(reading.latencyMs, scores);
			} else if (reading.status == SPEECHBROKERVOICE_FAILED) {
				// A failure is an answer too: it is counted, it costs the model its
				// standing, and it does not take the other models down with it.
				standing.NoteFailure(false);
				Loc::Warn("$SPEECHBROKERVOICE_LOG_ANSWER_FAILED", a_collector->UtteranceId(),
					reading.modelId, reading.failed);
			} else {
				Loc::Debug("$SPEECHBROKERVOICE_LOG_ANSWER_CANCELLED", a_collector->UtteranceId(),
					reading.modelId);
			}
		}

		// (3) THE TERMINAL FALL, MEASURED ON THE SNAPSHOT AND NEVER ON A LIVE TURN:
		// by the time this runs the turn belongs to another thread and has moved
		// on. Punctuation is a model's and is a property of a language; tone is
		// ours and is not (engine/engine.py:144-151).
		std::optional<float> fall;
		const auto* base = _arbiter->Base(answers);
		if (base != nullptr && !base->fragments.empty() && subject.audio) {
			const auto& tail = base->fragments.back();
			const auto  total = subject.audio->size();
			const auto  at = std::min<std::size_t>(total,
				static_cast<std::size_t>(MsToSamples(tail.startMs)));
			const auto  to = std::min<std::size_t>(total,
				static_cast<std::size_t>(MsToSamples(tail.endMs)));

			// Shorter than a tenth of a second and there is nothing to measure.
			if (to > at && to - at > kTargetSampleRate / 10u) {
				fall = TerminalFall(a_scratch,
					std::span<const Sample>(subject.audio->data() + at, to - at), subject.pitch);
			}
		}

		// (4) THE SERIAL GUARD AND THE RECONCILIATION, under the turn's own lock
		// and in one call, because a second worker would otherwise slip between
		// them (Arbiter.h, TurnLedger::Reconcile).
		std::vector<Slice> slices;
		const auto ledger = Ledger(a_collector->TurnId());
		if (ledger && _ears) {
			slices = ledger->Reconcile(*a_collector, *_arbiter, _ears->Judge(), fall);
		}

		// (5) OUT. Each publish is on its own: a bridge that threw on one piece
		// must not swallow the rest of the turn.
		for (const auto& slice : slices) {
			Loc::Info("$SPEECHBROKERVOICE_LOG_SLICE", a_collector->TurnId(), slice.id,
				slice.startMs, slice.endMs, slice.Text(), slice.complete);

			if (!_publish) {
				continue;
			}
			try {
				_publish(slice);
			} catch (const std::exception& e) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_PUBLISH_THREW", slice.id, e.what());
			} catch (...) {
				Loc::Error("$SPEECHBROKERVOICE_LOG_PUBLISH_THREW", slice.id,
					Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
			}
		}

		// (6) The pass is finished with, and with it possibly the turn. Retirement
		// is membership in this map, so the erase IS the retirement.
		bool turnStillOpen = false;
		{
			std::lock_guard<std::mutex> lock(_collectors);
			_open.erase(a_collector->UtteranceId());
			for (const auto& entry : _open) {
				if (entry.second->TurnId() == a_collector->TurnId()) {
					turnStillOpen = true;
					break;
				}
			}
		}

		if (a_collector->Final() && !turnStillOpen) {
			std::shared_ptr<TurnLedger> finished;
			{
				std::lock_guard<std::mutex> lock(_ledgers);
				const auto at = _byTurn.find(a_collector->TurnId());
				if (at != _byTurn.end()) {
					finished = at->second;
					_byTurn.erase(at);
				}
			}

			if (finished && !finished->Emitted()) {
				// A turn that ended without a single piece of speech coming out of
				// it is a silent loss, and a silent loss is worse than a noisy
				// failure because nothing in any log says it happened.
				Loc::Warn("$SPEECHBROKERVOICE_LOG_TURN_SILENT", a_collector->TurnId());
			}
		}
	}

	std::shared_ptr<TurnLedger> Host::Ledger(std::int64_t a_turnId)
	{
		std::lock_guard<std::mutex> lock(_ledgers);

		const auto at = _byTurn.find(a_turnId);
		if (at != _byTurn.end()) {
			return at->second;
		}

		// THE SLICE COUNTER IS THE SESSION'S, handed in by reference: slice ids are
		// monotone for the SESSION and not per turn, because the bridge reads
		// `refines` and `supersedes` against a map of them.
		auto ledger = std::make_shared<TurnLedger>(a_turnId, _earsSettings.turn,
			_earsSettings.segments, _nextSliceId);
		_byTurn.emplace(a_turnId, ledger);
		return ledger;
	}

	std::uint32_t Host::MaxRequestSamples() const
	{
		// A CEILING THAT WAS ANNOUNCED AND A CEILING THAT WAS ENFORCED MUST NOT BE
		// TWO FIELDS THAT CAN DRIFT APART, so this is the ears' own number whenever
		// the ears exist. They do not at Register - that runs at plugin load, long
		// before Begin - so the same arithmetic stands in for the window in between
		// (turn/Ears.h, MaxRequestSamples; turn/SpeechTurn.h, CutRules::maxSamples).
		if (_ears) {
			return _ears->MaxRequestSamples();
		}
		return static_cast<std::uint32_t>(SecondsToSamples(_earsSettings.vad.maxUttSec));
	}

	// --- the five entry points -----------------------------------------------

	std::int32_t SPEECHBROKERVOICE_CALL Host::OnRegister(const SpeechBrokerVoiceModelInfo* a_info,
		const SpeechBrokerVoiceModel* a_model, SpeechBrokerVoiceSession* a_session)
	{
		RegisterContext context;
		context.info = a_info;
		context.model = a_model;
		context.session = a_session;

		try {
			Fault fault;
			if (!GuardedHostCall(&RegisterBody, &context, &fault)) {
				// No handle exists yet, so there is nobody to eject. REFUSED is the
				// status the contract names for an adapter-side failure, and it is
				// never counted against a model.
				Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_FAULTED", "?", kEntryRegister,
					FaultName(fault.code), fault.code, fault.address);
				return SPEECHBROKERVOICE_REFUSED;
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryRegister, e.what());
			return SPEECHBROKERVOICE_REFUSED;
		} catch (...) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryRegister,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
			return SPEECHBROKERVOICE_REFUSED;
		}

		return context.status;
	}

	void SPEECHBROKERVOICE_CALL Host::OnUnregister(SpeechBrokerVoiceHandle a_handle)
	{
		HandleContext context;
		context.handle = a_handle;

		try {
			Fault fault;
			if (!GuardedHostCall(&UnregisterBody, &context, &fault)) {
				EjectForFault(a_handle, kEntryUnregister, fault);
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryUnregister, e.what());
		} catch (...) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryUnregister,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
		}
	}

	std::int32_t SPEECHBROKERVOICE_CALL Host::OnComplete(SpeechBrokerVoiceHandle a_handle,
		const SpeechBrokerVoiceAnswer* a_answer)
	{
		CompleteContext context;
		context.handle = a_handle;
		context.answer = a_answer;

		try {
			Fault fault;
			if (!GuardedHostCall(&CompleteBody, &context, &fault)) {
				// THE LIKELIER FAULT OF THE TWO: this ran on the model's thread over
				// pointers the shim supplied, and a stale char* out of third-party
				// code is the most probable in-process defect in the whole design.
				EjectForFault(a_handle, kEntryComplete, fault);
				return SPEECHBROKERVOICE_STALE;
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryComplete, e.what());
			return SPEECHBROKERVOICE_REFUSED;
		} catch (...) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryComplete,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
			return SPEECHBROKERVOICE_REFUSED;
		}

		return context.status;
	}

	void SPEECHBROKERVOICE_CALL Host::OnReady(SpeechBrokerVoiceHandle a_handle,
		std::int32_t a_ready, const char* a_reason)
	{
		ReadyContext context;
		context.handle = a_handle;
		context.ready = a_ready;
		context.reason = a_reason;

		try {
			Fault fault;
			if (!GuardedHostCall(&ReadyBody, &context, &fault)) {
				EjectForFault(a_handle, kEntryReady, fault);
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryReady, e.what());
		} catch (...) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryReady,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
		}
	}

	void SPEECHBROKERVOICE_CALL Host::OnLog(SpeechBrokerVoiceHandle a_handle, std::int32_t a_level,
		const char* a_key, const char* const* a_args, std::int32_t a_argCount)
	{
		LogContext context;
		context.handle = a_handle;
		context.level = a_level;
		context.key = a_key;
		context.args = a_args;
		context.argCount = a_argCount;

		try {
			Fault fault;
			if (!GuardedHostCall(&LogBody, &context, &fault)) {
				EjectForFault(a_handle, kEntryLog, fault);
			}
		} catch (const std::exception& e) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryLog, e.what());
		} catch (...) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_HOST_THREW", kEntryLog,
				Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
		}
	}
}
