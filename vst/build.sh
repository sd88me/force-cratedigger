#!/usr/bin/env bash
# Build Crate Digger as a VST2 plugin for the MPC OS plugin host (armhf).
#   vst/build/cratedigger.so          -> /sdcard/vst/ on the device
#   vst/build/pluginlist-entry.xml    the <PLUGIN> line for MPC.settings' pluginList-arm
# The engine bundle (bin/yt-dlp, bin/python3, bin/ffmpeg) is found at runtime: see
# find_module_dir() in cratedigger_vst.cpp.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 vst/gen_params.py
docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD":/b -w /b arm32v7/gcc:12 bash -euxc '
  mkdir -p vst/build/obj
  gcc -O2 -fPIC -fvisibility=hidden -std=gnu11 -DYT_POSIX_SPAWN -Isrc/include -Isrc/dsp \
      -c src/dsp/yt_stream_plugin.c -o vst/build/obj/core.o
  g++ -O2 -fPIC -fvisibility=hidden -std=c++17 -Wall -Wextra -Wno-unused-parameter \
      -Isrc/include -Isrc -Ivst/build -c vst/cratedigger_vst.cpp -o vst/build/obj/vst.o
  g++ -shared -o vst/build/cratedigger.so vst/build/obj/core.o vst/build/obj/vst.o \
      -static-libstdc++ -static-libgcc -lpthread -ldl
  strip vst/build/cratedigger.so
  echo "-- exported --"; readelf --dyn-syms -W vst/build/cratedigger.so | grep -E " GLOBAL .* [0-9]+ [A-Za-z]" | grep -v UND
  echo "-- needed --"; readelf -d vst/build/cratedigger.so | grep NEEDED
  echo "-- highest glibc (device has 2.39) --"; readelf -V vst/build/cratedigger.so | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
'
md5sum vst/build/cratedigger.so
