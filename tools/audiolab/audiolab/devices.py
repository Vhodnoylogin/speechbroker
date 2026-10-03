# -*- coding: utf-8 -*-
"""Звуковые устройства: перечислить и выбрать явно.

Выбор по образцу имени из настроек оказался плохой мыслью. В списке устройств
живёт "Steam Streaming Microphone" - он существует всегда и МОЛЧИТ, пока Steam VR
не стримит. Образец на него совпадал, запись шла, и в ней была тишина; догадаться,
что слушали не тот вход, было неоткуда.

Поэтому здесь только перечисление. Кто именно слушает и куда играет - решает
человек, и видит это в списке.
"""
from dataclasses import dataclass
from typing import List, Optional

import sounddevice as sd


@dataclass
class Device:
    index: Optional[int]      # None - устройство по умолчанию
    name: str
    api: str
    channels: int
    default_rate: int

    @property
    def title(self) -> str:
        return "%s [%s]" % (self.name.strip(), self.api)

    def as_json(self) -> dict:
        return {"index": self.index, "name": self.name.strip(), "api": self.api,
                "channels": self.channels, "rate": self.default_rate,
                "title": self.title}


def _apis() -> dict:
    return {i: a["name"] for i, a in enumerate(sd.query_hostapis())}


def _default_rate(want_input: bool) -> int:
    """Родная частота устройства, выбранного системой.

    Нужна отдельно, потому что "по умолчанию" - не строка в списке, а отсылка:
    своего номера у него нет, а частота у него есть, и без неё открытие
    начинается с наугад взятых 16 кГц и получает "Invalid sample rate".
    """
    try:
        info = sd.query_devices(kind="input" if want_input else "output")
        return int(info["default_samplerate"])
    except Exception:
        return 0


def _list(want_input: bool) -> List[Device]:
    apis = _apis()
    key = "max_input_channels" if want_input else "max_output_channels"
    out = [Device(None, "по умолчанию", "система", 1, _default_rate(want_input))]
    for index, info in enumerate(sd.query_devices()):
        if info[key] <= 0:
            continue
        out.append(Device(index, info["name"], apis.get(info["hostapi"], "?"),
                          info[key], int(info["default_samplerate"])))
    return out


def inputs() -> List[Device]:
    return _list(True)


def outputs() -> List[Device]:
    return _list(False)


def find(index: Optional[int], want_input: bool) -> Optional[Device]:
    for device in _list(want_input):
        if device.index == index:
            return device
    return None
