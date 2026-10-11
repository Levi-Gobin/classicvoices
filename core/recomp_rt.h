// Runtime helpers referenced by recompiled code (build/gen/*.c).
#pragma once
#include "thumb.h"

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
