# EnvoyVoiceAdapter - the sound and the models

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

The part of Envoy that answers for the voice: it **owns the microphone**, deals what it hears out
to the installed models and carries their answers into the bridge.

This is a **module of its own**. The sources of the bridge are not here and must not be: the bridge
is visible only through the contract it publishes in the `Envoy Framework - SDK` package. Any other
mod that wants to write an adapter will see it the same way - that is the point of the division.

What Envoy is as a whole is in the [description of the module](../README.md).

## The microphone belongs to the adapter

Capturing the sound, the silence and the boundaries of phrases are the business of the adapter, not
of the models. It is done by **its own service**: the service rides inside this very mod and comes
up by itself at load. There is nothing for a player to start - they install the adapter like any
other mod.

There is one service per game, and that is not a detail: a microphone is a device, and two models
each bringing a capture of its own would fight over it.

A model in this arrangement is a **recogniser**: it is given sound and it gives back text. It has
no program of its own.

## The adapter knows not one model by name

It reads the folder

    Data\SKSE\Plugins\envoy\adapters\voice\models\

and takes the declarations of the models out of it. Each listing is put there by a **separate mod** -
a model mod; the weights are loaded by the service, and all the adapter wants from a listing is
three things: what the model is called in the answers of the service, whether it gives a draft or a
final answer, and what it can do.
The full contract of a listing: [contract/envoy-voice-model.md](contract/envoy-voice-model.md).

From this follows what it was done for:

- **the adapter can be released to people** - there is not one path, port or language in it that is
  true only on the machine of its author;
- **changing the model needs no new build** and not even an edit to the settings: a person installs
  a different mod;
- **two models at once** are simply two mods. The particular case of "a fast one plus an accurate
  one", where the fast one gives a draft and the accurate one refines it, is arranged by installing,
  not by code;
- **somebody else can release a model of their own** without touching our code at all.

What the adapter declares to the bridge - `asr`, `tts` or both - it **works out** from the installed
models rather than taking it from its settings. Promising the bridge speech when not one model
speaks means taking the work away from an adapter that can do it.

## What the adapter does

- **It knows both sides.** Into the game it speaks by calling a function through the C ABI of the
  bridge, with no sockets. Outward, to the service, it goes over HTTP by itself: the transport is
  the choice of the adapter and the bridge knows nothing about it.
- **It answers for the life of the service.** It checks `/health`; if the service is already up it
  simply connects and never kills a process that is not ours. If it is not, the adapter brings it
  up by `autoStart` out of its settings and tells it the number of the process of the game, so that
  it goes out together with the game.
- **It polls the service.** One thread for the whole game: there is one service, and which model
  recognised an utterance is said in the answer, in the `engine` field. An answer from a draft model
  inside the `correlateMs` window counts as a draft the accurate one will refine.
- **It speaks.** A `Speak` job from the bridge goes to the first model that declared `tts`, or to
  the one named in `speakModel`.

## Talking to the service

| Request | When | Answer |
|---|---|---|
| `GET /health` | before starting and after bringing the service up | `200` if it is alive |
| `GET /listen?since=<number>` | endlessly, while the bridge keeps the adapter as the source | the new utterances from every model |
| `POST /say` | on a job from the bridge | `200` if it was said |

The answer to `/listen` is an object with an `utterances` array, and in every record:

| Key | What it means |
|---|---|
| `id` | the number of the utterance at the service; the adapter translates it into the number at the bridge |
| `text` | what was recognised |
| `engine` | which model recognised it |
| `score`, `margin` | the confidence and the margin over the second hypothesis |
| `ms` | how long it took |
| `complete` | how sure the service is that the phrase **ended**. By that number the bridge decides whether to hold it back or hand it over at once |
| `lengthClass` | short, middle, long |
| `supersedes` | the numbers of the pieces this utterance swallowed |

## The settings

`envoy-voice.json` describes **the adapter itself and its service**: where it is, how to bring it
up, the deadlines, the width of the refinement window. There are no models in it and there must not
be.

| Key | What it means |
|---|---|
| `service.url` | where the service is. Only the loopback is accepted: all the speech of the player goes through it |
| `service.autoStart` | how to bring it up. `exec` only inside the folder of the adapter |
| `service.listenTimeoutSec` | how long the service holds `/listen` before answering empty |
| `speakModel` | who is to speak. Empty means the first speaking one among those installed |
| `correlateMs` | the window in which an accurate answer counts as a refinement of a draft |
| `retryDelayMs` | the pause after a failed `/listen` |
| `healthTimeoutSec` | how long to wait for a connection on `/health` |
| `listenGraceSec` | how much longer than the deadline of the service to wait for its answer |
| `idleSleepMs` | the step of waiting while the bridge keeps the adapter in reserve |
| `sayTimeoutSec` | how long to wait for an answer to `/say` |
| `idMapLimit` | how many recent "number of the service -> number of the bridge" translations to remember per model |

The adapter parses its own settings file **strictly**: a mistake in it is ours, and on one the
adapter does not come up at all. Somebody else listing of a model, on the contrary, is parsed
gently: a listing that does not parse is skipped with a line in the log while the other models
work. A person who installed three model mods must not be left without all three because of one.

## The text of the adapter

The adapter has no window and no notices: everything it says goes into the log. The log is
translated like everything else in Envoy - not one line is written in the code, there are keys
there, and the text lives in `Interface\Translations\EnvoyVoiceAdapter_<language>.txt`. The
reading is done by `envoy-loc.h` out of the SDK of the bridge: the adapter is a library of its own
and writes its first lines before it has met the bridge.

What stays as it came is **the recognised speech**: what a person said is data and not a message,
and it reaches the log in the language it was said in.

The language comes from the `language` key in `envoy-voice.json`; `auto` is the language of the
game itself. English ships inside the mod, every other language as a mod of its own.

## Building

```
cmake -B build -S .
cmake --build build --config Release
tools\deploy.ps1 -Apply
tools\package.ps1 -Apply
```

`cmake` will refuse to configure until the bridge is laid out: the contract is taken from the SDK
package, and the path to it is set in `config/build.json` under the `deploy.sdk` key. That is not
an inconvenience but a check - an adapter built against a contract that does not exist would
silently disagree with the bridge about the version of the interface.

## Talking to the service is protected

The service is ours, but the channel to it is protected all the same, because the port can be taken
by a program that is not ours: the address has to be the loopback, `exec` has to lie inside the
folder of the adapter, and every request carries a one-off session secret in the `X-Envoy-Token`
header. The adapter makes the secret up at load and hands it to the service with the
`--envoy-token` argument.

## Compatibility with the bridge

It has two sides: a bridge **newer** than itself the adapter accepts and works by its own version;
a bridge **older** it refuses. A one-sided rule has already knocked the adapter out entirely once,
and the run did not happen, because speech did not reach the bridge at all.
