# The contract of a model mod

This document describes what a mod has to put into the build if it adds one more model of speech
recognition or synthesis to the `EnvoyVoiceAdapter` adapter.

## Who owns what

    the microphone  ->  the adapter        (the service inside its mod)
    the weights     ->  the model mod      (the listing and the files of the model)
    the text        ->  the bridge         (the auction between the subscribers)

**The microphone belongs to the adapter.** There is one per game: capturing the sound, the silence
and the boundaries of phrases are its business, and it is done by the service that rides inside the
mod of the adapter and comes up by itself. There is nothing for a player to start.

**A model mod is not a program.** It listens to no device, opens no ports and starts nothing. It
brings a model along: the files of the weights and a listing that says what the model is called,
what it can do and where its files lie. The sound is given to it by the service, and the result
comes back the same way.

Hence the main thing: **installing a second model means installing a second mod.** Neither the
adapter nor its settings are touched. The particular case of "a fast one plus an accurate one",
which the adapter exists for, is simply two installed mods.

## Where to put the listing

    Data\SKSE\Plugins\envoy\adapters\voice\models\<anything>.json

The name of the file plays no part - what matters is the `id` key inside it. Mod Organizer merges
the folders of mods into one, so every model mod puts its own file there and they do not collide.
The folder is read **in the order of the file names**: two launches give one and the same list.

The files of the model itself the mod puts next to it, in **its own** subfolder:

    Data\SKSE\Plugins\envoy\adapters\voice\models\<id>\...

## The fields of a listing

| Key | Required | What it means |
|---|---|---|
| `id` | yes | a short name without spaces. The service signs its answers with it, and the model is known by it in the log and in `speakModel` |
| `name` | no | a human name for the log; defaults to `id` |
| `enabled` | no | `false` - the listing is there but the model is not loaded. `false` by default |
| `class` | no | `fast` - the model gives a draft the accurate one refines afterwards. Otherwise a final answer |
| `provides` | no | what it can do: `asr` - recognises, `tts` - speaks, `asr,tts` - both. `asr` by default |
| `language` | no | the language the model listens in |
| `weights` | yes | the path to the files of the model, **relative to the folder of the listing** |
| `device` | no | `cuda` or `cpu` - what to compute on; the service is entitled to disregard it if there is no such thing |
| `budgetMs` | no | how long the model is given for a piece before its answer is no longer waited for |

An example:

```json
{
  "id": "whisper-ru-turbo",
  "name": "Whisper large-v3-turbo, Russian",
  "enabled": true,
  "class": "accurate",
  "provides": "asr,tts",
  "language": "ru",
  "weights": "whisper-ru-turbo/large-v3-turbo",
  "device": "cuda"
}
```

## Two rules of safety

A listing is fifteen lines of json, and any mod can put one into the game. So there are exactly two
rules, and both are checked rather than assumed.

**1. The weights lie inside their own mod.** `weights` is a relative path and it is taken from the
folder of the listing. An absolute path and any way out of the `models\` folder - through `..`,
through a drive letter, through a network name - is a refusal to load the model. A model mod is
entitled to point only at what it brought with it.

**2. There is neither an address nor a program in a listing.** A model mod names no host, no port
and no executable - there is simply nowhere to write them. All of that is a property of the service
of the adapter, and the adapter settles it. A listing used to be able to do both, and that was a
mistake: somebody else mod could name an address on the internet and receive all the speech of the
player, or start any program on their machine.

The conversation between the adapter and its service is protected separately: the address has to be
the loopback, the service is started only out of the folder of the adapter, and every request
carries a one-off session secret in the `X-Envoy-Token` header. None of that concerns a model mod.

## Text on screen

A model mod shows nothing to the player, so it has nothing to translate. The only line of it that
reaches a human eye is `name`, which the adapter writes into its log - and the log stays English,
like every other log in Envoy.

`language` is a different matter: it says which language the model listens in, and a subscriber
that registers a vocabulary has to be in the same one. That is why the demo subscribers take their
phrases out of a translation file rather than out of their scripts.

## What happens on a mistake

A mistake in somebody else listing does not cancel the rest. A file that does not parse, a listing
with no `id`, a second listing with the same `id`, weights outside their own mod - each of these is
a line in the log and one model skipped. A person who installed three model mods must not be left
without all three because of one.

Its own settings file (`envoy-voice.json`) the adapter parses strictly, on the contrary, and on a
mistake it does not come up at all: a mistake there is ours, and there is no point hiding it.

## An example

A finished and working example is the `model-whisper-ru` model mod in this same tree.
