# Speech Broker

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

One microphone for the whole build. The player says something out loud, and Speech Broker decides
**which mod acts on it** - so that five mods do not each raise a recognition engine of their own
and fight over the device.

**What it is not.** It is not a bus of messages between mods, and it is not an arbiter of arbitrary
resources. Everything it hands out is the speech of the player: the auction inside plays for an
**utterance**, not for a hand, not for the camera, not for anything else. Arbitrating something
else means writing another mod, not adding a channel here.

The boundary it owns is the one between the game and a model outside it, and speech is the first
thing to cross it. Four channels are planned along that boundary, and the name of the mod is the
one that is built:

| Channel | What it does | Shape of the exchange |
|---|---|---|
| `Listen` | the recognised speech of the player arrives in the game | a stream, topics, an auction |
| `Speak`  | the game asks for a line to be spoken | a queue with priority and interruption |
| `Ask`    | the game puts a question to a model | request and answer |
| `State`  | the register of the state of the world | publishing and polling |

The first cut implements only `Listen` and `State`, to the extent of the core. The rest is added
without touching what is written.

## Three parts, and each is installed on its own

Speech Broker is **one mod and one git branch**, but inside it there are three parts that stand alone.
Each has its own build, its own lay-out scripts, its own mod in the MO2 build and its own
description. A person installs as many of them as they need.

| Part | Folder | Mod in the build | Description |
|---|---|---|---|
| **Speech Broker** - the bridge | `bridge\` | `Speech Broker` | [bridge/README.md](bridge/README.md) |
| **SpeechBrokerVoiceAdapter** - the microphone and the models | `adapter-voice\` | `Speech Broker - Voice Adapter` | [adapter-voice/README.md](adapter-voice/README.md) |
| **a model mod** - one particular model | `model-whisper-ru\` | `Speech Broker - Voice Model - Whisper RU` | [model-whisper-ru/README.md](model-whisper-ru/README.md) |

Next to them lies `subscribers\demo\` - three test subscribers. That is not part of the delivery
but a check: on them it shows how the bridge settles an argument between mods. They are not
wanted in the working profiles.

### Why it is divided exactly like this

The division follows one mark: **what changes independently**.

- **The bridge** knows nothing about the microphone, the models or HTTP. It deals utterances out
  to the subscribers and settles the arguments between them. It can be replaced whole without
  touching anything else.
- **The adapter** owns the microphone, and it owns it inside the game: capturing the sound, the
  silence and the boundaries of phrases are its business, done in its own process and not behind
  a socket. Into the game it speaks by calling a function through the contract of the bridge. It
  knows not a single model by name.
- **A model mod** is a **shim**: an SKSE plugin like the other two. On one side it speaks the
  adapter's contract; on the other it either IS a model, RAISES one as a process of its own, or
  ATTACHES to one already running - here or on another machine. It declares which of the four it
  does, and a player may forbid a kind in the settings of the adapter.

Two properties follow at once. **A player has nothing to start by hand** - they install the mods
and the game brings everything up. And **changing the model needs neither a new build nor an edit
to the settings**: a person installs a different mod, while the microphone stays one for
everybody.

## Text on screen

Everything a player can read is a key, and the lines behind the keys live in Skyrim's own
translation files - `Interface\Translations\Speech Broker*_<language>.txt`. English is the source
language, and **every language ships inside the module it belongs to**: a translation is a part of
the module and not a mod to install beside it. The engine takes the file that matches the language
of the game.

The log is no exception: its lines are keys too. Exactly one thing stays as it came - **the
recognised speech**. Translating what a person said would be meaningless; it is data, not a
message.

The tables are written as UTF-8 in `localization/` of each module and turned into what the game
reads by `bridge/tools/build-localization.py`, which the bridge publishes in its SDK for exactly
this reason.

## What needs what

    a model mod  ->  needs the adapter
    the adapter  ->  needs the bridge
    a subscriber ->  needs the bridge

They all share one **contract**, and it is not copied between the parts. The bridge puts it into
the `Speech Broker - SDK` package, which is laid out next to the mod: the C ABI header for
adapters and the Papyrus declarations for listeners. The adapter and the subscriber build against
the **installed SDK** rather than against a neighbouring folder in the repository - exactly as any
other mod will. That is why their build refuses to run until the bridge is laid out: it is a
check, not an inconvenience.

The contract of a model mod is its own, and it is published by the adapter:
[adapter-voice/contract/speechbroker-voice-model.md](adapter-voice/contract/speechbroker-voice-model.md).

## Principles

- **Not one constant in the code.** Everything configurable lives in a settings file next to the
  module, and the built-in set of values creates that file on the first launch.
- **No tie to an edition of the game.** The mark is not "this is VR" but "is there a provider for
  the key". A field with no provider answers "unknown" rather than making something up.
- **A key describes the question, not the way it is answered.** `core.target.looked`, not "the
  crosshair".
- **Determinism.** Not one argument is settled by the order in which scripts woke up.
- **The core knows no platform.** The link to SKSE is a layer of its own on top, and that rests on
  the build: the `speechbroker-host` target builds the core without CommonLibSSE.

## The order of building

The bridge comes **first**, always: until its SDK appears in `mods\`, the rest will not build.

```
cd bridge         && cmake --build build --config Release && tools\build-papyrus.ps1 && tools\deploy.ps1 -Apply
cd adapter-voice  && cmake --build build --config Release && tools\deploy.ps1 -Apply
cd model-whisper-ru                                       && tools\deploy.ps1 -Apply
```

Lay out and pack only with the game closed.

## Where to look for what

| Question | Where to look |
|---|---|
| how the auction, the topics and holding work | [bridge/README.md](bridge/README.md) |
| how to check the bridge without the game | [bridge/README.md](bridge/README.md), the `speechbroker-host` target |
| how to write an adapter of your own | `cpp\speechbroker-adapter.h` in the SDK package |
| how to subscribe from a mod in Papyrus | `papyrus\SpeechBroker.psc`, `docs\speechbroker-papyrus.md` in the SDK package |
| how to add a model of your own | [adapter-voice/contract/speechbroker-voice-model.md](adapter-voice/contract/speechbroker-voice-model.md) |
| how a model mod is built inside | [model-whisper-ru/README.md](model-whisper-ru/README.md) |
| how to translate the text into another language | `localization\` of the module, and the section above |
