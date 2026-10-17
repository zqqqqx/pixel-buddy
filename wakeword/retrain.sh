#!/bin/sh
# Retrain the "Pixel" wake word with the recordings from the robot. Compares the new model with
# the current one on your own recordings and only flashes it if you say so.
set -e
cd "$(dirname "$0")"
.venv/bin/python fetch_user.py "${1:-pixel.local}"
.venv/bin/python filter_user.py
cp kws_pixel.pt kws_pixel_current.pt
.venv/bin/python train.py --init kws_pixel_current.pt --epochs 15 --lr 1e-3 --fa_per_hour 1
echo "== current model on your recordings"; .venv/bin/python eval_user.py kws_pixel_current.pt
echo "== new model on your recordings";     .venv/bin/python eval_user.py kws_pixel.pt
printf "Flash the new model? Threshold (e.g. 0.72) or n: "
read thr
if [ "$thr" = "n" ] || [ -z "$thr" ]; then
  cp kws_pixel_current.pt kws_pixel.pt
  .venv/bin/python -c "import torch, train; m = train.KWS(); m.load_state_dict(torch.load('kws_pixel.pt')); train.export(m, 0.72, train.OUT_H)"
  echo "kept the current model"; exit 0
fi
.venv/bin/python -c "import torch, train; m = train.KWS(); m.load_state_dict(torch.load('kws_pixel.pt')); train.export(m, $thr, train.OUT_H)"
g++ -O2 -std=c++17 -o ctest ctest.cpp ../src/kws.cpp && .venv/bin/python check_c.py
cd .. && ~/.local/bin/pio run -t upload
