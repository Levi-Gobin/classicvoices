// Shared declarations for the MacinTalk host's HLE modules.
#pragma once
#include "mt.h"
#include <pthread.h>

#define MT_TLS_KEYS 64

// One guest thread. The CPU comes first so an X86* is also an MtThread*.
typedef struct MtThread {
    X86 cpu;
    Mt *m;
    uint32_t self;        // guest pthread_t / MPTaskID value
    uint32_t errno_addr;  // guest int for ___error()
    uint32_t keys[MT_TLS_KEYS];
    uint32_t stack_top;
    jmp_buf fault;
} MtThread;

static inline MtThread *mt_tls(Mt *m, X86 *c) { (void)m; return (MtThread *)c; }

typedef struct { const char *name; MtHle fn; } MtHleEntry;

// Name hashing for the import tables (binding looks up thousands of names
// each time an engine opens).
static inline uint32_t mt_str_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

const char *mt_symbolize(Mt *m, uint32_t a, char *out, size_t cap);
void mt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// call a guest function on a given CPU (any thread); returns eax
uint32_t mt_call_on(Mt *m, X86 *c, uint32_t fn, int nargs, const uint32_t *args);
void mt_check(Mt *m, X86 *c, uint32_t a, uint32_t n);
// new guest thread state (stack, TLS); the CPU shares m->mem
// NULL when every stack is in use
MtThread *mt_thread_new(Mt *m);
void mt_thread_free(MtThread *t);
void mt_lock(Mt *m, X86 *c, pthread_mutex_t *mu);
// Start a detached host thread for the engine (a guest thread, the pacer):
// a 16 MB stack, since recompiled code recurses on the C stack, and the
// caller's interactive priority, so speech isn't left on efficiency cores.
int mt_thread_start(void *(*fn)(void *), void *arg);

extern const MtHleEntry mt_hle_libc[];

// audio capture (mt_audio.c)
void mt_audio_begin(void);
uint32_t mt_audio_wait(double quiet_s, double max_s);
const float *mt_audio_pcm(uint32_t *n);
double mt_audio_rate(void);
// CF helpers (mt_cf.c)
uint32_t mt_cf_bundle(Mt *m, const char *dir);
void mt_cf_set_dictionary_bundle(Mt *m, const char *dir);
uint32_t mt_cf_string(Mt *m, const char *s, size_t n);
// clock (mt_mp.c)
void mt_set_speed(double s);
double mt_speed(void);
