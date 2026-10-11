// ARMv7 Thumb-2 (integer) execution: interpreter + dispatcher into
// statically recompiled native code (see tools/recomp.py).
#pragma once
#include "thumb_ops.h"
#include <setjmp.h>
#include <stddef.h>

typedef struct Cpu Cpu;
// Called when PC lands in the trap region. Leaving r15 unchanged resumes at LR.
typedef void (*TrapHandler)(Cpu *cpu, uint32_t addr, void *ctx);
// Recompiled code: run from `start` until the function returns to the LR it was
// entered with (returns 0) or control leaves it some other way (returns 1).
// Either way c->k.r[15] holds where execution continues.
typedef int (*NativeFn)(Cpu *c, uint32_t start);
typedef struct { uint32_t addr; NativeFn fn; } NativeEntry;

struct Cpu {
    Core k;
    uint32_t trap_lo, trap_hi;             // [lo, hi): addresses handled by trap()
    uint32_t stub_lo, stub_hi, lazy_base;  // ARM `ldr pc,[lazy]` import stubs
    TrapHandler trap;
    void *trap_ctx;
    uint32_t return_addr;                  // sentinel LR for cpu_call
    uint64_t icount;                       // interpreted instructions
    const NativeEntry *native;             // sorted by addr
    size_t nnative;
    int64_t budget;                        // debug: native instructions left (if budget_on)
    int budget_on;
    uint32_t budget_exit_pc;
    uint8_t *profile;                      // optional: 1 byte per halfword of guest code below 4 MB
    jmp_buf *fault_jmp;
    char fault_msg[256];
};

void cpu_init(Cpu *cpu, uint8_t *mem);
// Call guest Thumb function `fn`; returns r0 (r1 in *hi if non-NULL).
uint32_t cpu_call(Cpu *cpu, uint32_t fn, const uint32_t *args, int nargs, uint32_t *hi);
void cpu_fault(Cpu *cpu, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

// Runtime entry for recompiled code: c->k already holds the callee's state with
// npc/thumb set to the call target. Returns 0 once execution reaches `ret`.
int rt_call(Cpu *c, uint32_t ret);
