Scriptname EnvoyDemoGreedy extends Quest
{Жадный подписчик: если выигрывает - забирает реплику себе одному.

 Слушает только тему world. Скажи "закрой дверь" - и увидишь, как он побеждает
 и как делящийся при этом остаётся без результата.

 Об отклике. Надписи Debug.Notification в Skyrim выводятся ПО ОДНОЙ с задержкой
 около пяти секунд и копятся в очереди, поэтому на потоке реплик они отстают на
 минуты и как признак бесполезны. Отсюда два правила в этом скрипте:
   - на чужие реплики не отвечаем вовсе, только Debug.Trace, иначе очередь
     забивается шумом и важное приходит с опозданием;
   - решающие моменты - выигрыш и отказ - показываются диалогом, который
     пропустить нельзя. Диалог в Papyrus не блокирует скрипт.
 Настоящее доказательство всё равно в журнале моста: он пишет каждый вопрос
 участника и каждую ставку с задержкой от оглашения.}

string Property NS = "DemoGreedy" AutoReadOnly

Event OnInit()
    ; Подписка на событие переживает сохранение, поэтому оформляется один раз.
    ; Темы и словарь живут только в памяти моста и гибнут вместе с процессом
    ; игры, поэтому мост зовёт объявиться заново на каждой загрузке.
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
    Debug.Trace("[Envoy] жадный объявился, словарь из " + words.Length + " фраз")
EndFunction

Event OnHeard(string asEventName, string asText, float afId)
    int id = afId as int
    float mine = Envoy.GetVocabularyScore(id, NS)
    if mine <= 0.5
        ; Чужая реплика. Молчим на экране нарочно - см. пояснение в заголовке.
        Debug.Trace("[Envoy] жадный [" + id + "]: не моё (" + mine + ")")
        return
    endIf
    Debug.Trace("[Envoy] жадный [" + id + "]: ставлю " + mine)
    Debug.Notification("жадный [" + id + "]: ставлю " + mine)
    Envoy.Bid(id, NS, mine, 0, true)
EndEvent

Event OnAward(string asEventName, string asWinner, float afId)
    if asWinner == NS
        int id = afId as int
        Debug.Trace("[Envoy] жадный [" + id + "]: ВЗЯЛ СЕБЕ")
        Debug.Notification("жадный [" + id + "]: ВЗЯЛ СЕБЕ")
        Debug.MessageBox("ЖАДНЫЙ ВЗЯЛ РЕПЛИКУ СЕБЕ\n\nРеплика " + id + ": " + Envoy.GetText(id) + "\n\nОн требовал исключительности и получил её - значит делящийся остался ни с чем.")
    endIf
EndEvent

Event OnDenied(string asEventName, string asWho, float afId)
    if asWho == NS
        int id = afId as int
        string why = Envoy.GetDenyReason(id, NS)
        Debug.Trace("[Envoy] жадный [" + id + "]: отказ - " + why)
        Debug.Notification("жадный [" + id + "]: отказ")
        Debug.MessageBox("ЖАДНОМУ ОТКАЗАНО\n\nРеплика " + id + ": " + Envoy.GetText(id) + "\n\nПричина: " + why)
    endIf
EndEvent
