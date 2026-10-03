# The model host: what the adapter owes, where the contract cannot say it

The contract between the adapter and a model mod is
[speechbroker-voice-model.h](../contract/speechbroker-voice-model.h). A contract states what crosses
the line; it cannot state how the side that wrote it must behave, because a third-party author
cannot check that and should not have to read it.

This page is the other half. Everything here is an obligation of **the adapter**, it is derivable
from no single line of the header, and every one of these points exists because leaving it unwritten
produced a defect that took real time to find — either in the python this is ported from, or in the
review of the contract itself.

The python reference is the `voice` branch, which is being dissolved into this module. Line numbers
below point at it while it still exists; when it is gone they remain as the record of where each
number came from.

## The audio of a pass

**The pass snapshot is a claim, not a call.** A `std::shared_ptr<const std::vector<float>>` created
once per pass, already trimmed of its trailing silence. Every queued dispatch entry captures a copy
and releases it either when `Submit` returns or when the entry is dropped unsent. **Never a view
into the live turn.** The guarantee the header makes to a model — that `samples` is valid for the
whole of the call — is derivable from this shape and from no other. The python had no such hole
because `turn.audio()` concatenated a fresh array per pass; it guaranteed **ownership per pass**,
not immutability, and this is the C++ spelling of the same thing.

**Never submit a buffer shorter than 250 ms of sound after trimming**, and never a buffer of
trailing silence the adapter itself detected.

**`maxRequestSamples` is enforced by ending the turn.** When the accumulated turn would exceed it,
close the turn, send the pass with `final == 1`, and start a new `turnId` at the next sample. No
sliding window: buffer sample zero must stay turn sample zero, because every time in the contract —
the anchors, the snap slack, the matching of a piece against what has already gone out — is measured
from turn zero.

## Dispatch

**The per-model queue holds at most one unsent request.** A replacement is accepted only from a
higher serial of the **same** `turnId`, and never over an entry whose `final == 1`. Without the
turnId condition the replacement silently discards the final pass of one turn in favour of the first
pass of the next.

**A dropped entry is recorded twice, not once:** against that model in the same ledger as a timeout,
**and** removed from that pass's expected set. Record only the first and the pass waits for a
request that was never sent; record only the second and a model that is always too slow to be
submitted to looks statistically perfect.

**`maxInFlight` is honoured against the debt.** Do not enter `Submit` while that many `Complete`s
are outstanding for that model. Without this the field is a promise backed by nothing.

**Two guards around every crossing, not one, and they catch different things.** A `catch(...)` takes
the C++ throw — the realistic one is a `std::runtime_error` out of a third-party library that could
not load its own dependency, not a `std::bad_alloc`, so log `what()`, it is the diagnostic. A
`__try`/`__except(EXCEPTION_EXECUTE_HANDLER)` takes the access violation, which no `catch` sees. The
handler of either must perform the same retraction a non-`OK` return performs, or the debt ledger is
left half-updated; a fault additionally ejects the model for the session, because `__except` unwinds
without running a destructor in any frame between and the state on the far side is of unknown shape.

**`__try` needs no compiler flag, and reaching for one is a trap.** Structured exception handling is
always available — MSVC generates support for it regardless of `/EH`, and SKSE's own plugin manager
wraps a plugin's entry point in `__try`/`__except` without `/EHa`. Do **not** switch this target to
`/EHa`: Microsoft counter-recommends mixing it with `/EHs`/`/EHsc` in one module, and `/EHsc` is set
twice in this build (the target's compile options and the cached `CMAKE_CXX_FLAGS`), so appending
would produce a contradictory command line rather than a change. What `__try` does need is its own
small non-inline function taking POD arguments: MSVC refuses `__try` in a function holding anything
with a destructor, so the C++ dispatch loop calls into the guard rather than containing it.

**Both directions are guarded, and the second one is the likelier fault.** The host table —
`Register`, `Complete`, `Ready`, `Log`, `Unregister` — is adapter code executed on the *model's* own
thread, over pointers the shim supplied. A stale `char*` from third-party code is the most probable
in-process fault in the whole design, and it lands inside the adapter on a thread the adapter never
created, inside a call the adapter never made. Each host entry point therefore carries its own POD
guard. Without it the contract's promise in that direction is unkeepable.

**The exception filter must pass through what it does not own.** Examine `ExceptionCode`: let
`0xE06D7363` reach the C++ handler and let the thread-naming exception `0x406D1388` pass, or the
structured guard swallows C++ throws before `catch(...)` and the ledger records the wrong failure.

**Supporting pieces.** `SetThreadStackGuarantee` of 64 KB on every dispatch thread, and
`_resetstkoflw()` called from the **body** of the `__except`, never from the filter. A `catch(...)`
at the top of every thread entry function: an exception escaping a thread procedure is
`std::terminate`, and that is a fail-fast nothing in the process observes.

**What no guard reaches, and what to do instead.** A fault on a thread the model created; any
fail-fast — `abort()`, `std::terminate`, heap corruption, a duplicate OpenMP runtime — which Windows
delivers past every handler including the player's crash logger, leaving no log at all; and a
library that calls `ExitProcess` on a missing dependency. Measured on this machine, 17.09.2026:
loading `cudnn64_9.dll` with a sub-library unreachable exits the process with code **127**, no
exception of any kind. That is why our own ready-made model declares
`SPEECHBROKERVOICE_KIND_CHILD`: in a child, 127 is a number `GetExitCodeProcess` returns and the
adapter routes into an ordinary ejection.

**Two things not to build.** `SetUnhandledExceptionFilter` is a single slot rather than a chain, and
the crash logger installed in this build re-arms its own from inside a vectored handler on every
exception dispatch — ours would be overwritten silently. And never `TerminateThread` a hung model
call: it leaves every lock that thread held permanently held, including CRT and loader locks, which
turns a hung recogniser into a hung game and, at exit, into the case where MO2 still believes the
game is running. Wait `stopMs`, log once, abandon the thread, never free its state.

## The deadline, and what closes a pass

**One absolute `steady_clock` deadline per collector**, set when the pass is created. The `deadlineMs`
handed to a model is computed **at the moment `Submit` is entered**, as `deadline - now`. If that is
already spent, the entry is dropped unsent and the model is removed from the expected set — never
submitted, never charged a timeout. A deadline measured from the model's own call while the timer
enforcing it was armed at pass creation is two different clocks wearing one name.

**A collector has four fields and no more:** the expected set (fixed at creation, shrink-only), the
answers, the deadline, the closed flag. Removal events — a dropped entry, an `Unregister`, a
`Ready(0)` — are logged as removals and cost no standing. **Never enter `Submit` for a collector
that has already closed.**

**Retirement is membership in the open-collector map, not a high-water mark.** A high-water mark
retires the silence probe the moment any later pass closes, which silently disables the one check in
the system that does not rest on trusting a model. Probe collectors live in a separate small map and
are erased only on `Unregister` or at the end of the session.

**The timer seals; a worker assembles.** When a deadline fires, the scheduler thread does exactly
two things: mark the collector closed and post to a worker. Folding, snapping, judging, reconciling
and handing to the bridge all run on the worker. The scheduler runs a task in its own single thread,
so anything heavy there delays every other armed deadline and manufactures the out-of-order closes
that the serial guard then has to catch.

**The serial guard lives at close, under the turn's own lock.** Compare the collector's serial with
the turn's last-reconciled serial: behind, and the collector is abandoned whole with every answer
`STALE` and no reconcile; ahead, and it reconciles and advances. The last-reconciled serial is
written nowhere else.

## Validation, in order

**Register refuses in this order:** NULL pointers → `structBytes` of info, model and session (a
multiple of 8, at least the version-1 size) → `abiVersion >= 1` → `reserved0 == 0` in both structs →
non-empty `id` → duplicate `id` (first wins) → NULL `Start`, `Stop` or `Submit` → `provides` parse →
`kind` against the player's policy. **Write nothing into the session on any status but `OK`.**

**An answer is validated in this order:** `structBytes` → `abiVersion` → status in
{`OK`, `CANCELLED`, `FAILED`} → `fragmentCount` within `[0, MAX_FRAGMENTS]` → `fragments` non-NULL
when the count is above zero → `fragmentStride` **exactly** the size of a fragment at the model's
declared version → and only then index. Then per fragment: `startMs <= endMs`, non-decreasing,
non-overlapping; clamp out-of-range times and log once rather than refusing a whole reading.

**One `strnlen` helper for every string copied out of a model struct** — `id`, `name`, `language`,
`provides`, every fragment's text, the failure text, the log key and every argument. Never `strlen`,
never `strcpy`. Truncate at the contract's maximum and log the truncation once per model, not once
per string.

**Log writes the id, then the key verbatim, then the arguments in order, tab separated.** Substitute
nothing. Drop a call whose argument count is out of range or that carries a NULL element, with one
line of the adapter's own.

## The life of a model

**Start:** up to five attempts with `startupMs` between them; `RETRY` and `NOT_READY` are both
recoverable. **No timeout on a hung Start** — wait on that model's own dispatch thread and log once
past `startupMs`. A late `OK` is honoured only if the model has not been ejected.

**The probe gates the roster.** After `Start` returns `OK`, submit the silence probe and hold the
model out of every pass until its probe answer arrives. A model that never answers the probe is
never asked for anything, with one log line. Ignoring the probe must not be the cheapest option:
zero inventions out of zero probes otherwise reads as a perfect record.

**The interim roster** uses the measured class once five latency samples exist and `declaredClass`
before that. `finalOnly` models are always excluded from interim passes. If the interim roster would
be empty because every installed model is `finalOnly`, run no interim passes at all and say so once
— do not ask a `finalOnly` model anyway.

**The deadline bootstrap** is `budgetMs + 1000 ms` until five latency samples exist, then the
measured p90. `budgetMs == 0` means the adapter's own default. Latencies are **not** persisted
between runs: hardware and surroundings change, and a stored number would look like a measurement
without being one.

**The busy rate** is a sliding window per model. Above one half, drop the model from the interim
roster and offer it final passes only; still above it there, drop it for the session. One log line
at each step. `BUSY` must not touch the failure numerator or denominator — counting the calls alone
would *raise* a busy model's weight.

**Unregister:** mark the handle draining, release the registry lock, then wait at most `stopMs`.
After that, leave the handle draining forever and abandon the model. `Complete`, `Ready` and `Log`
on a draining, dead or unknown handle read one atomic and return at once.

**Teardown is not promised as a join.** SKSE sends no shutdown message and a plugin dies with the
process; joining under the loader lock is a documented deadlock, and a third-party `Stop` is free to
take its whole startup budget. What must genuinely not survive the game is a **child process**, and
that is held by a Job Object with kill-on-close plus a parent-pid argument, not by a join. If an
orderly stop is wanted, hook a real in-game event, run it on a worker with a hard cap, and never on
the game thread and never from a destructor.

## The handshake and the table

**Broadcast the host table at `kDataLoaded`**, dispatching the **address of the table pointer** with
`dataLen == sizeof(void*)`. The table is a function-local static with process lifetime and is never
replaced.

**`SetVocabulary`:** clip the merged list to the contract's maximum, deliver it on the model's own
dispatch thread, never before `Start` returned `OK` and never after `Stop`.

## Numbers that are settings, not constants

`endSilenceMs` 1600, `sliceSilenceMs` 300 and `maxSpanMs` 4000 are read from the adapter's own
config. The contract promises a player may change them and tells a shim author not to hard-code
them; the adapter must make that true.

**Score normalisation falls back to the raw number below ten samples.** That is why the contract
fixes the scale at `[0, 1]`: the adapter must not pretend the ranking is always available. It also
means `reputations.json` needs a shipping path into the mod, or the calibration fallback is dead
along with it.
