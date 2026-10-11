// Interpreter run loop and dispatcher. Instruction semantics live in thumb_ops.h.
#include "thumb.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local Cpu *tls_cpu;

void cpu_init(Cpu *cpu, uint8_t *mem) {
    memset(cpu, 0, sizeof *cpu);
    cpu->k.mem = mem;
    cpu->k.thumb = 1;
}

void cpu_fault(Cpu *cpu, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cpu->fault_msg, sizeof cpu->fault_msg, fmt, ap);
    va_end(ap);
    if (cpu->fault_jmp) longjmp(*cpu->fault_jmp, 1);
    fprintf(stderr, "cpu fault: %s\n", cpu->fault_msg);
    __builtin_trap();
}

void vx_fault_mem(const char *kind, uint32_t addr, uint32_t pc) {
    cpu_fault(tls_cpu, "%s %#x near pc=%#x", kind, addr, pc - 4);
}

void vx_undef(uint32_t hw1, uint32_t hw2, uint32_t pc) {
    cpu_fault(tls_cpu, "undefined/unsupported instruction %04x %04x at %#x", hw1, hw2, pc);
}

static NativeFn lookup_native(const Cpu *c, uint32_t pc) {
    static int have_range = -1;
    static uint32_t range_lo, range_hi;
    if (have_range < 0) {  // debugging: VX_NATIVE_RANGE=lo-hi limits native code to [lo, hi)
        const char *e = getenv("VX_NATIVE_RANGE");
        have_range = e && sscanf(e, "%x-%x", &range_lo, &range_hi) == 2;
    }
    if (have_range && (pc < range_lo || pc >= range_hi)) return NULL;
    if (c->budget_on && c->budget < 0) return NULL;  // debug budget exhausted
    size_t lo = 0, hi = c->nnative;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t a = c->native[mid].addr;
        if (a == pc) return c->native[mid].fn;
        if (a < pc) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

// Returns 0 when PC reaches `stop`, 1 if the outermost return sentinel is hit
// first (a longjmp unwound past the frame that asked to stop).
static int run(Cpu *c, uint32_t stop) {
    Core *k = &c->k;
    int check_native = 1;
    for (;;) {
        uint32_t pc = k->r[15];
        if (pc == stop) return 0;
        if (pc == c->return_addr) return 1;
        if (pc >= c->trap_lo && pc < c->trap_hi) {
            c->trap(c, pc, c->trap_ctx);
            if (k->r[15] == pc) { bx_to(k, k->r[14]); k->r[15] = k->npc; }
            check_native = 1;
            continue;
        }
        if (!k->thumb) {
            if (pc >= c->stub_lo && pc < c->stub_hi) {
                bx_to(k, g32(k, c->lazy_base + (pc - c->stub_lo)));
                k->r[15] = k->npc;
                check_native = 1;
                continue;
            }
            cpu_fault(c, "ARM-mode execution at %#x (lr=%#x)", pc, k->r[14]);
        }
        if (check_native && c->profile && pc < 0x400000) c->profile[pc >> 1] = 1;
        if (check_native && c->nnative) {
            NativeFn f = lookup_native(c, pc);
            if (f) {
                int before = c->budget_on && c->budget >= 0;
                f(c, pc);
                if (before && c->budget < 0) {  // debug: dump state where native code handed back
                    fprintf(stderr, "handback pc=%x thumb=%u it=%x nzcv=%u%u%u%u", k->r[15], k->thumb, k->it, k->n, k->z, k->c, k->v);
                    for (int i = 0; i < 15; i++) fprintf(stderr, " r%d=%x", i, k->r[i]);
                    fprintf(stderr, "\n");
                }
                continue;
            }
        }
        check_native = 0;
        uint32_t hw1 = g16(k, pc), len;
        c->icount++;
        k->r[15] = pc + 4;
        if ((hw1 >> 11) >= 0x1D) {
            uint32_t hw2 = g16(k, pc + 2);
            len = 4;
            k->npc = pc + 4;
            if (k->it & 0xf) {
                int pass = cond_pass(k, k->it >> 4);
                k->it = (k->it & 7) ? ((k->it & 0xe0) | ((k->it << 1) & 0x1f)) : 0;
                if (pass) exec32(k, hw1, hw2, pc);
            } else {
                exec32(k, hw1, hw2, pc);
            }
        } else {
            len = 2;
            k->npc = pc + 2;
            if (k->it & 0xf) {
                int pass = cond_pass(k, k->it >> 4);
                k->it = (k->it & 7) ? ((k->it & 0xe0) | ((k->it << 1) & 0x1f)) : 0;
                if (pass) exec16(k, hw1, pc, 1);
            } else {
                exec16(k, hw1, pc, 0);
            }
        }
        check_native = k->npc != pc + len;
        k->r[15] = k->npc;
    }
}

int rt_call(Cpu *c, uint32_t ret) {
    c->k.r[15] = c->k.npc;
    c->k.it = 0;
    return run(c, ret & ~1u);
}

uint32_t cpu_call(Cpu *c, uint32_t fn, const uint32_t *args, int nargs, uint32_t *hi) {
    Cpu *prev_tls = tls_cpu;
    tls_cpu = c;
    Core saved = c->k;
    uint32_t sp = c->k.r[13] - 0x40;
    int nstack = nargs > 4 ? nargs - 4 : 0;
    sp -= 4 * nstack;
    sp &= ~7u;
    for (int i = 0; i < nstack; i++) s32(&c->k, sp + 4 * i, args[4 + i]);
    for (int i = 0; i < 4 && i < nargs; i++) c->k.r[i] = args[i];
    c->k.r[13] = sp;
    c->k.r[14] = c->return_addr | 1;
    c->k.r[15] = fn & ~1u;
    c->k.thumb = 1;
    c->k.it = 0;
    run(c, c->return_addr);
    uint32_t r0 = c->k.r[0], r1 = c->k.r[1];
    c->k = saved;
    tls_cpu = prev_tls;
    if (hi) *hi = r1;
    return r0;
}

int rt_import(Cpu *c, uint32_t stub, uint32_t ret) {
    uint32_t target = g32(&c->k, c->lazy_base + (stub - c->stub_lo));
    uint32_t t = target & ~1u;
    if ((target & 1) && t >= c->trap_lo && t < c->trap_hi) {
        c->k.r[15] = t;
        c->trap(c, t, c->trap_ctx);
        uint32_t lr = c->k.r[14];
        bx_to(&c->k, lr);
        c->k.r[15] = c->k.npc;
        return (lr & ~1u) == (ret & ~1u) && (lr & 1) ? 0 : 1;
    }
    c->k.npc = t;
    c->k.thumb = target & 1;
    return rt_call(c, ret);
}
