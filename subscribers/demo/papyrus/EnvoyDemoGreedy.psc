Scriptname EnvoyDemoGreedy extends Quest
{Жадный подписчик: если выигрывает - забирает реплику себе одному.

 Слушает только тему world. Скажи "закрой дверь" - и увидишь, как он побеждает
 и как делящийся при этом остаётся без результата.}

string Property NS = "DemoGreedy" AutoReadOnly

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
        return
    endIf

    string[] topics = new string[1]
    topics[0] = "world"
    Envoy.Subscribe(NS, topics)

    string[] words = new string[2]
    words[0] = "закрой дверь"
    words[1] = "проверка связи"
    Envoy.RegisterVocabulary(NS, words)

    RegisterForModEvent("Envoy_Speech_World", "OnHeard")
    RegisterForModEvent("Envoy_Award", "OnAward")
    RegisterForModEvent("Envoy_Denied", "OnDenied")
EndFunction

Event OnHeard(string asEventName, string asText, float afId)
    int id = afId as int
    float mine = Envoy.GetVocabularyScore(id, NS)
    if mine <= 0.5
        Debug.Notification("жадный [" + id + "]: не моё (" + mine + ")")
        return
    endIf
    Debug.Notification("жадный [" + id + "]: ставлю " + mine)
    Envoy.Bid(id, NS, mine, 0, true)
EndEvent

Event OnAward(string asEventName, string asWinner, float afId)
    if asWinner == NS
        Debug.Notification("жадный [" + (afId as int) + "]: ВЗЯЛ СЕБЕ")
    endIf
EndEvent

Event OnDenied(string asEventName, string asWho, float afId)
    if asWho == NS
        int id = afId as int
        Debug.Notification("жадный [" + id + "]: отказ - " + Envoy.GetDenyReason(id, NS))
    endIf
EndEvent
