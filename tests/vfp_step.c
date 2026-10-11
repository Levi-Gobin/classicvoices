// Single-step harness for differential testing of instruction semantics.
#include "../core/thumb_ops.h"
#include <setjmp.h>
static jmp_buf jb;
void vx_fault_mem(const char *kind, uint32_t addr, uint32_t pc) { (void)kind; (void)addr; (void)pc; longjmp(jb, 1); }
void vx_undef(uint32_t hw1, uint32_t hw2, uint32_t pc) { (void)hw1; (void)hw2; (void)pc; longjmp(jb, 2); }
// regs: r[16], n z c v, fpscr ; d[32]
int step(uint32_t *regs, uint64_t *d, uint8_t *mem, uint32_t hw1, uint32_t hw2, uint32_t pc) {
    Core c; memset(&c, 0, sizeof c);
    memcpy(c.r, regs, 64);
    c.n = regs[16]; c.z = regs[17]; c.c = regs[18]; c.v = regs[19]; c.fpscr = regs[20];
    memcpy(c.d, d, 256);
    c.mem = mem; c.thumb = 1;
    c.r[15] = pc + 4; c.npc = pc + 4;
    int rc = setjmp(jb);
    if (rc) return rc;
    exec32(&c, hw1, hw2, pc);
    memcpy(regs, c.r, 64);
    regs[16] = c.n; regs[17] = c.z; regs[18] = c.c; regs[19] = c.v; regs[20] = c.fpscr;
    memcpy(d, c.d, 256);
    return 0;
}
