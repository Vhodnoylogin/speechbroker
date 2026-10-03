# -*- coding: utf-8 -*-
"""Say something and see at once what the engine made of it.

    python -m audiolab listen
    python -m audiolab listen --input 17 --name proba

Enter starts a take, Enter ends it, and the recording goes straight through the
reference engine: the pieces, their completeness and the argument of the models
appear as they would have appeared in the game. Ctrl+C leaves.

The difference from the live listening the dissolved service did: there the
sound reached the engine while it was still being spoken, here it reaches it a
moment later, block by block, faster than real time. The engine sees the same
blocks in the same order and behaves the same way - and the take stays on disk,
so what has just been heard can be measured again without opening one's mouth.
The microphone of the game belongs to the adapter and to nothing else; this is
a tool, and it takes the microphone only while a person is standing over it.
"""
import argparse
from typing import List

from . import load, studio
from .. import replay
from ..prompts import pick_input


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab listen",
        description="Record one take and run the reference engine over it, again and again.")
    parser.add_argument("--input", type=int, default=None,
                        help="the number of the input; without it the inputs are listed and asked for")
    parser.add_argument("--name", default="live",
                        help="the prefix of the names the takes are saved under (default: live)")
    args = parser.parse_args(argv)

    manifest = load()
    made = studio(manifest)
    pick_input(made, args.input)

    settings = manifest.engine_settings()
    vocabulary = manifest.vocabulary
    # The models are brought up once and kept: bringing whisper up costs
    # seconds, and a person waiting for them between every phrase would stop
    # using the thing.
    adapters = manifest.adapters()
    for adapter in adapters:
        adapter.start()

    print("=" * 72)
    print("SPEAK. Enter starts a take, Enter ends it. Ctrl+C leaves.")
    print("=" * 72)

    number = 0
    try:
        while True:
            number += 1
            name = "%s-%03d" % (args.name, number)
            input("\nEnter - start recording %s " % name)
            started = made.start(name, "")
            if not started.get("ok"):
                print("could not open the input: %s" % started.get("error", ""))
                return 2
            input("Enter - stop ")
            made.stop(autosave=True)

            take = made.library.take(name)
            if take is None:
                print("the take was not saved")
                continue
            print("recorded %.2f s, peak %.0f%%" % (take.seconds, take.peak * 100))

            pieces = replay.run(made.library.read(name), settings, adapters, vocabulary,
                                log=lambda line: print("  " + line))
            if not pieces:
                print("  the engine produced not one piece")
            for piece in pieces:
                for line in replay.describe(piece):
                    print(line)
    except KeyboardInterrupt:
        print("")
    finally:
        made.recorder.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
