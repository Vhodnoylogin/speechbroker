# -*- coding: utf-8 -*-
"""One recording through the engine, without the game and without a microphone.

    python -m audiolab replay takes/door-closed.wav
    python -m audiolab replay door-closed            a take of the corpus, by name

The sound is fed in blocks of 64 ms, the way a microphone does it, so the engine
behaves exactly as it did in live work: the same thresholds, the same passes of
the models, the same argument between them. The one difference is that time runs
faster than real.

The output goes into a file and not onto the screen: the takes are Russian and
printing Cyrillic into a Windows console dies on cp1251.
"""
import argparse
import pathlib
from typing import List

from . import load, write_report
from .. import replay
from ..files import read_wav


def find(manifest, what: str) -> pathlib.Path:
    """A path as given, or the name of a take in the corpus."""
    direct = pathlib.Path(what)
    if direct.exists():
        return direct
    inside = manifest.resolve(what)
    if inside.exists():
        return inside
    named = manifest.corpus / (what + ".wav")
    return named if named.exists() else inside


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab replay",
        description="Run one recording through the reference engine and write down the pieces.")
    parser.add_argument("recording", help="a path to a .wav, or the name of a take in the corpus")
    parser.add_argument("--report", default="engine-replay.txt",
                        help="the name of the report inside the folder of reports")
    args = parser.parse_args(argv)

    manifest = load()
    path = find(manifest, args.recording)
    if not path.exists():
        print("no such recording: %s" % path)
        return 2

    lines: List[str] = ["=== the models ==="]

    def on_hypotheses(slice_id: int, hypotheses) -> None:
        # Hypotheses that came after the piece had already gone out. The prize
        # is not touched - they are written down so that it can be seen what a
        # later pass would have changed.
        lines.append("")
        lines.append("  NEW HYPOTHESES about piece %d (the prize stands)" % slice_id)
        for n, h in enumerate(hypotheses, 1):
            lines.append("      %d) %-46s score %.2f  models %d  (%s)"
                         % (n, '"%s"' % h.text, h.score, h.agreed, h.model))

    pcm = read_wav(path)
    adapters = manifest.adapters()
    lines.append("")
    lines.append("=== sound: %s, %.2f s, in blocks of %d ms ==="
                 % (path.name, len(pcm) / float(replay.RATE), replay.BLOCK_MS))

    # The pieces are written down as they arrive, not afterwards: a late
    # hypothesis about an earlier piece is only worth anything in its place.
    pieces = replay.run(pcm, manifest.engine_settings(), adapters, manifest.vocabulary,
                        log=lines.append, on_hypotheses=on_hypotheses,
                        on_slice=lambda piece: lines.extend(replay.describe(piece)))

    lines.append("")
    lines.append("pieces: %d" % len(pieces))
    write_report(manifest, args.report, lines)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
