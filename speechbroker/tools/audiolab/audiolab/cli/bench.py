# -*- coding: utf-8 -*-
"""The models over the synthesised corpus, by one command.

    python -m audiolab corpus      build the corpus
    python -m audiolab bench       run the models over it

It measures the same thing and with the same code as `calibrate`: all the work
lies in `audiolab.calibrate`, and there is no second copy of it here. The
difference is only the input - here it is always the synthesised corpus, whose
reference text is known EXACTLY because we pronounced it ourselves, and
completeness is not measured at all: synthesised speech is even in tempo and has
no terminal fall of the tone, so a label on it would be a label on an artefact.
"""
import argparse
from typing import List

from . import load
from .calibrate import run


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab bench",
        description="Run every model over the synthesised corpus and write the word error rate.")
    parser.add_argument("--corpus", default=None,
                        help="another folder of recordings; by default `bench.folder` of the settings")
    parser.add_argument("--report", default=None,
                        help="the name of the report inside the folder of reports")
    args = parser.parse_args(argv)

    manifest = load()
    corpus = manifest.resolve(args.corpus) if args.corpus else manifest.bench["folder"]
    if not corpus.exists():
        print("no such corpus: %s - build it: python -m audiolab corpus" % corpus)
        return 2

    report = args.report or ("engine-bench-%s.txt" % corpus.name)
    return run(manifest, corpus, "trust", None, report, "the reference corpus",
               live_only=False)


if __name__ == "__main__":
    raise SystemExit(main())
