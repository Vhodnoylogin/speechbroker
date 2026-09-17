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

## It fetches nothing and links against nothing, and that is the point

No CommonLibSSE, no SKSE, no json, nothing downloaded at configure time. It is a Windows executable,
the protocol, and a backend loaded from a DLL at run time; the only thing it carries is a folder of
third-party **headers**, `vendor/whisper.cpp/`, pinned and unmodified — see below for why they have
to be the real ones. So it configures and builds in seconds on a machine that has never seen the
game:

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

## What is here

The protocol, the framing, the exit codes, the parent watchdog, the binary-mode pipes, the argument
parsing — and the recognition, which is whisper.cpp's C API resolved by name out of a DLL loaded by
full path. Seventeen entry points, all of them or none: that is also the check that tells a real
whisper build from a file somebody renamed.

### The headers are vendored, and the agreement is measured

`whisper_full` takes a `whisper_full_params` **by value**. That struct is a hundred and sixty-odd
bytes of fields in a fixed order, and a copy of it written from memory rather than from the real
header is a guess whose failure mode is silent: the fields land at the wrong offsets and the model
is asked for something nobody typed. So `vendor/whisper.cpp/` holds `whisper.h` and the four `ggml`
headers it includes, unmodified, at a pinned build. **Headers only** — there is no library to link
and none is looked for.

Pinning is a promise the DLL does not make, so it is checked rather than trusted. At start-up the
backend asks the DLL for its own default parameters and reads a dozen fields spread from the first
byte of the struct to the last — an integer that cannot be out of range, pointers that are null by
default, floats that cannot leave [0, 1]. If the order has moved, at least one of them reads as
nonsense, and the refusal names the build this was made against instead of a model that quietly
answers rubbish.

### `ggml.dll` is loaded too, and this is the trap

`whisper.dll` computes nothing by itself. Since ggml went modular, every compute device — the
processor, CUDA, Vulkan — lives in a `ggml-<name>.dll` of its own and has to be **registered** before
a model is loaded. `whisper.dll` does not do it; `whisper-cli.exe` does it in its own start-up code,
which is why the command-line tool works beside the very same DLLs that leave a program with nothing.

And the failure is the worst shape a failure has: with no device registered, loading the model trips
`GGML_ASSERT(device)` deep inside ggml and **aborts the process** — not an error return, not an
exception. So the child does the registration itself, naming the library's own folder, and then
reads the count back, because *it registered nothing* has to become a sentence.

`ggml.dll` is therefore **not** in `child.preload` and must not be.

### What a person must drop in

1. A build of **whisper.cpp** as a shared library: `whisper.dll`, `ggml.dll` and the `ggml-*.dll`
   files beside them, plus whatever CUDA runtime that build needs. The official
   `whisper-bin-x64.zip` (processor only) and `whisper-cublas-*-bin-x64.zip` (CUDA) both carry
   exactly this. Put them in `child/runtime/` in the source tree — the lay-out copies anything in
   there next to the child — or straight into
   `<mod>/SKSE/Plugins/speechbroker/models/whisper-ru/child/`.
2. The weights in **GGML** format, one `.bin` in `weights/<id>/`. They are not in the repository and
   `tools\weights.ps1 -Fetch` brings them down and verifies them; [the weights README](../weights/README.md)
   says where from and what else works. GGML, not CTranslate2: `model.bin` with a `config.json` and a
   `vocabulary.json` beside it is a faster-whisper conversion and this backend cannot read it.
3. Nothing else. `--library`, `--weights`, `--device`, `--beam-size`, `--threads` and `--language`
   the shim already passes.

## Checking it without the game

```
SpeechBrokerWhisperChild.exe --weights weights\whisper-ru-turbo --library child\whisper.dll ^
    --device cpu --language ru --beam-size 5 --wav some-take.wav
```

`--wav` opens the backend by exactly the path the protocol opens it — the same preload, the same
library, the same layout check, the same weights — runs one 16 kHz mono wav through `Recognise`, and
prints every field of every piece. Nothing in it is a mock.

It exists because the loop was otherwise *lay the mod out, start Skyrim, put on a headset and talk*
for a question that takes a second to answer, and a loop that long is a loop nobody runs — which is
how a backend ends up shipped untested. What is *correct* for a given take is not decided here: that
belongs to `tools/audiolab`, which owns the reference texts. This says what the backend said.

A run on `takes/silence.wav` is worth doing once on any new weights, because it shows the thing the
adapter's silence probe exists for. On this build, `ggml-small` answers five and a half seconds of
silence with two confident lines of invented film credits.

### The sentinels, which are the part that is easy to get wrong

`-1` means *I do not know*, and the adapter degrades to a flat guess and does not punish the model
for it. `0` is a **claim**. `lastWordProb` is the dangerous one: the adapter picks one model's
segmentation as the lane onto which every other model's text is matched, and it picks from among the
models that carry word timings — tested as exactly `lastWordProb` and `medianGapMs`. If the times
come from dividing a segment proportionally by string length, which is what you do when the model
returns no word timings, **both must stay -1**, or this model wins the lane with boundaries that move
between passes. Here they come from `token_timestamps`, which is a real alignment to the audio.

Two more, from the same page:

- `score` is `exp(avg_logprob)` — a probability in [0, 1], higher better. Not normalised, and not
  multiplied by any opinion the model holds of itself.
- `noSpeechProb` is whisper's own, and whisper computes it **once per thirty-second window** rather
  than per segment, so every piece cut out of one window carries the same number. Measured on this
  build it sits around 2e-5 whether the take is speech or pure silence, which is exactly why the
  adapter has a probe of its own rather than trusting this field.
- **Silence must be answered with no fragments at all.** Straight after start-up the adapter submits
  one second of digital silence as an ordinary request, with no flag on it and none possible. A model
  that answers it with text is recorded as one that invents — and this is not hypothetical, as above.
  The backend does not special-case it: what the model says is what is reported.

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
