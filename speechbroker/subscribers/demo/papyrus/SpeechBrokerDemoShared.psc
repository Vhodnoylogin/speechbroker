Scriptname SpeechBrokerDemoShared extends Quest
{The sharing subscriber: it does not demand exclusivity.

 Its vocabulary deliberately overlaps the greedy one on the radio-check phrase, and
 on that phrase the whole rule shows at once:

   say the door phrase        - only the greedy one knows it, it takes the utterance;
   say the look-around phrase - only the sharing one knows it, it takes it unopposed;
   say the radio-check phrase - both know it. Whoever is more confident decides the
                                fate of the utterance: if the greedy one won, the
                                sharing one is left with nothing; if the sharing one
                                won, the greedy one drops out, because it asked for
                                "mine alone or not at all" itself.

 About the response - the same as for the greedy one: we do not answer the
 utterances of others on screen, because Skyrim shows notices one at a time every
 five seconds and the queue falls minutes behind; the decisive moments are shown
 with a dialogue.

 The vocabulary comes from SpeechBroker.Translate as well: the phrases have to be in the
 language the player actually speaks, which is the language of the installed model.}

string Property NS = "DemoShared" AutoReadOnly

Event OnInit()
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

    string[] topics = new string[2]
    topics[0] = "world"
    topics[1] = "dialogue"
    SpeechBroker.Subscribe(NS, topics)

    string[] words = new string[2]
    words[0] = SpeechBroker.Translate("$SPEECHBROKERDEMO_WORD_AROUND")
    words[1] = SpeechBroker.Translate("$SPEECHBROKERDEMO_WORD_RADIOCHECK")
    SpeechBroker.RegisterVocabulary(NS, words)

    RegisterForModEvent("SpeechBroker_Speech_World", "OnHeard")
    RegisterForModEvent("SpeechBroker_Speech_Dialogue", "OnHeard")
    RegisterForModEvent("SpeechBroker_Award", "OnAward")
    RegisterForModEvent("SpeechBroker_Denied", "OnDenied")
    Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARING") + " " + SpeechBroker.Translate("$SPEECHBROKERDEMO_DECLARED") + " " + words.Length + " " + SpeechBroker.Translate("$SPEECHBROKERDEMO_PHRASES"))
EndFunction

; The string of an event is always empty - the event only wakes. The text, the
; topic and the outcome are read from the bridge by number: the accurate model may
; refine an utterance after the broadcast, and a copy inside the event would part
; company with what the bridge holds to be true.
Event OnHeard(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    float mine = SpeechBroker.GetVocabularyScore(id, NS)
    if mine <= 0.5
        Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARING") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_NOT_MINE") + " (" + mine + ")")
        return
    endIf
    Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARING") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_BIDS") + " " + mine)
    Debug.Notification(SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_BIDS") + " " + mine)
    SpeechBroker.Bid(id, NS, mine, 0, false)
EndEvent

Event OnAward(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    if SpeechBroker.IsWinner(id, NS)
        int winners = SpeechBroker.GetWinners(id).Length
        Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GOT_IT") + " " + winners)
        Debug.Notification(SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_GOT_IT") + " " + winners)
        Debug.MessageBox(SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED_WON") + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_UTTERANCE") + " " + id + ": " + SpeechBroker.GetText(id) + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_WINNERS_TOTAL") + " " + winners + ".\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED_WON_WHY"))
    endIf
EndEvent

Event OnDenied(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string why = SpeechBroker.GetDenyReason(id, NS)
    if why != ""
        Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARING") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_REFUSED") + " - " + why)
        Debug.Notification(SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED") + " [" + id + "]: " + SpeechBroker.Translate("$SPEECHBROKERDEMO_REFUSED"))
        Debug.MessageBox(SpeechBroker.Translate("$SPEECHBROKERDEMO_SHARED_DENIED") + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_UTTERANCE") + " " + id + ": " + SpeechBroker.GetText(id) + "\n\n" + SpeechBroker.Translate("$SPEECHBROKERDEMO_REASON") + ": " + why)
    endIf
EndEvent
