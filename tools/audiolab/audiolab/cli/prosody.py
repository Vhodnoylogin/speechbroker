# -*- coding: utf-8 -*-
"""Does our measurement of the tone tell a finished phrase from an unfinished one.

    python -m audiolab prosody
    python -m audiolab prosody --takes samples/bench

This is the one check that cannot be made by synthesis and cannot be made by
comparing text. In the decisive pair of takes the WORDS ARE THE SAME and the
silence after them is the same - the difference is the intonation alone.
Word-for-word checking gives both a clean sheet, and the case is decided
entirely by the measurement of the terminal fall of the tone.

If the pair does not separate, the measurement is worth nothing and another
feature has to be looked for.
"""
import argparse
from typing import List

from . import load, write_report
from ..calibrate import RATE, trim_tail
from ..files import Library
from ..engine.prosody import PitchRange, terminal_fall, track_f0


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab prosody",
        description="Measure the terminal fall of the tone over every take of a folder.")
    parser.add_argument("--takes", default=None,
                        help="the folder of recordings; by default the corpus of the settings")
    parser.add_argument("--report", default="prosody-check.txt",
                        help="the name of the report inside the folder of reports")
    args = parser.parse_args(argv)

    manifest = load()
    where = manifest.resolve(args.takes) if args.takes else manifest.corpus
    if not where.exists():
        print("no such folder: %s" % where)
        return 2

    lines = ["=== the measurement of the tone: %s ===" % where, "",
             "%-24s %8s %8s %9s %9s" % ("take", "length", "tone low", "tone high", "fall")]

    library = Library(where)
    for take in library.by_name():
        # The tail of silence is cut off: the tone is measured on the last
        # word, not on what comes after it.
        pcm = trim_tail(library.read(take.name))
        if len(pcm) < RATE // 4:
            lines.append("%-24s %8s" % (take.name, "too short"))
            continue

        span = PitchRange()
        step = int(RATE * 0.064)
        for at in range(0, len(pcm) - step, step):
            span.add(track_f0(pcm[at:at + step], RATE))

        fall = terminal_fall(pcm, RATE, span)
        lines.append("%-24s %7.2fs %8s %9s %9s" % (
            take.name, len(pcm) / float(RATE),
            "%.0f Hz" % span.low if span.known else "-",
            "%.0f Hz" % span.high if span.known else "-",
            "%.2f" % fall if fall is not None else "no verdict"))

    lines.append("")
    lines.append("A fall of 1.00 - the voice sat down on the floor of its own range, and the")
    lines.append("phrase sounds finished. A fall of 0.00 - it stayed up: that is how a")
    lines.append("continuation sounds. What decides is not the number itself but the")
    lines.append("DIFFERENCE inside a pair of takes with the same words.")
    write_report(manifest, args.report, lines)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
