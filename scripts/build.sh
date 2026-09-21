#!/usr/bin/env bash
# =============================================================================
# Build webstream_host for the Akai Force (armhf) inside a QEMU-emulated
# armhf container (same toolchain as force-maze/force-acid — see
# scripts/Dockerfile) and assemble the MockbaMod addon folder under dist/.
#
#   dist/webstream_host         the armhf binary (DSP core + ring out + ctrl sock)
#   dist/ForceWebstream/        the addon folder (drop into AddOns/)
#
# This only builds the native binary. Run scripts/build-deps.sh separately
# (or first) to fetch yt-dlp/ffmpeg into dist/ForceWebstream/bin/ — without
# those the engine starts but every search/stream fails with a clear error.
#
# Requires Docker with armhf emulation (see force-acid/scripts/build.sh for
# the one-time qemu-user-static setup on a bare dockerd).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=force-webstream-builder
PLATFORM=linux/arm/v7

echo "== build armhf toolchain image ($PLATFORM) =="
docker build --platform "$PLATFORM" -t "$IMG" scripts

echo "== compile + package (native armhf under QEMU — slow, be patient) =="
docker run --rm --platform "$PLATFORM" \
  -u "$(id -u):$(id -g)" -v "$PWD":/build -w /build "$IMG" bash -euxc '
  COMMON="-O2 -Wall -Wextra -Wno-unused-parameter -Isrc -Isrc/include"
  rm -rf dist && mkdir -p dist/ForceWebstream/web obj

  # DSP core — C, upstream logic untouched apart from the two Force-path
  # #defines noted at the top of src/dsp/yt_stream_plugin.c. -Isrc/include
  # resolves its own #include "plugin_api_v1.h" (upstream keeps that header
  # next to the .c file; here it lives in src/include/, shared with the
  # host shim's own include of it).
  # No -std= override, matching upstream schwung-webstream'\''s own build.sh
  # invocation exactly: GCC'\''s default GNU dialect implicitly defines
  # _DEFAULT_SOURCE, exposing fdopen/popen/strtok_r/kill/usleep without extra
  # feature-test macros. -std=c11 (strict ISO mode) hides those and fails.
  gcc $COMMON -c src/dsp/yt_stream_plugin.c -o obj/yt_stream_plugin.o

  # host shim — C++
  g++ $COMMON -std=c++14 -c src/webstream_host.cpp      -o obj/webstream_host.o

  g++ obj/yt_stream_plugin.o obj/webstream_host.o \
      -lpthread \
      -o dist/webstream_host

  strip dist/webstream_host
  file dist/webstream_host
  echo "-- shared libs the Force must provide --"
  readelf -d dist/webstream_host | grep NEEDED
  echo "-- highest glibc symbol version required (want <= 2.28) --"
  { readelf -V dist/webstream_host | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -3; } || true

  rm -rf obj

  cp dist/webstream_host           dist/ForceWebstream/webstream_host
  cp addon/module.json             dist/ForceWebstream/
  cp addon/manage.sh               dist/ForceWebstream/
  cp addon/run_webstream_host.sh   dist/ForceWebstream/
  cp addon/NSMODULE.json           dist/ForceWebstream/
  cp addon/shadow_page.conf        dist/ForceWebstream/
  cp addon/web/manage.sh           dist/ForceWebstream/web/
  cp addon/web/run_webstream_web.sh dist/ForceWebstream/web/
  cp addon/web/server.py           dist/ForceWebstream/web/
  cp addon/web/index.html          dist/ForceWebstream/web/
  cp src/bin/yt_dlp_daemon.py      dist/ForceWebstream/  # moved into bin/ below
  mkdir -p dist/ForceWebstream/bin
  mv dist/ForceWebstream/yt_dlp_daemon.py dist/ForceWebstream/bin/yt_dlp_daemon.py
  chmod 0755 dist/ForceWebstream/webstream_host
  chmod 0755 dist/ForceWebstream/manage.sh dist/ForceWebstream/run_webstream_host.sh
  chmod 0755 dist/ForceWebstream/web/manage.sh dist/ForceWebstream/web/run_webstream_web.sh

  if [ -d build/deps/bin ]; then
    cp build/deps/bin/* dist/ForceWebstream/bin/
    chmod 0755 dist/ForceWebstream/bin/* || true
    echo "-- bundled runtime deps --"
    ls -la dist/ForceWebstream/bin
  else
    echo "!! build/deps/bin not found — run scripts/build-deps.sh first for a"
    echo "!! self-contained bundle, or copy your own yt-dlp/ffmpeg/ffprobe"
    echo "!! into dist/ForceWebstream/bin/ by hand before deploying."
  fi

  ls -la dist dist/ForceWebstream
'
echo "== done -> dist/ForceWebstream/ =="
