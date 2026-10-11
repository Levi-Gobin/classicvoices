// i386 interpreter: integer ISA, SSE/SSE2/SSE3 (scalar and packed), and the
// x87 subset compilers emit for float returns and conversions. See x86.h.
//
// Flags are computed eagerly into unpacked fields; flags an instruction leaves
// undefined are set the way Unicorn (QEMU) sets them where that is cheap, and
// the differential test (tests/x86_diff.py) masks the rest.
#include "x86.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang fp contract(off)

void x86_fault(X86 *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->err, sizeof c->err, fmt, ap);
    va_end(ap);
    if (c->fault) longjmp(*c->fault, 1);
    fprintf(stderr, "x86 fault: %s\n", c->err);
    __builtin_trap();
}

void x86_init(X86 *c, uint8_t *mem) {
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->mxcsr = 0x1F80;
    c->fcw = 0x037F;
    for (int i = 0; i < 8; i++) c->tag[i] = 1;
}

uint32_t x86_eflags(const X86 *c) {
    return 0x202 | c->cf | c->pf << 2 | c->af << 4 | c->zf << 6 | c->sf << 7 | c->df << 10 | c->of << 11;
}
void x86_set_eflags(X86 *c, uint32_t v) {
    c->cf = v & 1; c->pf = v >> 2 & 1; c->af = v >> 4 & 1; c->zf = v >> 6 & 1;
    c->sf = v >> 7 & 1; c->df = v >> 10 & 1; c->of = v >> 11 & 1;
}

// ---------------------------------------------------------------- decode state

typedef struct {
    uint32_t start;   // eip of the instruction
    int opsz;         // 16 or 32 (0x66 prefix)
    int rep;          // 0, 0xF3 or 0xF2
    uint32_t seg;     // segment base to add (fs/gs override)
    int has66;
    // modrm
    int mod, reg, rm;
    uint32_t ea;      // effective address when mod != 3
} D;

static inline uint8_t fetch8(X86 *c) { return rd8(c, c->eip++); }
static inline uint16_t fetch16(X86 *c) { uint16_t v = rd16(c, c->eip); c->eip += 2; return v; }
static inline uint32_t fetch32(X86 *c) { uint32_t v = rd32(c, c->eip); c->eip += 4; return v; }

static void modrm(X86 *c, D *d) {
    uint8_t m = fetch8(c);
    d->mod = m >> 6; d->reg = m >> 3 & 7; d->rm = m & 7;
    if (d->mod == 3) return;
    uint32_t a = 0;
    if (d->rm == 4) {
        uint8_t sib = fetch8(c);
        int ss = sib >> 6, idx = sib >> 3 & 7, base = sib & 7;
        if (base == 5 && d->mod == 0) a = fetch32(c);
        else a = c->r[base];
        if (idx != 4) a += c->r[idx] << ss;
    } else if (d->rm == 5 && d->mod == 0) {
        a = fetch32(c);
    } else {
        a = c->r[d->rm];
    }
    if (d->mod == 1) a += (uint32_t)(int32_t)(int8_t)fetch8(c);
    else if (d->mod == 2) a += fetch32(c);
    d->ea = a + d->seg;
}

// 8-bit register file: 0-3 = AL CL DL BL, 4-7 = AH CH DH BH
static inline uint8_t getr8(X86 *c, int i) { return i < 4 ? (uint8_t)c->r[i] : (uint8_t)(c->r[i - 4] >> 8); }
static inline void setr8(X86 *c, int i, uint8_t v) {
    if (i < 4) c->r[i] = (c->r[i] & ~0xFFu) | v;
    else c->r[i - 4] = (c->r[i - 4] & ~0xFF00u) | (uint32_t)v << 8;
}
static inline uint16_t getr16(X86 *c, int i) { return (uint16_t)c->r[i]; }
static inline void setr16(X86 *c, int i, uint16_t v) { c->r[i] = (c->r[i] & ~0xFFFFu) | v; }

// r/m operand of size 8/16/32
static inline uint32_t rm_get(X86 *c, D *d, int sz) {
    if (d->mod == 3) return sz == 8 ? getr8(c, d->rm) : sz == 16 ? getr16(c, d->rm) : c->r[d->rm];
    return sz == 8 ? rd8(c, d->ea) : sz == 16 ? rd16(c, d->ea) : rd32(c, d->ea);
}
static inline void rm_set(X86 *c, D *d, int sz, uint32_t v) {
    if (d->mod == 3) {
        if (sz == 8) setr8(c, d->rm, (uint8_t)v);
        else if (sz == 16) setr16(c, d->rm, (uint16_t)v);
        else c->r[d->rm] = v;
        return;
    }
    if (sz == 8) wr8(c, d->ea, (uint8_t)v);
    else if (sz == 16) wr16(c, d->ea, (uint16_t)v);
    else wr32(c, d->ea, v);
}
static inline uint32_t reg_get(X86 *c, int r, int sz) { return sz == 8 ? getr8(c, r) : sz == 16 ? getr16(c, r) : c->r[r]; }
static inline void reg_set(X86 *c, int r, int sz, uint32_t v) {
    if (sz == 8) setr8(c, r, (uint8_t)v);
    else if (sz == 16) setr16(c, r, (uint16_t)v);
    else c->r[r] = v;
}

static inline uint32_t mask_of(int sz) { return sz == 32 ? 0xFFFFFFFFu : (1u << sz) - 1; }
static inline uint32_t sext(uint32_t v, int sz) {
    return sz == 8 ? (uint32_t)(int32_t)(int8_t)v : sz == 16 ? (uint32_t)(int32_t)(int16_t)v : v;
}

// ---------------------------------------------------------------- flags / ALU

static inline uint32_t parity(uint32_t v) { return !__builtin_parity(v & 0xFF); }

static inline void szp(X86 *c, uint32_t r, int sz) {
    r &= mask_of(sz);
    c->zf = r == 0;
    c->sf = r >> (sz - 1) & 1;
    c->pf = parity(r);
}

// op: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp
static uint32_t alu(X86 *c, int op, uint32_t a, uint32_t b, int sz) {
    uint32_t m = mask_of(sz), msb = sz - 1, r;
    a &= m; b &= m;
    switch (op) {
    case 0: case 2: {
        uint32_t ci = op == 2 ? c->cf : 0;
        uint64_t w = (uint64_t)a + b + ci;
        r = (uint32_t)w & m;
        c->cf = (w >> sz) & 1;
        c->of = ((a ^ r) & (b ^ r)) >> msb & 1;
        c->af = ((a ^ b ^ r) >> 4) & 1;
        break;
    }
    case 3: case 5: case 7: {
        uint32_t bi = op == 3 ? c->cf : 0;
        r = (a - b - bi) & m;
        c->cf = (uint64_t)a < (uint64_t)b + bi;
        c->of = ((a ^ b) & (a ^ r)) >> msb & 1;
        c->af = ((a ^ b ^ r) >> 4) & 1;
        break;
    }
    case 1: r = a | b; c->cf = c->of = c->af = 0; break;
    case 4: r = a & b; c->cf = c->of = c->af = 0; break;
    default: r = a ^ b; c->cf = c->of = c->af = 0; break;
    }
    szp(c, r, sz);
    return r;
}

static uint32_t incdec(X86 *c, uint32_t a, int dec, int sz) {
    uint32_t m = mask_of(sz), msb = sz - 1, r = (dec ? a - 1 : a + 1) & m;
    a &= m;
    if (dec) { c->of = ((a ^ 1) & (a ^ r)) >> msb & 1; c->af = ((a ^ 1 ^ r) >> 4) & 1; }
    else { c->of = ((a ^ r) & (1 ^ r)) >> msb & 1; c->af = ((a ^ 1 ^ r) >> 4) & 1; }
    szp(c, r, sz);
    return r;
}

// group 2: 0 rol 1 ror 2 rcl 3 rcr 4 shl 5 shr 6 sal 7 sar
static uint32_t shift(X86 *c, int op, uint32_t a, uint32_t cnt, int sz) {
    uint32_t m = mask_of(sz), msb = sz - 1, r;
    a &= m;
    cnt &= 31;
    if (!cnt) return a;
    switch (op) {
    case 0: {  // rol
        uint32_t n = cnt % sz;
        r = n ? ((a << n) | (a >> (sz - n))) & m : a;
        c->cf = r & 1;
        c->of = (r >> msb ^ c->cf) & 1;
        return r;
    }
    case 1: {  // ror
        uint32_t n = cnt % sz;
        r = n ? ((a >> n) | (a << (sz - n))) & m : a;
        c->cf = r >> msb & 1;
        c->of = (r >> msb ^ r >> (msb - 1)) & 1;
        return r;
    }
    case 2: {  // rcl
        uint32_t n = cnt % (sz + 1);
        uint64_t w = (uint64_t)a | (uint64_t)c->cf << sz;
        for (uint32_t i = 0; i < n; i++) w = ((w << 1) | (w >> sz)) & ((2ull << sz) - 1);
        r = (uint32_t)w & m;
        if (n) { c->cf = (w >> sz) & 1; c->of = (r >> msb ^ c->cf) & 1; }
        return r;
    }
    case 3: {  // rcr
        uint32_t n = cnt % (sz + 1);
        uint64_t w = (uint64_t)a | (uint64_t)c->cf << sz;
        for (uint32_t i = 0; i < n; i++) w = ((w >> 1) | ((w & 1) << sz)) & ((2ull << sz) - 1);
        r = (uint32_t)w & m;
        if (n) { c->cf = (w >> sz) & 1; c->of = (r >> msb ^ r >> (msb - 1)) & 1; }
        return r;
    }
    case 4: case 6:  // shl
        r = cnt < 32 ? (a << cnt) & m : 0;
        c->cf = cnt <= (uint32_t)sz ? (a >> (sz - cnt)) & 1 : 0;
        c->of = (r >> msb ^ c->cf) & 1;
        c->af = 0;
        break;
    case 5:  // shr
        r = cnt < 32 ? a >> cnt : 0;
        c->cf = (a >> (cnt - 1)) & 1;
        c->of = a >> msb & 1;
        c->af = 0;
        break;
    default: {  // sar
        int32_t s = (int32_t)sext(a, sz);
        r = (uint32_t)(s >> (cnt > 31 ? 31 : cnt)) & m;
        c->cf = (uint32_t)(s >> (cnt - 1 > 31 ? 31 : cnt - 1)) & 1;
        c->of = 0;
        c->af = 0;
        break;
    }
    }
    szp(c, r, sz);
    return r;
}

static int cond(X86 *c, int cc) {
    int v;
    switch (cc >> 1) {
    case 0: v = c->of; break;
    case 1: v = c->cf; break;
    case 2: v = c->zf; break;
    case 3: v = c->cf | c->zf; break;
    case 4: v = c->sf; break;
    case 5: v = c->pf; break;
    case 6: v = c->sf != c->of; break;
    default: v = c->zf | (c->sf != c->of); break;
    }
    return (cc & 1) ? !v : v;
}

// ---------------------------------------------------------------- x87

#define ST(i) c->st[(c->top + (i)) & 7]
void x86_fpush(X86 *c, double v) { c->top = (c->top - 1) & 7; c->st[c->top] = v; c->tag[c->top] = 0; }
double x86_fpop(X86 *c) { double v = c->st[c->top]; c->tag[c->top] = 1; c->top = (c->top + 1) & 7; return v; }

static double f80_to_double(const uint8_t *p) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int sign = se >> 15, e = se & 0x7FFF;
    double v;
    if (e == 0 && mant == 0) v = 0;
    else if (e == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, e - 16383 - 63);
    return sign ? -v : v;
}
static void double_to_f80(double v, uint8_t *p) {
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double f = frexp(fabs(v), &e);  // f in [0.5, 1)
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
}

static double fround(X86 *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static void fcom_flags(X86 *c, double a, double b, int to_eflags) {
    int un = isnan(a) || isnan(b);
    int c0 = un || a < b, c2 = un, c3 = un || a == b;
    if (to_eflags) { c->cf = c0; c->pf = c2; c->zf = c3; c->of = c->sf = c->af = 0; }
    else c->fsw_cc = (uint16_t)(c0 << 8 | c2 << 10 | c3 << 14);
}

static void x87(X86 *c, D *d, uint8_t op) {
    modrm(c, d);
    int reg = d->reg;
    if (d->mod != 3) {
        uint32_t a = d->ea;
        switch (op) {
        case 0xD8: case 0xDC: case 0xDA: case 0xDE: {  // arith with m32fp / m64fp / m32int / m16int
            double v = op == 0xD8 ? (double)({ uint32_t u = rd32(c, a); float f; memcpy(&f, &u, 4); f; })
                     : op == 0xDC ? ({ uint64_t u = rd64(c, a); double g; memcpy(&g, &u, 8); g; })
                     : op == 0xDA ? (double)(int32_t)rd32(c, a) : (double)(int16_t)rd16(c, a);
            switch (reg) {
            case 0: ST(0) = ST(0) + v; break;
            case 1: ST(0) = ST(0) * v; break;
            case 2: fcom_flags(c, ST(0), v, 0); break;
            case 3: fcom_flags(c, ST(0), v, 0); x86_fpop(c); break;
            case 4: ST(0) = ST(0) - v; break;
            case 5: ST(0) = v - ST(0); break;
            case 6: ST(0) = ST(0) / v; break;
            default: ST(0) = v / ST(0); break;
            }
            return;
        }
        case 0xD9:
            switch (reg) {
            case 0: { uint32_t u = rd32(c, a); float f; memcpy(&f, &u, 4); x86_fpush(c, f); return; }
            case 2: case 3: { float f = (float)ST(0); uint32_t u; memcpy(&u, &f, 4); wr32(c, a, u); if (reg == 3) x86_fpop(c); return; }
            case 5: c->fcw = rd16(c, a); return;
            case 7: wr16(c, a, c->fcw); return;
            }
            break;
        case 0xDB:
            switch (reg) {
            case 0: x86_fpush(c, (double)(int32_t)rd32(c, a)); return;
            case 1: case 2: case 3: {  // fisttp / fist / fistp m32
                double v = reg == 1 ? trunc(ST(0)) : fround(c, ST(0));
                int32_t i = (v != v || v >= 2147483648.0 || v < -2147483648.0) ? INT32_MIN : (int32_t)v;
                wr32(c, a, (uint32_t)i);
                if (reg != 2) x86_fpop(c);
                return;
            }
            case 5: { uint8_t b[10]; x86_check(c, a, 10); memcpy(b, c->mem + a, 10); x86_fpush(c, f80_to_double(b)); return; }
            case 7: { uint8_t b[10]; double_to_f80(ST(0), b); x86_check(c, a, 10); memcpy(c->mem + a, b, 10); x86_fpop(c); return; }
            }
            break;
        case 0xDD:
            switch (reg) {
            case 0: { uint64_t u = rd64(c, a); double g; memcpy(&g, &u, 8); x86_fpush(c, g); return; }
            case 1: {  // fisttp m64
                double v = trunc(ST(0));
                int64_t i = (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
                wr64(c, a, (uint64_t)i);
                x86_fpop(c);
                return;
            }
            case 2: case 3: { uint64_t u; double g = ST(0); memcpy(&u, &g, 8); wr64(c, a, u); if (reg == 3) x86_fpop(c); return; }
            case 7: wr16(c, a, (uint16_t)(c->fsw_cc | (c->top & 7) << 11)); return;
            }
            break;
        case 0xDF:
            switch (reg) {
            case 0: x86_fpush(c, (double)(int16_t)rd16(c, a)); return;
            case 1: case 2: case 3: {
                double v = reg == 1 ? trunc(ST(0)) : fround(c, ST(0));
                int16_t i = (v != v || v >= 32768.0 || v < -32768.0) ? INT16_MIN : (int16_t)v;
                wr16(c, a, (uint16_t)i);
                if (reg != 2) x86_fpop(c);
                return;
            }
            case 5: x86_fpush(c, (double)(int64_t)rd64(c, a)); return;
            case 7: {
                double v = fround(c, ST(0));
                int64_t i = (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? INT64_MIN : (int64_t)v;
                wr64(c, a, (uint64_t)i);
                x86_fpop(c);
                return;
            }
            }
            break;
        }
        x86_fault(c, "x87 %02x /%d (mem) at %#x", op, reg, d->start);
    }
    int i = d->rm;
    switch (op) {
    case 0xD8: case 0xDC: case 0xDE: {
        double *dst = op == 0xD8 ? &ST(0) : &ST(i);
        double s = op == 0xD8 ? ST(i) : ST(0);
        int pop = op == 0xDE;
        switch (reg) {
        case 0: *dst = *dst + s; break;
        case 1: *dst = *dst * s; break;
        case 2: fcom_flags(c, ST(0), ST(i), 0); if (op == 0xDE) x86_fault(c, "x87 de /2"); return;
        case 3:
            if (op == 0xDE && i == 1) { fcom_flags(c, ST(0), ST(1), 0); x86_fpop(c); x86_fpop(c); return; }
            fcom_flags(c, ST(0), ST(i), 0); x86_fpop(c); return;
        // D8: fsub st0-sti ; DC/DE: fsubr form swaps (Intel's encoding quirk)
        case 4: *dst = op == 0xD8 ? *dst - s : s - *dst; break;
        case 5: *dst = op == 0xD8 ? s - *dst : *dst - s; break;
        case 6: *dst = op == 0xD8 ? *dst / s : s / *dst; break;
        default: *dst = op == 0xD8 ? s / *dst : *dst / s; break;
        }
        if (pop) x86_fpop(c);
        return;
    }
    case 0xD9:
        if (reg == 0) { double v = ST(i); x86_fpush(c, v); return; }
        if (reg == 1) { double t = ST(0); ST(0) = ST(i); ST(i) = t; return; }
        if (reg == 4 && i == 0) { ST(0) = -ST(0); return; }
        if (reg == 4 && i == 1) { ST(0) = fabs(ST(0)); return; }
        if (reg == 4 && i == 5) {  // fxam
            double v = ST(0);
            int empty = c->tag[c->top], s = signbit(v) != 0, c0, c2, c3;
            if (empty) { c3 = 1; c2 = 0; c0 = 1; }
            else if (isnan(v)) { c3 = 0; c2 = 0; c0 = 1; }
            else if (isinf(v)) { c3 = 0; c2 = 1; c0 = 1; }
            else if (v == 0) { c3 = 1; c2 = 0; c0 = 0; }
            else { c3 = 0; c2 = 1; c0 = 0; }
            c->fsw_cc = (uint16_t)(c0 << 8 | s << 9 | c2 << 10 | c3 << 14);
            return;
        }
        if (reg == 5) {
            static const double k[] = {1.0, 3.321928094887362347870, 1.442695040888963407360,
                                       3.141592653589793238463, 0.301029995663981195214, 0.693147180559945309417, 0};
            if (i <= 6) { x86_fpush(c, k[i]); return; }
        }
        if (reg == 7 && i == 2) { ST(0) = sqrt(ST(0)); return; }
        if (reg == 7 && i == 4) { ST(0) = fround(c, ST(0)); return; }
        if (reg == 7 && i == 6) { ST(0) = sin(ST(0)); return; }
        if (reg == 7 && i == 7) { ST(0) = cos(ST(0)); return; }
        if (reg == 7 && i == 5) { ST(0) = ST(0) * exp2(trunc(ST(1))); return; }  // fscale
        if (reg == 6 && i == 0) { ST(0) = exp2(ST(0)) - 1; return; }              // f2xm1
        if (reg == 6 && i == 1) { double y = ST(1) * log2(ST(0)); x86_fpop(c); ST(0) = y; return; }  // fyl2x
        if (reg == 2 && i == 0) return;  // fnop
        break;
    case 0xDA:
        if (reg == 5 && i == 1) { fcom_flags(c, ST(0), ST(1), 0); x86_fpop(c); x86_fpop(c); return; }  // fucompp
        if (reg <= 3) {  // fcmovb/e/be/u
            int t = reg == 0 ? c->cf : reg == 1 ? c->zf : reg == 2 ? (c->cf | c->zf) : c->pf;
            if (t) ST(0) = ST(i);
            return;
        }
        break;
    case 0xDB:
        if (reg <= 3) {  // fcmovnb/ne/nbe/nu
            int t = reg == 0 ? c->cf : reg == 1 ? c->zf : reg == 2 ? (c->cf | c->zf) : c->pf;
            if (!t) ST(0) = ST(i);
            return;
        }
        if (reg == 4 && i == 2) { c->fsw_cc = 0; return; }  // fnclex
        if (reg == 4 && i == 3) { c->fcw = 0x037F; c->fsw_cc = 0; c->top = 0; for (int k = 0; k < 8; k++) c->tag[k] = 1; return; }
        if (reg == 5 || reg == 6) { fcom_flags(c, ST(0), ST(i), 1); return; }  // fucomi / fcomi
        break;
    case 0xDD:
        if (reg == 0) { c->tag[(c->top + i) & 7] = 1; return; }      // ffree
        if (reg == 2) { ST(i) = ST(0); return; }                     // fst st(i)
        if (reg == 3) { ST(i) = ST(0); x86_fpop(c); return; }        // fstp st(i)
        if (reg == 4 || reg == 5) { fcom_flags(c, ST(0), ST(i), 0); if (reg == 5) x86_fpop(c); return; }  // fucom(p)
        break;
    case 0xDF:
        if (reg == 4 && i == 0) { c->r[EAX] = (c->r[EAX] & ~0xFFFFu) | c->fsw_cc | (uint32_t)(c->top & 7) << 11; return; }  // fnstsw ax
        if (reg == 5 || reg == 6) { fcom_flags(c, ST(0), ST(i), 1); x86_fpop(c); return; }  // fucomip / fcomip
        break;
    }
    x86_fault(c, "x87 %02x %02x at %#x", op, 0xC0 | reg << 3 | i, d->start);
}

// ---------------------------------------------------------------- SSE helpers

typedef union { uint64_t q[2]; uint32_t d[4]; uint16_t w[8]; uint8_t b[16]; int8_t sb[16]; int16_t sw[8]; int32_t sd[4];
                int64_t sq[2]; float f[4]; double g[2]; } V;

static inline float sse_ftz_in(X86 *c, float f) {
    if ((c->mxcsr & 0x40) && fpclassify(f) == FP_SUBNORMAL) return copysignf(0, f);
    return f;
}

// source operand of an SSE instruction: xmm register or memory of `bytes`
static V xsrc(X86 *c, D *d, int bytes) {
    V v;
    if (d->mod == 3) { memcpy(&v, &c->xmm[d->rm], 16); return v; }
    memset(&v, 0, sizeof v);
    x86_check(c, d->ea, (uint32_t)bytes);
    memcpy(&v, c->mem + d->ea, (size_t)bytes);
    return v;
}
static inline V xget(X86 *c, int r) { V v; memcpy(&v, &c->xmm[r], 16); return v; }
static inline void xset(X86 *c, int r, V v) { memcpy(&c->xmm[r], &v, 16); }

static int32_t cvt_f2i(X86 *c, double v, int trunc_) {
    if (v != v || v >= 2147483648.0 || v < -2147483648.0) return INT32_MIN;
    if (trunc_) return (int32_t)v;
    switch ((c->mxcsr >> 13) & 3) {
    case 0: return (int32_t)nearbyint(v);
    case 1: return (int32_t)floor(v);
    case 2: return (int32_t)ceil(v);
    default: return (int32_t)v;
    }
}

// SSE NaN rules: if either is NaN, result is the first NaN operand (quieted)
static inline float fnan2(float a, float b) {
    uint32_t u;
    float r = a != a ? a : b;
    memcpy(&u, &r, 4); u |= 0x400000; memcpy(&r, &u, 4);
    return r;
}
static inline double dnan2(double a, double b) {
    uint64_t u;
    double r = a != a ? a : b;
    memcpy(&u, &r, 8); u |= 0x8000000000000ull; memcpy(&r, &u, 8);
    return r;
}
static float fop(int op, float a, float b) {
    if (a != a || b != b) {
        if (op == 0x5D || op == 0x5F) return b;  // min/max return src2 on NaN
        return fnan2(a, b);
    }
    float r;
    switch (op) {
    case 0x58: r = a + b; break;
    case 0x59: r = a * b; break;
    case 0x5C: r = a - b; break;
    case 0x5E: r = a / b; break;
    case 0x5D: return a < b ? a : b;
    default: return a > b ? a : b;
    }
    if (r != r) { uint32_t u = 0xFFC00000u; memcpy(&r, &u, 4); }  // default NaN
    return r;
}
static double dop(int op, double a, double b) {
    if (a != a || b != b) {
        if (op == 0x5D || op == 0x5F) return b;
        return dnan2(a, b);
    }
    double r;
    switch (op) {
    case 0x58: r = a + b; break;
    case 0x59: r = a * b; break;
    case 0x5C: r = a - b; break;
    case 0x5E: r = a / b; break;
    case 0x5D: return a < b ? a : b;
    default: return a > b ? a : b;
    }
    if (r != r) { uint64_t u = 0xFFF8000000000000ull; memcpy(&r, &u, 8); }
    return r;
}
// sqrt: NaN inputs are quieted; negative inputs give the default NaN (sign set)
static float fsqrt_x86(float v) {
    if (v != v) return fnan2(v, v);
    if (v < 0) { uint32_t u = 0xFFC00000u; float r; memcpy(&r, &u, 4); return r; }
    return sqrtf(v);
}
static double dsqrt_x86(double v) {
    if (v != v) return dnan2(v, v);
    if (v < 0) { uint64_t u = 0xFFF8000000000000ull; double r; memcpy(&r, &u, 8); return r; }
    return sqrt(v);
}

static int cmp_pred(int imm, double a, double b) {
    int un = a != a || b != b;
    switch (imm & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}
static inline uint8_t sat_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static inline int8_t sat_s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }
static inline uint16_t sat_u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }
static inline int16_t sat_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

// 0F xx with prefix (none / 66 / F3 / F2)
static void sse(X86 *c, D *d, uint8_t op) {
    int pfx = d->rep ? d->rep : d->has66 ? 0x66 : 0;
    modrm(c, d);
    int dst = d->reg;
    V a, b;
    switch (op) {
    case 0x10: case 0x11: {  // movups/movupd/movss/movsd
        int bytes = pfx == 0xF3 ? 4 : pfx == 0xF2 ? 8 : 16;
        if (op == 0x10) {
            if (d->mod == 3) {
                a = xget(c, dst); b = xget(c, d->rm);
                if (bytes == 4) a.d[0] = b.d[0]; else if (bytes == 8) a.q[0] = b.q[0]; else a = b;
                xset(c, dst, a);
            } else {
                b = xsrc(c, d, bytes);  // memory form zeroes the upper part
                xset(c, dst, b);
            }
        } else {
            a = xget(c, dst);
            if (d->mod == 3) {
                b = xget(c, d->rm);
                if (bytes == 4) b.d[0] = a.d[0]; else if (bytes == 8) b.q[0] = a.q[0]; else b = a;
                xset(c, d->rm, b);
            } else {
                x86_check(c, d->ea, (uint32_t)bytes);
                memcpy(c->mem + d->ea, &a, (size_t)bytes);
            }
        }
        return;
    }
    case 0x12: case 0x13: case 0x16: case 0x17:
        if (op == 0x12 && pfx == 0xF3) {  // movsldup
            b = xsrc(c, d, 16); a.d[0] = a.d[1] = b.d[0]; a.d[2] = a.d[3] = b.d[2]; xset(c, dst, a); return;
        }
        if (op == 0x16 && pfx == 0xF3) {  // movshdup
            b = xsrc(c, d, 16); a.d[0] = a.d[1] = b.d[1]; a.d[2] = a.d[3] = b.d[3]; xset(c, dst, a); return;
        }
        if (op == 0x12 && pfx == 0xF2) {  // movddup
            b = xsrc(c, d, 8); a.q[0] = a.q[1] = b.q[0]; xset(c, dst, a); return;
        }
        if (pfx == 0 || pfx == 0x66) {
            int hi = op >= 0x16;
            if (op == 0x12 || op == 0x16) {
                a = xget(c, dst);
                if (d->mod == 3) {  // movhlps / movlhps
                    b = xget(c, d->rm);
                    if (op == 0x12) a.q[0] = b.q[1]; else a.q[1] = b.q[0];
                } else {
                    a.q[hi] = rd64(c, d->ea);
                }
                xset(c, dst, a);
            } else {
                a = xget(c, dst);
                wr64(c, d->ea, a.q[hi]);
            }
            return;
        }
        break;
    case 0x14: case 0x15:  // unpcklps/pd, unpckhps/pd
        a = xget(c, dst); b = xsrc(c, d, 16);
        if (pfx == 0x66) { V r; if (op == 0x14) { r.q[0] = a.q[0]; r.q[1] = b.q[0]; } else { r.q[0] = a.q[1]; r.q[1] = b.q[1]; } xset(c, dst, r); return; }
        if (pfx == 0) {
            V r;
            int o = op == 0x14 ? 0 : 2;
            r.d[0] = a.d[o]; r.d[1] = b.d[o]; r.d[2] = a.d[o + 1]; r.d[3] = b.d[o + 1];
            xset(c, dst, r);
            return;
        }
        break;
    case 0x28: case 0x29:  // movaps/movapd
        if (pfx == 0 || pfx == 0x66) {
            if (op == 0x28) { if (d->mod != 3 && (d->ea & 15)) x86_fault(c, "misaligned movaps load %#x", d->ea); xset(c, dst, xsrc(c, d, 16)); }
            else if (d->mod == 3) xset(c, d->rm, xget(c, dst));
            else { if (d->ea & 15) x86_fault(c, "misaligned movaps store %#x", d->ea); a = xget(c, dst); x86_check(c, d->ea, 16); memcpy(c->mem + d->ea, &a, 16); }
            return;
        }
        break;
    case 0x2A: {  // cvtsi2ss/sd
        int32_t v = (int32_t)rm_get(c, d, 32);
        a = xget(c, dst);
        if (pfx == 0xF3) a.f[0] = (float)v;
        else if (pfx == 0xF2) a.g[0] = (double)v;
        else x86_fault(c, "cvtpi2ps (MMX) at %#x", d->start);
        xset(c, dst, a);
        return;
    }
    case 0x2C: case 0x2D: {  // cvt(t)ss2si / sd2si
        double v;
        if (pfx == 0xF3) v = xsrc(c, d, 4).f[0];
        else if (pfx == 0xF2) v = xsrc(c, d, 8).g[0];
        else x86_fault(c, "cvt(t)ps2pi (MMX) at %#x", d->start);
        c->r[dst] = (uint32_t)cvt_f2i(c, v, op == 0x2C);
        return;
    }
    case 0x2E: case 0x2F: {  // (u)comiss / (u)comisd
        double x, y;
        if (pfx == 0x66) { x = xget(c, dst).g[0]; y = xsrc(c, d, 8).g[0]; }
        else { x = xget(c, dst).f[0]; y = xsrc(c, d, 4).f[0]; }
        int un = x != x || y != y;
        c->zf = un || x == y; c->pf = un; c->cf = un || x < y;
        c->of = c->sf = c->af = 0;
        return;
    }
    case 0x50: {  // movmskps/pd
        a = xget(c, d->rm);
        c->r[dst] = pfx == 0x66 ? (uint32_t)(a.q[0] >> 63 | (a.q[1] >> 63) << 1)
                                : (a.d[0] >> 31 | (a.d[1] >> 31) << 1 | (a.d[2] >> 31) << 2 | (a.d[3] >> 31) << 3);
        return;
    }
    case 0x51: case 0x52: case 0x53: {  // sqrt / rsqrt / rcp
        a = xget(c, dst);
        int n = pfx == 0xF3 ? 1 : pfx == 0 ? 4 : 0;
        if (n) {
            b = xsrc(c, d, n == 1 ? 4 : 16);
            for (int k = 0; k < n; k++)
                a.f[k] = op == 0x51 ? fsqrt_x86(b.f[k]) : op == 0x52 ? 1.0f / sqrtf(b.f[k]) : 1.0f / b.f[k];
        } else {
            int m = pfx == 0xF2 ? 1 : 2;
            if (op != 0x51) break;
            b = xsrc(c, d, m == 1 ? 8 : 16);
            for (int k = 0; k < m; k++) a.g[k] = dsqrt_x86(b.g[k]);
        }
        xset(c, dst, a);
        return;
    }
    case 0x54: case 0x55: case 0x56: case 0x57:  // and / andn / or / xor (ps/pd)
        a = xget(c, dst); b = xsrc(c, d, 16);
        for (int k = 0; k < 2; k++)
            a.q[k] = op == 0x54 ? a.q[k] & b.q[k] : op == 0x55 ? ~a.q[k] & b.q[k] : op == 0x56 ? a.q[k] | b.q[k] : a.q[k] ^ b.q[k];
        xset(c, dst, a);
        return;
    case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        a = xget(c, dst);
        if (pfx == 0xF3) { b = xsrc(c, d, 4); a.f[0] = fop(op, sse_ftz_in(c, a.f[0]), sse_ftz_in(c, b.f[0])); }
        else if (pfx == 0xF2) { b = xsrc(c, d, 8); a.g[0] = dop(op, a.g[0], b.g[0]); }
        else if (pfx == 0) { b = xsrc(c, d, 16); for (int k = 0; k < 4; k++) a.f[k] = fop(op, a.f[k], b.f[k]); }
        else { b = xsrc(c, d, 16); for (int k = 0; k < 2; k++) a.g[k] = dop(op, a.g[k], b.g[k]); }
        xset(c, dst, a);
        return;
    case 0x5A:  // cvtss2sd / cvtsd2ss / cvtps2pd / cvtpd2ps
        a = xget(c, dst);
        if (pfx == 0xF3) { b = xsrc(c, d, 4); a.g[0] = (double)b.f[0]; }
        else if (pfx == 0xF2) { b = xsrc(c, d, 8); a.f[0] = (float)b.g[0]; }
        else if (pfx == 0) { b = xsrc(c, d, 8); a.g[1] = (double)b.f[1]; a.g[0] = (double)b.f[0]; }
        else { b = xsrc(c, d, 16); a.f[0] = (float)b.g[0]; a.f[1] = (float)b.g[1]; a.q[1] = 0; }
        xset(c, dst, a);
        return;
    case 0x5B:  // cvtdq2ps / cvtps2dq / cvttps2dq
        b = xsrc(c, d, 16);
        if (pfx == 0) for (int k = 0; k < 4; k++) a.f[k] = (float)b.sd[k];
        else for (int k = 0; k < 4; k++) a.sd[k] = cvt_f2i(c, b.f[k], pfx == 0xF3);
        xset(c, dst, a);
        return;
    case 0x70: {  // pshufd / pshufhw / pshuflw
        b = xsrc(c, d, 16);
        uint8_t imm = fetch8(c);
        a = b;
        if (pfx == 0x66) for (int k = 0; k < 4; k++) a.d[k] = b.d[imm >> (2 * k) & 3];
        else if (pfx == 0xF3) for (int k = 0; k < 4; k++) a.w[4 + k] = b.w[4 + (imm >> (2 * k) & 3)];
        else if (pfx == 0xF2) for (int k = 0; k < 4; k++) a.w[k] = b.w[imm >> (2 * k) & 3];
        else break;
        xset(c, dst, a);
        return;
    }
    case 0x71: case 0x72: case 0x73: {  // shifts by immediate (66 only)
        if (pfx != 0x66) break;
        uint8_t imm = fetch8(c);
        a = xget(c, d->rm);
        int kind = dst;  // 2 srl, 4 sra, 6 sll, 3 psrldq, 7 pslldq
        if (op == 0x71) for (int k = 0; k < 8; k++) a.w[k] = kind == 2 ? (imm > 15 ? 0 : a.w[k] >> imm) : kind == 4 ? (uint16_t)(a.sw[k] >> (imm > 15 ? 15 : imm)) : (imm > 15 ? 0 : (uint16_t)(a.w[k] << imm));
        else if (op == 0x72) for (int k = 0; k < 4; k++) a.d[k] = kind == 2 ? (imm > 31 ? 0 : a.d[k] >> imm) : kind == 4 ? (uint32_t)(a.sd[k] >> (imm > 31 ? 31 : imm)) : (imm > 31 ? 0 : a.d[k] << imm);
        else if (kind == 2 || kind == 6) for (int k = 0; k < 2; k++) a.q[k] = imm > 63 ? 0 : kind == 2 ? a.q[k] >> imm : a.q[k] << imm;
        else if (kind == 3 || kind == 7) {
            V r; memset(&r, 0, sizeof r);
            int n = imm > 16 ? 16 : imm;
            if (kind == 3) memcpy(r.b, a.b + n, (size_t)(16 - n)); else memcpy(r.b + n, a.b, (size_t)(16 - n));
            a = r;
        } else break;
        xset(c, d->rm, a);
        return;
    }
    case 0x7C: case 0x7D:  // haddps/hsubps (F2), haddpd/hsubpd (66)
        a = xget(c, dst); b = xsrc(c, d, 16);
        if (pfx == 0xF2) {
            V r;
            int s = op == 0x7D;
            r.f[0] = s ? a.f[0] - a.f[1] : a.f[0] + a.f[1];
            r.f[1] = s ? a.f[2] - a.f[3] : a.f[2] + a.f[3];
            r.f[2] = s ? b.f[0] - b.f[1] : b.f[0] + b.f[1];
            r.f[3] = s ? b.f[2] - b.f[3] : b.f[2] + b.f[3];
            xset(c, dst, r);
            return;
        }
        if (pfx == 0x66) {
            V r;
            int s = op == 0x7D;
            r.g[0] = s ? a.g[0] - a.g[1] : a.g[0] + a.g[1];
            r.g[1] = s ? b.g[0] - b.g[1] : b.g[0] + b.g[1];
            xset(c, dst, r);
            return;
        }
        break;
    case 0xD0:  // addsubpd (66) / addsubps (F2)
        a = xget(c, dst); b = xsrc(c, d, 16);
        if (pfx == 0xF2) { a.f[0] -= b.f[0]; a.f[1] += b.f[1]; a.f[2] -= b.f[2]; a.f[3] += b.f[3]; xset(c, dst, a); return; }
        if (pfx == 0x66) { a.g[0] -= b.g[0]; a.g[1] += b.g[1]; xset(c, dst, a); return; }
        break;
    case 0xC2: {  // cmpps/pd/ss/sd
        uint8_t imm;
        a = xget(c, dst);
        if (pfx == 0xF3) { b = xsrc(c, d, 4); imm = fetch8(c); a.d[0] = cmp_pred(imm, a.f[0], b.f[0]) ? ~0u : 0; }
        else if (pfx == 0xF2) { b = xsrc(c, d, 8); imm = fetch8(c); a.q[0] = cmp_pred(imm, a.g[0], b.g[0]) ? ~0ull : 0; }
        else if (pfx == 0) { b = xsrc(c, d, 16); imm = fetch8(c); for (int k = 0; k < 4; k++) a.d[k] = cmp_pred(imm, a.f[k], b.f[k]) ? ~0u : 0; }
        else { b = xsrc(c, d, 16); imm = fetch8(c); for (int k = 0; k < 2; k++) a.q[k] = cmp_pred(imm, a.g[k], b.g[k]) ? ~0ull : 0; }
        xset(c, dst, a);
        return;
    }
    case 0xC6: {  // shufps / shufpd
        a = xget(c, dst); b = xsrc(c, d, 16);
        uint8_t imm = fetch8(c);
        V r;
        if (pfx == 0) { r.d[0] = a.d[imm & 3]; r.d[1] = a.d[imm >> 2 & 3]; r.d[2] = b.d[imm >> 4 & 3]; r.d[3] = b.d[imm >> 6 & 3]; }
        else { r.q[0] = a.q[imm & 1]; r.q[1] = b.q[imm >> 1 & 1]; }
        xset(c, dst, r);
        return;
    }
    case 0xE6:  // cvttpd2dq (66) / cvtdq2pd (F3) / cvtpd2dq (F2)
        if (pfx == 0xF3) { b = xsrc(c, d, 8); a.g[1] = b.sd[1]; a.g[0] = b.sd[0]; xset(c, dst, a); return; }
        if (pfx == 0x66 || pfx == 0xF2) { b = xsrc(c, d, 16); a.sd[0] = cvt_f2i(c, b.g[0], pfx == 0x66); a.sd[1] = cvt_f2i(c, b.g[1], pfx == 0x66); a.q[1] = 0; xset(c, dst, a); return; }
        break;
    }
    // ---- SSE2 integer (66 prefix) and moves
    if (op == 0x6E && pfx == 0x66) { memset(&a, 0, sizeof a); a.d[0] = rm_get(c, d, 32); xset(c, dst, a); return; }       // movd xmm, r/m32
    if (op == 0x7E && pfx == 0x66) { rm_set(c, d, 32, xget(c, dst).d[0]); return; }                                       // movd r/m32, xmm
    if (op == 0x7E && pfx == 0xF3) { b = xsrc(c, d, 8); memset(&a, 0, sizeof a); a.q[0] = b.q[0]; xset(c, dst, a); return; }  // movq xmm, xmm/m64
    if (op == 0xD6 && pfx == 0x66) {                                                                                       // movq xmm/m64, xmm
        a = xget(c, dst);
        if (d->mod == 3) { b.q[0] = a.q[0]; b.q[1] = 0; xset(c, d->rm, b); } else wr64(c, d->ea, a.q[0]);
        return;
    }
    if ((op == 0x6F || op == 0x7F) && (pfx == 0x66 || pfx == 0xF3)) {  // movdqa / movdqu
        if (op == 0x6F) { if (pfx == 0x66 && d->mod != 3 && (d->ea & 15)) x86_fault(c, "misaligned movdqa %#x", d->ea); xset(c, dst, xsrc(c, d, 16)); }
        else if (d->mod == 3) xset(c, d->rm, xget(c, dst));
        else { if (pfx == 0x66 && (d->ea & 15)) x86_fault(c, "misaligned movdqa %#x", d->ea); a = xget(c, dst); x86_check(c, d->ea, 16); memcpy(c->mem + d->ea, &a, 16); }
        return;
    }
    if (op == 0xF0 && pfx == 0xF2) { xset(c, dst, xsrc(c, d, 16)); return; }  // lddqu
    if (op == 0xE7 && pfx == 0x66) { a = xget(c, dst); x86_check(c, d->ea, 16); memcpy(c->mem + d->ea, &a, 16); return; }  // movntdq
    if (op == 0x2B && (pfx == 0 || pfx == 0x66)) { a = xget(c, dst); x86_check(c, d->ea, 16); memcpy(c->mem + d->ea, &a, 16); return; }  // movntps/pd
    if (op == 0xD7 && pfx == 0x66) {  // pmovmskb
        a = xget(c, d->rm);
        uint32_t m = 0;
        for (int k = 0; k < 16; k++) m |= (uint32_t)(a.b[k] >> 7) << k;
        c->r[dst] = m;
        return;
    }
    if (pfx == 0x66) {
        a = xget(c, dst);
        b = xsrc(c, d, 16);
        V r = a;
        int k;
        switch (op) {
        case 0x60: for (k = 0; k < 8; k++) { r.b[2 * k] = a.b[k]; r.b[2 * k + 1] = b.b[k]; } break;
        case 0x61: for (k = 0; k < 4; k++) { r.w[2 * k] = a.w[k]; r.w[2 * k + 1] = b.w[k]; } break;
        case 0x62: r.d[0] = a.d[0]; r.d[1] = b.d[0]; r.d[2] = a.d[1]; r.d[3] = b.d[1]; break;
        case 0x6C: r.q[0] = a.q[0]; r.q[1] = b.q[0]; break;
        case 0x68: for (k = 0; k < 8; k++) { r.b[2 * k] = a.b[8 + k]; r.b[2 * k + 1] = b.b[8 + k]; } break;
        case 0x69: for (k = 0; k < 4; k++) { r.w[2 * k] = a.w[4 + k]; r.w[2 * k + 1] = b.w[4 + k]; } break;
        case 0x6A: r.d[0] = a.d[2]; r.d[1] = b.d[2]; r.d[2] = a.d[3]; r.d[3] = b.d[3]; break;
        case 0x6D: r.q[0] = a.q[1]; r.q[1] = b.q[1]; break;
        case 0x63: for (k = 0; k < 8; k++) { r.sb[k] = sat_s8(a.sw[k]); r.sb[8 + k] = sat_s8(b.sw[k]); } break;
        case 0x67: for (k = 0; k < 8; k++) { r.b[k] = sat_u8(a.sw[k]); r.b[8 + k] = sat_u8(b.sw[k]); } break;
        case 0x6B: for (k = 0; k < 4; k++) { r.sw[k] = sat_s16(a.sd[k]); r.sw[4 + k] = sat_s16(b.sd[k]); } break;
        case 0x64: for (k = 0; k < 16; k++) r.b[k] = a.sb[k] > b.sb[k] ? 0xFF : 0; break;
        case 0x65: for (k = 0; k < 8; k++) r.w[k] = a.sw[k] > b.sw[k] ? 0xFFFF : 0; break;
        case 0x66: for (k = 0; k < 4; k++) r.d[k] = a.sd[k] > b.sd[k] ? ~0u : 0; break;
        case 0x74: for (k = 0; k < 16; k++) r.b[k] = a.b[k] == b.b[k] ? 0xFF : 0; break;
        case 0x75: for (k = 0; k < 8; k++) r.w[k] = a.w[k] == b.w[k] ? 0xFFFF : 0; break;
        case 0x76: for (k = 0; k < 4; k++) r.d[k] = a.d[k] == b.d[k] ? ~0u : 0; break;
        case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: {
            uint64_t n = b.q[0];
            if (op == 0xD1) for (k = 0; k < 8; k++) r.w[k] = n > 15 ? 0 : a.w[k] >> n;
            if (op == 0xD2) for (k = 0; k < 4; k++) r.d[k] = n > 31 ? 0 : a.d[k] >> n;
            if (op == 0xD3) for (k = 0; k < 2; k++) r.q[k] = n > 63 ? 0 : a.q[k] >> n;
            if (op == 0xE1) for (k = 0; k < 8; k++) r.sw[k] = (int16_t)(a.sw[k] >> (n > 15 ? 15 : n));
            if (op == 0xE2) for (k = 0; k < 4; k++) r.sd[k] = a.sd[k] >> (n > 31 ? 31 : n);
            if (op == 0xF1) for (k = 0; k < 8; k++) r.w[k] = n > 15 ? 0 : (uint16_t)(a.w[k] << n);
            if (op == 0xF2) for (k = 0; k < 4; k++) r.d[k] = n > 31 ? 0 : a.d[k] << n;
            if (op == 0xF3) for (k = 0; k < 2; k++) r.q[k] = n > 63 ? 0 : a.q[k] << n;
            break;
        }
        case 0xD4: for (k = 0; k < 2; k++) r.q[k] = a.q[k] + b.q[k]; break;
        case 0xD5: for (k = 0; k < 8; k++) r.w[k] = (uint16_t)(a.sw[k] * b.sw[k]); break;
        case 0xD8: for (k = 0; k < 16; k++) r.b[k] = sat_u8(a.b[k] - b.b[k]); break;
        case 0xD9: for (k = 0; k < 8; k++) r.w[k] = sat_u16(a.w[k] - b.w[k]); break;
        case 0xDA: for (k = 0; k < 16; k++) r.b[k] = a.b[k] < b.b[k] ? a.b[k] : b.b[k]; break;
        case 0xDB: r.q[0] = a.q[0] & b.q[0]; r.q[1] = a.q[1] & b.q[1]; break;
        case 0xDC: for (k = 0; k < 16; k++) r.b[k] = sat_u8(a.b[k] + b.b[k]); break;
        case 0xDD: for (k = 0; k < 8; k++) r.w[k] = sat_u16(a.w[k] + b.w[k]); break;
        case 0xDE: for (k = 0; k < 16; k++) r.b[k] = a.b[k] > b.b[k] ? a.b[k] : b.b[k]; break;
        case 0xDF: r.q[0] = ~a.q[0] & b.q[0]; r.q[1] = ~a.q[1] & b.q[1]; break;
        case 0xE0: for (k = 0; k < 16; k++) r.b[k] = (uint8_t)((a.b[k] + b.b[k] + 1) >> 1); break;
        case 0xE3: for (k = 0; k < 8; k++) r.w[k] = (uint16_t)((a.w[k] + b.w[k] + 1) >> 1); break;
        case 0xE4: for (k = 0; k < 8; k++) r.w[k] = (uint16_t)(((uint32_t)a.w[k] * b.w[k]) >> 16); break;
        case 0xE5: for (k = 0; k < 8; k++) r.w[k] = (uint16_t)(((int32_t)a.sw[k] * b.sw[k]) >> 16); break;
        case 0xE8: for (k = 0; k < 16; k++) r.sb[k] = sat_s8(a.sb[k] - b.sb[k]); break;
        case 0xE9: for (k = 0; k < 8; k++) r.sw[k] = sat_s16(a.sw[k] - b.sw[k]); break;
        case 0xEA: for (k = 0; k < 8; k++) r.sw[k] = a.sw[k] < b.sw[k] ? a.sw[k] : b.sw[k]; break;
        case 0xEB: r.q[0] = a.q[0] | b.q[0]; r.q[1] = a.q[1] | b.q[1]; break;
        case 0xEC: for (k = 0; k < 16; k++) r.sb[k] = sat_s8(a.sb[k] + b.sb[k]); break;
        case 0xED: for (k = 0; k < 8; k++) r.sw[k] = sat_s16(a.sw[k] + b.sw[k]); break;
        case 0xEE: for (k = 0; k < 8; k++) r.sw[k] = a.sw[k] > b.sw[k] ? a.sw[k] : b.sw[k]; break;
        case 0xEF: r.q[0] = a.q[0] ^ b.q[0]; r.q[1] = a.q[1] ^ b.q[1]; break;
        case 0xF4: r.q[0] = (uint64_t)a.d[0] * b.d[0]; r.q[1] = (uint64_t)a.d[2] * b.d[2]; break;
        case 0xF5: for (k = 0; k < 4; k++) r.sd[k] = a.sw[2 * k] * b.sw[2 * k] + a.sw[2 * k + 1] * b.sw[2 * k + 1]; break;
        case 0xF6: { uint32_t s0 = 0, s1 = 0; for (k = 0; k < 8; k++) { s0 += (uint32_t)abs(a.b[k] - b.b[k]); s1 += (uint32_t)abs(a.b[8 + k] - b.b[8 + k]); } r.q[0] = s0; r.q[1] = s1; break; }
        case 0xF8: for (k = 0; k < 16; k++) r.b[k] = (uint8_t)(a.b[k] - b.b[k]); break;
        case 0xF9: for (k = 0; k < 8; k++) r.w[k] = (uint16_t)(a.w[k] - b.w[k]); break;
        case 0xFA: for (k = 0; k < 4; k++) r.d[k] = a.d[k] - b.d[k]; break;
        case 0xFB: for (k = 0; k < 2; k++) r.q[k] = a.q[k] - b.q[k]; break;
        case 0xFC: for (k = 0; k < 16; k++) r.b[k] = (uint8_t)(a.b[k] + b.b[k]); break;
        case 0xFD: for (k = 0; k < 8; k++) r.w[k] = (uint16_t)(a.w[k] + b.w[k]); break;
        case 0xFE: for (k = 0; k < 4; k++) r.d[k] = a.d[k] + b.d[k]; break;
        default: goto bad;
        }
        xset(c, dst, r);
        return;
    }
bad:
    x86_fault(c, "unsupported SSE 0f %02x (prefix %02x) at %#x", op, pfx, d->start);
}

// ---------------------------------------------------------------- string ops

static void string_op(X86 *c, D *d, uint8_t op) {
    int sz = (op & 1) ? d->opsz : 8, step = sz / 8;
    int32_t delta = c->df ? -step : step;
    int rep = d->rep;
    for (;;) {
        if (rep && c->r[ECX] == 0) return;
        switch (op) {
        case 0xA4: case 0xA5: {  // movs
            uint32_t v = sz == 8 ? rd8(c, c->r[ESI] + d->seg) : sz == 16 ? rd16(c, c->r[ESI] + d->seg) : rd32(c, c->r[ESI] + d->seg);
            if (sz == 8) wr8(c, c->r[EDI], (uint8_t)v); else if (sz == 16) wr16(c, c->r[EDI], (uint16_t)v); else wr32(c, c->r[EDI], v);
            c->r[ESI] += (uint32_t)delta; c->r[EDI] += (uint32_t)delta;
            break;
        }
        case 0xAA: case 0xAB:  // stos
            if (sz == 8) wr8(c, c->r[EDI], (uint8_t)c->r[EAX]); else if (sz == 16) wr16(c, c->r[EDI], (uint16_t)c->r[EAX]); else wr32(c, c->r[EDI], c->r[EAX]);
            c->r[EDI] += (uint32_t)delta;
            break;
        case 0xAC: case 0xAD: {  // lods
            uint32_t v = sz == 8 ? rd8(c, c->r[ESI] + d->seg) : sz == 16 ? rd16(c, c->r[ESI] + d->seg) : rd32(c, c->r[ESI] + d->seg);
            reg_set(c, EAX, sz, v);
            c->r[ESI] += (uint32_t)delta;
            break;
        }
        case 0xA6: case 0xA7: {  // cmps
            uint32_t x = sz == 8 ? rd8(c, c->r[ESI] + d->seg) : sz == 16 ? rd16(c, c->r[ESI] + d->seg) : rd32(c, c->r[ESI] + d->seg);
            uint32_t y = sz == 8 ? rd8(c, c->r[EDI]) : sz == 16 ? rd16(c, c->r[EDI]) : rd32(c, c->r[EDI]);
            alu(c, 7, x, y, sz);
            c->r[ESI] += (uint32_t)delta; c->r[EDI] += (uint32_t)delta;
            break;
        }
        default: {  // scas
            uint32_t y = sz == 8 ? rd8(c, c->r[EDI]) : sz == 16 ? rd16(c, c->r[EDI]) : rd32(c, c->r[EDI]);
            alu(c, 7, c->r[EAX], y, sz);
            c->r[EDI] += (uint32_t)delta;
            break;
        }
        }
        if (!rep) return;
        c->r[ECX]--;
        if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
            if (rep == 0xF3 && !c->zf) return;
            if (rep == 0xF2 && c->zf) return;
        }
    }
}

// ---------------------------------------------------------------- 0F xx

static void two_byte(X86 *c, D *d) {
    uint8_t op = fetch8(c);
    int sz = d->opsz;
    if (op >= 0x80 && op <= 0x8F) {  // jcc rel32
        int32_t rel = (int32_t)fetch32(c);
        if (cond(c, op & 15)) c->eip += (uint32_t)rel;
        return;
    }
    if (op >= 0x90 && op <= 0x9F) { modrm(c, d); rm_set(c, d, 8, (uint32_t)cond(c, op & 15)); return; }
    if (op >= 0x40 && op <= 0x4F) {  // cmovcc
        modrm(c, d);
        uint32_t v = rm_get(c, d, sz);
        if (cond(c, op & 15)) reg_set(c, d->reg, sz, v);
        return;
    }
    if (op >= 0xC8 && op <= 0xCF) { c->r[op & 7] = __builtin_bswap32(c->r[op & 7]); return; }
    switch (op) {
    case 0x0B: x86_fault(c, "ud2 at %#x", d->start);
    case 0x18: case 0x1F: modrm(c, d); return;  // prefetch / nop Ev
    case 0x31: { c->r[EAX] = (uint32_t)c->icount; c->r[EDX] = (uint32_t)(c->icount >> 32); return; }  // rdtsc
    case 0xA2: {  // cpuid: a Core 2 with SSE3/SSSE3
        uint32_t leaf = c->r[EAX];
        if (leaf == 0) { c->r[EAX] = 1; c->r[EBX] = 0x756E6547; c->r[EDX] = 0x49656E69; c->r[ECX] = 0x6C65746E; }
        else { c->r[EAX] = 0x6FB; c->r[EBX] = 0; c->r[ECX] = 0x00000201; c->r[EDX] = 0x078BFBFF; }
        return;
    }
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {  // bt/bts/btr/btc r/m, reg
        modrm(c, d);
        int32_t bit = (int32_t)reg_get(c, d->reg, sz);
        uint32_t v;
        if (d->mod == 3) {
            bit &= sz - 1;
            v = rm_get(c, d, sz);
            c->cf = v >> bit & 1;
            if (op == 0xAB) v |= 1u << bit; else if (op == 0xB3) v &= ~(1u << bit); else if (op == 0xBB) v ^= 1u << bit;
            if (op != 0xA3) rm_set(c, d, sz, v);
        } else {
            uint32_t a = d->ea + (uint32_t)((bit >> 3) & ~((sz >> 3) - 1));
            int b = bit & (sz - 1);
            v = sz == 16 ? rd16(c, a) : rd32(c, a);
            c->cf = v >> b & 1;
            if (op == 0xAB) v |= 1u << b; else if (op == 0xB3) v &= ~(1u << b); else if (op == 0xBB) v ^= 1u << b;
            if (op != 0xA3) { if (sz == 16) wr16(c, a, (uint16_t)v); else wr32(c, a, v); }
        }
        return;
    }
    case 0xBA: {  // bt group with imm8
        modrm(c, d);
        uint8_t imm = fetch8(c);
        int b = imm & (sz - 1);
        uint32_t v = rm_get(c, d, sz);
        c->cf = v >> b & 1;
        if (d->reg == 5) v |= 1u << b; else if (d->reg == 6) v &= ~(1u << b); else if (d->reg == 7) v ^= 1u << b;
        else if (d->reg != 4) x86_fault(c, "0f ba /%d", d->reg);
        if (d->reg != 4) rm_set(c, d, sz, v);
        return;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: {  // shld / shrd
        modrm(c, d);
        uint32_t cnt = (op & 1) ? c->r[ECX] & 31 : fetch8(c) & 31u;
        if (!cnt) return;
        uint32_t dst = rm_get(c, d, sz), src = reg_get(c, d->reg, sz), r;
        if (sz == 16) x86_fault(c, "16-bit shld/shrd at %#x", d->start);
        if (op <= 0xA5) { r = dst << cnt | src >> (32 - cnt); c->cf = dst >> (32 - cnt) & 1; }
        else { r = dst >> cnt | src << (32 - cnt); c->cf = dst >> (cnt - 1) & 1; }
        c->of = ((r ^ dst) >> 31) & 1;
        c->af = 0;
        szp(c, r, 32);
        rm_set(c, d, 32, r);
        return;
    }
    case 0xAF: {  // imul Gv, Ev
        modrm(c, d);
        int64_t p = (int64_t)(int32_t)sext(reg_get(c, d->reg, sz), sz) * (int32_t)sext(rm_get(c, d, sz), sz);
        uint32_t r = (uint32_t)p & mask_of(sz);
        c->cf = c->of = (int64_t)(int32_t)sext(r, sz) != p;
        szp(c, r, sz);
        c->af = 0;
        reg_set(c, d->reg, sz, r);
        return;
    }
    case 0xB0: case 0xB1: {  // cmpxchg
        modrm(c, d);
        int s = op == 0xB0 ? 8 : sz;
        uint32_t v = rm_get(c, d, s), acc = reg_get(c, EAX, s);
        alu(c, 7, acc, v, s);
        if (c->zf) rm_set(c, d, s, reg_get(c, d->reg, s));
        else { rm_set(c, d, s, v); reg_set(c, EAX, s, v); }
        return;
    }
    case 0xC0: case 0xC1: {  // xadd
        modrm(c, d);
        int s = op == 0xC0 ? 8 : sz;
        uint32_t v = rm_get(c, d, s), r = reg_get(c, d->reg, s);
        uint32_t sum = alu(c, 0, v, r, s);
        reg_set(c, d->reg, s, v);
        rm_set(c, d, s, sum);
        return;
    }
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {  // movzx / movsx
        modrm(c, d);
        int s = (op & 1) ? 16 : 8;
        uint32_t v = rm_get(c, d, s);
        if (op >= 0xBE) v = sext(v, s);
        reg_set(c, d->reg, sz, v);
        return;
    }
    case 0xBC: case 0xBD: {  // bsf / bsr
        modrm(c, d);
        uint32_t v = rm_get(c, d, sz);
        c->zf = v == 0;
        if (v) reg_set(c, d->reg, sz, op == 0xBC ? (uint32_t)__builtin_ctz(v) : 31u - (uint32_t)__builtin_clz(v));
        return;
    }
    case 0xAE: {  // ldmxcsr / stmxcsr / fences
        uint8_t m = rd8(c, c->eip);
        if (m >= 0xE8) { c->eip++; return; }  // lfence/mfence/sfence
        modrm(c, d);
        if (d->reg == 2) { c->mxcsr = rd32(c, d->ea); return; }
        if (d->reg == 3) { wr32(c, d->ea, c->mxcsr); return; }
        x86_fault(c, "0f ae /%d at %#x", d->reg, d->start);
    }
    case 0x38: {  // SSSE3: pshufb only
        uint8_t op3 = fetch8(c);
        if (op3 == 0x00 && d->has66) {
            modrm(c, d);
            V a = xget(c, d->reg), b = xsrc(c, d, 16), r;
            for (int k = 0; k < 16; k++) r.b[k] = (b.b[k] & 0x80) ? 0 : a.b[b.b[k] & 15];
            xset(c, d->reg, r);
            return;
        }
        x86_fault(c, "0f 38 %02x at %#x", op3, d->start);
    }
    }
    sse(c, d, op);
}

// ---------------------------------------------------------------- main step

static void exec(X86 *c) {
    D d;
    d.start = c->eip;
    d.opsz = 32; d.rep = 0; d.seg = 0; d.has66 = 0;
    uint8_t op;
    for (;;) {
        op = fetch8(c);
        if (op == 0x66) { d.opsz = 16; d.has66 = 1; continue; }
        if (op == 0xF3 || op == 0xF2) { d.rep = op; continue; }
        if (op == 0xF0 || op == 0x2E || op == 0x3E || op == 0x26 || op == 0x36) continue;  // lock / flat segments
        if (op == 0x64) { d.seg = c->fs_base; continue; }
        if (op == 0x65) { d.seg = c->gs_base; continue; }
        break;
    }
    int sz = d.opsz;
    if (op < 0x40 && (op & 7) < 6) {  // ALU group 00-3D
        int aop = op >> 3, form = op & 7, s = (form & 1) ? sz : 8;
        if (form < 4) {
            modrm(c, &d);
            uint32_t a, b;
            if (form < 2) { a = rm_get(c, &d, s); b = reg_get(c, d.reg, s); }
            else { a = reg_get(c, d.reg, s); b = rm_get(c, &d, s); }
            uint32_t r = alu(c, aop, a, b, s);
            if (aop != 7) { if (form < 2) rm_set(c, &d, s, r); else reg_set(c, d.reg, s, r); }
        } else {
            uint32_t b = s == 8 ? fetch8(c) : s == 16 ? fetch16(c) : fetch32(c);
            uint32_t r = alu(c, aop, reg_get(c, EAX, s), b, s);
            if (aop != 7) reg_set(c, EAX, s, r);
        }
        return;
    }
    if (op >= 0x40 && op <= 0x4F) { reg_set(c, op & 7, sz, incdec(c, reg_get(c, op & 7, sz), op >= 0x48, sz)); return; }
    if (op >= 0x50 && op <= 0x57) { uint32_t v = c->r[op & 7]; if (sz == 16) { c->r[ESP] -= 2; wr16(c, c->r[ESP], (uint16_t)v); } else x86_push(c, v); return; }
    if (op >= 0x58 && op <= 0x5F) { if (sz == 16) { setr16(c, op & 7, rd16(c, c->r[ESP])); c->r[ESP] += 2; } else { uint32_t v = x86_pop(c); c->r[op & 7] = v; } return; }
    if (op >= 0x70 && op <= 0x7F) { int8_t rel = (int8_t)fetch8(c); if (cond(c, op & 15)) c->eip += (uint32_t)(int32_t)rel; return; }
    if (op >= 0x91 && op <= 0x97) { uint32_t t = reg_get(c, EAX, sz); reg_set(c, EAX, sz, reg_get(c, op & 7, sz)); reg_set(c, op & 7, sz, t); return; }
    if (op >= 0xB0 && op <= 0xB7) { setr8(c, op & 7, fetch8(c)); return; }
    if (op >= 0xB8 && op <= 0xBF) { reg_set(c, op & 7, sz, sz == 16 ? fetch16(c) : fetch32(c)); return; }
    if (op >= 0xD8 && op <= 0xDF) { x87(c, &d, op); return; }
    switch (op) {
    case 0x0F: two_byte(c, &d); return;
    case 0x60: {  // pusha
        uint32_t sp = c->r[ESP];
        for (int i = 0; i < 8; i++) x86_push(c, i == ESP ? sp : c->r[i]);
        return;
    }
    case 0x61:  // popa
        for (int i = 7; i >= 0; i--) { uint32_t v = x86_pop(c); if (i != ESP) c->r[i] = v; }
        return;
    case 0x68: x86_push(c, sz == 16 ? fetch16(c) : fetch32(c)); return;
    case 0x6A: x86_push(c, (uint32_t)(int32_t)(int8_t)fetch8(c)); return;
    case 0x69: case 0x6B: {  // imul Gv, Ev, imm
        modrm(c, &d);
        int32_t a = (int32_t)sext(rm_get(c, &d, sz), sz);
        int32_t b = op == 0x6B ? (int8_t)fetch8(c) : sz == 16 ? (int16_t)fetch16(c) : (int32_t)fetch32(c);
        int64_t p = (int64_t)a * b;
        uint32_t r = (uint32_t)p & mask_of(sz);
        c->cf = c->of = (int64_t)(int32_t)sext(r, sz) != p;
        szp(c, r, sz);
        c->af = 0;
        reg_set(c, d.reg, sz, r);
        return;
    }
    case 0x80: case 0x81: case 0x82: case 0x83: {
        int s = (op & 1) ? sz : 8;
        modrm(c, &d);
        uint32_t b = op == 0x81 ? (sz == 16 ? fetch16(c) : fetch32(c)) : op == 0x83 ? sext(fetch8(c), 8) : fetch8(c);
        uint32_t r = alu(c, d.reg, rm_get(c, &d, s), b, s);
        if (d.reg != 7) rm_set(c, &d, s, r);
        return;
    }
    case 0x84: case 0x85: { int s = (op & 1) ? sz : 8; modrm(c, &d); alu(c, 4, rm_get(c, &d, s), reg_get(c, d.reg, s), s); return; }
    case 0x86: case 0x87: {
        int s = (op & 1) ? sz : 8;
        modrm(c, &d);
        uint32_t a = rm_get(c, &d, s), b = reg_get(c, d.reg, s);
        rm_set(c, &d, s, b);
        reg_set(c, d.reg, s, a);
        return;
    }
    case 0x88: case 0x89: { int s = (op & 1) ? sz : 8; modrm(c, &d); rm_set(c, &d, s, reg_get(c, d.reg, s)); return; }
    case 0x8A: case 0x8B: { int s = (op & 1) ? sz : 8; modrm(c, &d); reg_set(c, d.reg, s, rm_get(c, &d, s)); return; }
    case 0x8D: modrm(c, &d); reg_set(c, d.reg, sz, d.ea - d.seg); return;
    case 0x8F: {
        // pop Ev: ESP is incremented before the effective address is used
        uint32_t v = x86_pop(c);
        modrm(c, &d);
        rm_set(c, &d, sz, v);
        return;
    }
    case 0x90: return;
    case 0x98: if (sz == 16) setr16(c, EAX, (uint16_t)(int16_t)(int8_t)c->r[EAX]); else c->r[EAX] = (uint32_t)(int32_t)(int16_t)c->r[EAX]; return;
    case 0x99: if (sz == 16) setr16(c, EDX, (c->r[EAX] & 0x8000) ? 0xFFFF : 0); else c->r[EDX] = (c->r[EAX] & 0x80000000u) ? ~0u : 0; return;
    case 0x9C: x86_push(c, x86_eflags(c)); return;
    case 0x9D: x86_set_eflags(c, x86_pop(c)); return;
    case 0x9E: { uint32_t ah = c->r[EAX] >> 8 & 0xFF; c->cf = ah & 1; c->pf = ah >> 2 & 1; c->af = ah >> 4 & 1; c->zf = ah >> 6 & 1; c->sf = ah >> 7 & 1; return; }
    case 0x9F: setr8(c, 4, (uint8_t)(x86_eflags(c) & 0xD5) | 2); return;
    case 0xA0: setr8(c, 0, rd8(c, fetch32(c) + d.seg)); return;
    case 0xA1: { uint32_t a = fetch32(c) + d.seg; reg_set(c, EAX, sz, sz == 16 ? rd16(c, a) : rd32(c, a)); return; }
    case 0xA2: wr8(c, fetch32(c) + d.seg, (uint8_t)c->r[EAX]); return;
    case 0xA3: { uint32_t a = fetch32(c) + d.seg; if (sz == 16) wr16(c, a, (uint16_t)c->r[EAX]); else wr32(c, a, c->r[EAX]); return; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &d, op);
        return;
    case 0xA8: alu(c, 4, c->r[EAX], fetch8(c), 8); return;
    case 0xA9: alu(c, 4, c->r[EAX], sz == 16 ? fetch16(c) : fetch32(c), sz); return;
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int s = (op & 1) ? sz : 8;
        modrm(c, &d);
        uint32_t cnt = op <= 0xC1 ? fetch8(c) : op <= 0xD1 ? 1 : c->r[ECX] & 0xFF;
        uint32_t v = rm_get(c, &d, s);
        uint32_t r = shift(c, d.reg, v, cnt, s);
        if (cnt & 31) rm_set(c, &d, s, r);
        return;
    }
    case 0xC2: { uint16_t n = fetch16(c); c->eip = x86_pop(c); c->r[ESP] += n; return; }
    case 0xC3: c->eip = x86_pop(c); return;
    case 0xC6: case 0xC7: {
        int s = (op & 1) ? sz : 8;
        modrm(c, &d);
        uint32_t v = s == 8 ? fetch8(c) : s == 16 ? fetch16(c) : fetch32(c);
        rm_set(c, &d, s, v);
        return;
    }
    case 0xC8: {  // enter imm16, 0
        uint16_t n = fetch16(c);
        uint8_t lvl = fetch8(c);
        if (lvl) x86_fault(c, "enter with nesting level at %#x", d.start);
        x86_push(c, c->r[EBP]);
        c->r[EBP] = c->r[ESP];
        c->r[ESP] -= n;
        return;
    }
    case 0xC9: c->r[ESP] = c->r[EBP]; c->r[EBP] = x86_pop(c); return;
    case 0xCC: x86_fault(c, "int3 at %#x", d.start);
    case 0xE3: { int8_t rel = (int8_t)fetch8(c); if (c->r[ECX] == 0) c->eip += (uint32_t)(int32_t)rel; return; }
    case 0xE0: case 0xE1: case 0xE2: {  // loopne / loope / loop
        int8_t rel = (int8_t)fetch8(c);
        c->r[ECX]--;
        int t = c->r[ECX] != 0 && (op == 0xE2 || (op == 0xE1 ? c->zf : !c->zf));
        if (t) c->eip += (uint32_t)(int32_t)rel;
        return;
    }
    case 0xE8: { int32_t rel = (int32_t)fetch32(c); x86_push(c, c->eip); c->eip += (uint32_t)rel; return; }
    case 0xE9: { int32_t rel = (int32_t)fetch32(c); c->eip += (uint32_t)rel; return; }
    case 0xEB: { int8_t rel = (int8_t)fetch8(c); c->eip += (uint32_t)(int32_t)rel; return; }
    case 0xF4: x86_fault(c, "hlt at %#x", d.start);
    case 0xF5: c->cf ^= 1; return;
    case 0xF6: case 0xF7: {
        int s = (op & 1) ? sz : 8;
        modrm(c, &d);
        uint32_t v = rm_get(c, &d, s), m = mask_of(s);
        switch (d.reg) {
        case 0: case 1: alu(c, 4, v, s == 8 ? fetch8(c) : s == 16 ? fetch16(c) : fetch32(c), s); return;
        case 2: rm_set(c, &d, s, ~v); return;
        case 3: {
            uint32_t r = alu(c, 5, 0, v, s);
            c->cf = (v & m) != 0;
            rm_set(c, &d, s, r);
            return;
        }
        case 4: case 5: {  // mul / imul (one operand)
            if (s == 8) {
                uint32_t r = d.reg == 4 ? (uint32_t)(uint8_t)c->r[EAX] * (uint8_t)v : (uint32_t)(uint16_t)((int16_t)(int8_t)c->r[EAX] * (int8_t)v);
                setr16(c, EAX, (uint16_t)r);
                c->cf = c->of = d.reg == 4 ? (r >> 8) != 0 : (int16_t)r != (int8_t)r;
                szp(c, r, 8);
            } else if (s == 16) {
                uint32_t r = d.reg == 4 ? (uint32_t)(uint16_t)c->r[EAX] * (uint16_t)v : (uint32_t)((int32_t)(int16_t)c->r[EAX] * (int16_t)v);
                setr16(c, EAX, (uint16_t)r); setr16(c, EDX, (uint16_t)(r >> 16));
                c->cf = c->of = d.reg == 4 ? (r >> 16) != 0 : (int32_t)r != (int16_t)r;
                szp(c, r, 16);
            } else {
                uint64_t r = d.reg == 4 ? (uint64_t)c->r[EAX] * v : (uint64_t)((int64_t)(int32_t)c->r[EAX] * (int32_t)v);
                c->r[EAX] = (uint32_t)r; c->r[EDX] = (uint32_t)(r >> 32);
                c->cf = c->of = d.reg == 4 ? c->r[EDX] != 0 : (int64_t)r != (int32_t)r;
                szp(c, (uint32_t)r, 32);
            }
            c->af = 0;
            return;
        }
        default: {  // div / idiv
            if ((v & m) == 0) {
                // MacinTalk 3 divides by zero above ~320 wpm (harmless on PowerPC,
                // where divw doesn't trap). Recover as panthera-speech's Windows
                // host does: retry with a divisor of 1 (written back if in memory).
                if (!c->div0_as_one) x86_fault(c, "divide by zero at %#x", d.start);
                v = 1;
                if (d.mod != 3) rm_set(c, &d, s, 1);
            }
            if (s == 32) {
                uint64_t n = (uint64_t)c->r[EDX] << 32 | c->r[EAX];
                if (d.reg == 6) {
                    uint64_t q = n / v;
                    if (q >> 32) x86_fault(c, "divide overflow at %#x", d.start);
                    c->r[EAX] = (uint32_t)q; c->r[EDX] = (uint32_t)(n % v);
                } else {
                    int64_t sn = (int64_t)n, sv = (int32_t)v;
                    if (sn == INT64_MIN && sv == -1) x86_fault(c, "divide overflow at %#x", d.start);
                    int64_t q = sn / sv;
                    if (q != (int32_t)q) x86_fault(c, "divide overflow at %#x", d.start);
                    c->r[EAX] = (uint32_t)q; c->r[EDX] = (uint32_t)(sn % sv);
                }
            } else if (s == 16) {
                uint32_t n = (c->r[EDX] & 0xFFFF) << 16 | (c->r[EAX] & 0xFFFF);
                if (d.reg == 6) { uint32_t q = n / (v & 0xFFFF); if (q >> 16) x86_fault(c, "divide overflow"); setr16(c, EAX, (uint16_t)q); setr16(c, EDX, (uint16_t)(n % (v & 0xFFFF))); }
                else { int32_t sn = (int32_t)n, sv = (int16_t)v; int32_t q = sn / sv; if (q != (int16_t)q) x86_fault(c, "divide overflow"); setr16(c, EAX, (uint16_t)q); setr16(c, EDX, (uint16_t)(sn % sv)); }
            } else {
                uint32_t n = c->r[EAX] & 0xFFFF;
                if (d.reg == 6) { uint32_t q = n / (v & 0xFF); if (q >> 8) x86_fault(c, "divide overflow"); setr8(c, 0, (uint8_t)q); setr8(c, 4, (uint8_t)(n % (v & 0xFF))); }
                else { int32_t sn = (int16_t)n, sv = (int8_t)v; int32_t q = sn / sv; if (q != (int8_t)q) x86_fault(c, "divide overflow"); setr8(c, 0, (uint8_t)q); setr8(c, 4, (uint8_t)(sn % sv)); }
            }
            return;
        }
        }
    }
    case 0xF8: c->cf = 0; return;
    case 0xF9: c->cf = 1; return;
    case 0xFC: c->df = 0; return;
    case 0xFD: c->df = 1; return;
    case 0xFE: case 0xFF: {
        int s = op == 0xFE ? 8 : sz;
        modrm(c, &d);
        switch (d.reg) {
        case 0: case 1: rm_set(c, &d, s, incdec(c, rm_get(c, &d, s), d.reg, s)); return;
        case 2: if (op == 0xFF) { uint32_t t = rm_get(c, &d, 32); x86_push(c, c->eip); c->eip = t; return; } break;
        case 4: if (op == 0xFF) { c->eip = rm_get(c, &d, 32); return; } break;
        case 6: if (op == 0xFF) { x86_push(c, rm_get(c, &d, sz)); return; } break;
        }
        break;
    }
    }
    c->eip = d.start;
    x86_fault(c, "unsupported opcode %02x %02x %02x at %#x", op, c->mem[d.start + 1], c->mem[d.start + 2], d.start);
}

void x86_step(X86 *c) {
    c->icount++;
    if (c->eip >= c->trap_lo && c->eip < c->trap_hi) {
        uint32_t at = c->eip;
        c->trap(c, at, c->trap_ctx);
        if (c->eip == at) c->eip = x86_pop(c);  // plain host function: return to caller
        return;
    }
    exec(c);
}

void x86_run(X86 *c) {
    uint32_t n = 0;
    while (c->eip != c->stop) {
        x86_step(c);
        if (!(++n & 4095) && c->abort && *c->abort) x86_fault(c, "engine shut down");
    }
}
