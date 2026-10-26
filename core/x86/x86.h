// i386 interpreter for Apple's Mac OS X 10.4-10.7 MacinTalk engines.
//
// Same model as the ARM side (thumb.h): a flat 32-bit guest address space at
// `mem`, faults reported through a callback, and imported functions reached
// through trap addresses that the host implements (HLE). No JIT, so it runs
// inside an iOS audio-unit extension. Hot functions can also be recompiled
// ahead of time to C (tools/recomp_x86.py); the interpreter hands over to that
// code through `native`.
#pragma once
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#define X86_MEM_SIZE 0x80000000u  // 2 GB guest space, reserved, mostly unbacked (the most; see X86.mem_size)

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

typedef union { uint64_t q[2]; uint32_t d[4]; uint16_t w[8]; uint8_t b[16]; float f[4]; double g[2]; } X86Xmm;

// The state instructions work on (x86_ops.h). Recompiled code keeps a copy in
// a local, which the compiler can hold in registers.
#define X86_CORE_FIELDS \
    uint32_t r[8]; \
    uint32_t eip; \
    uint32_t cf, pf, af, zf, sf, of, df;  /* flags kept unpacked (0/1) */ \
    uint32_t fs_base, gs_base; \
    X86Xmm xmm[8]; \
    uint32_t mxcsr; \
    double st[8];          /* x87: values kept as host doubles (arm64 long double is double) */ \
    int top;               /* physical index of ST(0) */ \
    uint8_t tag[8];        /* 1 = empty */ \
    uint16_t fcw, fsw_cc;  /* control word; condition bits C0..C3 in fsw layout */ \
    uint8_t *mem; \
    uint32_t mem_size;     /* bytes mapped at mem (X86_MEM_SIZE, or less where a process may not map that much) */ \
    int div0_as_one;       /* integer divide by zero retries with divisor 1 instead of faulting */ \
    uint64_t icount;       /* instructions interpreted (rdtsc reads it) */

typedef struct X86Core { X86_CORE_FIELDS } X86Core;

typedef struct X86 X86;
typedef void (*X86Trap)(X86 *c, uint32_t addr, void *ctx);
// Recompiled code for a guest function, entered at `start` (the function or a
// point inside it). Returns 0 when the function returned to the caller that
// entered it at its start, 1 otherwise; either way c's state is current and
// execution continues at c->eip.
typedef int (*X86NativeFn)(X86 *c, uint32_t start);
typedef struct { uint32_t addr; X86NativeFn fn; } X86Native;

struct X86 {
    union { X86Core k; struct { X86_CORE_FIELDS }; };
    uint32_t trap_lo, trap_hi;  // eip in [lo, hi) = host function
    X86Trap trap;
    void *trap_ctx;
    uint32_t stop;              // run() returns when eip reaches this
    jmp_buf *fault;
    char err[256];
    volatile int *abort;  // if set and nonzero, x86_run faults out (engine shutdown)
    const X86Native *native;    // sorted by addr
    size_t nnative;
    // Recompiled code recurses on the C stack where the interpreter doesn't:
    // below this address (set by x86_run for the running thread) it hands
    // over to the interpreter instead.
    uintptr_t stack_limit;
    uint32_t *profile;          // if set: profile[a - profile_lo] counts instructions interpreted
    uint32_t profile_lo, profile_hi;  // since control last landed at a (tools/build_mt_native.sh)
};

void x86_init(X86 *c, uint8_t *mem);
// Run from c->eip until eip == c->stop (or a fault longjmps out).
void x86_run(X86 *c);
// Execute exactly one instruction (traps count as one). For tests.
void x86_step(X86 *c);

// Faults: formats c->err and longjmps to *c->fault.
void x86_fault(X86 *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

uint32_t x86_eflags(const X86 *c);
void x86_set_eflags(X86 *c, uint32_t v);

// guest memory (little-endian host assumed)
static inline void x86_check(X86 *c, uint32_t a, uint32_t n) {
    if (__builtin_expect((uint64_t)a + n > c->mem_size || a < 0x1000, 0))
        x86_fault(c, "bad access %#x (%u bytes) at eip %#x (esp %#x -> %#x)", a, n, c->eip, c->r[ESP], (c->r[ESP] >= 0x1000 && c->r[ESP] < c->mem_size - 4) ? *(uint32_t *)(void *)(c->mem + c->r[ESP]) : 0);
}
static inline uint8_t rd8(X86 *c, uint32_t a) { x86_check(c, a, 1); return c->mem[a]; }
static inline uint16_t rd16(X86 *c, uint32_t a) { x86_check(c, a, 2); uint16_t v; __builtin_memcpy(&v, c->mem + a, 2); return v; }
static inline uint32_t rd32(X86 *c, uint32_t a) { x86_check(c, a, 4); uint32_t v; __builtin_memcpy(&v, c->mem + a, 4); return v; }
static inline uint64_t rd64(X86 *c, uint32_t a) { x86_check(c, a, 8); uint64_t v; __builtin_memcpy(&v, c->mem + a, 8); return v; }
static inline void wr8(X86 *c, uint32_t a, uint8_t v) { x86_check(c, a, 1); c->mem[a] = v; }
static inline void wr16(X86 *c, uint32_t a, uint16_t v) { x86_check(c, a, 2); __builtin_memcpy(c->mem + a, &v, 2); }
static inline void wr32(X86 *c, uint32_t a, uint32_t v) { x86_check(c, a, 4); __builtin_memcpy(c->mem + a, &v, 4); }
static inline void wr64(X86 *c, uint32_t a, uint64_t v) { x86_check(c, a, 8); __builtin_memcpy(c->mem + a, &v, 8); }

static inline void x86_push(X86 *c, uint32_t v) { c->r[ESP] -= 4; wr32(c, c->r[ESP], v); }
static inline uint32_t x86_pop(X86 *c) { uint32_t v = rd32(c, c->r[ESP]); c->r[ESP] += 4; return v; }
// cdecl argument i (0-based) of a host function entered through a trap:
// [esp] is the return address.
static inline uint32_t x86_arg(X86 *c, int i) { return rd32(c, c->r[ESP] + 4 + 4 * (uint32_t)i); }

// x87 helpers for host functions returning float/double in ST(0)
void x86_fpush(X86 *c, double v);
double x86_fpop(X86 *c);

// ---- for recompiled code (and x86.c)

// Faults raised by instructions (x86_ops.h), which see only the core: they go
// to the X86 this thread is running.
void x86_kfault(const char *fmt, ...) __attribute__((noreturn, cold, format(printf, 1, 2)));
void x86_atomic_begin(void);
void x86_atomic_end(void);
// Run the code at c->eip (already pushed `ret`) until it returns there with the
// stack as before the call, or up to X86_CALLEE_POP bytes above (`ret n`
// popping arguments, as functions returning a struct do). 0 = returned; 1 =
// control left some other way (state is in c; recompiled callers return 1 in
// turn).
#define X86_CALLEE_POP 256u
int x86_rt_call(X86 *c, uint32_t ret);
