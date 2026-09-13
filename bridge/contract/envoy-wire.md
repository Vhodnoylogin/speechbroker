# Envoy - the adapter contract (the first edition, over HTTP)

> **This is history, not the contract in force.** This is how adapters attached in the first
> edition: as a separate program over HTTP. Since version three an adapter is an SKSE plugin just
> like the bridge and talks to it by calling a function in one process; the contract in force is
> `envoy-adapter.h`. The document is kept because the rules in it - who calls whom, exclusivity per
> capability, a broken connection as the normal case - stayed the same and are explained here in
> more detail than in the header. The paths, the ports and the bodies of the requests in it are
> entirely out of date.

Contract version: **1**. Transport: HTTP on `127.0.0.1`, the port from the settings (8932 by
default). The encoding is UTF-8, the bodies of requests and answers are JSON.

## Who calls whom

**Envoy starts nothing.** It knows no external program, keeps no addresses for them and cannot talk
to Whisper, to Piper or to a language model. It holds a port and waits.

Talking to a particular model is done by an **adapter** - a small separate program that knows both
sides: its model and this contract. There can be as many adapters as you like, and Envoy learns
about them only at the moment they come and register themselves.

    model ──► adapter ──► Envoy ──► an event ──► a subscriber mod
    model ◄── adapter ◄── Envoy ◄── a request ◄── a subscriber mod

## The life cycle

1. The game starts and the Envoy plugin raises the server.
2. Envoy starts the programs **listed in its settings**. It does not know what those programs are -
   to it they are strings with paths. The knowledge about models stays in the adapters.
3. An adapter raises its model and calls `POST /v1/register`, declaring what it can do.
4. The adapter holds `GET /v1/pending` and receives jobs from the mods.
5. The adapter puts results in through `POST /v1/utterance`, `/v1/answer`, `/v1/state`.
6. The game closes, the connections break, the adapter sees that and puts its model out.

Point 6 matters: **the connection IS the sign that somebody needs the model.** While Envoy is
unreachable, the microphone and the graphics card are free.

## Registration

    POST /v1/register
    {
      "id": "voice",
      "name": "Whisper + Piper",
      "contract": 1,
      "provides": ["asr", "tts"],
      "languages": ["ru", "en"],
      "engines": [
        { "id": "whisper-large-v3-turbo", "class": "accurate", "typicalLatencyMs": 420 }
      ],
      "voices": ["ru_RU-irina-medium"],
      "stateKeys": [
        { "key": "voice.noiseLevel", "type": "float", "ttlSec": 5.0, "description": "the level of noise" }
      ]
    }

The answer: `{"ok":true,"session":"7f3a","pendingPath":"/v1/pending"}`.

A registration lives for as long as the adapter holds `pending` or confirms itself by registering
again no less often than `adapters.registrationTtlSec`. If it disappears, Envoy counts its
capabilities as unavailable and the keys of its state begin to answer "nobody to ask".

## Exclusivity per capability

The bridge owns neither the microphone nor the sound output - they belong to the models. But the
right to **be the source** it deals out itself, because it is the only one that sees every
participant at once.

The rule: exactly one adapter is active per capability. The choice is whoever is named in the
`adapters.primary` settings, failing that whoever registered first. The rest get
`{"kind":"listen","active":false}` and **are obliged to let go of the device**, not merely to stop
sending data.

Capabilities are counted separately: one adapter can be the source of `asr` and another of `tts`.
An adapter that has left (silent for longer than `registrationTtlSec`) frees its role, and the role
passes to the next one without a restart.

What for: two recognition models that opened the microphone at the same time will cut the stream
into utterances differently and give two versions of one phrase, which the bridge cannot bring
together. Windows permits this - which is why the ban has to be introduced as a rule rather than
relying on the device to protect itself.

A consequence follows: the pairing of "a fast one plus an accurate one" **cannot** be made out of
two adapters. Both engines have to sit behind one owner of the sound, and the adapter sends the
preliminary and the final result under one utterance number.

## What the adapter puts into Envoy

| Request | Body |
|---|---|
| `POST /v1/utterance` | the recognised utterance, see below |
| `POST /v1/answer` | `{"requestId":88,"ok":true,"payload":{...}}` |
| `POST /v1/speech-done` | `{"speechId":57,"ok":true,"interrupted":false}` |
| `POST /v1/state` | `{"keys":{"voice.noiseLevel":0.04},"ttlSec":5.0}` |

### An utterance

    {
      "final": true,
      "text": "fireball",
      "alternatives": [{"text":"fireball","score":0.93},{"text":"fire bolt","score":0.71}],
      "score": 0.93,
      "margin": 0.22,
      "lang": "en",
      "engine": "whisper-large-v3-turbo",
      "latencyMs": 420,
      "durationMs": 900,
      "channel": "",
      "wakeWord": false
    }

The answer: `{"id":10432}` - the number the utterance lives under in the game. The numbering is done
by **Envoy** and not by the adapter: the number has to fit into a Papyrus integer and to be one and
the same for every adapter.

`final: false` is a preliminary result from a fast engine. To refine it later, the adapter sends the
final version with the `"id"` field from the previous answer.

## What Envoy hands to the adapter

| Request | What |
|---|---|
| `GET /v1/pending?id=voice` | an endless stream of jobs, one job per line of JSON |
| `GET /v1/vocabulary?id=voice` | the current merged vocabulary of the subscribers |
| `GET /v1/status` | who is registered, who is subscribed, what is going on |

The jobs in the stream:

    {"kind":"vocabulary","phrases":["fireball","ice spike"]}
    {"kind":"speak","speechId":57,"text":"...","voice":"ru_RU-irina-medium","priority":10}
    {"kind":"stop","speechId":57}
    {"kind":"ask","requestId":88,"service":"llm","payload":{...}}
    {"kind":"listen","active":false,"reason":"menu"}

`listen` is a request to fall silent or to listen again: the game is paused, a menu is open, a load
is under way.

## The rules

1. **Envoy knows no models.** There must not be a single name of a program, a port or a model format
   in its code. All of that lives in the adapter and in the settings.
2. **The content of `payload` is opaque.** Envoy does not parse it - otherwise every new model would
   mean an update to the plugin.
3. **Nothing blocks the game.** `speak` and `ask` get a number at once and the result comes later.
4. **No files as a channel.** No logs, no `jsonl` - only these requests.
5. **A broken connection is the normal case.** An adapter reconnects by itself; missed jobs are not
   restored, because a stale utterance is of no use.
