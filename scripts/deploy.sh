#!/usr/bin/env bash
# Deploy dist/ForceCrateDigger/ to a live MockbaMod Force over scp.
# Usage: scripts/deploy.sh root@<force-ip>
#
# `scp -r` into an existing destination NESTS rather than merges (hit live
# on force-maze — see ~/.claude/skills/mockbamod-module-creator/
# references/porting-schwung-modules.md's "Deploy gotcha") — so this
# removes the remote addon dir first, then copies fresh.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST="${1:?usage: scripts/deploy.sh user@force-ip}"

if [ ! -d dist/ForceCrateDigger ]; then
  echo "dist/ForceCrateDigger not found — run scripts/build.sh first." >&2
  exit 1
fi

mmPath="$(ssh "$HOST" 'cat /dev/shm/.mmPath')"
echo "== remote mmPath: $mmPath =="

ssh "$HOST" "rm -rf '$mmPath/AddOns/ForceCrateDigger'"
scp -r dist/ForceCrateDigger "$HOST:$mmPath/AddOns/ForceCrateDigger"

echo "== deployed. On the device: =="
echo "  \"$mmPath/AddOns/ForceCrateDigger/manage.sh\" ENABLE"
echo "  \"$mmPath/AddOns/ForceCrateDigger/web/manage.sh\" ENABLE"
echo "Then start the engine itself from the nodeServer Modules page (/moduler)."
