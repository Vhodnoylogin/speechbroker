Scriptname EnvoyDemoObserver extends Quest
{Наблюдатель: видит КАЖДУЮ реплику, независимо от темы, и не участвует в розыгрыше.

 Нужен именно для того, чтобы было видно, когда реплика до кого-то не дошла:
 подписчик темы о чужих репликах не узнаёт никогда, а наблюдатель узнаёт обо всех.}

Event OnInit()
    ; Подписка на событие переживает сохранение, поэтому оформляется один раз.
    ; Всё остальное - темы, словарь - живёт только в памяти моста и гибнет вместе
    ; с процессом игры, поэтому мост зовёт объявиться заново на каждой загрузке.
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    Register()
EndEvent

Event OnEnvoyReady(string asEventName, string asUnused, float afContract)
    Register()
EndEvent

Function Register()
    if !Envoy.IsAvailable()
        Debug.Notification("Envoy: моста нет")
        return
    endIf
    RegisterForModEvent("Envoy_Speech_Any", "OnAny")
    RegisterForModEvent("Envoy_Settled", "OnSettled")
    Debug.Notification("Envoy: наблюдатель на связи, контракт " + Envoy.GetInterfaceVersion())
EndFunction

Event OnAny(string asEventName, string asTopic, float afId)
    int id = afId as int
    Debug.Notification("[" + id + "] тема " + asTopic + ": " + Envoy.GetText(id))
EndEvent

Event OnSettled(string asEventName, string asOutcome, float afId)
    Debug.Notification("[" + (afId as int) + "] итог: " + asOutcome)
EndEvent
