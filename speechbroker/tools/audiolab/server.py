# -*- coding: utf-8 -*-
"""HTTP над Studio - и ничего больше.

Каждый маршрут отображается на один метод Studio один в один. Ни одного решения
здесь не принимается: это переводчик, а не участник. Оттого страница в браузере
и остаётся снимаемой шкурой - её можно выбросить, а модуль продолжит работать.

    python server.py            поднять на 8933 и открыть страницу
    python server.py --no-open  без браузера
"""
import json
import pathlib
import sys
import time
import traceback
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from audiolab.studio import Studio   # noqa: E402

STUDIO = Studio.load(ROOT)
UI = ROOT / "ui"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path: pathlib.Path, mime: str):
        if not path.exists():
            return self._send({"error": "нет файла"}, 404)
        body = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}

        if url.path in ("/", "/index.html"):
            return self._file(UI / "index.html", "text/html; charset=utf-8")
        if url.path == "/api/devices":
            return self._send(STUDIO.devices())
        if url.path == "/api/meter":
            return self._send(STUDIO.meter())
        if url.path == "/api/takes":
            return self._send({"takes": STUDIO.takes()})
        if url.path == "/api/waveform":
            return self._send({"points": STUDIO.waveform(q.get("name", ""))})
        if url.path == "/api/wav":
            return self._file(STUDIO.library.path(q.get("name", "")), "audio/wav")
        if url.path == "/api/folder":
            return self._send(STUDIO.folder())
        return self._send({"error": "неизвестный путь"}, 404)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        size = int(self.headers.get("Content-Length", 0))
        try:
            body = json.loads(self.rfile.read(size).decode("utf-8")) if size else {}
        except Exception as exc:
            return self._send({"error": "плохой json: %s" % exc}, 400)

        routes = {
            "/api/choose": lambda: STUDIO.choose(body.get("input"), body.get("output")),
            "/api/listen": lambda: STUDIO.listen(),
            "/api/start": lambda: STUDIO.start(body.get("name", ""), body.get("reference", "")),
            "/api/stop": lambda: STUDIO.stop(bool(body.get("autosave"))),
            "/api/save": lambda: STUDIO.save(body.get("name", ""), body.get("reference", "")),
            "/api/discard": lambda: STUDIO.discard(),
            "/api/preview": lambda: STUDIO.preview(),
            "/api/cancel": lambda: STUDIO.cancel(),
            "/api/play": lambda: STUDIO.play(body.get("name", "")),
            "/api/hush": lambda: STUDIO.hush(),
            "/api/delete": lambda: STUDIO.delete(body.get("name", "")),
            "/api/reference": lambda: STUDIO.set_reference(body.get("name", ""),
                                                           body.get("reference", "")),
            "/api/synthesize": lambda: STUDIO.synthesize(body.get("name", ""),
                                                         body.get("sentences", []),
                                                         body.get("gaps")),
            "/api/folder": lambda: STUDIO.set_folder(body.get("path", "")),
            "/api/pick-folder": lambda: STUDIO.pick_folder(),
            "/api/reveal": lambda: STUDIO.reveal(),
        }
        route = routes.get(url.path)
        if route is None:
            return self._send({"error": "неизвестный путь"}, 404)
        try:
            return self._send(route())
        except Exception as exc:
            return self._send({"error": str(exc)}, 500)


class Server(ThreadingHTTPServer):
    # По умолчанию сокет разрешает переиспользование адреса, и на Windows это
    # значит, что ВТОРОЙ сервер спокойно встаёт на тот же порт. Оба слушают,
    # отвечает старый - со старым кодом, - и разобраться в этом снаружи нельзя.
    # Пусть лучше второй запуск честно падает.
    allow_reuse_address = False


LOG = ROOT / "studio.log"


def note(line: str) -> None:
    """Строка в журнал. Консоль пропадает вместе с окном, файл - нет.

    Заведено после того, как студия однажды исчезла вместе со своим окном
    и разбираться оказалось не по чему: причина ушла на экран, а экрана уже
    не было.
    """
    try:
        with LOG.open("a", encoding="utf-8") as out:
            out.write("%s  %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), line))
    except Exception:
        pass


def main():
    # Порт параметром - чтобы рядом с работающей студией можно было поднять
    # вторую и проверить её, не гася чужую.
    port = 8933
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    try:
        server = Server(("127.0.0.1", port), Handler)
    except OSError as exc:
        note("порт %d занят: %s" % (port, exc))
        print("порт %d уже занят: %s" % (port, exc))
        print("останови прежнюю студию и запусти снова")
        return 1
    note("студия поднята на %d, записи в %s" % (port, STUDIO.library.root))
    print("студия звука: http://127.0.0.1:%d/" % port)
    print("API живёт отдельно от страницы: /api/devices, /api/start, /api/stop, ...")
    print("журнал: %s" % LOG)
    if "--no-open" not in sys.argv:
        webbrowser.open("http://127.0.0.1:%d/" % port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        note("остановлена с клавиатуры")
    except Exception:
        note("УПАЛА:\n" + traceback.format_exc())
        traceback.print_exc()
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
