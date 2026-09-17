# Speech Broker adapter - the dispatch half: implementation notes shared by every implementer

Worktree: `D:\Skyrim VR Modding\Claude Skyrim VR\dev\wt-speechbroker\speechbroker` (branch `speechbroker`).
Never run a git command that changes state (no add/commit/stash/checkout/push). The integrator commits.

Adapter: `adapter-voice\`. The headers under `adapter-voice\src\models\*.h` are the DESIGN and are
authoritative; the contract is `adapter-voice\contract\speechbroker-voice-model.h`; the obligations of
the adapter are `adapter-voice\docs\model-host.md`. The reference python being deleted is
`D:\Skyrim VR Modding\Claude Skyrim VR\dev\voice\engine\*.py` (read-only). The half that owns the
microphone is `adapter-voice\src\audio\` and `adapter-voice\src\turn\` - done, do not edit.

Style: tabs; `namespace Voice::Models`; comments in English and they carry the reasoning (say WHY, as
the headers do); MSVC C++23 at `/W4 /WX /permissive- /EHsc` - warnings are errors; no exception may
cross the contract; every log line goes through `Voice::Loc` (`adapter-voice\src\Loc.h`:
`Loc::Info("$SPEECHBROKERVOICE_LOG_...", args...)`, placeholders `{0} {1}` numbered).

NEW LOCALISATION KEYS you introduce go into fragment files
`adapter-voice\localization\fragments\<your-topic>.english.txt` and `<your-topic>.russian.txt`
(same tab-separated `key<TAB>text` format as `adapter-voice\localization\speechbrokervoiceadapter.english.txt`,
prefix `$SPEECHBROKERVOICE_LOG_` or `$SPEECHBROKERVOICE_WORD_`). The integrator merges them into the
main tables and regenerates `LocStrings.h`. Do NOT edit the main tables or `src\LocStrings.h`.

Nothing under `src\models\` includes `Config.h`, `RE/` or `SKSE/`. `windows.h` is allowed where
needed (define `NOMINMAX` and `WIN32_LEAN_AND_MEAN` before including it, after every std header).

## Header amendments (decided; apply them in the header you own)

- `Arbiter.h`: `struct Hypothesis` gains `std::vector<std::string> voters;` (ids of every model that
  said this text after the fold, winner first). `struct Slice` gains `std::int32_t refines{ 0 };` (id
  of an already-sent slice whose hypotheses this one replaces; 0 = a new piece). `TurnLedger`'s
  constructor gains a fourth parameter `std::atomic<std::int32_t>& a_nextSliceId` - the session-wide
  slice counter owned by Host (slice ids are monotone for the SESSION, not per turn).
- `ModelHost.h`: `void Broadcast();` is REMOVED from `Host` (the SKSE dispatch lives in `main.cpp`,
  which calls `Host::Table()`). `Host` gains three public methods:
  - `void Drop(const std::shared_ptr<Collector>& a_collector, SpeechBrokerVoiceHandle a_handle, Removal a_why);`
    = `Collector::Remove` + one log line + if the collector is now `AllAnswered()` and `Close()` returns
    true, `PostClosed` it.
  - `void RemoveEverywhere(SpeechBrokerVoiceHandle a_handle, Removal a_why);` = `Drop` from every
    open timed collector (not the probes).
  - `void PostClosed(std::shared_ptr<Collector> a_collector);` = enqueue to the workers.
  `Host` gains `std::atomic<std::int32_t> _nextSliceId{ 1 };` and `std::atomic_bool _begun{ false };`
  (a model registered after `Begin` has its thread started at once inside `Register`).
- `Model.h`: unchanged in shape. `Model::Begin` DETACHES its thread (a Model is never destroyed) and
  `Model` gains `bool Done() const noexcept` (the dispatch thread has left its loop), which
  `WaitDrained` polls with a bounded sleep.
- `Reputation.h`: unchanged. `Reputation.cpp` includes the contract ONLY for one
  `static_assert(Speed::Fast == SPEECHBROKERVOICE_CLASS_FAST ...)`.

## Threads, and who calls what

- EARS CONSUMER THREAD -> `Host::OnPass(pass)`: roster = every registered model with
  `TakesPass(pass.final)`; empty -> one log line (latched for the "every model is finalOnly" case) and
  return (the serial is spent). `utteranceId = _nextUtterance++`; `deadline = now + max over roster of
  Model::BudgetMs()`; `collector = make_shared<Collector>(id, pass, handles, deadline)`;
  `_open[id] = collector`; `_scheduler.Arm(deadline, collector)`; for each model:
  `Offer(DispatchEntry{ audio = pass.audio, collector, utteranceId, turnId, serial, final, lostSamples, probe = false })`
  and log the `Queued` outcome. Never blocks, never calls into a model. Whole body in `try/catch(...)`.
- MODEL DISPATCH THREAD (one per model; `Model::Begin` -> private `Run`): `PrepareDispatchThread`;
  Start loop up to `startAttempts` with the model's own `startupMs` between attempts (RETRY and
  NOT_READY are both recoverable; a fault -> `Eject(Faulted)`; any other status -> `Eject(StartRefused)`;
  attempts exhausted -> `Eject(StartExhausted)`); one log line when a Start took longer than
  `startupMs`; then `_life = Probing` and `_host.SubmitProbe(self)`; then the loop: wait on `_wake`
  for `_pending` / `_control` / `_stopping`. Control first: Vocabulary -> `GuardedSetVocabulary` (only if
  the table has one), Cancel -> `GuardedCancel` (only if the table has one). Then the entry: if
  `collector->IsClosed()` or the deadline is already spent -> drop:
  `_host.Drop(collector, handle, Removal::DroppedUnsent)` + `_standing.NoteDropped()`. Wait (bounded by
  the deadline) until `DebtHasRoom()`; `TakeDebt(utteranceId)`; fill `SpeechBrokerVoiceRequest`
  (`structBytes = sizeof`, serial, turnId, utteranceId, final, sampleCount, `samples = audio->data()`,
  lostSamples, `deadlineMs` = remaining ms or 0 for the probe, `format = {16, 16000, 1, FLOAT32}`);
  `_inside = true`; `GuardedSubmit` inside `try { } catch (const std::exception&) { } catch (...) { }`;
  `_inside = false`; measure the call, log if over `submitOverrunMs`.
  Outcomes: OK -> nothing more (Complete will come). BUSY -> `ReleaseDebt`; `_busy.Note(true)`;
  `_host.Drop(collector, handle, DroppedUnsent)` WITHOUT `NoteDropped` (BUSY costs nothing); if
  `_busy.Rate() > busyRateLimit`: first time -> `_busyDemoted = true` + one line; already demoted and
  `entry.final` -> `Eject(BusyOnFinal)`. NOT_READY -> `ReleaseDebt`; `NoteReady(false)`.
  REFUSED / unknown / a C++ throw -> `ReleaseDebt`; `_standing.NoteFailure(false)`; `_host.Drop(...)`;
  log. A fault -> `ReleaseDebt`; `_host.Drop(...)`; `Eject(Faulted)`. Every outcome but BUSY ->
  `_busy.Note(false)`. If `ReleaseDebt` returns false after BUSY or NOT_READY the model called Complete
  and then refused: log it and `NoteFailure(false)` (`Ejection::ProtocolBroken` only if it repeats).
  On loop exit (`_stopping`): if Start had returned OK and the model was not ejected for a fault ->
  `GuardedStop`. Then `Done()` becomes true.
- `Model::Offer(entry)`: under `_lock` decide Accepted / AcceptedReplacing (same `turnId`, higher
  `serial`, pending `final == false`) / Refused; move the dropped entry OUT of the lock, then
  `_host.Drop(dropped.collector, handle, DroppedUnsent)` + `_standing.NoteDropped()`. A probe entry is
  never replaced and never refused (its slot is empty when it is offered).
- `Host::Complete` (a model's thread; static `OnComplete` -> POD context -> `GuardedHostCall` -> body,
  all inside `try/catch(...)`): `model = Find(handle)`; none, or `Where()` is Draining/Ejected -> STALE.
  Validate the answer in the documented order -> MALFORMED (+ `NoteFailure(false)`). Copy into a
  `Reading` (every string via one `strnlen` helper capped at `SPEECHBROKERVOICE_MAX_STRING_BYTES`,
  truncation logged once per model; sentinels: `endsSentence` -1 -> false, `lastWordProb < 0` -> -1.0f,
  `noSpeechProb < 0` -> 0.0f, `medianGapMs < 0` -> 0, `words < 0` -> 0; times clamped to
  `Subject().DurationMs()`, logged once per model). `collector = Collecting(utteranceId)` (searches
  `_open` then `_probes`); none -> STALE (and `model->ReleaseDebt(id)`). A probe collector:
  `model->ProbeAnswered(any fragment with non-empty text)`; `Deliver`; `Close`; return OK (no worker).
  A timed one: `!Deliver` -> STALE; `model->ReleaseDebt(id)`; if `AllAnswered() && Close()` ->
  `PostClosed(collector)`. Return OK.
- `Host::Ready`, `Host::Log`, `Host::Unregister`: as the headers say. `Log` writes
  `"{id}\t{key}\t{arg0}\t{arg1}..."` raw through spdlog at the mapped level (not through Loc - the key
  is the model's). `Unregister`: `BeginDrain` under no lock; `RemoveEverywhere(handle, Unregistered)`;
  erase the probe collector of that handle; `WaitDrained(stopMs)`.
- SCHEDULER THREAD: earliest armed deadline; when due: lock the weak_ptr; if `Close()` returns true ->
  `_post(collector)`. Nothing else, ever.
- WORKERS: `Host::Assemble(collector)`: (1) `missing = collector->Missing()`: each ->
  `model->ReleaseDebt(id)`, `Standing().NoteFailure(true)`, one line. (2) answers: OK ->
  `NoteCall(latencyMs, scores of every fragment)`; FAILED -> `NoteFailure(false)` + a line with `failed`;
  CANCELLED -> a debug line only. (3) the terminal fall: `base = _arbiter->Base(answers)`; if base: the
  last fragment `[startMs, endMs]` -> samples of `subject.audio` (clamped) -> if longer than
  `kTargetSampleRate / 10` samples: `TerminalFall(workerScratch, span, subject.pitch)` where
  `workerScratch` is a `PitchTracker` per worker built from the ears' `ProsodySettings`
  (`Ears::Settings().prosody`, `Prepare()` once). (4) `ledger = Ledger(turnId)`;
  `slices = ledger->Reconcile(*collector, *_arbiter, _ears->Judge(), fall)`. (5) each slice ->
  `_publish(slice)`. (6) erase the collector from `_open`; if `collector->Final()` and no other open
  collector carries this `turnId` -> erase the ledger (a line if `!Emitted()`).
- `TurnLedger::Reconcile` (under its own lock): serial guard (behind `_lastReconciled` -> one line,
  return empty); `spans = arbiter.Spans(answers)`, `lane = arbiter.Merge(answers)`,
  `arbiter.NoteAgreement(lane)`; `Snap(ms)` = the nearest of (`*subject.anchors` plus `elapsed`) within
  `snapSlackMs`, else `ms`, where `elapsed = subject.DurationMs() + subject.tailSilenceMs` (this is the
  same arithmetic as `SpeechTurn::Snap`, engine/turn.py:97-109, done here over the pass's own copy).
  Per span i: `hyps = lane[i]` (skip when empty); `start/end` snapped; skip when `end <= start`;
  `hasAfter = i + 1 < n`; `silenceAfter = hasAfter ? spans[i+1].startMs (raw) - end : tailSilenceMs`;
  `complete = judge.Judge(span.signs, hasAfter, silenceAfter, hasAfter ? nullopt : fall)`;
  `same` = a sent slice within `matchSlackMs` on both edges: if its top hypothesis `Key()` equals the
  new top `Key()` -> skip; else emit `Slice{ id = same.id, refines = same.id, start, end, hyps, complete }`
  and update the sent record's hypotheses; otherwise `covered` = sent slices lying inside
  `[start - slack, end + slack]` with `duration < newDuration - slack` -> emit
  `Slice{ id = nextSliceId++, start, end, hyps, complete, supersedes = covered ids, speechElapsedMs = end,
  lengthClass = Classify(span.signs.words > 0 ? words : word count of the top text, segments), emittedMs = elapsed }`
  and append it to `_sent`. Finally `_lastReconciled = serial`.
- `Arbiter`: `Base` = among usable readings (OK and non-empty) those with `CarriesWordTimings()`, else
  all usable; the most fragments; the FIRST among equals. `Merge`: per base fragment, for each usable
  reading find the fragment with the largest overlap; count it only if
  `overlap >= max(overlapMs, duration * overlapPercent / 100)`; hypothesis `{ text, score = standings.Of(id).Normalize(raw), model = id, voters = {id} }`;
  fold by `Key()` (empty key skipped): same key -> `agreed++`, `voters` appended, the record kept is the
  one with the larger `score * Weight()` (the score kept is the raw normalised score, never
  multiplied); `stable_sort` by (`agreed` desc, `score * Weight()` desc). `NoteAgreement(lane)`: for
  every stretch whose hypotheses carry at least two distinct voters in total: voters of a hypothesis
  with `agreed >= 2` -> `NoteAgreement(true)`, every other voter in that stretch -> `NoteAgreement(false)`.
  `Hypothesis::Key()`: UTF-8 aware lower-case, letters/digits/spaces only, trimmed (use
  `MultiByteToWideChar` + `CharLowerBuffW` + `IsCharAlphaNumericW` per code unit, back to UTF-8).
- `Reputation` = `engine/reputation.py` exactly: percentile index `min(n-1, int(n*p))` over a sorted
  copy; `Normalize`: pool = live scores if `>= minSample` else calibration scores; pool `< minSample` ->
  raw; else `count(s <= raw) / size`; `Weight`: `trust or defaultTrust`, `*= 1 - min(maxFailurePenalty, failures/calls)`
  when calls > 0, `*= 1 - min(maxInventionPenalty, InventionRate())`, clamp to `[minWeight, 1]`;
  `NoteDropped` increments `_dropped` only; `NoteFailure` increments `_calls` and `_failures` (and
  `_timeouts` when timeout); rings keep the last `keep` observations; `Load(path)` reads
  `{ "<id>": { "trust": 0.8, "declaredClass": "accurate", "calibrationScores": [..] } }` and returns
  false on any failure without throwing.
- `Guard.cpp`: the filter passes `0xE06D7363` (C++ throw), `0x406D1388` (thread naming),
  `0x80000003` and `0x80000004` (debugger) with `EXCEPTION_CONTINUE_SEARCH`; anything else fills the
  `Fault` (code, `ExceptionRecord->ExceptionAddress`, `stackOverflow = code == EXCEPTION_STACK_OVERFLOW`)
  and returns `EXCEPTION_EXECUTE_HANDLER`; `_resetstkoflw()` is called in the `__except` BODY when
  `stackOverflow`. Every guarded function has POD parameters only and no local with a destructor
  (MSVC C2712). `PrepareDispatchThread` = `SetThreadStackGuarantee`. `FaultName` returns a static
  name for the common codes and a `thread_local` hex buffer otherwise.

## The bridge side (main.cpp, owned by the integrator)

`Host::Get().Configure(config.models, config.ears, PublishToBridge)` at load; at SKSE `kDataLoaded`
`main.cpp` dispatches `&Host::Table()` (`dataLen == sizeof(void*)`, type
`SPEECHBROKERVOICE_MESSAGE_HOST`) and then starts a worker thread that calls `Host::Get().Begin()`.
`PublishToBridge(const Slice&)` maps to `SpeechBrokerAPI::UtteranceIn`: `text/engine/score` from
`hypotheses[0]`, `margin = hyp[0].score - hyp[1].score` (0 with one), `isFinal = true`,
`refinesId` = bridge id of `slice.refines`, `supersedes` = bridge ids of `slice.supersedes` (a bounded
map slice id -> bridge id, `idMapLimit`), `complete`, `lengthClass`, `altText/altScore` from the other
hypotheses, `latencyMs = 0`, `durationMs = slice.DurationMs()`, language "" (the bridge takes the
adapter's). Dropped when the bridge has put the adapter in reserve (`kJobListen active == false`).
