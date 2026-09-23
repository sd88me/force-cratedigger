#!/usr/bin/env bash
# =============================================================================
# Build cratedigger_host for the Akai Force (armhf) inside a QEMU-emulated
# armhf container (same toolchain as force-maze/force-acid — see
# scripts/Dockerfile) and assemble the MockbaMod addon folder under dist/.
#
#   dist/cratedigger_host         the armhf binary (DSP core + ring out + ctrl sock)
#   dist/ForceCrateDigger/        the addon folder (drop into AddOns/)
#
# This only builds the native binary. Run scripts/build-deps.sh separately
# (or first) to fetch yt-dlp/ffmpeg into dist/ForceCrateDigger/bin/ — without
# those the engine starts but every search/stream fails with a clear error.
#
# Requires Docker with armhf emulation (see force-acid/scripts/build.sh for
# the one-time qemu-user-static setup on a bare dockerd).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=force-cratedigger-builder
PLATFORM=linux/arm/v7

echo "== build armhf toolchain image ($PLATFORM) =="
docker build --platform "$PLATFORM" -t "$IMG" scripts

echo "== compile + package (native armhf under QEMU — slow, be patient) =="
docker run --rm --platform "$PLATFORM" \
  -u "$(id -u):$(id -g)" -v "$PWD":/build -w /build "$IMG" bash -euxc '
  COMMON="-O2 -Wall -Wextra -Wno-unused-parameter -Isrc -Isrc/include"
  rm -rf dist && mkdir -p dist/ForceCrateDigger/web obj

  # DSP core — C, upstream logic untouched apart from the two Force-path
  # #defines noted at the top of src/dsp/yt_stream_plugin.c. -Isrc/include
  # resolves its own #include "plugin_api_v1.h" (upstream keeps that header
  # next to the .c file; here it lives in src/include/, shared with the
  # host shim'\''s own include of it).
  # No -std= override, matching upstream schwung-webstream'\''s own build.sh
  # invocation exactly: GCC'\''s default GNU dialect implicitly defines
  # _DEFAULT_SOURCE, exposing fdopen/popen/strtok_r/kill/usleep without extra
  # feature-test macros. -std=c11 (strict ISO mode) hides those and fails.
  gcc $COMMON -c src/dsp/yt_stream_plugin.c -o obj/yt_stream_plugin.o

  # host shim — C++
  g++ $COMMON -std=c++14 -c src/cratedigger_host.cpp      -o obj/cratedigger_host.o

  g++ obj/yt_stream_plugin.o obj/cratedigger_host.o \
      -lpthread -lrt \
      -o dist/cratedigger_host

  strip dist/cratedigger_host
  file dist/cratedigger_host
  echo "-- shared libs the Force must provide --"
  readelf -d dist/cratedigger_host | grep NEEDED
  echo "-- highest glibc symbol version required (want <= 2.28) --"
  { readelf -V dist/cratedigger_host | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -3; } || true

  rm -rf obj

  cp dist/cratedigger_host           dist/ForceCrateDigger/cratedigger_host
  cp addon/module.json             dist/ForceCrateDigger/
  cp addon/manage.sh               dist/ForceCrateDigger/
  cp addon/run_cratedigger_host.sh   dist/ForceCrateDigger/
  cp addon/NSMODULE.json           dist/ForceCrateDigger/
  cp addon/shadow_page.conf        dist/ForceCrateDigger/
  cp addon/web/manage.sh           dist/ForceCrateDigger/web/
  cp addon/web/run_cratedigger_web.sh dist/ForceCrateDigger/web/
  cp addon/web/server.py           dist/ForceCrateDigger/web/
  cp addon/web/index.html          dist/ForceCrateDigger/web/
  cp src/bin/yt_dlp_daemon.py      dist/ForceCrateDigger/  # moved into bin/ below
  mkdir -p dist/ForceCrateDigger/bin
  mv dist/ForceCrateDigger/yt_dlp_daemon.py dist/ForceCrateDigger/bin/yt_dlp_daemon.py
  chmod 0755 dist/ForceCrateDigger/cratedigger_host
  chmod 0755 dist/ForceCrateDigger/manage.sh dist/ForceCrateDigger/run_cratedigger_host.sh
  chmod 0755 dist/ForceCrateDigger/web/manage.sh dist/ForceCrateDigger/web/run_cratedigger_web.sh

  if [ -d build/deps/bin ]; then
    cp build/deps/bin/* dist/ForceCrateDigger/bin/ 2>/dev/null || true
    chmod 0755 dist/ForceCrateDigger/bin/* || true
    echo "-- bundled runtime deps --"
    ls -la dist/ForceCrateDigger/bin
  else
    echo "!! build/deps/bin not found — run scripts/build-deps.sh first for a"
    echo "!! self-contained bundle, or copy your own yt-dlp/ffmpeg/ffprobe"
    echo "!! into dist/ForceCrateDigger/bin/ by hand before deploying."
  fi

  # The device'\''s Python has no zlib module at all - yt-dlp refuses to even
  # start without it. build-pyzlib.sh builds a private, self-contained
  # zlib.cpython-38-arm-linux-gnueabihf.so for it; cratedigger_host.cpp puts
  # bin/pylib/ on PYTHONPATH before spawning the yt-dlp daemon. See both
  # files'\'' comments for the full story.
  if [ -f build/deps/bin/pylib/zlib.cpython-38-arm-linux-gnueabihf.so ]; then
    mkdir -p dist/ForceCrateDigger/bin/pylib
    cp build/deps/bin/pylib/zlib.cpython-38-arm-linux-gnueabihf.so dist/ForceCrateDigger/bin/pylib/
    echo "-- bundled private zlib module --"
    ls -la dist/ForceCrateDigger/bin/pylib
  else
    echo "!! build/deps/bin/pylib/zlib.cpython-38-*.so not found — run"
    echo "!! scripts/build-pyzlib.sh first, or yt-dlp will refuse to start"
    echo "!! on-device (\"yt-dlp is unavailable\") with no zlib module."
  fi

  # Private, bundled Python 3.11 for the yt-dlp daemon (see
  # scripts/build-python.sh'\''s header for the full story - short version:
  # the device'\''s system Python is 3.8, which caps yt-dlp at a release from
  # September 2024, and YouTube'\''s own anti-bot JS challenge moves fast
  # enough that this eventually breaks ALL YouTube-backed playback, not
  # just a minority of videos). yt_stream_plugin.c execs
  # bin/python3/bin/python3.11 by absolute path directly, so this must be
  # present at that exact path for the daemon to start at all.
  if [ -x build/deps/bin/python3/bin/python3.11 ]; then
    rm -rf dist/ForceCrateDigger/bin/python3
    cp -R build/deps/bin/python3 dist/ForceCrateDigger/bin/python3
    echo "-- bundled private Python --"
    file dist/ForceCrateDigger/bin/python3/bin/python3.11
    du -sh dist/ForceCrateDigger/bin/python3
  else
    echo "!! build/deps/bin/python3/bin/python3.11 not found — run"
    echo "!! scripts/build-python.sh first, or the yt-dlp daemon will fail"
    echo "!! to even start on-device (exec of a nonexistent path)."
  fi

  ls -la dist dist/ForceCrateDigger
'
echo "== done -> dist/ForceCrateDigger/ =="
