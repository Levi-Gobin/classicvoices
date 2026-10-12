// Thumb-2 instruction semantics shared by the interpreter (thumb.c) and the
// static recompiler output (build/gen/*.c). Operates on a compact Core so the
// recompiled code can keep it in a local that the compiler promotes to registers.
#pragma once
#include <stdint.h>
#include <string.h>

#ifndef VX_OPS_INLINE
#define VX_OPS_INLINE static inline
#endif

#ifndef GUEST_MEM_SIZE
#define GUEST_MEM_SIZE 0x40000000u  // 1 GB flat guest address space (mostly unbacked)
#endif

typedef struct Core {
    uint32_t r[16];
    uint32_t n, z, c, v;   // condition flags, each 0 or 1
    uint32_t it;           // ITSTATE (interpreter only)
    uint32_t npc;          // next PC for the executing instruction
    uint32_t thumb;
    uint8_t *mem;          // host pointer to guest address 0
    uint32_t fpscr;        // only NZCV (bits 31:28) are modelled
    uint64_t d[32];        // VFP registers; S(2n) / S(2n+1) are the low / high halves of D(n)
} Core;

__attribute__((noreturn, cold)) void vx_fault_mem(const char *kind, uint32_t addr, uint32_t pc);
__attribute__((noreturn, cold)) void vx_undef(uint32_t hw1, uint32_t hw2, uint32_t pc);

static inline uint32_t g32(const Core *c, uint32_t a) {
    if (__builtin_expect(a > GUEST_MEM_SIZE - 4, 0)) vx_fault_mem("read32", a, c->r[15]);
    uint32_t v; memcpy(&v, c->mem + a, 4); return v;
}
static inline uint32_t g16(const Core *c, uint32_t a) {
    if (__builtin_expect(a > GUEST_MEM_SIZE - 2, 0)) vx_fault_mem("read16", a, c->r[15]);
    uint16_t v; memcpy(&v, c->mem + a, 2); return v;
}
static inline uint32_t g8(const Core *c, uint32_t a) {
    if (__builtin_expect(a >= GUEST_MEM_SIZE, 0)) vx_fault_mem("read8", a, c->r[15]);
    return c->mem[a];
}
static inline void s32(Core *c, uint32_t a, uint32_t v) {
    if (__builtin_expect(a > GUEST_MEM_SIZE - 4 || a < 0x1000, 0)) vx_fault_mem("write32", a, c->r[15]);
    memcpy(c->mem + a, &v, 4);
}
static inline void s16(Core *c, uint32_t a, uint32_t v) {
    if (__builtin_expect(a > GUEST_MEM_SIZE - 2 || a < 0x1000, 0)) vx_fault_mem("write16", a, c->r[15]);
    uint16_t h = (uint16_t)v; memcpy(c->mem + a, &h, 2);
}
static inline void s8(Core *c, uint32_t a, uint32_t v) {
    if (__builtin_expect(a >= GUEST_MEM_SIZE || a < 0x1000, 0)) vx_fault_mem("write8", a, c->r[15]);
    c->mem[a] = (uint8_t)v;
}

// ---------------------------------------------------------------- helpers

static inline uint32_t ror32(uint32_t v, unsigned n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }

static inline int cond_pass(const Core *c, unsigned cond) {
    switch (cond) {
    case 0: return c->z;
    case 1: return !c->z;
    case 2: return c->c;
    case 3: return !c->c;
    case 4: return c->n;
    case 5: return !c->n;
    case 6: return c->v;
    case 7: return !c->v;
    case 8: return c->c && !c->z;
    case 9: return !c->c || c->z;
    case 10: return c->n == c->v;
    case 11: return c->n != c->v;
    case 12: return !c->z && c->n == c->v;
    case 13: return c->z || c->n != c->v;
    default: return 1;
    }
}

static inline uint32_t addc(Core *c, uint32_t x, uint32_t y, uint32_t cin, int s) {
    uint64_t u = (uint64_t)x + y + cin;
    uint32_t r = (uint32_t)u;
    if (s) {
        c->n = r >> 31;
        c->z = r == 0;
        c->c = (uint32_t)(u >> 32) & 1;
        c->v = ((~(x ^ y) & (x ^ r)) >> 31) & 1;
    }
    return r;
}

static inline void nz(Core *c, uint32_t r) { c->n = r >> 31; c->z = r == 0; }

// type: 0 LSL, 1 LSR, 2 ASR, 3 ROR, 4 RRX
static inline uint32_t shift_c(uint32_t v, unsigned type, unsigned amt, uint32_t cin, uint32_t *cout) {
    if (type == 4) { *cout = v & 1; return (cin << 31) | (v >> 1); }
    if (amt == 0) { *cout = cin; return v; }
    switch (type) {
    case 0:
        if (amt < 32) { *cout = (v >> (32 - amt)) & 1; return v << amt; }
        *cout = amt == 32 ? (v & 1) : 0; return 0;
    case 1:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return v >> amt; }
        *cout = amt == 32 ? (v >> 31) : 0; return 0;
    case 2:
        if (amt < 32) { *cout = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *cout = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default: {
        uint32_t r = ror32(v, amt);
        *cout = r >> 31; return r;
    }
    }
}

// DecodeImmShift
static inline void decode_imm_shift(unsigned type, unsigned imm5, unsigned *t, unsigned *amt) {
    switch (type) {
    case 0: *t = 0; *amt = imm5; break;
    case 1: *t = 1; *amt = imm5 ? imm5 : 32; break;
    case 2: *t = 2; *amt = imm5 ? imm5 : 32; break;
    default:
        if (imm5 == 0) { *t = 4; *amt = 1; } else { *t = 3; *amt = imm5; }
    }
}

static inline uint32_t thumb_expand_imm_c(uint32_t imm12, uint32_t cin, uint32_t *cout) {
    if ((imm12 >> 10) == 0) {
        uint32_t b = imm12 & 0xff;
        *cout = cin;
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return (b << 16) | b;
        case 2: return (b << 24) | (b << 8);
        default: return b * 0x01010101u;
        }
    }
    uint32_t r = ror32(0x80 | (imm12 & 0x7f), imm12 >> 7);
    *cout = r >> 31;
    return r;
}

static inline uint32_t sext(uint32_t v, unsigned bits) {
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

// PC handling: while executing, r[15] = instruction address + 4 and c->npc
// holds the next PC. Writing r15 goes through these.

static inline void branch_to(Core *c, uint32_t a) { (void)c; c->npc = a & ~1u; }
static inline void bx_to(Core *c, uint32_t a) {
    if (a & 1) { c->thumb = 1; c->npc = a & ~1u; }
    else { c->thumb = 0; c->npc = a & ~3u; }
}
static inline void wreg(Core *c, unsigned d, uint32_t v) {
    if (d == 15) branch_to(c, v); else c->r[d] = v;
}
static inline void wreg_load(Core *c, unsigned d, uint32_t v) {
    if (d == 15) bx_to(c, v); else c->r[d] = v;
}

#define UNDEF() vx_undef(hw1, hw2, pc)

// Data-processing core shared by modified-immediate and shifted-register forms.
static inline void dp_op(Core *c, unsigned op, unsigned s, unsigned rn, unsigned rd,
                         uint32_t a, uint32_t b, uint32_t carry, uint32_t hw1, uint32_t hw2, uint32_t pc) {
    uint32_t r;
    switch (op) {
    case 0x0:  // AND / TST
        r = a & b;
        if (s) { nz(c, r); c->c = carry; }
        if (rd != 15) wreg(c, rd, r);
        else if (!s) UNDEF();
        return;
    case 0x1: r = a & ~b; break;
    case 0x2: r = (rn == 15) ? b : (a | b); break;    // ORR / MOV
    case 0x3: r = (rn == 15) ? ~b : (a | ~b); break;  // ORN / MVN
    case 0x4:  // EOR / TEQ
        r = a ^ b;
        if (s) { nz(c, r); c->c = carry; }
        if (rd != 15) wreg(c, rd, r);
        else if (!s) UNDEF();
        return;
    case 0x8:  // ADD / CMN
        r = addc(c, a, b, 0, s);
        if (rd != 15) wreg(c, rd, r);
        else if (!s) UNDEF();
        return;
    case 0xA: wreg(c, rd, addc(c, a, b, c->c, s)); return;
    case 0xB: wreg(c, rd, addc(c, a, ~b, c->c, s)); return;
    case 0xD:  // SUB / CMP
        r = addc(c, a, ~b, 1, s);
        if (rd != 15) wreg(c, rd, r);
        else if (!s) UNDEF();
        return;
    case 0xE: wreg(c, rd, addc(c, b, ~a, 1, s)); return;
    default: UNDEF();
    }
    if (s) { nz(c, r); c->c = carry; }
    wreg(c, rd, r);
}


// ---------------------------------------------------------------- 16-bit

VX_OPS_INLINE void exec16(Core *c, uint32_t hw1, uint32_t pc, int in_it) {
    const uint32_t hw2 = 0;
    const int setf = !in_it;
    uint32_t cout;
    switch (hw1 >> 10) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: {
        unsigned op = hw1 >> 11;
        unsigned rd = hw1 & 7, rm = (hw1 >> 3) & 7, imm5 = (hw1 >> 6) & 31;
        if (op <= 2) {  // LSL/LSR/ASR imm
            unsigned t, amt;
            decode_imm_shift(op, imm5, &t, &amt);
            uint32_t r = shift_c(c->r[rm], t, amt, c->c, &cout);
            c->r[rd] = r;
            if (setf) { nz(c, r); c->c = cout; }
        } else {  // add/sub reg/imm3
            unsigned rn = (hw1 >> 3) & 7;
            uint32_t b = (hw1 & 0x400) ? ((hw1 >> 6) & 7) : c->r[(hw1 >> 6) & 7];
            c->r[rd] = (hw1 & 0x200) ? addc(c, c->r[rn], ~b, 1, setf) : addc(c, c->r[rn], b, 0, setf);
        }
        return;
    }
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: {
        unsigned rdn = (hw1 >> 8) & 7, imm8 = hw1 & 0xff;
        switch ((hw1 >> 11) & 3) {
        case 0: c->r[rdn] = imm8; if (setf) nz(c, imm8); break;
        case 1: addc(c, c->r[rdn], ~(uint32_t)imm8, 1, 1); break;
        case 2: c->r[rdn] = addc(c, c->r[rdn], imm8, 0, setf); break;
        case 3: c->r[rdn] = addc(c, c->r[rdn], ~(uint32_t)imm8, 1, setf); break;
        }
        return;
    }
    case 0x10: {  // data processing
        unsigned rdn = hw1 & 7, rm = (hw1 >> 3) & 7;
        uint32_t a = c->r[rdn], b = c->r[rm], r;
        switch ((hw1 >> 6) & 15) {
        case 0x0: r = a & b; c->r[rdn] = r; if (setf) nz(c, r); return;
        case 0x1: r = a ^ b; c->r[rdn] = r; if (setf) nz(c, r); return;
        case 0x2: r = shift_c(a, 0, b & 0xff, c->c, &cout); c->r[rdn] = r; if (setf) { nz(c, r); c->c = cout; } return;
        case 0x3: r = shift_c(a, 1, b & 0xff, c->c, &cout); c->r[rdn] = r; if (setf) { nz(c, r); c->c = cout; } return;
        case 0x4: r = shift_c(a, 2, b & 0xff, c->c, &cout); c->r[rdn] = r; if (setf) { nz(c, r); c->c = cout; } return;
        case 0x5: c->r[rdn] = addc(c, a, b, c->c, setf); return;
        case 0x6: c->r[rdn] = addc(c, a, ~b, c->c, setf); return;
        case 0x7: r = shift_c(a, 3, b & 0xff, c->c, &cout); c->r[rdn] = r; if (setf) { nz(c, r); c->c = cout; } return;
        case 0x8: nz(c, a & b); return;
        case 0x9: c->r[rdn] = addc(c, ~b, 0, 1, setf); return;  // RSB #0 (NEG)
        case 0xA: addc(c, a, ~b, 1, 1); return;
        case 0xB: addc(c, a, b, 0, 1); return;
        case 0xC: r = a | b; c->r[rdn] = r; if (setf) nz(c, r); return;
        case 0xD: r = a * b; c->r[rdn] = r; if (setf) nz(c, r); return;
        case 0xE: r = a & ~b; c->r[rdn] = r; if (setf) nz(c, r); return;
        case 0xF: r = ~b; c->r[rdn] = r; if (setf) nz(c, r); return;
        }
        return;
    }
    case 0x11: {  // special data / BX
        unsigned rd = ((hw1 >> 4) & 8) | (hw1 & 7), rm = (hw1 >> 3) & 15;
        switch ((hw1 >> 8) & 3) {
        case 0: wreg(c, rd, c->r[rd] + c->r[rm]); return;
        case 1: addc(c, c->r[rd], ~c->r[rm], 1, 1); return;
        case 2: wreg(c, rd, c->r[rm]); return;
        case 3:
            if (hw1 & 0x80) {  // BLX reg
                uint32_t t = c->r[rm];
                c->r[14] = (pc + 2) | 1;
                bx_to(c, t);
            } else {
                bx_to(c, c->r[rm]);
            }
            return;
        }
        return;
    }
    case 0x12: case 0x13: {  // LDR literal
        uint32_t a = (c->r[15] & ~3u) + ((hw1 & 0xff) << 2);
        c->r[(hw1 >> 8) & 7] = g32(c, a);
        return;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {  // load/store reg offset
        unsigned rt = hw1 & 7;
        uint32_t a = c->r[(hw1 >> 3) & 7] + c->r[(hw1 >> 6) & 7];
        switch ((hw1 >> 9) & 7) {
        case 0: s32(c, a, c->r[rt]); return;
        case 1: s16(c, a, c->r[rt]); return;
        case 2: s8(c, a, c->r[rt]); return;
        case 3: c->r[rt] = sext(g8(c, a), 8); return;
        case 4: c->r[rt] = g32(c, a); return;
        case 5: c->r[rt] = g16(c, a); return;
        case 6: c->r[rt] = g8(c, a); return;
        case 7: c->r[rt] = sext(g16(c, a), 16); return;
        }
        return;
    }
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
        unsigned rt = hw1 & 7, imm5 = (hw1 >> 6) & 31;
        uint32_t base = c->r[(hw1 >> 3) & 7];
        switch ((hw1 >> 11) & 3) {
        case 0: s32(c, base + imm5 * 4, c->r[rt]); return;
        case 1: c->r[rt] = g32(c, base + imm5 * 4); return;
        case 2: s8(c, base + imm5, c->r[rt]); return;
        case 3: c->r[rt] = g8(c, base + imm5); return;
        }
        return;
    }
    case 0x20: case 0x21: case 0x22: case 0x23: {  // STRH/LDRH imm
        unsigned rt = hw1 & 7;
        uint32_t a = c->r[(hw1 >> 3) & 7] + ((hw1 >> 6) & 31) * 2;
        if (hw1 & 0x800) c->r[rt] = g16(c, a); else s16(c, a, c->r[rt]);
        return;
    }
    case 0x24: case 0x25: case 0x26: case 0x27: {  // SP-relative
        unsigned rt = (hw1 >> 8) & 7;
        uint32_t a = c->r[13] + (hw1 & 0xff) * 4;
        if (hw1 & 0x800) c->r[rt] = g32(c, a); else s32(c, a, c->r[rt]);
        return;
    }
    case 0x28: case 0x29:  // ADR
        c->r[(hw1 >> 8) & 7] = (c->r[15] & ~3u) + (hw1 & 0xff) * 4;
        return;
    case 0x2A: case 0x2B:  // ADD Rd, SP, imm
        c->r[(hw1 >> 8) & 7] = c->r[13] + (hw1 & 0xff) * 4;
        return;
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: {  // misc
        unsigned op = (hw1 >> 5) & 0x7f;
        if ((op & 0x7c) == 0x00) { c->r[13] += (hw1 & 0x7f) * 4; return; }
        if ((op & 0x7c) == 0x04) { c->r[13] -= (hw1 & 0x7f) * 4; return; }
        if ((hw1 & 0x0500) == 0x0100) {  // CBZ/CBNZ
            uint32_t v = c->r[hw1 & 7];
            uint32_t off = (((hw1 >> 9) & 1) << 6) | (((hw1 >> 3) & 31) << 1);
            if ((v == 0) != !!(hw1 & 0x800)) branch_to(c, c->r[15] + off);
            return;
        }
        if ((op & 0x78) == 0x10) {  // extend
            uint32_t m = c->r[(hw1 >> 3) & 7], r;
            switch ((hw1 >> 6) & 3) {
            case 0: r = sext(m & 0xffff, 16); break;
            case 1: r = sext(m & 0xff, 8); break;
            case 2: r = m & 0xffff; break;
            default: r = m & 0xff; break;
            }
            c->r[hw1 & 7] = r;
            return;
        }
        if ((op & 0x70) == 0x20) {  // PUSH
            uint32_t list = (hw1 & 0xff) | ((hw1 & 0x100) ? 0x4000 : 0);
            uint32_t a = c->r[13] - 4 * __builtin_popcount(list);
            c->r[13] = a;
            for (int i = 0; i < 15; i++)
                if (list & (1u << i)) { s32(c, a, c->r[i]); a += 4; }
            return;
        }
        if ((op & 0x70) == 0x60) {  // POP
            uint32_t list = (hw1 & 0xff) | ((hw1 & 0x100) ? 0x8000 : 0);
            uint32_t a = c->r[13];
            c->r[13] = a + 4 * __builtin_popcount(list);
            for (int i = 0; i < 16; i++)
                if (list & (1u << i)) { wreg_load(c, i, g32(c, a)); a += 4; }
            return;
        }
        if ((op & 0x78) == 0x50) {  // REV family
            uint32_t m = c->r[(hw1 >> 3) & 7], r;
            switch ((hw1 >> 6) & 3) {
            case 0: r = __builtin_bswap32(m); break;
            case 1: r = ((m & 0x00ff00ff) << 8) | ((m >> 8) & 0x00ff00ff); break;
            case 3: r = sext(((m & 0xff) << 8) | ((m >> 8) & 0xff), 16); break;
            default: UNDEF();
            }
            c->r[hw1 & 7] = r;
            return;
        }
        if ((op & 0x78) == 0x78) {  // IT / hints
            if (hw1 & 0xf) c->it = hw1 & 0xff;
            return;
        }
        if ((op & 0x78) == 0x70) vx_undef(hw1, 0xBE00, pc);
        UNDEF();
    }
    case 0x30: case 0x31: {  // STMIA
        unsigned rn = (hw1 >> 8) & 7;
        uint32_t a = c->r[rn];
        for (int i = 0; i < 8; i++)
            if (hw1 & (1u << i)) { s32(c, a, c->r[i]); a += 4; }
        c->r[rn] = a;
        return;
    }
    case 0x32: case 0x33: {  // LDMIA
        unsigned rn = (hw1 >> 8) & 7;
        uint32_t a = c->r[rn];
        for (int i = 0; i < 8; i++)
            if (hw1 & (1u << i)) { c->r[i] = g32(c, a); a += 4; }
        if (!(hw1 & (1u << rn))) c->r[rn] = a;
        return;
    }
    case 0x34: case 0x35: case 0x36: case 0x37: {  // B cond / SVC
        unsigned cond = (hw1 >> 8) & 15;
        if (cond >= 14) UNDEF();
        if (cond_pass(c, cond)) branch_to(c, c->r[15] + sext((hw1 & 0xff) << 1, 9));
        return;
    }
    case 0x38: case 0x39:
        branch_to(c, c->r[15] + sext((hw1 & 0x7ff) << 1, 12));
        return;
    default:
        UNDEF();
    }
}


// ---------------------------------------------------------------- VFP (scalar)

static inline float vs_get(const Core *c, unsigned i) {
    uint32_t u = (uint32_t)(c->d[i >> 1] >> ((i & 1) * 32)); float f; memcpy(&f, &u, 4); return f;
}
static inline void vs_set(Core *c, unsigned i, float f) {
    uint32_t u; memcpy(&u, &f, 4);
    unsigned sh = (i & 1) * 32;
    c->d[i >> 1] = (c->d[i >> 1] & ~((uint64_t)0xFFFFFFFFu << sh)) | ((uint64_t)u << sh);
}
static inline uint32_t vs_raw(const Core *c, unsigned i) { return (uint32_t)(c->d[i >> 1] >> ((i & 1) * 32)); }
static inline void vs_setraw(Core *c, unsigned i, uint32_t u) {
    unsigned sh = (i & 1) * 32;
    c->d[i >> 1] = (c->d[i >> 1] & ~((uint64_t)0xFFFFFFFFu << sh)) | ((uint64_t)u << sh);
}
static inline double vd_get(const Core *c, unsigned i) { double d; memcpy(&d, &c->d[i], 8); return d; }
static inline void vd_set(Core *c, unsigned i, double d) { memcpy(&c->d[i], &d, 8); }

static inline void vfp_compare(Core *c, double a, double b) {
    uint32_t f;
    if (a != a || b != b) f = 0x3;      // unordered: C V
    else if (a == b) f = 0x6;           // Z C
    else if (a < b) f = 0x8;            // N
    else f = 0x2;                       // C
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (f << 28);
}

static inline int32_t sat_s32(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return 2147483647;
    if (v <= -2147483648.0) return (int32_t)0x80000000u;
    return (int32_t)v;
}
static inline uint32_t sat_u32(double v) {
    if (v != v || v <= 0) return 0;
    if (v >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)v;
}
static inline double round_even(double v) { return __builtin_rint(v); }

// w = hw1:hw2 (same layout as the ARM encoding with cond = 1110)
static inline void exec_vfp(Core *c, uint32_t hw1, uint32_t hw2, uint32_t pc) {
#pragma clang fp contract(off)  // VFP VMLA/VMLS round the product, then the sum
    uint32_t w = (hw1 << 16) | hw2;
    unsigned coproc = (w >> 8) & 0xF;
    if (coproc != 10 && coproc != 11) UNDEF();
    int dbl = coproc == 11;
    unsigned Vd = (w >> 12) & 15, Vn = (w >> 16) & 15, Vm = w & 15;
    unsigned D = (w >> 22) & 1, N = (w >> 7) & 1, M = (w >> 5) & 1;
    unsigned sd = (Vd << 1) | D, sn = (Vn << 1) | N, sm = (Vm << 1) | M;  // single regs
    unsigned dd = (D << 4) | Vd, dn = (N << 4) | Vn, dm = (M << 4) | Vm;  // double regs

    if ((w & 0x0E000000u) == 0x0C000000u) {  // ---- load/store, 64-bit transfers
        unsigned op = (w >> 20) & 0x1F, rn = (w >> 16) & 15;
        if ((op & 0x1E) == 0x04) {  // VMOV two core regs <-> two singles / one double
            unsigned rt = (w >> 12) & 15, rt2 = (w >> 16) & 15;
            if (dbl) {
                if (op & 1) { c->r[rt] = (uint32_t)c->d[dm]; c->r[rt2] = (uint32_t)(c->d[dm] >> 32); }
                else c->d[dm] = ((uint64_t)c->r[rt2] << 32) | c->r[rt];
            } else {
                if (op & 1) { c->r[rt] = vs_raw(c, sm); c->r[rt2] = vs_raw(c, sm + 1); }
                else { vs_setraw(c, sm, c->r[rt]); vs_setraw(c, sm + 1, c->r[rt2]); }
            }
            return;
        }
        unsigned P = (op >> 4) & 1, U = (op >> 3) & 1, W = (op >> 1) & 1, L = op & 1;
        uint32_t imm8 = w & 0xFF;
        uint32_t base = rn == 15 ? (c->r[15] & ~3u) : c->r[rn];
        if (P && !W) {  // VLDR / VSTR
            uint32_t a = U ? base + imm8 * 4 : base - imm8 * 4;
            if (dbl) {
                if (L) c->d[dd] = (uint64_t)g32(c, a) | ((uint64_t)g32(c, a + 4) << 32);
                else { s32(c, a, (uint32_t)c->d[dd]); s32(c, a + 4, (uint32_t)(c->d[dd] >> 32)); }
            } else {
                if (L) vs_setraw(c, sd, g32(c, a)); else s32(c, a, vs_raw(c, sd));
            }
            return;
        }
        // VLDM / VSTM (IA, or DB with writeback; VPUSH/VPOP are the SP forms)
        if (P == U) UNDEF();
        uint32_t a = U ? base : base - imm8 * 4;
        if (dbl) {
            unsigned n = imm8 / 2;
            for (unsigned i = 0; i < n; i++, a += 8) {
                if (L) c->d[(dd + i) & 31] = (uint64_t)g32(c, a) | ((uint64_t)g32(c, a + 4) << 32);
                else { s32(c, a, (uint32_t)c->d[(dd + i) & 31]); s32(c, a + 4, (uint32_t)(c->d[(dd + i) & 31] >> 32)); }
            }
        } else {
            for (unsigned i = 0; i < imm8; i++, a += 4) {
                if (L) vs_setraw(c, (sd + i) & 63, g32(c, a)); else s32(c, a, vs_raw(c, (sd + i) & 63));
            }
        }
        if (W) c->r[rn] = U ? base + imm8 * 4 : base - imm8 * 4;
        return;
    }
    if ((w & 0x0F000000u) != 0x0E000000u) UNDEF();
    if (w & 0x10) {  // ---- register transfers
        unsigned L = (w >> 20) & 1, A = (w >> 21) & 7, rt = (w >> 12) & 15;
        if (!dbl && A == 0) {  // VMOV Sn <-> Rt
            if (L) c->r[rt] = vs_raw(c, sn); else vs_setraw(c, sn, c->r[rt]);
            return;
        }
        if (!dbl && A == 7) {  // VMRS / VMSR
            if (L) {
                if (rt == 15) { c->n = c->fpscr >> 31; c->z = (c->fpscr >> 30) & 1; c->c = (c->fpscr >> 29) & 1; c->v = (c->fpscr >> 28) & 1; }
                else c->r[rt] = c->fpscr;
            } else {
                c->fpscr = c->r[rt];
            }
            return;
        }
        if (dbl && (A & 4) == 0 && ((w >> 5) & 3) == 0) {  // VMOV Dd[x] <-> Rt (32-bit scalar)
            unsigned x = (w >> 21) & 1, reg = ((w >> 7) & 1) << 4 | Vn;
            if (L) c->r[rt] = (uint32_t)(c->d[reg] >> (32 * x));
            else c->d[reg] = (c->d[reg] & ~((uint64_t)0xFFFFFFFFu << (32 * x))) | ((uint64_t)c->r[rt] << (32 * x));
            return;
        }
        UNDEF();
    }
    // ---- data processing
    unsigned opc1 = ((w >> 20) & 0xB), opc2 = (w >> 16) & 15, op6 = (w >> 6) & 1;
    if (opc1 == 0xB) {
        if (!op6) {  // VMOV immediate: VFPExpandImm(imm8), imm8 = bits 19:16 : 3:0
            uint32_t imm8 = ((w >> 16) & 0xF) << 4 | (w & 0xF);
            unsigned sign = imm8 >> 7, b6 = (imm8 >> 6) & 1, rest = imm8 & 0x3F;
            if (dbl) {  // exponent = NOT(b6) : b6 x8 : imm[5:4]
                uint64_t exp = ((uint64_t)!b6 << 10) | (b6 ? 0x3FCull : 0) | ((rest >> 4) & 3);
                c->d[dd] = ((uint64_t)sign << 63) | (exp << 52) | ((uint64_t)(rest & 0xF) << 48);
            } else {    // exponent = NOT(b6) : b6 x5 : imm[5:4]
                uint32_t exp = ((uint32_t)!b6 << 7) | (b6 ? 0x7Cu : 0) | ((rest >> 4) & 3);
                vs_setraw(c, sd, (sign << 31) | (exp << 23) | ((rest & 0xF) << 19));
            }
            return;
        }
        switch (opc2) {
        case 0x0:
            if (((w >> 7) & 1) == 0) { if (dbl) c->d[dd] = c->d[dm]; else vs_setraw(c, sd, vs_raw(c, sm)); }  // VMOV reg
            else { if (dbl) c->d[dd] = c->d[dm] & ~(1ull << 63); else vs_setraw(c, sd, vs_raw(c, sm) & 0x7FFFFFFFu); }  // VABS
            return;
        case 0x1:
            if (((w >> 7) & 1) == 0) { if (dbl) c->d[dd] = c->d[dm] ^ (1ull << 63); else vs_setraw(c, sd, vs_raw(c, sm) ^ 0x80000000u); }  // VNEG
            else { if (dbl) vd_set(c, dd, __builtin_sqrt(vd_get(c, dm))); else vs_set(c, sd, __builtin_sqrtf(vs_get(c, sm))); }  // VSQRT
            return;
        case 0x4: case 0x5: {  // VCMP(E) with register / zero
            double a = dbl ? vd_get(c, dd) : vs_get(c, sd);
            double b = opc2 == 5 ? 0.0 : (dbl ? vd_get(c, dm) : vs_get(c, sm));
            vfp_compare(c, a, b);
            return;
        }
        case 0x7:  // VCVT between double and single
            if (dbl) vs_set(c, sd, (float)vd_get(c, dm));
            else vd_set(c, dd, (double)vs_get(c, sm));
            return;
        case 0x8: {  // VCVT int -> float (source is a single register)
            uint32_t u = vs_raw(c, sm);
            double v = ((w >> 7) & 1) ? (double)(int32_t)u : (double)u;
            if (dbl) vd_set(c, dd, v); else vs_set(c, sd, (float)v);
            return;
        }
        case 0xC: case 0xD: {  // VCVT float -> int (result in a single register)
            double v = dbl ? vd_get(c, dm) : vs_get(c, sm);
            if (!((w >> 7) & 1)) v = round_even(v);  // FPSCR rounding (round to nearest)
            vs_setraw(c, sd, opc2 == 0xD ? (uint32_t)sat_s32(v) : sat_u32(v));
            return;
        }
        default: UNDEF();
        }
    }
    if (dbl) {
        double a = vd_get(c, dn), b = vd_get(c, dm), d = vd_get(c, dd), r;
        switch (opc1) {
        case 0x0: r = op6 ? d - a * b : d + a * b; break;          // VMLA / VMLS
        case 0x1: r = op6 ? -d - a * b : -d + a * b; break;        // VNMLA / VNMLS
        case 0x2: r = op6 ? -(a * b) : a * b; break;               // VNMUL / VMUL
        case 0x3: r = op6 ? a - b : a + b; break;                  // VSUB / VADD
        case 0x8: if (op6) UNDEF(); r = a / b; break;              // VDIV
        default: UNDEF();
        }
        vd_set(c, dd, r);
    } else {
        float a = vs_get(c, sn), b = vs_get(c, sm), d = vs_get(c, sd), r;
        switch (opc1) {
        case 0x0: { float p = a * b; r = op6 ? d - p : d + p; break; }
        case 0x1: { float p = a * b; r = op6 ? -d - p : -d + p; break; }
        case 0x2: r = op6 ? -(a * b) : a * b; break;
        case 0x3: r = op6 ? a - b : a + b; break;
        case 0x8: if (op6) UNDEF(); r = a / b; break;
        default: UNDEF();
        }
        vs_set(c, sd, r);
    }
}

// ---------------------------------------------------------------- NEON (iOS 7 subset)
// iOS 7's compiler uses NEON only for block copies / zeroing: VLD1/VST1
// (multiple elements), VMOV immediate and VORR (register move).

static inline uint64_t neon_expand_imm(unsigned op, unsigned cmode, uint32_t imm8) {
    uint64_t r;
    switch (cmode >> 1) {
    case 0: r = imm8; r |= r << 32; break;
    case 1: r = imm8 << 8; r |= r << 32; break;
    case 2: r = imm8 << 16; r |= r << 32; break;
    case 3: r = (uint64_t)imm8 << 24; r |= r << 32; break;
    case 4: r = imm8 * 0x0001000100010001ull; break;
    case 5: r = (imm8 << 8) * 0x0001000100010001ull; break;
    case 6:
        r = (cmode & 1) ? ((imm8 << 16) | 0xFFFF) : ((imm8 << 8) | 0xFF);
        r |= r << 32; break;
    default:
        if (!(cmode & 1) && !op) { r = imm8 * 0x0101010101010101ull; break; }
        if (!(cmode & 1) && op) {
            r = 0;
            for (int i = 0; i < 8; i++) if (imm8 & (1u << i)) r |= 0xFFull << (8 * i);
            return r;
        }
        if (!op) {  // f32
            uint32_t f = ((imm8 & 0x80u) << 24) | ((imm8 & 0x40u) ? 0x3E000000u : 0x40000000u) | ((imm8 & 0x3Fu) << 19);
            r = f | ((uint64_t)f << 32); break;
        }
        r = 0; break;  // undefined
    }
    // op=1 with cmode < 14 is VMVN
    if (op && cmode < 14) r = ~r;
    return r;
}

// NEON arithmetic is always flush-to-zero with the default NaN.
static inline uint32_t neon_ftz(uint32_t u) { return (u & 0x7F800000u) ? u : (u & 0x80000000u); }
static inline uint64_t neon_f32x2(uint64_t a, uint64_t b, int kind) {
    uint64_t out = 0;
    for (int l = 0; l < 2; l++) {
        uint32_t ua = neon_ftz((uint32_t)(a >> (32 * l))), ub = neon_ftz((uint32_t)(b >> (32 * l))), uo;
        float fa, fb, fo;
        memcpy(&fa, &ua, 4); memcpy(&fb, &ub, 4);
        fo = kind == 0 ? fa + fb : kind == 1 ? fa - fb : fa * fb;
        memcpy(&uo, &fo, 4);
        if (fo != fo) uo = 0x7FC00000u;
        out |= (uint64_t)neon_ftz(uo) << (32 * l);
    }
    return out;
}

static inline void exec_neon_dp(Core *c, uint32_t hw1, uint32_t hw2, uint32_t pc) {
    unsigned D = (hw1 >> 6) & 1, Q = (hw2 >> 6) & 1;
    unsigned vd = (D << 4) | (hw2 >> 12);
    if ((hw1 & 0xEFB8) == 0xEF80 && (hw2 & 0x0090) == 0x0010) {  // one register + modified immediate
        unsigned cmode = (hw2 >> 8) & 15, op = (hw2 >> 5) & 1;
        uint32_t imm8 = ((hw1 >> 12 & 1) << 7) | ((hw1 & 7) << 4) | (hw2 & 15);
        if (op && cmode == 15) UNDEF();
        uint64_t v = neon_expand_imm(op, cmode, imm8);
        if (((cmode & 9) == 1 || (cmode & 13) == 9) && cmode < 12) {  // VORR/VBIC immediate
            for (unsigned i = 0; i <= Q; i++) c->d[(vd + i) & 31] = op ? (c->d[(vd + i) & 31] & v) : (c->d[(vd + i) & 31] | v);
            return;
        }
        c->d[vd] = v;
        if (Q) c->d[(vd + 1) & 31] = v;
        return;
    }
    if ((hw1 & 0xFFB0) == 0xEF20 && (hw2 & 0x0F10) == 0x0110) {  // VORR (register)
        unsigned N = (hw2 >> 7) & 1, M = (hw2 >> 5) & 1;
        unsigned vn = (N << 4) | (hw1 & 15), vm = (M << 4) | (hw2 & 15);
        uint64_t a0 = c->d[vn] | c->d[vm], a1 = Q ? (c->d[(vn + 1) & 31] | c->d[(vm + 1) & 31]) : 0;
        c->d[vd] = a0;
        if (Q) c->d[(vd + 1) & 31] = a1;
        return;
    }
    if ((hw1 & 0xEFA0) == 0xEF00 && (hw2 & 0x0F00) == 0x0D00 && !(hw1 & 0x10)) {  // VADD/VSUB/VMUL .F32
        unsigned N = (hw2 >> 7) & 1, M = (hw2 >> 5) & 1, U = (hw1 >> 12) & 1, op = (hw1 >> 5) & 1, b4 = (hw2 >> 4) & 1;
        unsigned vn = (N << 4) | (hw1 & 15), vm = (M << 4) | (hw2 & 15);
        int kind = !U && !b4 ? (op ? 1 : 0) : (U && b4 && !op ? 2 : -1);  // 0 add, 1 sub, 2 mul
        if (kind < 0) UNDEF();
        uint64_t res[2];
        for (unsigned r = 0; r <= Q; r++)
            res[r] = neon_f32x2(c->d[(vn + r) & 31], c->d[(vm + r) & 31], kind);
        for (unsigned r = 0; r <= Q; r++) c->d[(vd + r) & 31] = res[r];
        return;
    }
    if ((hw1 & 0xEFBF) == 0xEFBB && (hw2 & 0x0E10) == 0x0600) {  // VCVT between F32 and S32/U32
        unsigned M = (hw2 >> 5) & 1, op = (hw2 >> 7) & 3, vm = (M << 4) | (hw2 & 15);
        uint64_t res[2];
        for (unsigned r = 0; r <= Q; r++) {
            uint64_t in = c->d[(vm + r) & 31], out = 0;
            for (int l = 0; l < 2; l++) {
                uint32_t u = (uint32_t)(in >> (32 * l)), o;
                if (op == 0) { float f = (float)(int32_t)u; memcpy(&o, &f, 4); }
                else if (op == 1) { float f = (float)u; memcpy(&o, &f, 4); }
                else {
                    float f; memcpy(&f, &u, 4);
                    if ((u & 0x7F800000u) == 0) f = 0;  // flush denormal
                    if (f != f) o = 0;
                    else if (op == 2) o = f >= 2147483648.0f ? 0x7FFFFFFFu : f <= -2147483648.0f ? 0x80000000u : (uint32_t)(int32_t)f;
                    else o = f >= 4294967296.0f ? 0xFFFFFFFFu : f <= 0 ? 0 : (uint32_t)f;
                }
                out |= (uint64_t)o << (32 * l);
            }
            res[r] = out;
        }
        for (unsigned r = 0; r <= Q; r++) c->d[(vd + r) & 31] = res[r];
        return;
    }
    UNDEF();
}

static inline void exec_neon_ldst(Core *c, uint32_t hw1, uint32_t hw2, uint32_t pc) {
    unsigned D = (hw1 >> 6) & 1, L = (hw1 >> 5) & 1, rn = hw1 & 15, rm = hw2 & 15;
    unsigned vd = (D << 4) | (hw2 >> 12), type = (hw2 >> 8) & 15;
    if (hw1 & 0x80) UNDEF();  // single-element forms not used
    unsigned n;
    switch (type) {
    case 7: n = 1; break;
    case 10: n = 2; break;
    case 6: n = 3; break;
    case 2: n = 4; break;
    default: UNDEF(); n = 0;
    }
    uint32_t a = c->r[rn];
    for (unsigned i = 0; i < n; i++, a += 8) {  // little-endian: element size doesn't change the bytes
        uint64_t *r = &c->d[(vd + i) & 31];
        if (L) *r = (uint64_t)g32(c, a) | ((uint64_t)g32(c, a + 4) << 32);
        else { s32(c, a, (uint32_t)*r); s32(c, a + 4, (uint32_t)(*r >> 32)); }
    }
    if (rm == 13) c->r[rn] += 8 * n;
    else if (rm != 15) c->r[rn] += c->r[rm];
}

// ---------------------------------------------------------------- 32-bit

VX_OPS_INLINE void exec32(Core *c, uint32_t hw1, uint32_t hw2, uint32_t pc) {
    uint32_t cout;
    if ((hw1 & 0xFF10) == 0xF900) { exec_neon_ldst(c, hw1, hw2, pc); return; }
    if ((hw1 & 0xEF00) == 0xEF00) { exec_neon_dp(c, hw1, hw2, pc); return; }
    if ((hw1 & 0xEC00) == 0xEC00 && (hw1 & 0xFF00) != 0xFF00 && (hw1 & 0xFF00) != 0xEF00) {
        exec_vfp(c, hw1, hw2, pc);
        return;
    }
    // ---- load/store multiple
    if ((hw1 & 0xFE40) == 0xE800) {
        unsigned op = (hw1 >> 7) & 3, w = (hw1 >> 5) & 1, l = (hw1 >> 4) & 1, rn = hw1 & 15;
        uint32_t list = hw2, n = __builtin_popcount(list);
        uint32_t a;
        if (op == 1) a = c->r[rn];
        else if (op == 2) a = c->r[rn] - 4 * n;
        else UNDEF();
        uint32_t wb = (op == 1) ? c->r[rn] + 4 * n : c->r[rn] - 4 * n;
        if (l) {
            if (w && !(list & (1u << rn))) c->r[rn] = wb;
            for (int i = 0; i < 16; i++)
                if (list & (1u << i)) { wreg_load(c, i, g32(c, a)); a += 4; }
        } else {
            for (int i = 0; i < 15; i++)
                if (list & (1u << i)) { s32(c, a, c->r[i]); a += 4; }
            if (w) c->r[rn] = wb;
        }
        return;
    }
    // ---- load/store dual, exclusive, table branch
    if ((hw1 & 0xFE40) == 0xE840) {
        unsigned op1 = (hw1 >> 7) & 3, op2 = (hw1 >> 4) & 3, rn = hw1 & 15;
        if (op1 == 1 && op2 == 1 && (hw2 & 0xffe0) == 0xf000) {  // TBB/TBH
            uint32_t base = c->r[rn], m = c->r[hw2 & 15], off;
            off = (hw2 & 0x10) ? g16(c, base + m * 2) : g8(c, base + m);
            branch_to(c, c->r[15] + off * 2);
            return;
        }
        if ((op1 & 2) || (op2 & 2)) {  // LDRD/STRD imm
            unsigned p = (hw1 >> 8) & 1, u = (hw1 >> 7) & 1, w = (hw1 >> 5) & 1, l = (hw1 >> 4) & 1;
            unsigned rt = hw2 >> 12, rt2 = (hw2 >> 8) & 15;
            uint32_t imm = (hw2 & 0xff) * 4;
            uint32_t base = rn == 15 ? (c->r[15] & ~3u) : c->r[rn];
            uint32_t off = u ? base + imm : base - imm;
            uint32_t a = p ? off : base;
            if (l) {
                uint32_t v1 = g32(c, a), v2 = g32(c, a + 4);
                c->r[rt] = v1;
                c->r[rt2] = v2;
            } else {
                s32(c, a, c->r[rt]);
                s32(c, a + 4, c->r[rt2]);
            }
            if (w) c->r[rn] = off;
            return;
        }
        if (op1 == 0 && op2 == 1) {  // LDREX
            uint32_t a = c->r[rn] + (hw2 & 0xff) * 4;
            c->r[hw2 >> 12] = g32(c, a);
            return;
        }
        if (op1 == 0 && op2 == 0) {  // STREX (always succeeds)
            uint32_t a = c->r[rn] + (hw2 & 0xff) * 4;
            s32(c, a, c->r[hw2 >> 12]);
            c->r[(hw2 >> 8) & 15] = 0;
            return;
        }
        UNDEF();
    }
    // ---- data processing (shifted register)
    if ((hw1 & 0xFE00) == 0xEA00) {
        unsigned op = (hw1 >> 5) & 15, s = (hw1 >> 4) & 1, rn = hw1 & 15;
        unsigned rd = (hw2 >> 8) & 15, rm = hw2 & 15;
        unsigned imm5 = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
        unsigned t, amt;
        decode_imm_shift((hw2 >> 4) & 3, imm5, &t, &amt);
        uint32_t b = shift_c(c->r[rm], t, amt, c->c, &cout);
        if (op == 6) {  // PKHBT/PKHTB
            uint32_t a = c->r[rn], r;
            if (hw2 & 0x20) r = (a & 0xffff0000) | (b & 0xffff);
            else r = (b & 0xffff0000) | (a & 0xffff);
            c->r[rd] = r;
            return;
        }
        dp_op(c, op, s, rn, rd, c->r[rn], b, cout, hw1, hw2, pc);
        return;
    }
    if ((hw1 & 0xF800) == 0xF000) {
        if (!(hw2 & 0x8000)) {
            if (!(hw1 & 0x200)) {  // modified immediate
                unsigned op = (hw1 >> 5) & 15, s = (hw1 >> 4) & 1, rn = hw1 & 15, rd = (hw2 >> 8) & 15;
                uint32_t imm12 = ((hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff);
                uint32_t b = thumb_expand_imm_c(imm12, c->c, &cout);
                dp_op(c, op, s, rn, rd, c->r[rn], b, cout, hw1, hw2, pc);
                return;
            }
            // plain binary immediate
            unsigned op = (hw1 >> 4) & 31, rn = hw1 & 15, rd = (hw2 >> 8) & 15;
            uint32_t imm12 = ((hw1 & 0x400) << 1) | ((hw2 >> 4) & 0x700) | (hw2 & 0xff);
            unsigned lsb = ((hw2 >> 10) & 0x1c) | ((hw2 >> 6) & 3);
            switch (op) {
            case 0x00: wreg(c, rd, (rn == 15 ? (c->r[15] & ~3u) : c->r[rn]) + imm12); return;
            case 0x0A: wreg(c, rd, (rn == 15 ? (c->r[15] & ~3u) : c->r[rn]) - imm12); return;
            case 0x04: c->r[rd] = ((hw1 & 15) << 12) | imm12; return;
            case 0x0C: c->r[rd] = (c->r[rd] & 0xffff) | ((((hw1 & 15) << 12) | imm12) << 16); return;
            case 0x14: case 0x1C: {  // SBFX/UBFX
                unsigned width = (hw2 & 31) + 1;
                uint32_t v = c->r[rn] >> lsb;
                v = width == 32 ? v : (v & ((1u << width) - 1));
                c->r[rd] = (op == 0x14) ? sext(v, width) : v;
                return;
            }
            case 0x16: {  // BFI/BFC
                unsigned msb = hw2 & 31;
                if (msb < lsb) UNDEF();
                unsigned width = msb - lsb + 1;
                uint32_t mask = (width == 32 ? 0xffffffffu : ((1u << width) - 1)) << lsb;
                uint32_t src = rn == 15 ? 0 : (c->r[rn] << lsb);
                c->r[rd] = (c->r[rd] & ~mask) | (src & mask);
                return;
            }
            case 0x10: case 0x18: {  // SSAT/USAT
                unsigned sh = (hw1 >> 5) & 1, sat = hw2 & 31, t, amt;
                decode_imm_shift(sh << 1, lsb, &t, &amt);
                int64_t v = (int32_t)shift_c(c->r[rn], t, amt, c->c, &cout);
                if (op == 0x10) {
                    int64_t hi = (1ll << sat) - 1, lo = -(1ll << sat);
                    c->r[rd] = (uint32_t)(v > hi ? hi : v < lo ? lo : v);
                } else {
                    int64_t hi = (1ll << sat) - 1;
                    c->r[rd] = (uint32_t)(v > hi ? hi : v < 0 ? 0 : v);
                }
                return;
            }
            default: UNDEF();
            }
        }
        // branches and misc control
        unsigned s = (hw1 >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1;
        switch (hw2 & 0x5000) {
        case 0x0000: {
            unsigned cond = (hw1 >> 6) & 15;
            if ((cond & 0xe) == 0xe) {  // misc control: hints/barriers are no-ops
                if ((hw1 & 0xfff0) == 0xf3b0 || (hw1 & 0xffff) == 0xf3af) return;
                UNDEF();
            }
            uint32_t imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((hw1 & 0x3f) << 12) | ((hw2 & 0x7ff) << 1);
            if (cond_pass(c, cond)) branch_to(c, c->r[15] + sext(imm, 21));
            return;
        }
        default: {
            unsigned i1 = !(j1 ^ s), i2 = !(j2 ^ s);
            uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((hw1 & 0x3ff) << 12) | ((hw2 & 0x7ff) << 1);
            uint32_t off = sext(imm, 25);
            if ((hw2 & 0x5000) == 0x1000) { branch_to(c, c->r[15] + off); return; }
            c->r[14] = (pc + 4) | 1;
            if (hw2 & 0x1000) branch_to(c, c->r[15] + off);              // BL
            else { c->thumb = 0; c->npc = ((c->r[15] & ~3u) + off) & ~3u; }  // BLX imm
            return;
        }
        }
    }
    // ---- load/store single
    if ((hw1 & 0xFE00) == 0xF800) {
        unsigned sgn = (hw1 >> 8) & 1, u12 = (hw1 >> 7) & 1, size = (hw1 >> 5) & 3, l = (hw1 >> 4) & 1;
        unsigned rn = hw1 & 15, rt = hw2 >> 12;
        uint32_t a;
        int wb = 0;
        uint32_t wbv = 0;
        if (rn == 15) {
            if (!l) UNDEF();
            uint32_t base = c->r[15] & ~3u, imm = hw2 & 0xfff;
            a = u12 ? base + imm : base - imm;
        } else if (u12) {
            a = c->r[rn] + (hw2 & 0xfff);
        } else if (hw2 & 0x800) {
            unsigned p = (hw2 >> 10) & 1, u = (hw2 >> 9) & 1, w = (hw2 >> 8) & 1;
            uint32_t imm = hw2 & 0xff;
            uint32_t off = u ? c->r[rn] + imm : c->r[rn] - imm;
            a = p ? off : c->r[rn];
            if (w) { wb = 1; wbv = off; }
        } else if ((hw2 & 0xfc0) == 0) {
            a = c->r[rn] + (c->r[hw2 & 15] << ((hw2 >> 4) & 3));
        } else {
            UNDEF();
        }
        if (l) {
            if (rt == 15 && size != 2) { if (wb) c->r[rn] = wbv; return; }  // PLD/PLI
            uint32_t v;
            switch (size) {
            case 0: v = g8(c, a); if (sgn) v = sext(v, 8); break;
            case 1: v = g16(c, a); if (sgn) v = sext(v, 16); break;
            case 2: v = g32(c, a); break;
            default: UNDEF();
            }
            if (wb) c->r[rn] = wbv;
            wreg_load(c, rt, v);
        } else {
            if (sgn) UNDEF();
            switch (size) {
            case 0: s8(c, a, c->r[rt]); break;
            case 1: s16(c, a, c->r[rt]); break;
            case 2: s32(c, a, c->r[rt]); break;
            default: UNDEF();
            }
            if (wb) c->r[rn] = wbv;
        }
        return;
    }
    // ---- data processing (register)
    if ((hw1 & 0xFF00) == 0xFA00 && (hw2 & 0xF000) == 0xF000) {
        unsigned op1 = (hw1 >> 4) & 15, op2 = (hw2 >> 4) & 15, rn = hw1 & 15;
        unsigned rd = (hw2 >> 8) & 15, rm = hw2 & 15;
        if (op2 == 0 && op1 < 8) {  // shift by register
            uint32_t r = shift_c(c->r[rn], (op1 >> 1) & 3, c->r[rm] & 0xff, c->c, &cout);
            c->r[rd] = r;
            if (op1 & 1) { nz(c, r); c->c = cout; }
            return;
        }
        if (op2 & 8 && op1 < 8) {  // extend (+add)
            uint32_t m = ror32(c->r[rm], ((hw2 >> 4) & 3) * 8), v;
            switch (op1) {
            case 0: v = sext(m & 0xffff, 16); break;
            case 1: v = m & 0xffff; break;
            case 4: v = sext(m & 0xff, 8); break;
            case 5: v = m & 0xff; break;
            default: UNDEF();
            }
            c->r[rd] = (rn == 15) ? v : c->r[rn] + v;
            return;
        }
        if (op1 == 9 || op1 == 0xB) {
            uint32_t m = c->r[rm], r;
            if (op1 == 0xB && op2 == 8) r = m ? __builtin_clz(m) : 32;
            else if (op1 == 9 && op2 == 8) r = __builtin_bswap32(m);
            else if (op1 == 9 && op2 == 9) r = ((m & 0x00ff00ff) << 8) | ((m >> 8) & 0x00ff00ff);
            else if (op1 == 9 && op2 == 0xB) r = sext(((m & 0xff) << 8) | ((m >> 8) & 0xff), 16);
            else if (op1 == 9 && op2 == 0xA) r = __builtin_bitreverse32(m);
            else UNDEF();
            c->r[rd] = r;
            return;
        }
        UNDEF();
    }
    // ---- multiply / multiply-accumulate
    if ((hw1 & 0xFF80) == 0xFB00) {
        unsigned op1 = (hw1 >> 4) & 7, op2 = (hw2 >> 4) & 3, rn = hw1 & 15;
        unsigned ra = hw2 >> 12, rd = (hw2 >> 8) & 15, rm = hw2 & 15;
        uint32_t a = c->r[rn], b = c->r[rm];
        switch (op1) {
        case 0:
            if (op2 == 0) c->r[rd] = a * b + (ra == 15 ? 0 : c->r[ra]);
            else if (op2 == 1) c->r[rd] = c->r[ra] - a * b;
            else UNDEF();
            return;
        case 1: {  // SMULxy / SMLAxy
            int32_t x = (int16_t)((hw2 & 0x20) ? a >> 16 : a);
            int32_t y = (int16_t)((hw2 & 0x10) ? b >> 16 : b);
            c->r[rd] = (uint32_t)(x * y) + (ra == 15 ? 0 : c->r[ra]);
            return;
        }
        case 3: {  // SMULWy / SMLAWy
            if (op2 & 2) UNDEF();
            int64_t y = (int16_t)((hw2 & 0x10) ? b >> 16 : b);
            uint32_t r = (uint32_t)(((int64_t)(int32_t)a * y) >> 16);
            c->r[rd] = r + (ra == 15 ? 0 : c->r[ra]);
            return;
        }
        case 5: case 6: {  // SMMUL/SMMLA/SMMLS
            int64_t p = (int64_t)(int32_t)a * (int32_t)b;
            int64_t acc = (ra == 15) ? 0 : ((int64_t)c->r[ra] << 32);
            int64_t r = (op1 == 5) ? acc + p : acc - p;
            if (hw2 & 0x10) r += 0x80000000ll;
            c->r[rd] = (uint32_t)((uint64_t)r >> 32);
            return;
        }
        case 2: case 4: {  // SMUAD/SMLAD, SMUSD/SMLSD
            int32_t bm = (hw2 & 0x10) ? (int32_t)ror32(b, 16) : (int32_t)b;
            int32_t p1 = (int16_t)a * (int16_t)bm, p2 = (int16_t)(a >> 16) * (int16_t)((uint32_t)bm >> 16);
            int32_t r = (op1 == 2) ? p1 + p2 : p1 - p2;
            c->r[rd] = (uint32_t)r + (ra == 15 ? 0 : c->r[ra]);
            return;
        }
        default: UNDEF();
        }
    }
    if ((hw1 & 0xFF80) == 0xFB80) {
        unsigned op1 = (hw1 >> 4) & 7, op2 = (hw2 >> 4) & 15, rn = hw1 & 15;
        unsigned lo = hw2 >> 12, hi = (hw2 >> 8) & 15, rm = hw2 & 15;
        uint32_t a = c->r[rn], b = c->r[rm];
        uint64_t acc = ((uint64_t)c->r[hi] << 32) | c->r[lo];
        uint64_t r;
        switch (op1) {
        case 0: r = (uint64_t)((int64_t)(int32_t)a * (int32_t)b); break;
        case 2: r = (uint64_t)a * b; break;
        case 4:
            if (op2 == 0) r = acc + (uint64_t)((int64_t)(int32_t)a * (int32_t)b);
            else if ((op2 & 0xC) == 8) {
                int32_t x = (int16_t)((op2 & 2) ? a >> 16 : a), y = (int16_t)((op2 & 1) ? b >> 16 : b);
                r = acc + (uint64_t)(int64_t)(x * y);
            } else UNDEF();
            break;
        case 6:
            if (op2 == 0) r = acc + (uint64_t)a * b;
            else if (op2 == 6) r = (uint64_t)a * b + c->r[lo] + c->r[hi];
            else UNDEF();
            break;
        case 1: c->r[hi] = b ? (uint32_t)((int32_t)a / (int32_t)b) : 0; return;  // SDIV: Rd in hi slot
        case 3: c->r[hi] = b ? a / b : 0; return;
        default: UNDEF();
        }
        c->r[lo] = (uint32_t)r;
        c->r[hi] = (uint32_t)(r >> 32);
        return;
    }
    UNDEF();
}

