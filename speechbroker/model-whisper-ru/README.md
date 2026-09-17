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
weights/<id>/      where the weights go, through Git LFS
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

`weights/<id>/`, beside the settings, through **Git LFS** — `.gitattributes` in this folder covers
the model binary formats by kind rather than by size.

**A plain `git clone` must not download them.** GitHub gives 10 GiB of LFS bandwidth a month and
these are about a gigabyte after quantisation; `git clone` was never meant to be the player's
download channel, and what a player installs is the archive `tools/package.ps1` builds. The
`.lfsconfig` at the **root of the repository** excludes this folder, so a clone fetches pointers for
the weights and the bytes for everything else.

Two things about that, and both bite:

- **git-lfs reads `.lfsconfig` from the root of the working tree only.** It is not per-directory the
  way `.gitattributes` is, so the rule this module owns cannot live in this folder - a copy here
  would be read by nobody. It names this weights folder rather than excluding everything, because
  every plugin in the repository goes through LFS too and a clone that fetched a pointer instead of
  a 275-byte `.esp` would lay that pointer into the build and look like a broken plugin.
- **`git lfs pull --include=...` does nothing on its own.** The exclude wins over it and git-lfs
  reports success while fetching nothing. `--exclude=""` on the same command line is what clears it:
  `git lfs pull --exclude="" --include="…/weights/whisper-ru-small"`.

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

**It does not recognise speech yet.** `whisper.cpp` is neither vendored nor built by this repository,
and the child's backend is a **seam with a clean refusal**: it loads the library by full path,
checks that the entry points of whisper.cpp's C API are actually in it — which is what tells a real
`whisper.dll` from a file somebody renamed — and answers a failure naming what is missing. What a
person drops in, and what is left to wire, is in [child/README.md](child/README.md).

Two more things are knowingly left:

- **A dead child is not raised again.** The commonest cause is a runtime that is not installed, which
  no amount of retrying installs, and a relaunch loop would hide it behind a wall of identical
  failures. When the backend is wired and a transient death becomes plausible, the place for it is
  `OnChildGone`, followed by `Ready(handle, 1, NULL)`.
- **`Cancel` does not abort an inference.** The child runs one to the end. The shim drops a cancelled
  request that has not gone down the pipe yet — the cheapest cancelled pass is the one never sent —
  and pays the debt from the reply for one that has. Aborting needs a second thread in the child and
  a backend that can be interrupted, and it buys a saving nobody has measured.

## Building and laying out

The bridge first, then the adapter, then this — the reverse of the dependency. The shim builds
against the **installed** SDK of the adapter, exactly as a third party's model mod would; when that
is not laid out it falls back to the contract in the repository and says so in the configure output.

```
cd child           && cmake -B build -S . && cmake --build build --config Release
cd model-whisper-ru && cmake -B build -S . && cmake --build build --config Release
tools\weights.ps1
tools\deploy.ps1 -Apply
```

Both targets are built at `/W4 /WX`. Lay out and pack only with the game closed.
