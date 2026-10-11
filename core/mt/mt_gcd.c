// Just enough Grand Central Dispatch for Snow Leopard's MacinTalk, whose back
// end runs on timer sources instead of MP tasks. Semantics follow
// panthera-speech's tiger_host_gcd.c: each source gets its own thread; 10.6
// computes its deadlines from gettimeofday, so the start time of a timer is a
// (sped-up) wall-clock nanosecond count.
#include "mt_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }
static inline void R64(X86 *c, uint64_t v) { c->r[EAX] = (uint32_t)v; c->r[EDX] = (uint32_t)(v >> 32); }

// ---------------------------------------------------------------- the clock

static pthread_mutex_t g_clk_mu = PTHREAD_MUTEX_INITIALIZER;
static double g_wall0, g_mono0;  // anchors
static int g_have_anchor;

static double mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

// Guest wall time in microseconds: a real epoch, with elapsed time sped up.
uint64_t mt_wall_us(void) {
    pthread_mutex_lock(&g_clk_mu);
    if (!g_have_anchor) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        g_wall0 = tv.tv_sec + tv.tv_usec / 1e6;
        g_mono0 = mono_s();
        g_have_anchor = 1;
    }
    double t = g_wall0 + (mono_s() - g_mono0) * mt_speed();
    pthread_mutex_unlock(&g_clk_mu);
    return (uint64_t)(t * 1e6);
}

static void h_gettimeofday(Mt *m, X86 *c) {
    (void)m;
    uint64_t us = mt_wall_us();
    if (A(c, 0)) { wr32(c, A(c, 0), (uint32_t)(us / 1000000)); wr32(c, A(c, 0) + 4, (uint32_t)(us % 1000000)); }
    R(c, 0);
}
static void h_time(Mt *m, X86 *c) { (void)m; uint32_t t = (uint32_t)(mt_wall_us() / 1000000); if (A(c, 0)) wr32(c, A(c, 0), t); R(c, t); }
static void h_sleep(Mt *m, X86 *c) {
    if (m->dying) x86_fault(c, "engine shut down");
    double s = A(c, 0) / mt_speed();
    usleep((useconds_t)(s * 1e6));
    R(c, 0);
}

// ---------------------------------------------------------------- objects

#define MAX_Q 64
#define MAX_SRC 128
typedef struct { pthread_mutex_t mu; int used; } DQueue;
typedef struct {
    int used, retired, running, armed, cancel_delivered, thread_live;
    uint32_t guest, queue, handler, cancel_handler, ctx;
    double delay_s, period_s;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int rearm;
    int reaps_graph;  // handler is MacinTalk's deferred AUGraph stop
} DSource;
static DQueue g_q[MAX_Q];
static DSource g_src[MAX_SRC];
static pthread_mutex_t g_pool = PTHREAD_MUTEX_INITIALIZER;

#define QMAGIC 0x45555144u  // 'DQUE'
#define SMAGIC 0x43525344u  // 'DSRC'

static DSource *src_of(Mt *m, X86 *c, uint32_t o) {
    (void)m;
    if (!o || rd32(c, o) != SMAGIC) return NULL;
    uint32_t i = rd32(c, o + 4);
    return i < MAX_SRC ? &g_src[i] : NULL;
}
static DQueue *queue_of(Mt *m, X86 *c, uint32_t o) {
    (void)m;
    if (!o || rd32(c, o) != QMAGIC) return NULL;
    uint32_t i = rd32(c, o + 4);
    return i < MAX_Q ? &g_q[i] : NULL;
}

static void h_dispatch_queue_create(Mt *m, X86 *c) {
    pthread_mutex_lock(&g_pool);
    int i;
    for (i = 0; i < MAX_Q && g_q[i].used; i++) {}
    if (i == MAX_Q) { pthread_mutex_unlock(&g_pool); x86_fault(c, "too many dispatch queues"); }
    g_q[i].used = 1;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_q[i].mu, &at);
    pthread_mutex_unlock(&g_pool);
    uint32_t o = mt_static(m, 16);
    wr32(c, o, QMAGIC);
    wr32(c, o + 4, (uint32_t)i);
    R(c, o);
}
static void h_noop(Mt *m, X86 *c) { (void)m; (void)c; }

static void h_dispatch_source_create(Mt *m, X86 *c) {
    pthread_mutex_lock(&g_pool);
    int i;
    for (i = 0; i < MAX_SRC; i++) if (!g_src[i].used || (g_src[i].retired && !g_src[i].thread_live)) break;
    if (i == MAX_SRC) { pthread_mutex_unlock(&g_pool); fprintf(stderr, "mt: all dispatch sources in use\n"); R(c, 0); return; }
    DSource *s = &g_src[i];
    uint32_t guest = s->guest;
    if (s->used) { pthread_mutex_destroy(&s->mu); pthread_cond_destroy(&s->cv); }
    memset(s, 0, sizeof *s);
    s->used = 1;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->queue = A(c, 3);
    pthread_mutex_unlock(&g_pool);
    if (!guest) { guest = mt_static(m, 16); wr32(c, guest, SMAGIC); wr32(c, guest + 4, (uint32_t)i); }
    s->guest = guest;
    R(c, guest);
}
static void h_dispatch_set_context(Mt *m, X86 *c) { DSource *s = src_of(m, c, A(c, 0)); if (s) s->ctx = A(c, 1); }
static void h_dispatch_get_context(Mt *m, X86 *c) { DSource *s = src_of(m, c, A(c, 0)); R(c, s ? s->ctx : 0); }
static uint32_t deferred_stop_fn(Mt *m) {
    static Mt *owner;
    static uint32_t fn;
    if (owner != m) {
        owner = m;
        fn = 0;
        for (int i = 0; i < m->nimages && !fn; i++) fn = mt_export(m, i, "_MTBEAudioUnitDeferredStopAudioGraph");
    }
    return fn;
}
static void h_set_event_handler_f(Mt *m, X86 *c) {
    DSource *s = src_of(m, c, A(c, 0));
    if (!s) return;
    s->handler = A(c, 1);
    uint32_t ds = deferred_stop_fn(m);
    s->reaps_graph = ds && s->handler == ds;
}
static void h_set_cancel_handler_f(Mt *m, X86 *c) { DSource *s = src_of(m, c, A(c, 0)); if (s) s->cancel_handler = A(c, 1); }

// dispatch_source_set_timer(source, start, interval, leeway): three 64-bit values
static void h_dispatch_source_set_timer(Mt *m, X86 *c) {
    DSource *s = src_of(m, c, A(c, 0));
    if (!s) return;
    uint64_t start = (uint64_t)A(c, 1) | (uint64_t)A(c, 2) << 32;
    uint64_t interval = (uint64_t)A(c, 3) | (uint64_t)A(c, 4) << 32;
    const uint64_t NEAR = 3600ull * 24 * 1000000000ull;
    uint64_t wall = mt_wall_us() * 1000ull;
    if (s->reaps_graph) {
        // Lion arms this a few seconds after an utterance ends: that is its
        // end-of-speech signal. Firing it would stop the graph and wedge the
        // engine (panthera-speech), so it never fires.
        extern void mt_audio_mark_done(void);
        const uint64_t NEAR2 = 3600ull * 24 * 1000000000ull;
        uint64_t w = mt_wall_us() * 1000ull;
        if (start != ~0ull && start != 0x7FFFFFFFFFFFFFFFull && start < w + NEAR2) mt_audio_mark_done();
        pthread_mutex_lock(&s->mu);
        s->armed = 0;
        pthread_mutex_unlock(&s->mu);
        return;
    }
    pthread_mutex_lock(&s->mu);
    s->period_s = (interval == 0 || interval > 3600ull * 1000000000ull) ? 0 : interval / 1e9 / mt_speed();
    int near_wall = start < wall + NEAR && start + NEAR > wall;
    if (start == 0) { s->delay_s = 0; s->armed = 1; }
    else if (near_wall) { s->delay_s = start <= wall ? 0 : (double)(start - wall) / 1e9 / mt_speed(); s->armed = 1; }
    else if (start < NEAR) { s->delay_s = start / 1e9 / mt_speed(); s->armed = 1; }  // relative-looking
    else s->armed = 0;  // DISPATCH_TIME_FOREVER: disarm
    if (s->delay_s > 60) s->delay_s = 60;
    s->rearm = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}
static void h_dispatch_walltime(Mt *m, X86 *c) {
    (void)m;
    int64_t delta = (int64_t)((uint64_t)A(c, 1) | (uint64_t)A(c, 2) << 32);
    uint64_t base;
    if (A(c, 0)) base = (uint64_t)rd32(c, A(c, 0)) * 1000000000ull + rd32(c, A(c, 0) + 4);
    else base = mt_wall_us() * 1000ull;
    R64(c, base + (uint64_t)delta);
}
static void h_dispatch_time(Mt *m, X86 *c) {  // monotonic-ish: same wall clock, the engine only adds deltas
    (void)m;
    int64_t delta = (int64_t)((uint64_t)A(c, 2) | (uint64_t)A(c, 3) << 32);
    R64(c, mt_wall_us() * 1000ull + (uint64_t)delta);
}

typedef struct { Mt *m; DSource *s; MtThread *t; } SrcThread;

static void deliver(Mt *m, X86 *c, DSource *s, uint32_t fn) {
    DQueue *q = queue_of(m, c, s->queue);
    if (q) pthread_mutex_lock(&q->mu);
    uint32_t a = s->ctx;
    mt_call_on(m, c, fn, 1, &a);
    if (q) pthread_mutex_unlock(&q->mu);
}

static void *source_main(void *arg) {
    SrcThread *k = arg;
    Mt *m = k->m;
    DSource *s = k->s;
    X86 *c = &k->t->cpu;
    c->fault = &k->t->fault;
    if (setjmp(k->t->fault)) {
        if (!m->dying) mt_log("dispatch source handler faulted: %s", c->err);
        goto out;
    }
    for (;;) {
        pthread_mutex_lock(&s->mu);
        while (s->running && !m->dying && !s->armed) pthread_cond_wait(&s->cv, &s->mu);
        if (!s->running || m->dying) { pthread_mutex_unlock(&s->mu); break; }
        if (s->delay_s > 0) {
            s->rearm = 0;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            double t = ts.tv_sec + ts.tv_nsec / 1e9 + s->delay_s;
            ts.tv_sec = (time_t)t;
            ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
            int r = 0;
            while (!s->rearm && s->running && !m->dying && r == 0) r = pthread_cond_timedwait(&s->cv, &s->mu, &ts);
            if (!s->running || m->dying) { pthread_mutex_unlock(&s->mu); break; }
            if (s->rearm) { pthread_mutex_unlock(&s->mu); continue; }  // re-armed: re-read the delay
        }
        s->armed = 0;
        uint32_t h = s->handler;
        pthread_mutex_unlock(&s->mu);
        if (h) deliver(m, c, s, h);
        pthread_mutex_lock(&s->mu);
        if (s->period_s > 0 && !s->armed) { s->delay_s = s->period_s; s->armed = 1; }
        pthread_mutex_unlock(&s->mu);
        sched_yield();
    }
    // cancellation handler, on the source's queue, once
    pthread_mutex_lock(&g_pool);
    uint32_t ch = 0;
    if (s->retired && !s->cancel_delivered && !m->dying) { s->cancel_delivered = 1; ch = s->cancel_handler; }
    pthread_mutex_unlock(&g_pool);
    if (ch && !setjmp(k->t->fault)) deliver(m, c, s, ch);
out:
    pthread_mutex_lock(&g_pool);
    s->thread_live = 0;
    pthread_mutex_unlock(&g_pool);
    __sync_fetch_and_sub(&m->live, 1);
    free(k->t);
    free(k);
    return NULL;
}

static void h_dispatch_resume(Mt *m, X86 *c) {
    DSource *s = src_of(m, c, A(c, 0));
    if (!s) return;
    pthread_mutex_lock(&g_pool);
    if (s->running || s->thread_live) { pthread_mutex_unlock(&g_pool); return; }
    s->running = 1;
    s->thread_live = 1;
    pthread_mutex_unlock(&g_pool);
    SrcThread *k = calloc(1, sizeof *k);
    k->m = m;
    k->s = s;
    k->t = mt_thread_new(m);
    __sync_fetch_and_add(&m->live, 1);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, source_main, k);
}
static void h_dispatch_source_cancel(Mt *m, X86 *c) {
    DSource *s = src_of(m, c, A(c, 0));
    if (!s) return;
    pthread_mutex_lock(&g_pool);
    s->retired = 1;
    pthread_mutex_unlock(&g_pool);
    pthread_mutex_lock(&s->mu);
    s->running = 0;
    s->armed = 0;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

void mt_gcd_wake(void) {
    for (int i = 0; i < MAX_SRC; i++)
        if (g_src[i].used) { pthread_mutex_lock(&g_src[i].mu); pthread_cond_broadcast(&g_src[i].cv); pthread_mutex_unlock(&g_src[i].mu); }
}
void mt_gcd_reset(void) {
    for (int i = 0; i < MAX_SRC; i++) if (g_src[i].used) { pthread_mutex_destroy(&g_src[i].mu); pthread_cond_destroy(&g_src[i].cv); }
    for (int i = 0; i < MAX_Q; i++) if (g_q[i].used) pthread_mutex_destroy(&g_q[i].mu);
    memset(g_src, 0, sizeof g_src);
    memset(g_q, 0, sizeof g_q);
}

const MtHleEntry mt_hle_gcd[] = {
    {"_gettimeofday", h_gettimeofday}, {"_time", h_time}, {"_sleep", h_sleep},
    {"_dispatch_queue_create", h_dispatch_queue_create}, {"_dispatch_release", h_noop}, {"_dispatch_retain", h_noop},
    {"_dispatch_set_context", h_dispatch_set_context}, {"_dispatch_get_context", h_dispatch_get_context},
    {"_dispatch_source_create", h_dispatch_source_create},
    {"_dispatch_source_set_event_handler_f", h_set_event_handler_f},
    {"_dispatch_source_set_cancel_handler_f", h_set_cancel_handler_f},
    {"_dispatch_source_set_timer", h_dispatch_source_set_timer}, {"_dispatch_resume", h_dispatch_resume},
    {"_dispatch_source_cancel", h_dispatch_source_cancel}, {"_dispatch_walltime", h_dispatch_walltime},
    {"_dispatch_time", h_dispatch_time},
    {NULL, NULL},
};
