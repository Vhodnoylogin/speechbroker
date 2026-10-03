# -*- coding: utf-8 -*-
"""Проигрывание записи на выбранный выход.

Работает в своём потоке: воспроизведение секундной записи не должно вешать
ни ответ API, ни следующую запись.
"""
import threading
from typing import List, Optional

import numpy as np
import sounddevice as sd

RATE = 16000


class Player:
    def __init__(self):
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self.playing = ""
        self.error = ""

    @staticmethod
    def _resample(pcm: np.ndarray, rate: int) -> np.ndarray:
        if rate == RATE:
            return pcm
        want = int(len(pcm) * rate / float(RATE))
        return np.interp(np.linspace(0, len(pcm) - 1, want),
                         np.arange(len(pcm)), pcm).astype("float32")

    def play(self, name: str, pcm: np.ndarray, device: Optional[int],
             native_rate: Optional[int] = None) -> bool:
        """Сыграть запись. native_rate - частота, объявленная устройством.

        Её надо пробовать ПЕРВОЙ. WASAPI в общем режиме работает только на
        родной частоте устройства и на любую другую отвечает "Invalid sample
        rate"; начинать перебор с удобных нам 16 кГц значит получать отказ
        на каждом обычном выходе - ровно то, что и происходило.
        """
        self.stop()
        self.error = ""
        self._stop.clear()
        self.playing = name

        order: List[int] = []
        for rate in [native_rate, RATE, 48000, 44100]:
            if rate and int(rate) not in order:
                order.append(int(rate))

        def run():
            # Отказ на одной частоте - не ошибка, а ход перебора, и показывать
            # его человеку нельзя: раньше он выводился на экран и висел там всё
            # воспроизведение, потому что снимался только после его конца.
            refused = ""
            try:
                for rate in order:
                    try:
                        sd.play(self._resample(pcm, rate), samplerate=rate,
                                device=device, blocking=False)
                    except Exception as exc:
                        refused = str(exc)
                        continue

                    self.error = ""
                    stream = sd.get_stream()
                    while stream is not None and stream.active:
                        if self._stop.wait(0.05):
                            sd.stop()
                            break
                    return
                self.error = refused or "выход не принял ни одной частоты"
            except Exception as exc:
                self.error = str(exc)
            finally:
                self.playing = ""

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()
        return True

    def stop(self) -> None:
        self._stop.set()
        try:
            sd.stop()
        except Exception:
            pass
        self.playing = ""

    def state(self) -> dict:
        return {"playing": self.playing, "error": self.error}
