Scriptname EnvoyDemoShared extends Quest
{Делящийся подписчик: не требует исключительности.

 Словарь нарочно пересекается с жадным на фразе "проверка связи" - на ней и видно
 всё правило целиком:

   скажи "закрой дверь"   - её знает только жадный, он берёт реплику себе;
   скажи "что вокруг"     - её знает только делящийся, он берёт её без спора;
   скажи "проверка связи" - её знают оба. Кто увереннее, тот и решает судьбу:
                            победил жадный - делящийся остаётся ни с чем,
                            победил делящийся - жадный выбывает, потому что
                            сам просил "мне одному или никак".

 Об отклике - то же, что у жадного: на чужие реплики на экране не отвечаем,
 потому что надписи Skyrim выводит по одной раз в пять секунд и очередь отстаёт
 на минуты; решающие моменты показываются диалогом.}

string Property NS = "DemoShared" AutoReadOnly

Event OnInit()
    RegisterForModEvent("Envoy_Ready", "OnEnvoyReady")
    Register()
EndEvent

Event OnEnvoyReady(string asEventName, string asUnused, float afContract)
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
    words[0] = "что вокруг"
    words[1] = "проверка связи"
    Envoy.RegisterVocabulary(NS, words)

    RegisterForModEvent("Envoy_Speech_World", "OnHeard")
    RegisterForModEvent("Envoy_Speech_Dialogue", "OnHeard")
    RegisterForModEvent("Envoy_Award", "OnAward")
    RegisterForModEvent("Envoy_Denied", "OnDenied")
    Debug.Trace("[Envoy] делящийся объявился, словарь из " + words.Length + " фраз")
EndFunction

; Строка события всегда пуста - событие только будит. Текст, тема и итог
; читаются из моста по номеру: точная модель может уточнить реплику уже после
; рассылки, и копия в событии разошлась бы с тем, что мост считает истиной.
Event OnHeard(string asEventName, string asEmpty, float afId)
    int id = afId as int
    float mine = Envoy.GetVocabularyScore(id, NS)
    if mine <= 0.5
        Debug.Trace("[Envoy] делящийся [" + id + "]: не моё (" + mine + ")")
        return
    endIf
    Debug.Trace("[Envoy] делящийся [" + id + "]: ставлю " + mine)
    Debug.Notification("делящийся [" + id + "]: ставлю " + mine)
    Envoy.Bid(id, NS, mine, 0, false)
EndEvent

Event OnAward(string asEventName, string asEmpty, float afId)
    int id = afId as int
    if Envoy.IsWinner(id, NS)
        int winners = Envoy.GetWinners(id).Length
        Debug.Trace("[Envoy] делящийся [" + id + "]: получил, победителей " + winners)
        Debug.Notification("делящийся [" + id + "]: получил, победителей " + winners)
        Debug.MessageBox("ДЕЛЯЩИЙСЯ ПОЛУЧИЛ РЕПЛИКУ\n\nРеплика " + id + ": " + Envoy.GetText(id) + "\n\nПобедителей всего: " + winners + ".\nОн делится, поэтому жадный выбыл целиком - тот требовал исключительности.")
    endIf
EndEvent

Event OnDenied(string asEventName, string asEmpty, float afId)
    int id = afId as int
    string why = Envoy.GetDenyReason(id, NS)
    if why != ""
        Debug.Trace("[Envoy] делящийся [" + id + "]: отказ - " + why)
        Debug.Notification("делящийся [" + id + "]: отказ")
        Debug.MessageBox("ДЕЛЯЩЕМУСЯ ОТКАЗАНО\n\nРеплика " + id + ": " + Envoy.GetText(id) + "\n\nПричина: " + why)
    endIf
EndEvent
