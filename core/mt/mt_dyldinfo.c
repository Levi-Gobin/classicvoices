// Snow Leopard and later describe rebasing and binding with dyld's opcode
// streams (LC_DYLD_INFO) instead of relocation tables. Follows
// panthera-speech's tiger_host_dyldinfo.c.
#include "mt_internal.h"

#include <stdio.h>
#include <string.h>

uint32_t mt_resolve(Mt *m, MtImage *self, const char *name);

typedef struct { const uint8_t *p, *end; Mt *m; const char *path; } Stream;

static uint8_t s_byte(Stream *s) {
    if (s->p >= s->end) { fprintf(stderr, "%s: dyld info stream ran off the end\n", s->path); return 0; }
    return *s->p++;
}
static uint32_t s_uleb(Stream *s) {
    uint32_t v = 0, shift = 0, b;
    do { b = s_byte(s); if (shift < 32) v |= (b & 0x7F) << shift; shift += 7; } while (b & 0x80);
    return v;
}
static int32_t s_sleb(Stream *s) {
    int32_t v = 0;
    uint32_t shift = 0, b;
    do { b = s_byte(s); if (shift < 32) v |= (int32_t)((b & 0x7F) << shift); shift += 7; } while (b & 0x80);
    if (shift < 32 && (b & 0x40)) v |= -(1 << shift);
    return v;
}
static const char *s_cstr(Stream *s) {
    const char *start = (const char *)s->p;
    while (s->p < s->end && *s->p) s->p++;
    s->p++;
    return start;
}

static inline uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
// dyld_info_command: cmd, size, rebase_off, rebase_size, bind_off, bind_size, weak_off, weak_size, lazy_off, lazy_size, ...

static void rebase_at(Mt *m, MtImage *im, uint32_t seg, uint32_t off, uint32_t type, uint32_t *n) {
    if ((int)seg >= im->nsegs) return;
    uint32_t a = im->segaddr[seg] + off + im->slide;
    if (type == 1 || type == 2) {  // pointer / text absolute32
        uint32_t v = u32(m->mem + a) + im->slide;
        memcpy(m->mem + a, &v, 4);
        (*n)++;
    }
}

void mt_apply_rebases(Mt *m, MtImage *im) {
    Stream s = {im->slice + u32(im->info + 8), NULL, m, im->path};
    s.end = s.p + u32(im->info + 12);
    uint32_t seg = 0, off = 0, type = 1, n = 0;
    while (s.p < s.end) {
        uint8_t b = s_byte(&s);
        uint32_t op = b & 0xF0, imm = b & 0x0F;
        switch (op) {
        case 0x00: goto done;
        case 0x10: type = imm; break;
        case 0x20: seg = imm; off = s_uleb(&s); break;
        case 0x30: off += s_uleb(&s); break;
        case 0x40: off += imm * 4; break;
        case 0x50: for (uint32_t i = 0; i < imm; i++) { rebase_at(m, im, seg, off, type, &n); off += 4; } break;
        case 0x60: { uint32_t k = s_uleb(&s); for (uint32_t i = 0; i < k; i++) { rebase_at(m, im, seg, off, type, &n); off += 4; } break; }
        case 0x70: rebase_at(m, im, seg, off, type, &n); off += 4 + s_uleb(&s); break;
        case 0x80: { uint32_t k = s_uleb(&s), skip = s_uleb(&s); for (uint32_t i = 0; i < k; i++) { rebase_at(m, im, seg, off, type, &n); off += 4 + skip; } break; }
        default: fprintf(stderr, "%s: unknown rebase opcode %02x\n", im->path, op); goto done;
        }
    }
done:
    if (m->verbose) fprintf(stderr, "  %u rebases\n", n);
}

static void bind_stream(Mt *m, MtImage *im, uint32_t off_field, int lazy, uint32_t *n) {
    uint32_t start = u32(im->info + off_field), size = u32(im->info + off_field + 4);
    if (!size) return;
    Stream s = {im->slice + start, im->slice + start + size, m, im->path};
    uint32_t seg = 0, off = 0, type = 1;
    int32_t addend = 0;
    const char *sym = NULL;
    int weak = 0;
#define DO_BIND() do { \
        if (sym && type == 1 && (int)seg < im->nsegs) { \
            uint32_t t = mt_resolve(m, im, sym); \
            if (t || !weak) { uint32_t v = t + (uint32_t)addend; memcpy(m->mem + (uint32_t)(im->segaddr[seg] + off + im->slide), &v, 4); (*n)++; } \
        } } while (0)
    while (s.p < s.end) {
        uint8_t b = s_byte(&s);
        uint32_t op = b & 0xF0, imm = b & 0x0F;
        switch (op) {
        case 0x00:
            if (!lazy) return;
            sym = NULL; addend = 0; weak = 0; type = 1; seg = 0; off = 0;
            break;
        case 0x10: case 0x30: break;
        case 0x20: s_uleb(&s); break;
        case 0x40: weak = imm & 1; sym = s_cstr(&s); break;
        case 0x50: type = imm; break;
        case 0x60: addend = s_sleb(&s); break;
        case 0x70: seg = imm; off = s_uleb(&s); break;
        case 0x80: off += s_uleb(&s); break;
        case 0x90: DO_BIND(); off += 4; break;
        case 0xA0: DO_BIND(); off += 4 + s_uleb(&s); break;
        case 0xB0: DO_BIND(); off += 4 + imm * 4; break;
        case 0xC0: { uint32_t k = s_uleb(&s), skip = s_uleb(&s); for (uint32_t i = 0; i < k; i++) { DO_BIND(); off += 4 + skip; } break; }
        default: fprintf(stderr, "%s: unknown bind opcode %02x\n", im->path, op); return;
        }
    }
#undef DO_BIND
}

void mt_apply_binds(Mt *m, MtImage *im) {
    uint32_t n = 0;
    bind_stream(m, im, 16, 0, &n);  // bind
    bind_stream(m, im, 32, 1, &n);  // lazy bind
    if (m->verbose) fprintf(stderr, "%s: %u dyld-info binds\n", im->path, n);
}
