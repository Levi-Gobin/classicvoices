// Single-step harness for tests/x86_diff.py.
#include "../core/x86/x86.h"
#include <string.h>
static jmp_buf jb;
// regs: eax..edi, eip, eflags ; xmm: 16 u64 ; st: 8 doubles (physical) ; top
int step(uint32_t *regs, uint64_t *xmm, double *st, int *top, uint8_t *mem) {
    static X86 c;
    x86_init(&c, mem);
    memcpy(c.r, regs, 32);
    c.eip = regs[8];
    x86_set_eflags(&c, regs[9]);
    memcpy(c.xmm, xmm, 128);
    memcpy(c.st, st, 64);
    c.top = *top;
    for (int i = 0; i < 8; i++) c.tag[i] = 0;
    c.trap_lo = c.trap_hi = 0;
    c.fault = &jb;
    if (setjmp(jb)) return 1;
    x86_step(&c);
    memcpy(regs, c.r, 32);
    regs[8] = c.eip;
    regs[9] = x86_eflags(&c);
    memcpy(xmm, c.xmm, 128);
    memcpy(st, c.st, 64);
    *top = c.top;
    return 0;
}
