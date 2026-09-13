"""Сверка контракта Papyrus с тем, что плагин реально регистрирует.

Функция, объявленная в Envoy.psc, но не зарегистрированная в плагине, не падает
на сборке - она падает в игре, в журнале, у пользователя. Поэтому сверяем здесь.

    python tools/check-contract.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

declared = set(re.findall(
    r"^\s*(?:[A-Za-z_]+(?:\[\])?\s+)?Function\s+([A-Za-z_]+)\s*\(",
    (ROOT / "papyrus" / "Envoy.psc").read_text(encoding="utf-8"),
    re.MULTILINE))

registered = set(re.findall(
    r'RegisterFunction\(\s*"([A-Za-z_]+)"',
    (ROOT / "src" / "game" / "PapyrusApi.cpp").read_text(encoding="utf-8")))

missing = sorted(declared - registered)
extra = sorted(registered - declared)

print("объявлено в контракте: %d" % len(declared))
print("зарегистрировано в плагине: %d" % len(registered))

for name in missing:
    print("  НЕТ РЕАЛИЗАЦИИ: %s" % name)
for name in extra:
    print("  НЕ ОБЪЯВЛЕНО В КОНТРАКТЕ: %s" % name)

if missing or extra:
    sys.exit(1)
print("контракт и плагин совпадают")
