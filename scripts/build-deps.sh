#!/usr/bin/env bash
# =============================================================================
# Fetch the runtime dependencies force-cratedigger needs beyond its own
# compiled binary: yt-dlp and ffmpeg/ffprobe. Adapted from upstream
# schwung-webstream's scripts/build-deps.sh for the Force's actual
# architecture (armv7l/armhf — 32-bit, unlike Move's aarch64), which
# changes where ffmpeg comes from and drops deno entirely:
#
#   - yt-dlp: unchanged from upstream. Fetched as upstream's own published
#     release zipapp (see the comment at that step for why, and for the
#     real-world bug this caught), which is architecture-independent — it
#     runs under any Python 3.8+, arm or not.
#   - ffmpeg/ffprobe: upstream's source (yt-dlp/FFmpeg-Builds) only
#     publishes linux64/linuxarm64, no 32-bit ARM. johnvansickle.com's
#     well-known static builds do publish an armhf variant — used here
#     instead.
#   - deno: DROPPED. Deno has no official armv7/armhf Linux build (only
#     x86_64 and aarch64). yt-dlp's own JS interpreter is used as the
#     fallback for YouTube signature-cipher extraction instead — this
#     works for most videos but is a known-weaker fallback than deno for
#     the small subset of newer/more obfuscated signature challenges (see
#     README.md's Known Limitations section). soundcloud/archive/
#     freesound/cratedig do not use deno at all.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

OUT_DIR="$REPO_ROOT/build/deps/bin"
WORK_DIR="$REPO_ROOT/build/deps/work"
MANIFEST_PATH="$REPO_ROOT/build/deps/manifest.json"

mkdir -p "$OUT_DIR" "$WORK_DIR"

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "Missing required command: $1"; exit 1; }
}
require_cmd curl
require_cmd tar
require_cmd python3

echo "=== Fetching yt-dlp (official release zipapp, latest) ==="
# Was previously built from source (`git clone` + `make lazy-extractors
# yt-dlp`) - switched to fetching upstream's own published release asset
# instead, for two reasons found live on 2026-09-23: (1) that build needs
# a `zip` binary on the build host, which isn't always installed (this
# host didn't have one, blocking a rebuild); (2) a stale bundled yt-dlp is
# a real, silent failure mode, not a theoretical one - a build from months
# ago (2024.10.22) was still deployed on a live device and caused every
# YouTube-backed search result to fail with "The page needs to be
# reloaded" (a signature-cipher extraction error, not the already-known
# no-`deno` limitation this looked like at first).
#
# UPDATE 2026-09-23: back to "latest", not pinned. The pin above existed
# because yt-dlp 2024.12.13 dropped Python 3.8 support entirely (hard
# `sys.version_info` check, raises ImportError) and this addon's yt-dlp
# used to run under the device's SYSTEM Python (3.8.10). That's no longer
# true: scripts/build-python.sh bundles a private Python 3.11 specifically
# so this addon can track yt-dlp's actual latest release (needed to keep
# up with YouTube's evolving anti-bot JS challenge - see that script's
# header) instead of being frozen in time. Run build-python.sh alongside
# this script; without it, the fetched yt-dlp here will fail to even start
# under the device's own system Python 3.8, same failure mode as before.
curl -fsSL -o "$OUT_DIR/yt-dlp" \
  "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp"
chmod +x "$OUT_DIR/yt-dlp"
YTDLP_VERSION="$(python3 "$OUT_DIR/yt-dlp" --version 2>/dev/null || echo unknown)"
echo "-- fetched yt-dlp $YTDLP_VERSION (latest) --"

echo "=== Downloading ffmpeg/ffprobe (johnvansickle.com armhf static) ==="
FFMPEG_URL="https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-armhf-static.tar.xz"
curl -fL -o "$WORK_DIR/ffmpeg-armhf.tar.xz" "$FFMPEG_URL"
rm -rf "$WORK_DIR/ffmpeg-extract"
mkdir -p "$WORK_DIR/ffmpeg-extract"
tar -xJf "$WORK_DIR/ffmpeg-armhf.tar.xz" -C "$WORK_DIR/ffmpeg-extract"
FF_DIR="$(find "$WORK_DIR/ffmpeg-extract" -maxdepth 1 -type d -name 'ffmpeg-*armhf*' | head -n 1)"
if [ -z "$FF_DIR" ]; then
  echo "Failed to locate extracted ffmpeg directory"
  exit 1
fi
cp "$FF_DIR/ffmpeg" "$OUT_DIR/ffmpeg"
cp "$FF_DIR/ffprobe" "$OUT_DIR/ffprobe"
chmod +x "$OUT_DIR/ffmpeg" "$OUT_DIR/ffprobe"
"$OUT_DIR/ffmpeg" -version | head -n1 || echo "(ffmpeg is armhf — this host can't exec it to verify; that's expected off-device)"

echo "=== Writing dependency manifest ==="
python3 - "$OUT_DIR" "$MANIFEST_PATH" "$YTDLP_VERSION" "$FFMPEG_URL" <<'PY'
import hashlib, json, os, sys
from datetime import datetime, timezone

out_dir, manifest_path, ytdlp_version, ffmpeg_url = sys.argv[1:]

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()

manifest = {
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "target_arch": "armv7l (armhf) — Akai Force",
    "artifacts": {
        "yt-dlp": {
            "source_repo": "https://github.com/yt-dlp/yt-dlp",
            "release_version": ytdlp_version,
            "license": "Unlicense",
            "note": "architecture-independent (Python zipimport executable), "
                     "fetched as upstream's own published release asset",
            "sha256": sha256(os.path.join(out_dir, "yt-dlp")),
        },
        "ffmpeg": {
            "source_url": ffmpeg_url,
            "license": "GPL-3.0-or-later (build-dependent)",
            "sha256": sha256(os.path.join(out_dir, "ffmpeg")),
        },
        "ffprobe": {
            "source_url": ffmpeg_url,
            "license": "GPL-3.0-or-later (build-dependent)",
            "sha256": sha256(os.path.join(out_dir, "ffprobe")),
        },
        "deno": {
            "note": "NOT bundled — no official armv7/armhf Linux build exists. "
                     "yt-dlp falls back to its own JS interpreter.",
        },
    },
}
with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2)
print(f"wrote {manifest_path}")
PY

echo "=== Done: $OUT_DIR ==="
ls -la "$OUT_DIR"
