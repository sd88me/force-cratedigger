#!/bin/sh
# ForceWebstream AddOn Manager (MockbaMod convention).
#   sh manage.sh ENABLE | DISABLE | UNINSTALL
#
# Ported DSP core from Charles Vestal's schwung-webstream
# (https://github.com/charlesvestal/schwung-webstream, MIT) for Ableton
# Move. webstream_host renders it via its native v2 plugin API and writes
# audio into a shared-memory ring that the separate ForceAudioIn addon's
# forceAudioIn.so (LD_PRELOAD'd into MPC) mixes into what MPC reads from
# its capture device.
#
# This addon does NOT touch LD_PRELOAD or restart acvs - ForceAudioIn owns
# arming the shared tap exclusively (enable it separately, once; see its
# own README.md). webstream_host itself is started/stopped entirely from
# the nodeServer Modules page (/moduler) - never from this script, and
# never at boot (see NSMODULE.json - AUTOLAUNCHABLE is deliberately false).
# ENABLE/DISABLE here only control whether the addon's files are present.

appname=webstream_host
appTitle="Force Webstream"
appDir=ForceWebstream

mmPath=$(cat /dev/shm/.mmPath)
. $mmPath/MockbaMod/env.sh

runDir="$mmPath/AddOns"
installroot="$runDir/$appDir"
mode=$1

echo "
***********************************************************
*   $appTitle AddOn Manager for MockbaMod
***********************************************************
"

STOP() {
    for p in $(ps 2>/dev/null | grep "[w]ebstream_host" | awk '{print $1}'); do
        kill -9 $p 2>/dev/null
    done
}

if [ "$mode" = "UNINSTALL" ]; then
    STOP
    rm -rf "$installroot" 2>/dev/null
    echo "<<<< $appTitle uninstalled."
    exit 0
fi

if [ "$mode" = "DISABLE" ]; then
    STOP
    echo "$appTitle's webstream_host stopped (files kept - this addon has no boot-time footprint to remove)."
    exit 0
fi

if [ "$mode" = "ENABLE" ]; then
    echo "$appTitle enabled. Make sure the separate ForceAudioIn addon is"
    echo "also enabled (its own manage.sh ENABLE) - it arms the shared tap"
    echo "this addon needs. Start the engine itself from the nodeServer"
    echo "Modules page (/moduler), not from here."
    echo
    echo "First-time setup: run scripts/build-deps.sh (on your dev machine)"
    echo "and deploy its output into this addon's bin/ before enabling - see"
    echo "README.md. Without bin/yt-dlp and bin/ffmpeg present, search and"
    echo "playback will fail with a clear error in the web GUI."
    exit 0
fi

echo "Usage: sh manage.sh ENABLE | DISABLE | UNINSTALL"
echo
echo "Status:"
ps 2>/dev/null | grep -q "[w]ebstream_host" && echo "  engine: RUNNING" || echo "  engine: stopped"
echo "  start/stop from the nodeServer Modules page (/moduler) - requires"
echo "  the separate ForceAudioIn addon to be enabled first."
echo "  web GUI is a separate addon - see web/manage.sh (survives this being disabled)"
echo "  logs: /tmp/forceAudioIn.log (mix tap), /tmp/webstream_host.log (engine)"
echo "  control socket: /tmp/webstream_ctrl.sock"
