# -*- coding: utf-8 -*-
"""Синтез речи: тот же голос, что стоит в службе.

Синтезированные записи лежат в той же библиотеке, что и живые, и помечены
источником. Так их видно рядом, и видно, чем они отличаются: у синтеза паузы
ровно те, что мы вставили, интонация ровная, голос стерильный. Он отвечает
на вопрос "работает ли алгоритм", живой голос - "верны ли пороги".

Паузы задаются явно, потому что Piper их не делает: он озвучивает то, что дали,
и молчит ровно столько, сколько написано в тексте.
"""
import pathlib
from typing import List, Optional

import numpy as np

RATE = 16000


class Synth:
    def __init__(self, voice_path: pathlib.Path):
        self.voice_path = voice_path
        self._voice = None
        self._rate = 22050
        self.error = ""

    @property
    def ready(self) -> bool:
        return self._voice is not None

    def load(self) -> bool:
        if self._voice is not None:
            return True
        try:
            from piper import PiperVoice
            self._voice = PiperVoice.load(str(self.voice_path))
            self._rate = int(getattr(self._voice.config, "sample_rate", 22050))
            self.error = ""
            return True
        except Exception as exc:
            self.error = str(exc)
            return False

    def _one(self, text: str) -> np.ndarray:
        chunks = []
        for piece in self._voice.synthesize(text):
            raw = getattr(piece, "audio_int16_bytes", None)
            if raw is None:
                raw = piece if isinstance(piece, (bytes, bytearray)) else piece.audio_int16_bytes
            chunks.append(np.frombuffer(raw, dtype="<i2"))
        return np.concatenate(chunks) if chunks else np.zeros(0, dtype="<i2")

    def say(self, sentences: List[str], gaps_ms: Optional[List[int]] = None,
            tail_ms: int = 1800) -> np.ndarray:
        """Склеить предложения с паузами между ними и хвостом тишины."""
        if not self.load():
            return np.zeros(0, dtype="float32")

        gaps = gaps_ms or []
        parts = []
        for n, text in enumerate(sentences):
            parts.append(self._one(text))
            if n < len(gaps) and gaps[n]:
                parts.append(np.zeros(int(self._rate * gaps[n] / 1000), dtype="<i2"))
        if not sentences:
            parts.append(np.zeros(int(self._rate * 1.5), dtype="<i2"))
        parts.append(np.zeros(int(self._rate * tail_ms / 1000), dtype="<i2"))

        pcm = np.concatenate(parts).astype("float32") / 32768.0
        if self._rate != RATE:
            want = int(len(pcm) * RATE / self._rate)
            pcm = np.interp(np.linspace(0, len(pcm) - 1, want),
                            np.arange(len(pcm)), pcm).astype("float32")
        return pcm
