#!/bin/sh
############################################################
# Copy this file to $mmPath/AddOns to launch automatically
# at boot (manage.sh ENABLE does that for you).
############################################################
#
# Runs the Force Crate Digger browser control panel (server.py). Independent
# of the engine's own addon/manage.sh/run_cratedigger_host.sh - that one
# stays off until started from the nodeServer Modules page, but the
# *panel* can be up and reachable at all times, same as force-maze's
# web/run_maze_web.sh (this is a direct copy of that pattern) - every
# control just answers "engine not running" (503) until cratedigger_host's
# socket exists.
#
# PID-file based kill, not `killall python3` or a `pgrep -f` name match:
# this device runs other python3 processes (nodeServer's tooling, other
# addons' own web panels), and a name-based kill would take those down too.

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

APPDIR="$mmPath/AddOns/ForceCrateDigger/web"
PIDFILE="$APPDIR/.cratedigger_web.pid"

if [ "$1" = "kill" ]; then
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null
        rm -f "$PIDFILE"
    fi
else
    cd "$APPDIR" || exit 1
    python3 server.py --port 8308 --ctrl-sock /tmp/cratedigger_ctrl.sock >/tmp/cratedigger_web.log 2>&1 &
    echo $! > "$PIDFILE"
fi
