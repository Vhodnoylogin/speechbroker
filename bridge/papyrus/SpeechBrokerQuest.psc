Scriptname SpeechBrokerQuest extends Quest
{The carrier of the mod: it comes up when the game starts and closes the loop of the self-test.

 The question "do the events of the bridge reach the Papyrus scripts" cannot be
 settled from C++: the bridge sees only its own side. So the carrier quest catches
 SpeechBroker_Ping and calls Pong back. The answer, or the lack of one, is written by the
 bridge into its own log, and a test run no longer needs the Papyrus log to find
 this out.}

Event OnInit()
    RegisterForModEvent("SpeechBroker_Ready", "OnSpeechBrokerReady")
    RegisterForModEvent("SpeechBroker_Ping", "OnPing")
    OnGameReload()
EndEvent

Event OnSpeechBrokerReady(string asEventName, string asEmpty, float afContract, Form akSender)
    OnGameReload()
EndEvent

Event OnPing(string asEventName, string asEmpty, float afToken, Form akSender)
    SpeechBroker.Pong(afToken as int)
EndEvent

Function OnGameReload()
    if !SpeechBroker.IsAvailable()
        ; The one line of this mod that is written out in English. It says the
        ; plugin did not load, and the plugin is the thing that translates: calling
        ; SpeechBroker.Translate here would be calling a native that is not registered.
        Debug.Trace("[SpeechBroker] the bridge is unavailable: the plugin did not load")
        return
    endIf
    Debug.Trace("[SpeechBroker] " + SpeechBroker.Translate("$SPEECHBROKER_QUEST_CONTRACT") + " " + SpeechBroker.GetInterfaceVersion())
    ; The subscription to SpeechBroker_Ping is set up above and outlives a save, so the
    ; ring can be sent straight away: there is already somebody to catch it.
    SpeechBroker.SelfTest()
EndFunction
