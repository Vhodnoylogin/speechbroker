# SpeechBrokerVoiceAdapter - the microphone and the models

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

The part of Speech Broker that answers for the voice: it **owns the microphone**, cuts what it hears
into passes, hands every pass to every installed model, settles the argument between their readings
and carries the result to the bridge.

This is a **module of its own**. The sources of the bridge are not here and must not be: the bridge
is visible only through the contract it publishes in the `Speech Broker - SDK` package. Any other
mod that wants to write an adapter sees it the same way - that is the point of the division.

What Speech Broker is as a whole is in the [description of the module](../README.md).

## Everything happens inside the game, and nothing opens a socket

There is no service, no port, no token and no process to start. Until 17.09 this adapter was an HTTP
client to a python service that owned the microphone and loaded the models; the service is gone, and
with it `Service.cpp`, `Listen.cpp`, `Speak.cpp` and the whole of the settings that described them.

The reason is one sentence: **a microphone may have exactly one owner**, and the part of the system
that hears the pause is the part that must decide where a phrase ends. Splitting that across a
process boundary meant the adapter asked a stranger what it had just heard.

## The two halves under it

    src\audio, src\turn     THE EARS      the device, the noise floor, the cutting into passes
    src\models              THE DISPATCH  the registry of models, the deadlines, the arbitration

Neither knows the other's business, and neither knows this mod. Nothing under `src\audio` or
`src\turn` includes the settings, the contract or SKSE, so the whole listening half can be built and
run from a wav with no game at all; nothing under `src\models` includes the settings or the bridge
either. `src\main.cpp` is the only file that knows both, and `src\Config.cpp` the only one that
fills them from a file.

### The ears

One capture device, chosen by name rather than by index - indices are not stable between reboots.
The noise floor is measured once, at the start, and never adjusted: speech raises the level, so a
threshold that followed it would climb after the speaker and stop telling one from the other.

From the stream come **turns** and **passes**. A turn lasts from the block that opened the gate to
the long silence that closes it, and it accumulates sound from zero. A pass is a complete re-reading
of the turn so far, cut on a short pause or on a ceiling, with the trailing silence **already cut
off** - Whisper invents filler over trailing silence with its own silence filter switched on, and
the very first run of this system produced a whole sentence over nothing.

Each pass also carries what only the side with the microphone can know: the anchors, which are the
edges of the pauses measured from energy alone, and the speaker's own pitch range.

### The dispatch

A model is an **SKSE plugin** - a shim - and it registers through the C ABI in
[contract/speechbroker-voice-model.h](contract/speechbroker-voice-model.h). There is no folder of
listings any more and nothing about a model is written in our settings: a model is **installed**,
not configured.

One thread per model, so a shim that misbehaves in one of its calls **starves only itself**. Every
pass becomes a collector with one absolute deadline; a model that has not answered by then is
recorded as a timeout and the pass is sealed without it. A silent model must not be able to stop a
pass for ever - this project has already lost a whole evening of speech to a pass that was never
assembled, and a silent loss is worse than a noisy failure.

**Declared is a hint, measured is a fact.** A model says how quick it is and how long it wants; the
adapter believes that for five passes and then routes by the latency it measured. Straight after a
model starts it is given one second of **digital silence** and asked to recognise it: a model that
answers silence with text is recorded as one that invents, and its voice weighs less when two models
disagree. A model that never answers the probe is never asked for anything.

Two readings of one sound are folded onto **one lane** - the segmentation of a model that carries
word timings, because its boundaries came from an alignment to the audio and are reproducible. Two
models independently arriving at the same string is not two votes but one stronger reason, so equal
texts are merged and their agreement counted.

## What crosses to the bridge

A finished slice: the text, the alternatives the disagreement produced, how sure we are the sentence
**ended**, and the numbers of the pieces this one swallowed. The bridge holds an unfinished phrase
back - but only if it was told, and only the side that heard the pause can tell it.

Our slice numbers and the bridge's utterance numbers are different numbers, and the translation
between them lives in `main.cpp`, because the adapter is the only side that knows both.

**This adapter hears and does not speak.** A `Speak` job is refused at once rather than left without
an answer; speech is a model mod and an adapter of its own, and promising the bridge something we
cannot produce takes the work away from an adapter that can.

## The settings

`speechbroker-voice.json`, beside the mod. Three blocks, and the built-in defaults are the shipping
behaviour, so a key that is missing is not a mistake.

| Block | What it tunes |
|---|---|
| `ears` | the device, the noise floor, the pauses, the tone, where a phrase is judged finished |
| `models.allow` | which kinds of model a player permits. `remote` is off: it is the one kind where the sound of the room leaves the machine |
| `models.dispatch` | one model's life - the attempts at starting it, the probe, what a busy model costs |
| `models.standing` | what was measured against what was declared, and the calibration file behind it |
| `models.arbiter` | how much of a stretch a second model must cover before its text counts as being about it |

The adapter parses its own file **strictly**: a mistake in it is ours, and on one the adapter does
not come up at all.

There is deliberately no sample rate in it. The contract fixes 16 kHz, and changing it would hand
every installed model a buffer it declared it cannot take.

## The text of the adapter

The adapter has no window and no notices: everything it says goes into the log. The log is
translated like everything else in Speech Broker - not one line is written in the code, there are
keys there, and the text lives in
`Interface\Translations\SpeechBrokerVoiceAdapter_<language>.txt`. The tables are kept as UTF-8 in
`localization\`, split into sections by file, and turned into what the game reads by the script the
bridge publishes in its SDK.

What stays as it came is **the recognised speech**: what a person said is data and not a message,
and it reaches the log in the language it was said in.

## Building

```
cmake -B build -S .
cmake --build build --config Release
tools\deploy.ps1 -Apply
tools\package.ps1 -Apply
```

`cmake` refuses to configure until the bridge is laid out: the contract is taken from the SDK
package, and the path to it is in `config/build.json` under `deploy.sdk`. That is a check, not an
inconvenience - an adapter built against a contract that does not exist would silently disagree with
the bridge about the version of the interface.

Lay out and pack only with the game closed.

## Compatibility with the bridge

It has two sides: a bridge **newer** than itself the adapter accepts and works by its own version;
a bridge **older** it refuses. A one-sided rule has already knocked the adapter out entirely once,
and the run did not happen, because speech did not reach the bridge at all.
