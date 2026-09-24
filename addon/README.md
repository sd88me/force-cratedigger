# Force Crate Digger (Force Shadow addon) — original version, superseded

This is the **original** version of Crate Digger: a
[MockbaMod](http://mockbatheb.org/) AddOn for the Akai Force, with a
browser web GUI and an on-device "shadow GUI" touchscreen page (rendered
by [Force Shadow](https://github.com/sd88me/force-shadow)). It has been
**converted to a native MPC OS VST2 plugin** — see the
[repo's main README](../README.md) — and is no longer under active
development. This page is kept for anyone still running it, or extending
Force Shadow addons generally.

The DSP core, deploy tooling and this addon's own host shim
(`../src/cratedigger_host.cpp`) are unchanged from when this was the
primary version; nothing here has been removed, only frozen.

## What it is

Random music discovery via Discogs "crate dig": filter by genre, style,
decade, region and country, browse hits, and play them as a real
audio-generator voice, mixed into the Force's own audio output.

- **Engine** (`cratedigger_host`): a native armhf process that links the
  ported DSP core directly and mixes its output into the Force's audio
  via the [force-audio-jack](https://github.com/sd88me/force-audio-jack)
  shared-memory tap.
- **Web GUI** (`web/`): cascading Genre → Style and Region → Country
  dropdowns, a Decade dropdown, Search, a results list, transport
  control, gain, download to WAV, and a Skipback Rec button.
- **Shadow GUI** (`shadow_page.conf`): a touchscreen page themed after a
  vintage Akai MPC60, reachable from the Force's on-screen ADD-ONS
  launcher.

This is a **port and refocus** of Charles Vestal's
[schwung-webstream](https://github.com/charlesvestal/schwung-webstream)
(MIT licensed), originally built for Ableton Move. All credit for the DSP
core, the Discogs search/resolve/streaming pipeline, and the whole
yt-dlp/ffmpeg-backed design goes to that project.

## Screenshots

| PLAY | FILTERS |
|---|---|
| ![PLAY tab](../docs/previews/shadow_play.png) | ![FILTERS tab](../docs/previews/shadow_filters.png) |

## Build

Requires Docker with armhf emulation, and `zig` on `PATH` for
`build-pyzlib.sh`:

```sh
./scripts/build-deps.sh    # fetch yt-dlp + ffmpeg/ffprobe
./scripts/build-pyzlib.sh  # build the private zlib module (fallback)
./scripts/build-python.sh  # fetch the private Python 3.11 yt-dlp runs under
./scripts/build.sh         # compile cratedigger_host, assemble dist/ForceCrateDigger/
```

`dist/ForceCrateDigger/` is the full addon folder, ready to deploy.

## Installation

```sh
scripts/deploy.sh root@<force-ip>
```

Copies `dist/ForceCrateDigger/` onto the device, patches the two files
that hardcode absolute addon-install paths, and enables the engine and
web-GUI addons via `manage.sh ENABLE`.

Not done for you (enable these separately):

1. [ForceAudioJack](https://github.com/sd88me/force-audio-jack) — hard
   dependency for audio output.
2. `ForceAudioJackSkipback`, if you want the Skipback Rec button to work.

Once enabled, browse to `http://<force-ip>:8308/` for the web GUI.

## Discogs token (optional)

Crate Dig works without a token (25 requests/min). For 60/min, generate a
free personal access token at
[discogs.com](https://www.discogs.com/settings/developers) and put it in
`/data/UserData/schwung/config/webstream_providers.json`:

```json
{ "providers": { "cratedig": { "token": "YOUR_DISCOGS_PERSONAL_TOKEN" } } }
```

## Known limitations

See the git history of this file (pre-VST-pivot commits) for the full
list of Move→Force porting notes: the device's Python needed a private
zlib module and a private Python 3.11 for yt-dlp, there's no `deno` on
armhf, Skipback capture depends on the Force's own Audio-In routing being
set up in the current project, and the shadow GUI's filter state isn't
shared with the web GUI's.

## Releases

[`v0.2.0`](https://github.com/sd88me/mpc-vst-cratedigger/releases/tag/v0.2.0)
(2026-09-23) was the last release of this Force-addon version before the
project moved to the native VST plugin.

Third-party, unsupported community addon. Not affiliated with or endorsed
by Akai, InMusic, Ableton, Charles Vestal, or Discogs.
