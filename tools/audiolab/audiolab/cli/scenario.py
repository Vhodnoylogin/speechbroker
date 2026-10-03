# -*- coding: utf-8 -*-
"""Live sound -> the pieces of the engine -> a scenario for the bridge.

    python -m audiolab scenario
    python -m audiolab scenario --takes takes --out ../../bridge/tests/scenarios/live.json

This is the joint of the two halves. Until it existed the engine was checked
separately (sound -> text) and the bridge separately (text -> a winner), and
between them lay a file of lines written by hand. Here the lines are written by
the engine: every take is fed in blocks of 64 ms, the way a microphone feeds it,
and every piece the engine gives out becomes a step of the scenario.

What goes into a step: the best hypothesis as text, its score, the margin to the
second one, the rest of the hypotheses, the completeness, the class of length,
the number of the piece and which pieces it supersedes. The state of the game is
not set by sound - it comes from an optional file of the stage, which says which
take is played in a fight and which in a dialogue.

Where the scenario goes is a key of the settings (`scenario.out`) and not a path
in the code: the bridge is a part of the module of its own and may be checked
out anywhere. The file itself is derived and is not versioned.
"""
import argparse
import json
from typing import List

from . import load, write_report
from .. import replay
from ..files import Library

LENGTH_CLASS = {"short": 0, "middle": 1, "long": 2}


def step_of(piece, take: str, reference: str, state) -> dict:
    best = piece.hypotheses[0] if piece.hypotheses else None
    second = piece.hypotheses[1] if len(piece.hypotheses) > 1 else None

    step = {
        "_": "%s, piece %d" % (take, piece.id),
        "source": take,
        "reference": reference,
        "text": piece.text,
        "score": round(best.score, 3) if best else 0.0,
        # The margin is the distance to the second hypothesis. One hypothesis
        # means there is nothing to argue with, and the margin is the whole of it.
        "margin": round(best.score - second.score, 3) if (best and second)
                  else (round(best.score, 3) if best else 0.0),
        "alternatives": [{"text": h.text, "score": round(h.score, 3)}
                         for h in piece.hypotheses[1:]],
        "complete": round(piece.complete, 3),
        "lengthClass": LENGTH_CLASS.get(piece.length_class, 0),
        "sliceId": piece.id,
        "supersedes": list(piece.supersedes),
        "durationMs": piece.duration_ms,
        # When the piece was given out. The host plays the steps by these
        # stamps and not one after another: holding a piece back is only
        # checked when the continuation arrives after as much time as it would
        # have taken in life.
        "emitMs": piece.emitted_ms,
        "engine": "voice",
        "language": "ru",
    }
    if state:
        step["state"] = state
    return step


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab scenario",
        description="Build a scenario for the bridge out of the live takes.")
    parser.add_argument("--takes", default=None,
                        help="the folder of takes; by default the corpus of the settings")
    parser.add_argument("--out", default=None,
                        help="where to write the scenario; by default `scenario.out` of the settings")
    parser.add_argument("--stage", default=None,
                        help="the file saying which take belongs to which state of the game; "
                             "by default `scenario.stage` of the settings")
    parser.add_argument("--report", default="engine-scenario.txt",
                        help="the name of the trace inside the folder of reports")
    args = parser.parse_args(argv)

    manifest = load()
    settings = manifest.scenario
    corpus = manifest.resolve(args.takes) if args.takes else manifest.corpus
    out = manifest.resolve(args.out) if args.out else settings["out"]
    stage_path = manifest.resolve(args.stage) if args.stage else settings.get("stage")

    stage = {}
    if stage_path and stage_path.exists():
        stage = json.loads(stage_path.read_text(encoding="utf-8"))
        stage.pop("_", None)

    # Only the live takes: synthesis is no good for this check. Synthesised
    # speech is even in tempo and has no terminal fall of the tone, so the
    # boundaries of the pieces in it are put where a person would not have put
    # them - and the whole point here is that the lines are not written by hand.
    library = Library(corpus)
    takes = [t for t in library.by_name() if t.live]
    if not takes:
        print("not one live take in %s" % corpus)
        return 2

    steps: List[dict] = []
    trace = ["=== the engine over the live takes ===", ""]
    # The drivers are made once for all the takes: the weights are loaded on
    # the first one, and every engine after that registers the same drivers.
    adapters = manifest.adapters()
    settings_of_engine = manifest.engine_settings()

    for take in takes:
        pcm = library.read(take.name)
        pieces = replay.run(pcm, settings_of_engine, adapters, manifest.vocabulary)
        trace.append("%-16s %5.2f s -> pieces %d"
                     % (take.name, len(pcm) / float(replay.RATE), len(pieces)))

        if not pieces:
            # Zero pieces is a result too, and it needs a step: the bridge has
            # to be able to receive silence and play nothing.
            trace.append("      (the engine gave out not one piece)")
            silent = {"_": "%s, the engine kept quiet" % take.name, "source": take.name,
                      "reference": take.reference, "text": "", "score": 0.0, "margin": 0.0}
            # The stage goes in only when there is one: an empty "state" would
            # mean "a state is set and it is none", which is not the same thing
            # as "no state is set".
            if stage.get(take.name):
                silent["state"] = stage[take.name]
            steps.append(silent)
            continue

        for piece in pieces:
            trace.append('      piece %d [%d..%d] complete %.2f  "%s"'
                         % (piece.id, piece.start_ms, piece.end_ms, piece.complete, piece.text))
            steps.append(step_of(piece, take.name, take.reference, stage.get(take.name)))

    scenario = {
        "name": "Live sound through the engine",
        "_": "Built by `audiolab scenario` out of the takes in %s. The texts and the scores "
             "are not written by hand - they are what the engine gave out." % corpus,
        "steps": steps,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(scenario, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    trace.append("")
    trace.append("takes %d, steps %d -> %s" % (len(takes), len(steps), out))
    write_report(manifest, args.report, trace)
    print("scenario: %s" % out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
