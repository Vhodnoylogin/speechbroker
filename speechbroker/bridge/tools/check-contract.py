"""Checking the Papyrus contract against what the plugin really registers.

A function declared in SpeechBroker.psc but not registered in the plugin does not fail
at build time - it fails in the game, in the log, at the user. So we check here.

    python tools/check-contract.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

declared = set(re.findall(
    r"^\s*(?:[A-Za-z_]+(?:\[\])?\s+)?Function\s+([A-Za-z_]+)\s*\(",
    (ROOT / "papyrus" / "SpeechBroker.psc").read_text(encoding="utf-8"),
    re.MULTILINE))

registered = set(re.findall(
    r'RegisterFunction\(\s*"([A-Za-z_]+)"',
    (ROOT / "src" / "game" / "PapyrusApi.cpp").read_text(encoding="utf-8")))

missing = sorted(declared - registered)
extra = sorted(registered - declared)

print("declared in the contract: %d" % len(declared))
print("registered in the plugin: %d" % len(registered))

for name in missing:
    print("  NO IMPLEMENTATION: %s" % name)
for name in extra:
    print("  NOT DECLARED IN THE CONTRACT: %s" % name)

if missing or extra:
    sys.exit(1)
print("the contract and the plugin agree")
