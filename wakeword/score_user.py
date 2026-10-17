"""Score the robot recordings like the firmware (sliding, two evaluations averaged), padded with real room noise."""
import glob, sys, wave, numpy as np, torch
import kwsfeat as F
from train import KWS
m = KWS(); m.load_state_dict(torch.load(sys.argv[1] if len(sys.argv) > 1 else "kws_pixel.pt")); m.eval()
def load(f):
    with wave.open(f) as w: return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768
room = load(sorted(glob.glob("data/user/bg/*.wav"))[1])  # quiet room, same microphone
def best(x):
    audio = np.concatenate([room[:16000], x, room[16000:24000]])
    fe = F.features_np(audio); idx = range(49, fe.shape[1] + 1, 5)
    with torch.no_grad(): p = torch.sigmoid(m(torch.from_numpy(np.stack([fe[:, i-49:i] for i in idx])))).numpy()
    return max((p[k] + p[k-1]) / 2 for k in range(1, len(p)))
for kind in ("pos", "neg", "bg", "rejected"):
    r = [(f.split("/")[-1], best(load(f))) for f in sorted(glob.glob(f"data/user/{kind}/*.wav"))]
    print(f"{kind:8s}", " ".join(f"{n[-8:-4]}:{v:.2f}" for n, v in r))
