# A model mod: Whisper RU

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

Two models for `EnvoyVoiceAdapter` - recognition and synthesis of Russian speech on Whisper: the
accurate `large-v3-turbo` and the draft `small`.

This is a **module of its own and a mod of its own in the build**, and there is not a line of code
in it. A model mod is not a program: it does not listen to the microphone, it opens no ports and it
starts nothing. It carries a **model**: the files of the weights and a listing that says what the
model is called, what it can do and where its files lie.

It is also a sample: anybody can release a model of their own by copying this folder and correcting
one file.

What Envoy is as a whole is in the [description of the module](../README.md).
The contract of a listing is in [adapter-voice/contract/envoy-voice-model.md](../adapter-voice/contract/envoy-voice-model.md).

## Who owns what

    the microphone  ->  the adapter    its service, inside its mod, comes up by itself
    the weights     ->  this mod       the listing and the files of the model
    the text        ->  the bridge     the auction between the subscribers

Hence the order of installing, the reverse of the dependency: the bridge, the adapter, the model.
There is nothing to start separately - the adapter brings its service up itself.

## What is inside

    models/whisper-ru.json        the accurate model: large-v3-turbo
    models/whisper-ru-small.json  the draft one: small, gives a quick answer
    config/build.json             the names and the paths of the lay-out
    tools/deploy.ps1              put it into mods\
    tools/package.ps1             pack it into an archive and install through MO2

Two models in one mod is a convenience, not a duty: "a fast one plus an accurate one" works as two
different mods as well. The draft one gives an answer at once and the accurate one refines it
afterwards; the bridge knows about this and does not count them as two different utterances.

## The weights lie inside their own mod

In a listing the path is **relative**, and it is taken from the folder of the listing:

```json
"weights": "whisper-ru-turbo/large-v3-turbo"
```

The service will refuse to load weights outside the folder of models: neither an absolute path nor
a way out through `..`. A listing is fifteen lines of json, any mod can put one down, and without
this rule a "model mod" would be a way of making the service read anything on the disk.

There is neither an address nor a program in a listing - there is nowhere to write them. All of
that is a property of the service of the adapter, and the adapter settles it.

## The weights on this machine: build.local.json

Here the weights live in another module (the `voice` branch), and there is no point copying
gigabytes into the build. The lay-out puts **junctions** on them according to
`config/build.local.json`, which is not in the repository:

```json
{
  "weights": {
    "whisper-ru-turbo": "D:/.../dev/voice/models/whisper/large-v3-turbo",
    "whisper-ru-small": "D:/.../dev/voice/models/whisper/small"
  }
}
```

The key is the `id` of the model out of its listing; where to put the junction the lay-out takes
from the `weights` key of that same listing. To the service there is no difference: it sees a path
inside the mod.

If the file is missing the lay-out warns, there will be no weights in the mod, and the service will
find no models. When the mod goes out to people the weights lie straight in `models/<id>/` and no
junction is needed.

## There is no text on screen

A model mod shows nothing to the player, and there is nothing in it to translate. The only line
that reaches a human eye is the `name` field of a listing, which the adapter writes into its log;
the log itself is translated on the side of the adapter.

`language` is a different matter: it says which language the model listens in, and a subscriber
that registers a vocabulary has to be in the same one. That is why the test subscribers take their
phrases out of a translation file rather than out of their scripts.

## How to release a model of your own

1. Copy this folder under a name of your own.
2. In `models/<yours>.json` change `id`, `name`, `language`, `provides` and `weights`.
3. Put the files of the model into `models/<id>/` next to the listing.
4. In `config/build.json` change `modName`.

Nothing has to be changed in the adapter: it reads the whole folder and takes what it found. Two
listings with one `id` are a mistake; the adapter takes the first by file name and says so in the
log.

## The lay-out

```
tools\deploy.ps1            show what would be done
tools\deploy.ps1 -Apply     put it into mods\
tools\package.ps1 -Apply    pack it and install through MO2
```

Only with the game closed.
