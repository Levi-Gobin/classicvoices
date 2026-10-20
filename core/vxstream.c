#include "vxstream.h"
#include "vxengine.h"
#include "mt/mtengine.h"

#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING_FRAMES (1u << 20)  // ~47 s at 22.05 kHz
#ifndef MAX_ENGINES
#define MAX_ENGINES 3
#endif

// Recompiled engine code (build/gen*/) is linked in when available; each table
// is weak so a build without it falls back to the interpreter.
#include "thumb.h"
#define NATIVE_TABLE(b) \
    extern const NativeEntry vx_native_##b[] __attribute__((weak)); \
    extern const size_t vx_native_##b##_count __attribute__((weak));
NATIVE_TABLE(ios3)
NATIVE_TABLE(ios4)
NATIVE_TABLE(ios5)
NATIVE_TABLE(ios6)
NATIVE_TABLE(ios7)
NATIVE_TABLE(ios10)

static void install_native(Vx *vx, const char *build) {
#ifdef VX_DYNAMIC_NATIVE
    // Windows: recompiled code arrives as per-firmware DLLs built on the
    // user's machine (nvda/build_native.py) rather than linked in.
    extern int vx_try_native_dll(Vx *vx, const char *build);
    if (vx_try_native_dll(vx, build)) return;
#endif
    struct { const char *name; const NativeEntry *t; const size_t *n; } tabs[] = {
        {"ios3", vx_native_ios3, &vx_native_ios3_count}, {"ios4", vx_native_ios4, &vx_native_ios4_count},
        {"ios5", vx_native_ios5, &vx_native_ios5_count}, {"ios6", vx_native_ios6, &vx_native_ios6_count},
        {"ios7", vx_native_ios7, &vx_native_ios7_count}, {"ios10", vx_native_ios10, &vx_native_ios10_count},
    };
    for (size_t i = 0; i < sizeof tabs / sizeof tabs[0]; i++)
        if (strcmp(build, tabs[i].name) == 0 && tabs[i].t && tabs[i].n)
            vx_set_native(vx, tabs[i].t, *tabs[i].n);
}

#define OUT_RATE 22050

typedef struct {
    char key[96];  // "<build>/<lang>" ("ios3/en-US") or "<mac era>/<voice>" ("mac107/KarenCompact")
    Vx *vx;
    uint64_t last_used;
    int failures;  // utterances in a row the engine refused
} EngineSlot;

struct VxStream {
    char root[1024];        // holds <build>/voiced and <build>/TTSResources/<lang>
    char asset_root[1024];  // downloaded (enhanced) voices: <build>/<lang>/broker.hdr.asset

    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int quit;

    // pending job (guarded by mu)
    int has_job;
    char job_lang[96];
    char job_fallback[96];  // compact voice for pitch changes (see speak_segmented), or empty
    char *job_text;
    int job_rate, job_pitch, job_volume, job_pause;
    char preload_lang[96];

    // worker state
    EngineSlot engines[MAX_ENGINES];
    uint64_t tick;
    MtEngine *mt;      // MacinTalk: one generation loaded at a time
    char mt_gen[16];   // e.g. "mt105"

    // ring buffer: worker writes, render thread reads
    float *ring;
    _Atomic uint64_t write_pos, read_pos;
    _Atomic int producing;   // worker is still generating the current utterance
    _Atomic int cancel;      // ask the worker to stop the current utterance
    _Atomic uint64_t generation;

    double first_ms, synth_ms, audio_ms;
    char err[256];
};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

// Requested pitch (percent of normal) -> engine pitch (1..100, 50 = normal).
// Measured F0 ratios for engine values; interpolate inversely.
int vxs_engine_pitch(int percent) {
    static const double val[] = {1, 25, 50, 60, 75, 100};
    static const double ratio[] = {0.557, 0.760, 1.000, 1.130, 1.304, 1.581};
    double r = percent / 100.0;
    if (r <= ratio[0]) return 1;
    for (int i = 1; i < 6; i++)
        if (r <= ratio[i]) return (int)(val[i - 1] + (r - ratio[i - 1]) / (ratio[i] - ratio[i - 1]) * (val[i] - val[i - 1]) + 0.5);
    return 100;
}

// Which firmware's engine runs a voice key. Mac OS X voices use the iOS engine
// of the same Vocalizer generation: Lion and Mountain Lion voices are 5.2.3
// bet2 (iOS 6); Mavericks compacts are bet3 with the newer pipeline (iOS 7);
// Mavericks premium voices also need the vssq5 decoder, which iOS 7 dropped,
// so they run on iOS 10's engine. Writes e.g. "ios6" into out.
static void engine_build(const char *key, char *out, size_t cap) {
    const char *slash = strchr(key, '/');
    size_t bl = slash ? (size_t)(slash - key) : strlen(key);
    if (strncmp(key, "mac109/", 7) == 0) {
        size_t n = strlen(key);
        int compact = n >= 7 && strcmp(key + n - 7, "Compact") == 0;
        snprintf(out, cap, "%s", compact ? "ios7" : "ios10");
    } else if (strncmp(key, "mac", 3) == 0) {
        snprintf(out, cap, "ios6");
    } else {
        if (bl && key[bl - 1] == 'e') bl--;  // "ios6e" = enhanced voice for the iOS 6 engine
        snprintf(out, cap, "%.*s", (int)bl, key);
    }
}

// Voice keys: "ios6/en-US" is a built-in voice; "ios6e/en-US" is a downloaded
// enhanced voice for that firmware's engine. "mac107/KarenCompact" is a Mac
// OS X voice: <root>/mac107/KarenCompact if bundled, else downloaded into
// <asset_root>/mac107/KarenCompact; its common data is <root>/mac107/common.
static Vx *open_engine(const char *root, const char *asset_root, const char *key, char *err, size_t errlen) {
    char build[16], lang[80];
    const char *slash = strchr(key, '/');
    if (!slash || slash - key >= (long)sizeof build || strchr(slash + 1, '/') || strstr(key, "..")) {
        snprintf(err, errlen, "bad voice key %s", key);
        return NULL;
    }
    snprintf(build, sizeof build, "%.*s", (int)(slash - key), key);
    snprintf(lang, sizeof lang, "%s", slash + 1);
    char voiced[1100], ldir[1200], cdir[1100];
    if (strncmp(build, "mac", 3) == 0) {
        char eng[16];
        engine_build(key, eng, sizeof eng);
        snprintf(voiced, sizeof voiced, "%s/%s/voiced", root, eng);
        snprintf(ldir, sizeof ldir, "%s/%s/%s/broker.hdr", root, build, lang);
        if (access(ldir, R_OK) == 0) snprintf(ldir, sizeof ldir, "%s/%s/%s", root, build, lang);
        else snprintf(ldir, sizeof ldir, "%s/%s/%s", asset_root, build, lang);
        snprintf(cdir, sizeof cdir, "%s/%s/common", root, build);
        Vx *vx = vx_open(voiced, ldir, cdir, err, errlen);
        if (vx) install_native(vx, eng);
        return vx;
    }
    size_t bl = strlen(build);
    int enhanced = bl == 5 && build[4] == 'e';
    if (enhanced) build[4] = 0;
    snprintf(voiced, sizeof voiced, "%s/%s/voiced", root, build);
    if (enhanced) snprintf(ldir, sizeof ldir, "%s/%s/%s", asset_root, build, lang);
    else snprintf(ldir, sizeof ldir, "%s/%s/TTSResources/%s", root, build, lang);
    snprintf(cdir, sizeof cdir, "%s/%s/TTSResources/common", root, build);
    Vx *vx = vx_open(voiced, ldir, cdir, err, errlen);
    if (vx) install_native(vx, build);
    return vx;
}

// The engine for a voice key, opened if need be. `keep` names an engine that
// must stay open (the voice being spoken while its compact fallback loads);
// with evict == 0 nothing else is closed to make room either.
static Vx *engine_for_opt(VxStream *s, const char *key, int evict, const char *keep) {
    EngineSlot *lru = NULL;
    for (int i = 0; i < MAX_ENGINES; i++) {
        EngineSlot *e = &s->engines[i];
        if (e->vx && strcmp(e->key, key) == 0) { e->last_used = ++s->tick; return e->vx; }
        if (e->vx && keep && strcmp(e->key, keep) == 0) continue;
        if (!lru || !e->vx || (lru->vx && e->last_used < lru->last_used)) lru = e;
    }
    if (!lru) return NULL;
    if (lru->vx) { vx_close(lru->vx); lru->vx = NULL; lru->key[0] = 0; }
    Vx *vx = open_engine(s->root, s->asset_root, key, s->err, sizeof s->err);
    if (!vx) {
        if (!evict) return NULL;
        // Devices with 3 GB of RAM let a process map only ~1.3 GB: one
        // Vocalizer engine (1 GB) at a time, and no MacinTalk beside it. Make
        // room and try once more.
        int freed = 0;
        for (int i = 0; i < MAX_ENGINES; i++) {
            if (!s->engines[i].vx || (keep && strcmp(s->engines[i].key, keep) == 0)) continue;
            vx_close(s->engines[i].vx); s->engines[i].vx = NULL; s->engines[i].key[0] = 0; freed = 1;
        }
        if (s->mt) { mte_close(s->mt); s->mt = NULL; s->mt_gen[0] = 0; freed = 1; }
        if (!freed) return NULL;
        os_log_info(OS_LOG_DEFAULT, "CLASSICVOICES %{public}s: %{public}s; retrying with the other engines closed", key, s->err);
        vx = open_engine(s->root, s->asset_root, key, s->err, sizeof s->err);
        if (!vx) return NULL;
    }
    snprintf(lru->key, sizeof lru->key, "%s", key);
    lru->vx = vx;
    lru->failures = 0;
    lru->last_used = ++s->tick;
    return vx;
}

static Vx *engine_for(VxStream *s, const char *key) { return engine_for_opt(s, key, 1, NULL); }

// ---- resampling (iOS 3 voices are 16 kHz; the audio unit runs at 22.05 kHz)
// Windowed-sinc interpolation, 32 taps, 512 sub-sample phases.
#define RS_HALF 16
#define RS_TAPS (2 * RS_HALF)
#define RS_PHASES 512
static float rs_table[RS_PHASES + 1][RS_TAPS];
static pthread_once_t rs_once = PTHREAD_ONCE_INIT;
static void rs_init(void) {
    for (int p = 0; p <= RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES;
        for (int j = 0; j < RS_TAPS; j++) {
            double x = frac - (j - (RS_HALF - 1));  // distance from input sample i0 + j - 15
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
            double wx = x / RS_HALF;  // Blackman window over |x| <= RS_HALF
            double win = fabs(wx) >= 1 ? 0 : 0.42 + 0.5 * cos(M_PI * wx) + 0.08 * cos(2 * M_PI * wx);
            rs_table[p][j] = (float)(sinc * win);
        }
    }
}

// Silence trimming: the engine pads every utterance with ~40-110 ms of
// leading and ~200-250 ms of trailing silence. We drop the leading run (keeping
// a short preroll so onsets aren't clipped) and cap the trailing run, then add
// any explicit trailing pause the request asked for.
#define QUIET_LEVEL 160                 // |sample| below this counts as silence
#define PREROLL_FRAMES 110              // ~5 ms kept before the first sound
#define TAIL_KEEP_FRAMES 660            // ~30 ms kept after the last sound
#define QUIET_FLUSH_FRAMES (22050 * 2)  // don't hold more than 2 s of silence

typedef struct {
    VxStream *s;
    double t0;
    int got_first;   // first audible sample seen
    uint64_t frames; // frames pushed to the ring
    int16_t *quiet;  // pending silent run (or leading preroll window)
    size_t nquiet, capquiet;
    int rate;        // engine sample rate
    float *rin;      // resampler input history
    size_t nrin, caprin;
    double rpos;     // next output position, in input samples relative to rin[0]
    size_t cap_quiet; // if nonzero, the next silent run is shortened to this (segment joins)
    // diagnostics: silent runs (>= 40 ms) inside the output, as heard
    uint64_t out_pos, quiet_out, heard;
    char gaplog[256];
} Sink;

// Write output-rate samples to the ring buffer. Returns 1 if cancelled.
static int ring_write(Sink *k, const float *out, size_t n) {
    VxStream *s = k->s;
    size_t i = 0;
    while (i < n) {
        if (atomic_load_explicit(&s->cancel, memory_order_relaxed)) return 1;
        uint64_t w = atomic_load_explicit(&s->write_pos, memory_order_relaxed);
        uint64_t r = atomic_load_explicit(&s->read_pos, memory_order_acquire);
        size_t space = RING_FRAMES - (size_t)(w - r);
        if (space == 0) {  // reader is behind: wait for playback to catch up
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
            continue;
        }
        size_t chunk = n - i < space ? n - i : space;
        for (size_t j = 0; j < chunk; j++) {
            float v = out ? out[i + j] : 0.0f;
            s->ring[(w + j) & (RING_FRAMES - 1)] = v;
            if (fabsf(v) < 0.003f) {
                k->quiet_out++;
            } else {
                if (k->heard && k->quiet_out >= OUT_RATE / 25) {
                    size_t l = strlen(k->gaplog);
                    snprintf(k->gaplog + l, sizeof k->gaplog - l, " %llums@%llums",
                             (unsigned long long)(k->quiet_out * 1000 / OUT_RATE),
                             (unsigned long long)((k->out_pos - k->quiet_out) * 1000 / OUT_RATE));
                }
                k->quiet_out = 0;
                k->heard = 1;
            }
            k->out_pos++;
        }
        atomic_store_explicit(&s->write_pos, w + chunk, memory_order_release);
        i += chunk;
    }
    k->frames += n;
    return 0;
}

// Resample everything that has enough lookahead (or all of it when flushing).
static int resample_run(Sink *k, int flush) {
    if (flush) {
        for (int i = 0; i < RS_HALF + 1; i++) {
            if (k->nrin == k->caprin) { k->caprin = k->caprin * 2 + 64; k->rin = realloc(k->rin, k->caprin * sizeof(float)); }
            k->rin[k->nrin++] = 0;
        }
    }
    const double step = (double)k->rate / OUT_RATE;
    float out[512];
    size_t nout = 0;
    while ((size_t)k->rpos + RS_HALF + 1 < k->nrin) {
        size_t i0 = (size_t)k->rpos;
        double frac = k->rpos - i0;
        int ph = (int)(frac * RS_PHASES + 0.5);
        const float *h = rs_table[ph], *x = k->rin + i0 - (RS_HALF - 1);
        float acc = 0;
        for (int j = 0; j < RS_TAPS; j++) acc += x[j] * h[j];
        out[nout++] = acc;
        if (nout == 512) { if (ring_write(k, out, nout)) return 1; nout = 0; }
        k->rpos += step;
    }
    if (nout && ring_write(k, out, nout)) return 1;
    size_t keep_from = (size_t)k->rpos - (RS_HALF - 1);  // drop consumed history
    if (keep_from > 0 && keep_from <= k->nrin) {
        memmove(k->rin, k->rin + keep_from, (k->nrin - keep_from) * sizeof(float));
        k->nrin -= keep_from;
        k->rpos -= keep_from;
    }
    return 0;
}

// Push engine-rate samples (NULL = silence). Returns 1 if cancelled.
static int push(Sink *k, const int16_t *pcm, size_t n) {
    if (k->rate == OUT_RATE) {
        float buf[1024];
        for (size_t i = 0; i < n;) {
            size_t c = n - i < 1024 ? n - i : 1024;
            for (size_t j = 0; j < c; j++) buf[j] = pcm ? pcm[i + j] * (1.0f / 32768.0f) : 0.0f;
            if (ring_write(k, buf, c)) return 1;
            i += c;
        }
        return 0;
    }
    if (!k->rin) {  // start with RS_HALF-1 zeros of history so output 0 aligns with input 0
        pthread_once(&rs_once, rs_init);
        k->caprin = 8192;
        k->rin = calloc(k->caprin, sizeof(float));
        k->nrin = RS_HALF - 1;
        k->rpos = RS_HALF - 1;
    }
    if (k->nrin + n > k->caprin) {
        while (k->nrin + n > k->caprin) k->caprin *= 2;
        k->rin = realloc(k->rin, k->caprin * sizeof(float));
    }
    for (size_t i = 0; i < n; i++) k->rin[k->nrin++] = pcm ? pcm[i] * (1.0f / 32768.0f) : 0.0f;
    return resample_run(k, 0);
}

static void hold_quiet(Sink *k, int16_t v) {
    if (k->nquiet == k->capquiet) {
        k->capquiet = k->capquiet ? k->capquiet * 2 : 4096;
        k->quiet = realloc(k->quiet, k->capquiet * sizeof(int16_t));
    }
    k->quiet[k->nquiet++] = v;
}

static int on_pcm(void *ctx, const int16_t *pcm, size_t n) {
    Sink *k = ctx;
    VxStream *s = k->s;
    for (size_t i = 0; i < n; i++) {
        int loud = pcm[i] > QUIET_LEVEL || pcm[i] < -QUIET_LEVEL;
        if (!k->got_first) {
            if (!loud) {
                hold_quiet(k, pcm[i]);
                if (k->nquiet > PREROLL_FRAMES * 4) {  // keep only the last preroll window
                    memmove(k->quiet, k->quiet + k->nquiet - PREROLL_FRAMES, PREROLL_FRAMES * sizeof(int16_t));
                    k->nquiet = PREROLL_FRAMES;
                }
                continue;
            }
            k->got_first = 1;
            s->first_ms = now_ms() - k->t0;
            size_t pre = k->nquiet < PREROLL_FRAMES ? k->nquiet : PREROLL_FRAMES;
            if (push(k, k->quiet + k->nquiet - pre, pre)) return 1;
            k->nquiet = 0;
        }
        if (!loud) {
            hold_quiet(k, pcm[i]);
            if (k->nquiet >= QUIET_FLUSH_FRAMES) {
                if (push(k, k->quiet, k->nquiet)) return 1;
                k->nquiet = 0;
            }
            continue;
        }
        if (k->nquiet) {
            size_t keep = k->nquiet;
            if (k->cap_quiet && keep > k->cap_quiet) keep = k->cap_quiet;
            k->cap_quiet = 0;
            if (push(k, k->quiet + k->nquiet - keep, keep)) return 1;
            k->nquiet = 0;
        }
        size_t j = i;  // push the run of loud-or-short-quiet samples in one go
        while (j < n && (pcm[j] > QUIET_LEVEL || pcm[j] < -QUIET_LEVEL)) j++;
        if (push(k, pcm + i, j - i)) return 1;
        i = j - 1;
    }
    return atomic_load_explicit(&s->cancel, memory_order_relaxed) ? 1 : 0;
}

// End of utterance: keep a short tail, then any requested trailing pause.
static void finish(Sink *k, int pause_ms) {
    if (k->got_first) push(k, k->quiet, k->nquiet < TAIL_KEEP_FRAMES ? k->nquiet : TAIL_KEEP_FRAMES);
    if (pause_ms > 0) push(k, NULL, (size_t)pause_ms * (size_t)k->rate / 1000);
    if (k->rin) resample_run(k, 1);
    free(k->quiet);
    free(k->rin);
}

// Speak the way the original VoiceOver did (verified in iOS 3.1.3's and 6.1.6's
// VoiceOverTouch). Its speech component (`_handlePerformAction:`) sent every
// piece of an element's description -- label, value, each trait, each added
// with its own addString: -- to the engine as a separate utterance, so the
// break between "Settings" and "Active" is the engine's own end-of-utterance
// silence and each piece gets utterance-final intonation. So each ESC\eos=1\
// boundary from VoiceOver's <s> elements becomes its own engine call with the
// natural silence between them kept (no punctuation is added).
// Engines that understand ESC\pitch\ (iOS 6) keep it inline; older ones
// ignore it, so a pitch change starts a new call, joined with <= 100 ms.
// The iOS 7 and 10 engines take pitch as percent of normal (50-200,
// 100 = normal; measured linear in F0) instead of 1-100 with 50 = normal.
// Text arrives with the 1-100 scale; convert an engine value to percent.
static int pitch_value_to_percent(int v) {
    static const double val[] = {1, 25, 50, 60, 75, 100};
    static const double ratio[] = {0.557, 0.760, 1.000, 1.130, 1.304, 1.581};
    if (v <= 1) return 56;
    for (int i = 1; i < 6; i++)
        if (v <= val[i])
            return (int)(100 * (ratio[i - 1] + (v - val[i - 1]) / (val[i] - val[i - 1]) * (ratio[i] - ratio[i - 1])) + 0.5);
    return 158;
}

// With a fallback key, every segment whose pitch isn't normal (engine value
// 50) is spoken by that voice instead: VoiceOver on iOS 5 and 6 did this,
// since an enhanced voice can't change pitch (the compact one can). The
// fallback engine must share the primary's sample rate; it loads when first
// needed, and warm_fallback() keeps it ready for the next time.
static int speak_segmented(VxStream *s, Vx *vx, const char *primary_key, const char *fallback_key, int rate, int volume,
                           Sink *k, const char *text, int pitch, int inline_pitch, int percent_pitch) {
    static const char kEos[] = "\x1b\\eos=1\\", kPitch[] = "\x1b\\pitch=";
    // The segment buffer grows as needed: rescaled pitch commands (below) can
    // be longer than the ones in the text.
    size_t cap = strlen(text) + 1;
    char *seg = malloc(cap);
    size_t m = 0;
    int rc = 0;
    const char *p = text;
    Vx *fb = NULL;
    int fb_tried = 0;
    for (;;) {
        int at_end = *p == 0;
        int is_eos = !at_end && strncmp(p, kEos, sizeof kEos - 1) == 0;
        int is_pitch = !inline_pitch && !at_end && strncmp(p, kPitch, sizeof kPitch - 1) == 0;
        if (at_end || is_eos || is_pitch) {
            seg[m] = 0;
            int has_text = 0;
            for (size_t i = 0; i < m; i++) if (seg[i] != ' ') { has_text = 1; break; }
            if (has_text) {
                Vx *use = vx;
                if (fallback_key && pitch != 50) {
                    if (!fb && !fb_tried) {
                        fb_tried = 1;
                        fb = engine_for_opt(s, fallback_key, 1, primary_key);
                        if (fb && vx_sample_rate(fb) != vx_sample_rate(vx)) fb = NULL;
                        if (fb) { vx_set_param(fb, 3, (uint32_t)rate); vx_set_param(fb, 2, (uint32_t)volume); }
                        else os_log_info(OS_LOG_DEFAULT, "CLASSICVOICES no compact fallback %{public}s for %{public}s: %{public}s",
                                         fallback_key, primary_key, s->err);
                    }
                    if (fb) use = fb;
                }
                int prc = vx_set_param(use, 4, (uint32_t)(percent_pitch ? pitch_value_to_percent(pitch) : pitch));
                if (getenv("VX_SEGDEBUG")) fprintf(stderr, "segment pitch=%d rc=%x %s [%s]\n", pitch, prc, use == fb ? "(fallback)" : "", seg);
                if ((rc = vx_speak_utf8(use, seg, on_pcm, k))) break;
                // between utterances keep the engine's natural silence; inside a
                // sentence (pitch change only) join tightly
                k->cap_quiet = is_pitch ? (size_t)k->rate / 10 : 0;
            }
            m = 0;
            if (at_end) break;
            if (is_eos) { p += sizeof kEos - 1; continue; }
            p += sizeof kPitch - 1;
            pitch = atoi(p);
            while (*p && *p != '\\') p++;
            if (*p) p++;
            continue;
        }
        if (percent_pitch && strncmp(p, kPitch, sizeof kPitch - 1) == 0) {  // inline, rescaled
            p += sizeof kPitch - 1;
            int v = atoi(p);
            while (*p && *p != '\\') p++;
            if (*p) p++;
            char cmd[32];
            int len = snprintf(cmd, sizeof cmd, "%s%d\\", kPitch, pitch_value_to_percent(v));
            if (m + (size_t)len + 1 > cap) { cap = (m + (size_t)len + 1) * 2; seg = realloc(seg, cap); }
            memcpy(seg + m, cmd, (size_t)len);
            m += (size_t)len;
            continue;
        }
        if (m + 2 > cap) { cap *= 2; seg = realloc(seg, cap); }
        seg[m++] = *p++;
    }
    free(seg);
    return rc;
}

// ---- MacinTalk (Mac OS X 10.4-10.6): keys "mt104/Fred", "mt105/Alex" ...
static int is_mt_key(const char *key) { return key[0] == 'm' && key[1] == 't' && key[2] >= '0' && key[2] <= '9'; }

// A MacinTalk engine comes with the app (<root>/<gen>/MacinTalk) or was
// added from the user's own Mac OS X files (<asset_root>/<gen>/MacinTalk).
static void mt_engine_dir(const char *root, const char *asset_root, const char *gen, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s/MacinTalk", root, gen);
    if (access(out, R_OK) != 0 && asset_root) snprintf(out, cap, "%s/%s", asset_root, gen);
    else snprintf(out, cap, "%s/%s", root, gen);
}

static MtEngine *mt_for(VxStream *s, const char *key) {
    const char *slash = strchr(key, '/');
    if (!slash) return NULL;
    char gen[16];
    snprintf(gen, sizeof gen, "%.*s", (int)(slash - key), key);
    if (s->mt && !strcmp(s->mt_gen, gen) && !mte_broken(s->mt)) return s->mt;
    if (s->mt) { mte_close(s->mt); s->mt = NULL; s->mt_gen[0] = 0; }
    // A MacinTalk engine reserves 2 GB of address space; an audio-unit
    // extension has little to spare, so release the cached Vocalizer engines
    // (1 GB each, reopened in milliseconds when next needed).
    for (int i = 0; i < MAX_ENGINES; i++)
        if (s->engines[i].vx) { vx_close(s->engines[i].vx); s->engines[i].vx = NULL; s->engines[i].key[0] = 0; }
    char root[1100];
    mt_engine_dir(s->root, s->asset_root, gen, root, sizeof root);
    s->mt = mte_open(root, s->err, sizeof s->err);
    if (s->mt) {
        snprintf(s->mt_gen, sizeof s->mt_gen, "%s", gen);
        char extra[1100];  // voices copied onto the device: <asset_root>/<gen>/Voices
        snprintf(extra, sizeof extra, "%s/%s/Voices", s->asset_root, gen);
        mte_set_extra_voices(s->mt, extra);
    }
    else os_log_error(OS_LOG_DEFAULT, "CLASSICVOICES MacinTalk %{public}s: %{public}s", gen, s->err);
    return s->mt;
}

static int mt_pcm(void *ctx, const float *pcm, size_t n) {
    int16_t buf[1024];
    for (size_t i = 0; i < n;) {
        size_t c = n - i < 1024 ? n - i : 1024;
        for (size_t j = 0; j < c; j++) {
            float v = pcm[i + j] * 32767.0f;
            buf[j] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        if (on_pcm(ctx, buf, c)) return 1;
        i += c;
    }
    return 0;
}

static void *worker(void *arg) {
    VxStream *s = arg;
    pthread_mutex_lock(&s->mu);
    for (;;) {
        while (!s->quit && !s->has_job && !s->preload_lang[0]) pthread_cond_wait(&s->cv, &s->mu);
        if (s->quit) break;
        if (s->preload_lang[0] && !s->has_job) {
            char lang[96];
            snprintf(lang, sizeof lang, "%s", s->preload_lang);
            s->preload_lang[0] = 0;
            pthread_mutex_unlock(&s->mu);
            if (is_mt_key(lang)) mt_for(s, lang); else engine_for(s, lang);
            pthread_mutex_lock(&s->mu);
            continue;
        }
        char lang[96], fallback[96];
        snprintf(lang, sizeof lang, "%s", s->job_lang);
        snprintf(fallback, sizeof fallback, "%s", s->job_fallback);
        char *text = s->job_text;
        int rate = s->job_rate, pitch = s->job_pitch, volume = s->job_volume, pause = s->job_pause;
        s->job_text = NULL;
        s->has_job = 0;
        pthread_mutex_unlock(&s->mu);

        Sink k = {s, now_ms(), 0, 0, NULL, 0, 0, OUT_RATE, NULL, 0, 0, 0, 0, 0, 0, 0, {0}};
        if (is_mt_key(lang)) {
            MtEngine *mt = mt_for(s, lang);
            if (mt && !atomic_load(&s->cancel)) {
                const char *voice = strchr(lang, '/') + 1;
                if (mte_speak(mt, voice, text, rate, pitch, volume, mt_pcm, &k) < 0) {
                    snprintf(s->err, sizeof s->err, "%s", mte_error(mt));
                    os_log_error(OS_LOG_DEFAULT, "CLASSICVOICES MacinTalk speak failed: %{public}s", s->err);
                }
                // It shut itself down after a fault or a hang: the next
                // utterance opens a fresh one rather than every voice going quiet.
                if (mte_broken(mt)) {
                    os_log_error(OS_LOG_DEFAULT, "CLASSICVOICES MacinTalk %{public}s restarting", s->mt_gen);
                    mte_close(mt);
                    s->mt = NULL;
                    s->mt_gen[0] = 0;
                }
            }
            if (!atomic_load(&s->cancel)) finish(&k, pause);
            else { free(k.quiet); free(k.rin); }
            os_log(OS_LOG_DEFAULT, "CLASSICVOICES output %{public}s: %llums, gaps:%{public}s, tail %llums", lang,
                   (unsigned long long)(k.out_pos * 1000 / OUT_RATE), k.gaplog[0] ? k.gaplog : " none",
                   (unsigned long long)(k.quiet_out * 1000 / OUT_RATE));
            s->synth_ms = now_ms() - k.t0;
            s->audio_ms = k.frames * 1000.0 / OUT_RATE;
            free(text);
            pthread_mutex_lock(&s->mu);
            atomic_store(&s->producing, 0);
            pthread_cond_broadcast(&s->cv);
            continue;
        }
        Vx *vx = engine_for(s, lang);
        if (vx) k.rate = vx_sample_rate(vx);
        if (vx && !atomic_load(&s->cancel)) {
            vx_set_param(vx, 3, (uint32_t)rate);
            vx_set_param(vx, 2, (uint32_t)volume);
            // iOS 5-7 engines (Vocalizer 5.2.3) understand inline pitch commands
            char eb[16];
            engine_build(lang, eb, sizeof eb);
            int percent = strcmp(eb, "ios7") == 0 || strcmp(eb, "ios10") == 0;
            // With a fallback voice, pitch commands split the text so each
            // piece can go to the right engine.
            int inline_pitch = (percent || strcmp(eb, "ios5") == 0 || strcmp(eb, "ios6") == 0) && !fallback[0];
            int rc = speak_segmented(s, vx, lang, fallback[0] ? fallback : NULL, rate, volume, &k, text,
                                     vxs_engine_pitch(pitch), inline_pitch, percent);
            // One refused phrase is usually the text (e.g. a lone "(" for some
            // voices); two in a row means the engine itself is stuck, so start
            // it again rather than stay silent.
            for (int i = 0; i < MAX_ENGINES; i++) {
                EngineSlot *e = &s->engines[i];
                if (e->vx != vx) continue;
                if (!rc || atomic_load(&s->cancel)) { e->failures = 0; break; }
                if (++e->failures < 2) break;
                os_log_error(OS_LOG_DEFAULT, "CLASSICVOICES %{public}s failed twice (%{public}s); restarting it", lang, vx_last_error(vx));
                vx_close(vx);
                e->vx = NULL;
                e->key[0] = 0;
                break;
            }
        }
        if (!atomic_load(&s->cancel)) finish(&k, pause);
        else { free(k.quiet); free(k.rin); }
        os_log(OS_LOG_DEFAULT, "CLASSICVOICES output %{public}s: %llums, gaps:%{public}s, tail %llums",
               lang, (unsigned long long)(k.out_pos * 1000 / OUT_RATE), k.gaplog[0] ? k.gaplog : " none",
               (unsigned long long)(k.quiet_out * 1000 / OUT_RATE));
        s->synth_ms = now_ms() - k.t0;
        s->audio_ms = k.frames * 1000.0 / OUT_RATE;
        free(text);

        pthread_mutex_lock(&s->mu);
        atomic_store(&s->producing, 0);
        pthread_cond_broadcast(&s->cv);
        if (fallback[0] && !s->has_job) {
            // Have the compact voice ready for the next pitch change, without
            // delaying what was just said (the utterance is finished) and
            // without pushing the voice itself out on devices short of memory.
            pthread_mutex_unlock(&s->mu);
            engine_for_opt(s, fallback, 0, lang);
            pthread_mutex_lock(&s->mu);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

VxStream *vxs_create(const char *root) {
    VxStream *s = calloc(1, sizeof *s);
    snprintf(s->root, sizeof s->root, "%s", root);
    s->ring = calloc(RING_FRAMES, sizeof(float));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);  // recompiled code recurses on the C stack
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_create(&s->thread, &attr, worker, s);
    pthread_attr_destroy(&attr);
    return s;
}

void vxs_destroy(VxStream *s) {
    if (!s) return;
    vxs_cancel(s);
    pthread_mutex_lock(&s->mu);
    s->quit = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    pthread_join(s->thread, NULL);
    for (int i = 0; i < MAX_ENGINES; i++)
        if (s->engines[i].vx) vx_close(s->engines[i].vx);
    if (s->mt) mte_close(s->mt);
    free(s->job_text);
    free(s->ring);
    free(s);
}

void vxs_cancel(VxStream *s) {
    pthread_mutex_lock(&s->mu);
    atomic_store(&s->cancel, 1);
    free(s->job_text);
    s->job_text = NULL;
    if (s->has_job) {  // never picked up by the worker
        s->has_job = 0;
        atomic_store(&s->producing, 0);
    }
    while (atomic_load(&s->producing)) pthread_cond_wait(&s->cv, &s->mu);
    atomic_store(&s->read_pos, atomic_load(&s->write_pos));  // drop unplayed audio
    atomic_store(&s->cancel, 0);
    pthread_mutex_unlock(&s->mu);
}

void vxs_start(VxStream *s, const char *lang, const char *utf8_text, int rate, int pitch, int volume,
               int trailing_pause_ms) {
    vxs_start_with_fallback(s, lang, NULL, utf8_text, rate, pitch, volume, trailing_pause_ms);
}

void vxs_start_with_fallback(VxStream *s, const char *lang, const char *fallback_key, const char *utf8_text,
                             int rate, int pitch, int volume, int trailing_pause_ms) {
    vxs_cancel(s);
    pthread_mutex_lock(&s->mu);
    snprintf(s->job_lang, sizeof s->job_lang, "%s", lang);
    snprintf(s->job_fallback, sizeof s->job_fallback, "%s", fallback_key && !is_mt_key(lang) ? fallback_key : "");
    s->job_text = strdup(utf8_text);
    s->job_rate = rate < 50 ? 50 : rate > 400 ? 400 : rate;
    s->job_pitch = pitch < 50 ? 50 : pitch > 200 ? 200 : pitch;
    s->job_volume = volume < 0 ? 80 : volume > 100 ? 100 : volume;  // 80 = engine default
    s->job_pause = trailing_pause_ms < 0 ? 0 : trailing_pause_ms > 5000 ? 5000 : trailing_pause_ms;
    s->has_job = 1;
    s->first_ms = -1;
    atomic_store(&s->producing, 1);
    atomic_fetch_add(&s->generation, 1);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

void vxs_preload(VxStream *s, const char *lang) {
    pthread_mutex_lock(&s->mu);
    snprintf(s->preload_lang, sizeof s->preload_lang, "%s", lang);
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

size_t vxs_read(VxStream *s, float *out, size_t frames, int *done) {
    uint64_t r = atomic_load_explicit(&s->read_pos, memory_order_relaxed);
    int producing = atomic_load_explicit(&s->producing, memory_order_acquire);
    uint64_t w = atomic_load_explicit(&s->write_pos, memory_order_acquire);
    size_t avail = (size_t)(w - r);
    size_t n = avail < frames ? avail : frames;
    for (size_t i = 0; i < n; i++) out[i] = s->ring[(r + i) & (RING_FRAMES - 1)];
    atomic_store_explicit(&s->read_pos, r + n, memory_order_release);
    if (done) *done = !producing && n == avail;
    return n;
}

double vxs_last_first_audio_ms(VxStream *s) { return s->first_ms; }
double vxs_last_synth_ms(VxStream *s) { return s->synth_ms; }
double vxs_last_audio_ms(VxStream *s) { return s->audio_ms; }
const char *vxs_last_error(VxStream *s) { return s->err; }

typedef struct { double t0, first; uint64_t frames; } BenchSink;
static int bench_pcm(void *ctx, const int16_t *pcm, size_t n) {
    BenchSink *b = ctx;
    if (b->first < 0) b->first = now_ms() - b->t0;
    b->frames += n;
    (void)pcm;
    return 0;
}

void vxs_set_asset_root(VxStream *s, const char *path) {
    pthread_mutex_lock(&s->mu);
    snprintf(s->asset_root, sizeof s->asset_root, "%s", path);
    pthread_mutex_unlock(&s->mu);
}

static int bench_float(void *ctx, const float *pcm, size_t n) {
    BenchSink *b = ctx;
    if (b->first < 0) for (size_t i = 0; i < n; i++) if (pcm[i] > 0.003f || pcm[i] < -0.003f) { b->first = now_ms() - b->t0; break; }
    b->frames += n;
    return 0;
}

int vxs_benchmark(const char *root, const char *asset_root, const char *key, const char *text,
                  int rate, double *open_ms, double *first_ms, double *total_ms, double *audio_ms) {
    char err[256];
    double t = now_ms();
    if (is_mt_key(key)) {
        char dir[1100], gen[16];
        snprintf(gen, sizeof gen, "%.*s", (int)(strchr(key, '/') - key), key);
        mt_engine_dir(root, asset_root, gen, dir, sizeof dir);
        MtEngine *mt = mte_open(dir, err, sizeof err);
        if (!mt) { fprintf(stderr, "%s\n", err); return -1; }
        char extra[1100];
        snprintf(extra, sizeof extra, "%s/%.*s/Voices", asset_root ? asset_root : "", (int)(strchr(key, '/') - key), key);
        mte_set_extra_voices(mt, extra);
        *open_ms = now_ms() - t;
        BenchSink b = {now_ms(), -1, 0};
        int rc = mte_speak(mt, strchr(key, '/') + 1, text, rate, 100, -1, bench_float, &b);
        *total_ms = now_ms() - b.t0;
        *first_ms = b.first;
        *audio_ms = b.frames * 1000.0 / 22050;
        mte_close(mt);
        return rc;
    }
    Vx *vx = open_engine(root, asset_root ? asset_root : "", key, err, sizeof err);
    if (!vx) return -1;
    *open_ms = now_ms() - t;
    vx_set_param(vx, 3, (uint32_t)rate);
    BenchSink b = {now_ms(), -1, 0};
    int rc = vx_speak_utf8(vx, text, bench_pcm, &b);
    *total_ms = now_ms() - b.t0;
    *first_ms = b.first;
    *audio_ms = b.frames * 1000.0 / vx_sample_rate(vx);
    vx_close(vx);
    return rc;
}
