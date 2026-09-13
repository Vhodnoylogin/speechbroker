# Envoy Framework - Demo Subscriber

The Envoy test listener: three Papyrus quests showing how a mod takes part in making sense of
speech and how the bridge settles an argument between participants.

This is a **module of its own**. The sources of the bridge and of the adapter are not here: the
declarations of the bridge are taken at compile time out of the `Envoy Framework - SDK` package.
While Envoy lives on one git branch, a module is a folder of its own with a build of its own; a
branch of its own will come later. This module is not wanted in the working profiles of the build -
it exists for checking.

## Three participants

| Script | Topics | Vocabulary | How it bids |
|---|---|---|---|
| `EnvoyDemoObserver` | all, through `Envoy_Speech_Any` | none | takes no part, only shows |
| `EnvoyDemoGreedy` | `world` | the door phrase, the radio-check phrase | greedily: mine alone or not at all |
| `EnvoyDemoShared` | `world`, `dialogue` | the look-around phrase, the radio-check phrase | shares with other non-greedy ones |

The vocabularies deliberately overlap on the **radio-check phrase** - on it the whole rule shows at
once:

- the door phrase is known only to the greedy one, and it takes the utterance for itself;
- the look-around phrase is known only to the sharing one, and it takes it unopposed;
- the radio-check phrase is known to both. If the greedy one won, the sharing one is left with
  nothing; if the sharing one won, the greedy one drops out entirely, because it demanded
  exclusivity itself.

The observer exists precisely so that it shows when an utterance reached **nobody**: a subscriber of
a topic never learns about the utterances of others, while the observer learns about them all.

## The phrases are a translation, not code

The vocabulary is not written into the scripts. Every phrase comes from `Envoy.Translate` by a key,
and the lines behind the keys live in `localization/`, from where they are built into
`Interface\Translations\EnvoyDemo_<language>.txt`.

That is not tidiness. A subscriber registers the phrases the player is going to **say**, and they
have to be in the language of the installed model. English is the source language and ships inside
this mod; Russian is a mod of its own, `Envoy Framework - Demo Subscriber - Russian`, holding a
single folder. Changing the language means switching a mod, not editing a script.

The Russian table holds exactly the three phrases the live recordings were made with, so that the
scenarios in `bridge/tests` and this module say the same words.

## Building

```
tools\build-papyrus.ps1
tools\deploy.ps1 -Apply
tools\package.ps1 -Apply
```

Compilation refuses to run until the bridge is laid out: `Envoy.psc` is taken from the SDK package,
and the path to it is set in `config/build.json`. The lay-out needs the SDK too - the script that
builds the string tables is published there.

## About the encoding

The sources have to be UTF-8 **with a BOM**. Without it the Papyrus compiler reads a file as cp1251
and silently ruins any non-ASCII text. `build-papyrus.ps1` puts the mark in itself before every
build. The scripts themselves are pure ASCII now - everything that is not lives in the translation
tables, which are a different format entirely - but the rule stays, because a comment or a literal
in another language may appear at any time.
