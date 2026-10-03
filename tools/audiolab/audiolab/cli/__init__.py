# -*- coding: utf-8 -*-
"""The commands of the tooling: one module per command, all of them thin.

    python -m audiolab <command> [options]
    python -m audiolab.cli.<command> [options]      the same thing, directly

Every one of these modules only reads the settings, calls the package and puts
the answer somewhere. Not a single measurement lives here: they live in
`audiolab.calibrate`, `audiolab.replay`, `audiolab.prompts` and the reference
engine, so that two commands measuring the same thing cannot drift apart. That
already happened once on the branch these came from - the loudness threshold
was computed one way in the replay and another in the scenario builder, and
reports taken with the two could not be compared.

    record      walk a person through the prompts and record the takes
    listen      record one take and immediately show what the engine made of it
    calibrate   how far each model may be believed, and where completeness divides
    bench       the same measurement over the synthesised corpus
    corpus      synthesise that corpus
    replay      one recording through the engine, piece by piece
    scenario    the live takes turned into a scenario for the bridge
    prosody     does our measurement of the tone tell a finished phrase from a cut-off one

A report goes to a file rather than to the screen, and for a reason: the takes
and their reference texts are Russian, and printing Cyrillic into a Windows
console dies on cp1251. What a person needs to watch while it runs is printed
as it goes; the numbers are written down.
"""
import pathlib
from typing import List

from ..manifest import Manifest

# cli -> audiolab -> the folder of the module, where audio.json lies.
ROOT = pathlib.Path(__file__).resolve().parents[2]


def load() -> Manifest:
    return Manifest.load(ROOT)


def studio(manifest: Manifest):
    """The studio, pointed at the same folder of takes the manifest measures."""
    from ..studio import Studio

    made = Studio.load(ROOT)
    if made.library.root != manifest.corpus:
        made.set_folder(str(manifest.corpus))
    return made


def write_report(manifest: Manifest, name: str, lines: List[str]) -> pathlib.Path:
    path = manifest.reports / name
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("report: %s (%d lines)" % (path, len(lines)))
    return path
