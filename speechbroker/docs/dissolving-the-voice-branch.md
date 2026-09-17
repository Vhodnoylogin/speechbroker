# Dissolving the voice branch into this module

The `voice` branch carries a python voice service written before this module took its present shape:
it owns a microphone, cuts speech, drives recognition models, calibrates them and keeps their
reputation. It is being taken apart and its work moved here, after which the branch is deleted.

This page is the map of that move. It exists because the branch will be gone and the reasoning must
not go with it: every number in the adapter came from a line of that python, and when somebody asks
in a year why the pre-roll is 800 ms, the answer has to be findable.

## The architecture this serves

Stated by the owner, and it is the target rather than a description of what exists:

| Part | What it owns |
|---|---|
| **bridge** | **only** auctions an already-recognised string among subscriber mods |
| **adapter** | the microphone; cuts the stream into utterances; hands raw audio to every model mod; sends the text to the bridge |
| **model mod** | a **shim**, not a model: an SKSE plugin that speaks the contract on one side and, on the other, **is** a model, **raises** one, or **attaches** to one already running — here or on another machine |

Both the adapter and every model mod are SKSE plugins. The adapter is hard-coded, one implementation
for the whole build. A model mod is replaceable by installing a different mod.

## Where each file of the branch goes

`rewritten` means C++ inside a plugin; `tooling` means it stays python and moves under
`tools/audiolab`; `dropped` means it dies with the service.

| From `voice/` | Lines | To | How |
|---|---|---|---|
| `engine/audio.py` | 139 | `adapter-voice/src/audio/` | rewritten — **done** |
| `engine/parts.py` | 104 | the contract header, and `src/turn/` | rewritten — **done** |
| `engine/judge.py` | 63 | `src/turn/Completeness.cpp` | rewritten — **done** |
| `engine/turn.py` | 208 | `src/turn/{Pacer,SpeechTurn}.cpp` | rewritten — **done** |
| `engine/prosody.py` | 108 | `src/turn/Prosody.cpp` | rewritten — **done** |
| `engine/arbiter.py` | 137 | `src/models/Arbiter.cpp` | rewritten |
| `engine/reputation.py` | 215 | `src/models/Reputation.cpp` | rewritten, load only |
| `engine/models.py` | 261 | the contract, and the shim of a model mod | rewritten, split at the shim boundary |
| `engine/engine.py` | 171 | `src/turn/Ears.cpp` and the dispatch half | rewritten |
| `engine/cuda.py` | 57 | `model-whisper-ru` | rewritten — pure model bring-up |
| `engine/calibrate.py` | 411 | `tools/audiolab` | tooling — needs ground truth, cannot exist at run time |
| `engine/replay.py` | 91 | `tools/audiolab` | tooling |
| `voice-service.py` | 653 | — | dropped: the whole HTTP service disappears |
| `voice-record.py`, `voice-calibrate.py` | 431 | `tools/audiolab` | tooling |
| `engine-bench.py`, `bench-corpus.py`, `engine-replay.py`, `engine-scenario.py`, `prosody-check.py` | 537 | `tools/audiolab` | tooling |
| `voice.json` — `vad`, `pacer`, `segments`, `completeness` | — | `adapter-voice/speechbroker-voice.json` | rewritten — **done**, 56 fields |
| `voice.json` — `engine.models` | — | each model mod's own settings | dropped from the adapter |
| `voice.json` — `calibration` | — | `tools/audiolab`, as the labelling manifest | **must not be dropped**: it is the only ground truth for the 14 takes recorded before the `-end`/`-open` convention, and without it the completeness corpus falls from 22 labelled takes to 8 |
| `voice.json` — `port`, `watchdogSec`, `logFile`, `wakeWord` | — | — | dropped with the service |
| `reputations.json` | — | shipped into the adapter's mod | needs a publishing path, or the calibration fallback is dead |
| `models/` — 2.1 GB | — | `model-whisper-ru/weights/` | through Git LFS; see below |
| `samples/`, `requirements.txt` | — | `tools/audiolab` | tooling |
| `session.jsonl` | — | read once, then dropped | the only record of what the live service actually emitted |

## What was decided, and by whom

- **The fast/accurate pair stays.** The owner keeps it so that the race between two models is
  something the adapter can actually demonstrate. The last measurement had both declared classes
  answering as fast (p50 81 ms for small, 99 ms for turbo, against declared budgets of 700 and
  2500), so the draft-and-refine machinery is currently idle — that is a reason to measure again,
  not to drop the pair.
- **The weights go into git**, through LFS, in this repository, and the repository is **public**.
  The models are `openai/whisper-large-v3-turbo` and `openai/whisper-small`, both MIT, converted and
  quantised here rather than taken as somebody else's repack — one candidate repack had no licence at
  all, and no licence is no right to redistribute. Quantising to int8 halves the payload (2.02 GiB to
  1.05 GiB) without touching anything that was settled.
- **`git clone` must never be the player's download channel.** GitHub gives 10 GiB of LFS bandwidth a
  month; a hundred clones of the weights would exceed it. A committed `.lfsconfig` with
  `fetchexclude` makes a plain clone fetch pointers by default, and the archive built by
  `package.ps1` is what a player installs. The trap to write down verbatim: `git lfs pull --include`
  does **nothing** unless `--exclude=""` is passed as well, because the exclude wins, and it fails
  silently.
- **Our own ready-made model mod runs as a child process** (`SPEECHBROKERVOICE_KIND_CHILD`), and
  `INPROCESS` is documented as the enthusiast's path. The deciding fact is measured: the commonest
  real failure of this stack is a CUDA/cuDNN path or version mismatch, and it is **not an exception**
  — a missing cuDNN sub-library ends the process with exit code 127, no exception of any kind, no
  crash log, the window simply gone. In a child that is a number `GetExitCodeProcess` returns. The
  argument is against dragging a third-party GPU runtime into `SkyrimVR.exe`, not against in-process
  model mods in general: a self-contained CPU backend removes it entirely.
- **Speech is not an afterthought.** The piper voice was a second model inside the same python
  service, and `whisper-ru.json` declaring `provides: "asr,tts"` was false about Whisper. Speech has
  to become its own model mod; it did not "go missing", it was always part of what is being
  dissolved.

## What no guard reaches

Written here because it shapes the whole dispatch design. Inside one process the adapter can contain
a synchronous fault raised on a thread it created, inside a call it made — `__try`/`__except`, which
needs no compiler flag and is what SKSE itself does around a plugin's entry point. It cannot contain
a fault on a thread the model created, nor any fail-fast (`abort`, `std::terminate`, a corrupted
heap, a duplicate OpenMP runtime), which Windows delivers past every handler in the process including
the player's crash logger, leaving no log at all. Containment means the game survived, not the model:
the unwind runs no destructor in the frames between, so the model is ejected rather than retried.

## The order of work

Each stage is independently commitable and says how we would know it worked.

1. **Publish the contract from the adapter's SDK.** Done: `contract/speechbroker-voice-model.h`, both
   prose pages, `docs/model-host.md`, and the header in `publish[]`. Proved by compiling the header
   as C and as C++ at `/W4 /WX`, inside a hostile `#pragma pack`, and at `/Zp4` and `/Zp1`, plus
   three builds that must fail and do.
2. **The ears.** Done: microphone, resampling, gate, pacer, turn, prosody, completeness. Proved by
   building and by number; **not yet by listening**, which is what the wav source exists for.
3. **The dispatch half.** The registry, the collector, arbitration, reputation, the guard, and Ears
   actually constructed. Proved offline by a fake model against a canned wav, before any weights.
4. **`model-whisper-ru` becomes a program.** A shim plus a child process. Proved by the word error
   rate per take matching what the python measured on the same audio.
5. **Cut the recognition path over HTTP.** The microphone may have exactly one owner.
6. **`tools/audiolab` absorbs the surviving python**, including the labelling manifest.
7. **Re-home the weights and delete the branch.** Proved by a clean redeploy of all three mods, in
   order, on a machine where `dev/voice` no longer exists.
