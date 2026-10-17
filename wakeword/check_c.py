"""Compare the C inference (src/kws.cpp, built as ./ctest) with PyTorch on a few clips."""
import subprocess, sys, wave, glob, random
import numpy as np, torch
import kwsfeat as F
from train import KWS, Pool, make_stream, stream_probs
m = KWS(); m.load_state_dict(torch.load("kws_pixel.pt")); m.eval()
r = random.Random(5)
audio, marks = make_stream(Pool(True), r, 1.0, True)
pcm = (audio * 32767).astype(np.int16)
# the C ring starts half filled: fed from sample 160, its frame k is the PyTorch frame k-1
out = subprocess.run(["./ctest"], input=pcm[160:].tobytes(), capture_output=True).stdout.decode().split("\n")
c = {int(l.split()[0]): float(l.split()[1]) for l in out if l.strip()}
idx, p = stream_probs(m, pcm.astype(np.float32) / 32768)  # the same 16 bit samples as the C side
diffs = []
for i, pt in zip(idx, p):
    if i in c:
        diffs.append(abs(c[i] - pt))
print(f"compared {len(diffs)} evaluations: max |C - torch| = {max(diffs):.2e}, mean {np.mean(diffs):.2e}")
