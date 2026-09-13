# Envoy - the bridge

The heart of Envoy: an SKSE plugin that takes recognised speech from the adapters, picks a topic
for it, plays it out between the mods that subscribed and sends an event to the winner. About the
microphone, the models and HTTP it knows nothing - that is the business of the adapter.

What Envoy is as a whole and how the parts relate is in the [description of the module](../README.md).

## What the bridge does

| Duty | Where |
|---|---|
| take an utterance from an adapter and file it under a number | `src/wire/AdapterHost`, `src/bus/UtteranceStore` |
| pick a topic: dialogue, menu, combat, world | `src/bus/TopicRouter` |
| decide whether to hold an unfinished phrase back | `src/bus/Hold` |
| gather the bids and name the winner | `src/bus/Auction` |
| wake the winner with a Papyrus event | `src/game/ModEventBus` |
| keep the register of the state of the world | `src/bus/StateStore` |

**An event is a doorbell.** It wakes a mod up and carries only the number of an utterance;
everything else the mod fetches with the `Envoy.*` functions by that number. There is no answering
from inside an event, and that is deliberate: a payload inside the event would force the bridge to
guess what exactly each subscriber is going to need.

## Two layers: the core and SKSE

The bridge is split in two, and the split is held by the build rather than by a promise.

**The core** is `src/core` and `src/bus`: the auction, the store of utterances, the choice of
topic, the vocabularies, the settings, the log and the text on screen. About the game it knows
nothing - not one include of `<SKSE/…>` or `<RE/…>`. Everything it needs from the game it asks for
through three seams:

| Seam | The question of the core | The answer in the game | The answer without the game |
|---|---|---|---|
| `core/MainThread` | where to do this work | the task interface of SKSE | a queue of its own, drained by the main thread |
| `core/GameState` | is a menu open, is combat on | `RE::UI`, `RE::PlayerCharacter` | nothing is happening |
| `core/Events` | send this event to the subscribers | a Papyrus broadcast | a line in the log |

With no seams set the core answers itself, and these are not stopgaps for want of anything better:
outside the game the menus really are closed, and work taken from its own queue by the main thread
gives a strictly defined order - a check stops depending on when somebody else thread woke up.

**The SKSE layer** is `src/game` and `src/wire` plus `src/main.cpp`: the Papyrus functions, the
handing out of the interface to adapters, watching for a game load, and `game/SkseHost.cpp`, which
answers those same three questions by the means of Skyrim.

The rule is guarded by the `envoy-host` target out of `tests/`: it builds the core **without**
CommonLibSSE. Let an include from the game appear in the core and the host will not build, and the
breach shows at once rather than in six months.

## Text on screen

Every line a player can read is a key, and the lines behind the keys are read out of
`Interface\Translations\Envoy*_<language>.txt` - Skyrim's own format. The table is written by hand
as UTF-8 in `localization/` and turned into what the game reads by `tools/build-localization.py`;
the same script writes `src/core/LocStrings.h`, the baseline compiled into the plugin, so a missing
file shows text rather than bare keys.

English ships inside the mod. Every other language is a mod of its own holding a single
`Interface\Translations` folder, so adding a language means adding a mod and nothing else.

Two things are deliberately **not** translated. The log stays English: its lines travel into other
people's bug reports. And the five level names in the menu - `trace`, `debug`, `info`, `warning`,
`error` - stay as they are, because they are the values of the `log.level` key and a person reading
the window has to be able to type what they see into the settings file.

A subscriber gets at the same table through `Envoy.Translate`. It is wanted for two things the
engine cannot do by itself: a line glued together out of a translated part and a number, and text
that is never shown at all - the vocabulary a subscriber registers, which has to be in the language
the player actually speaks.

## Running the auction without the game

    build\tests\Release\envoy-host.exe
    envoy-host.exe --scenario tests\scenarios\default.json --report run.txt
    envoy-host.exe --config %TEMP%\priority.json    with a set order of participants

The host takes its settings from `envoy-host.json` next to itself, and `--config` replaces the
file. `tests/envoy-host.json` is a blank with a non-empty `auction.priority`: on it the **first
step** of the tie-breaking runs, which on the built-in settings says nothing. It has to be run as a
**copy outside the repository**: Config writes the missing keys back into the file, and the blank
would grow into the full set.

The host raises the same core, declares the **test subscribers** out of `tests/subscribers/*.json`
and plays a scenario out of `tests/scenarios/`. All that is wanted from a subscriber is what it
declared - its topics and its vocabulary; what it does with a win does not concern the check. The
question is exactly one: **who the bridge gave the utterance to, and why**.

The host bids on behalf of a subscriber itself, as the product of two quantities: how well the
utterance was heard and how close it is to the declared phrase. A real subscriber works its
confidence out however it likes - here the simplest defensible rule is taken.

**The report of the host is the safety net of the whole rework.** A run is taken before a change
and after it, and the reports have to match word for word; if they diverge silently, then some
behaviour changed that nobody declared.

The spoken phrases in `tests/scenarios/` and `tests/subscribers/` stay in Russian on purpose. They
are speech, not interface: they are what was said into a microphone in the live recordings, and
they are the only cover the Cyrillic case folding in `core/Text` has. One English vocabulary,
`spells-en.json`, stands among them precisely so that it shows the bridge measures both by one
yardstick without asking about the language.

## The layout

    contract/       the public contract: the ABI, the in-game API, the space of keys
    config/         the reference settings and config/build.json
    localization/   the string tables, UTF-8, one per language
    src/core/       the core: settings, log, scheduler, text, the seams to the game
    src/bus/        the core: utterances, subscriptions, topics, the auction
    src/game/       the SKSE layer: Papyrus, events, the state of the game
    src/wire/       the SKSE layer: handing the interface out to adapters
    providers/      the state providers
    papyrus/        Envoy.psc - the declarations for listeners, and the script of the carrier quest
    esp/            the carrier quest
    tests/          running the auction without the game
    docs/           descriptions, including a sketch of a universal adapter
    tools/          building, laying out into mods\ and checking the contract

## Building

    cmake --build build --config Release
    tools\build-papyrus.ps1
    tools\deploy.ps1 -Apply
    tools\package.ps1 -Apply

Lay out and pack only with the game closed. The bridge is laid out **first**: until its SDK package
appears in `mods\`, neither the adapter nor the subscriber will build.

## What the bridge publishes for other mods

The `Envoy Framework - SDK` package, laid out next to the mod:

| File | For whom |
|---|---|
| `cpp/envoy-adapter.h` | authors of adapters: the C ABI, the contract version, the structures of utterances |
| `cpp/envoy-abi.h` | authors of state providers |
| `papyrus/Envoy.psc` | authors of subscribers: the Papyrus declarations |
| `docs/envoy-papyrus.md` | the same people: every call and every event taken apart |
| `docs/envoy-keys.md` | the space of state keys |
| `localization/envoy.*.txt` | translators: the source tables of the bridge |
| `tools/build-localization.py` | anybody: turning a table into what the game reads |

The contract version is taken **from the binary** (`EnvoyAPI::kInterfaceVersion`) and not from the
settings: a third number that can be edited in a file has already parted company with the truth
once. The compatibility rule has two sides - an adapter accepts a bridge newer than itself and
refuses to work with one older.
