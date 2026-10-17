# PlatformIO pre-script: packs the wake word model chosen in src/config.h into
# srmodels.bin and adds it to the upload at the "model" partition offset.
import os, re, subprocess, sys
Import("env")

proj = env.subst("$PROJECT_DIR")
build = env.subst("$BUILD_DIR")
cfg = open(os.path.join(proj, "src", "config.h")).read()
model = re.search(r'#define\s+CFG_WAKEWORD_MODEL\s+"([^"]+)"', cfg).group(1)
sr = os.path.join(proj, "managed_components", "espressif__esp-sr")
out = os.path.join(build, "srmodels", "srmodels.bin")

if os.path.isdir(sr):
    os.makedirs(build, exist_ok=True)
    fake = os.path.join(build, "sdkconfig.srmodels")
    with open(fake, "w") as f:
        f.write('CONFIG_IDF_TARGET="esp32s3"\nCONFIG_SR_WN_%s=y\n' % model.upper())
    subprocess.check_call([env.subst("$PYTHONEXE"), os.path.join(sr, "model", "movemodel.py"),
                           "-d1", fake, "-d2", sr, "-d3", build], stdout=subprocess.DEVNULL)
    print("srmodels: packed %s (%d KB)" % (model, os.path.getsize(out) // 1024))
    # offset of the "model" partition from partitions.csv
    parts = open(os.path.join(proj, "partitions.csv")).read()
    off = re.search(r"^model\s*,[^,]*,[^,]*,\s*(0x[0-9a-fA-F]+)", parts, re.M).group(1)
    env.Append(FLASH_EXTRA_IMAGES=[(off, out)])
else:
    print("srmodels: esp-sr not downloaded yet, build once more to pack the wake word model")
