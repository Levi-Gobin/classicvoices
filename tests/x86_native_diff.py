#!/usr/bin/env python3
"""Check recompiled instructions against the interpreter, one by one.

Every per-instruction function in the generated files (build/mtgenNNN/*.c:
`static void i_<addr>(X86Core *k) { x86_exec_<group>(k, B0, B1); }`) runs from
random states with its bytes as compile-time constants, and the interpreter's
x86_exec runs the same bytes passed at run time; registers, flags, SSE/x87
state and memory must match. A mismatch means the instruction semantics in
core/x86/x86_ops.h depend on whether the compiler knows the bytes (undefined
behaviour, typically).

Usage: x86_native_diff.py <generated .c files...> [--trials N]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("files", nargs="+")
ap.add_argument("--trials", type=int, default=200)
a = ap.parse_args()

pat = re.compile(r"^static void [it]_([0-9a-f_]+)\(X86Core \*k\) \{ (x86_exec_\w+)\(k, (0x[0-9a-f]+ull), (0x[0-9a-f]+ull)\); \}$")
insns = {}
for f in a.files:
    for ln in open(f):
        m = pat.match(ln.strip())
        if m:
            insns[(m.group(3), m.group(4))] = (m.group(1), m.group(2))
print(f"{len(insns)} distinct instructions", flush=True)

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = ["#define X86_OPS_INLINE static inline __attribute__((always_inline))",
       '#include "x86/x86_ops.h"', "#include <stdio.h>", ""]
rows = []
for n, ((b0, b1), (name, grp)) in enumerate(sorted(insns.items(), key=lambda kv: kv[1][0])):
    src.append(f"static void t{n}(X86Core *k) {{ {grp}(k, {b0}, {b1}); }}")
    rows.append(f"    {{t{n}, {b0}, {b1}, \"{name}\"}},")
src += ["typedef struct { void (*fn)(X86Core *); uint64_t b0, b1; const char *name; } T;",
        "static const T tests[] = {"] + rows + ["};", r'''
#include <setjmp.h>
static jmp_buf jb;
static int faulted;
void x86_kfault(const char *fmt, ...) { faulted = 1; longjmp(jb, 1); }
void x86_atomic_begin(void) {}
void x86_atomic_end(void) {}
#define MEM (4u << 20)
static uint8_t m1[MEM], m2[MEM], m0[MEM];
static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void randomize(X86Core *k) {
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 8; i++) k->r[i] = 0x100000 + (uint32_t)(rnd() % (MEM - 0x200000)) ;
    if (rnd() & 1) for (int i = 0; i < 8; i++) if (rnd() % 3 == 0) k->r[i] = (uint32_t)rnd() % 64;
    if (rnd() % 4 == 0) for (int i = 0; i < 8; i++) if (rnd() & 1) k->r[i] = (uint32_t)rnd();
    k->r[ESP] = MEM - 0x10000 - (uint32_t)(rnd() % 0x1000) * 4;
    k->cf = rnd() & 1; k->pf = rnd() & 1; k->af = rnd() & 1; k->zf = rnd() & 1; k->sf = rnd() & 1; k->of = rnd() & 1;
    for (int i = 0; i < 8; i++) { k->xmm[i].q[0] = rnd(); k->xmm[i].q[1] = rnd(); }
    for (int i = 0; i < 8; i++) { if (rnd() & 1) k->xmm[i].f[0] = (float)((int)(rnd() % 2001) - 1000) / 7.0f; if (rnd() & 1) k->xmm[i].g[0] = (double)((int)(rnd() % 2001) - 1000) / 3.0; }
    for (int i = 0; i < 8; i++) { k->st[i] = (double)((int)(rnd() % 20001) - 10000) / 13.0; k->tag[i] = 0; }
    k->top = (int)(rnd() % 8);
    k->mxcsr = 0x1F80; k->fcw = 0x037F;
    k->eip = 0x1000;
    k->mem_size = MEM;
    k->div0_as_one = 1;
}
int main(int argc, char **argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 200, bad = 0, n = (int)(sizeof tests / sizeof tests[0]);
    for (size_t i = 0; i < MEM; i++) m0[i] = (uint8_t)rnd();
    for (int t = 0; t < n; t++) {
        int fails = 0;
        for (int tr = 0; tr < trials && fails < 1; tr++) {
            X86Core a, b;
            randomize(&a);
            b = a;
            memcpy(m1, m0, MEM); memcpy(m2, m0, MEM);
            a.mem = m1; b.mem = m2;
            volatile uint64_t v0 = tests[t].b0, v1 = tests[t].b1;
            int fa = 0, fb = 0;
            faulted = 0;
            if (!setjmp(jb)) tests[t].fn(&a); else fa = 1;
            faulted = 0;
            if (!setjmp(jb)) x86_exec(&b, v0, v1); else fb = 1;
            a.mem = b.mem = NULL;
            int same = fa == fb && (fa || (!memcmp(&a, &b, sizeof a) && !memcmp(m1, m2, MEM)));
            if (!same) {
                fails++; bad++;
                printf("MISMATCH %s bytes %016llx %016llx (fault %d/%d)", tests[t].name, (unsigned long long)tests[t].b0,
                       (unsigned long long)tests[t].b1, fa, fb);
                if (!fa && !fb) {
                    const uint32_t *x = (const uint32_t *)&a, *y = (const uint32_t *)&b;
                    for (size_t w = 0; w < sizeof a / 4; w++) if (x[w] != y[w]) printf(" core[%zu] %x/%x", w, x[w], y[w]);
                    for (size_t q = 0; q < MEM; q++) if (m1[q] != m2[q]) { printf(" mem %zx %02x/%02x", q, m1[q], m2[q]); break; }
                }
                printf("\n");
            }
        }
    }
    printf("%d instructions, %d trials each, %d mismatching\n", n, trials, bad);
    return bad != 0;
}
''']
d = tempfile.mkdtemp()
c = os.path.join(d, "t.c")
open(c, "w").write("\n".join(src))
exe = os.path.join(d, "t")
subprocess.check_call(["clang", "-O2", "-w", "-I" + os.path.join(root, "core"), c, "-o", exe])
sys.exit(subprocess.call([exe, str(a.trials)]))
