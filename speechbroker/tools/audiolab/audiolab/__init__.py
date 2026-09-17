# -*- coding: utf-8 -*-
"""audiolab - everything that takes part in making sound, and in measuring it.

    devices    list the inputs and the outputs, or choose one outright
    capture    record from the chosen input, with a level
    playback   play to the chosen output
    synth      synthesis of speech
    files      the recordings on disk: a WAV beside its reference text
    studio     Studio - the API any interface is pulled over from outside

    engine     the reference recognition engine, in python
    calibrate  the trust of a model, and where completeness divides
    replay     a recording through the engine, in blocks, the way a microphone feeds it
    prompts    walking a person through the list of prompts, one take per prompt
    manifest   the settings and the ground truth of the corpus, as one object
    cli        the commands; `python -m audiolab` lists them

A module of its own because sound is wanted by several and in different ways:
a live stream for the recognition engine, reference files for the calibration,
recording and listening for a person. Keeping that inside one of the consumers
would lock it up there.

    THE ENGINE THAT RUNS IN THE GAME IS THE C++ UNDER adapter-voice/src/.
    What is here is reference tooling: it needs ground truth, which does not
    exist at run time, and it exists so that every number in the adapter can be
    traced back to the line of python it was measured with. See README.md and
    docs/measurements.md.
"""
from .studio import Studio

__all__ = ["Studio"]
