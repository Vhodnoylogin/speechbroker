Scriptname EnvoyDemoObserver extends Quest
{Наблюдатель: видит КАЖДУЮ реплику, независимо от темы, и не участвует в розыгрыше.

 Нужен именно для того, чтобы было видно, когда реплика до кого-то не дошла:
 подписчик темы о чужих репликах не узнаёт никогда, а наблюдатель узнаёт обо всех.

 Отклик нарочно гипертрофирован. В шлеме надпись в углу мелкая и держится недолго,
 поэтому "не заметил" и "не сработало" выглядели одинаково и наблюдение ничего
 не доказывало. Решающие моменты теперь показываются диалогом, который пропустить
 нельзя: подключение к мосту и первая услышанная реплика после загрузки.
 Дальше - обычные надписи, иначе диалог пришлось бы закрывать на каждую фразу.}

bool heardOnce = false

Event OnInit()
    ; Подписка на событие переживает сохранение, поэтому оформляется один раз.
    ; Всё остальное живёт только в памяти моста и гибнет вместе с процессом игры,
    ; поэтому мост зовёт объявиться заново на каждой загрузке.
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    Register()
EndEvent

Event OnEnvoyReady(string asEventName, string asUnused, float afContract)
    Register()
EndEvent

Function Register()
    heardOnce = false
    if !Envoy.IsAvailable()
        Debug.Trace("[Envoy] моста нет: плагин не загрузился")
        Debug.MessageBox("Envoy: моста нет.\n\nПлагин Envoy.dll не загрузился - смотри Envoy.log.")
        return
    endIf
    RegisterForModEvent("Envoy_Speech_Any", "OnAny")
    RegisterForModEvent("Envoy_Settled", "OnSettled")

    string hello = "Envoy на связи, контракт " + Envoy.GetInterfaceVersion()
    Debug.Trace("[Envoy] " + hello)
    Debug.Notification(hello)
    Debug.MessageBox(hello + ".\n\nНаблюдатель подключён и слышит все реплики.\nСкажи что-нибудь - следующий диалог покажет распознанный текст.")
EndFunction

Event OnAny(string asEventName, string asTopic, float afId)
    int id = afId as int
    string line = "[" + id + "] тема " + asTopic + ": " + Envoy.GetText(id)
    Debug.Trace("[Envoy] " + line)
    Debug.Notification(line)

    ; Первая реплика после загрузки - диалогом: именно она доказывает, что путь
    ; от микрофона до скрипта работает целиком. Остальные - надписями.
    if !heardOnce
        heardOnce = true
        Debug.MessageBox("Envoy слышит.\n\nТема: " + asTopic + "\nРаспознано: " + Envoy.GetText(id) + "\n\nДальше отчёт пойдёт надписями в углу.")
    endIf
EndEvent

Event OnSettled(string asEventName, string asOutcome, float afId)
    string line = "[" + (afId as int) + "] итог: " + asOutcome
    Debug.Trace("[Envoy] " + line)
    Debug.Notification(line)
EndEvent
