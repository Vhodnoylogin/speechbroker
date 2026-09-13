"""Эталонная реализация того, чем владеет мост: выбор темы, снимок, аукцион.

Это исполняемая спецификация. C++ обязан вести себя ровно так же; расхождение
здесь и в игре означает ошибку в плагине, а не в сценарии.

Логика мода-подписчика сюда НЕ входит: как он решает, ставить ли и с какой
уверенностью, - его дело. Сценарий приносит уже готовые ставки.

Все пороги берутся из config/envoy.default.json. Констант в коде нет.
"""
from __future__ import annotations

COST_NAMES = {0: "reversible", 1: "costly"}


def pick_topic(state: dict, utterance: dict, cfg: dict) -> str:
    """Тема определяется фактом состояния игры, а не догадкой о смысле фразы."""
    t = cfg["topics"]
    for name in t["order"]:
        if name == "channel":
            if utterance.get("channel"):
                return "channel:" + utterance["channel"]
        elif name == "dialogue":
            if state.get("core.context.menuOpen") and \
               state.get("core.context.menuName") in t["dialogueMenuNames"]:
                return "dialogue"
        elif name == "menu":
            if state.get("core.context.menuOpen") and \
               state.get("core.context.menuName") not in t["menuIgnore"]:
                return "menu"
        elif name == "combat":
            if state.get("core.player.inCombat"):
                return "combat"
        elif name == "world":
            return "world"
    return "world"


def offered_to(subscribers: list, topic: str) -> list:
    """Кому вообще уйдёт предложение. Реплика попадает ровно в одну тему."""
    base = topic.split(":", 1)[0]
    out = []
    for s in subscribers:
        if not s.get("active", True):
            continue
        topics = s.get("topics", [])
        if base in topics or topic in topics:
            out.append(s["ns"])
    return out


def state_status(state: dict, providers: list, key: str) -> int:
    """0 - спросить некому, 1 - значение есть, 2 - поставщик не смог."""
    namespace = key.split(".", 1)[0]
    if namespace not in providers:
        return 0
    if key not in state:
        return 0
    return 2 if state[key] is None else 1


def run_auction(utterance: dict, bids: list, cfg: dict) -> dict:
    """bids: [{"ns":..., "confidence":..., "costClass":0|1, "greedy":bool}]

    Жадность - это заявка на исключительность, и она срабатывает только если
    заявитель победил. Правило:

      выиграл жадный      -> результат достаётся ему одному;
      выиграл нежадный    -> жадные исключаются целиком, результат делится
                             между всеми оставшимися нежадными.

    Жадный, попросивший "мне одному или никак", в раздаче не участвует:
    он сам отказался делиться.
    """
    a = cfg["auction"]
    trace = []

    if utterance.get("score", 0.0) < a["minUtteranceScore"]:
        return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
                "reason": "реплика ниже minUtteranceScore",
                "trace": trace}

    survivors = []
    for b in bids:
        cls = COST_NAMES[b["costClass"]]
        need = a["minConfidence"][cls]
        ok = b["confidence"] >= need
        trace.append({"ns": b["ns"], "class": cls, "confidence": b["confidence"],
                      "needConfidence": need, "passed": ok})
        if ok:
            survivors.append(b)

    if not survivors:
        return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
                "reason": "ни одна ставка не прошла порог уверенности",
                "trace": trace}

    order = a.get("priority", [])

    def rank(b):
        # детерминированный порядок: уверенность, затем список игрока, затем имя
        p = order.index(b["ns"]) if b["ns"] in order else len(order)
        return (-b["confidence"], p, b["ns"])

    survivors.sort(key=rank)
    top = survivors[0]
    cls = COST_NAMES[top["costClass"]]

    if len(survivors) > 1:
        second = survivors[1]
        margin = top["confidence"] - second["confidence"]
        need = a["minMargin"][cls]
        if margin < need:
            return _break_tie(survivors, bids, margin, need, a, trace)

    return _share(top, survivors,
                  "уверенность %.2f при пороге %.2f" % (top["confidence"], a["minConfidence"][cls]),
                  trace)


def _break_tie(survivors: list, bids: list, margin: float, need: float,
               a: dict, trace: list) -> dict:
    """Ничья: уверенность спорщиков больше не разведёт.

    Одна и та же запись словаря даёт одно и то же число всегда, поэтому ждать,
    что в другой раз кто-то опередит, бессмысленно - нужно правило. Правил три,
    и порядок между ними важен: сначала прямое указание игрока, потом вопрос,
    об одном ли вообще спор, и только потом готовность делиться.
    """
    order = a.get("priority", [])
    tied = [b for b in survivors if survivors[0]["confidence"] - b["confidence"] < need]

    # 1. Порядок из настроек - прямое указание игрока, и оно старше любых
    #    наших рассуждений о том, что он имел в виду.
    places = [(order.index(b["ns"]) if b["ns"] in order else len(order), b) for b in tied]
    best = min(place for place, _ in places)
    if best < len(order) and sum(1 for place, _ in places if place == best) == 1:
        winner = next(b for place, b in places if place == best)
        return {"winner": winner["ns"], "winners": [winner["ns"]],
                "denied": [b["ns"] for b in bids if b["ns"] != winner["ns"]],
                "reason": "отрыв %.2f < %.2f, спор решён порядком из настроек" % (margin, need),
                "trace": trace}

    # 2. Порядок молчит. Разные команды при неразличимой уверенности - это
    #    двусмысленная реплика: понять её можно двояко, и оба понимания равно
    #    правдоподобны. Пустая фраза значит "неизвестно" и считается разной.
    phrases = {b.get("phrase", "") for b in tied}
    if len(phrases) != 1 or "" in phrases:
        return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
                "reason": "отрыв %.2f < %.2f, фразу поняли по-разному - реплика двусмысленна"
                          % (margin, need),
                "trace": trace}

    # 3. Спор об одной команде. Решает объявленная готовность делиться: жадный
    #    сам сказал "мне одному или никак" и выбывает по собственному условию.
    if a.get("sharedWinsTie", True):
        winners = [b["ns"] for b in survivors if not b.get("greedy")]
        if winners:
            return {"winner": winners[0], "winners": winners,
                    "denied": [b["ns"] for b in bids if b["ns"] not in winners],
                    "reason": "отрыв %.2f < %.2f, одну команду просят несколько - её делают те, кто делится"
                              % (margin, need),
                    "trace": trace}

    return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
            "reason": "отрыв %.2f < %.2f, одну команду просят несколько и все требуют её себе"
                      % (margin, need),
            "trace": trace}

def _share(top: dict, survivors: list, reason: str, trace: list) -> dict:
    """Кому достаётся результат после того, как победитель определён."""
    if top.get("greedy"):
        winners = [top["ns"]]
        reason += "; победитель жадный - результат только ему"
    else:
        winners = [b["ns"] for b in survivors if not b.get("greedy")]
        if len(winners) > 1:
            reason += "; победитель нежадный - результат делится между %d" % len(winners)
        else:
            reason += "; победитель нежадный, делить не с кем"

    return {"winner": winners[0], "winners": winners,
            "denied": [b["ns"] for b in survivors if b["ns"] not in winners],
            "reason": reason, "trace": trace}
