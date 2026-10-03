# -*- coding: utf-8 -*-
"""Record the reference takes: a person is walked through the prompts.

    python -m audiolab record
    python -m audiolab record --input 17 --overwrite

Why a live voice when there is a synthesised one. Synthesis is good where
repeatability is wanted: the reference text is known exactly, the recording
reproduces byte for byte, a parameter can be swept to see where a threshold
breaks. But its prosody is artificial - the pauses are exactly the ones we
inserted, the intonation is flat, the voice sterile. Completeness is measured
PRECISELY by prosody, so tuning it on synthesis means tuning to an artefact.

The prompts live in the labelling manifest and the walk itself in
`audiolab.prompts`; there is no recording code here, and none in either - the
Studio already has it, tested, and knows how to choose an input.
"""
import argparse
from typing import List

from . import load, studio
from ..prompts import pick_input, record_prompts


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab record",
        description="Walk through the prompts of the manifest and record the takes.")
    parser.add_argument("--input", type=int, default=None,
                        help="the number of the input; without it the inputs are listed and asked for")
    parser.add_argument("--prefix", default="",
                        help="a prefix for the names of the takes, to record a second set "
                             "without overwriting the first")
    parser.add_argument("--overwrite", action="store_true",
                        help="re-record the takes that already exist; without it they are skipped")
    args = parser.parse_args(argv)

    manifest = load()
    prompts = manifest.prompts
    if not prompts:
        print("there is not one prompt in %s - `prompts` is empty" % manifest.path.name)
        return 2

    made = studio(manifest)
    pick_input(made, args.input)
    names = record_prompts(made, prompts, manifest.corpus, args.prefix,
                           manifest.completeness, args.overwrite)
    print("\nrecorded %d of %d into %s" % (len(names), len(prompts), manifest.corpus))
    if names:
        print("next: python -m audiolab calibrate")
    return 0 if names else 2


if __name__ == "__main__":
    raise SystemExit(main())
