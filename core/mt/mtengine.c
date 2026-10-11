// MacinTalk engine driver: load a generation, select voices, speak.
#include "mtengine.h"
#include "mt_internal.h"

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
};

#define OS(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define SO_RATE OS('r', 'a', 't', 'e')
#define SO_PBAS OS('p', 'b', 'a', 's')
#define SO_VOLM OS('v', 'o', 'l', 'm')

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

const char *mte_error(MtEngine *e) { return e->err; }
double mte_sample_rate(MtEngine *e) { (void)e; return mt_audio_rate(); }

#define GUARD(e, failret) \
    (e)->m->fault = &(e)->jb; \
    (e)->m->main->cpu.fault = &(e)->jb; \
    if (setjmp((e)->jb)) { snprintf((e)->err, sizeof (e)->err, "%s%s", (e)->m->err, (e)->m->main->cpu.err); return failret; }

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
    return e;
}

void mte_close(MtEngine *e) {
    if (!e) return;
    mt_destroy(e->m);
    free(e);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

uint32_t mt_speech_key(Mt *m, const char *sym);
uint32_t mt_cf_number(Mt *m, double v);
double mt_cf_number_value(Mt *m, uint32_t o);
static const char *prop_key(uint32_t sel) {
    return sel == SO_RATE ? "_kSpeechRateProperty" : sel == SO_PBAS ? "_kSpeechPitchBaseProperty" : "_kSpeechVolumeProperty";
}

static double get_fixed(MtEngine *e, uint32_t sel) {
    if (e->fn_copyprop) {  // 10.7: SECopySpeechProperty(chan, key, &CFNumber)
        uint32_t out = mt_static(e->m, 4);
        wr32(&e->m->main->cpu, out, 0);
        uint32_t a[3] = {e->chan, mt_speech_key(e->m, prop_key(sel)), out};
        if (mt_call(e->m, e->fn_copyprop, 3, a)) return 0;
        return mt_cf_number_value(e->m, rd32(&e->m->main->cpu, out));
    }
    if (!e->fn_getinfo) return 0;
    uint32_t v = mt_static(e->m, 4);
    uint32_t a[3] = {e->chan, sel, v};
    if (mt_call(e->m, e->fn_getinfo, 3, a)) return 0;
    return (int32_t)rd32(&e->m->main->cpu, v) / 65536.0;
}
static int set_fixed(MtEngine *e, uint32_t sel, double val) {
    if (e->fn_setprop) {  // 10.7: SESetSpeechProperty(chan, key, CFNumber)
        uint32_t a[3] = {e->chan, mt_speech_key(e->m, prop_key(sel)), mt_cf_number(e->m, val)};
        return (int)mt_call(e->m, e->fn_setprop, 3, a);
    }
    if (!e->fn_setinfo) return -1;
    uint32_t v = mt_static(e->m, 4);
    wr32(&e->m->main->cpu, v, (uint32_t)(int32_t)lround(val * 65536.0));
    uint32_t a[3] = {e->chan, sel, v};
    return (int)mt_call(e->m, e->fn_setinfo, 3, a);
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
    int rc = (int)mt_call(e->m, e->fn_use, 3, a);
    if (rc) { snprintf(e->err, sizeof e->err, "SEUseVoice(%s) failed: %d", voice, rc); return rc; }
    snprintf(e->voice, sizeof e->voice, "%s", voice);
    e->base_rate = get_fixed(e, SO_RATE);
    e->base_pitch = get_fixed(e, SO_PBAS);
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
// MacinTalk embedded commands.
static size_t convert(MtEngine *e, const char *s, const char *end, char *out, size_t cap, double pitch_base) {
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n++] = (char)(ch); } while (0)
    for (const char *p = s; p < end;) {
        unsigned char c = (unsigned char)*p;
        if (c == 0x1B && p + 1 < end && p[1] == '\\') {
            const char *q = memchr(p + 2, '\\', (size_t)(end - p - 2));
            if (!q) break;
            char cmd[64];
            snprintf(cmd, sizeof cmd, "%.*s", (int)(q - p - 2), p + 2);
            char buf[64];
            if (!strncmp(cmd, "pause=", 6)) snprintf(buf, sizeof buf, " [[slnc %d]] ", atoi(cmd + 6));
            else if (!strncmp(cmd, "pitch=", 6)) snprintf(buf, sizeof buf, " [[pbas %.2f]] ", pitch_base + 12.0 * log2(pitch_ratio(atoi(cmd + 6))));
            else buf[0] = 0;
            for (char *b = buf; *b; b++) PUT(*b);
            p = q + 1;
            continue;
        }
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
        int mr = to_macroman(cp);
        if (mr < 0) mr = ' ';
        PUT(mr);
    }
    out[n] = 0;
    (void)e;
    return n;
#undef PUT
}

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static int speak_one(MtEngine *e, const char *text, size_t len, MtPcmCallback cb, void *ctx) {
    Mt *m = e->m;
    uint32_t buf = mt_malloc(m, (uint32_t)len + 1);
    memcpy(G(m, buf), text, len + 1);
    mt_audio_begin();
    int rc;
    if (e->fn_speak) {
        uint32_t a[4] = {e->chan, buf, (uint32_t)len, 0};
        rc = (int)mt_call(m, e->fn_speak, 4, a);
    } else {  // 10.7 takes a CFString (MacRoman bytes here) and an options dictionary
        uint32_t a[3] = {e->chan, mt_cf_string(m, text, len), 0};
        rc = (int)mt_call(m, e->fn_speakcf, 3, a);
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
                if (e->fn_stop) { uint32_t s2[2] = {e->chan, 0}; mt_call(m, e->fn_stop, 2, s2); }
            }
            continue;
        }
        if (done) break;
        uint32_t sl = mt_audio_slices();
        if (sl != slices) {
            double gap = now_s() - last_change;
            if (gap > 0.25) mt_log("stall %.2fs at %u frames", gap, pos);
            slices = sl; last_change = now_s();
        }
        // The engine went quiet without stopping its graph. Generous: Alex can
        // pause a while paging in voice data on a phone.
        else if (now_s() - last_change > 5.0) { mt_log("gave up waiting at %u frames", pos); break; }
        usleep(500);
    }
    mt_free(m, buf);
    return stopped ? 1 : 0;
}

int mte_speak(MtEngine *e, const char *voice, const char *utf8, int rate, int pitch, int volume, MtPcmCallback cb, void *ctx) {
    GUARD(e, -1);
    if (use_voice(e, voice)) return -1;
    double r = e->base_rate * rate / 100.0, pb = e->base_pitch + 12.0 * log2(pitch / 100.0);
    double vol = volume < 0 ? 1.0 : volume / 100.0;
    if (r != e->cur_rate) { set_fixed(e, SO_RATE, r); e->cur_rate = r; }
    if (pb != e->cur_pitch) { set_fixed(e, SO_PBAS, pb); e->cur_pitch = pb; }
    if (vol != e->cur_volume) { set_fixed(e, SO_VOLM, vol); e->cur_volume = vol; }
    // Each ESC\eos=1\ segment is its own utterance, as VoiceOver's separate
    // announcements were.
    static const char kEos[] = "\x1b\\eos=1\\";
    const char *p = utf8, *end = utf8 + strlen(utf8);
    size_t cap = strlen(utf8) * 2 + 256;
    char *seg = malloc(cap);
    int rc = 0;
    while (p < end) {
        const char *q = strstr(p, kEos);
        const char *segend = q ? q : end;
        size_t n = convert(e, p, segend, seg, cap, pb);
        int blank = 1;
        for (size_t i = 0; i < n; i++) if (seg[i] != ' ') { blank = 0; break; }
        if (!blank) {
            rc = speak_one(e, seg, n, cb, ctx);
            if (rc) break;
        }
        p = q ? q + sizeof kEos - 1 : end;
    }
    free(seg);
    if (pb != e->cur_pitch) e->cur_pitch = -1;  // inline pbas may have moved it
    e->cur_pitch = -1;
    return rc < 0 ? rc : 0;
}
