// i386 interpreter run loop, faults and the hand-over to recompiled code.
// Instruction semantics live in x86_ops.h.
#include "x86_ops.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The X86 this thread is running, for faults raised from x86_ops.h.
static _Thread_local X86 *tls_cpu;

static pthread_mutex_t g_atomic = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int g_in_atomic;
void x86_atomic_begin(void) { pthread_mutex_lock(&g_atomic); g_in_atomic = 1; }
void x86_atomic_end(void) { g_in_atomic = 0; pthread_mutex_unlock(&g_atomic); }

void x86_fault(X86 *c, const char *fmt, ...) {
    if (g_in_atomic) x86_atomic_end();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    if (c->fault) longjmp(*c->fault, 1);
    fprintf(stderr, "x86 fault: %s\n", c->err);
    __builtin_trap();
}

void x86_kfault(const char *fmt, ...) {
    if (g_in_atomic) x86_atomic_end();
    X86 *c = tls_cpu;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!c) { fprintf(stderr, "x86 fault: %s\n", buf); __builtin_trap(); }
    x86_fault(c, "%s", buf);
}

void x86_init(X86 *c, uint8_t *mem) {
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->mem_size = X86_MEM_SIZE;
    c->mxcsr = 0x1F80;
    c->fcw = 0x037F;
    for (int i = 0; i < 8; i++) c->tag[i] = 1;
}

uint32_t x86_eflags(const X86 *c) { return k_eflags(&c->k); }
void x86_set_eflags(X86 *c, uint32_t v) { k_set_eflags(&c->k, v); }
void x86_fpush(X86 *c, double v) { k_fpush(&c->k, v); }
double x86_fpop(X86 *c) { return k_fpop(&c->k); }

// Interpret the instruction at c->eip; returns its length.
static uint32_t interpret(X86 *c) {
    uint32_t eip = c->eip;
    if (__builtin_expect(eip < 0x1000 || eip > c->mem_size - 16, 0))
        x86_fault(c, "bad access %#x (16 bytes) at eip %#x (esp %#x)", eip, eip, c->r[ESP]);
    uint64_t b[2];
    memcpy(b, c->mem + eip, 16);
    c->icount++;
    return x86_exec(&c->k, b[0], b[1]);
}

#ifdef X86_TRACE
// the last trap each guest thread made, for x86_trace_threads
static struct { X86 *c; uint32_t trap, caller, esp; } g_last[64];
static void trace_trap(X86 *c) {
    for (int i = 0; i < 64; i++) {
        if (g_last[i].c && g_last[i].c != c) continue;
        if (!g_last[i].c && !__sync_bool_compare_and_swap(&g_last[i].c, NULL, c)) continue;
        g_last[i].trap = c->eip;
        g_last[i].esp = c->r[ESP];
        g_last[i].caller = c->r[ESP] < c->mem_size - 4 ? *(uint32_t *)(void *)(c->mem + c->r[ESP]) : 0;
        break;
    }
}
void x86_trace_threads(void (*show)(uint32_t trap, uint32_t caller, uint32_t esp, uint32_t eip)) {
    for (int i = 0; i < 64 && g_last[i].c; i++) show(g_last[i].trap, g_last[i].caller, g_last[i].esp, g_last[i].c->eip);
}
#else
#define trace_trap(c) ((void)0)
#endif

static inline void do_trap(X86 *c) {
    uint32_t at = c->eip;
    trace_trap(c);
    c->trap(c, at, c->trap_ctx);
    if (c->eip == at) c->eip = x86_pop(c);  // plain host function: return to caller
}

void x86_step(X86 *c) {
    X86 *prev = tls_cpu;
    tls_cpu = c;
    if (c->eip >= c->trap_lo && c->eip < c->trap_hi) {
        c->icount++;
        do_trap(c);
    } else {
        interpret(c);
    }
    tls_cpu = prev;
}

static X86NativeFn lookup_native(const X86 *c, uint32_t eip) {
    // debugging: MT_NATIVE_RANGE=lo-hi[,lo-hi...] (hex) limits native code to those ranges
    static int nranges = -1;
    static uint32_t range[16][2];
    if (nranges < 0) {
        const char *e = getenv("MT_NATIVE_RANGE");
        int n = 0, used;
        while (e && n < 16 && sscanf(e, "%x-%x%n", &range[n][0], &range[n][1], &used) == 2) {
            n++;
            e += used;
            if (*e != ',') break;
            e++;
        }
        nranges = e ? n : 0;
    }
    if (nranges) {
        int in = 0;
        for (int i = 0; i < nranges && !in; i++) in = eip >= range[i][0] && eip < range[i][1];
        if (!in) return NULL;
    }
    size_t lo = 0, hi = c->nnative;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t a = c->native[mid].addr;
        if (a == eip) return c->native[mid].fn;
        if (a < eip) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// The lowest stack address recompiled code may reach on this thread, leaving
// room for host functions (traps) below it.
static uintptr_t stack_limit(void) {
    static _Thread_local uintptr_t limit;
    if (limit) return limit;
    uintptr_t lo = 0;
#ifdef __APPLE__
    pthread_t self = pthread_self();
    lo = (uintptr_t)pthread_get_stackaddr_np(self) - pthread_get_stacksize_np(self);
#else
    pthread_attr_t at;
    void *addr;
    size_t size;
    if (!pthread_getattr_np(pthread_self(), &at)) {
        if (!pthread_attr_getstack(&at, &addr, &size)) lo = (uintptr_t)addr;
        pthread_attr_destroy(&at);
    }
#endif
    // 256 KB for traps; if the stack is unknown, recompiled code still gets a
    // generous 1 MB below the current frame
    limit = lo ? lo + (256u << 10) : (uintptr_t)__builtin_frame_address(0) - (1u << 20);
    return limit;
}

// Runs until eip reaches c->stop (returns 1), or, when nested, until eip ==
// ret with esp at ret_esp or up to X86_CALLEE_POP above it (returns 0).
static int run(X86 *c, int nested, uint32_t ret, uint32_t ret_esp) {
    uint32_t n = 0, block = 0;
    int check = 1;  // control just moved somewhere other than the next instruction
    int stay = 0;   // recompiled code handed back without moving: interpret the next instruction
    for (;;) {
        uint32_t eip = c->eip;
        if (eip == c->stop) return 1;
        if (nested && eip == ret && c->r[ESP] - ret_esp <= X86_CALLEE_POP) return 0;
        if (eip >= c->trap_lo && eip < c->trap_hi) {
            c->icount++;
            do_trap(c);
            check = 1;
            continue;
        }
        if (check) {
            block = eip;
            if (c->nnative && !stay && (uintptr_t)__builtin_frame_address(0) > c->stack_limit) {
                X86NativeFn f = lookup_native(c, eip);
                if (f) {
                    uint32_t esp = c->r[ESP];
                    f(c, eip);
                    if (c->abort && *c->abort) x86_fault(c, "engine shut down");
                    stay = c->eip == eip && c->r[ESP] == esp;
                    continue;
                }
            }
            stay = 0;
        }
        if (c->profile && block >= c->profile_lo && block < c->profile_hi) c->profile[block - c->profile_lo]++;
        uint32_t len = interpret(c);
        check = c->eip != eip + len;
        if (!(++n & 4095) && c->abort && *c->abort) x86_fault(c, "engine shut down");
    }
}

void x86_run(X86 *c) {
    X86 *prev = tls_cpu;
    tls_cpu = c;
    c->stack_limit = stack_limit();
    run(c, 0, 0, 0);
    tls_cpu = prev;
}

int x86_rt_call(X86 *c, uint32_t ret) {
    return run(c, 1, ret, c->r[ESP] + 4);
}

// What recompiled code polls for an engine shutdown when the X86 has no abort flag.
const int x86_rt_never = 0;

