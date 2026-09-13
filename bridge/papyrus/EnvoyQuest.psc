Scriptname EnvoyQuest extends Quest
{The carrier of the mod: it comes up when the game starts and closes the loop of the self-test.

 The question "do the events of the bridge reach the Papyrus scripts" cannot be
 settled from C++: the bridge sees only its own side. So the carrier quest catches
 Envoy_Ping and calls Pong back. The answer, or the lack of one, is written by the
 bridge into its own log, and a test run no longer needs the Papyrus log to find
 this out.}

Event OnInit()
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    RegisterForModEvent("Envoy_Ping", "OnPing")
    OnGameReload()
EndEvent

Event OnEnvoyReady(string asEventName, string asEmpty, float afContract, Form akSender)
    OnGameReload()
EndEvent

Event OnPing(string asEventName, string asEmpty, float afToken, Form akSender)
    Envoy.Pong(afToken as int)
EndEvent

Function OnGameReload()
    if !Envoy.IsAvailable()
        Debug.Trace("[Envoy] the bridge is unavailable: the plugin did not load")
        return
    endIf
    Debug.Trace("[Envoy] contract version " + Envoy.GetInterfaceVersion())
    ; The subscription to Envoy_Ping is set up above and outlives a save, so the
    ; ring can be sent straight away: there is already somebody to catch it.
    Envoy.SelfTest()
EndFunction
