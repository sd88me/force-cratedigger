#!/usr/bin/env bash
# =============================================================================
# Fetch the runtime dependencies force-webstream needs beyond its own
# compiled binary: yt-dlp and ffmpeg/ffprobe. Adapted from upstream
# schwung-webstream's scripts/build-deps.sh for the Force's actual
# architecture (armv7l/armhf — 32-bit, unlike Move's aarch64), which
# changes where ffmpeg comes from and drops deno entirely:
#
#   - yt-dlp: unchanged from upstream. Built as a Python zipimport
#     executable (`make lazy-extractors yt-dlp`), which is architecture-
#     independent — it runs under any Python 3.8+, arm or not.
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
require_cmd git
require_cmd make
require_cmd python3

echo "=== Building yt-dlp (zipimport binary with lazy extractors) ==="
YTDLP_DIR="$WORK_DIR/yt-dlp-src"
if [ ! -d "$YTDLP_DIR/.git" ]; then
  git clone --depth 1 https://github.com/yt-dlp/yt-dlp.git "$YTDLP_DIR"
fi
(
  cd "$YTDLP_DIR"
  git pull --ff-only || true
  make clean >/dev/null 2>&1 || true
  make lazy-extractors yt-dlp
  cp yt-dlp "$OUT_DIR/yt-dlp"
)
chmod +x "$OUT_DIR/yt-dlp"

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
YTDLP_COMMIT="$(cd "$YTDLP_DIR" && git rev-parse HEAD 2>/dev/null || true)"
python3 - "$OUT_DIR" "$MANIFEST_PATH" "$YTDLP_COMMIT" "$FFMPEG_URL" <<'PY'
import hashlib, json, os, sys
from datetime import datetime, timezone

out_dir, manifest_path, ytdlp_commit, ffmpeg_url = sys.argv[1:]

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
            "source_ref": ytdlp_commit,
            "license": "Unlicense",
            "note": "architecture-independent (Python zipimport executable)",
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
