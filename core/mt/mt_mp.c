// Time and Multiprocessing Services for the MacinTalk guest.
//
// The engine schedules against the clock: its worker computes the delay to
// its next event and waits that long. As in panthera-speech, the guest's clock
// runs g_speed times faster than real time and every wait is divided by the
// same factor, which compresses the whole timeline (output is unchanged; see
// panthera's tiger_host_shims.c, where 128x was measured byte-identical).
#include "mt_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double g_speed = 128.0;

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }
static inline void R64(X86 *c, uint64_t v) { c->r[EAX] = (uint32_t)v; c->r[EDX] = (uint32_t)(v >> 32); }

void mt_set_speed(double s) { g_speed = s > 0 ? s : 1; }
double mt_speed(void) { return g_speed; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// AbsoluteTime: nanoseconds of (sped-up) guest time
#define TICKS_PER_S 1e9
static void h_UpTime(Mt *m, X86 *c) { (void)m; R64(c, (uint64_t)(now_s() * g_speed * TICKS_PER_S)); }

// Duration: positive = milliseconds, negative = -microseconds, 0x7fffffff = forever
static int32_t ticks_to_duration(int64_t ticks) {
    double ms = (double)ticks * 1000.0 / TICKS_PER_S;
    if (ms > 2147483000.0) return 0x7fffffff;
    if (ms < -2147483000.0) return -0x7ffffffe;
    if (ms > -1.0 && ms < 1.0) return (int32_t)(-ms * 1000.0);
    return (int32_t)ms;
}
static int64_t duration_to_ticks(int32_t d) {
    if (d == 0x7fffffff) return (int64_t)(TICKS_PER_S * 3600);
    if (d < 0) return (int64_t)(-(double)d * TICKS_PER_S / 1e6);
    return (int64_t)((double)d * TICKS_PER_S / 1e3);
}
static inline int64_t a64(X86 *c, int i) { return (int64_t)((uint64_t)A(c, i) | (uint64_t)A(c, i + 1) << 32); }
static void h_AbsoluteDeltaToDuration(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)ticks_to_duration(a64(c, 0) - a64(c, 2))); }
static void h_AbsoluteToDuration(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)ticks_to_duration(a64(c, 0))); }
static void h_AddDurationToAbsolute(Mt *m, X86 *c) { (void)m; R64(c, (uint64_t)(a64(c, 1) + duration_to_ticks((int32_t)A(c, 0)))); }
static void h_SubDurationFromAbsolute(Mt *m, X86 *c) { (void)m; R64(c, (uint64_t)(a64(c, 1) - duration_to_ticks((int32_t)A(c, 0)))); }

// a wait of `d` (Duration) in guest time, as a host deadline; 0 = forever
static double deadline_for(int32_t d) {
    if (d == 0x7fffffff) return 0;
    double ms = d < 0 ? -(double)d / 1000.0 : (double)d;
    return now_s() + ms / 1000.0 / g_speed;
}

void h_usleep(Mt *m, X86 *c) {
    if (m->dying) x86_fault(c, "engine shut down");
    double us = (double)A(c, 0) / g_speed;
    if (us >= 1) usleep((useconds_t)us);
    R(c, 0);
}

static void h_OSMemoryBarrier(Mt *m, X86 *c) { (void)m; (void)c; __sync_synchronize(); }

// ---------------------------------------------------------------- critical regions

typedef struct { pthread_mutex_t mu; } Region;
#define MAX_OBJ 512
static Region g_regions[MAX_OBJ];
static int g_nregions;
static pthread_mutex_t g_obj_mu = PTHREAD_MUTEX_INITIALIZER;

// guest IDs are small guest objects holding a host index: {magic, index}
static uint32_t new_id(Mt *m, uint32_t magic, uint32_t idx) {
    uint32_t a = mt_static(m, 8);
    memcpy(G(m, a), &magic, 4);
    memcpy(G(m, a + 4), &idx, 4);
    return a;
}
static uint32_t id_index(Mt *m, X86 *c, uint32_t id, uint32_t magic) {
    (void)m;
    if (!id || rd32(c, id) != magic) x86_fault(c, "bad MP object %#x", id);
    return rd32(c, id + 4);
}

static void h_MPCreateCriticalRegion(Mt *m, X86 *c) {
    pthread_mutex_lock(&g_obj_mu);
    int i = g_nregions++;
    pthread_mutex_unlock(&g_obj_mu);
    if (i >= MAX_OBJ) x86_fault(c, "too many critical regions");
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_regions[i].mu, &at);
    wr32(c, A(c, 0), new_id(m, 0x4D505247u, (uint32_t)i));  // 'MPRG'
    R(c, 0);
}
static void h_MPEnterCriticalRegion(Mt *m, X86 *c) {
    pthread_mutex_lock(&g_regions[id_index(m, c, A(c, 0), 0x4D505247u)].mu);
    R(c, 0);
}
static void h_MPExitCriticalRegion(Mt *m, X86 *c) {
    pthread_mutex_unlock(&g_regions[id_index(m, c, A(c, 0), 0x4D505247u)].mu);
    R(c, 0);
}

// ---------------------------------------------------------------- queues

#define QCAP 256
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t msg[QCAP][3];
    int head, tail, count;
} Queue;
static Queue g_queues[MAX_OBJ];
static int g_nqueues;

static void h_MPCreateQueue(Mt *m, X86 *c) {
    pthread_mutex_lock(&g_obj_mu);
    int i = g_nqueues++;
    pthread_mutex_unlock(&g_obj_mu);
    if (i >= MAX_OBJ) x86_fault(c, "too many MP queues");
    pthread_mutex_init(&g_queues[i].mu, NULL);
    pthread_cond_init(&g_queues[i].cv, NULL);
    wr32(c, A(c, 0), new_id(m, 0x4D505155u, (uint32_t)i));  // 'MPQU'
    R(c, 0);
}
static int queue_notify(Queue *q, uint32_t a, uint32_t b, uint32_t d) {
    pthread_mutex_lock(&q->mu);
    if (q->count >= QCAP) { pthread_mutex_unlock(&q->mu); return -1; }
    q->msg[q->tail][0] = a; q->msg[q->tail][1] = b; q->msg[q->tail][2] = d;
    q->tail = (q->tail + 1) % QCAP;
    q->count++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return 0;
}
static void h_MPNotifyQueue(Mt *m, X86 *c) {
    Queue *q = &g_queues[id_index(m, c, A(c, 0), 0x4D505155u)];
    R(c, (uint32_t)queue_notify(q, A(c, 1), A(c, 2), A(c, 3)));
}
static void h_MPWaitOnQueue(Mt *m, X86 *c) {
    Queue *q = &g_queues[id_index(m, c, A(c, 0), 0x4D505155u)];
    uint32_t p1 = A(c, 1), p2 = A(c, 2), p3 = A(c, 3);
    double deadline = deadline_for((int32_t)A(c, 4));
    pthread_mutex_lock(&q->mu);
    while (!q->count) {
        if (m->dying) { pthread_mutex_unlock(&q->mu); x86_fault(c, "engine shut down"); }
        if (deadline == 0) pthread_cond_wait(&q->cv, &q->mu);
        else {
            double left = deadline - now_s();
            if (left <= 0) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            double t = ts.tv_sec + ts.tv_nsec / 1e9 + left;
            ts.tv_sec = (time_t)t;
            ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
            pthread_cond_timedwait(&q->cv, &q->mu, &ts);
        }
    }
    if (!q->count) { pthread_mutex_unlock(&q->mu); R(c, (uint32_t)-30988); return; }  // kMPTimeoutErr
    if (p1) wr32(c, p1, q->msg[q->head][0]);
    if (p2) wr32(c, p2, q->msg[q->head][1]);
    if (p3) wr32(c, p3, q->msg[q->head][2]);
    q->head = (q->head + 1) % QCAP;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    R(c, 0);
}

// ---------------------------------------------------------------- tasks

typedef struct {
    Mt *m;
    MtThread *t;
    uint32_t entry, param, notify, t1, t2;
} Task;

static void *task_main(void *arg) {
    Task *k = arg;
    Mt *m = k->m;
    X86 *c = &k->t->cpu;
    c->fault = &k->t->fault;
    uint32_t status = 0;
    if (setjmp(k->t->fault)) {
        if (!m->dying) mt_log("MP task %#x faulted: %s", k->entry, c->err);
        status = (uint32_t)-1;
    } else {
        status = mt_call_on(m, c, k->entry, 1, &k->param);
    }
    if (k->notify && !m->dying) {
        Queue *q = &g_queues[rd32(c, k->notify + 4)];
        queue_notify(q, k->t1, k->t2, status);
    }
    __sync_fetch_and_sub(&m->live, 1);
    free(k->t);
    free(k);
    return NULL;
}

static void h_MPCreateTask(Mt *m, X86 *c) {
    Task *k = calloc(1, sizeof *k);
    k->m = m;
    k->entry = A(c, 0);
    k->param = A(c, 1);
    k->notify = A(c, 3);
    k->t1 = A(c, 4);
    k->t2 = A(c, 5);
    k->t = mt_thread_new(m);
    if (m->verbose) fprintf(stderr, "[mp] CreateTask entry=%#x param=%#x\n", k->entry, k->param);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 1 << 20);
    __sync_fetch_and_add(&m->live, 1);
    if (pthread_create(&th, &at, task_main, k)) { __sync_fetch_and_sub(&m->live, 1); R(c, (uint32_t)-108); return; }
    if (A(c, 7)) wr32(c, A(c, 7), k->t->self);
    R(c, 0);
}
static void h_MPTerminateTask(Mt *m, X86 *c) { (void)m; R(c, 0); }

// ---------------------------------------------------------------- data imports / ctype

#define RUNE_RUNETYPE 0x34
static uint32_t rune_mask(int ch) {
    uint32_t v = 0;
    if (ch < 0 || ch > 255) return 0;
    if (ch < 128) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) v |= 0x100;
        if (ch < 32 || ch == 127) v |= 0x200;
        if (ch >= '0' && ch <= '9') v |= 0x400 | (uint32_t)(ch - '0');
        if (ch > 32 && ch < 127) v |= 0x800;
        if (ch >= 'a' && ch <= 'z') v |= 0x1000;
        if (ch > 32 && ch < 127 && !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) v |= 0x2000;
        if (ch == ' ' || (ch >= 9 && ch <= 13)) v |= 0x4000;
        if (ch >= 'A' && ch <= 'Z') v |= 0x8000;
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) {
            v |= 0x10000;
            if (!(ch >= '0' && ch <= '9')) v |= (uint32_t)((ch | 0x20) - 'a' + 10);
        }
        if (ch == ' ' || ch == '\t') v |= 0x20000;
        if (ch >= 32 && ch < 127) v |= 0x40000;
    }
    return v;
}
uint32_t mt_rune_mask(uint32_t ch) { return rune_mask((int)ch); }

void mt_init_data_import(Mt *m, const char *name, uint32_t addr) {
    if (!strcmp(name, "__DefaultRuneLocale")) {
        uint8_t *p = G(m, addr);
        memcpy(p, "RuneMagi", 8);
        memcpy(p + 8, "NONE", 5);
        for (int ch = 0; ch < 256; ch++) {
            uint32_t t = rune_mask(ch), lo = (ch >= 'A' && ch <= 'Z') ? (uint32_t)ch + 32 : (uint32_t)ch;
            uint32_t up = (ch >= 'a' && ch <= 'z') ? (uint32_t)ch - 32 : (uint32_t)ch;
            memcpy(p + RUNE_RUNETYPE + 4 * ch, &t, 4);
            memcpy(p + RUNE_RUNETYPE + 1024 + 4 * ch, &lo, 4);
            memcpy(p + RUNE_RUNETYPE + 2048 + 4 * ch, &up, 4);
        }
        return;
    }
    if (!strcmp(name, "___stderrp") || !strcmp(name, "___stdoutp") || !strcmp(name, "___stdinp")) {
        uint32_t f = mt_static(m, 16);
        memcpy(G(m, f), "MTFL", 4);
        uint32_t idx = name[5] == 'e' ? 2 : name[5] == 'o' ? 1 : 0;
        memcpy(G(m, f + 4), &idx, 4);
        memcpy(G(m, addr), &f, 4);
        return;
    }
    if (!strcmp(name, "___sF")) {  // Tiger's stdin/stdout/stderr FILE array (88 bytes each)
        for (uint32_t i = 0; i < 3; i++) { memcpy(G(m, addr + 88 * i), "MTFL", 4); memcpy(G(m, addr + 88 * i + 4), &i, 4); }
        return;
    }
    if (!strcmp(name, "___stack_chk_guard")) { uint32_t v = 0x00000AFFu; memcpy(G(m, addr), &v, 4); return; }
    if (!strcmp(name, "___mb_cur_max")) { uint32_t v = 1; memcpy(G(m, addr), &v, 4); return; }
    extern void mt_cf_init_data(Mt * m, const char *name, uint32_t addr);
    extern void mt_lion_init_data(Mt * m, const char *name, uint32_t addr);
    mt_cf_init_data(m, name, addr);
    mt_lion_init_data(m, name, addr);
}

void mt_mp_wake(void) {
    for (int i = 0; i < g_nqueues; i++) { pthread_mutex_lock(&g_queues[i].mu); pthread_cond_broadcast(&g_queues[i].cv); pthread_mutex_unlock(&g_queues[i].mu); }
}
void mt_mp_reset(void) {
    for (int i = 0; i < g_nqueues; i++) { pthread_mutex_destroy(&g_queues[i].mu); pthread_cond_destroy(&g_queues[i].cv); memset(&g_queues[i], 0, sizeof g_queues[i]); }
    for (int i = 0; i < g_nregions; i++) pthread_mutex_destroy(&g_regions[i].mu);
    g_nqueues = g_nregions = 0;
}

const MtHleEntry mt_hle_mp[] = {
    {"_UpTime", h_UpTime}, {"_AbsoluteDeltaToDuration", h_AbsoluteDeltaToDuration},
    {"_AbsoluteToDuration", h_AbsoluteToDuration}, {"_AddDurationToAbsolute", h_AddDurationToAbsolute},
    {"_SubDurationFromAbsolute", h_SubDurationFromAbsolute}, {"_OSMemoryBarrier", h_OSMemoryBarrier},
    {"_MPCreateCriticalRegion", h_MPCreateCriticalRegion}, {"_MPEnterCriticalRegion", h_MPEnterCriticalRegion},
    {"_MPExitCriticalRegion", h_MPExitCriticalRegion}, {"_MPCreateQueue", h_MPCreateQueue},
    {"_MPNotifyQueue", h_MPNotifyQueue}, {"_MPWaitOnQueue", h_MPWaitOnQueue},
    {"_MPCreateTask", h_MPCreateTask}, {"_MPTerminateTask", h_MPTerminateTask},
    {NULL, NULL},
};
