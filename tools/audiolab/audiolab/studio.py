# -*- coding: utf-8 -*-
"""Studio - весь модуль одним предметом. Это и есть API.

Никакого интерфейса пользователя здесь нет и быть не должно: любой UI - окно,
страница в браузере, командная строка, чужая программа - натягивается СНАРУЖИ
на эти методы. Поэтому всё, что умеет модуль, умеет и тот, у кого нет UI вовсе.

Проверить это просто: если какое-то действие нельзя выполнить, не открывая
страницу, - значит оно уехало в UI, и это ошибка.
"""
import json
import os
import pathlib
import subprocess
import sys
import threading
from typing import List, Optional

from . import devices
from .capture import Recorder
from .files import Library, Take
from .playback import Player
from .synth import Synth


class Studio:
    def __init__(self, root: pathlib.Path, settings: dict = None,
                 config_path: pathlib.Path = None):
        s = dict(settings or {})
        self.root = root
        self.settings = s
        self.config_path = config_path or (root / "audio.json")
        self.library = Library(self._resolve(s.get("takes", "takes")))
        self.recorder = Recorder(int(s.get("blockMs", 32)))
        self.player = Player()

        # The voice of the synthesis is resolved against the folder of the
        # module, not against the current directory: a command is started from
        # wherever it is started, and a relative path in the settings means
        # "inside the module" - it has to travel with it.
        voice = s.get("voice")
        self.synth = Synth(self._resolve(voice)) if voice else None

        self.input_index: Optional[int] = None
        self.output_index: Optional[int] = None
        self._pending_name = ""
        self._pending_reference = ""

        # Остановленная, но ещё не сохранённая запись. Она живёт здесь, пока
        # человек решает: послушать, назвать, вписать эталонный текст - или
        # выбросить. Сохранять молча тоже можно, но это должно быть ВЫБОРОМ,
        # а не единственным поведением.
        #
        # Под замком, потому что сервер многопоточный: метр опрашивается
        # двадцать раз в секунду и успевал подсмотреть отложенную запись
        # в те миллисекунды, пока авто-сохранение её ещё не убрало. Страница
        # показывала "не сохранено" по уже сохранённой записи.
        self._pending = None
        self._guard = threading.Lock()

        # Счётчик правок библиотеки. По нему страница понимает, что список
        # изменился, и перечитывает его - вместо того чтобы дёргать полный
        # список двадцать раз в секунду.
        self.version = 0

    def _resolve(self, where) -> pathlib.Path:
        path = pathlib.Path(str(where))
        return path if path.is_absolute() else (self.root / path)

    # --- устройства ---------------------------------------------------------
    def devices(self) -> dict:
        return {"input": [d.as_json() for d in devices.inputs()],
                "output": [d.as_json() for d in devices.outputs()],
                "chosen": {"input": self.input_index, "output": self.output_index}}

    def choose(self, input_index=None, output_index=None) -> dict:
        if input_index is not None:
            self.input_index = None if input_index in ("", "null", -1) else int(input_index)
            # Открываем вход сразу: пусть уровень шевелится до записи, а не после.
            self.recorder.listen(self.input_index, self._native_rate(self.input_index))
        if output_index is not None:
            self.output_index = None if output_index in ("", "null", -1) else int(output_index)
        return dict(self.devices()["chosen"], error=self.recorder.error)

    def _native_rate(self, index, want_input: bool = True) -> Optional[int]:
        """Частота, которую устройство объявило своей.

        WASAPI в общем режиме работает ТОЛЬКО на ней и на любую другую отвечает
        "Invalid sample rate". Начинать перебор с удобной нам частоты значит
        гарантированно получить отказ на самых обычных устройствах - и на
        микрофонах, и на выходах.
        """
        found = devices.find(index, want_input=want_input)
        return found.default_rate if found and found.default_rate else None

    def listen(self) -> dict:
        """Открыть выбранный вход на прослушивание, ничего не записывая."""
        ok = self.recorder.listen(self.input_index, self._native_rate(self.input_index))
        return {"ok": ok, "error": self.recorder.error}

    # --- запись -------------------------------------------------------------
    def start(self, name: str, reference: str = "") -> dict:
        name = (name or "").strip() or self._free_name()
        self._pending_name, self._pending_reference = name, reference or ""
        ok = self.recorder.start(self.input_index, self._native_rate(self.input_index))
        return {"ok": ok, "name": name, "error": self.recorder.error}

    def stop(self, autosave: bool = False) -> dict:
        """Остановить запись. Сохранение - отдельное решение, если не autosave."""
        import numpy as np

        pcm = self.recorder.stop()
        if not len(pcm):
            with self._guard:
                self._pending = None
            return {"ok": False, "error": "ничего не записалось"}

        # При авто-сохранении отложенной записи не возникает НИ НА МГНОВЕНИЕ:
        # звук уходит в библиотеку прямо отсюда. Иначе метр успевает увидеть
        # её между двумя строками и объявить сохранённую запись несохранённой.
        if autosave:
            return self._store(pcm, self._pending_name, self._pending_reference)

        with self._guard:
            self._pending = pcm
        return {"ok": True, "pending": {"name": self._pending_name,
                                        "reference": self._pending_reference,
                                        "seconds": round(len(pcm) / 16000.0, 2),
                                        "peak": round(float(np.max(np.abs(pcm))), 4)}}

    def _store(self, pcm, name: str, reference: str) -> dict:
        take = self.library.save((name or self._free_name()).strip(), pcm, reference, "живой")
        self.version += 1
        return {"ok": True, "take": take.as_json()}

    def save(self, name: str = "", reference: str = "") -> dict:
        """Положить отложенную запись в библиотеку."""
        with self._guard:
            pcm, self._pending = self._pending, None
        if pcm is None:
            return {"ok": False, "error": "нечего сохранять"}
        return self._store(pcm, name or self._pending_name,
                           reference or self._pending_reference)

    def discard(self) -> dict:
        with self._guard:
            self._pending = None
        return {"ok": True}

    def preview(self) -> dict:
        """Послушать отложенную запись до того, как решать её судьбу."""
        with self._guard:
            pcm = self._pending
        if pcm is None:
            return {"ok": False, "error": "нечего слушать"}
        self.player.play("(не сохранено)", pcm, self.output_index,
                         self._native_rate(self.output_index, want_input=False))
        return {"ok": True}

    def cancel(self) -> dict:
        self.recorder.stop()
        with self._guard:
            self._pending = None
        return {"ok": True}

    def meter(self) -> dict:
        import numpy as np

        state = self.recorder.meter()
        state.update(self.player.state())
        state["version"] = self.version
        with self._guard:
            pcm = self._pending
        state["pending"] = None if pcm is None else {
            "seconds": round(len(pcm) / 16000.0, 2),
            "peak": round(float(np.max(np.abs(pcm))), 4)}
        return state

    # --- где лежат записи ---------------------------------------------------
    def folder(self) -> dict:
        where = self.library.root
        return {"path": str(where), "exists": where.exists(),
                "count": len(list(where.glob("*.wav"))) if where.exists() else 0}

    def set_folder(self, path: str) -> dict:
        """Переехать в другую папку записей и запомнить это в настройках.

        Запоминаем, потому что папка - настройка, а не состояние сессии:
        выбирать её заново при каждом запуске означало бы писать вслепую
        в прежнее место ровно до того мгновения, как это заметят.
        """
        raw = str(path or "").strip().strip('"')
        if not raw:
            return {"ok": False, "error": "пустой путь"}
        where = pathlib.Path(raw)
        if not where.is_absolute():
            where = self.root / where
        try:
            where.mkdir(parents=True, exist_ok=True)
        except Exception as exc:
            return {"ok": False, "error": "не удалось создать папку: %s" % exc}

        self.library = Library(where)
        self.version += 1
        # Свою папку записываем относительным путём: модуль должен переезжать
        # вместе с проектом, а абсолютный путь привязал бы его к этой машине.
        try:
            keep = str(where.relative_to(self.root))
        except ValueError:
            keep = str(where)
        try:
            self.settings["takes"] = keep
            self.config_path.write_text(
                json.dumps(self.settings, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8")
        except Exception as exc:
            return dict(self.folder(), ok=True,
                        error="папка сменена, но не запомнена: %s" % exc)
        return dict(self.folder(), ok=True)

    def reveal(self) -> dict:
        """Открыть папку записей в проводнике."""
        where = self.library.root
        if not where.exists():
            return {"ok": False, "error": "папки нет"}
        try:
            if sys.platform == "win32":
                os.startfile(str(where))
            elif sys.platform == "darwin":
                subprocess.Popen(["open", str(where)])
            else:
                subprocess.Popen(["xdg-open", str(where)])
        except Exception as exc:
            return {"ok": False, "error": str(exc)}
        return {"ok": True, "path": str(where)}

    def pick_folder(self) -> dict:
        """Спросить папку системным окном выбора.

        Это удобство, а не основной путь: браузер не отдаёт путь к папке,
        а набирать его руками долго. Основной путь - set_folder(path), и он
        работает без всякого окна, поэтому модуль остаётся пригоден без UI.
        """
        try:
            import tkinter
            from tkinter import filedialog
        except Exception as exc:
            return {"ok": False, "error": "окно выбора недоступно: %s" % exc}
        try:
            win = tkinter.Tk()
            win.withdraw()
            win.attributes("-topmost", True)
            chosen = filedialog.askdirectory(initialdir=str(self.library.root),
                                             title="Куда складывать записи")
            win.destroy()
        except Exception as exc:
            return {"ok": False, "error": str(exc)}
        if not chosen:
            return {"ok": False, "cancelled": True}
        return self.set_folder(chosen)

    # --- библиотека ---------------------------------------------------------
    def takes(self) -> List[dict]:
        return [t.as_json() for t in self.library.takes()]

    def waveform(self, name: str) -> List[float]:
        return self.library.waveform(name)

    def play(self, name: str) -> dict:
        take = self.library.take(name)
        if take is None:
            return {"ok": False, "error": "нет такой записи"}
        self.player.play(name, self.library.read(name), self.output_index,
                         self._native_rate(self.output_index, want_input=False))
        return {"ok": True}

    def hush(self) -> dict:
        self.player.stop()
        return {"ok": True}

    def delete(self, name: str) -> dict:
        gone = self.library.delete(name)
        if gone:
            self.version += 1
        return {"ok": gone}

    def set_reference(self, name: str, reference: str) -> dict:
        take = self.library.take(name)
        if take is None:
            return {"ok": False, "error": "нет такой записи"}
        (self.library.root / (name + ".txt")).write_text(reference, encoding="utf-8")
        return {"ok": True, "take": self.library.take(name).as_json()}

    # --- синтез -------------------------------------------------------------
    def synthesize(self, name: str, sentences: List[str],
                   gaps_ms: List[int] = None) -> dict:
        if self.synth is None:
            return {"ok": False, "error": "голос синтеза не настроен"}
        pcm = self.synth.say(sentences, gaps_ms)
        if not len(pcm):
            return {"ok": False, "error": self.synth.error or "синтез ничего не дал"}
        take = self.library.save(name or self._free_name("synth"), pcm,
                                 " ".join(sentences), "синтез")
        self.version += 1
        return {"ok": True, "take": take.as_json()}

    # --- служебное ----------------------------------------------------------
    def _free_name(self, prefix: str = "take") -> str:
        taken = {t.name for t in self.library.takes()}
        n = 1
        while "%s-%03d" % (prefix, n) in taken:
            n += 1
        return "%s-%03d" % (prefix, n)

    @staticmethod
    def load(root: pathlib.Path) -> "Studio":
        path = root / "audio.json"
        settings = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
        return Studio(root, settings, path)
