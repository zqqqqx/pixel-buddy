#!/bin/sh
# Publishes the firmware as a GitHub release, where every Pixel Buddy finds its updates (src/update.cpp):
# version and repository from src/version.h, files from tools/dist.sh (built without secrets.h).
#   tools/release.sh [notes.md]      (without notes: GitHub writes them from the commits)
set -e
cd "$(dirname "$0")/.."
V=$(sed -n 's/^#define PIXEL_VERSION "\([^"]*\)".*/\1/p' src/version.h)
REPO=$(sed -n 's/^#define PIXEL_REPO "\([^"]*\)".*/\1/p' src/version.h)
if gh release view "v$V" --repo "$REPO" >/dev/null 2>&1; then echo "v$V exists already - raise PIXEL_VERSION in src/version.h"; exit 1; fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then echo "commit first: the release is built from the code as it is"; exit 1; fi
git push -q origin HEAD
tools/dist.sh
if [ -n "$1" ]; then NOTES="--notes-file $1"; else NOTES="--generate-notes"; fi
gh release create "v$V" dist/pixel.bin dist/pixel-update.bin dist/pixel-models.bin --repo "$REPO" --target "$(git rev-parse HEAD)" \
  --title "Pixel Buddy $V" $NOTES
echo "released v$V - robots with an older version install it at their update time"
