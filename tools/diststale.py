# PlatformIO post-build: dist/ (the firmware for others, tools/dist.sh) is a separate build without secrets.h.
# Rebuilding it every time would double the build time, so this only says when it is older than the code.
import glob, os

Import("env")


def check(source, target, env):
    root = env.subst("$PROJECT_DIR")
    dist = os.path.join(root, "dist", "pixel.bin")
    files = [f for p in ("src/*", "web/*", "partitions.csv", "sdkconfig.defaults") for f in glob.glob(os.path.join(root, p))]
    newest = max(os.path.getmtime(f) for f in files)
    if not os.path.exists(dist) or os.path.getmtime(dist) < newest:
        print("\n\033[33mdist/ is older than the code: run tools/dist.sh before sharing it.\033[0m\n")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check)
