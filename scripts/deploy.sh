#!/usr/bin/env bash
# Deploy + enable Force Crate Digger on a live MockbaMod Force in one command.
# Usage: scripts/deploy.sh root@<force-ip>
#
# `scp -r` into an existing destination NESTS rather than merges (hit live
# on force-maze — see ~/.claude/skills/mockbamod-module-creator/
# references/porting-schwung-modules.md's "Deploy gotcha") — so this
# removes the remote addon dir first, then copies fresh.
#
# addon/NSMODULE.json and addon/shadow_page.conf ship with the literal
# sentinel path `/media/CHANGE_ME` in place of the real, per-device mmPath
# (MockbaMod addon paths vary by SD card, so a build step can't know it in
# advance). Both files use that same sentinel string, including inside
# shadow_page.conf's engine_arguments_json, which force-shadow's engine
# on/off button re-sends verbatim to nodeServer — so the two occurrences
# must stay byte-for-byte identical, and hand-editing both was the risk
# this script now removes: one sed pass, from the mmPath this script
# already looked up, patches every occurrence in both deployed files at
# once.
set -euo pipefail
cd "$(dirname "$0")/.."

APP_DIR="ForceCrateDigger"
HAS_WEB=1

HOST="${1:?usage: scripts/deploy.sh user@force-ip}"

if [ ! -d "dist/$APP_DIR" ]; then
  echo "dist/$APP_DIR not found - run scripts/build.sh first." >&2
  exit 1
fi

mmPath="$(ssh "$HOST" 'cat /dev/shm/.mmPath')"
echo "== remote mmPath: $mmPath =="

installroot="$mmPath/AddOns/$APP_DIR"
ssh "$HOST" "rm -rf '$installroot'"
scp -r "dist/$APP_DIR" "$HOST:$installroot"

echo "== patching per-device paths (/media/CHANGE_ME -> $mmPath) =="
ssh "$HOST" "sed -i \"s|/media/CHANGE_ME|$mmPath|g\" '$installroot/NSMODULE.json' '$installroot/shadow_page.conf'"

remaining="$(ssh "$HOST" "grep -l CHANGE_ME '$installroot/NSMODULE.json' '$installroot/shadow_page.conf' 2>/dev/null" || true)"
if [ -n "$remaining" ]; then
  echo "ERROR: CHANGE_ME still present after patching, deploy is broken:" >&2
  echo "$remaining" >&2
  exit 1
fi

ssh "$HOST" "'$installroot/manage.sh' ENABLE"
if [ "$HAS_WEB" = 1 ]; then
  ssh "$HOST" "'$installroot/web/manage.sh' ENABLE"
fi

cat <<EOF
== $APP_DIR deployed and enabled. ==

Two things this script does NOT do for you:
  1. Enable the separate ForceAudioJack addon (its own manage.sh ENABLE) -
     it arms the shared audio tap this addon writes into. Hard dependency
     for audio output.
  2. Start cratedigger_host itself - it does not auto-launch at boot on
     purpose. Start it from the nodeServer Modules page (/moduler) or the
     shadow page's own engine button.

Web GUI: http://${HOST#*@}:8308/
Discogs token (optional but recommended): see README.md.
EOF
