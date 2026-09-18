# The architecture of Speech Broker

This page is the record of how the mod is divided. It is written from the author's own statement of
it, and everything else - folders, mod names, contracts, the order of the work - follows from here
rather than the other way round.

The mod is divided **twice**, and the two divisions do not coincide. Confusing them is the mistake
this page exists to prevent: a "module" in the logical sense is not obliged to be a mod of its own,
while every implementation of the market is.

- **The physical division** is what a person downloads as an archive and ticks on in Mod Organizer.
- **The logical division** is the parts inside Speech Broker itself.

## The physical division: four kinds of mod

| Kind | Who writes it | Where it is today |
|---|---|---|
| **Consumer mods** | other modders - and they are the reason the whole thing is being built | `subscribers\demo\` - three test ones |
| **Model mods** | third parties; one of ours exists for the tests and for people who cannot be bothered | `model-whisper-ru\` |
| **Speech Broker itself** | us | `bridge\` and `adapter-voice\` |
| **Markets** | us and third parties | does not exist yet as a part - the auction sits inside the bridge |

A consumer mod and a model mod are the two ends of the thing. Everything between them is ours, and
everything between them is what the logical division cuts up.

## The logical division: five parts of the broker

| Part | What it owns | State today |
|---|---|---|
| **microphone engine** | the microphone, the raw sound, the model mods and the single packet of text made out of their answers | written - `adapter-voice\`, to be renamed |
| **sound text market** | an **interface**: it takes the packet of text and hands it to the mods that subscribed | written as part of the bridge, to be split into interface and implementations |
| **sound producer** | requests for sound made out of text, and the file that comes back | **a stub** - intended from the start, not written |
| **free text producer** | text between a subscriber and a model that the broker does not read | **a stub** - intended from the start, not written |
| **world state** | readings of the state of the world, attached to the packet | **a stub** - one collector, to prove the channel |

### 1. microphone engine

Owns the microphone and the sound it makes. It knows which model mods are present in the game,
hands each of them the raw sound, takes back their packets of recognised text and forms **one**
packet out of them, which it passes on. It knows no consumer and no market.

This is what is called `adapter-voice` today. The name is wrong in the same way "adapter" is always
wrong: it says what the code does to the neighbour rather than what it owns.

### 2. sound text market

Takes the finished packet of text and gives it to the mods that want it. It is **only an
interface**. There can be many implementations of it, and **each implementation is a module of its
own and a mod of its own**, with **events of its own** - a subscriber talks to the market it chose,
not to a common channel.

| Implementation | What decides who gets the text | Whose |
|---|---|---|
| the open market | everyone who asked gets it | ours, the simplest one |
| the dictionary market | a subscriber gets the text only if the text is in that subscriber's dictionary | ours |
| the auction | subscribers bid for the utterance and one of them takes it | ours, already written, to be moved out of the bridge |
| anything else | whatever its author decided | third parties, and we will never write them |

The last row is the reason the interface exists at all. A third-party market plugs in as an ordinary
mod, publishes its own events and decides for itself who deserves the text.

### 3. sound producer

Takes requests from subscriber mods for sound to be made out of text, passes the text to a model
mod, takes the sound back, makes a file of it and answers the subscriber with the file to use.
Intended from the very beginning and not written yet, so what stands in the code is a stub which
says so.

### 4. free text producer

A free conversation between subscriber mods and models that speak in text. The broker **does not
read** that text. It reads only the meta of the packet: who asked, whom they asked, and who is to
get the answer. Also intended from the beginning, also a stub for now.

### 5. world state

The ability to read various state of the world and **put it into the packet** the subscriber
receives, so that a mod can decide by the state the world was in **at the moment the line was
spoken**. That is the whole point of attaching it to the packet rather than letting a mod ask
afterwards: by the time a mod acts, the moment is gone.

This part is **multi-instance and non-competing**, and that is what separates it from the market:

|  | market | world state |
|---|---|---|
| how many take part | many subscribe, **one** is chosen | **every** collector contributes |
| what the decision is | who gets the utterance | nothing is decided |
| who reads the result | the winner | any mod, and only the part of it that it wants |

Each collector gathers its own reading and puts it into the packet. Nobody arbitrates between them
and nothing is awarded. A mod inside the market then decides whether to read the state at all and
which part of it to read.

**The first collector is deliberately a stub.** It answers `game`, `dialogue` or `paused` - which is
logically pointless, because a subscriber already knows that much, and technically it is the whole
proof that the channel works end to end: a reading taken at the moment of the utterance, carried in
the packet, read by a mod that asked for it.

### The naming of the fifth part

The author asked for it without a name. `world state` is proposed, with each instance called a
**state collector**, because the two names it might otherwise take are both already spoken for and
both would mislead: it is not a *market* - nobody competes and nothing is handed to a winner - and
it is not a *producer* - nothing is made on request, a collector reads what is already there.
`collector` is the author's own word for the instances.

## What this changes in what is written

| Logical part | Where the code is today | What happens to it |
|---|---|---|
| microphone engine | `adapter-voice\` | renamed - folder, mod name in the build, DLL, lay-out paths |
| sound text market - the interface | `bridge\src\bus\` - `TopicRouter`, `SubscriptionRegistry`, `UtteranceStore` | stays in the bridge, becomes the interface the implementations plug into |
| the auction | `bridge\src\bus\Auction.cpp`, `Hold.cpp` | moves out into a module and a mod of its own |
| the open market | nothing | new, the simplest implementation |
| sound producer | nothing | a stub, visible in the code |
| free text producer | nothing | a stub, visible in the code |
| world state | `bridge\src\bus\StateStore.cpp` | keeps the store, gains collectors and the packet |

The rename and the moving out of the auction are **one action**, by the author's instruction, and
not two moves across the same files.

There is no separate entity called "the bridge" in this division. `bridge\` is the physical home of
the market interface, the two producers and the world state - not a sixth part.
