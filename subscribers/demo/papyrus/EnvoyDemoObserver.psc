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

Event OnEnvoyReady(string asEventName, string asUnused, float afContract, Form akSender)
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
    ; Наблюдатель ничего не делает и потому объявляет себя отзывчивым: отменять
    ; ему нечего. Вызов здесь ради самой проверки - что новая функция контракта
    ; доходит до скрипта и не роняет его.
    Envoy.Declare("DemoObserver", 0, true)

    ; Диалог обязан сообщать неизвестное. Версия контракта была известна заранее
    ; и сомнений не вызывала; неизвестно другое - объявился ли кто-нибудь мосту
    ; и на какие темы. Один взгляд теперь отделяет "участников нет" от
    ; "участники есть, но события до них не доходят".
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
        list = "\n  никого"
    endIf

    string hello = "Envoy на связи, участников " + who.Length
    Debug.Trace("[Envoy] " + hello + list)
    Debug.Notification(hello)
    Debug.MessageBox(hello + list + "\n\nСкажи что-нибудь - следующий диалог покажет распознанный текст.")
EndFunction

; Строка события всегда пуста: событие только будит, данные читаются из моста.
; Три параметра сохранены потому, что такова подпись обработчика в Papyrus.
Event OnAny(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string topicName = Envoy.GetTopic(id)
    ; Завершённость - новая величина контракта: насколько мост уверен, что
    ; на этой реплике фраза кончилась. Пока её никто не присылает, она равна
    ; единице у всех, и это тоже наблюдение: значит придержание не работает
    ; не потому, что сломано, а потому, что ему нечем питаться.
    string line = "[" + id + "] тема " + topicName + ", завершённость " + Envoy.GetComplete(id) + ": " + Envoy.GetText(id)
    Debug.Trace("[Envoy] " + line)
    Debug.Notification(line)

    ; Первая реплика после загрузки - диалогом: именно она доказывает, что путь
    ; от микрофона до скрипта работает целиком. Остальные - надписями.
    if !heardOnce
        heardOnce = true
        Debug.MessageBox("Envoy слышит.\n\nТема: " + topicName + "\nРаспознано: " + Envoy.GetText(id) + "\n\nДальше отчёт пойдёт надписями в углу.")
    endIf
EndEvent

Event OnSettled(string asEventName, string asEmpty, float afId, Form akSender)
    int id = afId as int
    string line = "[" + id + "] итог: " + Envoy.GetOutcome(id)
    Debug.Trace("[Envoy] " + line)
    Debug.Notification(line)
EndEvent
