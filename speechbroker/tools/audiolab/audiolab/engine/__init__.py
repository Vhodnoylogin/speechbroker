# -*- coding: utf-8 -*-
"""The reference recognition engine, in python. NOT the one that runs in the game.

    THE RUNTIME IMPLEMENTATION IS THE C++ UNDER adapter-voice/src/.
    This package is reference tooling only: it exists so that the benches, the
    calibration and the offline replay can drive a whole recognition path from
    a wav file to the pieces a bridge would receive, without the game, and so
    that every number in the adapter can be traced back to the line of python
    it was measured with (docs/dissolving-the-voice-branch.md).

    parts       what speech is made of on its way from the microphone to the bridge
    judge       whether the sentence finished on a piece
    turn        one speaking turn: what was said, what was handed out, when to ask
    prosody     our own measurement of the tone of the speaker
    arbiter     the argument of several models over the same sound
    models      the model driver (faster-whisper) and the pool that deals sound out
    reputation  measured against declared; the half with a job here produces reputations.json
    engine      the assembly of all of the above
    cuda        the CUDA DLL path helper the python driver needs

Where a C++ file says `engine/<name>.py:<line>` it refers to the file of the same
name in this package as it stood on the `voice` branch; the reasoning has been kept
line for line where it could be, and only translated.

Each layer hides the transport of the next: the bridge does not know how the engine
takes sound, the engine does not know how a driver reaches its model.
"""
from .arbiter import Arbiter
from .engine import VoiceRecognizeEngine
from .judge import CompletenessJudge
from .models import ModelAdapter, ModelPool, WhisperAdapter
from .parts import Fragment, Hypothesis, ModelAnswer, Slice
from .turn import Pacer, SpeechTurn

__all__ = ["Arbiter", "CompletenessJudge", "Fragment", "Hypothesis", "ModelAdapter",
           "ModelAnswer", "ModelPool", "Pacer", "Slice", "SpeechTurn",
           "VoiceRecognizeEngine", "WhisperAdapter"]
