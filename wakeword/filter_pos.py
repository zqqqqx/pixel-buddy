#!/usr/bin/env python3
"""Keep only positive clips in which Whisper actually hears "Pixel" (TTS voices sometimes garble it).
Rejected clips go to data/rejected/. Results are cached in data/transcripts.json."""
import concurrent.futures as cf, glob, json, os, re, shutil, wave
from pathlib import Path
import requests
HERE = Path(__file__).resolve().parent
D = HERE / "data"
key = re.search(r"sk-[A-Za-z0-9_-]+", (HERE.parent / "openai.txt").read_text()).group(0)
cache_f = D / "transcripts.json"
cache = json.loads(cache_f.read_text()) if cache_f.exists() else {}
def tr(f):
    b = os.path.basename(f)
    if b in cache: return b, cache[b]
    for _ in range(3):
        try:
            r = requests.post("https://api.openai.com/v1/audio/transcriptions", headers={"Authorization": "Bearer " + key},
                              files={"file": open(f, "rb")}, data={"model": "whisper-1", "language": "de"}, timeout=60)
            return b, r.json().get("text", "")
        except Exception: pass
    return b, ""
(D / "rejected").mkdir(exist_ok=True)
fs = sorted(glob.glob(str(D / "pos" / "*.wav")))
moved = 0
for f in fs:  # the MLS voice babbles instead of saying the word
    if os.path.basename(f).startswith("pp_mls_"):
        shutil.move(f, D / "rejected" / os.path.basename(f)); moved += 1
fs = sorted(glob.glob(str(D / "pos" / "*.wav")))
with cf.ThreadPoolExecutor(12) as ex:
    for b, t in ex.map(tr, fs): cache[b] = t
cache_f.write_text(json.dumps(cache, ensure_ascii=False, indent=0))
keep = 0
for f in fs:
    t = cache[os.path.basename(f)]
    if re.search(r"[pb]i[kx]|pics|piks|pigs", t, re.I): keep += 1
    else:
        shutil.move(f, D / "rejected" / os.path.basename(f)); moved += 1
print(f"kept {keep}, rejected {moved}")
