"""Features of the "Pixel" wake word model - the same definition is implemented in C (src/kws.cpp).

16 kHz audio -> frames of 30 ms (480 samples, Hann window, DC removed) every 20 ms (320 samples)
-> 512-point FFT power -> 40 triangular mel bands (60..7600 Hz, HTK mel) -> ln(x + 1e-6)
-> per-band normalisation (mean/std of the training set, exported with the model).
The model sees the last 49 frames (~1 s).
"""
import numpy as np
import torch

SR = 16000
WIN = 480
HOP = 320
NFFT = 512
NMEL = 40
FMIN, FMAX = 60.0, 7600.0
FRAMES = 49
CLIP = WIN + (FRAMES - 1) * HOP  # 15840 samples
EPS = 1e-6


def hz2mel(f):
    return 2595.0 * np.log10(1.0 + f / 700.0)


def mel2hz(m):
    return 700.0 * (10 ** (m / 2595.0) - 1.0)


def mel_matrix():
    bins = np.arange(NFFT // 2 + 1) * SR / NFFT
    pts = mel2hz(np.linspace(hz2mel(FMIN), hz2mel(FMAX), NMEL + 2))
    m = np.zeros((NMEL, NFFT // 2 + 1), np.float32)
    for i in range(NMEL):
        lo, c, hi = pts[i], pts[i + 1], pts[i + 2]
        up = (bins - lo) / (c - lo)
        down = (hi - bins) / (hi - c)
        m[i] = np.maximum(0, np.minimum(up, down))
    return m


WINDOW = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(WIN) / WIN)).astype(np.float32)  # periodic Hann
MEL = mel_matrix()
_mel_t = torch.from_numpy(MEL)
_win_t = torch.from_numpy(WINDOW)


def features_torch(x: torch.Tensor) -> torch.Tensor:
    """x: [B, N] float audio in [-1, 1] -> [B, NMEL, frames] log mel (not normalised)."""
    fr = x.unfold(1, WIN, HOP)                          # [B, T, WIN]
    fr = fr - fr.mean(dim=2, keepdim=True)
    fr = fr * _win_t
    spec = torch.fft.rfft(fr, n=NFFT)                   # [B, T, 257]
    pw = spec.real ** 2 + spec.imag ** 2
    mel = torch.matmul(pw, _mel_t.T)                    # [B, T, 40]
    return torch.log(mel + EPS).transpose(1, 2)


def features_np(x: np.ndarray) -> np.ndarray:
    return features_torch(torch.from_numpy(x.astype(np.float32))[None])[0].numpy()
