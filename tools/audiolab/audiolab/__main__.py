# -*- coding: utf-8 -*-
"""The one entry point of the tooling.

    python -m audiolab                    the list of commands
    python -m audiolab <command> --help   what that command takes
    audiolab.cmd <command>                the same, with the interpreter found first

One entry point and not seven scripts, because the settings, the corpus and the
reference engine are one for all of them: a script of its own would sooner or
later read a folder of its own.
"""
import importlib
import sys
from typing import List

COMMANDS = [
    ("record", "walk through the prompts and record the takes"),
    ("listen", "record one take and show what the engine made of it"),
    ("calibrate", "how far each model may be believed, and where completeness divides"),
    ("bench", "the same measurement over the synthesised corpus"),
    ("corpus", "synthesise that corpus"),
    ("replay", "one recording through the engine, piece by piece"),
    ("scenario", "the live takes turned into a scenario for the bridge"),
    ("prosody", "does the measurement of the tone tell a finished phrase from a cut-off one"),
]


def usage() -> int:
    print(__doc__.strip())
    print("")
    for name, what in COMMANDS:
        print("    %-11s %s" % (name, what))
    print("")
    print("The studio - recording and listening by hand - is a page of its own:"
          " studio.cmd")
    return 2


def main(argv: List[str] = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "help"):
        return usage()

    name = argv[0]
    if name not in [c for c, _ in COMMANDS]:
        print("no such command: %s" % name)
        return usage()

    from .prompts import utf8_console

    utf8_console()
    module = importlib.import_module("audiolab.cli.%s" % name)
    return module.main(argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
