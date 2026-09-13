Scriptname EnvoyDemoShared extends Quest
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

 The vocabulary comes from Envoy.Translate as well: the phrases have to be in the
 language the player actually speaks, which is the language of the installed model.}

string Property NS = "DemoShared" AutoReadOnly

Event OnInit()
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    Register()
EndEvent

Event OnEnvoyReady(string asEventName, string asUnused, float afContract, Form akSender)
    Register()
EndEvent

Function Register()
    if !Envoy.IsAvailable()
        return
    endIf

    string[] topics = new string[2]
    topics[0] = "world"
    topics[1] = "dialogue"
    Envoy.Subscribe(NS, topics)

    string[] words = new string[2]
    words[0] = Envoy.Translate("$ENVOYDEMO_WORD_AROUND")
    words[1] = Envoy.Translate("$ENVOYDEMO_WORD_RADIOCHECK")
    Envoy.RegisterVocabulary(NS, words)

    RegisterForModEvent("Envoy_Speech_World", "OnHeard")
    RegisterForModEvent("Envoy_Speech_Dialogue", "OnHeard")
    RegisterForModEvent("Envoy_Award", "OnAward")
    RegisterForModEvent("Envoy_Denied", "OnDenied")
    Debug.Trace("[Envoy] the sharing one declared itself, a vocabulary of " + words.Length + " phrases")
EndFunction

; The string of an event is always empty - the event only wakes. The text, the
; topic and the outcome are read from the bridge by number: the accurate model may
; refine an utterance after the broadcast, and a copy inside the event would part
; company with what the bridge holds to be true.
Event OnHeard(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    float mine = Envoy.GetVocabularyScore(id, NS)
    if mine <= 0.5
        Debug.Trace("[Envoy] sharing [" + id + "]: not mine (" + mine + ")")
        return
    endIf
    Debug.Trace("[Envoy] sharing [" + id + "]: bidding " + mine)
    Debug.Notification(Envoy.Translate("$ENVOYDEMO_SHARED") + " [" + id + "]: " + Envoy.Translate("$ENVOYDEMO_BIDS") + " " + mine)
    Envoy.Bid(id, NS, mine, 0, false)
EndEvent

Event OnAward(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    if Envoy.IsWinner(id, NS)
        int winners = Envoy.GetWinners(id).Length
        Debug.Trace("[Envoy] sharing [" + id + "]: got it, winners " + winners)
        Debug.Notification(Envoy.Translate("$ENVOYDEMO_SHARED") + " [" + id + "]: " + Envoy.Translate("$ENVOYDEMO_GOT_IT") + " " + winners)
        Debug.MessageBox(Envoy.Translate("$ENVOYDEMO_SHARED_WON") + "\n\n" + Envoy.Translate("$ENVOYDEMO_UTTERANCE") + " " + id + ": " + Envoy.GetText(id) + "\n\n" + Envoy.Translate("$ENVOYDEMO_WINNERS_TOTAL") + " " + winners + ".\n" + Envoy.Translate("$ENVOYDEMO_SHARED_WON_WHY"))
    endIf
EndEvent

Event OnDenied(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string why = Envoy.GetDenyReason(id, NS)
    if why != ""
        Debug.Trace("[Envoy] sharing [" + id + "]: refused - " + why)
        Debug.Notification(Envoy.Translate("$ENVOYDEMO_SHARED") + " [" + id + "]: " + Envoy.Translate("$ENVOYDEMO_REFUSED"))
        Debug.MessageBox(Envoy.Translate("$ENVOYDEMO_SHARED_DENIED") + "\n\n" + Envoy.Translate("$ENVOYDEMO_UTTERANCE") + " " + id + ": " + Envoy.GetText(id) + "\n\n" + Envoy.Translate("$ENVOYDEMO_REASON") + ": " + why)
    endIf
EndEvent
