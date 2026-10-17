"""Check the robot recordings: keep "Pixel" ones, move others to data/user/rejected (transcripts cached)."""
import concurrent.futures as cf, glob, json, os, re, shutil, wave
from pathlib import Path
import numpy as np, requests
D = Path(__file__).resolve().parent / "data" / "user"
key = re.search(r"sk-[A-Za-z0-9_-]+", (D.parent.parent.parent / "openai.txt").read_text()).group(0)
cf_ = D / "transcripts.json"; cache = json.loads(cf_.read_text()) if cf_.exists() else {}
fs = sorted(glob.glob(str(D / "pos" / "*.wav")))
def tr(f):
    b = os.path.basename(f)
    if b in cache: return b, cache[b]
    r = requests.post("https://api.openai.com/v1/audio/transcriptions", headers={"Authorization": "Bearer " + key},
                      files={"file": open(f, "rb")}, data={"model": "whisper-1", "language": "de"}, timeout=60)
    return b, r.json().get("text", "")
with cf.ThreadPoolExecutor(8) as ex:
    for b, t in ex.map(tr, fs): cache[b] = t
cf_.write_text(json.dumps(cache, ensure_ascii=False, indent=0))
(D / "rejected").mkdir(exist_ok=True)
keep = rej = 0
for f in fs:
    b = os.path.basename(f); t = cache[b]
    if re.search(r"[pb]i[kx]|pics|piks|pigs|pixl", t, re.I): keep += 1
    else:
        rej += 1; print("rejected", b, "|", t); shutil.move(f, D / "rejected" / b)
print(f"kept {keep}, rejected {rej}")
