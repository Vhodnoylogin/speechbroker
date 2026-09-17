# The child

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

`SpeechBrokerWhisperChild.exe` — one process, one model, one pipe. It is raised by the shim
(`SpeechBrokerModelWhisperRu.dll`, inside `SkyrimVR.exe`), it reads whole buffers of sound from its
`stdin` and it writes readings of them back down its `stdout`.

The wire between the two is [docs/child-protocol.md](../docs/child-protocol.md), and that page is the
specification: another child written against it needs none of this source.

## Why it is a separate process

Measured on this machine, 17.09.2026: loading `cudnn64_9.dll` with a sub-library unreachable **ends
the process with exit code 127** — no exception, past every handler, past the player's crash logger,
no crash log at all. Inside the game that is the game gone with nothing to read. Here it is a number
`GetExitCodeProcess` returns to the shim, which turns it into an ordinary failure while the other
models carry on.

## It has no dependencies, and that is the point

No CommonLibSSE, no SKSE, no json, nothing fetched. It is a Windows executable, the protocol, and a
backend loaded from a DLL at run time. So it configures and builds in seconds on a machine that has
never seen the game:

```
cmake -B build -S .
cmake --build build --config Release
```

`/W4 /WX`, the same wall the shim is built against, and for the same reason: this program runs where
nobody is watching, so a warning it was allowed to ignore is a defect nobody ever sees.

The one file it shares with the shim is `../src/core/Wire.cpp`. Shared rather than copied, because
two hand-written parsers of one wire format drift, and the drift is silent.

## The backend seam

One interface, `Recogniser` in `src/Recogniser.h`, with three methods: `Open`, `SetVocabulary`,
`Recognise`. One implementation, `src/WhisperRecogniser.cpp`, which loads whisper.cpp's C API from a
DLL **by full path at run time**.

Loaded rather than linked, deliberately. Linking would make this program refuse to *start* without
`whisper.dll` beside it — a hard import failure, `0xc000007b`, nothing to read and no way to say
which file was missing. Loading it ourselves turns the same situation into one sentence naming the
file we wanted, which is the whole difference between a mod that is broken and a mod that tells you
what to drop in.

Full path, and never `PATH` or `SetDefaultDllDirectories`. In this process there is nobody else to
hurt — the contract forbids the policy change inside the game because the mod that pays for it is
somebody else's — but a dependency found by a search is one nobody can name afterwards, and *which
cudnn did it actually load* is the question this whole design exists to be able to answer.

## What is here, and what is not

**Here:** the protocol, the framing, the exit codes, the parent watchdog, the binary-mode pipes, the
argument parsing, and a backend that resolves `whisper.dll` by full path and checks that the entry
points of whisper.cpp's C API are genuinely in it — `whisper_init_from_file_with_params`,
`whisper_full`, `whisper_full_default_params` and five more. That check is real: it tells a whisper
build from a file somebody renamed.

**Not here: the inference call.** `whisper.cpp` is neither vendored, downloaded nor built by this
repository, and `Recognise` therefore answers a failure saying so. The reason is narrow and worth
stating: `whisper_full` takes a `whisper_full_params` **by value**, and that struct is large and has
changed shape across releases. Declaring it from memory rather than from `whisper.h` is exactly the
kind of guess that produces a crash with no diagnostic — so the call waits for the real header rather
than being written blind.

### What a person must drop in

1. A build of **whisper.cpp** as a shared library: `whisper.dll`, plus `ggml*.dll` beside it and
   whatever CUDA runtime that build needs. Put them in `child/runtime/` in the source tree — the
   lay-out copies anything in there next to the child — or straight into
   `<mod>/SKSE/Plugins/speechbroker/models/whisper-ru/child/`.
2. Name any dependency that must come up **before** `whisper.dll` in the `child.preload` list of the
   model's settings, in order. Each is loaded by full path.
3. The weights in ggml format, in `weights/<id>/`, with a `SHA256SUMS` beside them
   (`tools\weights.ps1 -Write`).

Nothing else changes: the shim already passes `--library`, `--weights`, `--device`, `--compute-type`,
`--beam-size`, `--threads` and `--language` on the command line.

### And what is left to write

In `WhisperRecogniser.cpp`, with `whisper.h` on the include path: build the params from the options,
call `whisper_full` on the samples, and turn each segment into a `Piece`.

The one thing to get right there is **the sentinels**, and it is not obvious. `-1` means *I do not
know*, and the adapter degrades to a flat guess and does not punish the model for it. `0` is a
**claim**. `lastWordProb` is the dangerous one: the adapter picks one model's segmentation as the
lane onto which every other model's text is matched, and it picks from among the models that carry
word timings — tested as exactly `lastWordProb` and `medianGapMs`. If the times come from dividing a
segment proportionally by string length, which is what you do when the model returns no word
timings, **both must stay -1**, or this model wins the lane with boundaries that move between passes.
Fill them only from a real alignment to the audio.

Two more, from the same page:

- `score` is `exp(avg_logprob)` — a probability in [0, 1], higher better. Not normalised, and not
  multiplied by any opinion the model holds of itself.
- **Silence must be answered with no fragments at all.** Straight after start-up the adapter submits
  one second of digital silence as an ordinary request, with no flag on it and none possible. A model
  that answers it with text is recorded as one that invents — and this is not hypothetical: the first
  model ever registered here answered a buffer of zeros with a full sentence, with its own
  voice-activity filter switched on.

## The endings

| Code | Meaning |
|---|---|
| 0 | stopped normally — `Bye`, a closed stdin, or the parent went away |
| 2 | the arguments were unusable, or the frame stream was lost |
| 3 | the backend library is missing or is not whisper.cpp |
| 4 | the weights folder is missing |
| **127** | **not ours** — a library that was loaded could not reach one of its own dependencies |

The shim knows every one of them by number and turns 2, 3 and 4 into a permanent refusal rather than
a retry: no amount of waiting installs a DLL.
