# force-webstream

A [MockbaMod](http://mockbatheb.org/) AddOn for the Akai Force: search and
stream web audio (YouTube, SoundCloud, archive.org, Freesound, and
Discogs-powered "crate dig" discovery) as a real audio-generator voice,
with a browser web GUI and an on-device touchscreen ("shadow GUI") page.

This is a **port** of Charles Vestal's
**[schwung-webstream](https://github.com/charlesvestal/schwung-webstream)**
(MIT licensed), originally built for Ableton Move. All credit for the DSP
core, the provider search/resolve/streaming pipeline, and the whole
yt-dlp/ffmpeg-backed design goes to that project — see
[Credit & what's actually new here](#credit--whats-actually-new-here).

## What you get

- **Engine** (`webstream_host`): a native armhf process that links the
  ported DSP core directly, renders decoded audio in real time, and mixes
  it into the Force's own audio output via the
  [force-audioin](https://github.com/sd88me/force-audioin) shared-memory
  tap (the same mechanism [force-maze](../force-maze)'s Maze Voice port
  uses).
- **Web GUI** (`addon/web/`): a browser page for searching providers,
  browsing results, playing, transport control (play/pause, ±15s seek,
  stop, restart), gain, and downloading the current stream to a WAV file.
- **Shadow GUI** (`addon/shadow_page.conf`): a page for the
  [force-shadow](../force-shadow) on-screen overlay — `SHIFT+SCENE-6`
  opens it directly on the Force's own touchscreen, with transport
  controls, output gain/routing, an engine on/off button, and a paged
  list of the current search results you can tap to play.

## How it works (for anyone extending this)

Same "port the host, not the DSP" pattern as this device's other Schwung
ports (force-maze, force-acid) — see
`~/.claude/skills/mockbamod-module-creator/references/porting-schwung-modules.md`
if you have that skill installed, or just read `src/webstream_host.cpp`'s
own header comment. In short:

- `src/dsp/yt_stream_plugin.c` is vendored **near-verbatim** from
  upstream — only two Move-specific absolute paths changed (see the file's
  own top-of-file comment). Every provider, the 60-second ring buffer, the
  yt-dlp daemon protocol, and the entire `set_param`/`get_param` control
  surface are unmodified.
- `src/webstream_host.cpp` is new: it links that DSP core directly (no
  `dlopen`), drives it from a wall-clock timer thread (the DSP core has no
  fixed block-size assumption — see that file's comments), and exposes a
  Unix control socket (`SET`/`GET`/`DESCRIBE`) that both the web GUI and
  the shadow GUI talk to. Unlike Maze Voice's host, this one has **no
  MIDI at all** — this module isn't note-driven (`module.json` declares
  `midi_in`/`midi_out` both false), so there's no RtMidi/ALSA dependency
  to link.
- Two extra control-socket keys exist only in this shim, not in the
  upstream DSP core: `search_results_json` (aggregates the core's
  per-index `search_result_*_<n>` keys into one JSON array, the shape
  Force Shadow's `list` widget needs) and `play_result_index` (composes
  the core's own `stream_provider`+`stream_url` SETs from one result
  index, so neither UI has to know that two-step protocol). See
  `webstream_host.cpp`'s control-socket header comment for the exact
  contract.

## Build

Requires Docker with armhf emulation (see
`force-acid/scripts/build.sh`'s own comment for the one-time
`qemu-user-static` setup on a bare dockerd).

```sh
./scripts/build-deps.sh   # fetch yt-dlp + ffmpeg/ffprobe (see Known Limitations re: deno)
./scripts/build.sh        # compile webstream_host, assemble dist/ForceWebstream/
```

`dist/ForceWebstream/` is the full addon folder, ready to deploy.

## Deploy

```sh
./scripts/deploy.sh root@<force-ip>
```

Or by hand: `scp -r dist/ForceWebstream root@<force-ip>:<mmPath>/AddOns/ForceWebstream`
(get `mmPath` from the device with `cat /dev/shm/.mmPath` over SSH).

## Required per-device edits before enabling

Two files hardcode an absolute path to this addon's own install location —
`/media/CHANGE_ME/AddOns/ForceWebstream` — because MockbaMod addon paths
are genuinely per-device (the SD card's mount point varies), not something
a build step can know in advance. This is the same situation
force-maze/ForceMazeVoice's own `NSMODULE.json` is in. **Before enabling,
edit both to your real path** (SSH in, `cat /dev/shm/.mmPath`, then
`<that>/AddOns/ForceWebstream`):

- `addon/NSMODULE.json` → the `ARGUMENTS` entry with `NAME` containing
  "EDIT to your device's real mmPath"
- `addon/shadow_page.conf` → `engine_nsmodule_path` and the matching
  entry inside `engine_arguments_json` (these two **must stay byte-for-
  byte identical** — Force Shadow's engine on/off button re-sends
  `engine_arguments_json` verbatim to nodeServer, so a stale copy would
  silently corrupt the real `NSMODULE.json` on next toggle)

## Enable

```sh
ssh root@<force-ip> '"<mmPath>/AddOns/ForceWebstream/manage.sh" ENABLE'
ssh root@<force-ip> '"<mmPath>/AddOns/ForceWebstream/web/manage.sh" ENABLE'
```

Then, separately:

1. Make sure [ForceAudioIn](https://github.com/sd88me/force-audioin) is
   enabled (its own `manage.sh ENABLE`) — it arms the shared audio tap
   this addon writes into. It is a hard dependency for audio output.
2. Start `webstream_host` itself from the nodeServer Modules page
   (`http://<force-ip>:8080/moduler`) or via `SHIFT+SCENE-6`'s engine
   button on the shadow page — it does **not** auto-launch at boot (see
   `addon/NSMODULE.json`'s `AUTOLAUNCHABLE: false` and the comment in
   `addon/manage.sh`).
3. Browse to `http://<force-ip>:8305/` for the web GUI (always running
   once its own addon is enabled, independent of whether the engine is
   started — every control there just answers "engine not running" until
   `webstream_host`'s socket exists).

## Provider configuration (tokens, etc.)

Same runtime config file upstream uses. Its default path,
`/data/UserData/schwung/config/webstream_providers.json`, is a leftover
from upstream's Move-specific layout — it won't exist on a Force, but
that's harmless: the daemon (`load_provider_config()` in
`src/bin/yt_dlp_daemon.py`) just falls back to no tokens configured, and
YouTube/SoundCloud/archive.org all work fine without one. If you want
Freesound or Discogs tokens, simplest is to just create that exact path
by hand on the device (`mkdir -p /data/UserData/schwung/config`, then
write the JSON below). The daemon also honors a `WEBSTREAM_PROVIDER_CONFIG`
env var for a different path — but note `webstream_host` is normally
launched by nodeServer's Modules page, which spawns it directly (no
shell, so no env var injection via `NSMODULE.json`'s `ARGUMENTS` — see
`~/.claude/skills/mockbamod-module-creator/references/web-gui.md`'s
"Modules page" section on that spawn behavior). If you need a non-default
path, point `PROCESSNAME`/`FILENAME` at a tiny wrapper shell script that
exports the var and then `exec`s the real binary, instead of the binary
directly.

```json
{
  "providers": {
    "freesound": { "enabled": true, "api_key": "YOUR_FREESOUND_TOKEN" },
    "cratedig": { "token": "YOUR_DISCOGS_PERSONAL_TOKEN" }
  }
}
```

See upstream's own README for the full provider config shape and how
Discogs crate-dig filtering (genre/style/decade/country) works — the web
GUI's Crate Dig tab exposes exactly those four fields.

## Known limitations (differences from the Move original)

- **No `deno`.** Deno publishes no official armv7/armhf Linux build, only
  x86_64 and aarch64. yt-dlp falls back to its own built-in JS
  interpreter for YouTube signature-cipher extraction — works for most
  videos, but is a weaker fallback than deno for a minority of more
  obfuscated challenges. SoundCloud/archive.org/Freesound/crate-dig don't
  use deno at all and are unaffected.
- **Downloads land in `/tmp/force-webstream-downloads`**, not a real
  sample-library path. The Force has no fixed, serial-independent path
  for its own sample library (it's mounted per-device under
  `/media/<serial>/...`), and guessing wrong seemed worse than being
  explicit — move files from there into your library via the Force's own
  file browser.
- **No on-device text entry for search queries.** The shadow GUI's
  `RESULTS` tab browses and plays whatever the *last search* returned
  (from either UI) and controls transport/output, but typing a new query
  is a web-GUI-only action — wiring the Force's on-screen keyboard
  through a `list`/text-widget flow was out of scope for this port.
- **No Q-Link/MIDI CC control surface.** This port's control surface is
  the web GUI and shadow GUI only, per the original request — no ALSA
  MIDI port is created at all (see [How it works](#how-it-works-for-anyone-extending-this)).
  Nothing stops a future addition of one, following force-maze's
  `PARAMS[]`/CC-table pattern if wanted.
- **The shadow page's layout has not been visually verified** against
  `force-shadow/tools/render_preview.c` or real hardware — it was written
  directly against the documented widget spec
  (`force-shadow/docs/adding-a-page.md`) without a live device to check
  against. Recommend rendering/reviewing it before relying on it, and
  expect to need coordinate tweaks.
- **The render-thread rate-correction constant is inherited, not
  re-measured** for this engine specifically — see
  `src/webstream_host.cpp`'s own comment on `RATE_CORRECTION`.

## Credit & what's actually new here

- **Original design, DSP core, provider backends, and daemon**:
  [Charles Vestal](https://github.com/charlesvestal),
  [schwung-webstream](https://github.com/charlesvestal/schwung-webstream),
  MIT licensed. Third-party notices for yt-dlp/ffmpeg/etc. carried over
  in `UPSTREAM_THIRD_PARTY_NOTICES.md`.
- **New for this port**: the Force host shim (`src/webstream_host.cpp`),
  the audio-injection wiring (`src/forceAudioInject.h`, vendored from
  [force-audioin](https://github.com/sd88me/force-audioin)), the browser
  web GUI (`addon/web/`, a new design — this project has no on-device
  browser UI to port from, only Move's own hardware-screen `ui.js`), the
  shadow GUI page (`addon/shadow_page.conf`), and all
  build/deploy/addon-manager scripts.

Third-party, unsupported community addon. Not affiliated with or endorsed
by Akai, InMusic, Ableton, Charles Vestal, or any streaming provider.
Users are responsible for complying with each provider's own terms of
service and content rights.
