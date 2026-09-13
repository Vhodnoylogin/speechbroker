# Envoy - the in-game API

*In Russian: [envoy-papyrus.ru.md](envoy-papyrus.ru.md). English is the source language; the other is a translation.*

Contract version **3** - the same number `Envoy.GetInterfaceVersion()` returns.
The declarations are in `Envoy.psc`.

## Five phases

| Phase | What happens | Who |
|---|---|---|
| `SUBSCRIBE` | a mod declares its topics, its vocabulary and what its action costs | the mod, once |
| `OFFER`     | the event of a topic arrives - an **offer**, not an order | the bridge |
| `INSPECT`   | the mod fetches the details and decides whether to take part | the mod |
| `BID`       | `Envoy.Bid(...)` - or silence | the mod |
| `AWARD`     | the bridge announces the winners | the bridge |
| `SETTLE`    | `Envoy.Done(...)` - a report on how it ended | the mod |

Today the bridge only writes `Done` into the log. Handing the prize to the runner-up on a failure
is intended - the `auction.awardRunnerUpOnFailure` key in the settings - but is not done.

## The signature of a handler is four parameters, and that is not up for discussion

```papyrus
Event OnSomething(string asEventName, string asEmpty, float afNumber, Form akSender)
```

**Three parameters do not work.** The Papyrus machine rejects the call outright and writes
`Incorrect number of arguments passed to function ... Expected 3, got 4 instead!` into the log,
while the handler does not run at all. From the outside that looks like "the events do not arrive",
and finding this cause took three runs in the game.

Of the four parameters exactly one carries meaning - `afNumber`: it holds the number of an
utterance, a request or a job. The string is always empty, because the event only wakes and the
data is read from the bridge by number: the accurate model may refine the text after the broadcast,
and a copy inside the event would part company with what the bridge holds to be true. The sender
the bridge does not fill in.

## The events

| Name | When | The number | What to read |
|---|---|---|---|
| `Envoy_Speech_Dialogue` | the dialogue window is open | the utterance | `GetText`, `GetVocabularyScore` |
| `Envoy_Speech_Menu`     | another menu is open, the game is paused | the utterance | the same |
| `Envoy_Speech_Combat`   | the player is in combat | the utterance | the same |
| `Envoy_Speech_World`    | none of the above | the utterance | the same |
| `Envoy_Speech_Channel`  | an explicit channel fired | the utterance | `GetChannel` |
| `Envoy_Speech_Any`      | every utterance whatever the topic - for observers | the utterance | `GetTopic` |
| `Envoy_Award`           | the lot is played out, there are winners | the utterance | `IsWinner`, `GetWinners` |
| `Envoy_Denied`          | the lot is played out, somebody was refused | the utterance | `GetDenyReason` |
| `Envoy_Settled`         | the lot is played out, whatever the outcome | the utterance | `GetOutcome` |
| `Envoy_Revoked`         | an utterance already handed over was absorbed by a longer one | the utterance | the winner undoes what it did, if it can |
| `Envoy_Ready`           | the bridge is ready, the game is loaded - participants declare themselves again | the contract version | `Subscribe`, `RegisterVocabulary`, `Declare` |
| `Envoy_Ping`            | the self-test of delivery; the carrier quest answers | the token | `Pong` |
| `Envoy_Answer`          | a model answered an `Ask` | the request | `GetAnswer` |
| `Envoy_SpeechDone`      | speaking finished | the speech | `GetSpeechResult` |

The signature is set by SKSE and must not be changed: **the name of the event, one string, one
number**.

An utterance lands in **exactly one** topic. A phrase said outside the dialogue window will never
reach the subscribers of `Envoy_Speech_Dialogue` - that is a cut made by the fact of the state of
the game, not by a guess. The topic is chosen at the moment the utterance is taken in and is not
revisited: an utterance held and let go later goes where it was said.

There is one `Envoy_Award`, one `Envoy_Denied` and one `Envoy_Settled` per outcome rather than per
recipient: there is no name in the event, and each participant asks `IsWinner` or `GetDenyReason`
for itself. The three names are kept for their condition: `Envoy_Award` stays silent when nobody
won, `Envoy_Denied` when nobody was refused, and `Envoy_Settled` always sounds.

## Holding

The recognition engine may report that the phrase most likely **did not end** on this piece -
`GetComplete` less than one. The bridge then works out the cost of a mistake from the room: how
many subscribers of the topic would recognise the phrase and what they declared about themselves
through `Declare`. If the risk is above the tolerance, the utterance is held: the subscribers do
not learn about it until it is clear whether the phrase ended. If the continuation arrives, the
fragment is thrown away and never reaches the subscribers at all. If there is no continuation, the
utterance is announced once the ceiling runs out.

Declaring yourself revocable (`Declare(..., abRevocable = true)`) makes the room cheaper and gets
you the unfinished thing earlier, but obliges you to listen for `Envoy_Revoked` and to be able to
undo what you did.

## The cost classes

    0  reversible  - a mistake is easy to undo (picking a line, opening a menu)
    1  costly      - a mistake is expensive (a spell, a blow, a spent resource)

For `costly` the settings demand a higher confidence and a bigger margin over the second claimant.
Not enough confidence means **nobody acts**: silence is always cheaper than a wrong action. Not
enough margin is a tie, and a tie is settled separately.

## A tie

The bids came together and confidence will not separate the arguers any further: one and the same
vocabulary entry always gives one and the same number, so waiting for somebody to come out ahead
next time is pointless. The bridge settles such an argument with three rules, in order.

1. **The order from the settings.** If the player listed the namespaces in `auction.priority`, the
   senior of the arguers takes the lot alone. A direct instruction from the player outranks any
   reasoning of the bridge about what they meant.
2. **Is the argument about one thing.** The bridge knows which vocabulary phrase each bidder
   recognised - the vocabularies are its own. If the phrases are **different**, the utterance is
   ambiguous: it can be understood two ways and both readings are equally plausible. Then nobody
   acts. That is the case the margin was introduced for.
3. **The willingness to share.** If the phrase is **one and the same**, there is no ambiguity at
   all: the argument is not about what was said but about whose command it is. It is carried out by
   those that declared themselves non-greedy; the greedy drop out by their own condition, "mine
   alone or not at all" - which they set themselves. The rule is switched off by the
   `auction.sharedWinsTie` setting.

If nobody is willing to share, the lot is lost: there is nobody to give way, and the bridge will
not pick at random.

**Every** bidder gets a reason for the refusal: one that passed the threshold gets the reason the
argument ended the way it did, one that did not gets its own, "confidence below the threshold of
its class". An empty answer from `GetDenyReason` means one thing: this participant never bid on
that utterance.

**For the author of a mod one applied rule follows from this:** greed is worth declaring only when
somebody else acting at the same time really does spoil everything. A greedy mod loses every tie
that has at least one non-greedy participant in it.

## Text for the player

Everything a player reads goes through `Envoy.Translate(key)`, and the lines behind the keys live
in `Interface\Translations\Envoy*_<language>.txt` - Skyrim's own format, UTF-16LE, one
`$KEY<tab>text` line each. A mod puts its own file next to ours, names it
`Envoy<Something>_<language>.txt`, and its keys appear in the table.

The engine already resolves a `$`-prefixed string by itself when the whole string is shown as it
is, so `Debug.MessageBox("$MY_KEY")` works without any help. `Envoy.Translate` is for the two cases
the engine cannot handle: a line glued together out of a translated part and a number, and text
that is never shown at all - **the vocabulary you register**, which has to be in the language the
player actually speaks. The demo subscribers take their phrases exactly that way; see
`subscribers/demo` in the repository.

An unknown key comes back as itself, so a forgotten line shows on screen rather than leaving a
blank nobody reports.

## A minimal subscriber

```papyrus
Scriptname MySpellVoice extends Quest

string Property NS = "MySpells" AutoReadOnly

Event OnInit()
    ; A subscription to an event outlives a save - it is set up once.
    ; The topics and the vocabulary live in the memory of the bridge and die with
    ; the process of the game, so the bridge calls everyone to declare themselves
    ; again on every load.
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    Register()
EndEvent

Event OnEnvoyReady(string asEventName, string asEmpty, float afContract, Form akSender)
    Register()
EndEvent

Function Register()
    if !Envoy.IsAvailable()
        return
    endIf
    string[] topics = new string[1]
    topics[0] = "world"
    Envoy.Subscribe(NS, topics)
    Envoy.RegisterVocabulary(NS, GetKnownSpellNames())
    Envoy.Declare(NS, 1, false)   ; a spell is expensive and irreversible
    RegisterForModEvent("Envoy_Speech_World", "OnHeard")
    RegisterForModEvent("Envoy_Award", "OnAward")
EndFunction

Event OnHeard(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    if !Envoy.IsFinal(id)
        return
    endIf
    float mine = Envoy.GetVocabularyScore(id, NS)
    if mine < 0.7
        return
    endIf
    if Envoy.GetStateStatus(id, "core.target.looked") == 1
        Form t = Envoy.GetStateForm(id, "core.target.looked")
        ; ... weigh up the target
    endIf
    Envoy.Bid(id, NS, mine, 1)
EndEvent

Event OnAward(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    if !Envoy.IsWinner(id, NS)
        return
    endIf
    bool ok = CastSpellNamed(Envoy.GetVocabularyMatch(id, NS))
    Envoy.Done(id, NS, ok)
EndEvent
```

## Rules for a subscriber

1. **Tell "no" from "unknown".** `GetStateStatus` gives back `0` when there is nobody to fill the
   key in. Taking that for "no" is a silent mistake, and therefore the worst kind.
2. **Do not compute for long in a handler.** The bid window is measured in tens of milliseconds.
   Every heavy check belongs after `Envoy_Award`.
3. **Do not act in `OFFER`.** Only the winner acts.
4. **Report with `Done`.** Runs are made sense of from the log of the bridge, and handing the prize
   to the runner-up on a failure will rest on that report.
5. **Declare yourself honestly.** A `Declare` with a false revocability buys speed at somebody
   else's expense: the bridge will hand over an unfinished phrase and there will be nobody to undo
   what was done.
6. **Take your vocabulary out of the translation file.** The phrases have to be in the language the
   player speaks, and that is the language of the installed model, not of your source code.
