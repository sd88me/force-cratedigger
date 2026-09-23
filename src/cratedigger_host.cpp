/* cratedigger_host.cpp — Force/MockbaMod runtime host for the ported
 * schwung-webstream DSP core (src/dsp/yt_stream_plugin.c, vendored
 * near-verbatim from https://github.com/charlesvestal/schwung-webstream by
 * Charles Vestal, MIT). Plays the role Move's chain host plays, the same
 * pattern force-maze's maze_host.cpp and force-acid's host_shim.cpp use:
 *
 *   Schwung host                          this shim
 *   -------------------------------------- --------------------------------
 *   dlopen(dsp.so), move_plugin_init_v2    links yt_stream_plugin.o, calls it directly
 *   render_block() per audio block         wall-clock timer thread -> render_block()
 *   set_param(key, "42") from a knob       a local control socket -> set_param()
 *   int16 stereo out via the mailbox       float32 into ForceAudioIn's shared-
 *                                          memory ring (forceAudioInject.h) --
 *                                          forceAudioIn.so (LD_PRELOAD'd into
 *                                          /usr/bin/MPC) mixes it into what MPC
 *                                          reads from its capture device.
 *
 * yt_stream_plugin.c never calls back into a host_api_v1_t (checked: its
 * host pointer is stored but never dereferenced), so none is provided here,
 * same as maze_host. Unlike Maze Voice this module has no MIDI-driven
 * synthesis (module.json declares midi_in/midi_out both false) — it's
 * driven entirely by its own search/playback state machine, controlled
 * over the same control socket the web GUI and shadow GUI both use. So
 * this shim, unlike maze_host, links no RtMidi/ALSA MIDI at all — one
 * dependency fewer, and one less thing that can collide with another
 * addon's port name.
 *
 * THE RENDER CADENCE drives two independent things inside the DSP core:
 * pump_pipe() (drains ffmpeg's stdout pipe into the core's own 60s ring,
 * non-blocking, wants to be called often regardless of frame count) and the
 * actual sample production handed back to us. Calling render_block often,
 * with the real elapsed-frame count each time (not a fixed guess), keeps
 * both happy — same reasoning as maze_host's timer loop, see its own
 * comments for the fuller writeup of why a fixed cadence drifts under
 * scheduler jitter on a shared, non-RT thread.
 *
 * Build: see scripts/build.sh (native armhf under QEMU, links -lpthread
 * -lrt — the shm_open/shm_unlink calls need -lrt on this target's older
 * glibc even though a modern host glibc folds them into libc and links
 * fine without it, confirmed the hard way on the first real build — same
 * toolchain as force-maze/force-acid otherwise, no -lasound needed here).
 */

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "include/plugin_api_v1.h"
#include "forceAudioInject.h"

extern "C" plugin_api_v2_t *move_plugin_init_v2(const void *host);

/* ---------------------------------------------------------------------------
 * Globals
 * ------------------------------------------------------------------------- */
static std::atomic<bool> g_run{true};
static std::mutex        g_lock;        /* serialises every call into the core */
static plugin_api_v2_t  *g_api  = nullptr;
static void             *g_inst = nullptr;

static ai_shm_t *g_shm = nullptr;
static std::atomic<uint64_t> g_ring_drops{0};

static std::string g_module_json;      /* raw module.json, served for DESCRIBE */
static std::string g_ctrl_sock_path = "/tmp/cratedigger_ctrl.sock";
static bool         g_verbose = false;
static unsigned     g_mix_slot = 0;    /* which /forceAudioInjectN this instance owns */

/* Skipback (force-audioin's "Force Audio Jack - Skipback" addon,
 * skipbackHost) is a separate companion process, not something this
 * addon owns or spawns directly — see the SKIPBACK REC button's own
 * comment further down for why toggling it goes through nodeServer's
 * moduler endpoint exactly the way Force Shadow's own engine on/off
 * button does, rather than this process fork/exec-ing it itself. Same
 * per-device mmPath problem every other NSMODULE path in this project
 * has (see README.md) — override with --skipback-nsmodule-path. */
static std::string g_skipback_nsmodule_path =
    "/media/CHANGE_ME/AddOns/ForceAudioJackSkipback/NSMODULE.json";
static const char *SKIPBACK_PROCESSNAME = "skipbackHost";
static const char *SKIPBACK_DIRNAME = "ForceAudioJackSkipback";
/* Verbatim from that addon's own NSMODULE.json ARGUMENTS array — must be
 * kept byte-for-byte in sync with it by hand (same fragility Force
 * Shadow's own engine_arguments_json has, see shadow_page.conf's
 * comment on that) since nodeServer's moduler endpoint overwrites the
 * real NSMODULE.json with whatever ARGUMENTS this sends. */
static const char *SKIPBACK_ARGUMENTS_JSON =
    "[{\"NAME\":\"window flag\",\"VALUE\":\"--window-sec\"},"
    "{\"NAME\":\"rolling window seconds (max 60)\",\"VALUE\":\"30\"},"
    "{\"NAME\":\"output-dir flag\",\"VALUE\":\"--output-dir\"},"
    "{\"NAME\":\"output folder\",\"VALUE\":\"/sdcard/Force Documents/Samples/Skipback\"}]";

/* ---------------------------------------------------------------------------
 * Shared-memory ring setup (producer side) — identical in shape to
 * maze_host's; see forceAudioInject.h for the full ABI contract.
 * ------------------------------------------------------------------------- */
static char g_shm_name[24];

static bool shm_setup() {
    ai_shm_name(g_mix_slot, g_shm_name, sizeof(g_shm_name));
    shm_unlink(g_shm_name);  /* we are the sole producer for this slot -- start clean */
    int fd = shm_open(g_shm_name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return false; }
    if (ftruncate(fd, AI_SHM_BYTES) != 0) { perror("ftruncate"); close(fd); return false; }
    void *m = mmap(nullptr, AI_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { perror("mmap"); return false; }

    g_shm = (ai_shm_t *)m;
    memset(g_shm, 0, AI_SHM_BYTES);
    g_shm->rate = (uint32_t)MOVE_SAMPLE_RATE;
    g_shm->channels = 2;
    g_shm->enabled = 1;
    g_shm->gain = 1.0f;
    g_shm->channel_mask = AI_CHAN_LR;
    __atomic_store_n(&g_shm->magic, AI_MAGIC, __ATOMIC_RELEASE);
    return true;
}

static void ring_push(const float *interleaved, uint32_t frames) {
    if (!g_shm) return;
    uint32_t head = g_shm->head;                                     /* sole producer */
    uint32_t tail = __atomic_load_n(&g_shm->tail, __ATOMIC_ACQUIRE);
    uint32_t space = (AI_RING_FRAMES - 1) - ((head - tail) & (AI_RING_FRAMES - 1));

    uint32_t take = frames;
    if (take > space) {
        take = space;
        g_ring_drops++;
    }
    for (uint32_t i = 0; i < take; i++) {
        uint32_t fr = (head + i) & (AI_RING_FRAMES - 1);
        float *dst = &g_shm->ring[(size_t)fr * AI_MAX_CH];
        dst[0] = interleaved[2 * i];
        dst[1] = interleaved[2 * i + 1];
    }
    __atomic_store_n(&g_shm->head, (head + take) & (AI_RING_FRAMES - 1), __ATOMIC_RELEASE);
    g_shm->frames_written += take;
}

/* ---------------------------------------------------------------------------
 * Timer thread — same elapsed-time-not-fixed-period technique as
 * maze_host's timer_loop(), same empirically-measured RATE_CORRECTION
 * starting point (both hosts feed the same forceAudioIn.so consumer at the
 * same real 44.1kHz capture rate, so the same ~1000ppm drift applies) —
 * re-verify live and retune if a long-running stream shows backlog growth
 * or shrinkage in the periodic stats line below (see maze_host.cpp's own
 * comment for why this is a fixed constant, not an adaptive controller).
 * ------------------------------------------------------------------------- */
static std::atomic<double>   g_max_wake_ms{0.0};
static std::atomic<uint64_t> g_late_wakes{0};
static std::atomic<uint64_t> g_total_wakes{0};
constexpr double RATE_CORRECTION = 44100.0 / (44100.0 - 45.0);   /* ~1.00102 */

static void timer_loop() {
    constexpr int MAX_FRAMES = 4096;   /* generous — webstream's own ring is 60s deep */
    int16_t  pcm[MAX_FRAMES * 2];
    float    flt[MAX_FRAMES * 2];
    const auto period = std::chrono::microseconds(1451);

    using clock = std::chrono::steady_clock;
    auto prev = clock::now();
    auto last_stat = prev;

    while (g_run.load()) {
        std::this_thread::sleep_for(period);
        auto now = clock::now();
        double secs = std::chrono::duration<double>(now - prev).count();
        prev = now;

        double ms = secs * 1000.0;
        double seen_max = g_max_wake_ms.load();
        if (ms > seen_max) g_max_wake_ms.store(ms);
        g_total_wakes++;

        int frames = (int)std::lround(secs * MOVE_SAMPLE_RATE * RATE_CORRECTION);
        if (frames < 1) frames = 1;
        if (frames > 400) g_late_wakes++;
        if (frames > MAX_FRAMES) frames = MAX_FRAMES;

        {
            std::lock_guard<std::mutex> lk(g_lock);
            g_api->render_block(g_inst, pcm, frames);
        }
        for (int i = 0; i < frames * 2; i++) flt[i] = pcm[i] / 32768.0f;
        ring_push(flt, (uint32_t)frames);

        if (now - last_stat >= std::chrono::seconds(5)) {
            last_stat = now;
            uint32_t backlog = g_shm ? (uint32_t)((g_shm->head - __atomic_load_n(&g_shm->tail, __ATOMIC_ACQUIRE))
                                                   & (AI_RING_FRAMES - 1))
                                      : 0;
            fprintf(stderr, "[cratedigger] render thread: max wake gap %.1fms, %llu/%llu wakes > 9ms, ring drops %llu, "
                            "backlog %u frames\n",
                    g_max_wake_ms.load(),
                    (unsigned long long)g_late_wakes.load(), (unsigned long long)g_total_wakes.load(),
                    (unsigned long long)g_ring_drops.load(), backlog);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Control socket — same newline-terminated text protocol every add-on in
 * this family uses (see force-shadow/docs/adding-a-page.md):
 *
 *   SET <key> <value>\n   -> "OK\n" or "ERR\n"
 *   GET <key>\n           -> "<value>\n" or "ERR\n"
 *   DESCRIBE\n            -> this module's module.json, one line (raw JSON,
 *                            since yt_stream_plugin.c has no built-in
 *                            "chain_params" get_param key the way Maze
 *                            Voice's own core does — served from the file
 *                            directly instead)
 *
 * Three key families are intercepted here rather than forwarded to
 * g_api->set_param/get_param, which only knows yt_stream_plugin.c's own
 * keys and would just error on any of these:
 *
 *  - "mix.*" — host-level output-mix controls (on/off, volume, L/R/L+R
 *    routing), same convention as maze_host/force-acid. Lives in the
 *    shared-memory struct forceAudioIn.so reads directly.
 *  - "search_results_json" / "search_results_shadow_json" (GET only) — the
 *    DSP core exposes each search result as a set of separately-indexed
 *    keys (search_result_title_<n>/_channel_<n>/_duration_<n>/
 *    _provider_<n>/_url_<n>, up to search_count), one get_param call per
 *    field per result. Both aggregate that into one JSON array of
 *    {"label": "..."} objects, the shape Force Shadow's `list` widget
 *    expects from a single GET call (see shadow_page.conf and
 *    force-shadow/docs/adding-a-page.md's `list` widget spec) — the
 *    "_shadow_" variant additionally runs every field through
 *    shadow_font_safe() first (uppercase + Force Shadow's actual glyph
 *    set only — see that function's own comment), since Force Shadow's
 *    baked font has no lowercase glyphs and a title/channel with
 *    lowercase or punctuation outside its small supported set renders as
 *    scattered near-blank text otherwise (confirmed by an offline render
 *    before this existed). shadow_page.conf's `list` widget uses the
 *    "_shadow_" key; the web GUI uses the plain one, since a browser has
 *    a real font and shouldn't be limited to it.
 *  - "play_result_index" (SET only) — Force Shadow's `list` widget sends a
 *    plain `SET <key> <index>` on tap; playing a search result actually
 *    needs two of the core's own real keys set together
 *    (stream_provider, then stream_url) from that result's own fields.
 *    This composes those two calls server-side so the shadow page and web
 *    GUI can both just "select result N" without duplicating that
 *    two-step protocol knowledge in two different UIs.
 *  - "cratedig_<dim>_index" (SET, integer index) for dim in genre/style/
 *    decade/region/country, and "cratedig_<dim>_text"/"_idx"/"_count"
 *    (GET) — five `stepper` widgets on the FILTERS tab (no on-device
 *    keyboard, so unlike the web GUI's free-text fields the shadow GUI
 *    can only cycle through fixed lists — CRATEDIG_GENRES/_STYLES/
 *    _DECADES/_REGIONS/_COUNTRIES above, ported verbatim from the
 *    original Move on-device UI's own tables). Style depends on the
 *    current genre selection, country on the current region — selecting
 *    genre/region resets style/country's index to 0, and their _count
 *    changes shape accordingly (see cd_current_styles_locked()/
 *    cd_current_countries_locked()). None of these five SETs touch the
 *    DSP core at all by themselves — only "cratedig_search_go" (the
 *    FILTERS tab's dedicated SEARCH button) composes all five current
 *    selections into the core's real "cratedig_filter" key, which is
 *    itself what triggers the actual Discogs search. This was originally
 *    "search on every filter tap"; a dedicated button was requested
 *    specifically to change that (see send_cratedig_filter_from_shadow_
 *    state_locked()'s own comment).
 *  - "<key>_shadow" (GET, generic suffix) — strips the suffix, fetches the
 *    real value (core or mix.*), and runs it through shadow_font_safe().
 *    Most of the core's own text status values (stream_status:
 *    "streaming"/"paused"/...; search_status: "idle"/"searching"/...) are
 *    lowercase words that would render the same way search results did
 *    before the fix above. playback_time is exempt (digits + ':' only,
 *    already safe) so its readout uses the plain key directly.
 *  - "skipback_toggle" (SET, any value) / "skipback_running" (GET, "1"/
 *    "0") / "skipback_status" (GET, "RECORDING"/"STOPPED" - already
 *    upper-case ASCII, so this one key serves both GUIs, no "_shadow"
 *    variant needed) — start/stop force-audioin's separate skipbackHost
 *    process via nodeServer's /moduler/UPDATE endpoint, the same
 *    mechanism Force Shadow's own engine on/off button uses. See
 *    send_skipback_toggle()'s own comment for why this goes through
 *    nodeServer rather than this process spawning skipbackHost itself.
 *    Playing a result (play_result_index above) also writes the track's
 *    title/channel, and its tempo if the source actually has one (see
 *    write_nowplaying_file()'s own comment — Discogs itself never does,
 *    for what it's worth), to /tmp/force_nowplaying.txt, so a skipback
 *    recording taken while a track is playing gets named after it
 *    instead of the Force project name/tempo (skipbackHost.c change,
 *    force-audioin repo, not this one). Cleared on "stop"/"stop_step"
 *    and on exit.
 * ------------------------------------------------------------------------- */
static bool handle_mix_set(const std::string &key, const std::string &val) {
    if (key == "mix.enabled") {
        g_shm->enabled = (val == "1" || val == "true") ? 1u : 0u;
        return true;
    }
    if (key == "mix.gain") {
        g_shm->gain = std::strtof(val.c_str(), nullptr) / 100.0f;
        return true;
    }
    if (key == "mix.channel_idx") {
        g_shm->channel_mask = (val == "0" || val == "L") ? AI_CHAN_L : (val == "1" || val == "R") ? AI_CHAN_R : AI_CHAN_LR;
        return true;
    }
    if (key == "mix.channel") {
        g_shm->channel_mask = (val == "L") ? AI_CHAN_L : (val == "R") ? AI_CHAN_R : AI_CHAN_LR;
        return true;
    }
    return false;
}
static bool handle_mix_get(const std::string &key, std::string &out) {
    if (key == "mix.enabled") { out = g_shm->enabled ? "1" : "0"; return true; }
    if (key == "mix.gain") {
        char b[32]; std::snprintf(b, sizeof(b), "%.1f", g_shm->gain * 100.0f);
        out = b; return true;
    }
    if (key == "mix.channel_idx") {
        uint32_t m = g_shm->channel_mask;
        out = (m == AI_CHAN_L) ? "0" : (m == AI_CHAN_R) ? "1" : "2";
        return true;
    }
    if (key == "mix.channel") {
        uint32_t m = g_shm->channel_mask;
        out = (m == AI_CHAN_L) ? "L" : (m == AI_CHAN_R) ? "R" : "L+R";
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Skipback toggle — start/stop force-audioin's separate skipbackHost
 * process (continuous rolling-buffer recording of the Force's real main
 * mix, flushed to WAV on a SHIFT+RECORD MidiLoop shortcut — see that
 * project's docs/PROPOSAL-force-audio-jack.md). Not this addon's own
 * process and not something this addon spawns directly — same reasoning
 * Force Shadow's own engine on/off button uses for why (see that
 * button's own comment in force_shadow.c): firing a plain HTTP POST to
 * nodeServer's already-running /moduler/UPDATE endpoint (the same one
 * the on-device Modules page itself uses) is a bounded, ordinary socket
 * call, whereas fork()/exec()-ing a companion process ourselves would be
 * reinventing process-lifecycle bookkeeping nodeServer already owns.
 * Checks the real process list (not an internal flag we could drift out
 * of sync with — e.g. if someone toggles it from the Modules page
 * directly) via `pgrep -x` before each toggle, so this is always acting
 * on the true current state.
 * ------------------------------------------------------------------------- */
static bool skipback_is_running() {
    std::string cmd = std::string("pgrep -x ") + SKIPBACK_PROCESSNAME + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

static void send_skipback_toggle(bool want_running) {
    char body[1024];
    int blen = std::snprintf(body, sizeof(body),
        "{\"CONFIGFILE\":\"%s\",\"PROCESSNAME\":\"%s\",\"DIRNAME\":\"%s\","
        "\"ARGUMENTS\":%s,\"RUNNING\":%s}",
        g_skipback_nsmodule_path.c_str(), SKIPBACK_PROCESSNAME, SKIPBACK_DIRNAME,
        SKIPBACK_ARGUMENTS_JSON, want_running ? "true" : "false");
    if (blen < 0 || (size_t)blen >= sizeof(body)) {
        fprintf(stderr, "[cratedigger] skipback_toggle: JSON body build failed/truncated\n");
        return;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[cratedigger] skipback_toggle: socket"); return; }
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char req[1536];
        int rlen = std::snprintf(req, sizeof(req),
            "POST /moduler/UPDATE HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            blen, body);
        if (rlen > 0 && (size_t)rlen < sizeof(req)) {
            send(fd, req, (size_t)rlen, MSG_NOSIGNAL);
            if (g_verbose) fprintf(stderr, "[cratedigger] skipback_toggle: sent RUNNING=%s\n",
                                    want_running ? "true" : "false");
        }
    } else {
        fprintf(stderr, "[cratedigger] skipback_toggle: connect to nodeServer failed: %s\n", strerror(errno));
    }
    close(fd);
}

/* Must be called with g_lock already held. */
static std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if ((unsigned char)c < 0x20) continue;
        out += c;
    }
    return out;
}

/* Force Shadow's baked font (force-shadow/src/font8x8.h's font_chars)
 * supports exactly: space, A-Z (uppercase only — no lowercase glyphs at
 * all), 0-9, and ". - / > % + :" — nothing else, including no em dash,
 * brackets, parentheses, commas, or apostrophes. Any unsupported byte
 * renders as an invisible space (font_glyph_index() falls back to glyph
 * 0), not a placeholder box — confirmed by rendering this project's own
 * shadow_page.conf RESULTS tab offline (docs/previews/shadow_results.png
 * before this fix): real video/track titles came out as scattered,
 * near-illegible single letters and digits with the rest of each title
 * silently blanked. This maps a string down to that supported set for
 * the shadow GUI specifically; the web GUI (see search_results_json,
 * without this transform) keeps full mixed-case titles since a browser
 * has a real font. */
static std::string shadow_font_safe(const std::string &s) {
    static const std::string allowed = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-/>%+:";
    std::string out;
    out.reserve(s.size());
    bool last_was_space = false;
    for (unsigned char c : s) {
        char u = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : (char)c;
        if (allowed.find(u) == std::string::npos) u = ' ';
        if (u == ' ' && last_was_space) continue;   /* collapse runs left by stripped punctuation */
        out += u;
        last_was_space = (u == ' ');
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t start = out.find_first_not_of(' ');
    return (start == std::string::npos) ? std::string() : out.substr(start);
}

/* ---------------------------------------------------------------------------
 * Crate Dig (Discogs) filter data: genre / style (genre-dependent) /
 * decade / region+country (region narrows country) — for the shadow
 * GUI's FILTERS tab (five `stepper` widgets) and, indirectly, the web
 * GUI (which gets the same lists over a GET endpoint — see server.py).
 *
 * Ported verbatim from the original Move on-device UI's own tables
 * (schwung-webstream's src/ui.js: CRATEDIG_GENRES/CRATEDIG_STYLES/
 * CRATEDIG_DECADES/CRATEDIG_COUNTRIES) — the authoritative real-Discogs-
 * taxonomy source this project already had, not something reinvented
 * here. Every array's index 0 is "" (Discogs field omitted = no filter
 * on that dimension), matching upstream's own "Any" convention.
 *
 * Every value here IS the literal string sent to Discogs (commas,
 * ampersands and all) — there is no separate hand-curated display label
 * per entry (impractical at ~700 style strings total). shadow_font_safe()
 * is called on the value at point of use instead; it already does
 * exactly the uppercase+filter transform needed for Force Shadow's font
 * and is proven correct (see its own comment) — a style string that
 * happens to exceed Force Shadow's `list`/`stepper` on-screen width
 * still filters correctly even if its label is visually truncated.
 * ------------------------------------------------------------------------- */
static const char *CRATEDIG_GENRES[] = {
    "", "Blues", "Brass & Military", "Children's", "Classical", "Electronic",
    "Folk, World, & Country", "Funk / Soul", "Hip Hop", "Jazz", "Latin",
    "Non-Music", "Pop", "Reggae", "Rock", "Stage & Screen",
};
static const int N_CRATEDIG_GENRES = (int)(sizeof(CRATEDIG_GENRES) / sizeof(CRATEDIG_GENRES[0]));

static const char *STYLES_ANY[] = { "" };
static const char *STYLES_BLUES[] = { "", "Boogie Woogie", "Chicago Blues", "Country Blues", "Delta Blues", "East Coast Blues", "Electric Blues", "Harmonica Blues", "Jump Blues", "Louisiana Blues", "Memphis Blues", "Modern Electric Blues", "Piano Blues", "Piedmont Blues", "Rhythm & Blues", "Texas Blues" };
static const char *STYLES_BRASS[] = { "", "Brass Band", "Marches", "Military", "Pipe & Drum" };
static const char *STYLES_CHILDRENS[] = { "", "Educational", "Nursery Rhymes", "Story" };
static const char *STYLES_CLASSICAL[] = { "", "Baroque", "Choral", "Classical", "Contemporary", "Early", "Impressionist", "Medieval", "Modern", "Neo-Classical", "Neo-Romantic", "Opera", "Operetta", "Oratorio", "Post-Modern", "Renaissance", "Romantic", "Serial", "Twelve-tone", "Zarzuela" };
static const char *STYLES_ELECTRONIC[] = { "", "Abstract", "Acid", "Acid House", "Acid Jazz", "Ambient", "Ballroom", "Baltimore Club", "Bassline", "Beatdown", "Berlin-School", "Big Beat", "Breakbeat", "Breakcore", "Breaks", "Broken Beat", "Chillwave", "Chiptune", "Dance-pop", "Dark Ambient", "Darkwave", "Deep House", "Deep Techno", "Disco", "Disco Polo", "Donk", "Doomcore", "Downtempo", "Drone", "Drum n Bass", "Dub", "Dub Techno", "Dubstep", "Dungeon Synth", "EBM", "Electro", "Electro House", "Electroclash", "Euro House", "Euro-Disco", "Eurobeat", "Eurodance", "Experimental", "Freestyle", "Funkot", "Future Jazz", "Gabber", "Garage House", "Ghetto", "Ghetto House", "Ghettotech", "Glitch", "Goa Trance", "Grime", "Hands Up", "Happy Hardcore", "Hard Beat", "Hard House", "Hard Techno", "Hard Trance", "Hardcore", "Hardstyle", "Harsh Noise Wall", "Hi NRG", "Hip Hop", "Hip-House", "House", "IDM", "Illbient", "Industrial", "Italo House", "Italo-Disco", "Italodance", "J-Core", "Jazzdance", "Juke", "Jumpstyle", "Jungle", "Latin", "Leftfield", "Lento Violento", "Makina", "Minimal", "Minimal Techno", "Modern Classical", "Musique Concr\xc3\xa8te", "Neo Trance", "Neofolk", "Nerdcore Techno", "New Age", "New Beat", "New Wave", "Noise", "Nu-Disco", "Power Electronics", "Progressive Breaks", "Progressive House", "Progressive Trance", "Psy-Trance", "Rhythmic Noise", "Schranz", "Skweee", "Sound Collage", "Speed Garage", "Speedcore", "Synth-pop", "Synthwave", "Tech House", "Tech Trance", "Techno", "Trance", "Tribal", "Tribal House", "Trip Hop", "Tropical House", "UK Funky", "UK Garage", "Vaporwave", "Witch House" };
static const char *STYLES_FOLK[] = { "", "Aboriginal", "African", "Andalusian Classical", "Appalachian Music", "Bangladeshi Classical", "Basque Music", "Bengali Music", "Bhangra", "Bluegrass", "Cajun", "Cambodian Classical", "Canzone Napoletana", "Carnatic", "Catalan Music", "Celtic", "Chacarera", "Chamam\xc3\xa9", "Chinese Classical", "Chutney", "Cobla", "Copla", "Country", "Dangdut", "\xc3\x89ntekhno", "Fado", "Filk", "Flamenco", "Folk", "Funan\xc3\xa1", "Gagaku", "Gamelan", "Gospel", "Griot", "Guarania", "Hawaiian", "Highlife", "Hillbilly", "Hindustani", "Honky Tonk", "Indian Classical", "Jota", "Kaseko", "Keroncong", "Kizomba", "Klasik", "Klezmer", "Korean Court Music", "La\xc3\xafk\xc3\xb3", "Lao Music", "Liscio", "Luk Krung", "Luk Thung", "Maloya", "Mbalax", "Min'y\xc5\x8d", "Mizrahi", "Mouth Music", "Mugham", "N\xc3\xa9pzene", "Nordic", "Ottoman Classical", "Overtone Singing", "Pacific", "Pasodoble", "Persian Classical", "Philippine Classical", "Phleng Phuea Chiwit", "Piobaireachd", "Polka", "Progressive Bluegrass", "Ra\xc3\xaf", "Rebetiko", "Romani", "Rune Singing", "Salegy", "S\xc3\xa1mi Music", "Sea Shanties", "S\xc3\xa9ga", "Sephardic", "Soukous", "Thai Classical", "Volksmusik", "Waiata", "Western Swing", "Yemenite Jewish", "Zamba", "Zemer Ivri", "Zouk", "Zydeco" };
static const char *STYLES_FUNK[] = { "", "Afrobeat", "Bayou Funk", "Boogie", "Contemporary R&B", "Disco", "Free Funk", "Funk", "Gogo", "Gospel", "Minneapolis Sound", "Neo Soul", "New Jack Swing", "P.Funk", "Psychedelic", "Rhythm & Blues", "Soul", "Swingbeat", "UK Street Soul" };
static const char *STYLES_HIPHOP[] = { "", "Bass Music", "Beatbox", "Bongo Flava", "Boom Bap", "Bounce", "Britcore", "Cloud Rap", "Conscious", "Crunk", "Cut-up/DJ", "DJ Battle Tool", "Electro", "Favela Funk", "G-Funk", "Gangsta", "Go-Go", "Grime", "Hardcore Hip-Hop", "Hiplife", "Horrorcore", "Hyphy", "Instrumental", "Jazzy Hip-Hop", "Kwaito", "Miami Bass", "Motswako", "Pop Rap", "Ragga HipHop", "RnB/Swing", "Screw", "Spaza", "Thug Rap", "Trap", "Trip Hop", "Turntablism" };
static const char *STYLES_JAZZ[] = { "", "Afro-Cuban Jazz", "Afrobeat", "Avant-garde Jazz", "Big Band", "Bop", "Bossa Nova", "Cape Jazz", "Contemporary Jazz", "Cool Jazz", "Dixieland", "Easy Listening", "Free Improvisation", "Free Jazz", "Fusion", "Gypsy Jazz", "Hard Bop", "Jazz-Funk", "Jazz-Rock", "Latin Jazz", "Modal", "Post Bop", "Ragtime", "Smooth Jazz", "Soul-Jazz", "Space-Age", "Swing" };
static const char *STYLES_LATIN[] = { "", "Afro-Cuban", "Ax\xc3\xa9", "Bachata", "Ba\xc3\xa3o", "Batucada", "Beguine", "Bolero", "Bomba", "Boogaloo", "Bossanova", "Candombe", "Carimb\xc3\xb3", "Cha-Cha", "Champeta", "Charanga", "Choro", "Compas", "Conjunto", "Corrido", "Cuatro", "Cubano", "Cumbia", "Danzon", "Descarga", "Forr\xc3\xb3", "Gaita", "Guaguanc\xc3\xb3", "Guajira", "Guaracha", "Jibaro", "Joropo", "Lambada", "Mambo", "Marcha Carnavalesca", "Mariachi", "Marimba", "Merengue", "MPB", "Musette", "M\xc3\xbasica Criolla", "Norte\xc3\xb1o", "Nueva Cancion", "Nueva Trova", "Occitan", "Pachanga", "Plena", "Porro", "Quechua", "Ranchera", "Reggaeton", "Rumba", "Salsa", "Samba", "Samba-Can\xc3\xa7\xc3\xa3o", "Seresta", "Son", "Son Montuno", "Sonero", "Tango", "Tejano", "Timba", "Trova", "Vallenato" };
static const char *STYLES_NONMUSIC[] = { "", "Audiobook", "Comedy", "Dialogue", "Education", "Field Recording", "Health-Fitness", "Interview", "Monolog", "Movie Effects", "Poetry", "Political", "Promotional", "Public Broadcast", "Public Service Announcement", "Radioplay", "Religious", "Sermon", "Sound Art", "Sound Poetry", "Special Effects", "Speech", "Spoken Word", "Technical", "Therapy" };
static const char *STYLES_POP[] = { "", "Ballad", "Barbershop", "Bollywood", "Break-In", "Bubblegum", "Chanson", "Enka", "Ethno-pop", "Europop", "Indie Pop", "J-pop", "K-pop", "Karaoke", "Kay\xc5\x8dkyoku", "Levenslied", "Light Music", "Music Hall", "N\xc3\xa9o Kyma", "Novelty", "Parody", "Schlager", "Vocal" };
static const char *STYLES_REGGAE[] = { "", "Azonto", "Bubbling", "Calypso", "Dancehall", "Dub", "Dub Poetry", "Junkanoo", "Lovers Rock", "Mento", "Ragga", "Rapso", "Reggae", "Reggae Gospel", "Reggae-Pop", "Rocksteady", "Roots Reggae", "Ska", "Soca", "Steel Band" };
static const char *STYLES_ROCK[] = { "", "Acid Rock", "Acoustic", "Alternative Rock", "AOR", "Arena Rock", "Art Rock", "Atmospheric Black Metal", "Avantgarde", "Beat", "Black Metal", "Blues Rock", "Brit Pop", "Classic Rock", "Coldwave", "Country Rock", "Crust", "Death Metal", "Deathcore", "Deathrock", "Depressive Black Metal", "Doo Wop", "Doom Metal", "Dream Pop", "Emo", "Ethereal", "Experimental", "Folk Metal", "Folk Rock", "Funeral Doom Metal", "Funk Metal", "Garage Rock", "Glam", "Goregrind", "Goth Rock", "Gothic Metal", "Grindcore", "Grunge", "Hard Rock", "Hardcore", "Heavy Metal", "Horror Rock", "Indie Rock", "Industrial", "Krautrock", "Lo-Fi", "Lounge", "Math Rock", "Melodic Death Metal", "Melodic Hardcore", "Metalcore", "Mod", "NDW", "Neofolk", "New Wave", "No Wave", "Noise", "Noisecore", "Nu Metal", "Oi", "Parody", "Pop Punk", "Pop Rock", "Pornogrind", "Post Rock", "Post-Hardcore", "Post-Metal", "Post-Punk", "Power Metal", "Power Pop", "Power Violence", "Prog Rock", "Progressive Metal", "Psychedelic Rock", "Psychobilly", "Pub Rock", "Punk", "Rock & Roll", "Rock Opera", "Rockabilly", "Shoegaze", "Ska", "Skiffle", "Sludge Metal", "Soft Rock", "Southern Rock", "Space Rock", "Speed Metal", "Stoner Rock", "Surf", "Swamp Pop", "Symphonic Rock", "Technical Death Metal", "Thrash", "Twist", "Viking Metal", "Y\xc3\xa9-Y\xc3\xa9" };
static const char *STYLES_STAGE[] = { "", "Musical", "Score", "Soundtrack", "Theme" };

struct GenreStyles { const char *genre; const char **styles; int n; };
#define GS(arr) (arr), (int)(sizeof(arr)/sizeof(arr[0]))
static const GenreStyles CRATEDIG_STYLES[] = {
    { "",                        GS(STYLES_ANY) },
    { "Blues",                   GS(STYLES_BLUES) },
    { "Brass & Military",        GS(STYLES_BRASS) },
    { "Children's",              GS(STYLES_CHILDRENS) },
    { "Classical",                GS(STYLES_CLASSICAL) },
    { "Electronic",               GS(STYLES_ELECTRONIC) },
    { "Folk, World, & Country",   GS(STYLES_FOLK) },
    { "Funk / Soul",              GS(STYLES_FUNK) },
    { "Hip Hop",                  GS(STYLES_HIPHOP) },
    { "Jazz",                     GS(STYLES_JAZZ) },
    { "Latin",                    GS(STYLES_LATIN) },
    { "Non-Music",                GS(STYLES_NONMUSIC) },
    { "Pop",                      GS(STYLES_POP) },
    { "Reggae",                   GS(STYLES_REGGAE) },
    { "Rock",                     GS(STYLES_ROCK) },
    { "Stage & Screen",           GS(STYLES_STAGE) },
};
#undef GS
static const int N_CRATEDIG_STYLE_GENRES = (int)(sizeof(CRATEDIG_STYLES) / sizeof(CRATEDIG_STYLES[0]));

/* Looks up the style array for whatever genre is currently selected
 * (matched by value string, not index — CRATEDIG_STYLES and
 * CRATEDIG_GENRES are independent arrays kept in the same order by
 * hand, so matching by string is a hair more robust than trusting that
 * ordering never drifts). Falls back to STYLES_ANY (just "") if the
 * current genre has no entry (shouldn't happen — every real genre does). */
static const GenreStyles *styles_for_genre(const char *genre_value) {
    for (int i = 0; i < N_CRATEDIG_STYLE_GENRES; i++)
        if (!strcmp(CRATEDIG_STYLES[i].genre, genre_value)) return &CRATEDIG_STYLES[i];
    return &CRATEDIG_STYLES[0];
}

static const char *CRATEDIG_DECADES[] = {
    "", "1950s", "1960s", "1970s", "1980s", "1990s", "2000s", "2010s", "2020s",
};
static const int N_CRATEDIG_DECADES = (int)(sizeof(CRATEDIG_DECADES) / sizeof(CRATEDIG_DECADES[0]));

/* Region is a UI-only grouping (not a real Discogs API field — the API
 * only takes `country`); it exists purely so the shadow GUI's country
 * stepper doesn't have to cycle through ~70 countries one at a time to
 * reach, say, Japan. Selecting a region resets the country index to 0
 * ("Any" within that region) the same way selecting a genre resets style. */
static const char *CRATEDIG_REGIONS[] = { "Any", "Americas", "Europe", "Africa", "Asia", "Oceania" };
static const int N_CRATEDIG_REGIONS = (int)(sizeof(CRATEDIG_REGIONS) / sizeof(CRATEDIG_REGIONS[0]));

static const char *COUNTRIES_ANY[] = { "" };
static const char *COUNTRIES_AMERICAS[] = { "", "Argentina", "Brazil", "Canada", "Chile", "Colombia", "Cuba", "Haiti", "Jamaica", "Mexico", "Peru", "Puerto Rico", "Trinidad & Tobago", "US", "Venezuela" };
static const char *COUNTRIES_EUROPE[] = { "", "Austria", "Belgium", "Bulgaria", "Croatia", "Czech Republic", "Denmark", "Finland", "France", "Germany", "Greece", "Hungary", "Iceland", "Ireland", "Italy", "Netherlands", "Norway", "Poland", "Portugal", "Romania", "Russia", "Serbia", "Spain", "Sweden", "Switzerland", "Turkey", "UK", "Ukraine" };
static const char *COUNTRIES_AFRICA[] = { "", "Algeria", "Benin", "Cameroon", "Cape Verde", "Congo", "Egypt", "Ethiopia", "Ghana", "Guinea", "Ivory Coast", "Kenya", "Mali", "Morocco", "Nigeria", "Senegal", "South Africa", "Tanzania", "Zimbabwe" };
static const char *COUNTRIES_ASIA[] = { "", "China", "India", "Indonesia", "Iran", "Israel", "Japan", "Lebanon", "Pakistan", "Philippines", "South Korea", "Taiwan", "Thailand", "Vietnam" };
static const char *COUNTRIES_OCEANIA[] = { "", "Australia", "New Zealand" };
static const char **CRATEDIG_COUNTRIES_BY_REGION[] = {
    COUNTRIES_ANY, COUNTRIES_AMERICAS, COUNTRIES_EUROPE, COUNTRIES_AFRICA, COUNTRIES_ASIA, COUNTRIES_OCEANIA,
};
static const int N_COUNTRIES_BY_REGION[] = {
    (int)(sizeof(COUNTRIES_ANY)/sizeof(COUNTRIES_ANY[0])),
    (int)(sizeof(COUNTRIES_AMERICAS)/sizeof(COUNTRIES_AMERICAS[0])),
    (int)(sizeof(COUNTRIES_EUROPE)/sizeof(COUNTRIES_EUROPE[0])),
    (int)(sizeof(COUNTRIES_AFRICA)/sizeof(COUNTRIES_AFRICA[0])),
    (int)(sizeof(COUNTRIES_ASIA)/sizeof(COUNTRIES_ASIA[0])),
    (int)(sizeof(COUNTRIES_OCEANIA)/sizeof(COUNTRIES_OCEANIA[0])),
};

/* Current shadow-side selection. Nothing is sent to the DSP core until
 * "cratedig_search_go" is SET (the FILTERS tab's dedicated SEARCH
 * button) — selecting a filter only updates this local state, unlike
 * the first version of this feature which re-searched on every tap (see
 * the conversation this changed in: a dedicated search button was
 * requested specifically to stop that). */
static int g_cd_genre_idx = 0;
static int g_cd_style_idx = 0;
static int g_cd_decade_idx = 0;
static int g_cd_region_idx = 0;
static int g_cd_country_idx = 0;

static std::string build_search_results_json_locked(bool shadow_safe) {
    char buf[256];
    int n = g_api->get_param(g_inst, "search_count", buf, sizeof(buf));
    int count = (n > 0) ? std::atoi(std::string(buf, n).c_str()) : 0;

    std::string json = "[";
    for (int i = 0; i < count; i++) {
        char key[48];
        std::string title, channel, duration, year;

        std::snprintf(key, sizeof(key), "search_result_title_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) title.assign(buf, n);

        std::snprintf(key, sizeof(key), "search_result_channel_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) channel.assign(buf, n);

        std::snprintf(key, sizeof(key), "search_result_duration_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) duration.assign(buf, n);

        /* Crate Dig results resolve to a YouTube stream internally (this
         * addon's own UI never exposes provider choice — see main()'s
         * forced search_provider — so search_result_provider_<n> is
         * always "youtube" here and not worth showing). meta_year is
         * Discogs' own release year instead, which is actually useful. */
        std::snprintf(key, sizeof(key), "search_result_year_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) year.assign(buf, n);

        std::string label;
        if (shadow_safe) {
            /* Only ". - / > % + :" survive shadow_font_safe()'s filter, so
             * build the separator structure from those, not from brackets/
             * em dash/parens which would just get stripped anyway. */
            label = shadow_font_safe(title.empty() ? "UNTITLED" : title);
            if (!channel.empty()) label += " - " + shadow_font_safe(channel);
            if (!year.empty()) label += " " + shadow_font_safe(year);
            if (!duration.empty()) label += " " + shadow_font_safe(duration);
        } else {
            label = title.empty() ? "(untitled)" : title;
            if (!channel.empty()) label += "  \xe2\x80\x94 " + channel;   /* em dash */
            if (!year.empty()) label += "  (" + year + ")";
            if (!duration.empty()) label += "  [" + duration + "]";
        }

        if (i) json += ",";
        json += "{\"label\":\"" + json_escape(label) + "\"}";
    }
    json += "]";
    return json;
}

/* Generic "now playing" file force-audioin's skipbackHost.c reads (added
 * there for this — see that repo's own commit) to name a skipback
 * recording after the actual track instead of the Force project name,
 * and its own tempo instead of the real project tempo WHEN one is
 * actually available (line 2, optional — see write_nowplaying_file()).
 * Not cratedigger-specific by convention — any addon that knows what's
 * actually playing can write here — but this is the only writer that
 * currently exists. Cleared on stop so a later non-cratedigger skipback
 * recording (or a stale cratedigger session) doesn't get mislabeled with
 * an old title; also self-expires after 10 minutes on skipbackHost's own
 * side regardless. */
static const char *NOWPLAYING_PATH = "/tmp/force_nowplaying.txt";

/* `tempo_bpm` is the track's own tempo if the source actually has one —
 * empty/omit it otherwise, which leaves skipbackHost's real project
 * tempo in place. As of this writing, force-cratedigger's own results
 * never pass one: Discogs (crate-dig's actual data source, see
 * search_result_provider_<n>/handle_play_result_index_locked's own
 * lookups) has no BPM field on a release at all — confirmed directly in
 * src/bin/yt_dlp_daemon.py's cratedig_search(), which sends "" for the
 * tempo field unconditionally (the SAMPLETTE_TEMPOS-style AcousticBrainz
 * tempo lookup a few lines above it only runs for a different, unused-
 * here provider). search_result_tempo_<n> is read anyway rather than
 * hardcoding an empty string here, so this starts working automatically
 * if a future daemon change ever populates it for crate-dig results
 * specifically, with zero change needed on this side. */
static void write_nowplaying_file(const std::string &title, const std::string &tempo_bpm) {
    FILE *f = fopen(NOWPLAYING_PATH, "w");
    if (!f) return;
    fputs(title.c_str(), f);
    fputc('\n', f);
    if (!tempo_bpm.empty()) {
        fputs(tempo_bpm.c_str(), f);
        fputc('\n', f);
    }
    fclose(f);
}
static void clear_nowplaying_file() { unlink(NOWPLAYING_PATH); }

static bool handle_play_result_index_locked(int idx, std::string &err) {
    char buf[256];
    char key[48];

    std::snprintf(key, sizeof(key), "search_result_provider_%d", idx);
    int n = g_api->get_param(g_inst, key, buf, sizeof(buf));
    std::string provider = (n > 0) ? std::string(buf, n) : std::string("youtube");

    std::snprintf(key, sizeof(key), "search_result_url_%d", idx);
    n = g_api->get_param(g_inst, key, buf, sizeof(buf));
    if (n <= 0) { err = "no such result"; return false; }
    std::string url(buf, n);

    std::snprintf(key, sizeof(key), "search_result_title_%d", idx);
    n = g_api->get_param(g_inst, key, buf, sizeof(buf));
    std::string title = (n > 0) ? std::string(buf, n) : std::string();

    std::snprintf(key, sizeof(key), "search_result_channel_%d", idx);
    n = g_api->get_param(g_inst, key, buf, sizeof(buf));
    std::string channel = (n > 0) ? std::string(buf, n) : std::string();

    /* Almost always empty for crate-dig results today — see
     * write_nowplaying_file()'s own comment for exactly why — but read
     * for real rather than assumed, so this just works the day that
     * changes. */
    std::snprintf(key, sizeof(key), "search_result_tempo_%d", idx);
    n = g_api->get_param(g_inst, key, buf, sizeof(buf));
    std::string tempo = (n > 0) ? std::string(buf, n) : std::string();

    g_api->set_param(g_inst, "stream_provider", provider.c_str());
    g_api->set_param(g_inst, "stream_url", url.c_str());
    /* Not cratedig-specific despite the name — the core's own next_track_step
     * and stream_eof auto-advance handlers already track "current selection"
     * generically through this one field regardless of provider, so reusing
     * it here keeps the shadow GUI's `list` widget `sel=` highlight (see
     * shadow_page.conf) in sync with whatever was actually selected. */
    char idx_str[16];
    std::snprintf(idx_str, sizeof(idx_str), "%d", idx);
    g_api->set_param(g_inst, "cratedig_result_index", idx_str);

    if (!title.empty()) {
        std::string nowplaying = title;
        if (!channel.empty()) nowplaying += " - " + channel;
        write_nowplaying_file(nowplaying, tempo);
    }
    return true;
}

/* "ANY" is shown for an empty filter value (genre/style/country's own
 * "no filter" entry is "", not a real Discogs string) rather than the
 * blank shadow_font_safe("") would otherwise produce - an empty stepper
 * box reads as broken, not as "unset". */
static std::string cd_display_text(const char *value) {
    return (value[0] == '\0') ? std::string("ANY") : shadow_font_safe(value);
}

static const GenreStyles *cd_current_styles_locked() {
    return styles_for_genre(CRATEDIG_GENRES[g_cd_genre_idx]);
}
static const char **cd_current_countries_locked(int *out_n) {
    *out_n = N_COUNTRIES_BY_REGION[g_cd_region_idx];
    return CRATEDIG_COUNTRIES_BY_REGION[g_cd_region_idx];
}

/* Composes all five current shadow-side selections into the DSP core's
 * own cratedig_filter JSON and forwards it - this is what actually
 * triggers the Discogs search (v2_set_param's own handling of that key,
 * not something this shim adds). Only called from the SEARCH button's
 * handler (SET cratedig_search_go) - selecting an individual filter
 * (genre/style/decade/region/country) only updates local state below,
 * deliberately NOT re-searching on every tap (an earlier version of this
 * feature did; a dedicated search button was requested specifically to
 * stop that - see the FILTERS tab's button in shadow_page.conf). */
static void send_cratedig_filter_from_shadow_state_locked() {
    const GenreStyles *styles = cd_current_styles_locked();
    const char *style_val = (g_cd_style_idx < styles->n) ? styles->styles[g_cd_style_idx] : "";
    int n_countries = 0;
    const char **countries = cd_current_countries_locked(&n_countries);
    const char *country_val = (g_cd_country_idx < n_countries) ? countries[g_cd_country_idx] : "";

    std::string json = "{\"genre\":\"" + json_escape(CRATEDIG_GENRES[g_cd_genre_idx]) +
                        "\",\"style\":\"" + json_escape(style_val) +
                        "\",\"decade\":\"" + json_escape(CRATEDIG_DECADES[g_cd_decade_idx]) +
                        "\",\"country\":\"" + json_escape(country_val) + "\"}";
    g_api->set_param(g_inst, "cratedig_filter", json.c_str());
}

static void handle_ctrl_line(int fd, const std::string &line) {
    char cmd[16] = {0}, key[64] = {0}, val[256] = {0};
    if (sscanf(line.c_str(), "%15s", cmd) != 1) { send(fd, "ERR\n", 4, 0); return; }

    if (!strcmp(cmd, "DESCRIBE")) {
        std::string reply = g_module_json + "\n";
        send(fd, reply.c_str(), reply.size(), 0);
        return;
    }
    if (!strcmp(cmd, "SET") && sscanf(line.c_str(), "%*s %63s %255[^\n]", key, val) == 2) {
        if (handle_mix_set(key, val)) { send(fd, "OK\n", 3, 0); return; }
        if (!strcmp(key, "play_result_index")) {
            int idx = std::atoi(val);
            std::string err;
            std::lock_guard<std::mutex> lk(g_lock);
            if (handle_play_result_index_locked(idx, err)) send(fd, "OK\n", 3, 0);
            else send(fd, "ERR\n", 4, 0);
            return;
        }
        if (!strcmp(key, "cratedig_genre_index")) {
            int idx = std::atoi(val);
            if (idx < 0 || idx >= N_CRATEDIG_GENRES) { send(fd, "ERR\n", 4, 0); return; }
            std::lock_guard<std::mutex> lk(g_lock);
            g_cd_genre_idx = idx;
            g_cd_style_idx = 0;   /* previous genre's style index may not exist in the new one */
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "cratedig_style_index")) {
            int idx = std::atoi(val);
            std::lock_guard<std::mutex> lk(g_lock);
            const GenreStyles *styles = cd_current_styles_locked();
            if (idx < 0 || idx >= styles->n) { send(fd, "ERR\n", 4, 0); return; }
            g_cd_style_idx = idx;
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "cratedig_decade_index")) {
            int idx = std::atoi(val);
            if (idx < 0 || idx >= N_CRATEDIG_DECADES) { send(fd, "ERR\n", 4, 0); return; }
            std::lock_guard<std::mutex> lk(g_lock);
            g_cd_decade_idx = idx;
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "cratedig_region_index")) {
            int idx = std::atoi(val);
            if (idx < 0 || idx >= N_CRATEDIG_REGIONS) { send(fd, "ERR\n", 4, 0); return; }
            std::lock_guard<std::mutex> lk(g_lock);
            g_cd_region_idx = idx;
            g_cd_country_idx = 0;   /* previous region's country index may not exist in the new one */
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "cratedig_country_index")) {
            int idx = std::atoi(val);
            std::lock_guard<std::mutex> lk(g_lock);
            int n_countries = 0;
            cd_current_countries_locked(&n_countries);
            if (idx < 0 || idx >= n_countries) { send(fd, "ERR\n", 4, 0); return; }
            g_cd_country_idx = idx;
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "cratedig_search_go")) {
            std::lock_guard<std::mutex> lk(g_lock);
            send_cratedig_filter_from_shadow_state_locked();
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "skipback_toggle")) {
            bool now_running = skipback_is_running();
            send_skipback_toggle(!now_running);
            send(fd, "OK\n", 3, 0);
            return;
        }
        if (!strcmp(key, "stop") || !strcmp(key, "stop_step")) {
            clear_nowplaying_file();
        }
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->set_param(g_inst, key, val);
        send(fd, "OK\n", 3, 0);
        return;
    }
    if (!strcmp(cmd, "GET") && sscanf(line.c_str(), "%*s %63s", key) == 1) {
        std::string mix_val;
        if (handle_mix_get(key, mix_val)) {
            std::string reply = mix_val + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "search_results_json") || !strcmp(key, "search_results_shadow_json")) {
            bool shadow_safe = !strcmp(key, "search_results_shadow_json");
            std::lock_guard<std::mutex> lk(g_lock);
            std::string reply = build_search_results_json_locked(shadow_safe) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        /* Five stepper GET triples (text/idx/count), all built the same
         * way: <dimension>_text is cd_display_text() of the currently
         * selected value ("ANY" for ""), _idx/_count drive the stepper
         * widget's own bounds. Style is genre-dependent, country is
         * region-dependent - both look up their current list fresh each
         * call rather than caching it, since the dependency can change
         * between calls (a genre tap resets style to index 0, but the
         * *list itself* also changes shape). */
        if (!strcmp(key, "cratedig_genre_text")) {
            std::lock_guard<std::mutex> lk(g_lock);
            std::string reply = cd_display_text(CRATEDIG_GENRES[g_cd_genre_idx]) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_genre_idx")) {
            std::string reply = std::to_string(g_cd_genre_idx) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_genre_count")) {
            std::string reply = std::to_string(N_CRATEDIG_GENRES) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_style_text")) {
            std::lock_guard<std::mutex> lk(g_lock);
            const GenreStyles *styles = cd_current_styles_locked();
            int idx = (g_cd_style_idx < styles->n) ? g_cd_style_idx : 0;
            std::string reply = cd_display_text(styles->styles[idx]) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_style_idx")) {
            std::string reply = std::to_string(g_cd_style_idx) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_style_count")) {
            std::lock_guard<std::mutex> lk(g_lock);
            std::string reply = std::to_string(cd_current_styles_locked()->n) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_decade_text")) {
            std::string reply = cd_display_text(CRATEDIG_DECADES[g_cd_decade_idx]) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_decade_idx")) {
            std::string reply = std::to_string(g_cd_decade_idx) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_decade_count")) {
            std::string reply = std::to_string(N_CRATEDIG_DECADES) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_region_text")) {
            std::string reply = shadow_font_safe(CRATEDIG_REGIONS[g_cd_region_idx]) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_region_idx")) {
            std::string reply = std::to_string(g_cd_region_idx) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_region_count")) {
            std::string reply = std::to_string(N_CRATEDIG_REGIONS) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_country_text")) {
            std::lock_guard<std::mutex> lk(g_lock);
            int n = 0; const char **countries = cd_current_countries_locked(&n);
            int idx = (g_cd_country_idx < n) ? g_cd_country_idx : 0;
            std::string reply = cd_display_text(countries[idx]) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_country_idx")) {
            std::string reply = std::to_string(g_cd_country_idx) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "cratedig_country_count")) {
            std::lock_guard<std::mutex> lk(g_lock);
            int n = 0; cd_current_countries_locked(&n);
            std::string reply = std::to_string(n) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "skipback_running")) {
            std::string reply = std::string(skipback_is_running() ? "1" : "0") + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        if (!strcmp(key, "skipback_status")) {
            /* Already upper-case ASCII-only, so this one string works
             * unmodified for both the web GUI and (via this exact key,
             * no separate "_shadow" variant needed) Force Shadow's font. */
            std::string reply = std::string(skipback_is_running() ? "RECORDING" : "STOPPED") + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }
        /* Generic "<real key>_shadow" convention: strip the suffix, fetch
         * the real value (core or mix.*), and run it through
         * shadow_font_safe() before returning. Exists because most of the
         * DSP core's own text status values (stream_status: "streaming"/
         * "paused"/...; search_status: "idle"/"searching"/...) are
         * lowercase words that would render as scattered near-blank text
         * on Force Shadow's uppercase-only font otherwise (same class of
         * bug search_results_shadow_json's own comment describes) —
         * confirmed by an offline render before this existed. shadow_page
         * .conf's STATUS/SEARCH readouts use e.g. get=stream_status_shadow.
         * playback_time is exempt (digits + ':' only, already safe) so
         * its readout can use the plain key directly. */
        std::string keystr(key);
        static const std::string kShadowSuffix = "_shadow";
        if (keystr.size() > kShadowSuffix.size() &&
            keystr.compare(keystr.size() - kShadowSuffix.size(), kShadowSuffix.size(), kShadowSuffix) == 0) {
            std::string real_key = keystr.substr(0, keystr.size() - kShadowSuffix.size());
            std::string mix_v;
            std::string raw;
            if (handle_mix_get(real_key, mix_v)) {
                raw = mix_v;
            } else {
                char buf[4096];
                int n;
                { std::lock_guard<std::mutex> lk(g_lock);
                  n = g_api->get_param(g_inst, real_key.c_str(), buf, sizeof(buf)); }
                if (n <= 0) { send(fd, "ERR\n", 4, 0); return; }
                raw.assign(buf, n);
            }
            std::string reply = shadow_font_safe(raw) + "\n";
            send(fd, reply.c_str(), reply.size(), 0);
            return;
        }

        char buf[4096];
        int n;
        { std::lock_guard<std::mutex> lk(g_lock);
          n = g_api->get_param(g_inst, key, buf, sizeof(buf)); }
        if (n <= 0) { send(fd, "ERR\n", 4, 0); return; }
        std::string reply(buf, n); reply += "\n";
        send(fd, reply.c_str(), reply.size(), 0);
        return;
    }
    send(fd, "ERR\n", 4, 0);
}

static void ctrl_server_loop(int lfd) {
    while (g_run.load()) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) continue;
        char buf[512];
        ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string line(buf);
            size_t nl = line.find('\n');
            if (nl != std::string::npos) line.resize(nl);
            if (g_verbose) fprintf(stderr, "[cratedigger] ctrl: %s\n", line.c_str());
            handle_ctrl_line(cfd, line);
        }
        close(cfd);
    }
}

static int ctrl_socket_listen(const std::string &path) {
    unlink(path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { perror("bind"); close(fd); return -1; }
    chmod(path.c_str(), 0666);
    if (listen(fd, 8) != 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
static void on_signal(int) { g_run.store(false); }

static void usage(const char *me) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -v                    verbose\n"
        "  --module-dir PATH     dir containing module.json, bin/yt-dlp,\n"
        "                        bin/yt_dlp_daemon.py, bin/ffmpeg (default: .)\n"
        "  --ctrl-sock PATH      control socket path (default: /tmp/cratedigger_ctrl.sock)\n"
        "  --skipback-nsmodule-path PATH\n"
        "                        path to ForceAudioJackSkipback's own NSMODULE.json,\n"
        "                        for the SKIPBACK REC button (default: a CHANGE_ME\n"
        "                        placeholder - see README.md)\n"
        "  --mix-slot N          voice slot 0..%d for forceAudioIn.so (default: 0) -\n"
        "                        each simultaneous voice needs a distinct slot\n",
        me, AI_MAX_VOICES - 1);
}

int main(int argc, char **argv) {
    /* Must be set before create_instance(): the DSP core spawns its
     * yt-dlp daemon subprocess (and later ffmpeg/yt-dlp pipes) from
     * inside create_instance's warmup thread, and a write to any of
     * their pipes after the child has already died raises SIGPIPE —
     * whose default action kills this whole process. Confirmed live in
     * local testing (exit 141) with no bin/yt-dlp present yet. Move's own
     * real host process presumably ignores SIGPIPE itself; this shim
     * provides that same protection explicitly since nothing else here
     * does. */
    std::signal(SIGPIPE, SIG_IGN);

    std::string module_dir = ".";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if      (a == "-v")                        g_verbose = true;
        else if (a == "--module-dir" && i+1 < argc) module_dir = argv[++i];
        else if (a == "--ctrl-sock"  && i+1 < argc) g_ctrl_sock_path = argv[++i];
        else if (a == "--skipback-nsmodule-path" && i+1 < argc) g_skipback_nsmodule_path = argv[++i];
        else if (a == "--mix-slot" && i+1 < argc) {
            int s = std::atoi(argv[++i]);
            if (s < 0 || s >= AI_MAX_VOICES) { usage(argv[0]); return 2; }
            g_mix_slot = (unsigned)s;
        }
        else { usage(argv[0]); return (a == "-h" || a == "--help") ? 0 : 2; }
    }

    {
        std::string path = module_dir + "/module.json";
        FILE *f = fopen(path.c_str(), "rb");
        if (f) {
            char buf[65536];
            size_t n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            g_module_json.assign(buf, n);
        } else {
            fprintf(stderr, "[cratedigger] warning: %s not found; DESCRIBE will return empty\n", path.c_str());
            g_module_json = "{}";
        }
    }

    /* The device's Python (/usr/bin/python3 -> AddOns/Python/python3.8) has
     * no `zlib` module built in at all - yt-dlp hard-requires it and refuses
     * to even start without it ("yt-dlp is unavailable"), which silently
     * broke every SEARCH/DOWNLOAD. Rather than touch the device's shared
     * system Python (used by nodeServer and other addons too, and it
     * wouldn't survive a MockbaMod Python update), bin/pylib/ ships a
     * private zlib.cpython-38-arm-linux-gnueabihf.so built by
     * scripts/build-pyzlib.sh - self-contained, statically linked against
     * zlib 1.3.1's own source, no dependency on the device's own
     * /usr/lib/libz.so.1. Prepending it to PYTHONPATH here, before
     * create_instance() spawns the yt-dlp daemon child (execlp("python3",
     * ...) in yt_stream_plugin.c), makes python3's `import zlib` resolve to
     * it via ordinary environment inheritance across fork()/exec() - no
     * change needed in that vendored upstream file. Verified live
     * (2026-09-23): SEARCH against yt/archive/soundcloud all went from
     * "yt-dlp is unavailable" to real results with this in place. */
    {
        std::string pylib_path = module_dir + "/bin/pylib";
        const char *existing = getenv("PYTHONPATH");
        std::string new_path = (existing && *existing)
            ? (pylib_path + ":" + existing) : pylib_path;
        setenv("PYTHONPATH", new_path.c_str(), 1);
    }

    if (!shm_setup()) { fprintf(stderr, "[cratedigger] shared memory setup failed\n"); return 1; }

    g_api = move_plugin_init_v2(nullptr);
    if (!g_api || g_api->api_version != 2) {
        fprintf(stderr, "[cratedigger] core init failed\n"); return 1;
    }
    g_inst = g_api->create_instance(module_dir.c_str(), nullptr);
    if (!g_inst) { fprintf(stderr, "[cratedigger] create_instance failed\n"); return 1; }

    /* This addon's own UI (web + shadow) only ever exposes Crate Dig, but
     * the DSP core defaults search_provider to "youtube" internally (see
     * v2_create_instance in yt_stream_plugin.c) - force it to cratedig at
     * startup so nothing is ever left pointed at a provider this UI can't
     * reach. Every other provider is still fully present in the engine
     * (untouched, per the "port the host, not the DSP" approach) - simply
     * not surfaced here. */
    g_api->set_param(g_inst, "search_provider", "cratedig");

    int lfd = ctrl_socket_listen(g_ctrl_sock_path);
    if (lfd < 0) { fprintf(stderr, "[cratedigger] control socket setup failed\n"); return 1; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    fprintf(stderr,
        "[cratedigger] up. ctrl socket %s  shm %s  module_dir %s\n"
        "[cratedigger] audio is mixed into the Force's capture input via\n"
        "[cratedigger] ForceAudioIn (must be enabled separately).\n",
        g_ctrl_sock_path.c_str(), g_shm_name, module_dir.c_str());

    std::thread timer(timer_loop);
    std::thread ctrl(ctrl_server_loop, lfd);

    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    timer.join();
    close(lfd);
    unlink(g_ctrl_sock_path.c_str());
    clear_nowplaying_file();
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->destroy_instance(g_inst);
    }
    if (g_shm) { munmap(g_shm, AI_SHM_BYTES); shm_unlink(g_shm_name); }
    fprintf(stderr, "[cratedigger] bye\n");
    return 0;
}
