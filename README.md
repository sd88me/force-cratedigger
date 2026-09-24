# mpc-vst-cratedigger

**Crate Digger** — a native MPC OS VST2 instrument plugin for Akai MPC
standalone devices (Force, MPC Live/Live II, One, X, Key 61): dig for
records by genre, style, decade, region and country on Discogs, then
stream and play them into an MPC track, with its own MPC screen skin and
Q-Links.

Loaded by MPC's own built-in plugin host — no companion app, no browser,
no separate GUI process. Add it to a track like any other instrument
plugin and the filter/search/transport controls live right on the MPC
screen.

## Screenshots

| PLAY | FILTERS |
|---|---|
| ![PLAY tab](docs/previews/shadow_play.png) | ![FILTERS tab](docs/previews/shadow_filters.png) |

(Previews are of the original Force Shadow page this plugin's skin was
ported from — see [Project history](#project-history) — not the MPC
plugin skin itself.)

## What it is

- **Engine**: the same [schwung-webstream](https://github.com/charlesvestal/schwung-webstream)
  DSP core (Charles Vestal, MIT licensed) that resolves and streams
  Discogs results via yt-dlp/ffmpeg, linked directly into the plugin
  (`vst/cratedigger_vst.cpp`) and rendered on a worker thread into a ring
  buffer — MPC's audio callback only ever copies from the ring, since the
  core takes locks, spawns ffmpeg, and reads pipes, none of which may
  happen on MPC's real-time audio thread.
- **Filters**: Genre, Style (depends on Genre), Decade, Region, Country
  (depends on Region) — five steppers, a SEARCH button, and a status
  readout, all as VST parameters MPC's Q-Links can reach.
- **Transport**: Play/Pause, Stop, ±15s seek, a gain knob, and NOW
  PLAYING / STATUS / TIME readouts.
- **Results**: an 8-row (2×4), paged results list on the PLAY tab —
  tapping a row plays it.
- **Skin**: a native MPC screen skin (`TUI.json` + Q-Links) built from
  `vst/layout.conf` via
  [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)'
  `shadow_skin.py`, in the same MPC60-inspired theme as the addon this
  plugin was converted from.

## How it works

- `src/dsp/yt_stream_plugin.c` is the vendored schwung-webstream DSP
  core, built here with `-DYT_POSIX_SPAWN`: inside MPC there's no
  `fork()`, so the daemon/ffmpeg/download paths use `posix_spawn`
  instead (fds 3+ closed, `LD_PRELOAD` stripped). This addon forces
  `search_provider` to `cratedig` — every other schwung-webstream
  provider is still present in the core, just not exposed by this
  plugin's own parameters.
- `vst/cratedigger_vst.cpp` is a hand-written VST2 ABI shim (no
  Steinberg SDK) around that core: filter/transport/search become VST
  parameters, MPC polls their display text for the skin's readouts and
  result-row labels, and the plugin refuses to load rather than half-work
  if it can't find its engine bundle (`bin/yt-dlp`, `bin/python3`,
  `bin/ffmpeg` — see `find_module_dir()`).
- `vst/params.json` is the parameter table (order is the VST index —
  append-only); `vst/gen_params.py` turns it into `params_gen.h`.
  `vst/layout.conf` is the skin layout; `vst/gen_skin.py` turns it +
  `params.json` into the shipped `Plugin Skins/` folder.
- No MIDI: `effProcessEvents` is a no-op, matching the original
  addon (`module.json` declared `midi_in`/`midi_out` both false) — this
  isn't a note-driven instrument, it's a browser/player exposed as one so
  MPC's plugin host can give it a track and a screen.

## Requirements

- A first-generation MPC OS standalone device (32-bit ARM: Force, MPC
  Live / Live II, One, X, Key 61). The installer refuses anything else;
  newer models are untested.
- **Root shell access (SSH)** to the device. Stock MPC OS doesn't offer
  this — you need a modded unit.
- A network connection on the device (Discogs search + streaming).
- Installing plugins this way is unofficial. Back up first; use at your
  own risk.

## Build

Requires Docker with armhf emulation (`arm32v7/gcc:12`, `--platform
linux/arm/v7`):

```sh
./scripts/build-deps.sh    # fetch yt-dlp + ffmpeg/ffprobe
./scripts/build-pyzlib.sh  # build the private zlib module (needs zig on PATH)
./scripts/build-python.sh  # fetch the private Python 3.11 yt-dlp runs under
./vst/build.sh             # compile vst/build/cratedigger.so
python3 vst/gen_skin.py    # build the MPC skin into vst/build/skin/
```

`vst/build.sh` also prints the plugin's exported symbols, needed shared
libs, and highest required glibc version — check those against the
target device before shipping.

## Installation

Unzip a release (or build the payload yourself), copy it to the device,
and run its installer:

```sh
scp -r Crate-Digger-<version> root@<device-ip>:/tmp/
ssh root@<device-ip> sh /tmp/Crate-Digger-<version>/install.sh
```

The installer checks the device architecture, copies the files, **stops
MPC** (save your project first), backs up `MPC.settings`, adds the
plugin to MPC's plugin list (`pluginList-arm`), and restarts MPC. Running
it again upgrades in place; add `-y` to skip the confirmation prompt.
`ssh root@<device-ip> sh /tmp/Crate-Digger-<version>/uninstall.sh` reverses
it.

Then add **Crate Digger** to a track from the plugin browser (Instrument
plugins). Its screen appears in the plugin view, and the Q-Links follow
the page.

### Install by hand

1. Copy the files:
   - `payload/vst/cratedigger.so` → `/sdcard/vst/cratedigger.so`
   - `payload/vst/cratedigger/bin` → `/sdcard/vst/cratedigger/bin`
   - `payload/Synths/sd88me - VST - Crate Digger/` →
     `/sdcard/Synths/sd88me - VST - Crate Digger/`
2. Stop MPC: `systemctl stop acvs`.
3. Back up `MPC.settings` (on a Force:
   `/media/az01-internal/Settings/MPC/MPC.settings`).
4. Inside `<VALUE name="pluginList-arm"><KNOWNPLUGINS>`, add the
   `<PLUGIN .../>` line from `plugin.xml` (create the `pluginList-arm`
   value, just before `</PROPERTIES>`, if there isn't one yet).
5. Start MPC: `systemctl start acvs`. If MPC comes up with default
   settings, restore your backup — the XML was malformed.

## Discogs token (optional but recommended)

Crate Dig works without a token (25 requests/min per device, Discogs'
public rate limit). For 60/min, generate a free personal access token at
[discogs.com](https://www.discogs.com/settings/developers) and put it in
`/data/UserData/schwung/config/webstream_providers.json` on the device:

```json
{ "providers": { "cratedig": { "token": "YOUR_DISCOGS_PERSONAL_TOKEN" } } }
```

## CPU

Measured on a Gen1 device (Cortex-A17) with `tools/bench.sh` from
[mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins): worst p99
0.8% of one audio block, worst block 4.9% — comfortably under the
~15% p99 rule of thumb for running several plugin instances at once.

## Known limitations

- No MIDI/note control — this is a browser/player, not a synth voice.
- Playback depends on yt-dlp/ffmpeg resolving Discogs' linked sources
  (mostly YouTube) live on the device; those resolution paths do break
  upstream from time to time (see git history for past yt-dlp/Python/zlib
  fixes carried over from the addon version).
- Tested on a Force; other Gen1 MPC OS devices are untested.

## Project history

This plugin started as **Force Crate Digger**, a
[MockbaMod](http://mockbatheb.org/) AddOn for the Akai Force with a
browser web GUI and a [Force Shadow](https://github.com/sd88me/force-shadow)
touchscreen page. That version proved out the DSP-core port and the
whole filter/search/transport design, then was **converted into this
native MPC plugin** — same core, same theme, no more separate GUI
process or addon manager. The addon version is no longer under active
development; its code, build and deploy tooling stay in this repo
(`addon/`, `src/`, `docs/`) for reference — see
[`addon/README.md`](addon/README.md) for its own build/install
instructions and porting notes.

## Credit

- **Original design, DSP core, provider backends, and daemon**:
  [Charles Vestal](https://github.com/charlesvestal),
  [schwung-webstream](https://github.com/charlesvestal/schwung-webstream),
  MIT licensed — including the genre/style/decade/region/country tables
  this plugin's filter pickers use. Third-party notices for
  yt-dlp/ffmpeg/etc. carried over in `UPSTREAM_THIRD_PARTY_NOTICES.md`.
- **New for this project**: the Force/MPC host shims
  (`src/cratedigger_host.cpp`, `vst/cratedigger_vst.cpp`), the VST
  parameter table and skin (`vst/`), the original addon's web GUI and
  shadow GUI (`addon/`), and all build/deploy tooling. Built with
  [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins).

## Releases

See [Releases](https://github.com/sd88me/mpc-vst-cratedigger/releases).
`v0.2.0` was the last release of the Force-addon version; VST plugin
releases start their own version numbering.

Third-party, unsupported project. Not affiliated with or endorsed by
Akai, InMusic, Ableton, Charles Vestal, or Discogs. Users are responsible
for complying with Discogs' own terms of service and content rights.
