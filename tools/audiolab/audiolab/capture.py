# -*- coding: utf-8 -*-
"""Запись с выбранного входа.

Пишет в память, а не сразу в файл: запись короткая, а решение сохранять её
принимается уже после того, как её услышали. Уровень считается по ходу, чтобы
человек видел, что микрофон вообще что-то слышит, - без этого запись в тишину
обнаруживается только при разборе.
"""
import threading
import time
from typing import List, Optional

import numpy as np
import sounddevice as sd

RATE = 16000


def to_16k(block: np.ndarray, src_rate: int) -> np.ndarray:
    """48 кГц делится нацело - усредняем целые группы отсчётов. Выбрасывать
    отсчёты нельзя: всё выше 8 кГц завернулось бы обратно в речь."""
    if src_rate == RATE:
        return block
    if src_rate % RATE == 0:
        step = src_rate // RATE
        keep = len(block) // step * step
        return block[:keep].reshape(-1, step).mean(axis=1).astype(np.float32)
    want = int(round(len(block) * RATE / float(src_rate)))
    return np.interp(np.linspace(0, len(block) - 1, want),
                     np.arange(len(block)), block).astype(np.float32)


class Recorder:
    """Вход слушается постоянно, а пишется - по команде.

    Разделение важное. Пока идёт только прослушивание, уровень уже шевелится,
    и человек видит, что микрофон живой, ДО того как нажать запись. Без этого
    неработающий вход обнаруживается лишь при разборе готового файла - живая
    запись take-002 вышла ровной тишиной с пиком 0.0000, и понять причину
    в тот момент было неоткуда.
    """

    METER = 60          # сколько последних уровней помним для полоски

    def __init__(self, block_ms: int = 32):
        self.block_ms = block_ms
        self._stream = None
        self._chunks: List[np.ndarray] = []
        self._lock = threading.Lock()
        self._src_rate = RATE
        self.levels: List[float] = []
        self.peak = 0.0
        self.error = ""
        self._collect = False
        self.device: Optional[int] = None

    @property
    def recording(self) -> bool:
        return self._collect

    @property
    def listening(self) -> bool:
        return self._stream is not None

    @property
    def seconds(self) -> float:
        with self._lock:
            return sum(len(c) for c in self._chunks) / float(RATE)

    def listen(self, device: Optional[int], rate: Optional[int] = None) -> bool:
        """Открыть вход и просто слушать. Ничего не записывается."""
        if self._stream is not None and self.device == device:
            return True
        self.close()
        # Windows отпускает звуковое устройство не мгновенно, и открытие
        # следующего сразу после закрытия предыдущего отвечает "Invalid sample
        # rate" - хотя дело не в частоте. Небольшая пауза это снимает.
        time.sleep(0.15)
        self.device = device
        self.error = ""
        self.levels, self.peak = [], 0.0

        # Частоту пробуем в том порядке, в каком её обычно поддерживают: своя
        # у устройства, затем привычные. Открытие - единственная правда о том,
        # что оно умеет: список поддерживаемых форматов у Windows врёт.
        candidates = [rate] if rate else []
        candidates += [RATE, 48000, 44100, 32000]
        for candidate in [c for c in candidates if c]:
            try:
                stream = sd.InputStream(device=device, channels=1, samplerate=candidate,
                                        dtype="float32",
                                        blocksize=int(candidate * self.block_ms / 1000),
                                        callback=self._on_block)
                stream.start()
            except Exception as exc:
                self.error = str(exc)
                continue
            self._stream, self._src_rate = stream, candidate
            self.error = ""
            return True
        return False

    def close(self) -> None:
        self._collect = False
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            finally:
                self._stream = None

    def start(self, device: Optional[int], rate: Optional[int] = None) -> bool:
        """Начать накапливать то, что и так уже слышим."""
        if not self.listen(device, rate):
            return False
        with self._lock:
            self._chunks = []
        self.peak = 0.0
        self._collect = True
        return True

    def _on_block(self, indata, frames, time_info, status) -> None:
        # Исключение отсюда уходит не в наш код, а в поток звуковой подсистемы,
        # и разбираться с ним там некому: в лучшем случае поток умирает молча,
        # в худшем процесс уходит целиком - консоль просто исчезает. Поэтому
        # ловим всё и оставляем след в self.error.
        try:
            block = to_16k(indata[:, 0].copy(), self._src_rate)
            level = float(np.sqrt(np.mean(np.square(block)))) if len(block) else 0.0
            if self._collect:
                with self._lock:
                    self._chunks.append(block)
                self.peak = max(self.peak,
                                float(np.max(np.abs(block))) if len(block) else 0.0)
            self.levels = (self.levels + [level])[-self.METER:]
        except Exception as exc:
            self.error = "сбой на входе: %s" % exc

    def stop(self) -> np.ndarray:
        """Прекратить накопление, но продолжать слушать."""
        self._collect = False
        with self._lock:
            pcm = np.concatenate(self._chunks) if self._chunks else np.zeros(0, dtype="float32")
            # Буфер отдаём и забываем: иначе метр продолжает показывать длину
            # уже законченной записи, и человек видит секунды, которых нет.
            self._chunks = []
        return pcm

    def meter(self) -> dict:
        # "слышит" - не то же самое, что "открыт": вход может быть открыт
        # и при этом отдавать ровную тишину, как Steam Streaming Microphone,
        # пока Steam VR не стримит. Ровно на этом и потерялась первая запись.
        #
        # Порог отделяет ЖИВОЙ вход от МЁРТВОГО, а не речь от молчания: тихая
        # комната на рабочем микрофоне даёт около 0.001, мёртвый вход - ровный
        # ноль. Ставить порог по речи нельзя, иначе молчащий человек выглядел
        # бы как сломанный микрофон.
        recent = self.levels
        return {"recording": self.recording,
                "listening": self.listening,
                "hears": bool(recent) and max(recent) > 0.0002,
                "seconds": round(self.seconds, 2),
                "peak": round(self.peak, 4),
                "levels": [round(v, 4) for v in self.levels],
                "error": self.error}
