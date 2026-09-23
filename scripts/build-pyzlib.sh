#!/usr/bin/env bash
# =============================================================================
# Build a private `zlib` CPython extension module for the Force's bundled
# Python (3.8.10, arm-linux-gnueabihf) and drop it into
# build/deps/bin/pylib/zlib.cpython-38-arm-linux-gnueabihf.so, from where
# scripts/build.sh copies it into dist/ForceCrateDigger/bin/pylib/.
#
# WHY THIS EXISTS: the device's Python (/usr/bin/python3, symlinked to
# AddOns/Python/python3.8/bin/python3.8) has NO `zlib` module at all — it was
# never built (confirmed: `import zlib` -> ModuleNotFoundError on real
# hardware). yt-dlp hard-requires it and refuses to run at all without it
# ("yt-dlp is unavailable"), which silently broke every SEARCH and DOWNLOAD
# in force-cratedigger. This was NOT a missing shared library on the device -
# /usr/lib/libz.so.1.3.1 is already present and working (other things on the
# device apparently link it) - it was specifically the CPython glue module
# (Modules/zlibmodule.c, built into a `zlib.cpython-38-*.so` and normally
# shipped in Python's own lib-dynload/) that was missing from this Python
# build. So the fix is exactly one small .so, not a from-source zlib build.
#
# APPROACH: rather than either (a) modifying the device's shared system
# Python (risky - other addons/nodeServer itself use it, and it wouldn't
# survive a MockbaMod Python update) or (b) requiring a full armhf Python
# build, this compiles just that one extension module against:
#   - Python 3.8.10's actual Modules/zlibmodule.c + its argument-clinic
#     header (fetched from the official CPython git tag - both are small,
#     stable, PSF-licensed C files, not the entire interpreter).
#   - zlib 1.3.1's public zlib.h/zconf.h (fetched from zlib.net - only used
#     at compile time for declarations; nothing from zlib's own C source is
#     compiled in).
#   - src/pyzlib/pyconfig.h: VENDORED, not fetched, because it's this exact
#     device Python build's actual config (pymalloc on, debug off, etc) -
#     copied once from a live device
#     (/media/662522/AddOns/Python/python3.8/include/python3.8/pyconfig.h)
#     since there's no public source for it; the rest of Python's own public
#     C-API headers (Python.h and friends) ARE fetched fresh below, since
#     those don't vary by device build.
# and statically links a real build of zlib 1.3.1's own C source into this
# module - self-contained, no runtime dependency on the device's own
# /usr/lib/libz.so.1 at all (that library's presence was useful evidence
# that zlib support was *reachable* on this device, but is not relied on
# here). An earlier version of this script tried linking against a tiny
# hand-written stub .so purely to satisfy the build-time linker (assuming
# the real work happened via zig's own static compiler-rt embedding, and
# that the device's real libz.so.1 would resolve everything else at
# runtime) - that produced a module that failed to import on-device with
# `undefined symbol: __aeabi_uidiv`. Root cause not fully chased down (zig's
# compiler-rt embedding for -shared outputs appears to depend on what's
# actually being linked against, not just present unconditionally per
# translation unit), but linking a real, complete zlib build sidesteps the
# question entirely and was verified working live - see below.
#
# DEPLOYMENT: this module is NOT installed into the device's shared
# lib-dynload/ (see (a) above). Instead it's bundled inside this addon's own
# dist/ForceCrateDigger/bin/pylib/ and made visible to python3 only for the
# yt-dlp daemon child process, via a PYTHONPATH set in cratedigger_host.cpp's
# main() before it spawns that child (see the comment there) - fully
# self-contained, no shared device state touched, uninstalls cleanly with the
# rest of this addon.
#
# VERIFIED LIVE (2026-09-23): with this module on PYTHONPATH, `import zlib`
# works, and yt_dlp_daemon.py's SEARCH command returns real results for the
# `yt`, `archive`, and `sc` (SoundCloud) providers - all three returned
# nothing but "yt-dlp is unavailable" before. DOWNLOAD has separate,
# provider-specific issues unrelated to zlib (see README's Known
# limitations) - this script only fixes the "yt-dlp can't even start"
# blocker.
#
# Requires zig (see scripts/build.sh's own header for how to get it) - reused
# here purely as a portable C cross-compiler, same as the rest of this
# project's build tooling. Needs zig >= 0.14.0 (see force-audioin's own
# build.sh for why 0.13.x is unsafe on this ARM target - not hit here since
# this module does no variadic-double printf, but keep the toolchain
# consistent project-wide regardless).
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG="${ZIG:-zig}"
TARGET=arm-linux-gnueabihf.2.39   # matches the Force's exact glibc (see force-audioin's build.sh)
CPYTHON_TAG=v3.8.10
ZLIB_VERSION=1.3.1

if ! command -v "$ZIG" >/dev/null 2>&1; then
    echo "zig not found (set ZIG=/path/to/zig, or put it on PATH)." >&2
    exit 1
fi

WORK_DIR="$PWD/build/deps/work/pyzlib"
OUT_DIR="$PWD/build/deps/bin/pylib"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR" "$OUT_DIR"
cd "$WORK_DIR"

echo "=== Fetching CPython ${CPYTHON_TAG}'s zlibmodule.c + clinic header ==="
curl -fsSL -o zlibmodule.c \
    "https://raw.githubusercontent.com/python/cpython/${CPYTHON_TAG}/Modules/zlibmodule.c"
mkdir -p clinic
curl -fsSL -o clinic/zlibmodule.c.h \
    "https://raw.githubusercontent.com/python/cpython/${CPYTHON_TAG}/Modules/clinic/zlibmodule.c.h"

echo "=== Fetching CPython ${CPYTHON_TAG}'s public C-API headers (Include/) ==="
mkdir -p pyinc
curl -fsSL -o cpython.tar.gz \
    "https://github.com/python/cpython/archive/refs/tags/${CPYTHON_TAG}.tar.gz"
tar -xzf cpython.tar.gz -C . "cpython-${CPYTHON_TAG#v}/Include"
cp -r "cpython-${CPYTHON_TAG#v}/Include/." pyinc/
# pyconfig.h is device-build-specific (not part of the public Include/ tree,
# normally generated by ./configure) - vendored, see this script's header.
cp "$PWD/../../../../src/pyzlib/pyconfig.h" pyinc/pyconfig.h

echo "=== Fetching zlib ${ZLIB_VERSION} source (built statically, see header) ==="
curl -fsSL -o zlib.tar.gz \
    "https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz"
tar -xzf zlib.tar.gz "zlib-${ZLIB_VERSION}"
ZSRC="zlib-${ZLIB_VERSION}"

echo "=== Building a static libz.a for arm-linux-gnueabihf ==="
ZLIB_SOURCES="adler32 compress crc32 deflate gzclose gzlib gzread gzwrite infback inffast inflate inftrees trees uncompr zutil"
mkdir -p zobj
for f in $ZLIB_SOURCES; do
    "$ZIG" cc -target "$TARGET" -O2 -fPIC -D_GNU_SOURCE -I "$ZSRC" -c "$ZSRC/$f.c" -o "zobj/$f.o"
done
"$ZIG" ar rcs libz.a zobj/*.o

echo "=== Compiling zlib.cpython-38-arm-linux-gnueabihf.so (static libz linked in) ==="
"$ZIG" cc -target "$TARGET" -O2 -fPIC -shared \
    -I pyinc -I "$ZSRC" \
    -o "$OUT_DIR/zlib.cpython-38-arm-linux-gnueabihf.so" \
    zlibmodule.c libz.a \
    -Wl,-soname,zlib.cpython-38-arm-linux-gnueabihf.so -Wl,-s

echo "-- built module --"
readelf -d "$OUT_DIR/zlib.cpython-38-arm-linux-gnueabihf.so" | grep NEEDED || echo "(no extra NEEDED entries - fully self-contained, as intended)"
file "$OUT_DIR/zlib.cpython-38-arm-linux-gnueabihf.so"
ls -la "$OUT_DIR"
