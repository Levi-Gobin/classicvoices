// Runtime helpers referenced by recompiled code (build/gen/*.c).
#pragma once
#include "thumb.h"

// A function's registers live in a local Core while it runs. Functions
// without VFP/NEON instructions never touch the 256 bytes of D registers, so
// they copy only the integer part in and out: these tiny fixed-point helpers
// are called millions of times, and copying the whole Core made up most of
// the engines' running time.
#include <stddef.h>
// always inlined: a call taking a Core's address would keep it in memory
#define RT_INLINE static inline __attribute__((always_inline))
#define RT_INT_PART offsetof(Core, d)
#define RT_IN(K, c) memcpy(&(K), &(c)->k, RT_INT_PART)
#define RT_OUT(c, K) memcpy(&(c)->k, &(K), RT_INT_PART)

// Call an import through its ARM stub. Returns 0 when it returned to `ret`.
int rt_import(Cpu *c, uint32_t stub, uint32_t ret);
// Call through a register (K.npc set by the BLX): 0 when it returned to `ret`.
int rt_icall(Cpu *c, uint32_t ret);
// A recompiled leaf (tools/recomp.py emit_leaf), run on the caller's registers.
typedef int (*LeafFn)(Core *);

// Same results as the __divsi3/__modsi3 HLE (division by 0 or -1 is defined).
RT_INLINE uint32_t rt_sdiv(uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    return y == 0 ? 0 : y == -1 ? (uint32_t)(-(int64_t)x) : (uint32_t)(x / y);
}
RT_INLINE uint32_t rt_smod(uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    return (y == 0 || y == -1) ? 0 : (uint32_t)(x % y);
}

// Host versions of the engines' own C-library helpers (tools/recomp.py
// HOST_FUNCS), for the builds whose code they were checked against. The
// engines call these byte loops millions of times a sentence (the Siri
// voices' active-prompt lookup above all); each does exactly what the guest
// code does to r0 and to memory. r1-r3 and the flags are the caller's to lose.
#include <string.h>
RT_INLINE uint32_t rt_cstr_len(Core *k, uint32_t a) {  // bytes before the NUL; faults like g8 if there is none
    const uint8_t *z = a < GUEST_MEM_SIZE ? memchr(k->mem + a, 0, GUEST_MEM_SIZE - a) : NULL;
    if (!z) vx_fault_mem("read8", GUEST_MEM_SIZE, k->r[15]);
    return (uint32_t)(z - (k->mem + a));
}
RT_INLINE void rt_strlen(Core *k) { k->r[0] = rt_cstr_len(k, k->r[0]); }
// cstdlib_strchr: the first byte equal to (signed char)r1, or the NUL when
// r1's low byte is 0; else 0
RT_INLINE void rt_strchr(Core *k) {  // mostly asked of short strings (punctuation sets): one pass
    uint32_t p = k->r[0];
    uint8_t ch = (uint8_t)k->r[1];
    for (;; p++) {
        uint8_t b = (uint8_t)g8(k, p);
        if (b == ch) { k->r[0] = p; return; }
        if (!b) { k->r[0] = 0; return; }
    }
}
// Would cstdlib_strchr(s, ch) find it (nonzero)? (rt_strchr's answer, for
// host loops)
RT_INLINE int rt_in_str(Core *k, uint32_t s, uint8_t ch) {
    for (;; s++) {
        uint8_t b = (uint8_t)g8(k, s);
        if (b == ch) return 1;
        if (!b) return 0;
    }
}
// cstdlib_strcmp: -1, 0 or 1 by the first differing unsigned byte
RT_INLINE void rt_strcmp(Core *k) {
    uint32_t a = k->r[0], b = k->r[1];
    for (;;) {
        uint32_t x = g8(k, a++), y = g8(k, b++);
        if (x != y) { k->r[0] = x < y ? 0xFFFFFFFFu : 1; return; }
        if (!x) { k->r[0] = 0; return; }
    }
}
RT_INLINE void rt_copy_check(Core *k, uint32_t dst, uint32_t src, uint32_t n) {
    if (src > GUEST_MEM_SIZE || n > GUEST_MEM_SIZE - src) vx_fault_mem("read8", src, k->r[15]);
    if (dst < 0x1000 || dst > GUEST_MEM_SIZE || n > GUEST_MEM_SIZE - dst) vx_fault_mem("write8", dst, k->r[15]);
}
// cstdlib_memcpy: a forward byte loop, so an overlap with dst after src
// repeats the bytes, as the guest's does; r0 stays dst
RT_INLINE void rt_memcpy(Core *k) {
    uint32_t d = k->r[0], s = k->r[1], n = k->r[2];
    if (!n) return;
    rt_copy_check(k, d, s, n);
    if (d > s && d - s < n) { for (uint32_t i = 0; i < n; i++) k->mem[d + i] = k->mem[s + i]; }
    else memmove(k->mem + d, k->mem + s, n);
}
// cstdlib_memmove: a true memmove; r0 stays dst
RT_INLINE void rt_memmove(Core *k) {
    uint32_t d = k->r[0], s = k->r[1], n = k->r[2];
    if (!n) return;
    rt_copy_check(k, d, s, n);
    memmove(k->mem + d, k->mem + s, n);
}
// cstdlib_strncmp: the difference of the first unequal unsigned bytes (or
// the bytes after the first n - 1 equal ones), 0 for n = 0
RT_INLINE void rt_strncmp(Core *k) {
    uint32_t a = k->r[0], b = k->r[1], n = k->r[2];
    if (!n) { k->r[0] = 0; return; }
    for (uint32_t i = 1; i < n; i++, a++, b++) {
        uint32_t x = g8(k, a);
        if (!x || x != g8(k, b)) break;
    }
    k->r[0] = g8(k, a) - g8(k, b);
}
// cstdlib_strstr: the first place r1 occurs in r0 (r0 itself for ""), else 0
RT_INLINE void rt_strstr(Core *k) {
    uint32_t h = k->r[0], nd = k->r[1];
    uint32_t first = g8(k, nd);
    if (!first) return;
    for (; g8(k, h); h++) {
        if (k->mem[h] != first) continue;
        uint32_t i = 1, y;
        while ((y = g8(k, nd + i)) && g8(k, h + i) == y) i++;
        if (!y) { k->r[0] = h; return; }
    }
    k->r[0] = 0;
}
// Shifts by a register, as Thumb-2 does them: by its bottom byte, 32 or
// more giving 0 (LSL) or the sign (ASR).
RT_INLINE uint32_t rt_lsl(uint32_t x, uint32_t n) { n &= 0xFF; return n >= 32 ? 0 : x << n; }
RT_INLINE uint32_t rt_asr(uint32_t x, uint32_t n) {
    n &= 0xFF;
    if (n >= 32) return (int32_t)x < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)((int32_t)x >> n);
}
// fxd_S32ShMultRndS32S32(a, b, shift): (a * b) >> shift with rounding, from
// 16-bit halves, step for step as iOS 7's engines compute it (the engines'
// most-called function: every unit-selection cost and filter tap)
RT_INLINE void rt_fxd_mult_rnd(Core *k) {
    uint32_t a = k->r[0], b = k->r[1], s = k->r[2];
    uint32_t bh = (uint32_t)((int32_t)b >> 16), al = a & 0xFFFF, bl = b & 0xFFFF;
    uint32_t ah = (uint32_t)((int32_t)a >> 16);
    uint32_t mid = bl * ah + bh * al;
    uint32_t lo = bl * al + 0x8000;
    uint32_t r1 = mid + (lo >> 16);
    uint32_t sh = (s + 0xFFF0) & 0xFFFF;
    if (sh) r1 = rt_asr(rt_lsl(1, sh - 1) + r1, sh);
    k->r[0] = rt_lsl(bh * ah, 16 - sh) + r1;
}
