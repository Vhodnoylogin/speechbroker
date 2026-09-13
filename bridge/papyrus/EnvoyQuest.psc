Scriptname EnvoyQuest extends Quest
{Носитель мода: поднимается при старте игры и замыкает круг самопроверки.

 Вопрос "доходят ли события моста до скриптов Papyrus" из C++ не решается: мост
 видит только свою сторону. Поэтому квест-носитель ловит Envoy_Ping и зовёт Pong
 обратно. Ответ или его отсутствие мост записывает в свой журнал, и прогону
 больше не нужен журнал Papyrus, чтобы это выяснить.}

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
        Debug.Trace("[Envoy] мост недоступен: плагин не загрузился")
        return
    endIf
    Debug.Trace("[Envoy] контракт версии " + Envoy.GetInterfaceVersion())
    ; Подписка на Envoy_Ping оформлена выше и переживает сохранение, поэтому
    ; звонок можно посылать сразу: ловить его уже есть кому.
    Envoy.SelfTest()
EndFunction
