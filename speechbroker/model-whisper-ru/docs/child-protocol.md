# The child protocol

*Теги: speechbroker, model-mod, protocol*

This is the wire between the shim (`SpeechBrokerModelWhisperRu.dll`, inside `SkyrimVR.exe`) and its
child (`SpeechBrokerWhisperChild.exe`, a process of its own). It is a **specification, not a
description**: somebody must be able to write another child against this page without reading a line
of our source. Where this page and `src/core/Wire.h` disagree, this page is the one a stranger reads
and the code is what gets corrected.

The contract one level up — between the adapter and the shim — is
[`speechbroker-voice-model.h`](../../adapter-voice/contract/speechbroker-voice-model.h). Nothing
here is visible to it. A model mod that runs its model in-process, or reaches a server, speaks its
own protocol or none and owes this page nothing.

## Why there is a child at all, in one paragraph

Measured on this machine, 17.09.2026: loading `cudnn64_9.dll` with a sub-library unreachable **ends
the process with exit code 127** — no exception of any kind, past every handler in the process and
past the player's crash logger, leaving no log at all. Inside `SkyrimVR.exe` that is the game gone
with nothing to read. In a child it is a number `GetExitCodeProcess` returns, which the shim turns
into an ordinary failure while the other models carry on. The argument is against dragging a
third-party GPU runtime into the game's process, not against in-process model mods in general.

## The shape of it

- **Transport:** the child's `stdin` and `stdout`, both binary, both anonymous pipes created by the
  shim. `stderr` is left as the game's and is not used: this process has no console and every
  diagnostic is a `Log` frame instead.
- **Direction:** the shim writes requests, vocabularies, cancels and one farewell; the child writes
  one hello, any number of log lines, and exactly one reply per request.
- **Framing:** every message is a 16-byte header and a body of the length the header names.
- **Byte order:** little-endian throughout. Integers are two's complement, floats are IEEE-754
  binary32.
- **Alignment:** none is assumed. Every field is written at its natural offset inside the body with
  no padding at all, so a reader that walks the body byte by byte is right.

### The shim never blocks the adapter, and this is how

The adapter calls `Submit` on its own dispatch thread and that call **must return within a few
milliseconds and must never wait** — not on a pipe, not on a lock. So the shim owns two threads of
its own and the pipe is touched by neither the adapter's thread nor the game's:

| Thread | What it does | What it may block on |
|---|---|---|
| the adapter's dispatch thread | copies the samples, puts a job on a queue, returns `OK` | nothing |
| the shim's **writer** | takes jobs off that queue and writes `Request` frames | the pipe's write buffer |
| the shim's **reader** | reads every frame and pays the debts by calling `Complete` | the pipe, forever |

A separate reader matters for one reason that is easy to miss: `Log` frames arrive at moments of the
child's choosing, including while no request is outstanding. With a single thread doing
write-then-read, those lines would sit in the pipe until the next request, and a child that wrote
enough of them would fill the buffer and **stall inside its own inference**. The reader drains
everything as it comes.

## The header

| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 4 | `magic` | `0x43564253` — the bytes `S B V C` in that order |
| 4 | 2 | `version` | `1`. Grows by one, never branches |
| 6 | 2 | `type` | see below |
| 8 | 4 | `bodyBytes` | length of the body, `0` … `67108864` (64 MiB) |
| 12 | 4 | `reserved` | `0` |

A header whose magic is wrong, whose version is not the reader's own, or whose `bodyBytes` is above
the ceiling **ends the conversation**. There is no resynchronising from an unidentifiable frame: the
next bytes would be read as somebody's length. The child leaves with exit code 2 and the shim treats
the closed pipe as a dead child.

Sixty-four megabytes is sixteen times the largest lawful request — the contract's `maxRequestSamples`
is twenty seconds, 1.28 MB — which leaves room for a setting to be raised without touching the
ceiling.

## The types

| # | Name | Direction | Meaning |
|---|---|---|---|
| 1 | `Hello` | child → shim | I am up and my model is loaded. Exactly one, before anything else |
| 3 | `Request` | shim → child | one whole buffer, from sample zero |
| 4 | `Reply` | child → shim | one reading of that whole buffer. Exactly one per `Request` |
| 5 | `Vocabulary` | shim → child | advisory phrases |
| 6 | `Cancel` | shim → child | advisory: that answer is no longer wanted |
| 7 | `Log` | child → shim | a localisation key and its arguments |
| 8 | `Bye` | shim → child | stop; leave with exit code 0. Empty body |

`2` is not used: it was a `Ready` that `Hello` made redundant, and the number is left alone rather
than reused, because a number that once meant something else is worse than a gap.

### A string, everywhere it appears

`u32 byteCount`, then exactly that many bytes of **UTF-8 with no terminator**. `byteCount` is below
4096 — the same ceiling the contract puts on its own strings, so that a string which crossed this
wire can be handed to the adapter unmeasured. A longer string is **truncated by the sender**, never
refused: losing text beats losing the answer that carried it.

### `Hello` (1), child → shim

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `protocolVersion` — must equal the header's `version` |
| 4 | 4 | `maxSamples` — the longest buffer this child will accept |
| 8 | … | `backend` (string) — `"whisper.cpp"`. For the log, never routed on |
| … | … | `modelId` (string) — echoed from `--model-id`, so a mix-up is visible |

Until this arrives the shim answers the adapter's `Start` with `RETRY`, which is exactly what `RETRY`
is for: nothing is wrong, the weights are still coming off the disk, come back later.

### `Request` (3), shim → child

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `utteranceId` (i64) — identifies the answer, and nothing else does |
| 8 | 8 | `turnId` (i64) |
| 16 | 4 | `serial` (i32) — which pass this is inside the turn, from 1. **Serials skip** |
| 20 | 4 | `final` (i32) — 1 if the buffer will not be extended again |
| 24 | 4 | `lostSamples` (u32) — samples the *adapter* lost inside this buffer |
| 28 | 4 | `deadlineMs` (u32) — 0 means no deadline is stated |
| 32 | 4 | `sampleCount` (u32) |
| 36 | `sampleCount` × 4 | the samples: float32, mono, 16000 Hz, nominally [-1, 1] |

**The buffer is always from sample zero.** There is no streaming and no delta: the child re-reads all
of it and gives back its own full segmentation of it, so that any two answers are comparable with
each other. A later pass of one turn carries a buffer that ends **no earlier** than the one before it
— *not* longer: in the ordinary one-phrase turn the final pass is byte-identical to the interim pass
before it, because the silence that triggered each was trimmed off each. Answering two identical
passes identically is correct.

**A buffer that is quiet throughout is lawful.** Straight after start-up the adapter submits one
second of digital silence as an ordinary request, with a `turnId` and `serial` of its own, `final` 1
and `deadlineMs` 0. It carries no flag and must never be given one. Answer it exactly as any other
buffer — the honest answer to silence is **no fragments at all**, and a model that answers it with
text is recorded as one that invents.

`deadlineMs` is advisory to the child: the shim does not enforce it, because killing an inference
mid-way costs more than a late answer, and the adapter discards a late answer for free.

### `Reply` (4), child → shim

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `utteranceId` (i64) — the one from the request |
| 8 | 4 | `serial` (i32) — echoed |
| 12 | 4 | `status` (i32) — `1` ok, `5` cancelled, `6` failed. **`0` is a refusal, never success** |
| 16 | 4 | `latencyMs` (i32) — the child's own inference time. Not what the adapter is told |
| 20 | 4 | `lostSamples` (u32) — samples the child lost or refused |
| 24 | 4 | `fragmentCount` (i32) — 0 … 4096 |
| 28 | … | `fragmentCount` fragments, each laid out below |
| … | … | `failed` (string) — empty unless `status` is 6 |

One reply per request, **always**, whatever happened. The shim owes the adapter exactly one
`Complete` per accepted utterance, and this is where it comes from.

`latencyMs` here is the child's own measurement. The number the adapter receives is measured by the
**shim**, from `Submit` to `Complete`, and includes queueing — that is what the contract asks for,
and the two are deliberately different.

### One fragment, 36 bytes plus its text

| Offset | Size | Field | Sentinel |
|---|---|---|---|
| 0 | 4 | `startMs` (i32) — ms from sample zero of the submitted buffer | — |
| 4 | 4 | `endMs` (i32) — `startMs <= endMs` | — |
| 8 | 4 | `score` (f32) — probability in [0, 1], higher better. `exp(avg_logprob)` for Whisper | — |
| 12 | 4 | `endsSentence` (i32) | `-1` |
| 16 | 4 | `lastWordProb` (f32) | `-1.0` |
| 20 | 4 | `noSpeechProb` (f32) | `-1.0` |
| 24 | 4 | `medianGapMs` (i32) | `-1` |
| 28 | 4 | `words` (i32) | `-1` |
| 32 | … | `text` (string) — may be empty, and empty is a lawful answer | — |

Fragments are ordered by `startMs` and do not overlap. The shim sorts and closes overlaps before it
hands them on, because a tangled answer is refused **whole** by the adapter and counted against this
model — but a child that produces one is defective, and the repair belongs in the child.

**The sentinels are not decoration.** `-1` means "I do not know" and the adapter degrades to a flat
guess. `0` is a *claim*, and `lastWordProb` is the dangerous one: the adapter picks one model's
segmentation as the lane onto which every other model's text is matched, and picks from among the
models that carry word timings, tested as exactly `lastWordProb` and `medianGapMs`. A child that
divides a segment's time proportionally by string length — which is what you do when the model
returns no word timings — **must leave both at -1**, or it wins the lane with boundaries that move
between passes. `medianGapMs` is `-1` for a piece of fewer than two words: there is no gap to
measure, and `0` would say the words ran together.

`score` must not be normalised and must not be multiplied by any opinion the child holds of itself.
The adapter ranks a score inside that model's own recorded distribution, but only once it holds ten
of them; until then it compares raw numbers across models directly, so a log-probability or a 0–100
confidence would win or lose every early comparison on scale alone.

### `Vocabulary` (5), shim → child

`i32 count`, then `count` strings. Advisory by contract: make a prompt of them, hot words, a grammar,
or nothing at all. Already clipped by the shim to the contract's ceiling of 64 and to the
`promptPhrases` setting, so a child may use all of them. The order is the adapter's merge order and
means nothing.

### `Cancel` (6), shim → child

`i64 utteranceId`. Advisory, and it arrives only for a request that has already gone down the pipe —
one still on the shim's queue is dropped there and never sent, which is the cheapest cancelled pass
there is. A child that cannot abort an inference ignores the frame and answers normally; the shim
pays the debt either way. A child that can abort answers with `status` 5, which pays the same debt
sooner.

### `Log` (7), child → shim

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `level` (i32) — 0 debug, 1 info, 2 warn, 3 error. The contract's own numbers |
| 4 | … | `key` (string) — a localisation key of this module's table |
| … | 4 | `argCount` (i32) — 0 … 16 |
| … | … | `argCount` strings |

**The child renders no text.** It has no localisation table and wants none: it sends a key and its
arguments, the shim hands them to the adapter's `Host::Log` verbatim, and the adapter writes down the
model's id, then the key, then the arguments, tab separated. It substitutes nothing — it does not
have the table. So a new message needs an edit to `localization/` and to nothing else, and a model
mod never needs the adapter changed in order to say something new.

## The endings

The exit code is the whole diagnostic channel of a child that could not start, so every one of them
is written down.

| Code | Meaning | What the shim does |
|---|---|---|
| 0 | stopped normally — `Bye`, a closed stdin, or the parent went away | nothing; expected |
| 2 | the arguments were unusable, or the frame stream was lost | `REFUSED` at `Start`: permanent |
| 3 | the backend library is missing or is not whisper.cpp | `REFUSED`: no waiting installs a DLL |
| 4 | the weights folder is missing | `REFUSED` |
| **127** | **a dependency of a loaded library could not be resolved** | `REFUSED`, and the log line names it |

127 is the measured one and the reason this whole arrangement exists. When the child dies *after*
start-up the shim reads the same code, writes one line naming it, fails every outstanding utterance
with `FAILED`, and calls `Ready(handle, 0, ...)` — which removes it from every open pass as a
*removal* rather than a timeout, so it costs the model no standing. It is not raised again: the
commonest cause of a 127 is a runtime that is not installed, and a relaunch loop would hide it behind
a wall of identical failures.

## Arguments the shim passes

Every path is absolute and is derived by the shim from its own module folder; nothing on the command
line is true only on one machine.

```
--model-id <id>          echoed back in Hello, so a mix-up is visible
--weights <folder>       verified against SHA256SUMS by the shim BEFORE the child is raised
--library <full path>    the backend DLL, loaded by full path
--preload <full path>    repeatable, in order, loaded by full path before the library
--device cuda|cpu
--compute-type <name>    the quantisation the weights were converted to
--beam-size <n>
--threads <n>            0 - let the backend choose
--language <iso>
--max-samples <n>        the adapter's maxRequestSamples, so the child can size once
--parent-pid <pid>       appended by the shim, always, and never by hand
```

`--parent-pid` arms the watchdog: the child opens that process and waits on it, and leaves the
instant it ends. It is the **belt to the Job Object's braces** — the shim also puts the child in a
job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, whose handle the kernel closes when `SkyrimVR.exe`
ends by any means at all. Both are wanted, because `Stop` may never be called, and a surviving child
keeps the graphics card and MO2's belief that the game is still running.

**Never PATH, never `SetDefaultDllDirectories`.** Dependencies are brought up by full path with
`LoadLibraryW`; a later bare-name load then finds the already-loaded module and resolves without any
policy change. The contract forbids the policy change inside the game's process because the mod that
pays for it is somebody else's; in the child there is nobody else to hurt, but a dependency found by
a search is one nobody can name afterwards — and "which cudnn did it actually load" is the question
this design exists to be able to answer.

## Writing another child

Everything above is enough. In summary: read 16 bytes, check the magic and the version, read
`bodyBytes` more, switch on `type`. Write `Hello` once you are up. Answer every `Request` with
exactly one `Reply` carrying the same `utteranceId`. Send diagnostics as `Log` frames with keys, not
sentences. Leave when stdin closes, when `Bye` arrives, or when `--parent-pid` goes away. Set the
sentinels to `-1` and never to `0`.
