// MacinTalk engine driver: load a generation, select voices, speak.
#include "mtengine.h"
#include "mt_internal.h"
#include "mt_native.h"
#include "mt_rates.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint32_t mt_audio_take(float *dst, uint32_t from, uint32_t max, uint32_t hold, int *done);
uint32_t mt_audio_slices(void);

struct MtEngine {
    Mt *m;
    int image;
    uint32_t chan;
    uint32_t fn_use, fn_speak, fn_setinfo, fn_getinfo, fn_stop;
    uint32_t fn_speakcf, fn_setprop, fn_copyprop;  // 10.7
    char root[1024];
    char extra[1024];
    char voice[128];
    double base_rate, base_pitch;  // the voice's defaults (wpm, pitch units)
    double cur_rate, cur_pitch, cur_volume;
    jmp_buf jb;
    char err[512];
    // watchdog: a call into the engine that never returns, or an engine
    // thread that faulted, shuts the engine down so the caller can reopen it
    volatile double call_t0;
    volatile double stop_deadline;  // a cancel's stop and drain must be done by then, or the engine is shut down
    volatile int closing;
    pthread_mutex_t dog_mu;
    pthread_cond_t dog_cv;
    char *seg;  // mte_speak's converted text (here so a fault out of the engine doesn't leak it)
    int reset_commands;  // reset the channel before an utterance that follows embedded commands
    int dirty;           // the last utterance had embedded commands ([[...]]), which persist
    double trk_rate, trk_pitch;  // the channel's rate and pitch base as embedded commands leave them
    int twelve_hour;     // this release reads 13:45 as "one forty-five PM" (Leopard)
    int spell_24h;       // ... and 24-hour times are respelled the way Snow Leopard reads them
    char *seg24;         // mte_speak's respelled text
    char *segcmd;        // ... and with a leading command's bracket put back
    int modern_rate;     // rate follows today's Alex's curve, not a percent of the voice's default
    MtMarkCallback mark_cb;  // bookmarks (ESC\mrk=N\), with mte_speak's ctx
    pthread_t dog;
    int dog_started;
};

#define OS(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define SO_RATE OS('r', 'a', 't', 'e')
#define SO_PBAS OS('p', 'b', 'a', 's')
#define SO_VOLM OS('v', 'o', 'l', 'm')
#define SO_RSET OS('r', 's', 'e', 't')

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

// Leopard's SpeechDictionary (3.5 in 10.5.0; Tiger has 3.3, Snow Leopard 3.10).
static int is_leopard(const char *root) {
    char p[1100], buf[4096];
    snprintf(p, sizeof p, "%s/SpeechDictionary/Resources/version.plist", root);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *k = strstr(buf, "CFBundleShortVersionString");
    const char *v = k ? strstr(k, "<string>") : NULL;
    int major = 0, minor = 0;
    if (!v || sscanf(v + 8, "%d.%d", &major, &minor) != 2) return 0;
    return major == 3 && minor >= 4 && minor < 10;
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

void mt_wake_all(void);
#define MT_CALL_LIMIT 10.0  // seconds; far longer than any call takes, even Alex paging in
// MacinTalk holds its channel's lock until it has synthesized the whole
// utterance, so SEStopSpeechAt can take seconds on a long one (VoiceOver waits
// for it before the next). Past this, shutting the engine down and opening a
// new one for the next utterance is much quicker.
#ifndef MT_STOP_LIMIT
#define MT_STOP_LIMIT 0.012  // a stop MacinTalk honours is done in ~1 ms (Snow Leopard's Alex up to 8); a restart takes 20-40
#endif

static void *watchdog(void *arg) {
    MtEngine *e = arg;
    pthread_mutex_lock(&e->dog_mu);
    while (!e->closing) {
        // 20 ms between checks, so closing the engine never waits long for
        // this; a pending stop wakes it (dog_cv) and is timed exactly
        double wake = now_s() + 0.02, sd = e->stop_deadline;
        if (sd > 0 && sd < wake) wake = sd;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double rt = ts.tv_sec + ts.tv_nsec / 1e9 + (wake - now_s());
        ts.tv_sec = (time_t)rt;
        ts.tv_nsec = (long)((rt - (double)ts.tv_sec) * 1e9);
        pthread_cond_timedwait(&e->dog_cv, &e->dog_mu, &ts);
        Mt *m = e->m;
        double t0 = e->call_t0;
        int stuck = t0 > 0 && now_s() - t0 > MT_CALL_LIMIT;
        sd = e->stop_deadline;
        int slow_stop = sd > 0 && now_s() >= sd;
        if (m->dying || !(m->broken || stuck || slow_stop)) continue;
        if (slow_stop) mt_log("MacinTalk is finishing the cancelled utterance; restarting it instead");
        else mt_log("MacinTalk %s; restarting it", stuck ? "stopped responding" : "faulted");
        m->broken = 1;
        m->dying = 1;  // every guest thread, the caller's included, faults out
        mt_wake_all();
    }
    pthread_mutex_unlock(&e->dog_mu);
    return NULL;
}

#ifdef X86_TRACE
void x86_trace_threads(void (*show)(uint32_t trap, uint32_t caller, uint32_t esp, uint32_t eip));
static Mt *trace_mt;
static void show_thread(uint32_t trap, uint32_t caller, uint32_t esp, uint32_t eip) {
    char s[256], t[256];
    uint32_t idx = (trap - MT_TRAP_BASE) / 16;
    mt_log("  thread esp %#x: last trap %s from %s; eip now %s", esp,
           idx < (uint32_t)trace_mt->ntraps ? trace_mt->trap_name[idx] : "?", mt_symbolize(trace_mt, caller, s, sizeof s),
           mt_symbolize(trace_mt, eip, t, sizeof t));
}
#endif

static uint32_t call(MtEngine *e, uint32_t fn, int nargs, const uint32_t *args) {
    e->call_t0 = now_s();
#ifdef MT_TEST_HOOKS
    if (getenv("MT_TEST_BREAK") && !strcmp(getenv("MT_TEST_BREAK"), "hang")) {  // a lock nobody will release
        static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_trylock(&held);
        mt_lock(e->m, &e->m->main->cpu, &held);
    }
#endif
    uint32_t r = mt_call(e->m, fn, nargs, args);
    e->call_t0 = 0;
    return r;
}

const char *mte_error(MtEngine *e) { return e->err; }
double mte_sample_rate(MtEngine *e) { (void)e; return mt_audio_rate(); }

#define GUARD(e, failret) \
    (e)->m->fault = &(e)->jb; \
    (e)->m->main->cpu.fault = &(e)->jb; \
    if (setjmp((e)->jb)) { (e)->call_t0 = 0; (e)->stop_deadline = 0; snprintf((e)->err, sizeof (e)->err, "%s%s", (e)->m->err, (e)->m->main->cpu.err); return failret; }

MtEngine *mte_open(const char *root, char *err, size_t errlen) {
    MtEngine *e = calloc(1, sizeof *e);
    snprintf(e->root, sizeof e->root, "%s", root);
    e->m = mt_create(err, errlen);
    if (!e->m) { free(e); return NULL; }
    e->m->verbose = getenv("MT_VERBOSE") ? atoi(getenv("MT_VERBOSE")) : 0;
    e->m->fault = &e->jb;
    e->m->main->cpu.fault = &e->jb;
    if (setjmp(e->jb)) {
        snprintf(err, errlen, "%s%s", e->m->err, e->m->main->cpu.err);
        mt_destroy(e->m);
        free(e);
        return NULL;
    }
    char p[1200];
    static const char *libs[] = {"libc++abi.dylib", "libstdc++.6.0.4.dylib", "libstdc++.6.0.9.dylib", NULL};
    for (int i = 0; libs[i]; i++) {
        snprintf(p, sizeof p, "%s/lib/%s", root, libs[i]);
        if (exists(p)) mt_load(e->m, p);
    }
    snprintf(p, sizeof p, "%s/SpeechDictionary/SpeechDictionary", root);
    mt_load(e->m, p);
    snprintf(p, sizeof p, "%s/MacinTalk", root);
    e->image = mt_load(e->m, p);
    mt_bind_all(e->m);
    mt_native_attach(e->m);
    snprintf(p, sizeof p, "%s/SpeechDictionary", root);
    mt_cf_set_dictionary_bundle(e->m, p);
    mt_run_initializers(e->m);
    e->fn_use = mt_export(e->m, e->image, "_SEUseVoice");
    e->fn_speak = mt_export(e->m, e->image, "_SESpeakBuffer");
    e->fn_setinfo = mt_export(e->m, e->image, "_SESetSpeechInfo");
    e->fn_getinfo = mt_export(e->m, e->image, "_SEGetSpeechInfo");
    e->fn_stop = mt_export(e->m, e->image, "_SEStopSpeechAt");
    e->fn_speakcf = mt_export(e->m, e->image, "_SESpeakCFString");
    e->fn_setprop = mt_export(e->m, e->image, "_SESetSpeechProperty");
    e->fn_copyprop = mt_export(e->m, e->image, "_SECopySpeechProperty");
    uint32_t open = mt_export(e->m, e->image, "_SEOpenSpeechChannel");
    if (!open || !e->fn_use || (!e->fn_speak && !e->fn_speakcf)) {
        snprintf(err, errlen, "MacinTalk speech API not found in %s", root);
        mt_destroy(e->m);
        free(e);
        return NULL;
    }
    uint32_t chanp = mt_static(e->m, 4);
    uint32_t rc = mt_call(e->m, open, 1, &chanp);
    e->chan = rd32(&e->m->main->cpu, chanp);
    if (rc || !e->chan) {
        snprintf(err, errlen, "SEOpenSpeechChannel failed: %d", (int)rc);
        mt_destroy(e->m);
        free(e);
        return NULL;
    }
    e->reset_commands = 1;
    e->spell_24h = 1;
    e->twelve_hour = is_leopard(root);
    pthread_mutex_init(&e->dog_mu, NULL);
    pthread_cond_init(&e->dog_cv, NULL);
    e->dog_started = pthread_create(&e->dog, NULL, watchdog, e) == 0;
    return e;
}

void mte_close(MtEngine *e) {
    if (!e) return;
    pthread_mutex_lock(&e->dog_mu);
    e->closing = 1;
    pthread_cond_signal(&e->dog_cv);
    pthread_mutex_unlock(&e->dog_mu);
    if (e->dog_started) pthread_join(e->dog, NULL);
    mt_native_finish(e->m);
    mt_destroy(e->m);
    free(e->seg);
    free(e->seg24);
    free(e->segcmd);
    pthread_mutex_destroy(&e->dog_mu);
    pthread_cond_destroy(&e->dog_cv);
    free(e);
}

int mte_broken(MtEngine *e) { return e->m->broken || e->m->dying; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t mt_speech_key(Mt *m, const char *sym);
uint32_t mt_cf_number(Mt *m, double v);
double mt_cf_number_value(Mt *m, uint32_t o);
const char *mt_cf_text(Mt *m, uint32_t s);
static const char *prop_key(uint32_t sel) {
    return sel == SO_RATE ? "_kSpeechRateProperty" : sel == SO_PBAS ? "_kSpeechPitchBaseProperty" : "_kSpeechVolumeProperty";
}

static double get_fixed(MtEngine *e, uint32_t sel) {
    if (e->fn_copyprop) {  // 10.7: SECopySpeechProperty(chan, key, &CFNumber)
        uint32_t out = mt_static(e->m, 4);
        wr32(&e->m->main->cpu, out, 0);
        uint32_t a[3] = {e->chan, mt_speech_key(e->m, prop_key(sel)), out};
        if (call(e, e->fn_copyprop, 3, a)) return 0;
        return mt_cf_number_value(e->m, rd32(&e->m->main->cpu, out));
    }
    if (!e->fn_getinfo) return 0;
    uint32_t v = mt_static(e->m, 4);
    uint32_t a[3] = {e->chan, sel, v};
    if (call(e, e->fn_getinfo, 3, a)) return 0;
    return (int32_t)rd32(&e->m->main->cpu, v) / 65536.0;
}
static int set_fixed(MtEngine *e, uint32_t sel, double val) {
    if (sel == SO_RATE) e->trk_rate = val;
    if (sel == SO_PBAS) e->trk_pitch = val;
    if (e->fn_setprop) {  // 10.7: SESetSpeechProperty(chan, key, CFNumber)
        uint32_t a[3] = {e->chan, mt_speech_key(e->m, prop_key(sel)), mt_cf_number(e->m, val)};
        return (int)call(e, e->fn_setprop, 3, a);
    }
    if (!e->fn_setinfo) return -1;
    uint32_t v = mt_static(e->m, 4);
    wr32(&e->m->main->cpu, v, (uint32_t)(int32_t)lround(val * 65536.0));
    uint32_t a[3] = {e->chan, sel, v};
    return (int)call(e, e->fn_setinfo, 3, a);
}

// Embedded commands ([[inpt PHON]], [[rate 300]] ...) change the channel until
// changed back; reset it to the voice's defaults (rate, pitch, modes) and set
// VoiceOver's values again.
static void reset_channel(MtEngine *e) {
    if (e->fn_setprop) {  // 10.7: SESetSpeechProperty(chan, kSpeechResetProperty, NULL)
        uint32_t a[3] = {e->chan, mt_speech_key(e->m, "_kSpeechResetProperty"), 0};
        call(e, e->fn_setprop, 3, a);
    } else if (e->fn_setinfo) {
        uint32_t a[3] = {e->chan, SO_RSET, 0};
        call(e, e->fn_setinfo, 3, a);
    }
    e->cur_rate = e->cur_pitch = e->cur_volume = -1;
    e->trk_rate = e->base_rate;
    e->trk_pitch = e->base_pitch;
}

void mte_set_reset_commands(MtEngine *e, int on) { e->reset_commands = on; }
void mte_set_24_hour(MtEngine *e, int on) { e->spell_24h = on; }
void mte_set_modern_rate(MtEngine *e, int on) { e->modern_rate = on; }
void mte_set_mark_callback(MtEngine *e, MtMarkCallback cb) { e->mark_cb = cb; }

// Words per minute today's Alex (macOS 26) speaks at a rate of `pct` percent
// of normal, as the system sends it (12.5% at the slowest, 400% at the
// fastest): two straight lines meeting at 100% (222 wpm), measured through
// AVSpeechSynthesizer.write, within 3 wpm.
static double modern_target(int pct) { return pct <= 100 ? 48.73 + 1.7366 * pct : 2.7078 * pct - 48.3; }

// The rate to ask this voice for so that it speaks `target` words per minute,
// measured the same way (mt_rates.h): MacinTalk's rate isn't words per minute
// for every voice (Tiger's Fred asked for 500 speaks 1281). Past the fastest
// the voice gets, it is asked for the rate where it levelled off (or, for
// Tiger's Cellos and Organ, before it collapses to 32 wpm). A voice not in the
// table is taken at its word.
static double rate_for(const MtEngine *e, double target) {
    const char *gen = strrchr(e->root, '/');
    gen = gen ? gen + 1 : e->root;
    for (size_t i = 0; i < sizeof kMtRates / sizeof *kMtRates; i++) {
        if (strcmp(kMtRates[i].gen, gen) || strcmp(kMtRates[i].voice, e->voice)) continue;
        const short *w = kMtRates[i].wpm;
        int n = 1;  // the rising part
        while (n < MT_RATE_POINTS && w[n] > w[n - 1] * 1.01 + 0.5) n++;
        if (n == 1 || w[0] <= 0) return target;  // rate does nothing (Bells)
        if (target <= w[0]) return kMtRateAsked[0] * target / w[0];
        if (target >= w[n - 1]) return kMtRateAsked[n - 1];
        int j = 1;
        while (w[j] < target) j++;
        double f = (target - w[j - 1]) / (w[j] - w[j - 1]);
        return kMtRateAsked[j - 1] + f * (kMtRateAsked[j] - kMtRateAsked[j - 1]);
    }
    return target;
}

void mte_set_extra_voices(MtEngine *e, const char *dir) { snprintf(e->extra, sizeof e->extra, "%s", dir ? dir : ""); }

static int use_voice(MtEngine *e, const char *voice) {
    if (!strcmp(e->voice, voice)) return 0;
    char dir[1300], vd[1400];
    snprintf(dir, sizeof dir, "%s/Voices/%s.SpeechVoice", e->root, voice);
    snprintf(vd, sizeof vd, "%s/Contents/Resources/VoiceDescription", dir);
    if (!exists(vd) && e->extra[0]) {
        snprintf(dir, sizeof dir, "%s/%s.SpeechVoice", e->extra, voice);
        snprintf(vd, sizeof vd, "%s/Contents/Resources/VoiceDescription", dir);
    }
    FILE *f = fopen(vd, "rb");
    uint8_t h[12];
    if (!f || fread(h, 1, 12, f) != 12) { if (f) fclose(f); snprintf(e->err, sizeof e->err, "no voice %s", voice); return -1; }
    fclose(f);
    uint32_t spec = mt_static(e->m, 8);
    wr32(&e->m->main->cpu, spec, be32(h + 4));
    wr32(&e->m->main->cpu, spec + 4, be32(h + 8));
    uint32_t a[3] = {e->chan, spec, mt_cf_bundle(e->m, dir)};
    int rc = (int)call(e, e->fn_use, 3, a);
    if (rc) { snprintf(e->err, sizeof e->err, "SEUseVoice(%s) failed: %d", voice, rc); return rc; }
    snprintf(e->voice, sizeof e->voice, "%s", voice);
    e->base_rate = get_fixed(e, SO_RATE);
    e->base_pitch = get_fixed(e, SO_PBAS);
    e->trk_rate = e->base_rate;
    e->trk_pitch = e->base_pitch;
    e->cur_rate = e->cur_pitch = e->cur_volume = -1;
    if (e->m->verbose) fprintf(stderr, "[mte] voice %s: rate %.1f wpm, pitch %.2f\n", voice, e->base_rate, e->base_pitch);
    return 0;
}

// MacRoman for U+0080..: inverse of the table panthera-speech generated from
// Python's mac_roman codec.
static const uint16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};
static int to_macroman(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    for (int i = 0; i < 128; i++) if (kMacRoman[i] == cp) return 0x80 + i;
    return -1;
}

// engine pitch value (1-100, 50 = normal) -> ratio; same table as vxstream
static double pitch_ratio(int v) {
    static const double val[] = {1, 25, 50, 60, 75, 100};
    static const double ratio[] = {0.557, 0.760, 1.000, 1.130, 1.304, 1.581};
    if (v <= 1) return ratio[0];
    for (int i = 1; i < 6; i++)
        if (v <= val[i]) return ratio[i - 1] + (v - val[i - 1]) / (val[i] - val[i - 1]) * (ratio[i] - ratio[i - 1]);
    return ratio[5];
}

// Convert one segment of UTF-8 (with ESC commands) to MacRoman with
// MacinTalk embedded commands. Vocalizer's L&H+ phonemes (ESC\toi=lhp\ ...
// ESC\toi=orth\) mean nothing to MacinTalk and are left out; its own
// ([[inpt PHON]] ...) pass through.
static size_t convert(MtEngine *e, const char *s, const char *end, char *out, size_t cap, double pitch_base) {
    size_t n = 0;
    int lhp = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n++] = (char)(ch); } while (0)
    for (const char *p = s; p < end;) {
        unsigned char c = (unsigned char)*p;
        if (c == 0x1B && p + 1 < end && p[1] == '\\') {
            const char *q = memchr(p + 2, '\\', (size_t)(end - p - 2));
            if (!q) break;
            char cmd[64];
            snprintf(cmd, sizeof cmd, "%.*s", (int)(q - p - 2), p + 2);
            char buf[64];
            if (!strncmp(cmd, "toi=", 4)) lhp = !strcmp(cmd + 4, "lhp");
            if (lhp || !strncmp(cmd, "toi=", 4)) buf[0] = 0;
            else if (!strncmp(cmd, "pause=", 6)) snprintf(buf, sizeof buf, " [[slnc %d]] ", atoi(cmd + 6));
            else if (!strncmp(cmd, "rate=", 5)) {  // percent of normal, as mte_speak's rate
                int v = atoi(cmd + 5);
                double wpm = e->modern_rate ? rate_for(e, modern_target(v < 10 ? 10 : v > 400 ? 400 : v))
                                            : e->base_rate * (v < 50 ? 50 : v > 400 ? 400 : v) / 100.0;
                snprintf(buf, sizeof buf, " [[rate %.1f]] ", wpm);
            } else if (!strncmp(cmd, "vol=", 4)) {  // 0-100, as mte_speak's volume
                int v = atoi(cmd + 4);
                snprintf(buf, sizeof buf, " [[volm %.2f]] ", (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0);
            } else if (!strcmp(cmd, "tn=spell")) snprintf(buf, sizeof buf, " [[char LTRL]] ");
            else if (!strcmp(cmd, "tn=normal")) snprintf(buf, sizeof buf, " [[char NORM]] ");
            else if (!strncmp(cmd, "pitch=", 6)) snprintf(buf, sizeof buf, " [[pbas %.2f]] ", pitch_base + 12.0 * log2(pitch_ratio(atoi(cmd + 6))));
            else if (!strncmp(cmd, "pitchpct=", 9) && atof(cmd + 9) > 0)  // percent of the voice's pitch (clamp_commands caps it)
                snprintf(buf, sizeof buf, " [[pbas %.2f]] ", fmax(0, pitch_base + 12.0 * log2(atof(cmd + 9) / 100)));
            else buf[0] = 0;
            for (char *b = buf; *b; b++) PUT(*b);
            p = q + 1;
            continue;
        }
        if (lhp) { p++; continue; }
        uint32_t cp;
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (p + len > end) break;
        if (len == 1) cp = c;
        else if (len == 2) cp = (c & 0x1Fu) << 6 | (p[1] & 0x3F);
        else if (len == 3) cp = (c & 0x0Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3F);
        else cp = (c & 0x07u) << 18 | (p[1] & 0x3Fu) << 12 | (p[2] & 0x3Fu) << 6 | (p[3] & 0x3F);
        p += len;
        // MacinTalk embedded commands ([[rate 300]], [[pbas 50]], [[slnc 500]] ...)
        // pass through, as they did with the original Speech Manager voices.
        if (cp == 0x2019 || cp == 0x2018) cp = '\'';
        if (cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') cp = ' ';  // control bytes stand for other characters in 10.7's strings
        int mr = to_macroman(cp);
        if (mr < 0) mr = ' ';
        PUT(mr);
    }
    out[n] = 0;
    (void)e;
    return n;
#undef PUT
}

// Out-of-range values in embedded commands wrap inside MacinTalk: rate is a
// 16.16 fixed-point number (32768 wpm speaks at the slowest rate), pbas
// becomes a 16-bit frequency (above about 155 it drops to the lowest pitch),
// and pmod and volm wrap past 2^32. Every release levels off well inside
// those limits (pbas by about 80, pmod at 200, volm at 2), so a value past
// the top is set to the top: rate 32767, pbas 127, pmod 200, volm 2. A
// relative change (+N, -N) that would carry the value past the top is cut to
// reach the top; pbas and rate are tracked through the text for that.
// Measured on Fred, Vicki and Alex in 10.4-10.7, which all agree.
static size_t clamp_commands(MtEngine *e, const char *in, size_t n, char *out, size_t cap) {
    size_t o = 0;
#define EMIT(src, len) do { if (o + (len) >= cap) return 0; memcpy(out + o, (src), (len)); o += (len); } while (0)
    const char *p = in, *end = in + n;
    while (p < end) {
        const char *open = memmem(p, (size_t)(end - p), "[[", 2);
        if (!open) { EMIT(p, (size_t)(end - p)); break; }
        const char *close = memmem(open + 2, (size_t)(end - open - 2), "]]", 2);
        if (!close) close = end;
        EMIT(p, (size_t)(open - p));
        const char *q = open;
        for (const char *c = open + 2; c < close;) {
            while (c < close && (*c == ' ' || *c == '\t' || *c == ';')) c++;
            if (close - c < 4) break;
            char name[5];
            for (int i = 0; i < 4; i++) name[i] = (char)(c[i] | 0x20);
            name[4] = 0;
            const char *a = c + 4, *cmdend = memchr(c, ';', (size_t)(close - c));
            if (!cmdend) cmdend = close;
            c = cmdend;
            if (!strcmp(name, "rset")) { e->trk_rate = e->base_rate; e->trk_pitch = e->base_pitch; continue; }
            int kind = !strcmp(name, "rate") ? 1 : !strcmp(name, "pbas") ? 2 : !strcmp(name, "pmod") ? 3 : !strcmp(name, "volm") ? 4 : 0;
            if (!kind || a >= cmdend || (*a != ' ' && *a != '\t')) continue;
            while (a < cmdend && (*a == ' ' || *a == '\t')) a++;
            const char *num = a;
            int sign = 0;
            if (a < cmdend && (*a == '+' || *a == '-')) sign = *a++ == '+' ? 1 : -1;
            const char *digits = a;
            while (a < cmdend && (isdigit((unsigned char)*a) || *a == '.')) a++;
            if (a == digits) continue;
            char tmp[64];
            snprintf(tmp, sizeof tmp, "%.*s", (int)(a - digits) < 60 ? (int)(a - digits) : 60, digits);
            double v = atof(tmp), top = kind == 1 ? 32767 : kind == 2 ? 127 : kind == 3 ? 200 : 2;
            double *trk = kind == 1 ? &e->trk_rate : kind == 2 ? &e->trk_pitch : NULL;
            char rep[48];
            rep[0] = 0;
            if (!sign) {
                if (v > top) { v = top; snprintf(rep, sizeof rep, "%g", top); }
                if (trk) *trk = v;
            } else if (trk) {
                double d = sign * v;
                if (*trk + d > top) { d = top - *trk > 0 ? top - *trk : 0; snprintf(rep, sizeof rep, "+%.3f", d); }
                *trk += d;
                if (*trk < 0) *trk = 0;
            } else if (v > top) {
                snprintf(rep, sizeof rep, "%c%g", sign > 0 ? '+' : '-', top);
            }
            if (rep[0]) {
                EMIT(q, (size_t)(num - q));
                EMIT(rep, strlen(rep));
                q = a;
            }
        }
        const char *stop = close < end ? close + 2 : end;
        EMIT(q, (size_t)(stop - q));
        p = stop;
    }
    out[o] = 0;
    return o;
#undef EMIT
}

// Leopard reads a 24-hour time as a 12-hour one ("13:45" is "one forty-five
// PM", "00:30" "twelve thirty AM"), where Tiger, Snow Leopard and Lion read it
// as written. Times that can only be 24-hour (hour 13-24, or a leading zero)
// are respelled so Leopard says what Snow Leopard says: 13:45 -> "13 45"
// (thirteen forty-five), 18:05 -> "18 oh 5", 00:30 -> "0 0 30", 07:30 ->
// "oh 7 30", 18:05:45 -> "18 oh 5 45". NULL if nothing changed.
static void put_pair(char **o, const char *d) {  // two digits as Snow Leopard says them in a time
    if (d[0] == '0' && d[1] == '0') *o += sprintf(*o, "0 0");
    else if (d[0] == '0') *o += sprintf(*o, "oh %c", d[1]);
    else *o += sprintf(*o, "%c%c", d[0], d[1]);
}
static char *spell_24h(const char *s) {
    const char *c = strchr(s, ':');
    if (!c) return NULL;
    size_t n = strlen(s);
    char *out = NULL, *o = NULL;
    const char *copied = s;
    for (; c; c = strchr(c + 1, ':')) {
        const char *h = c;
        while (h > s && isdigit((unsigned char)h[-1]) && c - h < 3) h--;
        int hl = (int)(c - h);
        if (hl < 1 || hl > 2 || h < copied) continue;
        if (h > s && (isalnum((unsigned char)h[-1]) || h[-1] == ':' || h[-1] == '.' || h[-1] == ',')) continue;
        const char *m = c + 1;
        if (!isdigit((unsigned char)m[0]) || !isdigit((unsigned char)m[1]) || m[0] > '5') continue;
        const char *t = m + 2, *sec = NULL;
        if (t[0] == ':' && isdigit((unsigned char)t[1]) && isdigit((unsigned char)t[2]) && t[1] <= '5') { sec = t + 1; t += 3; }
        if (isdigit((unsigned char)*t) || *t == ':') continue;
        int hv = hl == 2 ? (h[0] - '0') * 10 + h[1] - '0' : h[0] - '0';
        int lead0 = hl == 2 && h[0] == '0';
        if (!lead0 && (hv < 13 || hv > 24)) continue;
        if (!out) { out = malloc(n * 3 + 16); o = out; }
        memcpy(o, copied, (size_t)(h - copied));
        o += h - copied;
        if (lead0) put_pair(&o, h);
        else o += sprintf(o, "%d", hv);
        *o++ = ' ';
        put_pair(&o, m);
        if (sec) { *o++ = ' '; put_pair(&o, sec); }
        copied = t;
        c = t - 1;
    }
    if (!out) return NULL;
    strcpy(o, copied);
    return out;
}

// Voices whose last stop missed MT_STOP_LIMIT (Leopard's Alex and Vicki on a
// fast Mac, Snow Leopard's Vicki): the next cancel restarts the engine
// straight away rather than waiting out the limit first, with every eighth
// trying a stop again. Process-wide, since a restart replaces the MtEngine.
static struct { char key[160]; int slow, skipped; } g_stops[32];
static pthread_mutex_t g_stops_mu = PTHREAD_MUTEX_INITIALIZER;
static int stop_hint(MtEngine *e, int set) {  // set: -1 read (and count a skip), 0/1 record
    char key[160];
    snprintf(key, sizeof key, "%s/%s", e->root, e->voice);
    pthread_mutex_lock(&g_stops_mu);
    int i = 0;
    while (i < 32 && g_stops[i].key[0] && strcmp(g_stops[i].key, key)) i++;
    if (i == 32) i = (int)(mt_str_hash(key) & 31);
    if (strcmp(g_stops[i].key, key)) { snprintf(g_stops[i].key, sizeof g_stops[i].key, "%s", key); g_stops[i].slow = g_stops[i].skipped = 0; }
    int r = 0;
    if (set >= 0) { g_stops[i].slow = set; g_stops[i].skipped = 0; }
    else if (g_stops[i].slow && ++g_stops[i].skipped % 8) r = 1;
    pthread_mutex_unlock(&g_stops_mu);
    return r;
}

// macOS's speech framework (say, AVSpeechSynthesizer) turns the embedded
// commands it knows into SSML and swallows some, but one it doesn't know at
// the very start of the text loses its first bracket: "[[inpt TUNE]]ppp"
// arrives as " [inpt TUNE]]ppp", which Lion's Alex reads as text ("question").
// Later in the text they come through whole. Put the bracket back. NULL if
// the text doesn't start that way.
static char *restore_command(const char *s) {
    const char *p = s;
    for (;;) {  // past spaces and our own ESC\...\ commands
        while (*p == ' ' || *p == '\t') p++;
        if (p[0] == 0x1B && p[1] == '\\') { const char *q = strchr(p + 2, '\\'); if (!q) return NULL; p = q + 1; continue; }
        break;
    }
    if (p[0] != '[' || p[1] == '[') return NULL;
    for (int i = 1; i <= 4; i++) if (!isalpha((unsigned char)p[i])) return NULL;
    if (p[5] != ' ' && p[5] != ';' && p[5] != ']') return NULL;
    const char *close = strstr(p, "]]");
    if (!close) return NULL;
    for (const char *q = p + 1; q < close; q++) if (*q == '[') return NULL;
    size_t n = strlen(s), at = (size_t)(p - s);
    char *out = malloc(n + 2);
    memcpy(out, s, at);
    out[at] = '[';
    memcpy(out + at + 1, s + at, n - at + 1);
    return out;
}

static int speak_one(MtEngine *e, const char *text, size_t len, MtPcmCallback cb, void *ctx) {
    Mt *m = e->m;
    uint32_t buf = mt_malloc(m, (uint32_t)len + 1);
    memcpy(G(m, buf), text, len + 1);
    mt_audio_begin();
    int rc;
    if (e->fn_speak) {
        uint32_t a[4] = {e->chan, buf, (uint32_t)len, 0};
        rc = (int)call(e, e->fn_speak, 4, a);
    } else {  // 10.7 takes a CFString (MacRoman bytes here) and an options dictionary
        uint32_t a[3] = {e->chan, mt_cf_string(m, text, len), 0};
        rc = (int)call(e, e->fn_speakcf, 3, a);
    }
    if (rc) { mt_free(m, buf); snprintf(e->err, sizeof e->err, "SESpeakBuffer failed: %d", rc); return rc; }
    float tmp[4096];
    uint32_t pos = 0, slices = mt_audio_slices();
    double last_change = now_s();
    int stopped = 0;
    for (;;) {
        int done;
        uint32_t n = mt_audio_take(tmp, pos, 4096, 2048, &done);
        if (n) {
            pos += n;
            if (!stopped && cb(ctx, tmp, n)) {  // cancelled: stop the engine, drain quietly
                stopped = 1;
                // The deadline covers the stop and the drain after it: a
                // stop can return at once and the engine still synthesize
                // for 40 ms before it is done, longer than a restart takes.
                if (e->fn_stop && stop_hint(e, -1)) {
                    mt_log("MacinTalk stops slowly with %s; restarting it", e->voice);
                    m->broken = 1;
                    m->dying = 1;  // as the watchdog does: every guest thread faults out
                    mt_wake_all();
                    snprintf(e->err, sizeof e->err, "MacinTalk was slow to stop; it reopens for the next utterance");
                    return -1;
                }
                if (e->fn_stop) {
                    stop_hint(e, 1);  // cleared once the drain is done in time; a stop the watchdog cuts off faults out of here
                    pthread_mutex_lock(&e->dog_mu);
                    e->stop_deadline = now_s() + MT_STOP_LIMIT;
                    pthread_cond_signal(&e->dog_cv);
                    pthread_mutex_unlock(&e->dog_mu);
                    uint32_t s2[2] = {e->chan, 0};
                    call(e, e->fn_stop, 2, s2);  // faults out (to mte_speak's GUARD) if the watchdog shuts the engine down
                }
            }
            continue;
        }
        if (done) break;
        if (m->broken) {
            e->stop_deadline = 0;
            if (stopped) { snprintf(e->err, sizeof e->err, "MacinTalk was slow to stop; it reopens for the next utterance"); return -1; }
            mt_log("engine faulted at %u frames", pos);
            return -1;
        }
        uint32_t sl = mt_audio_slices();
        if (sl != slices) {
            double gap = now_s() - last_change;
            if (gap > 0.25) mt_log("stall %.2fs at %u frames", gap, pos);
            slices = sl; last_change = now_s();
        }
        // The engine went quiet without stopping its graph. Generous: Alex can
        // pause a while paging in voice data on a phone.
        else if (now_s() - last_change > 5.0) {
            mt_log("gave up waiting at %u frames", pos);
#ifdef X86_TRACE
            trace_mt = e->m;
            x86_trace_threads(show_thread);
#endif
            m->broken = 1;  // whatever it was doing, it won't speak again
            e->stop_deadline = 0;
            return -1;
        }
        usleep(stopped ? 100 : 500);
    }
    e->stop_deadline = 0;
    if (stopped && e->fn_stop) stop_hint(e, 0);
    mt_free(m, buf);
    return stopped ? 1 : 0;
}

int mte_prepare(MtEngine *e, const char *voice) {
    if (mte_broken(e)) return -1;
    GUARD(e, -1);
    return use_voice(e, voice);
}

// The phonemes MacinTalk would speak for `utf8` (MacRoman text, no ESC
// commands), for checking what the engine makes of a piece of text.
int mte_phonemes(MtEngine *e, const char *voice, const char *utf8, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = 0;
    if (mte_broken(e)) return -1;
    GUARD(e, -1);
    if (use_voice(e, voice)) return -1;
    Mt *m = e->m;
    char *spelled = e->twelve_hour && e->spell_24h ? spell_24h(utf8) : NULL;
    if (spelled) utf8 = spelled;
    size_t len = strlen(utf8);
    char *text = malloc(len * 2 + 256);
    size_t n = convert(e, utf8, utf8 + len, text, len * 2 + 256, e->base_pitch);
    uint32_t fn = mt_export(m, e->image, "_SECopyPhonemesFromText");
    int rc;
    if (fn) {  // 10.7: SECopyPhonemesFromText(chan, CFString, &CFString)
        uint32_t res = mt_static(m, 4);
        wr32(&m->main->cpu, res, 0);
        uint32_t a[3] = {e->chan, mt_cf_string(m, text, n), res};
        rc = (int)call(e, fn, 3, a);
        if (!rc && rd32(&m->main->cpu, res)) snprintf(out, cap, "%s", mt_cf_text(m, rd32(&m->main->cpu, res)));
    } else {  // SETextToPhonemes(chan, text, len, Handle, &bytes)
        fn = mt_export(m, e->image, "_SETextToPhonemes");
        uint32_t buf = mt_malloc(m, (uint32_t)n + 1), ph = mt_malloc(m, 65536), h = mt_static(m, 8);
        memcpy(G(m, buf), text, n + 1);
        wr32(&m->main->cpu, h, ph);
        wr32(&m->main->cpu, h + 4, 0);
        uint32_t a[5] = {e->chan, buf, (uint32_t)n, h, h + 4};
        rc = fn ? (int)call(e, fn, 5, a) : -1;
        uint32_t got = rd32(&m->main->cpu, h + 4), p = rd32(&m->main->cpu, h);
        if (!rc) snprintf(out, cap, "%.*s", (int)(got < cap ? got : cap - 1), (const char *)G(m, p));
        mt_free(m, buf);
    }
    free(text);
    free(spelled);
    return rc;
}

int mte_speak(MtEngine *e, const char *voice, const char *utf8, int rate, int pitch, int volume, MtPcmCallback cb, void *ctx) {
    if (mte_broken(e)) { snprintf(e->err, sizeof e->err, "MacinTalk stopped; it reopens for the next utterance"); return -1; }
    GUARD(e, -1);
    if (use_voice(e, voice)) return -1;
    if (e->dirty && e->reset_commands) reset_channel(e);
    e->dirty = 0;
    double r = e->modern_rate ? rate_for(e, modern_target(rate)) : e->base_rate * (rate < 50 ? 50 : rate) / 100.0, pb = e->base_pitch + 12.0 * log2(pitch / 100.0);
    double vol = volume < 0 ? 1.0 : volume / 100.0;
    if (r != e->cur_rate) { set_fixed(e, SO_RATE, r); e->cur_rate = r; }
    if (pb != e->cur_pitch) { set_fixed(e, SO_PBAS, pb); e->cur_pitch = pb; }
    if (vol != e->cur_volume) { set_fixed(e, SO_VOLM, vol); e->cur_volume = vol; }
    // Each ESC\eos=1\ segment is its own utterance, as VoiceOver's separate
    // announcements were; so is each piece between bookmarks (ESC\mrk=N\),
    // each reported to mark_cb once the piece before it is spoken.
    static const char kEos[] = "\x1b\\eos=1\\", kMrk[] = "\x1b\\mrk=";
    free(e->seg24);
    e->seg24 = NULL;
    free(e->segcmd);
    e->segcmd = NULL;
    if ((e->segcmd = restore_command(utf8))) utf8 = e->segcmd;
    if (e->twelve_hour && e->spell_24h && (e->seg24 = spell_24h(utf8))) utf8 = e->seg24;
    const char *p = utf8, *end = utf8 + strlen(utf8);
    size_t cap = strlen(utf8) * 2 + 256, ccap = cap * 3;
    free(e->seg);
    char *seg = e->seg = malloc(cap + ccap), *clamped = seg + cap;
    int rc = 0;
    while (p < end) {
        const char *q = strstr(p, kEos), *mk = strstr(p, kMrk);
        if (mk && (!q || mk < q)) q = mk;
        else mk = NULL;
        const char *segend = q ? q : end;
        size_t n = convert(e, p, segend, seg, cap, pb);
        int blank = 1;
        for (size_t i = 0; i < n; i++) if (seg[i] != ' ') { blank = 0; break; }
        if (!blank) {
            const char *text = seg;
            if (memmem(seg, n, "[[", 2)) {
                e->dirty = 1;
                size_t cn = clamp_commands(e, seg, n, clamped, ccap);
                if (cn) { text = clamped; n = cn; }
            }
            rc = speak_one(e, text, n, cb, ctx);
            if (rc) break;
        }
        if (mk) {
            const char *num = mk + sizeof kMrk - 1, *close = strchr(num, '\\');
            if (e->mark_cb) e->mark_cb(ctx, (unsigned)strtoul(num, NULL, 10));
            p = close ? close + 1 : end;
        } else p = q ? q + sizeof kEos - 1 : end;
    }
    free(seg);
    e->seg = NULL;
    free(e->seg24);
    e->seg24 = NULL;
    free(e->segcmd);
    e->segcmd = NULL;
    if (pb != e->cur_pitch) e->cur_pitch = -1;  // inline pbas may have moved it
    e->cur_pitch = -1;
    return rc < 0 ? rc : 0;
}
