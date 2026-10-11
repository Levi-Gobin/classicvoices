// AudioToolbox for the MacinTalk guest: where the speech arrives.
//
// The engine builds ScheduledSoundPlayer -> DefaultOutput and hands over
// finished float PCM with kAudioUnitProperty_ScheduleAudioSlice (3300). The
// host plays the part of the audio hardware: a pacer thread "plays" each
// slice (sleeping its duration divided by the clock speed), copies it into
// the utterance buffer at its sample time, marks it complete and calls the
// engine's completion proc -- which is the engine's own clock. Design and
// struct layouts from panthera-speech's tiger_host_audio.c.
#include "mt_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }

// ScheduledAudioSlice (92 bytes on i386)
#define SL_SAMPLETIME 0
#define SL_TSFLAGS 56
#define SL_PROC 64
#define SL_UDATA 68
#define SL_FLAGS 72
#define SL_FRAMES 84
#define SL_BUFLIST 88
#define SLICE_COMPLETE 1

#define kAUProp_StreamFormat 8
#define kAUProp_ScheduleAudioSlice 3300
#define kAUProp_ScheduleStartTime 3301

double mt_speed(void);

// ---------------------------------------------------------------- utterance buffer

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static float *g_pcm;
static uint32_t g_pcm_n, g_pcm_cap;
static uint32_t g_epoch_seq, g_last_epoch = ~0u, g_epoch_base, g_epoch_slices, g_epoch_start;
static double g_time_origin, g_last_stime;
static int g_have_origin;
static uint32_t g_slices, g_utt;
static int g_stopped;
static double g_begin_t, g_first_t;
static double g_rate = 22050;
static uint8_t g_asbd[40];
static int g_have_asbd;

static void pcm_put(uint32_t pos, const float *s, uint32_t n) {
    uint32_t end = pos + n;
    if (end > g_pcm_cap) {
        uint32_t cap = g_pcm_cap ? g_pcm_cap : 1u << 16;
        while (cap < end) cap *= 2;
        g_pcm = realloc(g_pcm, cap * sizeof(float));
        g_pcm_cap = cap;
    }
    while (g_pcm_n < pos) g_pcm[g_pcm_n++] = 0;
    memcpy(g_pcm + pos, s, n * sizeof(float));
    if (end > g_pcm_n) g_pcm_n = end;
    else if (end < g_pcm_n && pos == g_epoch_start && g_epoch_slices == 1) g_pcm_n = end;  // replaced the kick
}

// place a slice at its sample time (see panthera's collect_slice for why the
// epochs exist: the engine restarts the player's timeline within an utterance)
static void collect(Mt *m, uint32_t slice, uint32_t epoch) {
    uint8_t *s = G(m, slice);
    uint32_t frames, bl, tsflags;
    double stime;
    memcpy(&frames, s + SL_FRAMES, 4);
    memcpy(&bl, s + SL_BUFLIST, 4);
    memcpy(&stime, s + SL_SAMPLETIME, 8);
    memcpy(&tsflags, s + SL_TSFLAGS, 4);
    if (!bl || !frames) return;
    if (epoch != g_last_epoch) {
        // a new epoch continues after the last; but if the previous epoch was
        // just the engine's one-frame silent kick, the new one replaces it
        if (g_last_epoch != ~0u)
            g_epoch_base = (g_epoch_slices == 1 && g_pcm_n == g_epoch_start + 1 && g_pcm[g_epoch_start] == 0.0f) ? g_epoch_start : g_pcm_n;
        g_time_origin = stime;
        g_have_origin = 1;
        g_last_stime = 0;
        g_last_epoch = epoch;
        g_epoch_slices = 0;
        g_epoch_start = g_epoch_base;
    }
    g_epoch_slices++;
    if (!g_have_origin) { g_time_origin = stime; g_have_origin = 1; }
    stime -= g_time_origin;
    if (stime < 0) { g_epoch_base = g_pcm_n; g_time_origin += stime; stime = 0; }
    if (stime < g_last_stime) g_epoch_base = g_pcm_n;
    g_last_stime = stime;
    uint32_t nbufs, bytes, data;
    memcpy(&nbufs, G(m, bl), 4);
    if (!nbufs) return;
    memcpy(&bytes, G(m, bl + 8), 4);
    memcpy(&data, G(m, bl + 12), 4);
    if (!data) return;
    uint32_t n = bytes / 4;
    if (frames < n) n = frames;
    uint32_t pos = g_epoch_base + (stime > 0 ? (uint32_t)(stime + 0.5) : 0);
    if (!(tsflags & 1)) pos = g_pcm_n;
    pcm_put(pos, (const float *)G(m, data), n);
}

// ---------------------------------------------------------------- pacer

#define PQ 512
typedef struct { uint32_t proc, udata, slice, frames, utt, epoch; } Pending;
static Pending g_q[PQ];
static int g_qh, g_qt, g_qn, g_busy;
static int g_pacer_started;
static MtThread *g_pacer_thread;
static pthread_t g_pacer_tid;

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void *pacer_main(void *arg) {
    Mt *m = arg;
    g_pacer_tid = pthread_self();
    MtThread *t = g_pacer_thread;
    X86 *c = &t->cpu;
    c->fault = &t->fault;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_qn && !m->dying) pthread_cond_wait(&g_cv, &g_mu);
        if (m->dying) { pthread_mutex_unlock(&g_mu); break; }
        Pending job = g_q[g_qh];
        g_qh = (g_qh + 1) % PQ;
        g_qn--;
        g_busy = 1;
        pthread_mutex_unlock(&g_mu);

        double s = job.frames / g_rate / mt_speed();
        if (s >= 1e-6) usleep((useconds_t)(s * 1e6));
        else sched_yield();

        pthread_mutex_lock(&g_mu);
        if (job.utt == g_utt) {
            uint32_t before = g_pcm_n;
            collect(m, job.slice, job.epoch);
            if (!g_first_t)  // first audible sample
                for (uint32_t i = before; i < g_pcm_n; i++) if (fabsf(g_pcm[i]) > 1e-3f) { g_first_t = now_s() - g_begin_t; break; }
        }
        g_slices++;
        pthread_mutex_unlock(&g_mu);

        uint32_t fl = rd32(c, job.slice + SL_FLAGS) | SLICE_COMPLETE;
        wr32(c, job.slice + SL_FLAGS, fl);
        if (job.proc) {
            if (setjmp(t->fault)) mt_log("slice completion faulted: %s", c->err);
            else {
                uint32_t args[2] = {job.udata, job.slice};
                mt_call_on(m, c, job.proc, 2, args);
            }
        }
        pthread_mutex_lock(&g_mu);
        g_busy = 0;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
    }
    __sync_fetch_and_sub(&m->live, 1);
    return NULL;
}

static void schedule_slice(Mt *m, X86 *c, uint32_t slice) {
    if (!g_pacer_started) {
        g_pacer_started = 1;
        g_pacer_thread = mt_thread_new(m);
        pthread_t th;
        __sync_fetch_and_add(&m->live, 1);
        pthread_create(&th, NULL, pacer_main, m);
        pthread_detach(th);
    }
    Pending p = {rd32(c, slice + SL_PROC), rd32(c, slice + SL_UDATA), slice, rd32(c, slice + SL_FRAMES), 0, 0};
    pthread_mutex_lock(&g_mu);
    while (g_qn >= PQ && !m->dying) pthread_cond_wait(&g_cv, &g_mu);
    if (m->dying) { pthread_mutex_unlock(&g_mu); x86_fault(c, "engine shut down"); }
    p.utt = g_utt;
    p.epoch = g_epoch_seq;
    g_q[g_qt] = p;
    g_qt = (g_qt + 1) % PQ;
    g_qn++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// ---------------------------------------------------------------- host side

void mt_audio_mark_done(void) {
    pthread_mutex_lock(&g_mu);
    g_stopped = 1;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

void mt_audio_begin(void) {
    pthread_mutex_lock(&g_mu);
    g_utt++;
    g_pcm_n = 0;
    g_epoch_base = 0;
    g_last_stime = 0;
    g_have_origin = 0;
    g_last_epoch = ~0u;
    g_epoch_slices = g_epoch_start = 0;
    g_stopped = 0;
    g_begin_t = now_s();
    g_first_t = 0;
    pthread_mutex_unlock(&g_mu);
}

// Wait for the utterance to finish: the engine stops its graph, then the
// pacer drains. A quiet period ends it otherwise. Returns frames collected.
uint32_t mt_audio_wait(double quiet_s, double max_s) {
    double t0 = now_s(), last_change = t0;
    uint32_t last = ~0u;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        uint32_t sl = g_slices;
        int idle = !g_qn && !g_busy, stopped = g_stopped;
        uint32_t n = g_pcm_n;
        pthread_mutex_unlock(&g_mu);
        double t = now_s();
        if (sl != last) { last = sl; last_change = t; }
        if (stopped && idle) return n;
        if (idle && t - last_change > quiet_s) return n;
        if (t - t0 > max_s) return n;
        usleep(1000);
    }
}
const float *mt_audio_pcm(uint32_t *n) { *n = g_pcm_n; return g_pcm; }

// Streaming: copy frames [from, from+max) that are final. Slices can land a
// little behind the end, so while the utterance runs `hold` frames are kept
// back. *done = utterance finished (graph stopped and pacer idle, or quiet).
uint32_t mt_audio_take(float *dst, uint32_t from, uint32_t max, uint32_t hold, int *done) {
    pthread_mutex_lock(&g_mu);
    int idle = !g_qn && !g_busy;
    *done = g_stopped && idle;
    uint32_t avail = g_pcm_n;
    if (!*done) avail = avail > hold ? avail - hold : 0;
    uint32_t n = 0;
    if (avail > from) {
        n = avail - from < max ? avail - from : max;
        memcpy(dst, g_pcm + from, n * sizeof(float));
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}
uint32_t mt_audio_slices(void) { pthread_mutex_lock(&g_mu); uint32_t n = g_slices; pthread_mutex_unlock(&g_mu); return n; }
double mt_audio_rate(void) { return g_rate; }
double mt_audio_first_ms(void) { return g_first_t * 1e3; }

// ---------------------------------------------------------------- AUGraph

static uint32_t g_graph, g_units[8];
static int g_nnodes;

static void h_NewAUGraph(Mt *m, X86 *c) { if (!g_graph) g_graph = mt_static(m, 16); wr32(c, A(c, 0), g_graph); R(c, 0); }
static void h_AUGraphAddNode(Mt *m, X86 *c) {  // (graph, desc*, node*)
    (void)m;
    if (g_nnodes >= 8) { R(c, (uint32_t)-50); return; }
    wr32(c, A(c, 2), (uint32_t)++g_nnodes);
    R(c, 0);
}
static void h_AUGraphNewNode(Mt *m, X86 *c) {  // (graph, desc*, size, data, node*)
    (void)m;
    if (g_nnodes >= 8) { R(c, (uint32_t)-50); return; }
    wr32(c, A(c, 4), (uint32_t)++g_nnodes);
    R(c, 0);
}
static uint32_t unit_for(Mt *m, uint32_t node) {
    if (!g_units[node - 1]) { g_units[node - 1] = mt_static(m, 16); memcpy(G(m, g_units[node - 1]), "AUNT", 4); memcpy(G(m, g_units[node - 1] + 4), &node, 4); }
    return g_units[node - 1];
}
static void h_AUGraphNodeInfo(Mt *m, X86 *c) {  // (graph, node, desc*, unit*)
    uint32_t node = A(c, 1);
    if (node < 1 || node > 8) { R(c, (uint32_t)-50); return; }
    if (A(c, 3)) wr32(c, A(c, 3), unit_for(m, node));
    R(c, 0);
}
static void h_AUGraphGetNodeInfo(Mt *m, X86 *c) {  // (graph, node, desc*, csize*, cdata*, unit*)
    uint32_t node = A(c, 1);
    if (node < 1 || node > 8) { R(c, (uint32_t)-50); return; }
    if (A(c, 5)) wr32(c, A(c, 5), unit_for(m, node));
    R(c, 0);
}
static void h_AUGraphStop(Mt *m, X86 *c) {
    (void)m;
    pthread_mutex_lock(&g_mu);
    g_stopped = 1;
    pthread_mutex_unlock(&g_mu);
    R(c, 0);
}
static void h_ok(Mt *m, X86 *c) { (void)m; R(c, 0); }
static void h_noop(Mt *m, X86 *c) { (void)m; (void)c; }

// ---------------------------------------------------------------- AudioUnit properties

static void default_asbd(uint8_t *a) {
    // Float32, 22050 Hz, mono, packed, non-interleaved
    double rate = g_rate;
    uint32_t v[8] = {0x6C70636Du, 0x29, 4, 1, 4, 1, 32, 0};  // 'lpcm', float|packed|noninterleaved
    memcpy(a, &rate, 8);
    memcpy(a + 8, v, 32);
}

static void h_AudioUnitSetProperty(Mt *m, X86 *c) {
    uint32_t prop = A(c, 1), data = A(c, 4), size = A(c, 5);
    if (prop == kAUProp_ScheduleAudioSlice) {
        pthread_mutex_lock(&g_mu);
        pthread_mutex_unlock(&g_mu);
        schedule_slice(m, c, data);
    } else if (prop == kAUProp_ScheduleStartTime) {
        pthread_mutex_lock(&g_mu);
        g_epoch_seq++;
        pthread_mutex_unlock(&g_mu);
    } else if (prop == kAUProp_StreamFormat && size >= 40) {
        memcpy(g_asbd, G(m, data), 40);
        g_have_asbd = 1;
        double r;
        memcpy(&r, g_asbd, 8);
        if (r > 1000) g_rate = r;
    }
    R(c, 0);
}
static void h_AudioUnitGetProperty(Mt *m, X86 *c) {
    uint32_t prop = A(c, 1), data = A(c, 4), sizep = A(c, 5);
    uint32_t size = sizep ? rd32(c, sizep) : 0;
    if (prop == kAUProp_StreamFormat && data && size >= 40) {
        uint8_t a[40];
        if (g_have_asbd) memcpy(a, g_asbd, 40); else default_asbd(a);
        memcpy(G(m, data), a, 40);
        wr32(c, sizep, 40);
        R(c, 0);
        return;
    }
    if (data && size) { mt_check(m, c, data, size); memset(G(m, data), 0, size); }
    R(c, 0);
}
static void h_AudioUnitGetPropertyInfo(Mt *m, X86 *c) {
    (void)m;
    if (A(c, 4)) wr32(c, A(c, 4), A(c, 1) == kAUProp_StreamFormat ? 40 : 4);
    if (A(c, 5)) wr8(c, A(c, 5), 1);
    R(c, 0);
}
// MacinTalk resets the unit when its wait for queued slices times out (guest
// time runs fast, so on a busy phone it does), then retires the slices it still
// counts as queued. The pacer must not complete those too, or the count goes
// negative and the retire loop runs off the end of memory: deliver them all
// first, so the engine finds none left.
static void h_AudioUnitReset(Mt *m, X86 *c) {
    if (g_pacer_started && !pthread_equal(pthread_self(), g_pacer_tid)) {
        double until = now_s() + 3.0;
        pthread_mutex_lock(&g_mu);
        while ((g_qn || g_busy) && !m->dying && now_s() < until) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_cv, &g_mu, &ts);
        }
        pthread_mutex_unlock(&g_mu);
    }
    R(c, 0);
}

// AudioFile: recording to a file is never asked of this host
static void h_AudioFileFail(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)-50); }

static void h_AudioConverterUnavailable(Mt *m, X86 *c) { (void)m; x86_fault(c, "AudioConverter (AAC) not implemented yet"); }

void mt_audio_wake(void) { pthread_mutex_lock(&g_mu); pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu); }
void mt_audio_reset(void) {
    g_pacer_started = 0;
    g_pacer_thread = NULL;
    g_qh = g_qt = g_qn = g_busy = 0;
    g_graph = 0;
    memset(g_units, 0, sizeof g_units);
    g_nnodes = 0;
    g_have_asbd = 0;
    g_rate = 22050;
    g_epoch_seq = 0;
    g_slices = 0;
    g_pcm_n = 0;
}

const MtHleEntry mt_hle_audio[] = {
    {"_NewAUGraph", h_NewAUGraph}, {"_AUGraphAddNode", h_AUGraphAddNode}, {"_AUGraphNewNode", h_AUGraphNewNode},
    {"_AUGraphNodeInfo", h_AUGraphNodeInfo}, {"_AUGraphGetNodeInfo", h_AUGraphGetNodeInfo},
    {"_AUGraphConnectNodeInput", h_ok}, {"_AUGraphOpen", h_ok}, {"_AUGraphInitialize", h_ok},
    {"_AUGraphStart", h_ok}, {"_AUGraphStop", h_AUGraphStop}, {"_AUGraphUninitialize", h_ok},
    {"_DisposeAUGraph", h_ok}, {"_CAShow", h_noop},
    {"_AudioUnitSetProperty", h_AudioUnitSetProperty}, {"_AudioUnitGetProperty", h_AudioUnitGetProperty},
    {"_AudioUnitGetPropertyInfo", h_AudioUnitGetPropertyInfo}, {"_AudioUnitReset", h_AudioUnitReset},
    {"_AudioUnitAddPropertyListener", h_ok},
    {"_AudioFileCreate", h_AudioFileFail}, {"_AudioFileClose", h_ok}, {"_AudioFileOptimize", h_ok},
    {"_AudioFileSetProperty", h_ok}, {"_AudioFileWritePackets", h_AudioFileFail},
    {"_AudioConverterNew", h_AudioConverterUnavailable}, {"_AudioConverterDispose", h_ok},
    {"_AudioConverterFillComplexBuffer", h_AudioConverterUnavailable}, {"_AudioConverterGetProperty", h_AudioConverterUnavailable},
    {"_AudioConverterGetPropertyInfo", h_AudioConverterUnavailable}, {"_AudioConverterSetProperty", h_AudioConverterUnavailable},
    {"_AudioConverterReset", h_ok}, {"_AudioFormatGetProperty", h_AudioConverterUnavailable},
    {"_SpeechBusy", h_ok},
    {NULL, NULL},
};
