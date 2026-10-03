# -*- coding: utf-8 -*-
"""The tone of the speaker: our own measurement, independent of the language model.

Reference only: the runtime is `adapter-voice/src/turn/Prosody.cpp`.
See the package docstring.

Why it is needed separately. The punctuation of Whisper is three quarters
language, not acoustics: it has seen millions of texts where a lone noun stands
with a full stop, and it will put a full stop after "Fireball" no matter how it
sounded. Exactly where it matters most to us - did the speaker finish or will
they go on - its prior overrides the sound.

A terminal fall of the tone to the bottom of the speaker's range carries no such
prior: it is a measurement, not a guess at how people usually write. So we
measure it ourselves.

Computed by autocorrelation - no external libraries, and almost free; for our
purpose accuracy to the hertz is not needed, only whether the last word sank to
the floor of its own range.
"""
from dataclasses import dataclass, field
from typing import List, Optional

import numpy as np

MIN_HZ = 70.0
MAX_HZ = 350.0
FRAME_MS = 40
HOP_MS = 20


def track_f0(pcm: np.ndarray, rate: int) -> List[float]:
    """The fundamental per frame. Zero where there is no voice."""
    frame = int(rate * FRAME_MS / 1000)
    hop = int(rate * HOP_MS / 1000)
    if len(pcm) < frame:
        return []

    lo = int(rate / MAX_HZ)
    hi = min(int(rate / MIN_HZ), frame - 1)
    if hi <= lo:
        return []

    out: List[float] = []
    for at in range(0, len(pcm) - frame, hop):
        window = pcm[at:at + frame].astype(np.float64)
        window = window - window.mean()
        energy = float(np.dot(window, window))
        if energy < 1e-6:
            out.append(0.0)
            continue

        corr = np.correlate(window, window, mode="full")[frame - 1:]
        peak = int(np.argmax(corr[lo:hi]) + lo)
        # A weak peak is noise or a whisper: there is no tone there, and none
        # should be invented.
        if corr[peak] < 0.3 * corr[0]:
            out.append(0.0)
        else:
            out.append(rate / float(peak))
    return out


@dataclass
class PitchRange:
    """The range of the speaker over this turn.

    There can be no absolute thresholds here: everybody has their own voice,
    and the same person speaks higher in a fight than in a menu. We measure
    relative to the speaker, and right now.
    """

    low: float = 0.0
    high: float = 0.0
    voiced: int = 0
    _values: List[float] = field(default_factory=list)

    def add(self, f0: List[float]) -> None:
        self._values.extend(v for v in f0 if v > 0.0)
        if len(self._values) >= 8:
            arr = np.array(self._values)
            self.low = float(np.percentile(arr, 10))
            self.high = float(np.percentile(arr, 90))
            self.voiced = len(self._values)

    @property
    def known(self) -> bool:
        return self.voiced >= 8 and self.high > self.low


def terminal_fall(pcm: np.ndarray, rate: int, span: PitchRange,
                  tail_ms: int = 250) -> Optional[float]:
    """How far the last word sank to the floor of its own range.

    1.0 - sank to the floor, that is the phrase sounds finished.
    0.0 - stayed at the top: that is how a continuation sounds.
    None - nothing to judge by: no voice was found, or the range is not known yet.
    """
    if not span.known:
        return None

    tail = pcm[-int(rate * tail_ms / 1000):] if len(pcm) else pcm
    voiced = [v for v in track_f0(tail, rate) if v > 0.0]
    if len(voiced) < 3:
        return None

    # The median of the last third of the tail: the very end often goes into
    # creak, where autocorrelation lies.
    last = sorted(voiced[-max(3, len(voiced) // 3):])
    value = last[len(last) // 2]

    place = (value - span.low) / max(1e-6, span.high - span.low)
    return float(max(0.0, min(1.0, 1.0 - place)))
