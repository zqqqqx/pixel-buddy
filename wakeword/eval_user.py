"""Hit rates on the robot recordings per threshold (train / held-out validation / negatives)."""
import glob, sys, wave, numpy as np, torch
import kwsfeat as F
from train import KWS
m = KWS(); m.load_state_dict(torch.load(sys.argv[1] if len(sys.argv) > 1 else "kws_pixel.pt")); m.eval()
def load(f):
    with wave.open(f) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768
room = load("data/user/bg/u_bg_001.wav")
def best(x):
    a = np.concatenate([room[:16000], x, room[16000:24000]]); fe = F.features_np(a); idx = range(49, fe.shape[1] + 1, 5)
    with torch.no_grad(): p = torch.sigmoid(m(torch.from_numpy(np.stack([fe[:, i-49:i] for i in idx])))).numpy()
    return max((p[k] + p[k-1]) / 2 for k in range(1, len(p)))
def bgmax(x):  # long background: count windows above threshold
    fe = F.features_np(x); idx = range(49, fe.shape[1] + 1, 5)
    with torch.no_grad(): p = torch.sigmoid(m(torch.from_numpy(np.stack([fe[:, i-49:i] for i in idx])))).numpy()
    return np.array([(p[k] + p[k-1]) / 2 for k in range(1, len(p))])
tr = [best(load(f)) for f in glob.glob("data/user/pos/u_pos_*.wav")]
va = [best(load(f)) for f in glob.glob("data/user/pos/u_val_*.wav")]
ng = [best(load(f)) for f in glob.glob("data/user/neg/*.wav")]
bg = np.concatenate([bgmax(load(f)) for f in glob.glob("data/user/bg/*.wav")])
bgv = np.concatenate([bgmax(load(f)) for f in glob.glob("data/user/bg/u_bg_00[89].wav")])
print(f"{len(tr)} train, {len(va)} held-out, {len(ng)} robot voice, {len(bg)/10:.0f} s background")
for t in (0.6, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95):
    print(f"thr {t:.2f}: train {np.mean(np.array(tr) >= t)*100:3.0f}%  held-out {np.mean(np.array(va) >= t)*100:3.0f}%  "
          f"robot voice {np.sum(np.array(ng) >= t)}/{len(ng)}  background max {bg.max():.2f}, windows>=thr {np.sum(bg >= t)} (held-out {np.sum(bgv >= t)})")
