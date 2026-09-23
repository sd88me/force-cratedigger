#!/bin/sh
############################################################
# ForceCrateDigger — deliberately a no-op (mirrors force-maze's
# ForceMazeVoice/run_maze_host.sh).
#
# This addon never arms any LD_PRELOAD tap or auto-starts cratedigger_host at
# boot - that's ForceAudioJack's job now (see its own README.md), and
# cratedigger_host is only ever started on demand via the nodeServer Modules
# page (/moduler), never at boot. NSMODULE.json also sets
# AUTOLAUNCHABLE:false for this reason.
#
# This file still exists, as a harmless stub, only because nodeServer's
# moduler endpoint names its own optional "Autoload" copy target after
# PROCESSNAME (run_cratedigger_host.sh) - if that toggle is ever set despite
# AUTOLAUNCHABLE:false, having this file present and inert is safer than it
# failing to find a file to copy. It intentionally does nothing at all, on
# every invocation, "kill" included.
############################################################
exit 0
