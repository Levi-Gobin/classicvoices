// sd_classicvoices: a Speech Dispatcher output module for the Classic Voices
// engine (the iOS 3-6 and Mac OS X Lion-Mavericks Vocalizer voices, from your
// own firmware). Orca and anything else that speaks through Speech Dispatcher
// can use them.
//
// The protocol is handled by Speech Dispatcher's own module helpers
// (speechd/, vendored, BSD 2-clause) on the main thread; one worker thread
// runs each message through the engine stream and hands the audio to the
// server (server-side audio, Speech Dispatcher 0.10 and later), reporting
// index marks as the audio before them is sent.
//
// Installed as ~/.local/libexec/speech-dispatcher-modules/sd_classicvoices,
// Speech Dispatcher finds it by itself; see linux/README.md.
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "cv_catalog.h"
#include "cv_paths.h"
#include "cv_rules.h"
#include "cv_ssml.h"
#include "spd_module_main.h"
#include "vxstream.h"

void cv_set_native_dir(const char *dir);

#define DBG(...) do { if (g_debug) { fprintf(stderr, "classicvoices: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int g_debug;
static int g_rules = 1, g_fallback = 1;  // Apple's pronunciation rules; compact voice for pitch changes
static CvPaths g_paths;
static VxStream *g_stream;
static CvVoice *g_voices;
static size_t g_nvoices;
static SPDVoice **g_spd_voices;

// settings from SET (guarded by g_mu)
static struct {
    int rate, pitch, volume;  // speechd -100..100
    char voice_type[32], language[32], synthesis_voice[128];
    char cap_let[16], spelling[8];
} g_set = {0, 0, 100, "", "", "", "none", "off"};

// the job handed to the worker
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static char *g_job_text;
static SPDMessageType g_job_type;
static unsigned g_job_gen;
static int g_quit;
static atomic_uint g_gen;  // bumped by STOP/PAUSE; a job whose gen is stale stops
static pthread_t g_worker;

// ---------------------------------------------------------------- config

// classicvoices.conf (all optional):
//   DataPath "/path/to/classicvoices"     instead of ~/.local/share/classicvoices
//   PronunciationRules 0                  don't apply Apple's text rules ("iOS" -> "I O S" ...)
//   PitchFallback 0                       let enhanced/premium voices ignore pitch changes
//   Debug 1                               log to Speech Dispatcher's module log
int module_config(const char *configfile) {
    cv_default_paths(&g_paths);
    const char *e = getenv("CV_DEBUG");
    g_debug = e && *e && *e != '0';
    FILE *f = configfile ? fopen(configfile, "r") : NULL;
    if (!f) return 0;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[1100];
        if (line[0] == '#') continue;
        if (sscanf(line, " %63s \"%1099[^\"]\"", key, val) == 2 || sscanf(line, " %63s %1099s", key, val) == 2) {
            if (!strcasecmp(key, "DataPath")) cv_paths_from_root(&g_paths, val);
            else if (!strcasecmp(key, "Debug")) g_debug = atoi(val) != 0;
            else if (!strcasecmp(key, "PronunciationRules")) g_rules = atoi(val) != 0;
            else if (!strcasecmp(key, "PitchFallback")) g_fallback = atoi(val) != 0;
        }
    }
    fclose(f);
    return 0;
}

static void rescan(void) {
    cv_free_voices(g_voices);
    g_nvoices = cv_scan_voices(g_paths.voices, g_paths.downloads, &g_voices);
    if (g_spd_voices) {
        for (SPDVoice **v = g_spd_voices; *v; v++) { free((*v)->name); free((*v)->language); free((*v)->variant); free(*v); }
        free(g_spd_voices);
    }
    g_spd_voices = calloc(g_nvoices + 1, sizeof *g_spd_voices);
    for (size_t i = 0; i < g_nvoices; i++) {
        SPDVoice *v = calloc(1, sizeof *v);
        v->name = strdup(g_voices[i].name);
        v->language = strdup(g_voices[i].language[0] ? g_voices[i].language : "none");
        v->variant = NULL;
        g_spd_voices[i] = v;
    }
}

// ---------------------------------------------------------------- audio out

// Speech Dispatcher's server-audio frame (module_process.c's
// module_tts_output_send_server, which can't be used off the main thread:
// its wrapper reads commands from stdin between chunks).
static void send_audio(const int16_t *pcm, size_t frames) {
    const char *p = (const char *)pcm, *end = p + frames * 2;
    pthread_mutex_lock(&module_stdout_mutex);
    printf("705-bits=16\n705-num_channels=1\n705-sample_rate=22050\n705-num_samples=%zu\n705-big_endian=0\n", frames);
    fputs("705-AUDIO", stdout);
    putc(0, stdout);
    const char escape = 0x7d, invert = 1 << 5;  // HDLC escaping of NL and the escape byte
    while (p < end) {
        const char *stop = memchr(p, escape, (size_t)(end - p));
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        if (nl && (!stop || nl < stop)) stop = nl;
        const char *next;
        if (stop) next = stop + 1;
        else next = stop = end;
        fwrite(p, 1, (size_t)(stop - p), stdout);
        if (stop < end) { putc(escape, stdout); putc(*stop ^ invert, stdout); }
        p = next;
    }
    putc('\n', stdout);
    fputs("705 AUDIO\n", stdout);
    fflush(stdout);
    pthread_mutex_unlock(&module_stdout_mutex);
}

// ---------------------------------------------------------------- the worker

static int pct_exp(int v, double per_octave, int lo, int hi) {
    double p = 100.0 * __builtin_pow(2.0, v / per_octave);
    int r = (int)(p + 0.5);
    return r < lo ? lo : r > hi ? hi : r;
}

static const CvVoice *choose_voice(const char *synth_voice, const char *language, const char *voice_type) {
    const CvVoice *v = NULL;
    if (synth_voice[0] && strcmp(synth_voice, "NULL")) v = cv_find_voice(g_voices, g_nvoices, synth_voice);
    if (v) return v;
    const char *lang = language[0] && strcmp(language, "NULL") ? language : "en-US";
    // voice type: prefer a voice of that gender for the same locale and generation (speechd's
    // default type is MALE1, which shouldn't turn American English into British Daniel)
    int want_female = -1;
    if (!strncasecmp(voice_type, "female", 6) || !strncasecmp(voice_type, "child_female", 12)) want_female = 1;
    else if (!strncasecmp(voice_type, "male", 4) || !strncasecmp(voice_type, "child_male", 10)) want_female = 0;
    const CvVoice *best = cv_pick_voice(g_voices, g_nvoices, lang);
    // a language no voice speaks (or none, as with the C locale): American English, not the first voice listed
    if (!best) best = cv_pick_voice(g_voices, g_nvoices, "en-US");
    if (best && want_female >= 0 && best->female != want_female) {
        for (size_t i = 0; i < g_nvoices; i++) {
            const CvVoice *c = &g_voices[i];
            if (c->female == want_female && !strcmp(c->language, best->language) && strncmp(c->key, best->key, 4) == 0)
                return c;
        }
    }
    return best ? best : (g_nvoices ? &g_voices[0] : NULL);
}

// Plain text for CHAR and KEY messages ("shift_a" -> "shift a").
static char *key_text(const char *in) {
    char *s = strdup(in);
    for (char *p = s; *p; p++) if (*p == '_' || *p == '-') *p = ' ';
    return s;
}

static char *xml_escape(const char *in) {
    size_t n = 0;
    for (const char *p = in; *p; p++) n += *p == '<' || *p == '>' ? 4 : *p == '&' ? 5 : 1;
    char *o = malloc(n + 1), *q = o;
    for (const char *p = in; *p; p++) {
        if (*p == '<') { memcpy(q, "&lt;", 4); q += 4; }
        else if (*p == '>') { memcpy(q, "&gt;", 4); q += 4; }
        else if (*p == '&') { memcpy(q, "&amp;", 5); q += 5; }
        else *q++ = *p;
    }
    *q = 0;
    return o;
}

static void speak_job(char *text, SPDMessageType type, unsigned gen) {
    pthread_mutex_lock(&g_mu);
    int rate = pct_exp(g_set.rate, 50, 50, 400);    // -100..100 -> 25% (50%)..400%
    int pitch = pct_exp(g_set.pitch, 100, 50, 200);  // -100..100 -> 50%..200%
    // speechd's volume runs -100..100 with 100 the normal (default) level:
    // that is the voice's own volume (80 on the engine's scale), and lower
    // values scale it down to silence at -100.
    int volume = g_set.volume >= 100 ? -1 : (g_set.volume + 100) * 80 / 200;
    const CvVoice *v = choose_voice(g_set.synthesis_voice, g_set.language, g_set.voice_type);
    int cap_pitch = !strcmp(g_set.cap_let, "pitch");
    int spell = !strcmp(g_set.spelling, "on");
    pthread_mutex_unlock(&g_mu);

    module_report_event_begin();
    if (type == SPD_MSGTYPE_SOUND_ICON) {
        module_report_icon(text);
        module_report_event_end();
        return;
    }
    if (!v) {
        DBG("no voices under %s", g_paths.root);
        module_report_event_end();
        return;
    }
    char *ssml = NULL;
    if (type == SPD_MSGTYPE_CHAR || type == SPD_MSGTYPE_KEY || type == SPD_MSGTYPE_SPELL || spell) {
        char *plain = type == SPD_MSGTYPE_KEY ? key_text(text) : strdup(text);
        if (spell && type == SPD_MSGTYPE_TEXT) {  // letter by letter
            size_t n = strlen(plain);
            char *sp = malloc(n * 2 + 1), *q = sp;
            for (size_t i = 0; i < n; i++) { *q++ = plain[i]; if ((plain[i] & 0xC0) != 0x80 && i + 1 < n && (plain[i + 1] & 0xC0) != 0x80) *q++ = ' '; }
            *q = 0;
            free(plain);
            plain = sp;
        }
        int upper = type == SPD_MSGTYPE_CHAR && isupper((unsigned char)plain[0]) && !plain[1];
        char *esc = xml_escape(plain);
        size_t n = strlen(esc) + 64;
        ssml = malloc(n);
        if (upper && cap_pitch) snprintf(ssml, n, "<speak><prosody pitch=\"+50%%\">%s</prosody></speak>", esc);
        else snprintf(ssml, n, "<speak>%s</speak>", esc);
        free(esc);
        free(plain);
    }
    CvUtterance u;
    const CvRules *rules = g_rules ? cv_rules_for(g_paths.voices, cv_voice_build(v), v->language) : NULL;
    cv_parse_ssml_rules(ssml ? ssml : text, rate, pitch, volume, rules, &u);
    free(ssml);
    const char *fallback = g_fallback && v->fallback[0] ? v->fallback : NULL;
    DBG("speak %s (%zu segments) %s", v->key, u.n, text);

    float buf[2205];
    int16_t pcm[2205];
    int stopped = 0;
    for (size_t i = 0; i < u.n && !stopped; i++) {
        CvSegment *g = &u.seg[i];
        if (g->text[0] || g->trailing_pause_ms) {
            vxs_start_with_fallback(g_stream, v->key, fallback, g->text, g->rate, g->pitch, g->volume,
                                    g->trailing_pause_ms);
            int done = 0, idle = 0;
            while (!done) {
                if (atomic_load(&g_gen) != gen) { stopped = 1; break; }
                size_t n = vxs_read(g_stream, buf, 2205, &done);
                if (!n) {
                    if (++idle > 15000) { DBG("engine stalled: %s", vxs_last_error(g_stream)); break; }
                    usleep(1000);
                    continue;
                }
                idle = 0;
                for (size_t k = 0; k < n; k++) {
                    float x = buf[k] * 32767.0f;
                    pcm[k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
                }
                send_audio(pcm, n);
            }
        }
        if (!stopped && g->mark) module_report_index_mark(g->mark);
    }
    cv_free_utterance(&u);
    if (stopped) {
        vxs_cancel(g_stream);
        module_report_event_stop();
    } else {
        module_report_event_end();
    }
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (!g_quit && !g_job_text) pthread_cond_wait(&g_cv, &g_mu);
        if (g_quit) break;
        char *text = g_job_text;
        SPDMessageType type = g_job_type;
        unsigned gen = g_job_gen;
        g_job_text = NULL;
        pthread_mutex_unlock(&g_mu);
        if (atomic_load(&g_gen) != gen) {  // stopped before it began
            module_report_event_begin();
            module_report_event_stop();
        } else {
            speak_job(text, type, gen);
        }
        free(text);
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

// ---------------------------------------------------------------- module API

int module_init(char **msg) {
    rescan();
    cv_set_native_dir(g_paths.native);
    g_stream = vxs_create(g_paths.voices);
    vxs_set_asset_root(g_stream, g_paths.downloads);
    module_audio_set_server();
    pthread_create(&g_worker, NULL, worker, NULL);
    char m[1400];
    snprintf(m, sizeof m, "Classic Voices: %zu voices in %s", g_nvoices, g_paths.root);
    *msg = strdup(m);
    if (!g_nvoices) fprintf(stderr, "classicvoices: no voices found under %s; import firmware with classicvoices-data\n", g_paths.root);
    return 0;
}

SPDVoice **module_list_voices(void) {
    pthread_mutex_lock(&g_mu);
    rescan();  // voices imported since startup appear without a restart
    pthread_mutex_unlock(&g_mu);
    return g_spd_voices;
}

int module_set(const char *var, const char *val) {
    pthread_mutex_lock(&g_mu);
    int ok = 0;
    if (!strcmp(var, "rate")) g_set.rate = atoi(val);
    else if (!strcmp(var, "pitch")) g_set.pitch = atoi(val);
    else if (!strcmp(var, "volume")) g_set.volume = atoi(val);
    else if (!strcmp(var, "voice")) snprintf(g_set.voice_type, sizeof g_set.voice_type, "%s", val);
    else if (!strcmp(var, "language")) snprintf(g_set.language, sizeof g_set.language, "%s", val);
    else if (!strcmp(var, "synthesis_voice")) {
        snprintf(g_set.synthesis_voice, sizeof g_set.synthesis_voice, "%s", val);
        const CvVoice *v = strcmp(val, "NULL") ? cv_find_voice(g_voices, g_nvoices, val) : NULL;
        if (v) vxs_preload(g_stream, v->key);  // warm it up before the first message
    } else if (!strcmp(var, "cap_let_recogn")) snprintf(g_set.cap_let, sizeof g_set.cap_let, "%s", val);
    else if (!strcmp(var, "spelling_mode")) snprintf(g_set.spelling, sizeof g_set.spelling, "%s", val);
    else if (!strcmp(var, "punctuation_mode") || !strcmp(var, "pitch_range") || !strcmp(var, "punctuation_some") ||
             !strcmp(var, "punctuation_most")) { /* accepted, not used */ }
    else ok = -1;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int module_audio_set(const char *var, const char *val) {
    // only server-side audio (module_audio_set_server)
    return !strcmp(var, "audio_output_method") && !strcmp(val, "server") ? 0 : -1;
}
int module_audio_init(char **status) { *status = strdup("server audio"); return 0; }
int module_loglevel_set(const char *var, const char *val) { (void)var; (void)val; return 0; }
int module_debug(int enable, const char *file) { (void)file; g_debug = enable; return 0; }

int module_loop(void) {
    int ret = module_process(STDIN_FILENO, 1);
    if (ret) fprintf(stderr, "classicvoices: server closed the connection\n");
    return ret;
}

int module_speak(char *data, size_t bytes, SPDMessageType msgtype) {
    (void)bytes;
    pthread_mutex_lock(&g_mu);
    free(g_job_text);  // a message the worker hasn't taken yet is replaced
    g_job_text = strdup(data);
    g_job_type = msgtype;
    g_job_gen = atomic_load(&g_gen);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return 1;
}

int module_stop(void) {
    atomic_fetch_add(&g_gen, 1);
    vxs_cancel(g_stream);
    return 0;
}

size_t module_pause(void) {
    module_stop();
    return 0;
}

int module_close(void) {
    pthread_mutex_lock(&g_mu);
    g_quit = 1;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mu);
    if (g_stream) {
        module_stop();
        pthread_join(g_worker, NULL);
        vxs_destroy(g_stream);
        g_stream = NULL;
    }
    return 0;
}
