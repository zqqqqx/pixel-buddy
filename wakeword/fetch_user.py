#!/usr/bin/env python3
"""Download the wake word recordings made on the robot (web interface -> System) into data/user/."""
import sys
from pathlib import Path
import requests

HOST = sys.argv[1] if len(sys.argv) > 1 else "pixel.local"
DATA = Path(__file__).resolve().parent / "data" / "user"
names = requests.get(f"http://{HOST}/api/kws/list", timeout=10).json()
new = 0
for i, n in enumerate(sorted(names)):
    # fa_*: the robot woke up but nobody spoke (false wake-ups) -> negatives
    # ap_*: "Pixel" from a real activation (somebody spoke afterwards) -> positives
    # pv_* / nv_*: checked in the web interface ("Pixel" / "kein Pixel"); the unchecked copy goes
    kind = "pos" if n.startswith(("pos", "ap", "pv")) else "neg" if n.startswith(("fa", "nv")) else "bg"
    if n.startswith(("pv_", "nv_")):
        for d in ("pos", "bg", "neg", "rejected"):
            for pre in ("u_", "u_val_"):
                old = DATA / d / (pre + n[3:])
                if old.exists():
                    old.unlink()
                    print(f"{n[3:]}: replaced by the checked {n}")
    # every 5th recorded "Pixel" is kept back for validation (prefix u_val); the automatic ones only train
    tag = "u_val_" if n.startswith("pos") and i % 5 == 4 else "u_"
    out = DATA / kind / (tag + n)
    if any((DATA / d / (pre + n)).exists() for d in ("pos", "bg", "neg", "rejected") for pre in ("u_", "u_val_")):
        continue  # already here (possibly sorted into neg / rejected)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(requests.get(f"http://{HOST}/api/kws/file", params={"name": n}, timeout=30).content)
    new += 1
print(f"{len(names)} recordings on the robot, {new} new downloaded to {DATA}")
