# -*- coding: utf-8 -*-
"""Build the reference corpus the models are benched on.

    python -m audiolab corpus

The sound is synthesised, so the reference text is known EXACTLY - we
pronounced it ourselves. That is the footing live speech cannot give: there is
nothing to check the correctness of a transcription against on a human voice.

The corpus is uneven on purpose: single commands, four sentences in a row, the
case of "a command that turned out to be the beginning of a phrase", and plain
silence - the check against invention. What is in it is written in the settings,
under `bench.items`, and the synthesis itself is the Studio's, the same one the
page uses.
"""
import argparse
from typing import List

from . import ROOT, load
from ..files import Library
from ..studio import Studio


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab corpus",
        description="Synthesise the reference corpus out of `bench.items` of the settings.")
    parser.add_argument("--overwrite", action="store_true",
                        help="re-synthesise the recordings that already exist")
    args = parser.parse_args(argv)

    manifest = load()
    bench = manifest.bench
    items = bench.get("items") or {}
    if not items:
        print("`bench.items` is empty in %s - there is nothing to synthesise" % manifest.path.name)
        return 2

    made = Studio.load(ROOT)
    if made.synth is None:
        print("the voice of the synthesis is not set: the `voice` key of %s"
              % manifest.path.name)
        return 2
    # The corpus is not the folder of takes, and the settings must not learn
    # otherwise: which folder is being filled is a property of this run.
    made.library = Library(bench["folder"])

    print("corpus: %s" % bench["folder"])
    for name, item in items.items():
        if made.library.take(name) is not None and not args.overwrite:
            print("  %-24s exists, skipped (--overwrite to replace)" % name)
            continue
        answer = made.synthesize(name, list(item.get("sentences", [])),
                                 list(item.get("gapsMs", [])))
        if not answer.get("ok"):
            print("  %-24s FAILED: %s" % (name, answer.get("error", "")))
            return 2
        take = answer["take"]
        print("  %-24s %5.2f s  %d sentences"
              % (name, take["seconds"], len(item.get("sentences", []))))

    print("next: python -m audiolab bench")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
