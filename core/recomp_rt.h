// Runtime helpers referenced by recompiled code (build/gen/*.c).
#pragma once
#include "thumb.h"

// A function's registers live in a local Core while it runs. Functions
// without VFP/NEON instructions never touch the 256 bytes of D registers, so
// they copy only the integer part in and out: these tiny fixed-point helpers
// are called millions of times, and copying the whole Core made up most of
// the engines' running time.
#include <stddef.h>
#define RT_INT_PART offsetof(Core, d)
#define RT_IN(K, c) memcpy(&(K), &(c)->k, RT_INT_PART)
#define RT_OUT(c, K) memcpy(&(c)->k, &(K), RT_INT_PART)

// Call an import through its ARM stub. Returns 0 when it returned to `ret`.
int rt_import(Cpu *c, uint32_t stub, uint32_t ret);

// Same results as the __divsi3/__modsi3 HLE (division by 0 or -1 is defined).
static inline uint32_t rt_sdiv(uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    return y == 0 ? 0 : y == -1 ? (uint32_t)(-(int64_t)x) : (uint32_t)(x / y);
}
static inline uint32_t rt_smod(uint32_t a, uint32_t b) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    return (y == 0 || y == -1) ? 0 : (uint32_t)(x % y);
}
