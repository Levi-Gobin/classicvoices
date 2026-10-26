// libSystem for the MacinTalk guest: memory, strings, stdio, files, math,
// pthreads, time. Guest pointers are offsets into m->mem.
#include "mt_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- helpers

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }
static inline void R64(X86 *c, uint64_t v) { c->r[EAX] = (uint32_t)v; c->r[EDX] = (uint32_t)(v >> 32); }
static inline double argd(X86 *c, int word) { uint64_t u = (uint64_t)A(c, word) | (uint64_t)A(c, word + 1) << 32; double d; memcpy(&d, &u, 8); return d; }
static inline float argf(X86 *c, int word) { uint32_t u = A(c, word); float f; memcpy(&f, &u, 4); return f; }
static inline void RD(X86 *c, double v) { x86_fpush(c, v); }  // float/double return in ST(0)

void mt_check(Mt *m, X86 *c, uint32_t a, uint32_t n) {
    if (n && ((uint64_t)a + n > m->mem_size || a < 0x1000)) x86_fault(c, "bad guest range %#x+%u", a, n);
}

uint32_t *mt_errno_slot(Mt *m, X86 *c) { return (uint32_t *)(void *)G(m, mt_tls(m, c)->errno_addr); }
static void set_errno(Mt *m, X86 *c, int e) { *mt_errno_slot(m, c) = (uint32_t)e; }

// ---------------------------------------------------------------- memory

static void h_malloc(Mt *m, X86 *c) { R(c, mt_malloc(m, A(c, 0))); }
static void h_calloc(Mt *m, X86 *c) { R(c, mt_calloc(m, A(c, 0) * A(c, 1))); }
static void h_free(Mt *m, X86 *c) { mt_free(m, A(c, 0)); }
static void h_realloc(Mt *m, X86 *c) {
    uint32_t p = A(c, 0), n = A(c, 1);
    if (!p) { R(c, mt_malloc(m, n)); return; }
    uint32_t old = mt_msize(m, p);
    if (n <= old) { R(c, p); return; }
    uint32_t q = mt_malloc(m, n);
    memcpy(G(m, q), G(m, p), old);
    mt_free(m, p);
    R(c, q);
}
static void h_memcpy(Mt *m, X86 *c) { uint32_t d = A(c, 0), s = A(c, 1), n = A(c, 2); mt_check(m, c, d, n); mt_check(m, c, s, n); memmove(G(m, d), G(m, s), n); R(c, d); }
static void h_memset(Mt *m, X86 *c) { uint32_t d = A(c, 0), n = A(c, 2); mt_check(m, c, d, n); memset(G(m, d), (int)A(c, 1), n); R(c, d); }
static void h_memcmp(Mt *m, X86 *c) { uint32_t n = A(c, 2); mt_check(m, c, A(c, 0), n); mt_check(m, c, A(c, 1), n); R(c, (uint32_t)memcmp(G(m, A(c, 0)), G(m, A(c, 1)), n)); }
static void h_memchr(Mt *m, X86 *c) {
    uint32_t s = A(c, 0), n = A(c, 2);
    mt_check(m, c, s, n);
    uint8_t *p = memchr(G(m, s), (int)(A(c, 1) & 0xFF), n);
    R(c, p ? (uint32_t)(p - m->mem) : 0);
}

// ---------------------------------------------------------------- strings

static void h_strlen(Mt *m, X86 *c) { R(c, (uint32_t)strlen(mt_str(m, A(c, 0)))); }
static void h_strcmp(Mt *m, X86 *c) { R(c, (uint32_t)strcmp(mt_str(m, A(c, 0)), mt_str(m, A(c, 1)))); }
static void h_strcoll(Mt *m, X86 *c) { R(c, (uint32_t)strcmp(mt_str(m, A(c, 0)), mt_str(m, A(c, 1)))); }
static void h_strcpy(Mt *m, X86 *c) { const char *s = mt_str(m, A(c, 1)); size_t n = strlen(s) + 1; mt_check(m, c, A(c, 0), (uint32_t)n); memmove(G(m, A(c, 0)), s, n); R(c, A(c, 0)); }
static void h_strcat(Mt *m, X86 *c) {
    const char *d = mt_str(m, A(c, 0)), *s = mt_str(m, A(c, 1));
    size_t dl = strlen(d), n = strlen(s) + 1;
    mt_check(m, c, A(c, 0), (uint32_t)(dl + n));
    memmove(G(m, A(c, 0)) + dl, s, n);
    R(c, A(c, 0));
}
static void h_strchr(Mt *m, X86 *c) { const char *s = mt_str(m, A(c, 0)); const char *p = strchr(s, (int)(A(c, 1) & 0xFF)); R(c, p ? (uint32_t)((const uint8_t *)p - m->mem) : 0); }
static void h_strstr(Mt *m, X86 *c) {
    const char *h = mt_str(m, A(c, 0)), *p = strstr(h, mt_str(m, A(c, 1)));
    R(c, p ? A(c, 0) + (uint32_t)(p - h) : 0);
}
static void h_strtol(Mt *m, X86 *c) {
    const char *s = mt_str(m, A(c, 0));
    char *e;
    long v = strtol(s, &e, (int)A(c, 2));
    if (A(c, 1)) wr32(c, A(c, 1), A(c, 0) + (uint32_t)(e - s));
    R(c, (uint32_t)(int32_t)v);
}
static void h_strdup(Mt *m, X86 *c) { R(c, mt_strdup(m, mt_str(m, A(c, 0)))); }
static void h_strtok_r(Mt *m, X86 *c) {
    uint32_t s = A(c, 0), sep = A(c, 1), save = A(c, 2);
    if (!s) s = rd32(c, save);
    if (!s) { R(c, 0); return; }
    const char *seps = mt_str(m, sep);
    char *p = (char *)G(m, s);
    p += strspn(p, seps);
    if (!*p) { wr32(c, save, 0); R(c, 0); return; }
    char *e = p + strcspn(p, seps);
    if (*e) { *e = 0; wr32(c, save, (uint32_t)((uint8_t *)e + 1 - m->mem)); }
    else wr32(c, save, 0);
    R(c, (uint32_t)((uint8_t *)p - m->mem));
}
static void h_strxfrm(Mt *m, X86 *c) {
    const char *s = mt_str(m, A(c, 1));
    size_t n = strlen(s);
    if (A(c, 2) > n) { memcpy(G(m, A(c, 0)), s, n + 1); }
    R(c, (uint32_t)n);
}
static void h_tolower(Mt *m, X86 *c) { (void)m; uint32_t v = A(c, 0); R(c, v < 128 ? (uint32_t)tolower((int)v) : v); }
static void h_toupper(Mt *m, X86 *c) { (void)m; uint32_t v = A(c, 0); R(c, v < 128 ? (uint32_t)toupper((int)v) : v); }
// ___maskrune(c, mask): the ctype table lookup for characters >= 128 (or as a call)
uint32_t mt_rune_mask(uint32_t ch);
static void h_maskrune(Mt *m, X86 *c) { (void)m; R(c, mt_rune_mask(A(c, 0)) & A(c, 1)); }

static void h_atoi(Mt *m, X86 *c) { R(c, (uint32_t)atoi(mt_str(m, A(c, 0)))); }
static void h_atol(Mt *m, X86 *c) { R(c, (uint32_t)atol(mt_str(m, A(c, 0)))); }
static void h_atof(Mt *m, X86 *c) { RD(c, atof(mt_str(m, A(c, 0)))); }
static void endptr(Mt *m, X86 *c, uint32_t s, const char *base, const char *end, uint32_t out) {
    if (out) wr32(c, out, s + (uint32_t)(end - base));
    (void)m;
}
static void h_strtod(Mt *m, X86 *c) { const char *s = mt_str(m, A(c, 0)); char *e; double v = strtod(s, &e); endptr(m, c, A(c, 0), s, e, A(c, 1)); RD(c, v); }
static void h_strtof(Mt *m, X86 *c) { const char *s = mt_str(m, A(c, 0)); char *e; float v = strtof(s, &e); endptr(m, c, A(c, 0), s, e, A(c, 1)); RD(c, v); }

// ---------------------------------------------------------------- printf

// i386 varargs: consecutive stack words; doubles take two.
typedef struct { X86 *c; uint32_t sp; } Va;
static uint32_t va32(Va *v) { uint32_t x = rd32(v->c, v->sp); v->sp += 4; return x; }
static double vad(Va *v) { uint64_t u = rd32(v->c, v->sp) | (uint64_t)rd32(v->c, v->sp + 4) << 32; v->sp += 8; double d; memcpy(&d, &u, 8); return d; }

size_t mt_format(Mt *m, char *out, size_t cap, const char *fmt, Va *va) {
    size_t n = 0;
#define PUT(s, len) do { size_t _l = (len); if (n + _l < cap) memcpy(out + n, (s), _l); else if (n < cap) memcpy(out + n, (s), cap - n - 1); n += _l; } while (0)
    for (const char *p = fmt; *p;) {
        if (*p != '%') { const char *q = strchr(p, '%'); size_t l = q ? (size_t)(q - p) : strlen(p); PUT(p, l); p += l; continue; }
        const char *spec = p++;
        while (*p && strchr("-+ #0", *p)) p++;
        int star_w = 0, star_p = 0;
        if (*p == '*') { star_w = 1; p++; } else while (isdigit((unsigned char)*p)) p++;
        if (*p == '.') { p++; if (*p == '*') { star_p = 1; p++; } else while (isdigit((unsigned char)*p)) p++; }
        int ll = 0, h = 0, L = 0;
        while (*p && strchr("hlLqjzt", *p)) { if (*p == 'l') ll++; if (*p == 'q') ll = 2; if (*p == 'h') h++; if (*p == 'L') L = 1; p++; }
        char conv = *p ? *p++ : 0;
        char f[32];
        size_t fl = (size_t)(p - spec);
        if (fl >= sizeof f - 4) fl = sizeof f - 4;
        // rebuild the spec without length modifiers, then add host ones
        char base[32];
        size_t bl = 0;
        for (size_t i = 0; i < fl - 1; i++) if (!strchr("hlLqjzt", spec[i])) base[bl++] = spec[i];
        base[bl] = 0;
        int w = star_w ? (int)va32(va) : 0, pr = star_p ? (int)va32(va) : 0;
        char tmp[512];
        int k = 0;
        switch (conv) {
        case '%': PUT("%", 1); continue;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (ll >= 2) {
                uint64_t v = va32(va); v |= (uint64_t)va32(va) << 32;
                snprintf(f, sizeof f, "%sll%c", base, conv);
                k = star_w && star_p ? snprintf(tmp, sizeof tmp, f, w, pr, v) : star_w ? snprintf(tmp, sizeof tmp, f, w, v) : star_p ? snprintf(tmp, sizeof tmp, f, pr, v) : snprintf(tmp, sizeof tmp, f, v);
            } else {
                uint32_t v = va32(va);
                if (h == 1) v = conv == 'd' || conv == 'i' ? (uint32_t)(int32_t)(int16_t)v : (uint16_t)v;
                if (h == 2) v = conv == 'd' || conv == 'i' ? (uint32_t)(int32_t)(int8_t)v : (uint8_t)v;
                snprintf(f, sizeof f, "%s%c", base, conv);
                int iv = (int)v;
                k = star_w && star_p ? snprintf(tmp, sizeof tmp, f, w, pr, iv) : star_w ? snprintf(tmp, sizeof tmp, f, w, iv) : star_p ? snprintf(tmp, sizeof tmp, f, pr, iv) : snprintf(tmp, sizeof tmp, f, iv);
            }
            break;
        }
        case 'p': k = snprintf(tmp, sizeof tmp, "0x%x", va32(va)); break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double v;
            if (L) {  // long double: 12 bytes on i386
                uint8_t b[12];
                for (int i = 0; i < 3; i++) { uint32_t x = va32(va); memcpy(b + 4 * i, &x, 4); }
                uint64_t mant; uint16_t se; memcpy(&mant, b, 8); memcpy(&se, b + 8, 2);
                v = (se & 0x7FFF) ? ldexp((double)mant, (se & 0x7FFF) - 16383 - 63) : 0;
                if (se & 0x8000) v = -v;
            } else v = vad(va);
            snprintf(f, sizeof f, "%s%c", base, conv);
            k = star_w && star_p ? snprintf(tmp, sizeof tmp, f, w, pr, v) : star_w ? snprintf(tmp, sizeof tmp, f, w, v) : star_p ? snprintf(tmp, sizeof tmp, f, pr, v) : snprintf(tmp, sizeof tmp, f, v);
            break;
        }
        case 's': {
            uint32_t a = va32(va);
            const char *s = a ? mt_str(m, a) : "(null)";
            snprintf(f, sizeof f, "%ss", base);
            char *big = NULL;
            size_t need = strlen(s) + (size_t)(w > 0 ? w : 0) + 1;
            char *dst = need < sizeof tmp ? tmp : (big = malloc(need));
            k = star_w && star_p ? snprintf(dst, need, f, w, pr, s) : star_w ? snprintf(dst, need, f, w, s) : star_p ? snprintf(dst, need, f, pr, s) : snprintf(dst, need, f, s);
            PUT(dst, (size_t)k);
            free(big);
            continue;
        }
        default:
            PUT(spec, (size_t)(p - spec));
            continue;
        }
        if (k > 0) PUT(tmp, (size_t)(k < (int)sizeof tmp ? k : (int)sizeof tmp - 1));
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
    return n;
}

static uint32_t put_guest(Mt *m, X86 *c, uint32_t dst, uint32_t cap, const char *s, size_t n) {
    if (!cap) return (uint32_t)n;
    size_t k = n < cap - 1 ? n : cap - 1;
    mt_check(m, c, dst, (uint32_t)k + 1);
    memcpy(G(m, dst), s, k);
    G(m, dst)[k] = 0;
    return (uint32_t)n;
}

// FILE: a 16-byte guest object {magic 'MTFL', host fd slot, 0, 0}
#define MAX_FILES 64
static FILE *g_files[MAX_FILES];
static pthread_mutex_t g_files_mu = PTHREAD_MUTEX_INITIALIZER;


static FILE *host_file(Mt *m, X86 *c, uint32_t gf) {
    if (gf == 0) x86_fault(c, "NULL FILE*");
    if (memcmp(G(m, gf), "MTFL", 4)) x86_fault(c, "not a FILE %#x", gf);
    uint32_t i = rd32(c, gf + 4);
    if (i == 1) return stdout;
    if (i == 2) return stderr;
    return i < MAX_FILES ? g_files[i] : NULL;
}
static int quiet_stdio(void) { static int q = -1; if (q < 0) q = getenv("MT_STDIO") == NULL; return q; }

static void h_printf(Mt *m, X86 *c) {
    char buf[4096];
    Va va = {c, c->r[ESP] + 8};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 0)), &va);
    if (!quiet_stdio()) fputs(buf, stderr);
    R(c, (uint32_t)n);
}
static void h_fprintf(Mt *m, X86 *c) {
    char buf[4096];
    Va va = {c, c->r[ESP] + 12};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 1)), &va);
    FILE *f = host_file(m, c, A(c, 0));
    if (f == stdout || f == stderr) { if (!quiet_stdio()) fputs(buf, stderr); }
    else if (f) fwrite(buf, 1, n, f);
    R(c, (uint32_t)n);
}
static void h_sprintf(Mt *m, X86 *c) {
    char buf[8192];
    Va va = {c, c->r[ESP] + 12};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 1)), &va);
    R(c, put_guest(m, c, A(c, 0), (uint32_t)n + 1, buf, n));
}
static void h_snprintf(Mt *m, X86 *c) {
    char buf[8192];
    Va va = {c, c->r[ESP] + 16};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 2)), &va);
    R(c, put_guest(m, c, A(c, 0), A(c, 1), buf, n));
}
static void h_vsprintf(Mt *m, X86 *c) {
    char buf[8192];
    Va va = {c, A(c, 2)};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 1)), &va);
    R(c, put_guest(m, c, A(c, 0), (uint32_t)n + 1, buf, n));
}
static void h_vsnprintf(Mt *m, X86 *c) {
    char buf[8192];
    Va va = {c, A(c, 3)};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 2)), &va);
    R(c, put_guest(m, c, A(c, 0), A(c, 1), buf, n));
}
static void h_vfprintf(Mt *m, X86 *c) {
    char buf[4096];
    Va va = {c, A(c, 2)};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 1)), &va);
    FILE *f = host_file(m, c, A(c, 0));
    if (f == stdout || f == stderr) { if (!quiet_stdio()) fputs(buf, stderr); }
    else if (f) fwrite(buf, 1, n, f);
    R(c, (uint32_t)n);
}
static void h_puts(Mt *m, X86 *c) { if (!quiet_stdio()) fprintf(stderr, "%s\n", mt_str(m, A(c, 0))); R(c, 1); }
static void h_putchar(Mt *m, X86 *c) { (void)m; if (!quiet_stdio()) fputc((int)A(c, 0), stderr); R(c, A(c, 0)); }
static void h_fputs(Mt *m, X86 *c) {
    FILE *f = host_file(m, c, A(c, 1));
    const char *s = mt_str(m, A(c, 0));
    if (f == stdout || f == stderr) { if (!quiet_stdio()) fputs(s, stderr); } else if (f) fputs(s, f);
    R(c, 1);
}
static void h_fputc(Mt *m, X86 *c) {
    FILE *f = host_file(m, c, A(c, 1));
    if (f == stdout || f == stderr) { if (!quiet_stdio()) fputc((int)A(c, 0), stderr); } else if (f) fputc((int)A(c, 0), f);
    R(c, A(c, 0));
}
static void h_fwrite(Mt *m, X86 *c) {
    uint32_t n = A(c, 1) * A(c, 2);
    FILE *f = host_file(m, c, A(c, 3));
    mt_check(m, c, A(c, 0), n);
    if (f == stdout || f == stderr) { if (!quiet_stdio()) fwrite(G(m, A(c, 0)), 1, n, stderr); R(c, A(c, 2)); return; }
    R(c, f ? (uint32_t)fwrite(G(m, A(c, 0)), A(c, 1), A(c, 2), f) : 0);
}
static void h_fflush(Mt *m, X86 *c) { (void)m; R(c, 0); }
static void h_fopen(Mt *m, X86 *c) {
    const char *path = mt_str(m, A(c, 0)), *mode = mt_str(m, A(c, 1));
    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) { set_errno(m, c, EACCES); R(c, 0); return; }  // read-only engine data
    FILE *f = fopen(path, "rb");
    if (!f) { set_errno(m, c, errno); R(c, 0); return; }
    pthread_mutex_lock(&g_files_mu);
    int i;
    for (i = 3; i < MAX_FILES && g_files[i]; i++) {}
    if (i == MAX_FILES) { pthread_mutex_unlock(&g_files_mu); fclose(f); R(c, 0); return; }
    g_files[i] = f;
    pthread_mutex_unlock(&g_files_mu);
    uint32_t g = mt_calloc(m, 16);
    memcpy(G(m, g), "MTFL", 4);
    wr32(c, g + 4, (uint32_t)i);
    R(c, g);
}
static void h_fclose(Mt *m, X86 *c) {
    uint32_t g = A(c, 0);
    FILE *f = host_file(m, c, g);
    uint32_t i = rd32(c, g + 4);
    if (f && i >= 3) { fclose(f); g_files[i] = NULL; }
    memset(G(m, g), 0, 4);
    mt_free(m, g);
    R(c, 0);
}
static void h_fread(Mt *m, X86 *c) {
    uint32_t n = A(c, 1) * A(c, 2);
    FILE *f = host_file(m, c, A(c, 3));
    mt_check(m, c, A(c, 0), n);
    R(c, f ? (uint32_t)fread(G(m, A(c, 0)), A(c, 1), A(c, 2), f) : 0);
}
static void h_fseek(Mt *m, X86 *c) { FILE *f = host_file(m, c, A(c, 0)); R(c, f ? (uint32_t)fseek(f, (long)(int32_t)A(c, 1), (int)A(c, 2)) : ~0u); }
static void h_ftell(Mt *m, X86 *c) { FILE *f = host_file(m, c, A(c, 0)); R(c, f ? (uint32_t)ftell(f) : ~0u); }
static void h_getc(Mt *m, X86 *c) { FILE *f = host_file(m, c, A(c, 0)); R(c, f && f != stdout && f != stderr ? (uint32_t)getc(f) : ~0u); }
static void h_ungetc(Mt *m, X86 *c) { FILE *f = host_file(m, c, A(c, 1)); R(c, f ? (uint32_t)ungetc((int)A(c, 0), f) : ~0u); }
static void h_fileno(Mt *m, X86 *c) { R(c, rd32(c, A(c, 0) + 4)); (void)m; }

// ---------------------------------------------------------------- POSIX files
// Descriptors are host descriptors (the engine only reads voice data).

static void h_open(Mt *m, X86 *c) {
    int flags = (int)A(c, 1);
    if ((flags & 3) != O_RDONLY) { set_errno(m, c, EACCES); R(c, ~0u); return; }
    int fd = open(mt_str(m, A(c, 0)), O_RDONLY);
    if (fd < 0) set_errno(m, c, errno);
    R(c, (uint32_t)fd);
}
static void h_close(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)close((int)A(c, 0))); }
static void h_read(Mt *m, X86 *c) {
    uint32_t n = A(c, 2);
    mt_check(m, c, A(c, 1), n);
    ssize_t r = read((int)A(c, 0), G(m, A(c, 1)), n);
    if (r < 0) set_errno(m, c, errno);
    R(c, (uint32_t)r);
}
static void h_pread(Mt *m, X86 *c) {
    uint32_t n = A(c, 2);
    int64_t off = (int64_t)((uint64_t)A(c, 3) | (uint64_t)A(c, 4) << 32);
    mt_check(m, c, A(c, 1), n);
    ssize_t r = pread((int)A(c, 0), G(m, A(c, 1)), n, (off_t)off);
    if (r < 0) set_errno(m, c, errno);
    R(c, (uint32_t)r);
}
static void h_write(Mt *m, X86 *c) {
    int fd = (int)A(c, 0);
    uint32_t n = A(c, 2);
    if (fd == 1 || fd == 2) { mt_check(m, c, A(c, 1), n); if (!quiet_stdio()) fwrite(G(m, A(c, 1)), 1, n, stderr); R(c, n); return; }
    set_errno(m, c, EBADF);
    R(c, ~0u);
}
static void h_lseek(Mt *m, X86 *c) {
    int64_t off = (int64_t)((uint64_t)A(c, 1) | (uint64_t)A(c, 2) << 32);
    off_t r = lseek((int)A(c, 0), (off_t)off, (int)A(c, 3));
    (void)m;
    R64(c, (uint64_t)r);
}
static void h_fcntl(Mt *m, X86 *c) { (void)m; R(c, 0); }
static void h_ioctl(Mt *m, X86 *c) { (void)m; R(c, ~0u); }

// struct stat for i386 Darwin (32-bit inode variant): 96 bytes
//   0 dev, 4 ino, 8 mode(16), 10 nlink(16), 12 uid, 16 gid, 20 rdev,
//   24 atimespec, 32 mtimespec, 40 ctimespec, 48 size(64), 56 blocks(64),
//   64 blksize, 68 flags, 72 gen, 76 lspare, 80 qspare[2]
static void fill_stat(Mt *m, X86 *c, uint32_t out, const struct stat *st) {
    mt_check(m, c, out, 96);
    uint8_t *p = G(m, out);
    memset(p, 0, 96);
    uint32_t v;
    v = (uint32_t)st->st_dev; memcpy(p + 0, &v, 4);
    v = (uint32_t)st->st_ino; memcpy(p + 4, &v, 4);
    uint16_t mode = (uint16_t)st->st_mode, nlink = (uint16_t)st->st_nlink;
    memcpy(p + 8, &mode, 2); memcpy(p + 10, &nlink, 2);
    v = st->st_uid; memcpy(p + 12, &v, 4);
    v = st->st_gid; memcpy(p + 16, &v, 4);
    v = (uint32_t)st->st_atime; memcpy(p + 24, &v, 4);
    v = (uint32_t)st->st_mtime; memcpy(p + 32, &v, 4);
    v = (uint32_t)st->st_ctime; memcpy(p + 40, &v, 4);
    uint64_t sz = (uint64_t)st->st_size; memcpy(p + 48, &sz, 8);
    uint64_t blocks = (uint64_t)st->st_blocks; memcpy(p + 56, &blocks, 8);
    v = (uint32_t)st->st_blksize; memcpy(p + 64, &v, 4);
}
// struct stat with 64-bit inodes (stat$INODE64, 10.6+), i386: 108 bytes
//   0 dev, 4 mode(16), 6 nlink(16), 8 ino(64), 16 uid, 20 gid, 24 rdev,
//   28 atime, 36 mtime, 44 ctime, 52 birthtime, 60 size(64), 68 blocks(64),
//   76 blksize, 80 flags, 84 gen, 88 lspare, 92 qspare[2]
static void fill_stat64(Mt *m, X86 *c, uint32_t out, const struct stat *st) {
    mt_check(m, c, out, 108);
    uint8_t *p = G(m, out);
    memset(p, 0, 108);
    uint32_t v;
    v = (uint32_t)st->st_dev; memcpy(p + 0, &v, 4);
    uint16_t mode = (uint16_t)st->st_mode, nlink = (uint16_t)st->st_nlink;
    memcpy(p + 4, &mode, 2); memcpy(p + 6, &nlink, 2);
    uint64_t ino = (uint64_t)st->st_ino; memcpy(p + 8, &ino, 8);
    v = st->st_uid; memcpy(p + 16, &v, 4);
    v = st->st_gid; memcpy(p + 20, &v, 4);
    v = (uint32_t)st->st_atime; memcpy(p + 28, &v, 4);
    v = (uint32_t)st->st_mtime; memcpy(p + 36, &v, 4);
    v = (uint32_t)st->st_ctime; memcpy(p + 44, &v, 4);
    uint64_t sz = (uint64_t)st->st_size; memcpy(p + 60, &sz, 8);
    uint64_t blocks = (uint64_t)st->st_blocks; memcpy(p + 68, &blocks, 8);
    v = (uint32_t)st->st_blksize; memcpy(p + 76, &v, 4);
}
static void h_stat64(Mt *m, X86 *c) {
    struct stat st;
    if (stat(mt_str(m, A(c, 0)), &st)) { set_errno(m, c, errno); R(c, ~0u); return; }
    fill_stat64(m, c, A(c, 1), &st);
    R(c, 0);
}
static void h_fstat64(Mt *m, X86 *c) {
    struct stat st;
    if (fstat((int)A(c, 0), &st)) { set_errno(m, c, errno); R(c, ~0u); return; }
    fill_stat64(m, c, A(c, 1), &st);
    R(c, 0);
}
static void h_stat(Mt *m, X86 *c) {
    struct stat st;
    if (stat(mt_str(m, A(c, 0)), &st)) { set_errno(m, c, errno); R(c, ~0u); return; }
    fill_stat(m, c, A(c, 1), &st);
    R(c, 0);
}
static void h_fstat(Mt *m, X86 *c) {
    struct stat st;
    if (fstat((int)A(c, 0), &st)) { set_errno(m, c, errno); R(c, ~0u); return; }
    fill_stat(m, c, A(c, 1), &st);
    R(c, 0);
}

// mmap: files are mapped read-only into the guest's mapping region. Address
// space given back by munmap is reused: every voice change maps the voice's
// files, and Alex alone takes hundreds of megabytes.
static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
#define MAX_FREE_MAPS 256
static struct { uint32_t a, len; } g_free_maps[MAX_FREE_MAPS];  // sorted by address, never adjacent
static int g_nfree_maps;

// with g_map_mu held: the start of `span` free bytes, or 0
static uint32_t map_take_free(uint32_t span) {
    for (int i = 0; i < g_nfree_maps; i++) {
        if (g_free_maps[i].len < span) continue;
        uint32_t a = g_free_maps[i].a;
        g_free_maps[i].a += span;
        g_free_maps[i].len -= span;
        if (!g_free_maps[i].len) {
            memmove(&g_free_maps[i], &g_free_maps[i + 1], (size_t)(g_nfree_maps - i - 1) * sizeof g_free_maps[0]);
            g_nfree_maps--;
        }
        return a;
    }
    return 0;
}

// with g_map_mu held
static void map_give_back(Mt *m, uint32_t a, uint32_t len) {
    if (a + len == m->map_next) {  // the end of what was handed out: just move back
        m->map_next = a;
        while (g_nfree_maps && g_free_maps[g_nfree_maps - 1].a + g_free_maps[g_nfree_maps - 1].len == m->map_next)
            m->map_next = g_free_maps[--g_nfree_maps].a;
        return;
    }
    int i = 0;
    while (i < g_nfree_maps && g_free_maps[i].a < a) i++;
    if (i > 0 && g_free_maps[i - 1].a + g_free_maps[i - 1].len >= a) {  // joins (or overlaps) the one before
        uint32_t end = a + len > g_free_maps[i - 1].a + g_free_maps[i - 1].len ? a + len : g_free_maps[i - 1].a + g_free_maps[i - 1].len;
        g_free_maps[i - 1].len = end - g_free_maps[i - 1].a;
        i--;
    } else {
        if (g_nfree_maps == MAX_FREE_MAPS) return;  // too fragmented to remember: leak it
        memmove(&g_free_maps[i + 1], &g_free_maps[i], (size_t)(g_nfree_maps - i) * sizeof g_free_maps[0]);
        g_free_maps[i].a = a;
        g_free_maps[i].len = len;
        g_nfree_maps++;
    }
    while (i + 1 < g_nfree_maps && g_free_maps[i].a + g_free_maps[i].len >= g_free_maps[i + 1].a) {  // joins the one after
        uint32_t end = g_free_maps[i + 1].a + g_free_maps[i + 1].len;
        if (end > g_free_maps[i].a + g_free_maps[i].len) g_free_maps[i].len = end - g_free_maps[i].a;
        memmove(&g_free_maps[i + 1], &g_free_maps[i + 2], (size_t)(g_nfree_maps - i - 2) * sizeof g_free_maps[0]);
        g_nfree_maps--;
    }
}

static void h_mmap(Mt *m, X86 *c) {
    uint32_t len = A(c, 1), flags = A(c, 3);
    int fd = (int)A(c, 4);
    int64_t off = (int64_t)((uint64_t)A(c, 5) | (uint64_t)A(c, 6) << 32);
    size_t page = (size_t)getpagesize();
    size_t span = ((size_t)len + page - 1) & ~(page - 1);
    pthread_mutex_lock(&g_map_mu);
    uint32_t a = span <= 0xFFFFFFFFu ? map_take_free((uint32_t)span) : 0;
    if (!a) {
        a = (m->map_next + (uint32_t)page - 1) & ~((uint32_t)page - 1);
        if ((uint64_t)a + span > m->map_hi) { pthread_mutex_unlock(&g_map_mu); set_errno(m, c, ENOMEM); R(c, ~0u); return; }
        m->map_next = a + (uint32_t)span;
    }
    pthread_mutex_unlock(&g_map_mu);
    if (!(flags & 0x1000) && fd >= 0) {  // not MAP_ANON
        void *p = mmap(G(m, a), span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)off);
        if (p == MAP_FAILED) {
            if (m->verbose) fprintf(stderr, "[mmap] fd %d len %u off %lld failed: %s\n", fd, len, (long long)off, strerror(errno));
            set_errno(m, c, errno);
            R(c, ~0u);
            return;
        }
    }
    if (m->verbose) fprintf(stderr, "[mmap] fd %d len %u flags %#x -> %#x\n", fd, len, flags, a);
    R(c, a);
}
static void h_munmap(Mt *m, X86 *c) {
    uint32_t a = A(c, 0), len = A(c, 1);
    size_t page = (size_t)getpagesize();
    size_t span = ((size_t)len + page - 1) & ~(page - 1);
    if (a >= m->map_lo && a + span <= m->map_next && !(a & (page - 1))) {
        mmap(G(m, a), span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_ANON, -1, 0);  // drop the file pages
        pthread_mutex_lock(&g_map_mu);
        map_give_back(m, a, (uint32_t)span);
        pthread_mutex_unlock(&g_map_mu);
    }
    R(c, 0);
}
static void h_zero(Mt *m, X86 *c) { (void)m; R(c, 0); }
static void h_getpagesize(Mt *m, X86 *c) { (void)m; R(c, 4096); }
static void h_mincore(Mt *m, X86 *c) {  // report everything resident
    uint32_t len = A(c, 1), vec = A(c, 2), n = (len + 4095) / 4096;
    mt_check(m, c, vec, n);
    memset(G(m, vec), 1, n);
    R(c, 0);
}

// ---------------------------------------------------------------- math

static void h_sqrt(Mt *m, X86 *c) { (void)m; RD(c, sqrt(argd(c, 0))); }
static void h_sin(Mt *m, X86 *c) { (void)m; RD(c, sin(argd(c, 0))); }
static void h_log10(Mt *m, X86 *c) { (void)m; RD(c, log10(argd(c, 0))); }
static void h_ceil(Mt *m, X86 *c) { (void)m; RD(c, ceil(argd(c, 0))); }
static void h_bcopy(Mt *m, X86 *c) { uint32_t s = A(c, 0), d = A(c, 1), n = A(c, 2); mt_check(m, c, s, n); mt_check(m, c, d, n); memmove(G(m, d), G(m, s), n); }
static void h_bzero(Mt *m, X86 *c) { uint32_t d = A(c, 0), n = A(c, 1); mt_check(m, c, d, n); memset(G(m, d), 0, n); }
static void h_isdigit(Mt *m, X86 *c) { (void)m; uint32_t v = A(c, 0); R(c, v >= '0' && v <= '9'); }
// keymgr: libgcc's per-thread / process-wide hooks (exception handling); nothing is ever stored
static void h_keymgr_zero(Mt *m, X86 *c) { (void)m; R(c, 0); }
// getsectdatafromheader(mh, seg, sect, size*): used to register __eh_frame; report none
static void h_getsectdata(Mt *m, X86 *c) { (void)m; if (A(c, 3)) wr32(c, A(c, 3), 0); R(c, 0); }
static void h_sinh(Mt *m, X86 *c) { (void)m; RD(c, sinh(argd(c, 0))); }
static void h_floor(Mt *m, X86 *c) { (void)m; RD(c, floor(argd(c, 0))); }
static void h_cos(Mt *m, X86 *c) { (void)m; RD(c, cos(argd(c, 0))); }
static void h_log(Mt *m, X86 *c) { (void)m; RD(c, log(argd(c, 0))); }
static void h_pow(Mt *m, X86 *c) { (void)m; RD(c, pow(argd(c, 0), argd(c, 2))); }
static void h_floorf(Mt *m, X86 *c) { (void)m; RD(c, floorf(argf(c, 0))); }
static void h_ceilf(Mt *m, X86 *c) { (void)m; RD(c, ceilf(argf(c, 0))); }
static void h_cosf(Mt *m, X86 *c) { (void)m; RD(c, cosf(argf(c, 0))); }
static void h_sinf(Mt *m, X86 *c) { (void)m; RD(c, sinf(argf(c, 0))); }
static void h_expf(Mt *m, X86 *c) { (void)m; RD(c, expf(argf(c, 0))); }
static void h_logf(Mt *m, X86 *c) { (void)m; RD(c, logf(argf(c, 0))); }
static void h_powf(Mt *m, X86 *c) { (void)m; RD(c, powf(argf(c, 0), argf(c, 1))); }
static void h_lrintf(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)lrintf(argf(c, 0))); }
// 64-bit integer helpers (libgcc)
static void h_divdi3(Mt *m, X86 *c) { (void)m; int64_t a = (int64_t)((uint64_t)A(c, 0) | (uint64_t)A(c, 1) << 32), b = (int64_t)((uint64_t)A(c, 2) | (uint64_t)A(c, 3) << 32); R64(c, b ? (uint64_t)(a / b) : 0); }
static void h_udivdi3(Mt *m, X86 *c) { (void)m; uint64_t a = (uint64_t)A(c, 0) | (uint64_t)A(c, 1) << 32, b = (uint64_t)A(c, 2) | (uint64_t)A(c, 3) << 32; R64(c, b ? a / b : 0); }
static void h_umoddi3(Mt *m, X86 *c) { (void)m; uint64_t a = (uint64_t)A(c, 0) | (uint64_t)A(c, 1) << 32, b = (uint64_t)A(c, 2) | (uint64_t)A(c, 3) << 32; R64(c, b ? a % b : 0); }

// random(): the engine seeds with srandom; keep a deterministic generator so
// renders are reproducible (Darwin's random() algorithm, from FreeBSD).
static uint32_t g_rand_state[34];
static int g_rand_i;
static void rand_seed(uint32_t s) {
    int32_t r[34];
    r[0] = (int32_t)s;
    for (int i = 1; i < 31; i++) {
        int64_t hi = r[i - 1] / 127773, lo = r[i - 1] % 127773;
        int64_t w = 16807 * lo - 2836 * hi;
        if (w < 0) w += 2147483647;
        r[i] = (int32_t)w;
    }
    for (int i = 31; i < 34; i++) r[i] = r[i - 31];
    for (int i = 0; i < 34; i++) g_rand_state[i] = (uint32_t)r[i];
    g_rand_i = 34;
    // discard 310 outputs as glibc/FreeBSD do
}
static uint32_t rand_next(void) {
    // additive feedback x[i] = x[i-3] + x[i-31]
    static uint32_t ring[31];
    static int f = 3, r = 0, init = 0;
    if (!init) {
        for (int i = 0; i < 31; i++) ring[i] = g_rand_state[i];
        f = 3; r = 0;
        for (int k = 0; k < 310; k++) { ring[f] += ring[r]; f = (f + 1) % 31; r = (r + 1) % 31; }
        init = 1;
    }
    ring[f] += ring[r];
    uint32_t v = ring[f] >> 1;
    f = (f + 1) % 31; r = (r + 1) % 31;
    return v;
}
static void h_srandom(Mt *m, X86 *c) { (void)m; rand_seed(A(c, 0)); }
static void h_random(Mt *m, X86 *c) { (void)m; R(c, rand_next()); }

// ---------------------------------------------------------------- process / misc

static void h_abort(Mt *m, X86 *c) { (void)m; x86_fault(c, "guest called abort()"); }
// The engine's own stack check caught it overrunning a buffer (some unusual
// input does this to Snow Leopard's Alex); on a Mac the process would end.
static void h_stack_chk_fail(Mt *m, X86 *c) { (void)m; x86_fault(c, "MacinTalk overran a buffer (its stack check fired)"); }
static void h_exit(Mt *m, X86 *c) { (void)m; x86_fault(c, "guest called exit(%d)", (int)A(c, 0)); }
static void h_assert_rtn(Mt *m, X86 *c) {
    x86_fault(c, "guest assertion failed: %s:%d %s (%s)", mt_str(m, A(c, 1)) ? mt_str(m, A(c, 1)) : "?", (int)A(c, 2),
              A(c, 3) ? mt_str(m, A(c, 3)) : "", A(c, 0) ? mt_str(m, A(c, 0)) : "");
}
static void h_getenv(Mt *m, X86 *c) { (void)m; R(c, 0); }
void h_usleep(Mt *m, X86 *c);  // mt_mp.c (virtual clock)
static void h_error(Mt *m, X86 *c) { mt_errno_slot(m, c); R(c, mt_tls(m, c)->errno_addr); }
static void h_cxa_atexit(Mt *m, X86 *c) { (void)m; R(c, 0); }
static uint32_t g_locale_name;
static void h_setlocale(Mt *m, X86 *c) {
    if (!g_locale_name) g_locale_name = mt_strdup(m, "C");
    R(c, g_locale_name);
}
static void h_mlock(Mt *m, X86 *c) { (void)m; R(c, 0); }

// ---------------------------------------------------------------- pthreads
// Guest pthread_mutex_t (44 bytes on i386) and pthread_cond_t (28) hold
// {magic, index into a host table, their own address}. The address marks
// copies: the engine copies structures holding them, and a copy is a new
// object. Each host slot records the guest object that owns it, so a destroy
// frees only its own slot, once; 10.6 makes and destroys several per utterance.

#define MAX_SYNC 1024
typedef struct {
    uint32_t magic;
    int n, nspare;
    uint32_t owner[MAX_SYNC];  // guest address using each slot, 0 if spare
    uint32_t spare[MAX_SYNC];  // initialised slots free for reuse
} SyncTable;
static pthread_mutex_t g_mutex[MAX_SYNC];
static pthread_cond_t g_cond[MAX_SYNC];
static SyncTable g_mtab = {0x4D54584Du}, g_ctab = {0x4D54434Eu};  // 'MTXM', 'MTCN'
static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;

static int sync_valid(SyncTable *t, X86 *c, uint32_t g, uint32_t *idx) {
    uint32_t i = rd32(c, g + 4);
    if (rd32(c, g) != t->magic || rd32(c, g + 8) != g || i >= MAX_SYNC || t->owner[i] != g) return 0;
    *idx = i;
    return 1;
}
// The host slot for guest object g, giving it one if it has none.
static uint32_t sync_slot(SyncTable *t, X86 *c, uint32_t g) {
    uint32_t idx;
    if (sync_valid(t, c, g, &idx)) return idx;
    pthread_mutex_lock(&g_sync_mu);
    if (!sync_valid(t, c, g, &idx)) {
        if (t->nspare) idx = t->spare[--t->nspare];
        else if (t->n < MAX_SYNC) {
            idx = (uint32_t)t->n++;
            if (t == &g_mtab) {
                pthread_mutexattr_t at;
                pthread_mutexattr_init(&at);
                pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
                pthread_mutex_init(&g_mutex[idx], &at);
            } else {
                pthread_cond_init(&g_cond[idx], NULL);
            }
        } else {
            pthread_mutex_unlock(&g_sync_mu);
            x86_fault(c, t == &g_mtab ? "too many mutexes" : "too many condition variables");
        }
        t->owner[idx] = g;
        wr32(c, g + 8, g);
        wr32(c, g + 4, idx);
        wr32(c, g, t->magic);
    }
    pthread_mutex_unlock(&g_sync_mu);
    return idx;
}
// The host object stays initialised for the next one. A mutex someone still
// holds is never reused.
static void sync_destroy(SyncTable *t, X86 *c, uint32_t g) {
    pthread_mutex_lock(&g_sync_mu);
    uint32_t idx;
    if (sync_valid(t, c, g, &idx)) {
        wr32(c, g, 0);
        t->owner[idx] = 0;
        int busy = 0;
        if (t == &g_mtab) {
            busy = pthread_mutex_trylock(&g_mutex[idx]) != 0;
            if (!busy) pthread_mutex_unlock(&g_mutex[idx]);
        }
        if (busy) t->owner[idx] = 1;  // never handed out again
        else if (t->nspare < MAX_SYNC) t->spare[t->nspare++] = idx;
    }
    pthread_mutex_unlock(&g_sync_mu);
}

static pthread_mutex_t *mutex_of(Mt *m, X86 *c, uint32_t g) { (void)m; return &g_mutex[sync_slot(&g_mtab, c, g)]; }
static pthread_cond_t *cond_of(Mt *m, X86 *c, uint32_t g) { (void)m; return &g_cond[sync_slot(&g_ctab, c, g)]; }
static void h_mutex_init(Mt *m, X86 *c) { sync_destroy(&g_mtab, c, A(c, 0)); wr32(c, A(c, 0), 0); mutex_of(m, c, A(c, 0)); R(c, 0); }
static void h_mutex_lock(Mt *m, X86 *c) { mt_lock(m, c, mutex_of(m, c, A(c, 0))); R(c, 0); }
static void h_mutex_unlock(Mt *m, X86 *c) { R(c, (uint32_t)pthread_mutex_unlock(mutex_of(m, c, A(c, 0)))); }
static void h_mutex_destroy(Mt *m, X86 *c) { (void)m; sync_destroy(&g_mtab, c, A(c, 0)); R(c, 0); }
static void h_cond_destroy(Mt *m, X86 *c) { (void)m; sync_destroy(&g_ctab, c, A(c, 0)); R(c, 0); }
static void h_cond_init(Mt *m, X86 *c) { sync_destroy(&g_ctab, c, A(c, 0)); wr32(c, A(c, 0), 0); cond_of(m, c, A(c, 0)); R(c, 0); }
static void h_cond_signal(Mt *m, X86 *c) { R(c, (uint32_t)pthread_cond_signal(cond_of(m, c, A(c, 0)))); }
static void h_cond_wait(Mt *m, X86 *c) {
    if (m->dying) x86_fault(c, "engine shut down");
    R(c, (uint32_t)pthread_cond_wait(cond_of(m, c, A(c, 0)), mutex_of(m, c, A(c, 1))));
    if (m->dying) x86_fault(c, "engine shut down");
}
static void h_once(Mt *m, X86 *c) {
    // pthread_once_t {sig, state}: run the guest routine once
    uint32_t ctl = A(c, 0), fn = A(c, 1);
    static pthread_mutex_t once_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once_mu);
    int run = rd32(c, ctl + 4) != 0x4F4E4345u;  // 'ONCE'
    if (run) wr32(c, ctl + 4, 0x4F4E4345u);
    pthread_mutex_unlock(&once_mu);
    if (run && fn) mt_call_on(m, c, fn, 0, NULL);
    R(c, 0);
}
static void h_pthread_self(Mt *m, X86 *c) { R(c, mt_tls(m, c)->self); }
// Keys are per engine: counting across engines ran Snow Leopard (a key per
// open) out after 63 restarts in one process.
static uint32_t g_next_key = 1;
static void h_key_create(Mt *m, X86 *c) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    uint32_t k = g_next_key++;
    pthread_mutex_unlock(&mu);
    if (k >= MT_TLS_KEYS) x86_fault(c, "too many pthread keys");
    wr32(c, A(c, 0), k);
    R(c, 0);
}
static void h_getspecific(Mt *m, X86 *c) { uint32_t k = A(c, 0); R(c, k < MT_TLS_KEYS ? mt_tls(m, c)->keys[k] : 0); }
static void h_setspecific(Mt *m, X86 *c) { uint32_t k = A(c, 0); if (k < MT_TLS_KEYS) mt_tls(m, c)->keys[k] = A(c, 1); R(c, 0); }

// mach: enough to say "not available"
static void h_mach_fail(Mt *m, X86 *c) { (void)m; R(c, 5); }  // KERN_FAILURE
static void h_mig_reply(Mt *m, X86 *c) { (void)m; R(c, 0); }

// wide-character functions are pulled in by libstdc++'s locale code; the
// engine never uses wide streams
static void h_wcslen(Mt *m, X86 *c) { uint32_t p = A(c, 0), n = 0; while (rd32(c, p + 4 * n)) n++; (void)m; R(c, n); }
static void h_btowc(Mt *m, X86 *c) { (void)m; uint32_t v = A(c, 0); R(c, v < 128 ? v : ~0u); }
static void h_wctob(Mt *m, X86 *c) { (void)m; uint32_t v = A(c, 0); R(c, v < 128 ? v : ~0u); }
static void h_mb_unsupported(Mt *m, X86 *c) { (void)m; x86_fault(c, "wide/multibyte conversion is not implemented"); }

void mt_libc_wake(void) { for (int i = 0; i < g_ctab.n; i++) pthread_cond_broadcast(&g_cond[i]); }
void mt_libc_reset(void) {
    for (int i = 0; i < g_mtab.n; i++) pthread_mutex_destroy(&g_mutex[i]);
    for (int i = 0; i < g_ctab.n; i++) pthread_cond_destroy(&g_cond[i]);
    g_mtab.n = g_mtab.nspare = g_ctab.n = g_ctab.nspare = 0;
    memset(g_mtab.owner, 0, sizeof g_mtab.owner);
    memset(g_ctab.owner, 0, sizeof g_ctab.owner);
    g_locale_name = 0;
    g_nfree_maps = 0;
    g_next_key = 1;
    for (int i = 3; i < MAX_FILES; i++) if (g_files[i]) { fclose(g_files[i]); g_files[i] = NULL; }
}

const MtHleEntry mt_hle_libc[] = {
    {"_malloc", h_malloc}, {"_calloc", h_calloc}, {"_free", h_free}, {"_realloc", h_realloc},
    {"_memcpy", h_memcpy}, {"_memmove", h_memcpy}, {"_memset", h_memset}, {"_memcmp", h_memcmp}, {"_memchr", h_memchr},
    {"_strlen", h_strlen}, {"_strcmp", h_strcmp}, {"_strcoll", h_strcoll}, {"_strcpy", h_strcpy}, {"_strcat", h_strcat},
    {"_strchr", h_strchr}, {"_strdup", h_strdup}, {"_strtok_r", h_strtok_r}, {"_strxfrm", h_strxfrm},
    {"___tolower", h_tolower}, {"___toupper", h_toupper}, {"___maskrune", h_maskrune},
    {"_atoi", h_atoi}, {"_atol", h_atol}, {"_atof", h_atof}, {"_strtod", h_strtod}, {"_strtof", h_strtof},
    {"_printf", h_printf}, {"_fprintf", h_fprintf}, {"_sprintf", h_sprintf}, {"_snprintf", h_snprintf},
    {"_vsprintf", h_vsprintf}, {"_vsnprintf", h_vsnprintf}, {"_vfprintf", h_vfprintf}, {"_strstr", h_strstr},
    {"_strtol", h_strtol}, {"_sinh", h_sinh}, {"_sqrt", h_sqrt}, {"_sin", h_sin}, {"_log10", h_log10}, {"_ceil", h_ceil},
    {"_bcopy", h_bcopy}, {"_bzero", h_bzero}, {"___bzero", h_bzero}, {"_isdigit", h_isdigit}, {"_madvise", h_zero},
    {"__keymgr_get_per_thread_data", h_keymgr_zero}, {"__keymgr_set_per_thread_data", h_keymgr_zero},
    {"__keymgr_unlock_processwide_ptr", h_keymgr_zero}, {"__keymgr_set_and_unlock_processwide_ptr", h_keymgr_zero},
    {"__keymgr_get_and_lock_processwide_ptr", h_keymgr_zero}, {"_getsectdatafromheader", h_getsectdata}, {"_puts", h_puts}, {"_putchar", h_putchar}, {"_fputs", h_fputs}, {"_fputc", h_fputc},
    {"_putc", h_fputc}, {"_fwrite", h_fwrite}, {"_fflush", h_fflush}, {"_fopen", h_fopen}, {"_fclose", h_fclose},
    {"_fread", h_fread}, {"_fseek", h_fseek}, {"_ftell", h_ftell}, {"_getc", h_getc}, {"_ungetc", h_ungetc},
    {"_fileno", h_fileno}, {"_setvbuf", h_zero},
    {"_open", h_open}, {"_close", h_close}, {"_read", h_read}, {"_pread", h_pread}, {"_write", h_write},
    {"_lseek", h_lseek}, {"_fcntl", h_fcntl}, {"_ioctl", h_ioctl}, {"_stat", h_stat}, {"_fstat", h_fstat},
    {"_stat$INODE64", h_stat64}, {"_fstat$INODE64", h_fstat64}, {"_lstat$INODE64", h_stat64}, {"_stat64", h_stat64}, {"_fstat64", h_fstat64},
    {"_mmap", h_mmap}, {"_munmap", h_munmap}, {"_mlock", h_mlock}, {"_munlock", h_zero}, {"_posix_madvise", h_zero},
    {"_mincore", h_mincore}, {"_getpagesize", h_getpagesize},
    {"_floor", h_floor}, {"_cos", h_cos}, {"_log", h_log}, {"_pow", h_pow}, {"_floorf", h_floorf},
    {"_ceilf", h_ceilf}, {"_cosf", h_cosf}, {"_sinf", h_sinf}, {"_expf", h_expf}, {"_logf", h_logf},
    {"_powf", h_powf}, {"_lrintf", h_lrintf},
    {"___divdi3", h_divdi3}, {"___udivdi3", h_udivdi3}, {"___umoddi3", h_umoddi3},
    {"_srandom", h_srandom}, {"_random", h_random},
    {"_abort", h_abort}, {"___stack_chk_fail", h_stack_chk_fail}, {"_exit", h_exit}, {"___assert_rtn", h_assert_rtn}, {"_getenv", h_getenv},
    {"_usleep", h_usleep}, {"___error", h_error}, {"___cxa_atexit", h_cxa_atexit}, {"_setlocale", h_setlocale},
    {"_pthread_mutex_init", h_mutex_init}, {"_pthread_mutex_lock", h_mutex_lock}, {"_pthread_mutex_unlock", h_mutex_unlock},
    {"_pthread_mutex_destroy", h_mutex_destroy}, {"_pthread_mutexattr_init", h_zero}, {"_pthread_mutexattr_destroy", h_zero},
    {"_pthread_mutexattr_settype", h_zero}, {"_pthread_cond_init", h_cond_init}, {"_pthread_cond_destroy", h_cond_destroy},
    {"_pthread_cond_signal", h_cond_signal}, {"_pthread_cond_wait", h_cond_wait}, {"_pthread_once", h_once},
    {"_pthread_self", h_pthread_self}, {"_pthread_key_create", h_key_create},
    {"_pthread_getspecific", h_getspecific}, {"_pthread_setspecific", h_setspecific},
    {"_pthread_mach_thread_np", h_zero},
    {"_mach_host_self", h_zero}, {"_host_get_io_master", h_mach_fail}, {"_mach_msg", h_mach_fail},
    {"_mach_port_deallocate", h_zero}, {"_mach_port_destroy", h_zero}, {"_mig_get_reply_port", h_mig_reply},
    {"_wcslen", h_wcslen}, {"_btowc", h_btowc}, {"_wctob", h_wctob},
    {"_mbrtowc", h_mb_unsupported}, {"_wcrtomb", h_mb_unsupported}, {"_getwc", h_mb_unsupported},
    {"_putwc", h_mb_unsupported}, {"_ungetwc", h_mb_unsupported},
    {NULL, NULL},
};
