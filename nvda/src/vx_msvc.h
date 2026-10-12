// Force-included (/FI) when MSVC compiles the recompiled engine code
// (build/native/gen*/*.c). Those files only pull in thumb.h / thumb_ops.h /
// recomp_rt.h, so this just maps the GCC-isms they use onto MSVC. Keep MSVC
// at its default /arch (no FMA exists there), so VMLA is never contracted and
// the float results stay bit-identical to the interpreter.
#pragma once
#ifdef _MSC_VER
#include <intrin.h>
#include <math.h>
#include <stdint.h>

#define __attribute__(x)

#define __builtin_expect(x, y) (x)
#define __builtin_popcount(x) ((int)__popcnt((unsigned)(x)))
#define __builtin_bswap32(x) _byteswap_ulong(x)
#define __builtin_rint(x) rint(x)
#define __builtin_sqrt(x) sqrt(x)
#define __builtin_sqrtf(x) sqrtf(x)
#define __builtin_trap() __debugbreak()

static __forceinline int __builtin_clz(unsigned x) {
    unsigned long i;
    _BitScanReverse(&i, x);  // callers guard x != 0
    return 31 - (int)i;
}

static __forceinline unsigned __builtin_bitreverse32(unsigned v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
#endif
