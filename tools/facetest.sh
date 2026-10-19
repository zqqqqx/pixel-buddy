#!/bin/sh
# The web interface draws the personality pictures with its own copy of the face formulas (web/index.html,
# drawFace). This renders every personality both ways - the browser code under node, the firmware in the
# simulator - and compares the outlines (overlap of the eye areas). Needs node, ffmpeg, python3 + numpy.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
g++ -O2 -std=c++17 -Isrc sim/sim.cpp src/eyes.cpp -o "$T/sim"
cat > "$T/face.js" <<'JS'
const fs=require('fs');const src=fs.readFileSync('web/index.html','utf8');
const js=src.match(/<script>([\s\S]*)<\/script>/)[1];
const pre='const PRESETS='+fs.readFileSync('src/presets.h','utf8').match(/R"PRESETS\(([\s\S]*)\)PRESETS"/)[1]+';';
const fns=js.match(/function shapeK[\s\S]*?\n  ctx.putImageData\(img,0,0\)\}/)[0];
eval(pre+'\n'+fns+`
for(const p of PRESETS){const cv={width:240,height:165,getContext(){return{createImageData(w,h){return{data:new Uint8ClampedArray(w*h*4)}},
 putImageData(img){const b=Buffer.alloc(240*165);for(let i=0;i<240*165;i++)b[i]=img.data[i*4]|img.data[i*4+1]|img.data[i*4+2]?255:0;
 fs.writeFileSync(process.argv[2]+'/'+p.id+'.web',b)}}}};drawFace(cv,p.look,{sclera:'#ffffff',iris:'#808080',bg:'#000000'})
 const k={lid_top:'lidTop',lid_bottom:'lidBottom',eye_w:'eyeW',eye_h:'eyeH',iris_w:'irisW',iris_h:'irisH'};
 fs.writeFileSync(process.argv[2]+'/'+p.id+'.look',Object.entries(p.look).filter(([a,v])=>typeof v==='number').map(([a,v])=>(k[a]||a)+'='+v).join(' '))}`);
JS
node "$T/face.js" "$T"
for f in "$T"/*.look; do
  id=$(basename "$f" .look)
  # calm and in the middle: no wandering, as little resting movement as the engine allows
  SIM_LOOK="$(cat "$f") energy=0.3 roam=0" "$T/sim" auto 5 2>/dev/null |
    ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 240x240 -r 50 -i - -vf "select='eq(n\,240)',crop=240:165:0:36" \
      -frames:v 1 -f rawvideo -pix_fmt gray "$T/$id.sim"
done
python3 - "$T" <<'PY'
import sys, glob, os, numpy as np
d, bad = sys.argv[1], 0
for f in sorted(glob.glob(d + "/*.web")):
    a = np.fromfile(f, np.uint8) > 0
    b = np.fromfile(f[:-4] + ".sim", np.uint8) > 20
    iou = (a & b).sum() / max(1, (a | b).sum())
    ok = iou > 0.8
    bad += not ok
    print(f"{os.path.basename(f)[:-4]:10s} overlap {iou:.2f} {'ok' if ok else 'DIFFERENT'}")
sys.exit(1 if bad else 0)
PY
rm -rf "$T"
echo "faces: web and firmware agree"
