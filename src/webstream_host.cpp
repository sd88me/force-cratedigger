/* webstream_host.cpp — Force/MockbaMod runtime host for the ported
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
 * Build: see scripts/build.sh (native armhf under QEMU, links -lpthread,
 * same toolchain as force-maze/force-acid — no -lasound needed here).
 */

#include <atomic>
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
#include <sys/un.h>
#include <unistd.h>

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
static std::string g_ctrl_sock_path = "/tmp/webstream_ctrl.sock";
static bool         g_verbose = false;
static unsigned     g_mix_slot = 0;    /* which /forceAudioInjectN this instance owns */

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
            fprintf(stderr, "[webstream] render thread: max wake gap %.1fms, %llu/%llu wakes > 9ms, ring drops %llu, "
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

static std::string build_search_results_json_locked(bool shadow_safe) {
    char buf[256];
    int n = g_api->get_param(g_inst, "search_count", buf, sizeof(buf));
    int count = (n > 0) ? std::atoi(std::string(buf, n).c_str()) : 0;

    std::string json = "[";
    for (int i = 0; i < count; i++) {
        char key[48];
        std::string title, channel, duration, provider;

        std::snprintf(key, sizeof(key), "search_result_title_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) title.assign(buf, n);

        std::snprintf(key, sizeof(key), "search_result_channel_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) channel.assign(buf, n);

        std::snprintf(key, sizeof(key), "search_result_duration_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) duration.assign(buf, n);

        std::snprintf(key, sizeof(key), "search_result_provider_%d", i);
        n = g_api->get_param(g_inst, key, buf, sizeof(buf));
        if (n > 0) provider.assign(buf, n);

        std::string label;
        if (shadow_safe) {
            /* Only ". - / > % + :" survive shadow_font_safe()'s filter, so
             * build the separator structure from those, not from brackets/
             * em dash/parens which would just get stripped anyway. */
            label = shadow_font_safe(title.empty() ? "UNTITLED" : title);
            if (!channel.empty()) label += " - " + shadow_font_safe(channel);
            if (!duration.empty()) label += " " + shadow_font_safe(duration);
            if (!provider.empty()) label = shadow_font_safe(provider) + ": " + label;
        } else {
            label = title.empty() ? "(untitled)" : title;
            if (!channel.empty()) label += "  \xe2\x80\x94 " + channel;   /* em dash */
            if (!duration.empty()) label += "  [" + duration + "]";
            if (!provider.empty()) label = "[" + provider + "] " + label;
        }

        if (i) json += ",";
        json += "{\"label\":\"" + json_escape(label) + "\"}";
    }
    json += "]";
    return json;
}

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
    return true;
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
            if (g_verbose) fprintf(stderr, "[webstream] ctrl: %s\n", line.c_str());
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
        "  --ctrl-sock PATH      control socket path (default: /tmp/webstream_ctrl.sock)\n"
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
            fprintf(stderr, "[webstream] warning: %s not found; DESCRIBE will return empty\n", path.c_str());
            g_module_json = "{}";
        }
    }

    if (!shm_setup()) { fprintf(stderr, "[webstream] shared memory setup failed\n"); return 1; }

    g_api = move_plugin_init_v2(nullptr);
    if (!g_api || g_api->api_version != 2) {
        fprintf(stderr, "[webstream] core init failed\n"); return 1;
    }
    g_inst = g_api->create_instance(module_dir.c_str(), nullptr);
    if (!g_inst) { fprintf(stderr, "[webstream] create_instance failed\n"); return 1; }

    int lfd = ctrl_socket_listen(g_ctrl_sock_path);
    if (lfd < 0) { fprintf(stderr, "[webstream] control socket setup failed\n"); return 1; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    fprintf(stderr,
        "[webstream] up. ctrl socket %s  shm %s  module_dir %s\n"
        "[webstream] audio is mixed into the Force's capture input via\n"
        "[webstream] ForceAudioIn (must be enabled separately).\n",
        g_ctrl_sock_path.c_str(), g_shm_name, module_dir.c_str());

    std::thread timer(timer_loop);
    std::thread ctrl(ctrl_server_loop, lfd);

    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    timer.join();
    close(lfd);
    unlink(g_ctrl_sock_path.c_str());
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_api->destroy_instance(g_inst);
    }
    if (g_shm) { munmap(g_shm, AI_SHM_BYTES); shm_unlink(g_shm_name); }
    fprintf(stderr, "[webstream] bye\n");
    return 0;
}
