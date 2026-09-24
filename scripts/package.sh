#!/usr/bin/env bash
# Build the SD-card release zip: dist-zip/ForceCrateDigger-<version>.zip,
# unpacking to
#   AddOns/ForceCrateDigger/   cratedigger_host + web panel + the bundled
#                              runtime (yt-dlp, ffmpeg/ffprobe, private
#                              Python 3.11, private zlib module)
# Unzip it onto the SD card root.
#
# Runs the full build first - build-deps.sh, build-pyzlib.sh (needs zig),
# build-python.sh, then build.sh (Docker, armhf under QEMU) - so the zip
# always carries that day's latest yt-dlp. SKIP_BUILD=1 reuses an existing
# dist/ForceCrateDigger instead.
#
# The /media/<serial> sentinel in NSMODULE.json and shadow_page.conf (which
# deploy.sh patches on the device) is filled in with MockbaMod's fixed
# /media/662522 here, since an unzipped folder never goes through deploy.sh.
# .github/workflows/release.yml runs this for every published release.
set -euo pipefail
cd "${PKG_ROOT:-$(dirname "$0")/..}"
VER="${1:-$(git describe --tags --always)}"
MMPATH=/media/662522
if [ "${SKIP_BUILD:-}" != 1 ]; then
  scripts/build-deps.sh
  scripts/build-pyzlib.sh
  scripts/build-python.sh
  scripts/build.sh
fi

D=dist/ForceCrateDigger
for f in cratedigger_host bin/yt_dlp_daemon.py bin/yt-dlp bin/ffmpeg bin/ffprobe \
         bin/python3/bin/python3.11 bin/pylib/zlib.cpython-38-arm-linux-gnueabihf.so; do
  [ -e "$D/$f" ] || { echo "missing $D/$f - the build is incomplete" >&2; exit 1; }
done

OUT="$PWD/dist-zip"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
A="$STAGE/AddOns/ForceCrateDigger"
mkdir -p "$STAGE/AddOns" "$OUT"
# -L: SD cards are FAT/exFAT (no symlinks), so store real files.
cp -rL "$D" "$A"
sed -i "s|/media/<serial>|$MMPATH|g" "$A/NSMODULE.json" "$A/shadow_page.conf"
if grep -rl '/media/<serial>' "$A" --include='*.json' --include='*.conf'; then
  echo "/media/<serial> placeholder still present (above)" >&2; exit 1
fi
rm -f "$OUT/ForceCrateDigger-$VER.zip"
python3 -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', sys.argv[2], 'AddOns')" "$OUT/ForceCrateDigger-$VER" "$STAGE"
python3 -m zipfile -l "$OUT/ForceCrateDigger-$VER.zip" | grep -v '/bin/python3/lib/' 
ls -la "$OUT/ForceCrateDigger-$VER.zip"
