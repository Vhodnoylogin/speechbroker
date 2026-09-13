Scriptname EnvoyDemoObserver extends Quest
{The observer: it sees EVERY utterance whatever the topic and takes no part in the draw.

 It exists precisely so that it shows when an utterance did not reach somebody: a
 subscriber of a topic never learns about the utterances of others, while the
 observer learns about them all.

 The response is deliberately overblown. In a headset a notice in the corner is
 small and does not stay long, so "did not notice" and "did not work" looked the
 same and watching proved nothing. The decisive moments are now shown with a
 dialogue that cannot be missed: connecting to the bridge, and the first utterance
 heard after a load. After that come ordinary notices, or the dialogue would have
 to be closed on every phrase.

 Every line the player reads comes from Envoy.Translate, so it is in the language
 the game runs in. The tables live in localization/ and ship as
 Interface\Translations\EnvoyDemo_<language>.txt.}

bool heardOnce = false

Event OnInit()
    ; A subscription to an event outlives a save, so it is set up once. Everything
    ; else lives only in the memory of the bridge and dies with the process of the
    ; game, which is why the bridge calls everyone to declare themselves again on
    ; every load.
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    Register()
EndEvent

Event OnEnvoyReady(string asEventName, string asUnused, float afContract, Form akSender)
    Register()
EndEvent

Function Register()
    heardOnce = false
    if !Envoy.IsAvailable()
        ; These two are the only lines here written out rather than taken from a
        ; key, and for two different reasons. The trace cannot go through
        ; Envoy.Translate, because the plugin that would translate it is exactly
        ; what did not load. The dialogue can: the engine resolves a $-string
        ; shown whole out of the translation file by itself, with no plugin.
        Debug.Trace("[Envoy] no bridge: the plugin did not load")
        Debug.MessageBox("$ENVOYDEMO_NO_BRIDGE")
        return
    endIf
    RegisterForModEvent("Envoy_Speech_Any", "OnAny")
    RegisterForModEvent("Envoy_Settled", "OnSettled")
    ; The observer does nothing and therefore declares itself revocable: it has
    ; nothing to undo. The call is here for the check itself - that a new function
    ; of the contract reaches a script and does not bring it down.
    Envoy.Declare("DemoObserver", 0, true)

    ; A dialogue is obliged to report what is not known. The contract version was
    ; known in advance and raised no doubt; what is not known is something else -
    ; whether anybody declared themselves to the bridge and on which topics. One
    ; glance now separates "there are no participants" from "there are
    ; participants, but the events do not reach them".
    string[] who = Envoy.GetNamespaces()
    string[] mine
    string list = ""
    string joined = ""
    int i = 0
    int j = 0
    while i < who.Length
        mine = Envoy.GetTopicsOf(who[i])
        joined = ""
        j = 0
        while j < mine.Length
            if j > 0
                joined = joined + ", "
            endIf
            joined = joined + mine[j]
            j = j + 1
        endWhile
        list = list + "\n  " + who[i] + ": " + joined
        i = i + 1
    endWhile
    if who.Length == 0
        list = "\n  " + Envoy.Translate("$ENVOYDEMO_NOBODY")
    endIf

    string hello = Envoy.Translate("$ENVOYDEMO_ON_AIR") + " " + who.Length
    Debug.Trace("[Envoy] " + hello + list)
    Debug.Notification(hello)
    Debug.MessageBox(hello + list + "\n\n" + Envoy.Translate("$ENVOYDEMO_SAY_SOMETHING"))
EndFunction

; The string of an event is always empty: the event only wakes, the data is read
; from the bridge. The three parameters are kept because that is the signature of
; a handler in Papyrus.
Event OnAny(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string topicName = Envoy.GetTopic(id)
    ; Completeness is a new quantity of the contract: how sure the bridge is that
    ; the phrase ended on this utterance. While nobody sends it, it is one for
    ; everybody, and that too is an observation: holding does not work not because
    ; it is broken but because it has nothing to feed on.
    string line = "[" + id + "] " + Envoy.Translate("$ENVOYDEMO_TOPIC") + " " + topicName + ", " + Envoy.Translate("$ENVOYDEMO_COMPLETENESS") + " " + Envoy.GetComplete(id) + ": " + Envoy.GetText(id)
    Debug.Trace("[Envoy] " + line)
    Debug.Notification(line)

    ; The first utterance after a load goes as a dialogue: it is the one that proves
    ; the path from the microphone to the script works end to end. The rest go as
    ; notices.
    if !heardOnce
        heardOnce = true
        Debug.MessageBox(Envoy.Translate("$ENVOYDEMO_HEARS") + "\n\n" + Envoy.Translate("$ENVOYDEMO_TOPIC") + ": " + topicName + "\n" + Envoy.Translate("$ENVOYDEMO_RECOGNISED") + ": " + Envoy.GetText(id) + "\n\n" + Envoy.Translate("$ENVOYDEMO_FURTHER"))
    endIf
EndEvent

Event OnSettled(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string line = "[" + id + "] " + Envoy.Translate("$ENVOYDEMO_OUTCOME") + ": " + Envoy.GetOutcome(id)
    Debug.Trace("[Envoy] " + line)
    Debug.Notification(line)
EndEvent
