#!/usr/bin/env python3
"""Wake word "Pixel": build the raw dataset (16 kHz mono WAV) in wakeword/data/.

  pos/      the word "Pixel" (alone or at the end of a short phrase), many speakers and styles
              - OpenAI gpt-4o-mini-tts: 13 voices x speaking styles (needs ../openai.txt)
              - Piper (offline): German voices incl. 236 MLS speakers and 8 emotions,
                English LibriTTS (900+ speakers) for even more voices
  neg/      speech that is NOT the wake word: everyday German sentences, sound-alike words
            (Pinsel, Pickel, Bissel ...), the robot's own voice, English sentences
  noise/    background: synthetic (white/pink/brown, hum, fan) + ESC-50 environmental sounds
  user/     optional: real recordings from the robot (see README), used as is

Re-running only creates what is missing. Usage:  .venv/bin/python gen_dataset.py
"""
import concurrent.futures as cf
import io
import json
import os
import random
import re
import sys
import wave
from pathlib import Path

import numpy as np
import requests
from scipy.signal import resample_poly

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
SR = 16000
rng = random.Random(1234)


def save(path: Path, x: np.ndarray, sr: int):
    if sr != SR:
        g = np.gcd(sr, SR)
        x = resample_poly(x.astype(np.float32), SR // g, sr // g)
    x = np.asarray(x, np.float32)
    peak = np.max(np.abs(x)) + 1e-9
    x = x / peak * 0.7 if peak > 1.0 else x
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())


# ------------------------------------------------------------------ texts
POS_TEXTS = ["Pixel", "Pixel!", "Pixel?", "Pixel.", "Pixel...", "Hey Pixel", "Hallo Pixel", "Hey, Pixel!",
             "Okay Pixel", "Na, Pixel?", "Du, Pixel", "Pixel, Pixel", "He Pixel!", "Sag mal, Pixel"]
POS_TEXTS_EN = ["Pixel", "Pixel!", "Pixel?", "Hey Pixel", "Hey, Pixel!", "Okay Pixel"]

# sound-alikes and words with similar parts (must NOT trigger)
CONFUSABLE = ["Pinsel", "Bissel", "ein bissel", "Pickel", "Wechsel", "Sessel", "Kessel", "Achsel", "Deichsel",
              "Brezel", "Pixie", "Pixar", "Picknick", "Pistole", "Pilz", "Pizza", "Mixer", "Fichtel", "Bixby",
              "Pippi", "Puzzle", "bisschen", "Pegel", "Pinkel", "Pils", "Wichtel", "Nickel", "Sichel", "Ticket",
              "Fixstern", "fix", "Bix", "Mix", "Text", "Exzel", "Excel", "Pfeifer", "Wickel", "Gipfel", "Zipfel",
              "Krempel", "Tempel", "Wimpel", "Simpel", "Ritzel", "Bitte schön", "Pick sie", "Pech", "Pinsel bitte",
              "Pikachu", "Physik", "Biss", "Hexe", "Haxe", "Saxophon", "Taxi", "Praxis", "Max", "Rex", "Felix",
              "Alexa", "Hallo", "Hey", "Okay", "Siri", "Ping", "Pünktlich", "Pickelig", "Spiegel", "Schlüssel",
              "Stiefel", "Kipfel", "Pistazie", "Bügel", "Igel", "Vogel", "Nebel", "Esel", "Ampel", "Insel", "Mittel"]

def chat_sentences():
    """What is said to / by the robot (conversation log on the SD card, via the web interface)."""
    cache = DATA / "chatlog.txt"
    if not cache.exists():
        try:
            t = requests.get("http://pixel.local/api/log", timeout=5).text
        except Exception:
            try:
                t = requests.get("http://pixel.local/api/log", timeout=5).text
            except Exception:
                t = ""
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(t)
    out = []
    for line in cache.read_text().splitlines():
        m = re.match(r"\S+ \S+\s+[^:]+:\s*(.+)", line)
        if m and "pixel" not in m.group(1).lower() and len(m.group(1)) > 3:
            out.append(m.group(1).strip()[:200])
    return out


def openai_key():
    p = HERE.parent / "openai.txt"
    m = re.search(r"sk-[A-Za-z0-9_-]+", p.read_text()) if p.exists() else None
    return m.group(0) if m else None


def gpt_sentences(key, n_batches=8):
    """Everyday German sentences (no 'Pixel') from a cheap text model."""
    cache = DATA / "sentences.json"
    if cache.exists():
        return json.loads(cache.read_text())
    topics = ["Alltag und Familie", "Essen und Kochen", "Arbeit und Ausbildung (IT)", "Wetter und Natur",
              "Technik, Handy und Computer", "Freizeit, Sport, Musik", "Smalltalk mit einem Sprachassistenten",
              "Fragen an einen Roboter, Befehle (Licht, Farbe, Lautstaerke)"]
    out = []
    for t in topics[:n_batches]:
        r = requests.post("https://api.openai.com/v1/responses", headers={"Authorization": "Bearer " + key},
                          json={"model": "gpt-5.4-nano", "reasoning": {"effort": "low"},
                                "input": f"Schreibe 70 verschiedene, kurze bis mittellange deutsche Saetze, wie man sie "
                                         f"im Alltag spricht (umgangssprachlich, auch Fragen und Ausrufe), Thema: {t}. "
                                         f"Das Wort Pixel darf NIE vorkommen. Ein Satz pro Zeile, keine Nummerierung."},
                          timeout=120).json()
        text = "".join(c.get("text", "") for o in r.get("output", []) if o.get("type") == "message"
                       for c in o.get("content", []))
        out += [s.strip(" -•\t") for s in text.splitlines() if len(s.strip()) > 5 and "pixel" not in s.lower()]
        print(f"  sentences: {len(out)}", flush=True)
    cache.write_text(json.dumps(out, ensure_ascii=False, indent=0))
    return out


EN_SENT = ["What time is it right now?", "Turn the music down a little.", "I think it's going to rain later.",
           "Can you send me the picture?", "Let's grab some pizza tonight.", "That pixel art game looks fun.",
           "The meeting starts in ten minutes.", "Where did I put my keys?", "This is a really nice picture.",
           "I need to fix the bicycle.", "Pick up the phone, please.", "She mixed the colors together.",
           "The pickle jar is empty.", "Check your email when you get a chance.", "Happy birthday to you!"]

# ------------------------------------------------------------------ OpenAI TTS
OAI_VOICES = ["alloy", "ash", "ballad", "coral", "echo", "fable", "nova", "onyx", "sage", "shimmer", "verse",
              "marin", "cedar"]
OAI_STYLES = ["", "Sprich ganz normal und deutlich.", "Ruf freudig, als wuerdest du jemanden begruessen.",
              "Fluestere leise.", "Frag neugierig.", "Sag es muede und gedehnt.", "Sag es schnell und beilaeufig.",
              "Ruf es laut aus einiger Entfernung.", "Sprich wie ein aufgeregtes Kind.", "Sag es genervt.",
              "Sag es freundlich und warm.", "Sprich mit leichtem englischen Akzent.", "Sag es fragend und zoegerlich.",
              "Sprich tief und ruhig.", "Sing es kurz."]


def oai_tts(key, text, voice, style, path):
    if path.exists():
        return
    body = {"model": "gpt-4o-mini-tts", "voice": voice, "input": text, "response_format": "pcm"}
    if style:
        body["instructions"] = style
    for _ in range(3):
        try:
            r = requests.post("https://api.openai.com/v1/audio/speech", headers={"Authorization": "Bearer " + key},
                              json=body, timeout=60)
            if r.status_code == 200 and len(r.content) > 2000:
                save(path, np.frombuffer(r.content, np.int16).astype(np.float32) / 32768, 24000)
                return
        except requests.RequestException:
            pass


# ------------------------------------------------------------------ Piper
PIPER = [("de", "de_DE", "thorsten", "medium"), ("de", "de_DE", "thorsten", "high"), ("de", "de_DE", "eva_k", "x_low"),
         ("de", "de_DE", "kerstin", "low"), ("de", "de_DE", "karlsson", "low"), ("de", "de_DE", "ramona", "low"),
         ("de", "de_DE", "pavoque", "low"), ("de", "de_DE", "thorsten_emotional", "medium"),
         ("de", "de_DE", "mls", "medium"), ("en", "en_US", "libritts_r", "medium")]


def piper_voice(lang, loc, name, q):
    d = DATA / "voices"
    d.mkdir(parents=True, exist_ok=True)
    base = f"{loc}-{name}-{q}"
    for ext in (".onnx", ".onnx.json"):
        f = d / (base + ext)
        if not f.exists():
            url = f"https://huggingface.co/rhasspy/piper-voices/resolve/main/{lang}/{loc}/{name}/{q}/{base}{ext}"
            print("  download", base + ext, flush=True)
            r = requests.get(url, timeout=600)
            r.raise_for_status()
            f.write_bytes(r.content)
    from piper import PiperVoice
    return PiperVoice.load(str(d / (base + ".onnx")))


def piper_say(voice, text, path, speaker=None):
    if path.exists():
        return
    from piper import SynthesisConfig
    cfg = SynthesisConfig(speaker_id=speaker, length_scale=rng.uniform(0.75, 1.35),
                          noise_scale=rng.uniform(0.4, 0.9), noise_w_scale=rng.uniform(0.5, 1.0))
    chunks = list(voice.synthesize(text, syn_config=cfg))
    if not chunks:
        return
    x = np.concatenate([c.audio_float_array for c in chunks])
    save(path, x, chunks[0].sample_rate)


# ------------------------------------------------------------------ noise
def synth_noise():
    d = DATA / "noise"
    if (d / "synth_09.wav").exists():
        return
    n = SR * 30
    for i in range(10):
        w = np.random.default_rng(i).standard_normal(n)
        f = np.fft.rfft(w)
        freqs = np.fft.rfftfreq(n, 1 / SR) + 1
        kind = i % 5
        if kind == 1:
            f /= np.sqrt(freqs)          # pink
        elif kind == 2:
            f /= freqs                   # brown
        elif kind == 3:                  # fan / air: band limited rumble
            f *= np.exp(-((freqs - 300) / 400) ** 2) + 0.05
        x = np.fft.irfft(f, n)
        if kind == 4:                    # mains hum + harmonics
            t = np.arange(n) / SR
            x = 0.2 * x / np.std(x) + sum(np.sin(2 * np.pi * 50 * k * t + k) / k for k in range(1, 8))
        save(d / f"synth_{i:02d}.wav", 0.3 * x / (np.max(np.abs(x)) + 1e-9), SR)


def esc50():
    """ESC-50 environmental sounds (dogs, rain, keyboard typing, doors, ...), 2000 x 5 s."""
    d = DATA / "noise"
    if any(d.glob("esc_*.wav")):
        return
    print("  download ESC-50 ...", flush=True)
    import zipfile
    z = DATA / "esc50.zip"
    if not z.exists():
        r = requests.get("https://github.com/karoldvl/ESC-50/archive/master.zip", timeout=1200)
        r.raise_for_status()
        z.write_bytes(r.content)
    import soundfile as sf
    with zipfile.ZipFile(z) as zf:
        for name in zf.namelist():
            if name.endswith(".wav") and "/audio/" in name:
                x, sr = sf.read(io.BytesIO(zf.read(name)), dtype="float32")
                if x.ndim > 1:
                    x = x.mean(1)
                save(d / ("esc_" + Path(name).stem + ".wav"), x, sr)
    z.unlink()


# ------------------------------------------------------------------ main
def main():
    DATA.mkdir(exist_ok=True)
    key = openai_key()
    print("noise ...", flush=True)
    synth_noise()
    esc50()

    sentences = chat_sentences()
    print(f"chat log sentences: {len(sentences)}", flush=True)
    if key:
        sentences += gpt_sentences(key)
    print(f"sentences total: {len(sentences)}", flush=True)

    # --- OpenAI positives / robot-voice negatives
    if key:
        print("OpenAI TTS ...", flush=True)
        jobs = []
        for v in OAI_VOICES:
            for si, st in enumerate(OAI_STYLES):
                txt = rng.choice(POS_TEXTS)
                jobs.append((txt, v, st, DATA / "pos" / f"oai_{v}_{si:02d}.wav"))
            for k in range(8):  # the same voices saying sound-alikes / sentences
                txt = rng.choice(CONFUSABLE + sentences)
                jobs.append((txt, v, rng.choice(OAI_STYLES), DATA / "neg" / f"oai_{v}_{k:02d}.wav"))
        with cf.ThreadPoolExecutor(8) as ex:
            list(ex.map(lambda j: oai_tts(key, *j[:3], j[3]), jobs))

    # --- Piper
    for lang, loc, name, q in PIPER:
        print(f"piper {name} ...", flush=True)
        v = piper_voice(lang, loc, name, q)
        nspk = v.config.num_speakers
        en = lang == "en"
        if nspk > 1:
            spk = list(range(nspk))
            rng.shuffle(spk)
            spk = spk[:300]
            for s in spk:
                for k in range(2):
                    piper_say(v, rng.choice(POS_TEXTS_EN if en else POS_TEXTS), DATA / "pos" / f"pp_{name}_{s}_{k}.wav", s)
                piper_say(v, rng.choice(EN_SENT if en else sentences + CONFUSABLE), DATA / "neg" / f"pp_{name}_{s}.wav", s)
                if not en:
                    piper_say(v, rng.choice(CONFUSABLE), DATA / "neg" / f"ppc_{name}_{s}.wav", s)
        else:
            for k in range(30):
                piper_say(v, rng.choice(POS_TEXTS_EN if en else POS_TEXTS), DATA / "pos" / f"pp_{name}_{q}_{k}.wav")
            for k in range(60):
                txt = rng.choice(CONFUSABLE) if k % 2 else rng.choice(sentences)
                piper_say(v, txt, DATA / "neg" / f"pp_{name}_{q}_{k}.wav")
    for d in ("pos", "neg", "noise"):
        print(d, len(list((DATA / d).glob("*.wav"))))


if __name__ == "__main__":
    main()
