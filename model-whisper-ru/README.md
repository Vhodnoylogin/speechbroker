# A model mod: Whisper RU

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

Two Russian recognition models for `SpeechBrokerVoiceAdapter` — the accurate `large-v3-turbo` and the
draft `small` — behind one **SKSE plugin**.

**This mod is a program.** It used to be two json listings and not a line of code, because a model
mod used to be a data file the adapter parsed. It is not that any more: a model mod is a **shim**. On
one side it speaks the contract — [`speechbroker-voice-model.h`](../adapter-voice/contract/speechbroker-voice-model.h),
a plain C ABI — and on the other side it **is** a model, **raises** one, or **attaches** to one. Ours
raises one.

It is also a sample. Anybody can release a model of their own by copying this folder; the section
[Releasing a model of your own](#releasing-a-model-of-your-own) says what to change.

What Speech Broker is as a whole is in the [description of the module](../README.md).

## Why a child process, and the number behind it

`SPEECHBROKERVOICE_KIND_CHILD`, declared at registration. The deciding fact is measured, not feared —
on this machine, 17.09.2026:

> Loading `cudnn64_9.dll` with a sub-library unreachable **ends the process with exit code 127**. No
> exception of any kind. Past every handler in the process, past the player's crash logger. No crash
> log at all — the window is simply gone.

The adapter can contain a great deal: a C++ throw out of a model becomes a failure, an access
violation inside a call becomes an ejection. It cannot contain that. Nothing in a shared address
space can, and a CUDA or cuDNN version mismatch is the **commonest** real failure of this stack.

In a child process, 127 is a number `GetExitCodeProcess` returns. The shim writes one line naming it,
fails the utterances outstanding, goes `Ready(handle, 0, ...)` — and the other models carry on. That
is the whole argument, and it is narrow: it is against dragging a **third-party GPU runtime** into
`SkyrimVR.exe`, not against in-process model mods. A self-contained CPU backend removes it entirely,
and `SPEECHBROKERVOICE_KIND_INPROCESS` is the enthusiast's path.

The child is tied to the **process**, never to `Stop`. The contract is explicit that `Stop` may never
be called at all, so two mechanisms hold it: a **Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`**,
whose handle the kernel closes when the game ends by any means at all, and **`--parent-pid`**, which
arms a watchdog inside the child. The job is the guarantee; the watchdog covers the case where
assigning to the job failed. A surviving child would keep the graphics card and — worse in daily use
— keep MO2 believing the game is still running, which means the build cannot be edited.

## What is inside

```
src/core/          the shim, and NOT ONE LINE OF RE/ OR SKSE/ IN IT
    ModelShim      the whole of our side of the contract: Start, Stop, Submit, Cancel, SetVocabulary
    ChildProcess   the job object, the pipes, LoadLibraryW by full path
    Wire           the protocol to the child, shared with the child itself
    Settings       the settings files, and the built-in values that write them on a first run
    Sha256         the weights are what they should be, or the model does not start
    Log            a key and its arguments, rendered for our file and sent raw to the adapter
src/main.cpp       the SKSE plumbing: three entry points and one broadcast caught
child/             a console program of its own, with its own CMakeLists and its own README
docs/child-protocol.md   the wire, precisely enough that somebody could write another child
models/*.json      the settings - PRIVATE to this mod now, see below
localization/      the log, as key<TAB>text, English and Russian
weights/<id>/      where the weights go - fetched, not versioned; SOURCE and SHA256SUMS say how
tools/deploy.ps1   put it into mods\
tools/weights.ps1  verify SHA256SUMS beside the weights, or write them
tools/package.ps1  pack it into an archive and install through MO2
```

The core is free of `RE/` and `SKSE/` on purpose. Everything hard here — the debt of exactly one
`Complete` per accepted utterance, a `Submit` that must never wait, the sentinels that must not be
zero — is testable without a game, and a core that needs `SkyrimVR.exe` to be exercised is a core
exercised by putting on a headset.

## The settings stopped being the interface

`models/whisper-ru.json` is now **this mod's own settings file** and nothing else reads it. That
changes what may be in it: the old listing could carry no address and no program, and that rule was
enforceable because the adapter was the thing parsing it. The contract is the C ABI now, so the
guarantee moved out of a parser and into the open — the shim **declares** its kind, the adapter
writes it into the log in words at every registration, and the player may forbid a kind in the
adapter's settings.

One rule survives, with a new reason. Every path in the settings is **relative to the folder of the
settings file and may not leave it**. Not to stop a fifteen-line json from making a service read
anything on the disk — nobody else parses it — but because the weights, the `SHA256SUMS` beside them
and the archive a player installs are one unit. A path leading out of the mod means the thing that
was verified and the thing that was shipped are not the same thing.

**`provides` is `asr`.** It said `asr,tts` on the turbo listing, and that was false about Whisper:
Whisper does not speak. **Speech synthesis is a model mod of its own** — it did not go missing, it
was always a separate thing, and it needs its own shim, its own weights and its own registration.

The shim **writes these files out of its built-in values the first time it finds none**, so deleting
one is safe, and deleting a single line of one is safe too: a missing field falls back to the value
the code ships with.

## Where the weights go

`weights/<id>/` beside the settings is the **recipe** — `SOURCE` says where each set comes from,
`SHA256SUMS` says which bytes are the right ones, and [weights/README.md](weights/README.md) says the
rest. The **files** are not in this repository at all: they lie in `D:\Skyrim VR Modding\Speech Models\ggml\<id>\`,
in the modding root beside MO2 and SSEEdit, by the same rule as every other thing somebody else
wrote. One key says where — `weightsStore` in `config/build.json` — and one command turns the recipe
back into the files:

```
tools\weights.ps1 -Fetch
```

The format is **GGML** — one `.bin`, whole, the format whisper.cpp reads. Not a CTranslate2
conversion; those are easy to mistake for it, because the big file is called `model.bin` in both.

### Why they are not versioned, stated so it is not re-litigated

They were, for one afternoon, through Git LFS. Two things decided it back:

- **An LFS object cannot be taken back.** Once pushed it stays in the remote's storage after the file
  is gone from every commit; GitHub's own answer is to delete and recreate the repository, or to
  write to support. GitHub's free allowance is ten gibibytes, so two gigabytes is a fifth of it and
  costs nothing today — but it is a fifth that never comes back on its own, and that is enough.
- **A recipe reproduces them exactly.** These are unmodified public releases: the same bytes for
  everybody, fetched by name, with a checksum that proves it. That is the opposite of a plugin or a
  recorded take, which exist nowhere else — and those stay in LFS for exactly that reason.

There is a third thing, quieter and worth knowing: with no LFS rule for `*.bin`, GitHub **refuses**
any file over 100 MB at push time. The mistake costs one rejected push. With the rule, the push
succeeds and the bytes are there for good. The loud free failure is the better one.

### And they are verified before the child is raised

`SHA256SUMS` beside each set, in sha256sum's own format. The shim checks it at `Start` and refuses
the model if it does not match — or, with `requireSums` true, if it is not there at all.

This is measured too: **five hundred bytes flipped inside a converted `model.bin` produce no error of
any kind** — not at load, not at inference — and a model that returns rubbish. Nothing above the shim
can tell that apart from a bad recording or a hard accent, because an answer is text and a score and
rubbish has both. `tools/weights.ps1` verifies the same file, and writes it with `-Write`.

Nothing in this module copies or moves a weight file. `deploy.ps1` puts a **junction** on each set:
one set of bytes, two names, no minutes spent and no second copy for the game to verify while the
original is the one being edited.

## Releasing a model of your own

1. Copy this folder under a name of your own.
2. In `models/<yours>.json` change `id`, `name`, `language`, `class`, `budgetMs` and `weights`.
3. Put the weights into `weights/<id>/` and run `tools\weights.ps1 -Write`.
4. In `config/build.json` change `modName`, and in `CMakeLists.txt` the project and output names.
5. If your model is not Whisper, write another `Recogniser` behind the seam in `child/` — that is one
   interface with three methods — or another child entirely against
   [docs/child-protocol.md](docs/child-protocol.md).

Nothing has to change in the adapter. Two model mods with one `id` are a mistake; the adapter refuses
the second with `DUPLICATE` and says so in its log.

## What this mod does today, and what it does not

**It does:** register both models at the adapter's broadcast, refuse an adapter older than it needs,
verify the weights, raise a child under a job object, take buffers without ever waiting, pay exactly
one `Complete` per accepted utterance, clip and forward the vocabulary, answer `Cancel`, survive a
child that dies, and say all of it through localisation keys in both directions.

**And it recognises speech.** The child calls whisper.cpp's C API out of a DLL it loads by full
path — seventeen entry points, all of them or none, which is also what tells a real `whisper.dll`
from a file somebody renamed. The headers it was built against are vendored and pinned, because
`whisper_full` takes its parameter block by value, and the agreement with the DLL is **measured** at
start-up rather than assumed. What a person drops in — a whisper.cpp release and the weights in GGML
format — is in [child/README.md](child/README.md), along with `--wav`, which runs one take through
the real path without the game.

`whisper.cpp` itself is still neither vendored nor built here, and should not be: it is a release
somebody publishes, its CUDA variant is 675 MB, and a repository that built it would be building a
dependency instead of a mod.

Two more things are knowingly left:

- **A dead child is not raised again.** The commonest cause is a runtime that is not installed, which
  no amount of retrying installs, and a relaunch loop would hide it behind a wall of identical
  failures. When the backend is wired and a transient death becomes plausible, the place for it is
  `OnChildGone`, followed by `Ready(handle, 1, NULL)`.
- **`Cancel` does not abort an inference.** The child runs one to the end. The shim drops a cancelled
  request that has not gone down the pipe yet — the cheapest cancelled pass is the one never sent —
  and pays the debt from the reply for one that has. Aborting needs a second thread in the child and
  a backend that can be interrupted, and it buys a saving nobody has measured.

## From a fresh clone to a working mod

Nothing binary is versioned in this module. A clone is source plus two recipes, and **three things
come from outside** - this table is all of them, with the exact names, because "download whisper.cpp"
is not an instruction anybody can follow twice the same way.

| What | Exactly which file | Size | From |
|---|---|---|---|
| the recognition runtime | `whisper-bin-x64.zip` (processor only) **or** `whisper-cublas-12.4.0-bin-x64.zip` (CUDA 12) | 8.6 MB / 675 MB | [whisper.cpp release **b5130**](https://github.com/ggml-org/whisper.cpp/releases/tag/b5130) |
| the accurate model | `ggml-large-v3-turbo.bin` | 1.62 GB | [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) |
| the fast model | `ggml-small.bin` | 488 MB | the same repository |

**b5130 is the build the headers in `child/vendor/whisper.cpp/` are pinned to.** A different build is
allowed - the backend asks the DLL for its own default parameters at start-up and checks a dozen
fields across the struct - but if the layout has moved it refuses by name rather than answering
rubbish, and then this is the number to come back to.

What is **not** wanted, because it is the one mistake that looks right: a CTranslate2 conversion,
which is what faster-whisper reads. Its big file is also called `model.bin`, it sits beside a
`config.json` and a `vocabulary.json`, and this backend cannot read a byte of it.

### The five steps

1. **The runtime.** Unpack the zip and put `whisper.dll`, `ggml.dll` and every `ggml-*.dll` into
   `child/runtime/` — **the files themselves, flat, not the `Release` folder the zip contains.**
   `tools/deploy.ps1` copies that folder's files next to the child and **does not recurse**, so a
   nested folder is skipped in silence and the child then refuses at start-up naming `whisper.dll`.
   Nothing is renamed and nothing goes on `PATH`. If the folder is absent the mod still lays out
   cleanly: the backend is third-party and is not ours to ship.
2. **The weights.** One command, and no URL is typed by hand:

   ```
   tools\weights.ps1 -Fetch
   ```

   It reads `SOURCE` and `SHA256SUMS` in each `weights/<id>/`, fetches what is missing into the store
   outside the repository, copies the sums in beside the bytes, and then
   verifies everything against the sums. A file already present is left alone. See
   [weights/README.md](weights/README.md) for what else fits there - the quantised models are much
   smaller for very little accuracy.
3. **Build**, the bridge first, then the adapter, then this - the reverse of the dependency. The shim
   builds against the **installed** SDK of the adapter, exactly as a third party's model mod would;
   when that is not laid out it falls back to the contract in the repository and says so in the
   configure output.

   ```
   cd child            && cmake -B build -S . && cmake --build build --config Release
   cd model-whisper-ru && cmake -B build -S . && cmake --build build --config Release
   ```

   Both targets are built at `/W4 /WX`.
4. **Check it before the game**, which costs a second and saves a headset:

   ```
   child\build\Release\SpeechBrokerWhisperChild.exe --weights weights\whisper-ru-turbo ^
       --library child\runtime\whisper.dll --device cpu --language ru --beam-size 5 ^
       --wav ..\tools\audiolab\takes\fireball-ru.wav
       --wav ..	oolsudiolab	akesireball-ru.wav
   ```

   It should print one piece and the words that were spoken. If it refuses, the refusal names the
   file it wanted.
5. **Lay out and pack**, and only with the game closed.

   ```
   tools\deploy.ps1 -Apply
   tools\package.ps1 -Apply
   ```

What a player installs is the archive step 5 builds, with the weights already inside it. `git clone`
was never meant to be a download channel - that is the other half of why the weights are not here.
