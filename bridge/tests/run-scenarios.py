"""Прогон сценариев без игры.

    python tests/run-scenarios.py            все сценарии
    python tests/run-scenarios.py fireball    только совпадающие по имени
"""
import json
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import reference_auction as ref  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CFG = json.loads((ROOT / "config" / "envoy.default.json").read_text(encoding="utf-8"))


def merge(base: dict, over: dict) -> dict:
    out = dict(base)
    for k, v in (over or {}).items():
        out[k] = merge(base.get(k, {}), v) if isinstance(v, dict) else v
    return out


def run_one(path: Path) -> tuple[bool, str]:
    sc = json.loads(path.read_text(encoding="utf-8"))
    cfg = merge(CFG, sc.get("config"))
    exp = json.loads((path.parent.parent / "expected" / path.name).read_text(encoding="utf-8"))

    topic = ref.pick_topic(sc["state"], sc["utterance"], cfg)
    offered = ref.offered_to(sc["subscribers"], topic)
    bids = [dict(b["bid"], ns=b["ns"]) for b in sc["subscribers"]
            if b.get("bid") and b["ns"] in offered]
    result = ref.run_auction(sc["utterance"], bids, cfg)

    got = {"topic": topic, "offered": offered, "winner": result["winner"]}
    want = {"topic": exp["topic"], "offered": exp["offered"], "winner": exp["winner"]}
    if "winners" in exp:
        got["winners"] = sorted(result.get("winners", []))
        want["winners"] = sorted(exp["winners"])
    if "denied" in exp:
        got["denied"] = sorted(result.get("denied", []))
        want["denied"] = sorted(exp["denied"])
    if got == want:
        return True, result["reason"]
    return False, "ожидалось %s, получено %s" % (want, got)


def main() -> int:
    flt = sys.argv[1] if len(sys.argv) > 1 else ""
    files = sorted((ROOT / "tests" / "scenarios").glob("*.json"))
    files = [f for f in files if flt in f.stem]
    if not files:
        print("сценариев не найдено")
        return 1
    failed = 0
    for f in files:
        ok, note = run_one(f)
        print("%s %-28s %s" % ("OK  " if ok else "СБОЙ", f.stem, note))
        failed += 0 if ok else 1
    print("\n%d из %d прошли" % (len(files) - failed, len(files)))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
