Scriptname SpeechBrokerDemoGreedy extends Quest
{The greedy subscriber: if it wins, it takes the utterance for itself alone.

 It listens to the world topic only. Say the phrase behind $SPEECHBROKERDEMO_WORD_DOOR and
 you will see it win, and the sharing one left with nothing.

 About the response. Debug.Notification notices in Skyrim come out ONE AT A TIME
 with a delay of about five seconds and pile up in a queue, so on a stream of
 utterances they fall minutes behind and are useless as a sign. Hence two rules in
 this script:
   - to the utterances of others we do not answer on screen at all, only
     Debug.Trace, or the queue fills with noise and what matters arrives late;
   - the decisive moments - a win and a refusal - are shown with a dialogue that
     cannot be missed. A dialogue in Papyrus does not block the script.
 The real proof is in the log of the bridge anyway: it writes down every question a
 participant asks and every bid with its delay from the announcement.

 The vocabulary comes from SpeechBroker.Translate as well, and that is not for tidiness:
 the phrases have to be in the language the player actually speaks, which is the
 language of the installed model. Changing the language means adding a translation
 mod, not editing this script.}

string Property NS = "DemoGreedy" AutoReadOnly

Event OnInit()
    ; A subscription to an event outlives a save, so it is set up once. The topics
    ; and the vocabulary live only in the memory of the bridge and die with the
    ; process of the game, which is why the bridge calls everyone to declare
    ; themselves again on every load.
    RegisterForModEvent("SpeechBroker_Ready", "OnSpeechBrokerReady")
    Register()
EndEvent

Event OnSpeechBrokerReady(string asEventName, string asUnused, float afContract, Form akSender)
    Register()
EndEvent

Function Register()
    if !SpeechBroker.IsAvailable()
        return
    endIf

    string[] topics = new string[1]
    topics[0] = "world"
    SpeechBroker.Subscribe(NS, topics)

    string[] words = new string[2]
    words[0] = SpeechBroker.Translate("$SPEECHBROKERDEMO_WORD_DOOR")
    words[1] = SpeechBroker.Translate("$SPEECHBROKERDEMO_WORD_RADIOCHECK")
    SpeechBroker.RegisterVocabulary(NS, words)

    RegisterForModEvent("SpeechBroker_Speech_World", "OnHeard")
    RegisterForModEvent("SpeechBroker_Award", "OnAward")
    RegisterForModEvent("SpeechBroker_Denied", "OnDenied")
    Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " " + SpeechBroker.Translate("$SPEECHBROKERDEMO_DECLARED") + " " + words.Length + " " + SpeechBroker.Translate("$SPEECHBROKERDEMO_PHRASES"))
EndFunction

; The string of an event is always empty - the event only wakes. The text, the
; topic and the outcome are read from the bridge by number: the accurate model may
; refine an utterance after the broadcast, and a copy inside the event would part
; company with what the bridge holds to be true.
Event OnHeard(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    float mine = SpeechBroker.GetVocabularyScore(id, NS)
    if mine <= 0.5
        ; Somebody else utterance. We keep quiet on screen on purpose - see the
        ; explanation in the header.
        Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_NOT_MINE") + " (" + mine + ")")
        return
    endIf
    Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_BIDS") + " " + mine)
    Debug.Notification(SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_BIDS") + " " + mine)
    SpeechBroker.Bid(id, NS, mine, 0, true)
EndEvent

Event OnAward(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    if SpeechBroker.IsWinner(id, NS)
        Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_TOOK_IT"))
        Debug.Notification(SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_TOOK_IT"))
        Debug.MessageBox(SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY_WON") + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_UTTERANCE") + " " + id + ": " + SpeechBroker.GetText(id) + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY_WON_WHY"))
    endIf
EndEvent

Event OnDenied(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string why = SpeechBroker.GetDenyReason(id, NS)
    if why != ""
        Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_REFUSED") + " - " + why)
        Debug.Notification(SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_REFUSED"))
        Debug.MessageBox(SpeechBroker.Translate("$SPEECHBROKERDEMO_GREEDY_DENIED") + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_UTTERANCE") + " " + id + ": " + SpeechBroker.GetText(id) + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_REASON") + ": " + why)
    endIf
EndEvent
