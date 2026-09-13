"""The reference implementation of what the bridge owns: the choice of topic,
the snapshot and the auction.

This is an executable specification. The C++ is obliged to behave exactly the
same; a divergence between here and the game means a fault in the plugin, not in
the scenario.

The logic of a subscriber mod does NOT belong here: how it decides whether to
bid and with what confidence is its own business. The scenario brings bids that
are already made.

Every threshold comes from config/envoy.default.json. There are no constants in
the code.
"""
from __future__ import annotations

COST_NAMES = {0: "reversible", 1: "costly"}


def pick_topic(state: dict, utterance: dict, cfg: dict) -> str:
    """The topic is settled by the fact of the state of the game, not by a guess
    at the sense of the phrase."""
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
    """Who the offer goes to at all. An utterance lands in exactly one topic."""
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
    """0 - nobody to ask, 1 - there is a value, 2 - the provider could not."""
    namespace = key.split(".", 1)[0]
    if namespace not in providers:
        return 0
    if key not in state:
        return 0
    return 2 if state[key] is None else 1


def run_auction(utterance: dict, bids: list, cfg: dict) -> dict:
    """bids: [{"ns":..., "confidence":..., "costClass":0|1, "greedy":bool}]

    Greed is a claim to exclusivity, and it only fires if the one claiming it won.
    The rule:

      a greedy one won      -> the result goes to it alone;
      a non-greedy one won  -> the greedy are excluded entirely and the result is
                               shared between all the remaining non-greedy ones.

    A greedy one that asked for "mine alone or not at all" takes no part in the
    sharing: it refused to share itself.
    """
    a = cfg["auction"]
    trace = []

    if utterance.get("score", 0.0) < a["minUtteranceScore"]:
        return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
                "reason": "the utterance is below minUtteranceScore",
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
                "reason": "not one bid passed the confidence threshold",
                "trace": trace}

    order = a.get("priority", [])

    def rank(b):
        # a deterministic order: confidence, then the list of the player, then the name
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
                  "confidence %.2f at a threshold of %.2f" % (top["confidence"], a["minConfidence"][cls]),
                  trace)


def _break_tie(survivors: list, bids: list, margin: float, need: float,
               a: dict, trace: list) -> dict:
    """A tie: confidence will not separate the arguers any further.

    One and the same vocabulary entry always gives one and the same number, so
    waiting for somebody to come out ahead next time is pointless - a rule is
    needed. There are three rules, and the order between them matters: first the
    direct instruction of the player, then the question whether the argument is
    about one thing at all, and only then the willingness to share.
    """
    order = a.get("priority", [])
    tied = [b for b in survivors if survivors[0]["confidence"] - b["confidence"] < need]

    # 1. The order from the settings is a direct instruction from the player, and
    #    it outranks any reasoning of ours about what they meant.
    places = [(order.index(b["ns"]) if b["ns"] in order else len(order), b) for b in tied]
    best = min(place for place, _ in places)
    if best < len(order) and sum(1 for place, _ in places if place == best) == 1:
        winner = next(b for place, b in places if place == best)
        return {"winner": winner["ns"], "winners": [winner["ns"]],
                "denied": [b["ns"] for b in bids if b["ns"] != winner["ns"]],
                "reason": "margin %.2f < %.2f, the argument was settled by the order from the settings" % (margin, need),
                "trace": trace}

    # 2. The order says nothing. Different commands at indistinguishable
    #    confidence mean an ambiguous utterance: it can be understood two ways
    #    and both readings are equally plausible. An empty phrase means
    #    "unknown" and counts as different.
    phrases = {b.get("phrase", "") for b in tied}
    if len(phrases) != 1 or "" in phrases:
        return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
                "reason": "margin %.2f < %.2f, the phrase was understood differently - the utterance is ambiguous"
                          % (margin, need),
                "trace": trace}

    # 3. An argument about one command. It is settled by the declared willingness
    #    to share: a greedy one said "mine alone or not at all" itself and drops
    #    out by its own condition.
    if a.get("sharedWinsTie", True):
        winners = [b["ns"] for b in survivors if not b.get("greedy")]
        if winners:
            return {"winner": winners[0], "winners": winners,
                    "denied": [b["ns"] for b in bids if b["ns"] not in winners],
                    "reason": "margin %.2f < %.2f, several ask for one command - it is done by those that share"
                              % (margin, need),
                    "trace": trace}

    return {"winner": None, "winners": [], "denied": [b["ns"] for b in bids],
            "reason": "margin %.2f < %.2f, several ask for one command and all demand it for themselves"
                      % (margin, need),
            "trace": trace}

def _share(top: dict, survivors: list, reason: str, trace: list) -> dict:
    """Who gets the result once the winner has been settled."""
    if top.get("greedy"):
        winners = [top["ns"]]
        reason += "; the winner is greedy - the result goes to it alone"
    else:
        winners = [b["ns"] for b in survivors if not b.get("greedy")]
        if len(winners) > 1:
            reason += "; the winner is not greedy - the result is shared between %d" % len(winners)
        else:
            reason += "; the winner is not greedy, but there is nobody to share with"

    return {"winner": winners[0], "winners": winners,
            "denied": [b["ns"] for b in survivors if b["ns"] not in winners],
            "reason": reason, "trace": trace}
