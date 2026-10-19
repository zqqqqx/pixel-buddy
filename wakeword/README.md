# Wake word "Pixel"

A small own keyword-spotting network that runs on the ESP32-S3 (the other names – Buddy, Jarvis …
– use Espressif's WakeNet models). Everything happens offline on the robot; the PC is only needed
for training.

| Step | File | What it does |
|---|---|---|
| Dataset | `gen_dataset.py` | "Pixel" spoken by OpenAI TTS voices (13 voices x speaking styles) and Piper voices (German + 900 English LibriTTS speakers); negatives: everyday German sentences, sound-alikes (Pinsel, Pickel, Bissel ...), the robot's own voice; noise: ESC-50 + synthetic. |
| | `gen_more.py` | more German "Pixel" clips |
| | `filter_pos.py` | keeps only clips in which Whisper really hears "Pixel" (TTS sometimes garbles it) |
| Features | `kwsfeat.py` | 30 ms frames every 20 ms, 512 FFT, 40 log-mel bands, ~1 s window (49 frames) |
| Training | `train.py` | depthwise-separable 1-D CNN, ~21k parameters, ~0.5 M multiply-adds per evaluation; on-the-fly augmentation (speed, reverb, mic colouring, noise 0-30 dB SNR, level, SpecAugment); streaming validation that counts false wake-ups per hour exactly like the firmware; `--init` + hard negative mining for fine-tuning; exports `../src/kws_model.h` (BatchNorm and input normalisation folded in) |
| Inference | `../src/kws.cpp` | the same features + network in plain C, network every 100 ms, two evaluations averaged, 1.5 s refractory time |
| Check | `ctest.cpp`, `check_c.py` | runs the C code on the PC and compares it with PyTorch (difference ~1e-5) |

## Make it better with your own voice

1. App -> Settings -> System -> Train the wake word -> "Record the name": say "Pixel" ~20 times
   (quiet, loud, near, far) and "Record the room": some background (TV, talking, music - without "Pixel").
2. On the PC: `./retrain.sh` - downloads the recordings, trains, checks the C code, flashes.

Every 5th own recording is kept back for validation and decides which epoch is exported. Your recordings
count 6x in training. Every activation is recorded (2 s up to the wake word + 4 s after it). When somebody
spoke afterwards, the robot keeps the "Pixel" (2 s up to the detection) as `ap_*.wav`; when nobody said
anything (no words in the transcript), all of it as a false wake-up `fa_*.wav`. At most 800 recordings stay on
the card (app -> "Delete automatic recordings" once they are on the PC). `fetch_user.py` puts `ap_` into the
positives (training only) and `fa_` into the negatives; `eval_user.py` tests the network on them. A real
"Pixel" with nothing after it lands in `fa_` too - listen to them before training.

## Setup

```
python3 -m venv .venv
.venv/bin/pip install torch --index-url https://download.pytorch.org/whl/cpu
.venv/bin/pip install numpy scipy soundfile piper-tts requests
.venv/bin/python gen_dataset.py && .venv/bin/python gen_more.py && .venv/bin/python filter_pos.py
.venv/bin/python train.py
```

`../openai.txt` must contain an OpenAI API key (TTS + Whisper, a few cents in total).
