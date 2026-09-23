#!/usr/bin/env bash
# =============================================================================
# Fetch a private, self-contained Python 3.11 for arm-linux-gnueabihf and
# drop it into build/deps/bin/python3/, from where scripts/build.sh copies
# it into dist/ForceCrateDigger/bin/python3/.
#
# WHY THIS EXISTS: the device's own Python (/usr/bin/python3, see
# scripts/build-pyzlib.sh's header) is 3.8.10, and yt-dlp dropped Python 3.8
# support after release 2024.10.22 - every later release hard-requires 3.9+.
# YouTube's own anti-bot JS challenge keeps evolving and yt-dlp has to keep
# shipping new releases to track it; frozen at a release from
# September 2024, this addon's YouTube-backed playback (which is most of
# what Discogs previews resolve through) eventually stops working
# altogether - confirmed live 2026-09-23, even the first video ever
# uploaded to YouTube failed to resolve. There is no way to fix that by
# patching our own code; it needs a newer yt-dlp, which needs a newer
# Python than the device has.
#
# NOT a device-wide Python upgrade - deliberately, same reasoning as
# build-pyzlib.sh's private module: the device's Python is a shared
# resource (nodeServer, other addons' scripts) and replacing or patching it
# system-wide is a large-blast-radius change this addon has no business
# making. Instead this bundles its OWN private interpreter, same
# self-contained-addon pattern already used for yt-dlp/ffmpeg, just a
# bigger download. src/dsp/yt_stream_plugin.c's start_daemon_locked() execs
# this bundled interpreter's absolute path directly (not the bare "python3"
# on PATH), so nothing about the device's own Python changes at all.
#
# No cross-compilation needed: astral-sh/python-build-standalone publishes
# ready-made, self-contained CPython builds for
# armv7-unknown-linux-gnueabihf - this just downloads one. "install_only_
# stripped" is used deliberately: smallest variant that still has the full
# stdlib (including a real zlib/ssl/etc, built in - no repeat of the
# private-zlib-module dance from build-pyzlib.sh is needed for this
# interpreter), no debug symbols or pip needed since nothing here installs
# packages.
#
# 3.11 chosen over newer options (3.12/3.13/3.14 also available) as the
# most mature/widely-deployed choice with the least surface for surprises
# on this device - not "latest for its own sake". Revisit only if a real
# need arises, not proactively.
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

PBS_RELEASE_TAG="20260901"
PYTHON_VERSION="3.11.16"
PYTHON_ASSET="cpython-${PYTHON_VERSION}+${PBS_RELEASE_TAG}-armv7-unknown-linux-gnueabihf-install_only_stripped.tar.gz"
PYTHON_URL="https://github.com/astral-sh/python-build-standalone/releases/download/${PBS_RELEASE_TAG}/${PYTHON_ASSET}"

OUT_DIR="$PWD/build/deps/bin"
WORK_DIR="$PWD/build/deps/work/python"
rm -rf "$WORK_DIR" "$OUT_DIR/python3"
mkdir -p "$WORK_DIR" "$OUT_DIR"

echo "=== Fetching Python ${PYTHON_VERSION} (armv7-unknown-linux-gnueabihf) ==="
curl -fsSL -o "$WORK_DIR/python.tar.gz" "$PYTHON_URL"
tar -xzf "$WORK_DIR/python.tar.gz" -C "$WORK_DIR"
# python-build-standalone always extracts to a top-level "python/" dir.
mv "$WORK_DIR/python" "$OUT_DIR/python3"
rm -rf "$WORK_DIR"

echo "-- fetched --"
file "$OUT_DIR/python3/bin/python3.11"
du -sh "$OUT_DIR/python3"
echo "NOTE: this binary is ARM - it will not run on this build host, only on"
echo "the device. That's expected; there is nothing to smoke-test locally"
echo "beyond the file above existing and looking like a real ELF binary."
