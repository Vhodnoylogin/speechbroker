# -*- coding: utf-8 -*-
"""Walking a person through a list of prompts with the Studio, one take per prompt.

Why a live voice when there is a synthesised one. Synthesis is good where
repeatability is wanted: the reference text is known exactly, the recording
reproduces byte for byte, a parameter can be swept to see where a threshold
breaks. But its prosody is artificial - the pauses are exactly the ones we
inserted, the intonation is flat, the voice sterile. And completeness is
measured PRECISELY by prosody, so tuning it on synthesis means tuning to an
artefact.

That is why the prompt list in the manifest is built of minimal pairs: the
same words said two ways. The difference between them is what our measurement
of the tone has to catch.

No recording code lives here and none should: the Studio already has it,
tested, and knows how to choose an input. This module only leads the person
and names the files. A prompt that says whether its phrase is `finished` gets
the `-end`/`-open` suffix on its name, so a take recorded through the list is
labelled for the completeness measurement without a separate action.
"""
import sys
from typing import List, Optional

from .files import Library


def utf8_console() -> None:
    """Cyrillic on a Windows console otherwise dies on cp1251.

    This is the one mode where the output has to go to the screen: a person
    reads the phrase they are to say. So the encoding is fixed rather than
    worked around by writing to a file.
    """
    try:
        if sys.platform == "win32":
            import ctypes
            ctypes.windll.kernel32.SetConsoleOutputCP(65001)
            ctypes.windll.kernel32.SetConsoleCP(65001)
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def pick_input(studio, wanted: Optional[int]) -> None:
    """Choose an input. A named one silently, otherwise ask."""
    if wanted is not None:
        studio.choose(input_index=wanted)
        return

    print("\nInputs:")
    for device in studio.devices()["input"]:
        print("  %-4s %s" % (device["index"] if device["index"] is not None else "-",
                             device["title"]))
    print("\nChoose the number of the input (empty - the default).")
    print("Careful: an input that gives a flat zero yields empty takes, and by the")
    print("file it is too late to tell. Listen to the first take.")
    raw = input("input: ").strip()
    studio.choose(input_index=int(raw) if raw else -1)


def take_name(prompt: dict, prefix: str, marks: dict) -> str:
    name = "%s-%s" % (prefix, prompt["name"]) if prefix else prompt["name"]
    if prompt.get("finished") is True:
        name += marks.get("endSuffix", "-end")
    elif prompt.get("finished") is False:
        name += marks.get("openSuffix", "-open")
    return name


def record_prompts(studio, prompts: List[dict], corpus, prefix: str, marks: dict,
                   overwrite: bool = False) -> List[str]:
    """Lead the person through the prompts and put the takes into the corpus.

    Returns the names recorded, so that a measurement can be limited to them.
    The library of the studio is pointed at the corpus for the duration
    without touching audio.json: which folder is measured is a property of
    this run, not of the studio.
    """
    studio.library = Library(corpus)
    names = []

    print("=" * 72)
    print("RECORDING. %d prompts. Read the whole list now, so as not to look at" % len(prompts))
    print("the screen later.")
    print("=" * 72)
    for n, prompt in enumerate(prompts, 1):
        text = prompt.get("text", "")
        print("\n%d. %s" % (n, ('"%s"' % text) if text else "(silence)"))
        if prompt.get("how"):
            print("   %s" % prompt["how"])
    print("\n" + "=" * 72)

    for n, prompt in enumerate(prompts, 1):
        text = prompt.get("text", "")
        name = take_name(prompt, prefix, marks)
        if studio.library.take(name) is not None and not overwrite:
            print("\n--- %d/%d  %s  already exists, skipped (--overwrite to replace)"
                  % (n, len(prompts), name))
            continue

        print("\n--- %d/%d  %s ---" % (n, len(prompts), name))
        print("    %s" % (('"%s"' % text) if text else "(say NOTHING - this is a silence probe)"))
        if prompt.get("how"):
            print("    %s" % prompt["how"])

        input("    Enter - start recording ")
        started = studio.start(name, text)
        if not started.get("ok"):
            print("    could not open the input: %s" % started.get("error", ""))
            return names

        input("    Enter - stop ")
        studio.stop(autosave=True)

        take = studio.library.take(name)
        if take is None:
            print("    the take was not saved")
            continue
        note = ""
        if text.strip() and take.peak < 0.01:
            note = "   SILENCE - the input looks dead"
        print("    recorded %.2f s, peak %.0f%%%s" % (take.seconds, take.peak * 100, note))
        names.append(name)

    studio.recorder.close()
    return names
