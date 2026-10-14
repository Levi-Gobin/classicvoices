// Just enough Grand Central Dispatch for Snow Leopard's MacinTalk, whose back
// end runs on timer sources instead of MP tasks. Timer semantics follow
// panthera-speech's tiger_host_gcd.c; 10.6 computes its deadlines from
// gettimeofday, so the start time of a timer is a (sped-up) wall-clock
// nanosecond count.
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

// Each queue is serial and has one thread, which runs its sources' timers in
// deadline order. 10.6 starts a one-shot source per back-end task, several
// per utterance; a thread for each source would leave interrupted work
// queued up on threads until the engine ran out of them.
#define MAX_Q 64
#define MAX_SRC 128
typedef struct {
    int used, started;
    pthread_cond_t cv;
} DQueue;
typedef struct {
    int used, resumed, armed, retired, cancel_done, busy;
    int q;  // index into g_q
    uint32_t guest, handler, cancel_handler, ctx;
    double deadline, period_s;  // monotonic seconds
    unsigned gen;  // bumped by each set_timer
    int reaps_graph;  // handler is MacinTalk's deferred AUGraph stop
} DSource;
static DQueue g_q[MAX_Q];
static DSource g_src[MAX_SRC];
static uint32_t g_qguest[MAX_Q];
static int g_default_q = -1;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // all queue and source state

#define QMAGIC 0x45555144u  // 'DQUE'
#define SMAGIC 0x43525344u  // 'DSRC'

static DSource *src_of(Mt *m, X86 *c, uint32_t o) {
    (void)m;
    if (!o || rd32(c, o) != SMAGIC) return NULL;
    uint32_t i = rd32(c, o + 4);
    return i < MAX_SRC ? &g_src[i] : NULL;
}

// with g_mu held; -1 when full
static int new_queue(Mt *m, X86 *c) {
    int i;
    for (i = 0; i < MAX_Q && g_q[i].used; i++) {}
    if (i == MAX_Q) return -1;
    g_q[i].used = 1;
    g_q[i].started = 0;
    pthread_cond_init(&g_q[i].cv, NULL);
    if (!g_qguest[i]) { g_qguest[i] = mt_static(m, 16); wr32(c, g_qguest[i], QMAGIC); wr32(c, g_qguest[i] + 4, (uint32_t)i); }
    return i;
}

static void h_dispatch_queue_create(Mt *m, X86 *c) {
    pthread_mutex_lock(&g_mu);
    int i = new_queue(m, c);
    pthread_mutex_unlock(&g_mu);
    if (i < 0) x86_fault(c, "too many dispatch queues");
    R(c, g_qguest[i]);
}
static void h_noop(Mt *m, X86 *c) { (void)m; (void)c; }

static void h_dispatch_source_create(Mt *m, X86 *c) {
    uint32_t qo = A(c, 3);
    pthread_mutex_lock(&g_mu);
    int qi = -1;
    if (qo && rd32(c, qo) == QMAGIC && rd32(c, qo + 4) < MAX_Q) qi = (int)rd32(c, qo + 4);
    else {  // a global queue: one shared serial queue stands in
        if (g_default_q < 0) g_default_q = new_queue(m, c);
        qi = g_default_q;
    }
    int i;
    for (i = 0; i < MAX_SRC && g_src[i].used; i++) {}
    if (i == MAX_SRC || qi < 0) { pthread_mutex_unlock(&g_mu); mt_log("mt: all dispatch sources in use"); R(c, 0); return; }
    DSource *s = &g_src[i];
    uint32_t guest = s->guest;
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->q = qi;
    pthread_mutex_unlock(&g_mu);
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
        if (start != ~0ull && start != 0x7FFFFFFFFFFFFFFFull && start < wall + NEAR) mt_audio_mark_done();
        pthread_mutex_lock(&g_mu);
        s->armed = 0;
        pthread_mutex_unlock(&g_mu);
        return;
    }
    pthread_mutex_lock(&g_mu);
    s->period_s = (interval == 0 || interval > 3600ull * 1000000000ull) ? 0 : interval / 1e9 / mt_speed();
    double delay = 0;
    s->armed = 1;
    if (start == 0) delay = 0;
    else if (start < NEAR) delay = start / 1e9 / mt_speed();  // relative-looking
    // Already due, however long ago: 10.6 adds its delays to times it took
    // earlier, and the guest clock runs fast, so a day-old base is a few
    // minutes of speech away.
    else if (start <= wall) delay = 0;
    else if (start - wall < NEAR) delay = (double)(start - wall) / 1e9 / mt_speed();
    else s->armed = 0;  // DISPATCH_TIME_FOREVER: disarm
    if (delay > 60) delay = 60;
    s->deadline = mono_s() + delay;
    s->gen++;
    pthread_cond_broadcast(&g_q[s->q].cv);
    pthread_mutex_unlock(&g_mu);
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

typedef struct { Mt *m; int q; MtThread *t; } QueueThread;

static void wait_until(pthread_cond_t *cv, double deadline) {
    double left = deadline - mono_s();
    if (left <= 0) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9 + left;
    ts.tv_sec = (time_t)t;
    ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
    pthread_cond_timedwait(cv, &g_mu, &ts);
}

static void *queue_main(void *arg) {
    QueueThread *k = arg;
    Mt *m = k->m;
    X86 *c = &k->t->cpu;
    DQueue *q = &g_q[k->q];
    c->fault = &k->t->fault;
    DSource *volatile running = NULL;
    if (setjmp(k->t->fault)) {
        // The handler faulted. Log it and carry on with the queue's other work.
        char at[160], hn[160];
        if (!m->dying) m->broken = 1;
        if (!m->dying) mt_log("dispatch source handler %s faulted: %s in %s", running ? mt_symbolize(m, running->handler, hn, sizeof hn) : "?",
                              c->err, mt_symbolize(m, c->eip, at, sizeof at));
        pthread_mutex_lock(&g_mu);
        if (running) { running->busy = 0; if (running->retired && running->cancel_done) running->used = 0; }
        running = NULL;
        pthread_mutex_unlock(&g_mu);
        if (m->dying) goto out;
        c->r[ESP] = k->t->stack_top - 16;
    }
    pthread_mutex_lock(&g_mu);
    while (!m->dying) {
        // a cancelled source's cancel handler first, then the earliest due timer
        DSource *pick = NULL;
        uint32_t fn = 0;
        unsigned gen = 0;
        double now = mono_s(), soonest = INFINITY;
        for (int i = 0; i < MAX_SRC; i++) {
            DSource *s = &g_src[i];
            if (!s->used || s->q != k->q || s->busy) continue;
            if (s->retired) {
                if (!s->cancel_done) { s->cancel_done = 1; pick = s; fn = s->cancel_handler; break; }
                continue;
            }
            if (!s->resumed || !s->armed || !s->handler) continue;
            if (s->deadline <= now) { if (!pick || s->deadline < pick->deadline) pick = s; }
            else if (s->deadline < soonest) soonest = s->deadline;
        }
        if (!pick) {
            if (soonest == INFINITY) pthread_cond_wait(&q->cv, &g_mu);
            else wait_until(&q->cv, soonest);
            continue;
        }
        if (!pick->retired) { fn = pick->handler; pick->armed = 0; gen = pick->gen; }
        if (pick->retired && !fn) { pick->used = 0; continue; }
        pick->busy = 1;
        running = pick;
        uint32_t a = pick->ctx;
        pthread_mutex_unlock(&g_mu);
#ifdef MT_TEST_HOOKS
        if (getenv("MT_TEST_BREAK") && !strcmp(getenv("MT_TEST_BREAK"), "fault")) x86_fault(c, "test fault");
#endif
        mt_call_on(m, c, fn, 1, &a);
        pthread_mutex_lock(&g_mu);
        running = NULL;
        pick->busy = 0;
        if (pick->retired && pick->cancel_done) pick->used = 0;
        else if (pick->period_s > 0 && pick->gen == gen && !pick->retired) { pick->deadline = mono_s() + pick->period_s; pick->armed = 1; }
    }
    pthread_mutex_unlock(&g_mu);
out:
    __sync_fetch_and_sub(&m->live, 1);
    mt_thread_free(k->t);
    free(k);
    return NULL;
}

static void h_dispatch_resume(Mt *m, X86 *c) {
    DSource *s = src_of(m, c, A(c, 0));
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    s->resumed = 1;
    DQueue *q = &g_q[s->q];
    int start = !q->started;
    q->started = 1;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&g_mu);
    if (!start) return;
    QueueThread *k = calloc(1, sizeof *k);
    k->m = m;
    k->q = s->q;
    k->t = mt_thread_new(m);
    if (!k->t) {
        free(k);
        pthread_mutex_lock(&g_mu);
        q->started = 0;
        pthread_mutex_unlock(&g_mu);
        x86_fault(c, "too many engine threads");
    }
    __sync_fetch_and_add(&m->live, 1);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, queue_main, k);
    pthread_attr_destroy(&at);
}
static void h_dispatch_source_cancel(Mt *m, X86 *c) {
    DSource *s = src_of(m, c, A(c, 0));
    if (!s) return;
    pthread_mutex_lock(&g_mu);
    if (!s->retired) {
        s->retired = 1;
        s->armed = 0;
        // never resumed: no queue thread will look at it
        if (!s->resumed && !s->busy) s->used = 0;
        pthread_cond_broadcast(&g_q[s->q].cv);
    }
    pthread_mutex_unlock(&g_mu);
}

void mt_gcd_wake(void) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_Q; i++) if (g_q[i].used) pthread_cond_broadcast(&g_q[i].cv);
    pthread_mutex_unlock(&g_mu);
}
void mt_gcd_reset(void) {
    for (int i = 0; i < MAX_Q; i++) if (g_q[i].used) pthread_cond_destroy(&g_q[i].cv);
    memset(g_src, 0, sizeof g_src);
    memset(g_q, 0, sizeof g_q);
    memset(g_qguest, 0, sizeof g_qguest);
    g_default_q = -1;
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
