Scriptname EnvoyDemoShared extends Quest
{Делящийся подписчик: не требует исключительности.

 Словарь нарочно пересекается с жадным на фразе "проверка связи" - на ней и видно
 всё правило целиком:

   скажи "закрой дверь"   - её знает только жадный, он берёт реплику себе;
   скажи "что вокруг"     - её знает только делящийся, он берёт её без спора;
   скажи "проверка связи" - её знают оба. Кто увереннее, тот и решает судьбу:
                            победил жадный - делящийся остаётся ни с чем,
                            победил делящийся - жадный выбывает, потому что
                            сам просил "мне одному или никак".}

string Property NS = "DemoShared" AutoReadOnly

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
EndFunction

Event OnHeard(string asEventName, string asText, float afId)
    int id = afId as int
    float mine = Envoy.GetVocabularyScore(id, NS)
    if mine <= 0.5
        Debug.Notification("делящийся [" + id + "]: не моё (" + mine + ")")
        return
    endIf
    Debug.Notification("делящийся [" + id + "]: ставлю " + mine)
    Envoy.Bid(id, NS, mine, 0, false)
EndEvent

Event OnAward(string asEventName, string asWinner, float afId)
    if asWinner == NS
        int id = afId as int
        Debug.Notification("делящийся [" + id + "]: получил, всего победителей " + Envoy.GetWinners(id).Length)
    endIf
EndEvent

Event OnDenied(string asEventName, string asWho, float afId)
    if asWho == NS
        int id = afId as int
        Debug.Notification("делящийся [" + id + "]: отказ - " + Envoy.GetDenyReason(id, NS))
    endIf
EndEvent
