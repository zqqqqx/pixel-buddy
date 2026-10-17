#!/bin/sh
# Firmware for others: built WITHOUT src/secrets.h (no WiFi, no API key inside), merged into one
# image, plus a web page that flashes it from the browser (ESP Web Tools, Chrome / Edge).
#   tools/dist.sh            -> dist/pixel.bin, dist/manifest.json, dist/index.html
# Host the dist folder on https (e.g. GitHub Pages) or try it locally:
#   cd dist && python3 -m http.server 8000   ->  http://localhost:8000
set -e
cd "$(dirname "$0")/.."
PIO=${PIO:-$HOME/.local/bin/pio}
PLATFORMIO_BUILD_FLAGS="-DPIXEL_PUBLIC=1" "$PIO" run
B=.pio/build/muma154
# nothing private may be inside
if grep -aq "sk-proj-\|sk-[A-Za-z0-9]\{20\}" $B/firmware.bin; then echo "API key found in the firmware - stop"; exit 1; fi
PY=$(head -1 "$PIO" | sed 's/^#!//')
MODEL=$(sed -n 's/^model *,[^,]*,[^,]*, *\(0x[0-9a-fA-F]*\).*/\1/p' partitions.csv)
"$PY" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --chip esp32s3 merge_bin -o dist/pixel.bin \
  --flash_mode qio --flash_freq 80m --flash_size 16MB \
  0x0 $B/bootloader.bin 0x8000 $B/partitions.bin 0xf000 $B/ota_data_initial.bin \
  0x20000 $B/firmware.bin "$MODEL" $B/srmodels/srmodels.bin
cat > dist/manifest.json <<JSON
{"name": "Pixel", "version": "$(date +%Y-%m-%d)", "new_install_prompt_erase": true,
 "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "pixel.bin", "offset": 0}]}]}
JSON
cp tools/install.html dist/index.html
cp $B/firmware.bin dist/pixel-update.bin   # for "Firmware-Update" in the web interface
echo "dist/ ready: pixel.bin (full install), pixel-update.bin (update over WiFi), index.html"
"$PIO" run >/dev/null   # back to the own build (with secrets.h) for the next upload
