# -*- coding: utf-8 -*-
"""Записи на диске: WAV рядом с эталонным текстом.

Одна запись - два файла: `имя.wav` и `имя.txt`. Текст в спутнике, а не в имени
файла и не в отдельной базе: так запись остаётся самодостаточной, её можно
скопировать куда угодно, и она не рассыплется.

Пустой `имя.txt` - это не отсутствие текста, а утверждение "здесь тишина",
и калибровка по нему проверяет, не выдумывает ли модель на пустом месте.
"""
import pathlib
import wave
from dataclasses import dataclass
from typing import List, Optional

import numpy as np

RATE = 16000

# What `name.src` says about where the voice came from. The words are Russian
# because the forty-five takes on disk already carry them and the studio page
# shows them as they are; they are data, not interface text. Everything that
# tells a live take from a synthesised one compares against these two names.
SOURCE_LIVE = "живой"
SOURCE_SYNTH = "синтез"


def read_wav(path) -> np.ndarray:
    """A wav as mono float32 at RATE, whatever it was on disk.

    One reader for the whole tooling: the calibration, the replay, the prosody
    check and the studio all read the same way, so that a difference between
    two reports can never be a difference in how the file was opened.
    """
    with wave.open(str(path), "rb") as wav:
        rate, channels = wav.getframerate(), wav.getnchannels()
        raw = wav.readframes(wav.getnframes())
    pcm = np.frombuffer(raw, dtype="<i2").astype("float32") / 32768.0
    if channels > 1:
        pcm = pcm.reshape(-1, channels).mean(axis=1)
    if rate != RATE:
        want = int(len(pcm) * RATE / rate)
        pcm = np.interp(np.linspace(0, len(pcm) - 1, want),
                        np.arange(len(pcm)), pcm).astype("float32")
    return pcm


@dataclass
class Take:
    name: str
    path: pathlib.Path
    seconds: float
    reference: str
    source: str            # SOURCE_LIVE | SOURCE_SYNTH
    peak: float

    @property
    def live(self) -> bool:
        return self.source == SOURCE_LIVE

    @property
    def is_silence(self) -> bool:
        """An empty reference is not "no text" but the claim "there is silence here"."""
        return not self.reference.strip()

    def as_json(self) -> dict:
        return {"name": self.name, "seconds": round(self.seconds, 2),
                "reference": self.reference, "source": self.source,
                "peak": round(self.peak, 3), "silent": self.peak < 0.01}


class Library:
    """Все записи в одной папке, живые и синтезированные вперемешку."""

    def __init__(self, root: pathlib.Path):
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)

    def path(self, name: str) -> pathlib.Path:
        return self.root / (name + ".wav")

    def save(self, name: str, pcm: np.ndarray, reference: str, source: str) -> Take:
        data = (np.clip(pcm, -1.0, 1.0) * 32767).astype("<i2")
        with wave.open(str(self.path(name)), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)
            wav.writeframes(data.tobytes())
        (self.root / (name + ".txt")).write_text(reference, encoding="utf-8")
        (self.root / (name + ".src")).write_text(source, encoding="utf-8")
        return self.take(name)

    def read(self, name: str) -> np.ndarray:
        return read_wav(self.path(name))

    def take(self, name: str) -> Optional[Take]:
        path = self.path(name)
        if not path.exists():
            return None
        with wave.open(str(path), "rb") as wav:
            seconds = wav.getnframes() / float(wav.getframerate())
        text = self.root / (name + ".txt")
        src = self.root / (name + ".src")
        pcm = self.read(name)
        return Take(name=name, path=path, seconds=seconds,
                    reference=text.read_text(encoding="utf-8").strip() if text.exists() else "",
                    source=src.read_text(encoding="utf-8").strip() if src.exists() else "неизвестно",
                    peak=float(np.max(np.abs(pcm))) if len(pcm) else 0.0)

    def takes(self) -> List[Take]:
        """Свежие сверху.

        По имени сортировать нельзя: новая запись уезжала бы в середину списка
        по алфавиту, и найти только что записанное было бы негде. Порядок по
        времени правки отвечает на вопрос, который задают чаще всего, - "что
        я записал только что".
        """
        files = sorted(self.root.glob("*.wav"),
                       key=lambda f: f.stat().st_mtime, reverse=True)
        out = [self.take(f.stem) for f in files]
        return [t for t in out if t is not None]

    def by_name(self) -> List[Take]:
        """Alphabetical, for the reports: a measurement must read the same way
        twice, and the order of editing does not."""
        out = [self.take(f.stem) for f in sorted(self.root.glob("*.wav"))]
        return [t for t in out if t is not None]

    def delete(self, name: str) -> bool:
        gone = False
        for suffix in (".wav", ".txt", ".src"):
            p = self.root / (name + suffix)
            if p.exists():
                p.unlink()
                gone = True
        return gone

    def waveform(self, name: str, points: int = 400) -> List[float]:
        """Огибающая для рисования: максимум по равным долям записи."""
        pcm = self.read(name)
        if not len(pcm):
            return []
        step = max(1, len(pcm) // points)
        return [float(np.max(np.abs(pcm[at:at + step])))
                for at in range(0, len(pcm) - step, step)]
