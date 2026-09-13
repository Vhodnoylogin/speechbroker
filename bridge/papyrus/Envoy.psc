Scriptname Envoy Hidden
{Игровое API моста Envoy. Версия контракта 3 - та же, что возвращает GetInterfaceVersion.

 Событие только будит мод и приносит номер реплики. Всё остальное мод забирает
 функциями этого скрипта. Событие ничего не возвращает — ответить из него нельзя.}

; ---------------------------------------------------------------- версия и наличие
int Function GetInterfaceVersion() global native
bool Function IsAvailable() global native

; ---------------------------------------------------------------- подписка
; asTopics: "dialogue", "menu", "combat", "world", "channel:<имя>"
Function Subscribe(string asNamespace, string[] asTopics) global native
Function Unsubscribe(string asNamespace) global native
Function SetActive(string asNamespace, bool abActive) global native

Function RegisterVocabulary(string asNamespace, string[] asPhrases) global native
Function ClearVocabulary(string asNamespace) global native

; Что участник о себе объявляет. От этого зависит, придержит ли мост
; незаконченную фразу или отдаст сразу.
;   aiCostClass: 0 - обратимое действие, 1 - дорогое
;   abRevocable: умеешь ли ты отменить сделанное. Объявивший true получает
;                незаконченное раньше прочих и обязан слушать Envoy_Revoked:
;                отзывчивость покупает скорость, но и обязывает.
Function Declare(string asNamespace, int aiCostClass, bool abRevocable = false) global native

; ---------------------------------------------------------------- о реплике
string Function GetText(int aiUtteranceId) global native
float Function GetScore(int aiUtteranceId) global native
float Function GetMargin(int aiUtteranceId) global native
bool Function IsFinal(int aiUtteranceId) global native
; Насколько мост уверен, что на этой реплике фраза ЗАКОНЧИЛАСЬ. Единица -
; закончилась; ноль - человек, скорее всего, ещё говорит. Реплику с низким
; числом мост придерживает и до подписчиков может не донести вовсе.
float Function GetComplete(int aiUtteranceId) global native
string Function GetEngineId(int aiUtteranceId) global native
string Function GetLanguage(int aiUtteranceId) global native
string Function GetChannel(int aiUtteranceId) global native
int Function GetLatencyMs(int aiUtteranceId) global native
string[] Function GetAlternatives(int aiUtteranceId) global native
float[] Function GetAlternativeScores(int aiUtteranceId) global native

; ---------------------------------------------------------------- лично для подписчика
string Function GetVocabularyMatch(int aiUtteranceId, string asNamespace) global native
float Function GetVocabularyScore(int aiUtteranceId, string asNamespace) global native
float Function GetVocabularyMargin(int aiUtteranceId, string asNamespace) global native
bool Function WonPrevious(string asNamespace) global native
float Function SecondsSinceWin(string asNamespace) global native

; ---------------------------------------------------------------- снимок мира
; GetStateStatus: 0 - спросить некому, 1 - значение есть, 2 - поставщик не смог
int Function GetStateStatus(int aiUtteranceId, string asKey) global native
float Function GetStateAge(int aiUtteranceId, string asKey) global native

bool Function GetStateBool(int aiUtteranceId, string asKey, bool abDefault = false) global native
int Function GetStateInt(int aiUtteranceId, string asKey, int aiDefault = 0) global native
float Function GetStateFloat(int aiUtteranceId, string asKey, float afDefault = 0.0) global native
string Function GetStateString(int aiUtteranceId, string asKey, string asDefault = "") global native
Form Function GetStateForm(int aiUtteranceId, string asKey) global native

string[] Function GetKeys() global native

; ---------------------------------------------------------------- аукцион
; aiCostClass: 0 - обратимое действие, 1 - дорогое
Function Bid(int aiUtteranceId, string asNamespace, float afConfidence, int aiCostClass, bool abGreedy = false) global native
Function Done(int aiUtteranceId, string asNamespace, bool abSucceeded) global native
string Function GetWinner(int aiUtteranceId) global native

; ---------------------------------------------------------------- поставщик состояния
; asType: "bool" | "int" | "float" | "string" | "form"
; afTtlSec: сколько секунд значение считается достоверным; 0 - не устаревает
Function DeclareKey(string asKey, string asType, float afTtlSec, string asDescription) global native
Function RetractKey(string asKey) global native

Function PublishBool(string asKey, bool abValue) global native
Function PublishInt(string asKey, int aiValue) global native
Function PublishFloat(string asKey, float afValue) global native
Function PublishString(string asKey, string asValue) global native
Function PublishForm(string asKey, Form akValue) global native

; ---------------------------------------------------------------- адаптеры
; Мост держит по одной активной способности за раз: остальные адаптеры глушатся
; и обязаны отпустить своё устройство. Переключение действует сразу и живёт
; до конца сессии; постоянное умолчание задаётся в файле настроек.
string[] Function GetAdapters() global native
string Function GetSource(string asCapability) global native
bool Function SetSource(string asCapability, string asAdapter) global native
Function ReloadSettings() global native

; ---------------------------------------------------------------- итог розыгрыша
; Победителей может быть несколько: жадность - заявка на исключительность,
; и она срабатывает только если заявитель победил.
;   выиграл жадный   -> результат только ему
;   выиграл делящийся -> жадные выбывают, результат делят все остальные
string[] Function GetWinners(int aiUtteranceId) global native
bool Function IsWinner(int aiUtteranceId, string asNamespace) global native
string Function GetDenyReason(int aiUtteranceId, string asNamespace) global native
string Function GetOutcome(int aiUtteranceId) global native

; Ответ модели на Ask и исход озвучки. Приходят событиями Envoy_Answer
; и Envoy_SpeechDone, в которых лежит только номер запроса.
string Function GetAnswer(int aiRequestId) global native

; Самопроверка рассылки событий. SelfTest зовёт квест-носитель при запуске;
; мост в ответ шлёт Envoy_Ping, а скрипт обязан вызвать Pong с той же меткой.
; Ответ или его отсутствие мост записывает в свой журнал - иначе узнать,
; доходят ли события до Papyrus, из C++ невозможно.
; Кто объявился мосту и на какие темы. Нужно затем, чтобы участник мог показать
; игроку то, чего тот не знает, вместо повторения версии контракта.
string[] Function GetNamespaces() global native
string[] Function GetTopicsOf(string asNamespace) global native

Function SelfTest() global native
Function Pong(int aiToken) global native

string Function GetSpeechResult(int aiSpeechId) global native

string Function GetTopic(int aiUtteranceId) global native

; ---------------------------------------------------------------- из игры в модель
; Второе направление моста. Оба вызова возвращают номер сразу и никогда не ждут:
; результат приходит событием, иначе Papyrus встал бы колом.
;
;   событие "Envoy_SpeechDone", строка = ok | interrupted | failed, число = номер озвучки
;   событие "Envoy_Answer",     строка = ответ модели,              число = номер запроса
;
; Содержимое asPayload мост не разбирает: что положил мод, то и получит адаптер.
int Function Say(string asText, string asVoice = "", int aiPriority = 0) global native
Function StopSpeech(int aiSpeechId) global native
int Function Ask(string asService, string asPayload) global native
