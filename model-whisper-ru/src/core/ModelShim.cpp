#include "ModelShim.h"

#include "Sha256.h"

#include <algorithm>
#include <cstring>

namespace WhisperRu
{
	namespace
	{
		// The child's own vocabulary of endings, so that a number in the log has
		// a meaning attached to it. 127 is the measured one and the reason this
		// whole arrangement exists; the rest are ours and are documented in
		// docs/child-protocol.md.
		constexpr std::uint32_t kExitBadArguments = 2;
		constexpr std::uint32_t kExitNoBackend = 3;
		constexpr std::uint32_t kExitNoWeights = 4;

		// The measured one. A missing sub-library of a GPU runtime ends a
		// process with this and NOTHING else - no exception, no crash log. In
		// SkyrimVR.exe that was the game gone with nothing to read.
		constexpr std::uint32_t kExitMissingDependency = 127;

		std::int64_t Millis(std::chrono::steady_clock::time_point a_from)
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - a_from).count();
		}
	}

	std::vector<ModelShim*>& Models()
	{
		static std::vector<ModelShim*> models;
		return models;
	}

	ModelShim::ModelShim(ModelSettings a_settings) :
		m_settings(std::move(a_settings))
	{}

	// --------------------------------------------------------------- thunks
	std::int32_t SPEECHBROKERVOICE_CALL ModelShim::StartThunk(void* a_user)
	{
		return static_cast<ModelShim*>(a_user)->Start();
	}

	void SPEECHBROKERVOICE_CALL ModelShim::StopThunk(void* a_user)
	{
		static_cast<ModelShim*>(a_user)->Stop();
	}

	std::int32_t SPEECHBROKERVOICE_CALL ModelShim::SubmitThunk(void* a_user,
		const SpeechBrokerVoiceRequest* a_request)
	{
		return static_cast<ModelShim*>(a_user)->Submit(a_request);
	}

	void SPEECHBROKERVOICE_CALL ModelShim::CancelThunk(void* a_user, std::int64_t a_utteranceId)
	{
		static_cast<ModelShim*>(a_user)->Cancel(a_utteranceId);
	}

	void SPEECHBROKERVOICE_CALL ModelShim::VocabularyThunk(void* a_user,
		const char* const* a_phrases, std::int32_t a_count)
	{
		static_cast<ModelShim*>(a_user)->SetVocabulary(a_phrases, a_count);
	}

	// -------------------------------------------------------------- the log
	void ModelShim::Say(std::int32_t a_level, const std::string& a_key,
		const std::vector<std::string>& a_args)
	{
		Log::Say(a_level, a_key, a_args);
		if (!m_host || !m_host->Log || !m_registered) {
			return;
		}
		// The adapter writes our id, then the key VERBATIM, then the arguments
		// in order, tab separated. It does not substitute them into anything -
		// it does not have our table - so the numbered placeholders are for
		// whoever renders that table later.
		// The adapter drops a call whose argument count is out of range, with a
		// line of its own, so the clip happens here where the line survives.
		constexpr std::size_t kCeiling = SPEECHBROKERVOICE_MAX_LOG_ARGS;
		const std::size_t     take = a_args.size() > kCeiling ? kCeiling : a_args.size();
		std::vector<const char*> argv;
		argv.reserve(take);
		for (std::size_t i = 0; i < take; ++i) {
			argv.push_back(a_args[i].c_str());
		}
		m_host->Log(m_session.handle, a_level, a_key.c_str(), argv.empty() ? nullptr : argv.data(),
			static_cast<std::int32_t>(take));
	}

	// ------------------------------------------------------------- register
	bool ModelShim::Register(const SpeechBrokerVoiceHost* a_host)
	{
		if (!a_host) {
			return false;
		}
		// Read in the order the contract prescribes: the pointer, then
		// structBytes against a multiple of 8 and the version-1 floor, then
		// abiVersion, and only then anything later. An adapter one version newer
		// is accepted by design; an adapter below the minimum THIS code needs is
		// refused, and the minimum is 1 because we read nothing a later version
		// added.
		if (a_host->structBytes % 8 != 0 || a_host->structBytes < SPEECHBROKERVOICE_HOST_BYTES_V1) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_HOST_MALFORMED", a_host->structBytes);
			return false;
		}
		if (a_host->abiVersion < kMinimumHostVersion) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_HOST_TOO_OLD", a_host->abiVersion, kMinimumHostVersion);
			return false;
		}
		if (!a_host->Register) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_HOST_MALFORMED", a_host->structBytes);
			return false;
		}
		m_host = a_host;

		// The paths are resolved and contained HERE and not at Start, so that a
		// mod laid out wrongly says so at load rather than at the first word the
		// player speaks.
		if (!IsContained(m_settings.source.parent_path(), m_settings.weightsRelative, m_weights)) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_PATH_OUTSIDE", m_settings.weightsRelative,
				m_settings.source);
			return false;
		}
		if (!IsContained(m_settings.source.parent_path(), m_settings.child.exec, m_exec)) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_PATH_OUTSIDE", m_settings.child.exec, m_settings.source);
			return false;
		}

		SpeechBrokerVoiceModelInfo info{};
		info.structBytes = sizeof(info);
		info.abiVersion = SPEECHBROKERVOICE_MODEL_ABI_VERSION;
		info.id = m_settings.id.c_str();
		info.name = m_settings.name.c_str();
		info.language = m_settings.language.c_str();
		info.provides = m_settings.provides.c_str();

		// Declared, unverifiable, logged by the adapter, and gated there: a kind
		// the player has forbidden is refused before Start is ever called, so a
		// forbidden model never starts a process. Ours raises a child and says
		// so.
		info.kind = SPEECHBROKERVOICE_KIND_CHILD;

		info.budgetMs = static_cast<std::uint32_t>(std::max(0, m_settings.budgetMs));
		info.startupMs = static_cast<std::uint32_t>(std::max(0, m_settings.startupMs));
		info.stopMs = static_cast<std::uint32_t>(std::max(0, m_settings.stopMs));
		info.finalOnly = m_settings.finalOnly ? 1 : 0;
		info.maxInFlight = static_cast<std::uint32_t>(std::max(1, m_settings.maxInFlight));
		info.declaredClass = (m_settings.declaredClass == "fast")
			? SPEECHBROKERVOICE_CLASS_FAST
			: SPEECHBROKERVOICE_CLASS_ACCURATE;
		info.reserved0 = 0;

		m_table = {};
		m_table.structBytes = sizeof(m_table);
		m_table.reserved0 = 0;
		m_table.user = this;
		m_table.Start = &ModelShim::StartThunk;
		m_table.Stop = &ModelShim::StopThunk;
		m_table.SetVocabulary = &ModelShim::VocabularyThunk;
		m_table.Submit = &ModelShim::SubmitThunk;
		m_table.Cancel = &ModelShim::CancelThunk;

		// The out-parameter protocol inverts the meaning of structBytes: WE
		// allocate this, so it is the size of OUR buffer, set before the call,
		// and the adapter writes at most that many bytes. It never writes its
		// own sizeof.
		m_session = {};
		m_session.structBytes = sizeof(m_session);

		const auto status = m_host->Register(&info, &m_table, &m_session);
		if (status != SPEECHBROKERVOICE_OK) {
			// Nothing was written into the session on any status but OK, so
			// there is no handle to log and none is logged.
			Tell(Log::kError, "$SBWHISPERRU_LOG_REGISTER_REFUSED", m_settings.id, status);
			m_host = nullptr;
			return false;
		}
		m_registered = true;
		Tell(Log::kInfo, "$SBWHISPERRU_LOG_REGISTERED", m_settings.id, m_session.handle,
			m_session.abiVersion, m_session.maxRequestSamples);
		return true;
	}

	// ---------------------------------------------------------------- start
	bool ModelShim::VerifyWeights(std::string& a_failedFile, std::string& a_reason)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(m_weights, ec)) {
			a_failedFile = m_weights.string();
			a_reason = "missing";
			return false;
		}
		const auto sums = VerifySums(m_weights);
		if (!sums.ok) {
			a_failedFile = sums.failedFile;
			a_reason = sums.failedReason;
			return false;
		}
		if (sums.checked == 0) {
			// No sums file at all. Five hundred flipped bytes inside model.bin
			// raise no error anywhere and produce a model that answers rubbish,
			// and nothing above this line could tell that from a hard accent. So
			// a shipped mod refuses, and an author building their own turns
			// requireSums off until they have generated them.
			if (m_settings.requireSums) {
				a_failedFile = (m_weights / "SHA256SUMS").string();
				a_reason = "missing";
				return false;
			}
			Tell(Log::kWarn, "$SBWHISPERRU_LOG_WEIGHTS_UNVERIFIED", m_weights);
			return true;
		}
		Tell(Log::kInfo, "$SBWHISPERRU_LOG_WEIGHTS_VERIFIED", m_weights, sums.checked);
		return true;
	}

	bool ModelShim::Raise()
	{
		ChildProcess::Options options;
		options.exe = m_exec;
		options.workingDir = m_exec.parent_path();
		options.args = {
			"--model-id", m_settings.id,
			"--weights", m_weights.string(),
			"--library", (m_settings.source.parent_path() / m_settings.child.library).string(),
			"--device", m_settings.child.device,
			"--beam-size", std::to_string(m_settings.child.beamSize),
			"--threads", std::to_string(m_settings.child.threads),
			"--language", m_settings.language,
			"--max-samples", std::to_string(m_session.maxRequestSamples)
		};
		for (const auto& one : m_settings.child.preload) {
			options.args.push_back("--preload");
			options.args.push_back((m_settings.source.parent_path() / one).string());
		}

		std::string why;
		if (!m_child.Start(options, why)) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_CHILD_NOT_STARTED", m_exec, why);
			return false;
		}
		Tell(Log::kInfo, "$SBWHISPERRU_LOG_CHILD_STARTED", m_exec, m_settings.child.device);
		m_raised = true;
		m_reader = std::thread([this]() { ReaderLoop(); });
		m_writer = std::thread([this]() { WriterLoop(); });
		return true;
	}

	std::int32_t ModelShim::Start()
	{
		std::unique_lock startGuard(m_startLock);
		if (m_ready) {
			return SPEECHBROKERVOICE_OK;
		}

		// What we will actually be given, for the life of the session. A format
		// we do not know is REFUSED here and never converted: the adapter owns
		// the microphone and every conversion with it, and a shim that resampled
		// would be guessing at what it was given.
		if (m_session.format.sampleFormat != SPEECHBROKERVOICE_FMT_FLOAT32 ||
			m_session.format.channels != 1) {
			Tell(Log::kError, "$SBWHISPERRU_LOG_FORMAT_UNKNOWN",
				m_session.format.sampleFormat, m_session.format.channels,
				m_session.format.sampleRate);
			return SPEECHBROKERVOICE_REFUSED;
		}

		if (!m_raised) {
			std::string failedFile;
			std::string reason;
			if (!VerifyWeights(failedFile, reason)) {
				// Permanent, so REFUSED and not RETRY: waiting will not mend a
				// hash. The adapter takes this model out for the session and
				// says so loudly, and the others carry on.
				Tell(Log::kError, "$SBWHISPERRU_LOG_WEIGHTS_BAD", failedFile, reason);
				return SPEECHBROKERVOICE_REFUSED;
			}
			std::error_code ec;
			if (!std::filesystem::exists(m_exec, ec)) {
				Tell(Log::kError, "$SBWHISPERRU_LOG_CHILD_MISSING", m_exec);
				return SPEECHBROKERVOICE_REFUSED;
			}
			if (!Raise()) {
				return SPEECHBROKERVOICE_REFUSED;
			}
		}

		// Wait for the child to say it is up. A Start that never returns is not
		// timed out by the adapter - it waits on our own dispatch thread and
		// nothing else waits on that - but a Start that comes back with RETRY
		// costs nothing and is honest about a model still loading gigabytes off
		// a cold disk. Five attempts, startupMs apart, is the adapter's own
		// default.
		const auto wait = std::chrono::milliseconds(std::max(1, m_settings.startupMs));
		m_started.wait_for(startGuard, wait, [this]() { return m_hello || !m_child.Alive(); });

		if (m_hello) {
			m_ready = true;
			Tell(Log::kInfo, "$SBWHISPERRU_LOG_READY", m_settings.id);
			return SPEECHBROKERVOICE_OK;
		}
		if (!m_child.Alive()) {
			std::uint32_t code = 0;
			m_child.ExitCode(code);
			Tell(Log::kError, "$SBWHISPERRU_LOG_CHILD_DIED_AT_START", m_settings.id, code,
				code == kExitMissingDependency ? "missing-dependency"
				: code == kExitNoBackend       ? "no-backend"
				: code == kExitNoWeights       ? "no-weights"
				: code == kExitBadArguments    ? "bad-arguments"
				                               : "unknown");
			return SPEECHBROKERVOICE_REFUSED;
		}
		Tell(Log::kInfo, "$SBWHISPERRU_LOG_STILL_STARTING", m_settings.id, m_settings.startupMs);
		return SPEECHBROKERVOICE_RETRY;
	}

	// ----------------------------------------------------------------- stop
	void ModelShim::Stop()
	{
		// Quick, and the slow part is tied to the process instead. Two moments
		// bring us here and one of them is the player wanting out of the game; a
		// minute of teardown there is the difference between a build that can be
		// edited afterwards and one where MO2 still believes the game is
		// running.
		if (m_stopping.exchange(true)) {
			return;
		}
		Tell(Log::kInfo, "$SBWHISPERRU_LOG_STOPPING", m_settings.id);

		m_child.WriteFrame(Wire::EncodeBye());
		m_child.CloseInput();  // end of file: the child's own orderly ending
		m_wake.notify_all();

		// Nothing is joined here. The contract does not oblige it, stopMs is two
		// seconds, and a join against a child that is mid-inference would spend
		// all of it. The abrupt ending goes to a thread of its own so that Stop
		// returns in microseconds, and the Job Object would do the same work at
		// process exit if this never ran at all.
		std::thread([this]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(
				std::max(1, m_settings.stopMs) / 2));
			if (m_child.Alive()) {
				m_child.Terminate();
			}
		}).detach();
	}

	// --------------------------------------------------------------- submit
	std::int32_t ModelShim::Submit(const SpeechBrokerVoiceRequest* a_request)
	{
		if (!a_request || a_request->structBytes % 8 != 0 ||
			a_request->structBytes < SPEECHBROKERVOICE_REQUEST_BYTES_V1) {
			return SPEECHBROKERVOICE_REFUSED;
		}
		if (!m_ready || m_stopping) {
			return SPEECHBROKERVOICE_NOT_READY;
		}
		if (a_request->sampleCount == 0 || !a_request->samples) {
			return SPEECHBROKERVOICE_REFUSED;
		}

		Job job;
		job.utteranceId = a_request->utteranceId;
		job.turnId = a_request->turnId;
		job.serial = a_request->serial;
		job.final = a_request->final;
		job.lostSamples = a_request->lostSamples;
		job.deadlineMs = a_request->deadlineMs;

		// THE COPY IS THE WHOLE OF Submit'S WORK, and it is not optional: the
		// pointer is borrowed for the length of this call and every registered
		// model gets the same one. 640 kB on a ten-second final pass, a memcpy,
		// well inside the few milliseconds this call is allowed.
		job.samples.assign(a_request->samples, a_request->samples + a_request->sampleCount);

		{
			std::lock_guard guard(m_lock);
			// maxInFlight is read by the adapter against the DEBT, so a shim
			// that declares its true depth should never have to say BUSY at
			// all. It is still answered rather than assumed: the alternative is
			// an unbounded queue, and an unbounded queue turns a slow child into
			// a growing pile of buffers nobody is waiting for any more.
			if (m_debts.size() >= static_cast<std::size_t>(std::max(1, m_settings.maxInFlight))) {
				return SPEECHBROKERVOICE_BUSY;
			}
			Debt debt;
			debt.serial = a_request->serial;
			debt.acceptedAt = std::chrono::steady_clock::now();
			m_debts.emplace(job.utteranceId, debt);
			m_queue.push_back(std::move(job));
		}
		m_wake.notify_one();
		// From here we owe exactly one Complete for this utteranceId - not two,
		// not none - discharged by any status, or by the pass closing at its
		// deadline without us.
		return SPEECHBROKERVOICE_OK;
	}

	void ModelShim::Cancel(std::int64_t a_utteranceId)
	{
		bool sent = false;
		{
			std::lock_guard guard(m_lock);
			const auto found = m_debts.find(a_utteranceId);
			if (found == m_debts.end()) {
				return;
			}
			found->second.cancelled = true;
			sent = found->second.sent;
		}
		if (sent) {
			// Already down the pipe. The debt stays ours and the reply will pay
			// it; the frame is advisory and the reference child ignores it. A
			// child that can abort an inference mid-way answers it with status
			// cancelled, which pays the same debt sooner.
			m_child.WriteFrame(Wire::EncodeCancel(a_utteranceId));
		}
		// Not yet sent: the writer will find the flag, drop the job and pay with
		// CANCELLED. Doing it here would race the writer for the same debt.
		m_wake.notify_one();
	}

	void ModelShim::SetVocabulary(const char* const* a_phrases, std::int32_t a_count)
	{
		std::vector<std::string> phrases;
		if (a_phrases && a_count > 0) {
			const auto take = std::min({ a_count,
				static_cast<std::int32_t>(SPEECHBROKERVOICE_MAX_VOCABULARY),
				std::max(0, m_settings.child.promptPhrases) });
			for (std::int32_t i = 0; i < take; ++i) {
				if (a_phrases[i]) {
					phrases.emplace_back(a_phrases[i]);
				}
			}
		}
		// Clipping is expressly allowed and is not a failure: a prompt competes
		// for a window that also has to hold the answer, and a long one makes
		// recognition worse. The order is the adapter's merge order and means
		// nothing, so the first N is as good a choice as any.
		Tell(Log::kInfo, "$SBWHISPERRU_LOG_VOCABULARY", static_cast<std::int32_t>(phrases.size()), a_count);

		Job job;
		job.vocabulary = true;
		job.phrases = std::move(phrases);
		{
			std::lock_guard guard(m_lock);
			m_vocabulary = job.phrases;
			m_queue.push_back(std::move(job));
		}
		m_wake.notify_one();
	}

	// -------------------------------------------------------------- threads
	void ModelShim::WriterLoop()
	{
		for (;;) {
			Job job;
			{
				std::unique_lock guard(m_lock);
				m_wake.wait(guard, [this]() { return !m_queue.empty() || m_stopping; });
				if (m_stopping && m_queue.empty()) {
					return;
				}
				if (m_queue.empty()) {
					continue;
				}
				job = std::move(m_queue.front());
				m_queue.pop_front();
				if (!job.vocabulary) {
					const auto found = m_debts.find(job.utteranceId);
					if (found != m_debts.end()) {
						found->second.sent = true;
					}
				}
			}

			if (job.vocabulary) {
				m_child.WriteFrame(Wire::EncodeVocabulary(job.phrases));
				continue;
			}

			Debt debt;
			{
				std::lock_guard guard(m_lock);
				const auto found = m_debts.find(job.utteranceId);
				if (found != m_debts.end() && found->second.cancelled) {
					// Cancelled before it ever went down the pipe. The cheapest
					// cancelled pass is the one never sent.
					debt = found->second;
					m_debts.erase(found);
					AnswerFailure(job.utteranceId, debt, SPEECHBROKERVOICE_CANCELLED, {});
					continue;
				}
			}

			Wire::Request request;
			request.utteranceId = job.utteranceId;
			request.turnId = job.turnId;
			request.serial = job.serial;
			request.final = job.final;
			request.lostSamples = job.lostSamples;
			request.deadlineMs = job.deadlineMs;
			request.samples = std::move(job.samples);
			if (!m_child.WriteFrame(Wire::EncodeRequest(request))) {
				// The pipe has gone, which means the child has. The reader is
				// about to see the same thing; it is the one that pays every
				// outstanding debt, so that two threads cannot both pay one.
				return;
			}
		}
	}

	void ModelShim::ReaderLoop()
	{
		Wire::Header header{};
		std::vector<std::uint8_t> body;
		for (;;) {
			if (!m_child.ReadFrame(header, body)) {
				break;
			}
			switch (static_cast<Wire::Type>(header.type)) {
			case Wire::Type::Hello:
				{
					Wire::Hello hello;
					if (Wire::DecodeHello(body.data(), body.size(), hello)) {
						Tell(Log::kInfo, "$SBWHISPERRU_LOG_CHILD_HELLO", hello.backend, hello.modelId,
							hello.maxSamples);
					}
					{
						std::lock_guard guard(m_startLock);
						m_hello = true;
					}
					m_started.notify_all();
					// Anything the adapter told us before the child was up goes
					// down now, in order, so a vocabulary set during start-up is
					// not silently lost.
					{
						std::lock_guard guard(m_lock);
						if (!m_vocabulary.empty()) {
							m_child.WriteFrame(Wire::EncodeVocabulary(m_vocabulary));
						}
					}
					break;
				}
			case Wire::Type::Log:
				{
					Wire::LogLine line;
					if (Wire::DecodeLog(body.data(), body.size(), line)) {
						// The child renders nothing. It emits a key of this
						// module's own table and its arguments, and they travel
						// unchanged into the adapter's log - which is what
						// Host::Log is for and why a model mod needs no edit to
						// the adapter to say something new.
						Say(line.level, line.key, line.args);
					}
					break;
				}
			case Wire::Type::Reply:
				{
					Wire::Reply reply;
					if (!Wire::DecodeReply(body.data(), body.size(), reply)) {
						Tell(Log::kWarn, "$SBWHISPERRU_LOG_CHILD_FRAME_BROKEN",
							static_cast<std::int32_t>(header.type));
						break;
					}
					Debt debt;
					if (!TakeDebt(reply.utteranceId, debt)) {
						// Nobody is waiting for it. Not an error: the pass may
						// have closed at its deadline while the child was still
						// working, which is the ordinary end of a late answer.
						Tell(Log::kDebug, "$SBWHISPERRU_LOG_REPLY_UNOWED", reply.utteranceId);
						break;
					}
					Answer(reply.utteranceId, debt, reply);
					break;
				}
			default:
				Tell(Log::kWarn, "$SBWHISPERRU_LOG_CHILD_FRAME_UNKNOWN",
					static_cast<std::int32_t>(header.type));
				break;
			}
		}
		OnChildGone();
	}

	void ModelShim::OnChildGone()
	{
		{
			std::lock_guard guard(m_startLock);
			m_ready = false;
		}
		std::uint32_t code = 0;
		const bool ended = m_child.ExitCode(code);

		if (!m_stopping) {
			// THE NUMBER THIS DESIGN EXISTS FOR. 127 is the measured real case:
			// a GPU runtime that cannot reach a sub-library ends its process
			// with it, past every exception handler and past the player's crash
			// logger. Inside SkyrimVR.exe that was the game gone and no log at
			// all; here it is a line and an ejection.
			Tell(Log::kError, "$SBWHISPERRU_LOG_CHILD_GONE", m_settings.id,
				ended ? static_cast<std::int32_t>(code) : -1,
				code == kExitMissingDependency ? "missing-dependency"
				: code == kExitNoBackend       ? "no-backend"
				: code == kExitNoWeights       ? "no-weights"
				: code == kExitBadArguments    ? "bad-arguments"
				                               : "unknown");
		}

		// Every debt outstanding is paid, once each, as a failure. The adapter
		// weighs a failure against this model's standing and does not take the
		// other models down with it - which is the whole of the bargain.
		for (;;) {
			std::int64_t utteranceId = 0;
			{
				std::lock_guard guard(m_lock);
				if (m_debts.empty()) {
					break;
				}
				utteranceId = m_debts.begin()->first;
			}
			Debt debt;
			if (TakeDebt(utteranceId, debt)) {
				AnswerFailure(utteranceId, debt, SPEECHBROKERVOICE_FAILED, "child process ended");
			}
		}

		// Going not-ready removes us from every pass still open, as a REMOVAL
		// and not a timeout, so it costs no standing. There is no
		// re-registration in this contract and the child is not raised again:
		// the commonest cause of this line is a runtime that is not installed,
		// which a relaunch loop would hide behind a wall of identical failures.
		if (m_host && m_host->Ready && m_registered) {
			m_host->Ready(m_session.handle, 0, "child process ended");
		}
		m_wake.notify_all();
	}

	// -------------------------------------------------------------- answers
	bool ModelShim::TakeDebt(std::int64_t a_utteranceId, Debt& a_out)
	{
		std::lock_guard guard(m_lock);
		const auto found = m_debts.find(a_utteranceId);
		if (found == m_debts.end()) {
			return false;
		}
		a_out = found->second;
		m_debts.erase(found);
		return true;
	}

	void ModelShim::AnswerFailure(std::int64_t a_utteranceId, const Debt& a_debt,
		std::int32_t a_status, const std::string& a_failed)
	{
		if (!m_host || !m_host->Complete || !m_registered) {
			return;
		}
		SpeechBrokerVoiceAnswer answer{};
		answer.structBytes = sizeof(answer);
		answer.abiVersion = m_session.abiVersion;
		answer.utteranceId = a_utteranceId;
		answer.serial = a_debt.serial;
		answer.status = a_status;
		answer.latencyMs = static_cast<std::int32_t>(Millis(a_debt.acceptedAt));
		answer.fragmentStride = sizeof(SpeechBrokerVoiceFragment);
		answer.fragments = nullptr;
		answer.fragmentCount = 0;
		answer.lostSamples = 0;
		// A failure is an answer too, and the text is a STRING rather than a
		// code because the adapter writes it into its log for a person to read.
		answer.failed = a_failed.empty() ? nullptr : a_failed.c_str();
		m_host->Complete(m_session.handle, &answer);
	}

	void ModelShim::Answer(std::int64_t a_utteranceId, const Debt& a_debt, const Wire::Reply& a_reply)
	{
		if (!m_host || !m_host->Complete || !m_registered) {
			return;
		}
		if (a_reply.status != Wire::kStatusOk) {
			AnswerFailure(a_utteranceId, a_debt, a_reply.status, a_reply.failed);
			return;
		}

		// The order and the bounds are part of the contract, and an answer that
		// breaks either is refused WHOLE and counted against us. The child is
		// ours and ought to be ordered already; this is the last place the
		// defect can be caught on our own side of the line, so it is caught
		// here rather than trusted.
		std::vector<Wire::Fragment> pieces = a_reply.fragments;
		std::stable_sort(pieces.begin(), pieces.end(),
			[](const Wire::Fragment& a_left, const Wire::Fragment& a_right) {
				return a_left.startMs < a_right.startMs;
			});

		std::vector<SpeechBrokerVoiceFragment> fragments;
		std::vector<std::string>               texts;
		fragments.reserve(pieces.size());
		texts.reserve(pieces.size());

		std::int32_t previousEnd = 0;
		std::int32_t dropped = 0;
		for (auto& piece : pieces) {
			if (piece.endMs < piece.startMs) {
				++dropped;
				continue;
			}
			if (piece.startMs < previousEnd) {
				// Overlapping pieces have no defined behaviour above this line:
				// the adapter matches by TIME. Closing the gap is a smaller lie
				// than dropping a piece of speech.
				piece.startMs = previousEnd;
				if (piece.endMs < piece.startMs) {
					++dropped;
					continue;
				}
			}
			previousEnd = piece.endMs;
			texts.push_back(std::move(piece.text));

			SpeechBrokerVoiceFragment fragment{};
			fragment.startMs = piece.startMs;
			fragment.endMs = piece.endMs;
			fragment.text = texts.back().c_str();
			fragment.score = piece.score;
			fragment.endsSentence = piece.endsSentence;
			// THE SENTINELS ARE PASSED THROUGH AND NEVER INVENTED. -1 in
			// lastWordProb says "my times do not come from an alignment to the
			// audio"; a zero there would pass the adapter's test for word
			// timings and win the lane that decides the spans for every other
			// model, with boundaries that move between passes.
			fragment.lastWordProb = piece.lastWordProb;
			fragment.noSpeechProb = piece.noSpeechProb;
			fragment.medianGapMs = piece.medianGapMs;
			fragment.words = piece.words;
			fragments.push_back(fragment);
		}
		if (dropped != 0) {
			Tell(Log::kWarn, "$SBWHISPERRU_LOG_FRAGMENTS_DROPPED", dropped, a_utteranceId);
		}

		SpeechBrokerVoiceAnswer answer{};
		answer.structBytes = sizeof(answer);
		answer.abiVersion = m_session.abiVersion;
		answer.utteranceId = a_utteranceId;
		answer.serial = a_debt.serial;
		answer.status = SPEECHBROKERVOICE_OK;
		// Measured by US, wall clock, from Submit to here, so that what the
		// adapter learns is what this model IS rather than what it declared. The
		// child's own number is its inference alone and is not this.
		answer.latencyMs = static_cast<std::int32_t>(Millis(a_debt.acceptedAt));
		// An equality the adapter checks before it indexes the array even once,
		// and a zero here would make every element alias element 0.
		answer.fragmentStride = sizeof(SpeechBrokerVoiceFragment);
		answer.fragments = fragments.empty() ? nullptr : fragments.data();
		answer.fragmentCount = static_cast<std::int32_t>(fragments.size());
		answer.lostSamples = a_reply.lostSamples;
		answer.failed = nullptr;

		const auto status = m_host->Complete(m_session.handle, &answer);
		if (status == SPEECHBROKERVOICE_MALFORMED) {
			// Counted against us, and the adapter's log names the field. Our own
			// log says which utterance, so the two can be put side by side.
			Tell(Log::kError, "$SBWHISPERRU_LOG_ANSWER_MALFORMED", a_utteranceId,
				answer.fragmentCount);
		} else if (status == SPEECHBROKERVOICE_STALE) {
			Tell(Log::kDebug, "$SBWHISPERRU_LOG_ANSWER_STALE", a_utteranceId, answer.latencyMs);
		}
		// Everything it points at is borrowed for the length of that call and
		// the adapter has copied what it keeps, so fragments and texts may die
		// here.
	}
}
