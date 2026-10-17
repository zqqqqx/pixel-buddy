#!/usr/bin/env python3
"""Train the "Pixel" wake word model and export it for the ESP32 (src/kws_model.h).

Every training example is ~1 s of audio built on the fly from data/:
  positive  "Pixel" ending 0.70..0.98 s into the window (that is when the robot must react),
            sometimes followed by the start of more speech ("Pixel, wie spaet ist es")
  negative  other speech, sound-alike words at the same place, noise only, the word cut off
            half way ("Pix-"), or a "Pixel" that ended long ago
and augmented: speed/pitch, room reverb, microphone colouring, background noise at 0..30 dB SNR,
level, SpecAugment. Held-out voices and noise are used for validation and for a streaming
test that counts false wake-ups per hour exactly like the firmware does.

Usage:  .venv/bin/python train.py [--epochs 40]
"""
import argparse
import glob
import math
import os
import random
import struct
import time
import wave
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from scipy.signal import butter, fftconvolve, lfilter, resample

import kwsfeat as F

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
OUT_H = HERE.parent / "src" / "kws_model.h"
N = F.CLIP


def load(path):
    with wave.open(path) as w:
        return np.frombuffer(w.readframes(w.getnframes()), np.int16).copy()


def is_val(name):
    """held-out speakers / voices / noise for validation"""
    b = os.path.basename(name)
    if b.startswith(("u_bg_008", "u_bg_009")):  # held-out room recordings
        return True
    if b.startswith(("oai_nova", "oai_cedar", "oai2_nova", "oai2_cedar", "pp_kerstin", "pp2_kerstin",
                     "pp_thorsten_high", "pp2_thorsten_high", "u_val")):
        return True
    parts = b.split("_")
    if b.startswith(("pp_mls_", "pp_libritts_r_", "ppc_mls_")):
        num = [p for p in parts if p.split(".")[0].isdigit()]
        return bool(num) and int(num[0].split(".")[0]) % 10 == 0
    if b.startswith("esc_"):
        return b[4] == "5"  # ESC-50 fold 5
    return b.startswith("synth_08") or b.startswith("synth_09")


def trim(x, thr=0.02):
    """cut leading/trailing silence of a TTS clip"""
    a = np.abs(x.astype(np.float32) / 32768)
    env = np.convolve(a, np.ones(160) / 160, "same")
    idx = np.where(env > thr * env.max())[0]
    if len(idx) == 0:
        return x
    return x[max(0, idx[0] - 160): idx[-1] + 160]


class Pool:
    def __init__(self, val):
        def pick(d):
            fs = sorted(glob.glob(str(DATA / d / "*.wav")))
            return [load(f) for f in fs if is_val(f) == val]
        self.pos = [trim(x) for x in pick("pos")]
        self.neg = [trim(x) for x in pick("neg")]
        self.noise = pick("noise")
        self.user_pos = [trim(x) for x in pick("user/pos")] if (DATA / "user" / "pos").exists() else []
        self.user_bg = pick("user/bg") if (DATA / "user" / "bg").exists() else []
        # the robot's own voice through its microphone etc.: important negatives, used 4x
        if (DATA / "user" / "neg").exists():
            self.neg += [trim(x) for x in pick("user/neg")] * 4
        self.neg_w = None  # hard negative mining: sampling weights for neg

    def neg_idx(self, r):
        if self.neg_w is None:
            return r.randrange(len(self.neg))
        return r.choices(range(len(self.neg)), weights=self.neg_w)[0]


# ------------------------------------------------------------------ augmentation
def rir(r):
    rt60 = r.uniform(0.12, 0.7)
    n = int(rt60 * F.SR)
    t = np.arange(n) / F.SR
    h = np.random.default_rng(r.randrange(1 << 30)).standard_normal(n) * np.exp(-6.9 * t / rt60)
    h[0] = r.uniform(2, 8)  # direct sound
    return (h / np.sqrt(np.sum(h ** 2))).astype(np.float32)


def colour(x, r):
    """small-speaker / MEMS-mic like band limit"""
    b, a = butter(2, r.uniform(70, 300) / (F.SR / 2), "high")
    x = lfilter(b, a, x)
    b, a = butter(2, r.uniform(3800, 7600) / (F.SR / 2), "low")
    return lfilter(b, a, x).astype(np.float32)


def speech(x, r, speed=True, reverb=True):
    x = x.astype(np.float32) / 32768
    if speed and r.random() < 0.7:
        f = r.uniform(0.88, 1.15)
        x = resample(x, max(32, int(len(x) / f))).astype(np.float32)
    if reverb and r.random() < 0.45:
        y = fftconvolve(x, rir(r))[: len(x) + int(0.06 * F.SR)]  # short tail: the word end stays where it is
        x = (y * r.uniform(0.5, 1.0) + np.pad(x, (0, len(y) - len(x))) * r.uniform(0.3, 1.0)).astype(np.float32)
    if r.random() < 0.6:
        x = colour(x, r)
    return x / (np.max(np.abs(x)) + 1e-9)


def bed(pool, r, n):
    """background noise of length n (0 if none)"""
    srcs = pool.noise + pool.user_bg
    if r.random() < 0.12 or not srcs:
        return np.zeros(n, np.float32)
    z = srcs[r.randrange(len(srcs))].astype(np.float32) / 32768
    if len(z) < n:
        z = np.tile(z, n // len(z) + 1)
    o = r.randrange(0, len(z) - n + 1)
    z = z[o: o + n]
    return z / (np.sqrt(np.mean(z ** 2)) + 1e-6)


def place(win, clip, end):
    """add clip into win so that it ends at sample `end` (may be cut at the start)"""
    s = end - len(clip)
    a = max(0, s)
    win[a:end] += clip[a - s:]


def example(pool, r):
    """one (audio, label) pair"""
    win = np.zeros(N, np.float32)
    lab = 0.0
    kind = r.random()
    pos = pool.pos + pool.user_pos * 6
    if kind < 0.30:                                  # wake word at the end of the window
        lab = 1.0
        w = speech(pos[r.randrange(len(pos))], r)
        end = int(r.uniform(0.70, 0.985) * N)
        place(win, w, end)
        if r.random() < 0.3:                         # ... preceded by other speech
            p = speech(pool.neg[pool.neg_idx(r)], r) * r.uniform(0.3, 1.0)
            place(win, p[-int(r.uniform(0.2, 0.6) * F.SR):], max(0, end - len(w) - int(r.uniform(0.05, 0.3) * F.SR)))
        if r.random() < 0.25 and end < N - 800:      # ... followed by more words
            f = speech(pool.neg[pool.neg_idx(r)], r)
            g = end + int(r.uniform(0.04, 0.2) * F.SR)
            f = f[: max(0, N - g)]
            win[g: g + len(f)] += f * r.uniform(0.5, 1.0)
    elif kind < 0.62:                                # other speech anywhere
        for _ in range(r.choice([1, 1, 2])):
            w = speech(pool.neg[pool.neg_idx(r)], r)
            if len(w) > N // 2 and r.random() < 0.7:  # a random part of a longer sentence
                w = w[: r.randrange(N // 3, len(w) + 1)]
            place(win, w, int(r.uniform(0.4, 1.0) * N))
    elif kind < 0.80:                                # sound-alike / any word right where the wake word would be
        w = speech(pool.neg[pool.neg_idx(r)], r)
        w = w[: int(r.uniform(0.25, 0.8) * F.SR)] if len(w) > 0.8 * F.SR and r.random() < 0.5 else w
        place(win, w, int(r.uniform(0.65, 1.0) * N))
    elif kind < 0.84 and pool.user_bg:               # the real room through the real microphone, nothing said
        z = pool.user_bg[r.randrange(len(pool.user_bg))].astype(np.float32) / 32768
        o = r.randrange(0, max(1, len(z) - N))
        z = z[o: o + N]
        x = np.zeros(N, np.float32)
        x[: len(z)] = z * 10 ** (r.uniform(-6, 6) / 20)
        return np.clip(x, -1, 1), 0.0
    elif kind < 0.89:                                # "Pix-" cut off, or the word ended long ago
        w = speech(pos[r.randrange(len(pos))], r)
        if r.random() < 0.5:
            place(win, w[: int(len(w) * r.uniform(0.35, 0.6))], int(r.uniform(0.85, 1.0) * N))
        else:
            place(win, w, int(r.uniform(0.15, 0.45) * N))
    # else: noise only
    # mix: speech level, noise at an SNR, overall gain
    sp = np.sqrt(np.mean(win ** 2)) + 1e-6
    if np.max(np.abs(win)) > 0:
        win = win / sp * 0.1
    snr = r.uniform(0, 30)
    nz = bed(pool, r, N) * (0.1 / (10 ** (snr / 20)) if np.max(np.abs(win)) > 0 else r.uniform(0.003, 0.2))
    x = win + nz
    # the robot's microphone delivers quiet audio: speech windows around -40 dBFS (measured), loud close speech -15
    x = x * (10 ** (r.uniform(-56, -12) / 20) / (np.sqrt(np.mean(x ** 2)) + 1e-6))
    x = np.clip(x + r.normalvariate(0, 1) * 1e-4, -1, 1)
    return x.astype(np.float32), lab


class DS(torch.utils.data.IterableDataset):
    def __init__(self, pool, n, seed):
        self.pool, self.n, self.seed = pool, n, seed

    def __iter__(self):
        wi = torch.utils.data.get_worker_info()
        r = random.Random(self.seed + (wi.id if wi else 0) * 7919 + int(time.time() * 1000) % 100000)
        cnt = self.n // (wi.num_workers if wi else 1)
        for _ in range(cnt):
            x, y = example(self.pool, r)
            yield torch.from_numpy(x), torch.tensor(y)


# ------------------------------------------------------------------ model
class DSBlock(nn.Module):
    def __init__(self, cin, cout, k, s):
        super().__init__()
        self.dw = nn.Conv1d(cin, cin, k, s, k // 2, groups=cin, bias=False)
        self.bn1 = nn.BatchNorm1d(cin)
        self.pw = nn.Conv1d(cin, cout, 1, bias=False)
        self.bn2 = nn.BatchNorm1d(cout)

    def forward(self, x):
        return torch.relu(self.bn2(self.pw(torch.relu(self.bn1(self.dw(x))))))


class KWS(nn.Module):
    """~20k parameters, ~0.5M multiply-adds per second of audio window"""
    C1, C2, C3 = 48, 64, 64

    def __init__(self):
        super().__init__()
        self.c1 = nn.Conv1d(F.NMEL, self.C1, 3, 1, 1, bias=False)
        self.bn1 = nn.BatchNorm1d(self.C1)
        self.b2 = DSBlock(self.C1, self.C2, 5, 2)
        self.b3 = DSBlock(self.C2, self.C3, 5, 2)
        self.b4 = DSBlock(self.C3, self.C3, 5, 1)
        self.fc1 = nn.Linear(self.C3, 32)
        self.fc2 = nn.Linear(32, 1)
        self.drop = nn.Dropout(0.15)
        self.register_buffer("mean", torch.zeros(F.NMEL))
        self.register_buffer("std", torch.ones(F.NMEL))

    def forward(self, f):  # f: [B, 40, 49] log mel
        x = (f - self.mean[None, :, None]) / self.std[None, :, None]
        x = torch.relu(self.bn1(self.c1(x)))
        x = self.b4(self.b3(self.b2(x)))
        x = x.mean(dim=2)
        return self.fc2(self.drop(torch.relu(self.fc1(x)))).squeeze(1)


def spec_augment(f, r):
    f = f.clone()
    for i in range(f.shape[0]):
        if r.random() < 0.5:
            a = r.randrange(0, F.NMEL - 6)
            f[i, a: a + r.randrange(1, 6)] = f[i].mean()
        if r.random() < 0.3:
            a = r.randrange(0, F.FRAMES - 5)
            f[i, :, a: a + r.randrange(1, 4)] = f[i].mean()
    return f


# ------------------------------------------------------------------ streaming test (same logic as the firmware)
EVAL_EVERY = 5      # frames (100 ms)
REFRACTORY = 1.5    # s


def stream_probs(model, audio):
    feats = F.features_np(audio)  # [40, T]
    T = feats.shape[1]
    idx = list(range(F.FRAMES, T + 1, EVAL_EVERY))
    batch = np.stack([feats[:, i - F.FRAMES: i] for i in idx])
    with torch.no_grad():
        p = torch.sigmoid(model(torch.from_numpy(batch))).numpy()
    return np.array(idx), p


def detect(idx, p, thr):
    """2 consecutive evaluations averaging above thr, then a refractory period"""
    hits, last = [], -1e9
    for k in range(1, len(p)):
        t = idx[k] * F.HOP / F.SR
        if (p[k] + p[k - 1]) / 2 > thr and t - last > REFRACTORY:
            hits.append(t)
            last = t
    return hits


def make_stream(pool, r, minutes, with_pos):
    out, marks = [], []
    total = 0
    while total < minutes * 60 * F.SR:
        if with_pos and r.random() < 0.25:
            w = speech(pool.pos[r.randrange(len(pool.pos))], r)
            marks.append((total + len(w)) / F.SR)
        else:
            w = speech(pool.neg[pool.neg_idx(r)], r)
        gap = np.zeros(int(r.uniform(0.2, 2.5) * F.SR), np.float32)
        seg = np.concatenate([w * r.uniform(0.3, 1.0), gap])
        out.append(seg)
        total += len(seg)
    s = np.concatenate(out)
    nz = bed(pool, r, len(s)) if pool.noise else np.zeros(len(s), np.float32)
    s = s * 0.1 / (np.sqrt(np.mean(s ** 2)) + 1e-6) + nz * 0.1 / 10 ** (r.uniform(5, 25) / 20)
    s = s * (10 ** (r.uniform(-45, -28) / 20) / (np.sqrt(np.mean(s ** 2)) + 1e-6))  # real microphone levels
    return np.clip(s, -1, 1).astype(np.float32), marks


def evaluate(model, vpool, seed=7):
    model.eval()
    r = random.Random(seed)
    neg, _ = make_stream(vpool, r, 20, False)
    mix, marks = make_stream(vpool, r, 6, True)
    ni, npb = stream_probs(model, neg)
    mi, mp = stream_probs(model, mix)
    res = []
    for thr in (0.6, 0.8, 0.9, 0.95, 0.97, 0.98, 0.99):
        fa = len(detect(ni, npb, thr)) * 3          # per hour (20 min stream)
        hits = detect(mi, mp, thr)
        found = sum(any(-0.35 <= h - m < 0.7 for h in hits) for m in marks)
        res.append((thr, fa, found / max(1, len(marks))))
    model.train()
    return res


def user_eval(model, vpool):
    """held-out robot recordings: share of "Pixel" clips reaching 0.7, highest background score"""
    if not vpool.user_pos or not vpool.user_bg:
        return None
    model.eval()
    room = vpool.user_bg[0].astype(np.float32) / 32768

    def peak(a):
        _, p = stream_probs(model, a)
        return max((p[k] + p[k - 1]) / 2 for k in range(1, len(p)))
    rec = np.mean([peak(np.concatenate([room[:16000], x.astype(np.float32) / 32768, room[16000:24000]])) >= 0.7
                   for x in vpool.user_pos])
    bg = max(peak(b.astype(np.float32) / 32768) for b in vpool.user_bg)
    model.train()
    return float(rec), float(bg)


# ------------------------------------------------------------------ export
def fold(conv_w, bn):
    s = (bn.weight / torch.sqrt(bn.running_var + bn.eps)).detach()
    w = conv_w.detach() * s.view(-1, *([1] * (conv_w.dim() - 1)))
    b = (bn.bias - bn.running_mean * s).detach()
    return w, b


def carr(name, a):
    a = np.asarray(a.detach() if torch.is_tensor(a) else a, np.float32).ravel()
    body = ",".join(f"{v:.8g}f" for v in a)
    return f"static const float {name}[{len(a)}] = {{{body}}};\n"


def export(model, thr, path):
    m = model.eval()
    w1, b1 = fold(m.c1.weight, m.bn1)                    # [48, 40, 3]
    # fold the input normalisation into the first layer
    istd = (1 / m.std).detach()
    b1 = b1 - (w1 * (m.mean * istd)[None, :, None]).sum(dim=(1, 2))
    w1 = w1 * istd[None, :, None]
    s = "// generated by wakeword/train.py - do not edit\n#pragma once\n"
    s += f"#define KWS_NMEL {F.NMEL}\n#define KWS_FRAMES {F.FRAMES}\n#define KWS_C1 {m.C1}\n#define KWS_C2 {m.C2}\n#define KWS_C3 {m.C3}\n"
    s += f"#define KWS_THRESHOLD {thr:.3f}f\n#define KWS_EVAL_EVERY {EVAL_EVERY}\n"
    mel = F.MEL
    start = [int(np.nonzero(r)[0][0]) for r in mel]
    length = [int(np.nonzero(r)[0][-1]) - st + 1 for r, st in zip(mel, start)]
    s += "static const short KWS_MEL_START[%d] = {%s};\n" % (F.NMEL, ",".join(map(str, start)))
    s += "static const short KWS_MEL_LEN[%d] = {%s};\n" % (F.NMEL, ",".join(map(str, length)))
    s += carr("KWS_MEL_W", np.concatenate([mel[i, start[i]: start[i] + length[i]] for i in range(F.NMEL)]))
    s += carr("KWS_W1", w1) + carr("KWS_B1", b1)
    # the first layer pads with normalised 0 = the band mean in raw features (normalisation is folded in)
    s += carr("KWS_PAD", m.mean)
    for n, blk in (("2", m.b2), ("3", m.b3), ("4", m.b4)):
        dw, db = fold(blk.dw.weight, blk.bn1)
        pw, pb = fold(blk.pw.weight, blk.bn2)
        s += carr(f"KWS_DW{n}", dw) + carr(f"KWS_DB{n}", db) + carr(f"KWS_PW{n}", pw) + carr(f"KWS_PB{n}", pb)
    s += carr("KWS_FC1", m.fc1.weight) + carr("KWS_FB1", m.fc1.bias) + carr("KWS_FC2", m.fc2.weight) + carr("KWS_FB2", m.fc2.bias)
    path.write_text(s)
    print(f"exported {path} ({path.stat().st_size // 1024} KB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--bs", type=int, default=96)
    ap.add_argument("--fa_per_hour", type=float, default=0.5, help="allowed false wake-ups per hour (validation)")
    ap.add_argument("--init", help="continue from a checkpoint (fine-tuning)")
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--out", default=str(OUT_H), help="header to export (default: the firmware's)")
    ap.add_argument("--save", default=str(HERE / "kws_pixel.pt"), help="checkpoint to write")
    args = ap.parse_args()
    torch.set_num_threads(4)
    tp, vp = Pool(False), Pool(True)
    print(f"train: {len(tp.pos)} pos {len(tp.neg)} neg {len(tp.noise)} noise | val: {len(vp.pos)} pos {len(vp.neg)} neg "
          f"{len(vp.noise)} noise | user: {len(tp.user_pos)} pos {len(tp.user_bg)} bg", flush=True)
    model = KWS()
    # input normalisation from a sample of training windows
    with torch.no_grad():
        r = random.Random(1)
        fs = F.features_torch(torch.from_numpy(np.stack([example(tp, r)[0] for _ in range(1500)])))
        model.mean.copy_(fs.mean(dim=(0, 2)))
        model.std.copy_(fs.std(dim=(0, 2)) + 1e-3)
    if args.init:
        model.load_state_dict(torch.load(args.init))
        # hard negatives: every negative clip right where the wake word would be -> how sure is the model?
        model.eval()
        r = random.Random(11)
        xs = []
        for w in tp.neg:
            win = np.zeros(N, np.float32)
            x = w.astype(np.float32) / 32768
            place(win, x / (np.max(np.abs(x)) + 1e-9) * 0.3, int(0.9 * N))
            xs.append(win)
        with torch.no_grad():
            pr = torch.sigmoid(model(F.features_torch(torch.from_numpy(np.stack(xs))))).numpy()
        tp.neg_w = list(1.0 + 8.0 * pr)
        hard = int((pr > 0.5).sum())
        print(f"hard negatives: {hard} of {len(pr)} clips score > 0.5 (sampled more often)", flush=True)
        model.train()
    nparam = sum(p.numel() for p in model.parameters())
    print(f"model: {nparam} parameters", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-3)
    total = args.epochs * args.steps
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=total, pct_start=0.1)
    dl = torch.utils.data.DataLoader(DS(tp, total * args.bs, 42), batch_size=args.bs, num_workers=8)
    lossf = nn.BCEWithLogitsLoss(pos_weight=torch.tensor(1.0))
    r = random.Random(3)
    best, step, t0 = None, 0, time.time()
    run = 0.0
    for x, y in dl:
        f = spec_augment(F.features_torch(x), r)
        loss = lossf(model(f), y)
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        run = 0.98 * run + 0.02 * loss.item()
        step += 1
        if step % args.steps == 0:
            ep = step // args.steps
            res = evaluate(model, vp)
            ok = [(t, fa, rc) for t, fa, rc in res if fa <= args.fa_per_hour]
            pick = max(ok, key=lambda z: z[2]) if ok else res[-1]
            ue = user_eval(model, vp)
            print(f"epoch {ep:3d} loss {run:.4f} {time.time() - t0:5.0f}s | " +
                  " ".join(f"{t:.2f}:{fa:.0f}/h,{rc * 100:.0f}%" for t, fa, rc in res) +
                  (f" | user held-out {ue[0] * 100:.0f}% @0.7, background max {ue[1]:.2f}" if ue else ""), flush=True)
            # the real microphone decides: held-out own recordings first (no background above 0.7), then the
            # synthetic validation streams
            key = ((ue[1] < 0.7, ue[0]) if ue else ()) + (pick[2], -pick[1])
            if ep > (0 if args.init else args.epochs // 3) and (best is None or key > best[2]):
                best = (pick, {k: v.clone() for k, v in model.state_dict().items()}, key)
        if step >= total:
            break
    (thr, fa, rc), sd, _ = best
    model.load_state_dict(sd)
    torch.save(sd, args.save)
    print(f"best: threshold {thr} -> {fa:.1f} false wake-ups/h, recall {rc * 100:.0f}% (validation streams)")
    export(model, thr, Path(args.out))


if __name__ == "__main__":
    main()
