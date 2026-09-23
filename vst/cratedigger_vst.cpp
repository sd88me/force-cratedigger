/* =============================================================================
 * cratedigger_vst.cpp - Crate Digger as a VST2 instrument for the MPC OS plugin
 * host (see https://github.com/sd88me/mpc-vst-plugins). The same schwung-webstream
 * DSP core the Force host (src/cratedigger_host.cpp) links, built with
 * -DYT_POSIX_SPAWN (no fork() inside MPC), plus this host glue:
 *
 *  - The core renders on a worker thread into a ring buffer; MPC's audio callback
 *    only copies from the ring. The core starts ffmpeg pipelines, takes locks and
 *    reads pipes from inside render_block, none of which may happen on MPC's
 *    audio thread.
 *  - The Force host's control-socket glue (filter steppers, SEARCH, transport,
 *    result list) becomes VST parameters whose value text MPC polls for display
 *    (verified: mpc-vst-plugins docs/NOTES.md "Dynamic text in skins").
 *  - Parameter table: params.json -> build/params_gen.h (gen_params.py).
 * ========================================================================== */
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <glob.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "plugin_api_v1.h"
#include "cratedig_tables.h"
#include "params_gen.h"

extern "C" plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host);

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24,
    effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35, effGetEffectName = 45,
    effGetVendorString = 47, effGetProductString = 48, effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

/* ---- constants ------------------------------------------------------------ */
static const int BLOCK = 128;             /* core's native block (44.1 kHz, same as Move) */
static const int RING = 16384;            /* frames, power of two */
static const int TARGET_FILL = 2048;      /* ~46 ms kept ahead of MPC */
static const int SLOTS = 8;               /* result rows per page */
enum { DIM_GENRE, DIM_STYLE, DIM_DECADE, DIM_REGION, DIM_COUNTRY, NDIMS };

struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    plugin_api_v2_t *api;
    void *core;
    std::mutex lock;                      /* serialises every core call */
    int16_t ring[RING * 2];
    std::atomic<uint32_t> wpos{0}, rpos{0};
    std::thread worker;
    std::atomic<bool> running{false};
    int dim[NDIMS] = {0, 0, 0, 0, 0};     /* filter selection (like the host's g_cd_*_idx) */
    int page = 0;
    float gain = 1.0f;
    volatile char release[NPARAMS];       /* triggers to report back to 0 */
    char chunk[256];
};

/* ---- small helpers -------------------------------------------------------- */
static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)

static std::string core_get(Plugin *p, const char *key) {
    char buf[512];
    int n = p->api->get_param(p->core, key, buf, sizeof buf);
    return n > 0 ? std::string(buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1)) : std::string();
}
static std::string core_get_idx(Plugin *p, const char *fmt, int i) {
    char key[64];
    std::snprintf(key, sizeof key, fmt, i);
    return core_get(p, key);
}
static std::string upper(std::string s) {
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
static void copy_str(void *dst, const std::string &s, size_t max) {
    std::strncpy((char *)dst, s.c_str(), max - 1);
    ((char *)dst)[max - 1] = 0;
}

/* ---- filter lists (cratedig_tables.h, shared with the Force host) --------- */
static int dim_count(Plugin *p, int d) {
    switch (d) {
    case DIM_GENRE: return N_CRATEDIG_GENRES;
    case DIM_STYLE: return styles_for_genre(CRATEDIG_GENRES[p->dim[DIM_GENRE]])->n;
    case DIM_DECADE: return N_CRATEDIG_DECADES;
    case DIM_REGION: return N_CRATEDIG_REGIONS;
    default: return N_COUNTRIES_BY_REGION[p->dim[DIM_REGION]];
    }
}
static const char *dim_value(Plugin *p, int d) {
    int i = p->dim[d];
    switch (d) {
    case DIM_GENRE: return CRATEDIG_GENRES[i];
    case DIM_STYLE: return styles_for_genre(CRATEDIG_GENRES[p->dim[DIM_GENRE]])->styles[i];
    case DIM_DECADE: return CRATEDIG_DECADES[i];
    case DIM_REGION: return CRATEDIG_REGIONS[i];
    default: return CRATEDIG_COUNTRIES_BY_REGION[p->dim[DIM_REGION]][i];
    }
}
static void set_dim(Plugin *p, int d, int i) {
    int n = dim_count(p, d);
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    p->dim[d] = i;
    if (d == DIM_GENRE) p->dim[DIM_STYLE] = 0;       /* style list depends on genre */
    if (d == DIM_REGION) p->dim[DIM_COUNTRY] = 0;    /* country list depends on region */
}

static std::string json_escape(const char *s) {
    std::string o;
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') o += '\\';
        o += *s;
    }
    return o;
}

/* ---- results -------------------------------------------------------------- */
static int result_count(Plugin *p) { return std::atoi(core_get(p, "search_count").c_str()); }
static int page_count(Plugin *p) { int n = result_count(p); return n > 0 ? (n + SLOTS - 1) / SLOTS : 1; }

static std::string result_label(Plugin *p, int idx) {
    std::string t = core_get_idx(p, "search_result_title_%d", idx);
    std::string ch = core_get_idx(p, "search_result_channel_%d", idx);
    std::string yr = core_get_idx(p, "search_result_year_%d", idx);
    std::string du = core_get_idx(p, "search_result_duration_%d", idx);
    char num[16];
    std::snprintf(num, sizeof num, "%02d  ", idx + 1);
    std::string l = num + (t.empty() ? std::string("(untitled)") : t);
    if (!ch.empty()) l += " - " + ch;
    if (!yr.empty()) l += "  " + yr;
    if (!du.empty()) l += "  " + du;
    return l;
}

/* Same two-step protocol as the host's handle_play_result_index_locked(). */
static bool play_result(Plugin *p, int idx) {
    std::string url = core_get_idx(p, "search_result_url_%d", idx);
    if (url.empty()) return false;
    std::string prov = core_get_idx(p, "search_result_provider_%d", idx);
    char s[16];
    std::snprintf(s, sizeof s, "%d", idx);
    p->api->set_param(p->core, "stream_provider", prov.empty() ? "youtube" : prov.c_str());
    p->api->set_param(p->core, "stream_url", url.c_str());
    p->api->set_param(p->core, "cratedig_result_index", s);
    return true;
}
static int current_result(Plugin *p) {
    if (core_get(p, "stream_url").empty()) return -1;
    return std::atoi(core_get(p, "cratedig_result_index").c_str());
}

static void do_search(Plugin *p) {
    std::string json = std::string("{\"genre\":\"") + json_escape(dim_value(p, DIM_GENRE)) +
                       "\",\"style\":\"" + json_escape(dim_value(p, DIM_STYLE)) +
                       "\",\"decade\":\"" + json_escape(dim_value(p, DIM_DECADE)) +
                       "\",\"country\":\"" + json_escape(dim_value(p, DIM_COUNTRY)) + "\"}";
    p->page = 0;
    p->api->set_param(p->core, "cratedig_filter", json.c_str());
    LOG("search %s\n", json.c_str());
}

/* ---- worker: the core renders here, never on MPC's audio thread ------------ */
static void worker_main(Plugin *p) {
    int16_t block[BLOCK * 2];
    while (p->running.load()) {
        uint32_t fill = p->wpos.load() - p->rpos.load();
        if (fill + BLOCK > (uint32_t)TARGET_FILL) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(p->lock);
            p->api->render_block(p->core, block, BLOCK);
        }
        uint32_t w = p->wpos.load();
        for (int i = 0; i < BLOCK; i++) {
            uint32_t k = (w + i) & (RING - 1);
            p->ring[k * 2] = block[i * 2];
            p->ring[k * 2 + 1] = block[i * 2 + 1];
        }
        p->wpos.store(w + BLOCK);
    }
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *p = (Plugin *)e->object;
    (void)in;
    for (int i = 0; i < NPARAMS; i++)
        if (p->release[i]) { p->release[i] = 0; p->master(e, audioMasterAutomate, i, 0, 0, 0.0f); }
    uint32_t r = p->rpos.load(), avail = p->wpos.load() - r;
    for (int32_t i = 0; i < n; i++) {
        if ((uint32_t)i < avail) {
            uint32_t k = (r + i) & (RING - 1);
            out[0][i] = p->ring[k * 2] * (1.0f / 32768.0f);
            out[1][i] = p->ring[k * 2 + 1] * (1.0f / 32768.0f);
        } else {
            out[0][i] = out[1][i] = 0.0f;   /* underrun: silence, never wait */
        }
    }
    p->rpos.store(r + ((uint32_t)n < avail ? (uint32_t)n : avail));
}

/* ---- parameters ----------------------------------------------------------- */
static int stepper_count(Plugin *p, int i) { return i == P_PAGE ? page_count(p) : dim_count(p, i); }
static int stepper_index(Plugin *p, int i) { return i == P_PAGE ? p->page : p->dim[i]; }
static void stepper_set(Plugin *p, int i, int v) {
    if (i == P_PAGE) {
        int n = page_count(p);
        p->page = v < 0 ? 0 : v > n - 1 ? n - 1 : v;
    } else {
        set_dim(p, i, v);
    }
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *p = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0;
    std::lock_guard<std::mutex> lk(p->lock);
    switch (PARAMS[i].type) {
    case T_STEPPER: {
        int n = stepper_count(p, i);
        return n > 1 ? (float)stepper_index(p, i) / (n - 1) : 0.0f;
    }
    case T_FLOAT: return (p->gain - PARAMS[i].min) / (PARAMS[i].max - PARAMS[i].min);
    case T_SLOT: return current_result(p) == p->page * SLOTS + (i - P_RESULT_1) ? 1.0f : 0.0f;
    default: return 0.0f;
    }
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *p = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    std::lock_guard<std::mutex> lk(p->lock);
    switch (PARAMS[i].type) {
    case T_STEPPER: {
        /* On an item: select it. Between items: a Q-Link/encoder nudge, step one. */
        int n = stepper_count(p, i), cur = stepper_index(p, i);
        float pos = (v < 0 ? 0 : v > 1 ? 1 : v) * (n - 1);
        if (n < 2) return;
        if (std::fabs(pos - std::round(pos)) < 0.001f) stepper_set(p, i, (int)std::lround(pos));
        else stepper_set(p, i, cur + (pos > cur ? 1 : -1));
        return;
    }
    case T_FLOAT: {
        char s[32];
        p->gain = PARAMS[i].min + (PARAMS[i].max - PARAMS[i].min) * (v < 0 ? 0 : v > 1 ? 1 : v);
        std::snprintf(s, sizeof s, "%.3f", p->gain);
        p->api->set_param(p->core, "gain", s);
        return;
    }
    case T_SLOT:
        if (v > 0.5f) {
            int idx = p->page * SLOTS + (i - P_RESULT_1);
            if (idx < result_count(p)) play_result(p, idx);
        }
        return;
    case T_TRIGGER: break;
    default: return;
    }
    if (v <= 0.5f) return;
    p->release[i] = 1;
    if (i >= P_GENRE_PREV && i <= P_COUNTRY_NEXT) {
        int d = (i - P_GENRE_PREV) / 2, dir = ((i - P_GENRE_PREV) % 2) ? 1 : -1;
        set_dim(p, d, p->dim[d] + dir);
    } else if (i == P_PAGE_PREV || i == P_PAGE_NEXT) {
        stepper_set(p, P_PAGE, p->page + (i == P_PAGE_NEXT ? 1 : -1));
    } else if (i == P_SEARCH) {
        do_search(p);
    } else if (i == P_PLAY_PAUSE) {
        /* Like the host: with nothing loaded, play the highlighted result. */
        if (core_get(p, "stream_url").empty()) play_result(p, std::atoi(core_get(p, "cratedig_result_index").c_str()));
        else p->api->set_param(p->core, "play_pause_step", "trigger");
    } else if (i == P_STOP) {
        p->api->set_param(p->core, "stop_step", "trigger");
    } else if (i == P_REWIND_15) {
        p->api->set_param(p->core, "rewind_15_step", "trigger");
    } else if (i == P_FORWARD_15) {
        p->api->set_param(p->core, "forward_15_step", "trigger");
    }
}

static std::string display(Plugin *p, int i) {
    switch (PARAMS[i].type) {
    case T_STEPPER:
        if (i == P_PAGE) {
            char s[32];
            std::snprintf(s, sizeof s, "PAGE %d / %d", p->page + 1, page_count(p));
            return s;
        } else {
            const char *v = dim_value(p, i);
            return v[0] ? upper(v) : std::string("ANY");
        }
    case T_FLOAT: {
        char s[16];
        std::snprintf(s, sizeof s, "%.2f", p->gain);
        return s;
    }
    case T_SLOT: {
        int idx = p->page * SLOTS + (i - P_RESULT_1);
        return idx < result_count(p) ? result_label(p, idx) : std::string();
    }
    case T_READOUT:
        if (i == P_STATUS) return upper(core_get(p, "stream_status"));
        if (i == P_TIME) return core_get(p, "playback_time");
        if (i == P_SEARCH_STATUS) return upper(core_get(p, "search_status"));
        if (i == P_NOW_PLAYING) {
            int c = current_result(p);
            if (c < 0) return "";
            std::string t = core_get_idx(p, "search_result_title_%d", c), ch = core_get_idx(p, "search_result_channel_%d", c);
            return ch.empty() ? t : t + " - " + ch;
        }
        return "";
    default:
        return "";
    }
}

/* ---- module dir: where bin/yt-dlp, bin/python3, bin/ffmpeg live ---------- */
static bool has_bin(const std::string &d) {
    struct stat sb;
    return !d.empty() && stat((d + "/bin/yt-dlp").c_str(), &sb) == 0;
}
static std::string find_module_dir() {
    Dl_info info;
    std::string so_dir;
    if (dladdr((void *)&find_module_dir, &info) && info.dli_fname) {
        so_dir = info.dli_fname;
        so_dir = so_dir.substr(0, so_dir.rfind('/'));
    }
    if (has_bin(so_dir + "/cratedigger")) return so_dir + "/cratedigger";
    if (FILE *f = std::fopen((so_dir + "/cratedigger_module_dir.txt").c_str(), "r")) {
        char line[512] = {0};
        if (std::fgets(line, sizeof line, f)) line[std::strcspn(line, "\r\n")] = 0;
        std::fclose(f);
        if (has_bin(line)) return line;
    }
    glob_t g;
    std::string found;
    if (glob("/media/*/AddOns/ForceCrateDigger", 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && found.empty(); i++)
            if (has_bin(g.gl_pathv[i])) found = g.gl_pathv[i];
        globfree(&g);
    }
    return found;
}

/* ---- dispatcher ----------------------------------------------------------- */
static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *ptr, float opt) {
    Plugin *p = (Plugin *)e->object;
    (void)opt;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        p->running = false;
        if (p->worker.joinable()) p->worker.join();
        p->api->destroy_instance(p->core);
        delete p;
        return 1;
    case effGetPlugCategory: return 2;
    case effGetEffectName: case effGetProductString: copy_str(ptr, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(ptr, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && PARAMS[idx].type != T_READOUT;
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(ptr, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel: copy_str(ptr, "", 8); return 1;
    case effGetParamDisplay:
        if (idx >= 0 && idx < NPARAMS) {
            std::lock_guard<std::mutex> lk(p->lock);
            copy_str(ptr, display(p, idx), 100);
        }
        return 1;
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: return 1;   /* no MIDI (yet) */
    case effCanDo: return -1;
    case effGetChunk: {
        std::lock_guard<std::mutex> lk(p->lock);
        std::snprintf(p->chunk, sizeof p->chunk, "g=%d;s=%d;d=%d;r=%d;c=%d;gain=%.3f",
                      p->dim[0], p->dim[1], p->dim[2], p->dim[3], p->dim[4], p->gain);
        *(void **)ptr = p->chunk;
        return (intptr_t)std::strlen(p->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || v > (intptr_t)sizeof p->chunk) return 0;
        char s[256];
        std::memcpy(s, ptr, (size_t)v);
        s[v - 1] = 0;
        int g = 0, st = 0, d = 0, r = 0, c = 0;
        float gn = 1.0f;
        std::sscanf(s, "g=%d;s=%d;d=%d;r=%d;c=%d;gain=%f", &g, &st, &d, &r, &c, &gn);
        std::lock_guard<std::mutex> lk(p->lock);
        set_dim(p, DIM_GENRE, g); set_dim(p, DIM_STYLE, st); set_dim(p, DIM_DECADE, d);
        set_dim(p, DIM_REGION, r); set_dim(p, DIM_COUNTRY, c);
        char gs[32];
        p->gain = gn;
        std::snprintf(gs, sizeof gs, "%.3f", gn);
        p->api->set_param(p->core, "gain", gs);
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/cratedigger_vst.log", "a");
    /* A write to a dead child's pipe must not kill MPC. Only touch SIGPIPE if the host
     * left it at the default (children get the default back via posix_spawn). */
    struct sigaction sa;
    if (sigaction(SIGPIPE, nullptr, &sa) == 0 && sa.sa_handler == SIG_DFL) {
        signal(SIGPIPE, SIG_IGN);
        LOG("SIGPIPE was default in host; now ignored\n");
    }
    std::string dir = find_module_dir();
    LOG("VSTPluginMain: module dir '%s'\n", dir.c_str());
    if (dir.empty()) return nullptr;   /* no engine bundle: refuse to load rather than half-work */

    plugin_api_v2_t *api = move_plugin_init_v2(nullptr);
    if (!api) return nullptr;
    Plugin *p = new Plugin();
    p->api = api;
    p->master = master;
    std::memset((void *)p->release, 0, sizeof p->release);
    p->core = api->create_instance(dir.c_str(), nullptr);
    if (!p->core) { delete p; return nullptr; }
    api->set_param(p->core, "search_provider", "cratedig");
    api->set_param(p->core, "gain", "1.000");

    AEffect *e = &p->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = p;

    p->running = true;
    p->worker = std::thread(worker_main, p);
    return e;
}
