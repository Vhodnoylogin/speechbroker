# -*- coding: utf-8 -*-
"""A run of the reference engine over recorded sound - no microphone and no game.

The sound is fed in blocks, the way a microphone does it, so the engine behaves
exactly as it did in live work: the same thresholds, the same passes of the
models, the same argument. The one difference is that time runs faster than real.

Kept as one module because these ten lines had already been copied into two
scripts, and every copy managed to drift from the others in small ways: here
the loudness threshold was computed differently, there the tail of silence was
shorter. Runs taken with different copies of the same thing cannot be compared.
"""
from typing import Callable, List, Optional

import numpy as np

from .engine.models import ModelAdapter
from .engine.parts import Slice

RATE = 16000
BLOCK_MS = 64


def loudness(blocks: List[np.ndarray]) -> np.ndarray:
    return np.array([float(np.sqrt(np.mean(np.square(b))) * 32768) for b in blocks])


def trigger_level(level: np.ndarray) -> float:
    """The loudness threshold - from the recording itself, not a number in the code.

    Every microphone and every room has its own floor of silence. The level is
    taken over the WHOLE recording, not its beginning: in a synthesised file
    the speech starts from the very first block, and "the silence at the start"
    would come out louder than the speech.
    """
    floor = float(np.percentile(level, 20))
    return max(120.0, floor * 3.5, float(np.percentile(level, 60)) * 0.35)


def run(pcm: np.ndarray, settings: dict, adapters: List[ModelAdapter],
        vocabulary: List[str] = None,
        log: Callable[[str], None] = None,
        on_hypotheses: Callable[[int, list], None] = None,
        block_ms: int = BLOCK_MS,
        on_slice: Callable[[Slice], None] = None) -> List[Slice]:
    """Run a recording through and return the pieces the engine would hand to a bridge.

    `on_slice` is for a report that has to keep the ORDER of what happened: a
    piece and a late hypothesis about an earlier piece arrive interleaved, and
    describing the pieces afterwards would put every late hypothesis before
    every piece. Whoever only wants the result ignores it - the pieces are
    returned anyway.
    """
    from .engine.engine import VoiceRecognizeEngine

    pieces: List[Slice] = []
    clock = {"ms": 0}

    def caught(piece: Slice) -> None:
        # The stamp is put here and not inside the engine: the engine does not
        # know where the sound comes from, and it has no clock. Here there is
        # one - it is the position of the feed.
        piece.emitted_ms = clock["ms"]
        pieces.append(piece)
        if on_slice is not None:
            on_slice(piece)

    engine = VoiceRecognizeEngine(RATE, settings, log or (lambda _m: None),
                                 caught, on_hypotheses or (lambda *_: None))
    for adapter in adapters:
        engine.pool.register(adapter)
    engine.set_vocabulary(vocabulary or [])

    step = int(RATE * block_ms / 1000)
    blocks = [pcm[at:at + step] for at in range(0, len(pcm), step)]
    blocks = [b for b in blocks if len(b)]
    if not blocks:
        engine.stop()
        return pieces

    level = loudness(blocks)
    trigger = trigger_level(level)
    if log:
        log("    silence floor %.0f, loudness trigger %.0f"
            % (float(np.percentile(level, 20)), trigger))
    for block, rms in zip(blocks, level):
        clock["ms"] += block_ms
        engine.feed(block, rms >= trigger, block_ms)

    # A tail of silence, so that the turn closes the way it does in life.
    quiet = np.zeros(step, dtype="float32")
    for _ in range(40):
        clock["ms"] += block_ms
        engine.feed(quiet, False, block_ms)

    engine.stop()
    return pieces


def last_slice(pieces: List[Slice]) -> Optional[Slice]:
    """The tail piece - the only one whose completeness is in question.

    All the others are finished by construction: speech follows them in the
    same re-reading, so the speaker finished them.
    """
    return pieces[-1] if pieces else None


def describe(piece: Slice) -> List[str]:
    """The lines a report prints for one piece."""
    lines = ["", "  PIECE %d  [%d..%d ms]  class %s  complete %.2f%s"
             % (piece.id, piece.start_ms, piece.end_ms, piece.length_class, piece.complete,
                "  supersedes %s" % piece.supersedes if piece.supersedes else "")]
    for n, h in enumerate(piece.hypotheses, 1):
        lines.append("      %d) %-46s score %.2f  models %d  (%s)"
                     % (n, '"%s"' % h.text, h.score, h.agreed, h.model))
    return lines
