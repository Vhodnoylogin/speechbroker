Scriptname SpeechBroker Hidden
{In-game API of the SpeechBroker bridge. Contract version 3 - the same number GetInterfaceVersion returns.

 An event only wakes a mod up and brings it the number of an utterance. Everything
 else the mod fetches with the functions of this script. The event returns nothing:
 there is no answering from inside it.}

; ---------------------------------------------------------------- version and presence
int Function GetInterfaceVersion() global native
bool Function IsAvailable() global native

; ---------------------------------------------------------------- subscribing
; asTopics: "dialogue", "menu", "combat", "world", "channel:<name>"
Function Subscribe(string asNamespace, string[] asTopics) global native
Function Unsubscribe(string asNamespace) global native
Function SetActive(string asNamespace, bool abActive) global native

Function RegisterVocabulary(string asNamespace, string[] asPhrases) global native
Function ClearVocabulary(string asNamespace) global native

; What a participant declares about itself. Whether the bridge holds an unfinished
; phrase back or hands it over at once depends on this.
;   aiCostClass: 0 - a reversible action, 1 - an expensive one
;   abRevocable: whether you can undo what you did. Declaring true gets you the
;                unfinished thing before anybody else and obliges you to listen for
;                SpeechBroker_Revoked: being revocable buys speed and costs duty.
Function Declare(string asNamespace, int aiCostClass, bool abRevocable = false) global native

; ---------------------------------------------------------------- about the utterance
string Function GetText(int aiUtteranceId) global native
float Function GetScore(int aiUtteranceId) global native
float Function GetMargin(int aiUtteranceId) global native
bool Function IsFinal(int aiUtteranceId) global native
; How sure the bridge is that the phrase ENDED on this utterance. One - it ended;
; zero - the person is most likely still speaking. An utterance with a low number
; is held back by the bridge and may never reach the subscribers at all.
float Function GetComplete(int aiUtteranceId) global native
string Function GetEngineId(int aiUtteranceId) global native
string Function GetLanguage(int aiUtteranceId) global native
string Function GetChannel(int aiUtteranceId) global native
int Function GetLatencyMs(int aiUtteranceId) global native
string[] Function GetAlternatives(int aiUtteranceId) global native
float[] Function GetAlternativeScores(int aiUtteranceId) global native

; ---------------------------------------------------------------- for the subscriber alone
string Function GetVocabularyMatch(int aiUtteranceId, string asNamespace) global native
float Function GetVocabularyScore(int aiUtteranceId, string asNamespace) global native
float Function GetVocabularyMargin(int aiUtteranceId, string asNamespace) global native
bool Function WonPrevious(string asNamespace) global native
float Function SecondsSinceWin(string asNamespace) global native

; ---------------------------------------------------------------- snapshot of the world
; GetStateStatus: 0 - nobody to ask, 1 - there is a value, 2 - the provider could not
int Function GetStateStatus(int aiUtteranceId, string asKey) global native
float Function GetStateAge(int aiUtteranceId, string asKey) global native

bool Function GetStateBool(int aiUtteranceId, string asKey, bool abDefault = false) global native
int Function GetStateInt(int aiUtteranceId, string asKey, int aiDefault = 0) global native
float Function GetStateFloat(int aiUtteranceId, string asKey, float afDefault = 0.0) global native
string Function GetStateString(int aiUtteranceId, string asKey, string asDefault = "") global native
Form Function GetStateForm(int aiUtteranceId, string asKey) global native

string[] Function GetKeys() global native

; ---------------------------------------------------------------- the auction
; aiCostClass: 0 - a reversible action, 1 - an expensive one
Function Bid(int aiUtteranceId, string asNamespace, float afConfidence, int aiCostClass, bool abGreedy = false) global native
Function Done(int aiUtteranceId, string asNamespace, bool abSucceeded) global native
string Function GetWinner(int aiUtteranceId) global native

; ---------------------------------------------------------------- state provider
; asType: "bool" | "int" | "float" | "string" | "form"
; afTtlSec: for how many seconds the value counts as trustworthy; 0 - never stales
Function DeclareKey(string asKey, string asType, float afTtlSec, string asDescription) global native
Function RetractKey(string asKey) global native

Function PublishBool(string asKey, bool abValue) global native
Function PublishInt(string asKey, int aiValue) global native
Function PublishFloat(string asKey, float afValue) global native
Function PublishString(string asKey, string asValue) global native
Function PublishForm(string asKey, Form akValue) global native

; ---------------------------------------------------------------- adapters
; The bridge keeps one active source per capability at a time: the other adapters
; are muted and have to let go of their device. Switching takes effect at once and
; lasts until the end of the session; the lasting default is set in the settings file.
string[] Function GetAdapters() global native
string Function GetSource(string asCapability) global native
bool Function SetSource(string asCapability, string asAdapter) global native
Function ReloadSettings() global native

; ---------------------------------------------------------------- text for the player
; Gives back the line the key stands for, in the language the game runs in.
;
; The engine already resolves a $-prefixed string on its own when the whole string
; is shown as it is - Debug.MessageBox("$MY_KEY") works without any help from us.
; This function is for the other case: a line glued together out of a translated
; part and a number, which the engine cannot handle, and for text that is not shown
; at all - the phrases a subscriber registers as its vocabulary, which have to be
; in the language the player actually speaks.
;
; Where the lines come from: Interface\Translations\SpeechBroker*_<language>.txt, UTF-16LE,
; one "$KEY<tab>text" line each. A mod puts its own file next to ours and names it
; SpeechBroker<Something>_<language>.txt, and its keys appear here. An unknown key comes
; back as itself, so a forgotten line is visible on screen rather than blank.
string Function Translate(string asKey) global native

; ---------------------------------------------------------------- how the draw ended
; There can be more than one winner: greed is a claim to exclusivity, and it only
; fires if the one claiming it won.
;   a greedy one won   -> the result goes to it alone
;   a sharing one won  -> the greedy drop out, the rest share the result
string[] Function GetWinners(int aiUtteranceId) global native
bool Function IsWinner(int aiUtteranceId, string asNamespace) global native
string Function GetDenyReason(int aiUtteranceId, string asNamespace) global native
string Function GetOutcome(int aiUtteranceId) global native

; The model's answer to Ask and the outcome of speaking. They arrive as the events
; SpeechBroker_Answer and SpeechBroker_SpeechDone, which carry nothing but the request number.
string Function GetAnswer(int aiRequestId) global native

; A self-test of event delivery. The carrier quest calls SelfTest on startup; the
; bridge answers with SpeechBroker_Ping, and the script has to call Pong with the same
; token. Whether the answer came or not goes into the bridge's log - there is no
; other way from C++ to learn whether events reach Papyrus at all.
; Who declared themselves to the bridge and on which topics. Wanted so that a
; participant can show the player something they do not know, instead of repeating
; the contract version at them.
string[] Function GetNamespaces() global native
string[] Function GetTopicsOf(string asNamespace) global native

Function SelfTest() global native
Function Pong(int aiToken) global native

string Function GetSpeechResult(int aiSpeechId) global native

string Function GetTopic(int aiUtteranceId) global native

; ---------------------------------------------------------------- from the game to the model
; The bridge's second direction. Both calls hand back a number at once and never
; wait: the result arrives as an event, or Papyrus would seize up.
;
;   event "SpeechBroker_SpeechDone", string = ok | interrupted | failed, number = speech id
;   event "SpeechBroker_Answer",     string = the model's answer,        number = request id
;
; The bridge does not look inside asPayload: what the mod put in is what the adapter
; gets out.
int Function Say(string asText, string asVoice = "", int aiPriority = 0) global native
Function StopSpeech(int aiSpeechId) global native
int Function Ask(string asService, string asPayload) global native
