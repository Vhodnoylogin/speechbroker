# The contract of a model mod

*In Russian: [speechbroker-voice-model.ru.md](speechbroker-voice-model.ru.md). English is the source language; the other is a translation.*

This document describes what a mod has to do if it adds one more model of speech recognition to the
`SpeechBrokerVoiceAdapter` adapter. The rules are here; the exact declarations, every field and
every refusal are in [speechbroker-voice-model.h](speechbroker-voice-model.h), which is the contract
itself and is published beside this file.

## Who owns what

    the microphone  ->  the adapter    (one per game, and nobody else touches it)
    the weights     ->  the model mod  (the files, and the code that brings them up)
    the text        ->  the bridge     (the auction between the subscribers)

**The microphone belongs to the adapter.** Capturing the sound, finding the silence and deciding
where an utterance begins and ends are its business alone. A model is never asked to listen.

**A model mod is a program.** It is an ordinary SKSE plugin, loaded into the same process of the
game as the adapter, and that is why the two talk by calling a function rather than over a network.
It is made of two halves: a thin shim that speaks this contract, and the model itself. The model may
be anything at all — a library linked into this very process, a child process the shim starts, a
server on another machine. The adapter is never told which, beyond one declared word, and never
cares. The shim knows exactly two things: **how to bring its model up, and how to hand it the
sound.**

**A model never speaks to the bridge.** It answers the adapter, and the adapter carries the text on.
There is no way to reach the bridge from this contract and no mention of it in the header.

Hence the thing that has not changed: **installing a second model means installing a second mod.**
Neither the adapter nor its settings are touched. "A fast one plus an accurate one", which the
adapter exists for, is simply two installed mods.

## How a model mod finds the adapter

Four facts, and none of them is guessable — they are written out in full at
`SPEECHBROKERVOICE_MESSAGE_HOST` in the header:

1. The adapter **broadcasts** an SKSE message of type `0x564D444C` from the plugin named
   `SpeechBrokerVoiceAdapter`.
2. The payload is **a pointer to the pointer** — `dataLen == sizeof(void*)`, dereference once. That
   is the house convention, the same one the bridge uses towards its own adapters, and a shim that
   copies the adapter's own line of code is right.
3. The moment is SKSE's **`kDataLoaded`**, which is late on purpose: by then every model plugin has
   certainly loaded and had its chance to subscribe. Register your messaging listener in
   `SKSEPlugin_Load` — it is the only place early enough.
4. There is **no second broadcast** and no entry point to ask for the table afterwards. A model that
   was not listening is simply never asked for anything, and recognition carries on with whatever
   else is installed.

From the table the shim calls `Register` once, handing over three things: what it is
(`SpeechBrokerVoiceModelInfo`), what it can do (`SpeechBrokerVoiceModel` — a table of function
pointers), and a buffer for the adapter to fill (`SpeechBrokerVoiceSession`).

`Register` runs on the thread of the game, during plugin load, and does nothing slow. **Do not bring
your model up there.** `Start` comes later, on a worker of the adapter, and may take as long as it
honestly needs.

## The shape of the handoff: a whole utterance

The adapter listens, decides where speech ended, cuts the trailing silence off itself and only then
offers a finished buffer **from sample zero**. There is no streaming and no delta. A model re-reads
all of it and gives back its own full segmentation of it.

That is not an aesthetic choice. It is what makes any two answers comparable — with each other and
with what has already been sent onward. The question "did it give me B, or A and B" must not be
askable, or the adapter cannot reconcile two models at all.

Within one turn the adapter may ask several times, each pass with its own `utteranceId` and a buffer
that **ends no earlier** than the one before it. Only the last carries `final == 1`. Two passes of
one turn may legitimately be byte-identical — the silence that triggers the last pass is the longest
one, and it is trimmed away again — so answering them identically is correct, and nothing should
test a pass for being *longer* than its predecessor.

What crosses is `float32`, 16 kHz, mono, and the buffer is valid for the length of the `Submit` call
and not one instruction longer. **Copy it inside the call.** What comes back is a set of fragments,
each with its text and its place in the buffer, delivered through `Complete` from a thread of your
own. You owe exactly one `Complete` for every `Submit` that returned `OK`.

A remote model should say so and declare `finalOnly`: every pass re-sends the whole buffer from zero,
which is beneath notice inside this process and the dominant cost across a network. The header does
that arithmetic in bytes and does not hide the answer.

## Nothing here ever touches the thread of the game

Except `Register`, which is deliberately trivial. Not `Start`, not `Submit`, not `Complete`, not
`Stop`, not `Unregister`, not `Log`. A model mod that blocks the game thread costs a person frames
in a headset, and there is no forgiving that.

## The two rules of safety, and why one of them had to change

**1. The weights lie inside their own mod.** A model mod resolves its files relative to its own DLL
and points at nothing it did not bring with it. That rule is unchanged in substance; what changed is
its enforcer. It used to be a parse rule over a path in a JSON file, checked by the adapter. A shim
is a program, so nobody can check its file names for it — the rule now rests on the shim, and the
adapter states it here so that a model mod which breaks it does so knowingly.

**2. A model mod declares what kind of thing it is, and the player decides.** The old contract
forbade a listing to name an address or an executable, because a fifteen-line JSON file from
somebody else's mod could otherwise have sent all of a player's speech to the internet. That
guarantee cannot survive as a parse rule now: a model mod **is** an executable, and it can open any
socket it likes without telling anyone. So the guarantee moves to where it can still be kept:

- `SpeechBrokerVoiceModelInfo::kind` declares `INPROCESS`, `CHILD` or `REMOTE`. It is **declared and
  unverifiable** — the header says so in those words rather than pretending otherwise.
- The adapter **gates registration on it**, against the player's own setting, before `Start` and
  before any transport of the model is opened.
- The adapter **writes it in the log, in words**, for every model that registers.

A mod that lies about its kind is doing something a mod can always do, and no contract prevents it.
What this contract does prevent is a model quietly shipping speech off the machine while everything
looks ordinary — the declaration makes the honest case visible and the dishonest case a lie somebody
can be held to.

## One broken model must not cost a person the others

Every refusal in this contract is said out loud and costs nothing but the model that caused it. A
version that does not meet, an id already taken, a NULL function pointer, a malformed struct, a
model forbidden by the player's setting — each is one line in the log naming the model, and the
others carry on untouched. A `Start` that never returns is not timed out and not killed: the adapter
waits on that model's own dispatch thread, where nothing else waits, and says so once.

A person who installed three model mods must not be left without all three because of one.

The adapter parses its **own** settings file strictly, on the contrary, and on a mistake it does not
come up at all: a mistake there is ours, and there is no point hiding it.

## Where the files go

The DLL of a model mod goes where every SKSE plugin goes:

    Data\SKSE\Plugins\<YourModelMod>.dll

Its weights and its own settings go into **its own** folder, resolved relative to that DLL:

    Data\SKSE\Plugins\speechbroker\models\<id>\...

Mod Organizer merges the folders of mods into one, so every model mod owns its own subfolder and
they do not collide. There is no shared registry file any more and nothing to merge: a model mod is
known to the adapter because it registered, not because it left a note somewhere.

## There is no text on screen

A model mod shows nothing to a player, so it has nothing to translate. The only line of it a human
eye reaches is `name`, written into the log of the adapter — and `Host::Log` takes a **localisation
key of your own** plus its arguments, never a formed sentence, so your own log lines are translated
on your side by the ordinary means.

`language` says which language the model listens in. **At version 1 the adapter logs it and routes
nothing on it:** it does not exclude a model from a pass and does not weigh it down for a mismatch.
That is said plainly here and in the header, because a shim author who believes it gates routing
will spend real time on it for nothing.

## An example

There is not a working one yet, and saying otherwise would be the most expensive kind of mistake in
a document like this. `model-whisper-ru` in this same tree is today a pair of JSON listings with no
code — the shape this contract replaces. It becomes the worked example when it becomes a program,
and this line changes on that day.
