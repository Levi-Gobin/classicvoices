// Loader, libc HLE, and Vocalizer driver. See NOTES.md for the reverse-engineered API.
#include "vxengine.h"
#include "thumb.h"

#include <ctype.h>
#include <errno.h>
#include <runetype.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- guest layout
#define TRAP_LO        0x00300000u
#define TRAP_HI        0x00310000u
#define RETURN_TRAP    0x0030FFFCu
#define TRAP_OUTNOTIFY 0x0030F000u
#define TRAP_MAPPING   0x0030F100u  // 5 consecutive slots
#define DATA_SYMS      0x00380000u
#define MAP_LO         0x04000000u  // file mappings: 960 MB (Mac premium voices reach ~750 MB)
#define MAP_HI         GUEST_MEM_SIZE
#define HEAP_LO        0x01400000u
#define HEAP_HI        0x03C00000u
#define STACK_TOP      0x04000000u

#define MAX_IMPORTS    1024
#define MAX_FILES      64
#define MAX_MAPS       32
#define HEAP_BUCKETS   256  // exact-size free lists for blocks <= 4096 bytes

// Per-firmware facts about voiced (see NOTES.md). iOS 6 exports the engine API
// by name; iOS 3's binary is stripped, so its addresses are fixed here.
typedef struct {
    const char *name;
    uint32_t stub_addr;                       // identifies the build
    uint32_t iface_heap, iface_critsec, iface_stream;
    uint32_t fn_initialize, fn_open, fn_setparams, fn_setoutdevice, fn_process;  // 0 = from symbols
    int rate_khz;
    int split_broker;  // iOS 3: common/broker.hdr + per-language broker.hdr, language from pipeline string
} Build;

static const Build kBuilds[] = {
    {"iOS 10.3.4 (14G61)", 0x263AA4, 0x264958, 0x264968, 0x264978, 0, 0, 0, 0, 0, 22, 0},
    {"iOS 7.1.2 (11D257)", 0x27F968, 0x281018, 0x281028, 0x281038, 0, 0, 0, 0, 0, 22, 0},
    {"iOS 6.1.6 (10B500)", 0x206A54, 0x208B08, 0x208B18, 0x208B28, 0, 0, 0, 0, 0, 22, 0},
    {"iOS 5.1.1 (9B206)", 0x202A7C, 0x204A34, 0x204A44, 0x204A60, 0, 0, 0, 0, 0, 22, 0},
    {"iOS 3.1.3 (7E18)", 0x18FAB0, 0x190C54, 0x190C44, 0x190C28,
     0xC0510, 0xBF914, 0xBECE0, 0xBDBB0, 0xBF61C, 16, 1},
    {"iOS 4.3.5 (8L1)", 0x1C2A70, 0x1C4968, 0x1C4958, 0x1C493C,
     0xC9618, 0xC90F0, 0xCA98C, 0xC83B0, 0xCA738, 16, 1},
};

#define VE_MSG_OUTBUFREQ  0x8u
#define VE_MSG_OUTBUFDONE 0x10u
#define VE_USER_STOP      0x80000200u

typedef void (*Hle)(Vx *vx);

typedef struct {
    char path[1024];
    uint32_t guest;
    uint32_t size;
    int refs;
} Mapping;

struct Vx {
    Cpu cpu;
    uint8_t *mem;
    jmp_buf fault;
    char err[512];

    Hle hle[MAX_IMPORTS];
    const char *hle_name[MAX_IMPORTS];
    int nimports;
    uint32_t data_sym_next;
    uint32_t errno_addr;
    uint32_t strerror_buf;

    // heap
    uint32_t heap_next;
    uint32_t free_small[HEAP_BUCKETS + 1];
    uint32_t free_large;  // singly linked, first fit

    // files
    FILE *files[MAX_FILES];
    int fds[MAX_FILES];
    uint32_t map_next;
    Mapping maps[MAX_MAPS];

    // pthread TLS / keys
    uint32_t tls[64];
    uint32_t next_key;

    // engine
    uint32_t fn_initialize, fn_open, fn_setparams, fn_setoutdevice, fn_process;
    const Build *build;
    uint32_t hspeech, hinst;  // guest addresses of 2-word handles
    uint32_t pcm_buf, pcm_buf_size, mark_buf;
    char dirs[2][1024];
    char voice[64];
    char language[64];
    VxPcmCallback cb;
    void *cb_ctx;
    int cancelled;
};

// ---------------------------------------------------------------- small utils

static inline uint32_t arg(Vx *vx, int i) {
    Cpu *c = &vx->cpu;
    return i < 4 ? c->k.r[i] : g32(&c->k, c->k.r[13] + 4 * (i - 4));
}
static inline void ret(Vx *vx, uint32_t v) { vx->cpu.k.r[0] = v; }
static inline void ret64(Vx *vx, uint64_t v) { vx->cpu.k.r[0] = (uint32_t)v; vx->cpu.k.r[1] = (uint32_t)(v >> 32); }

static const char *gstr(Vx *vx, uint32_t a) {
    if (!a || a >= GUEST_MEM_SIZE) return NULL;
    // guest strings are NUL-terminated within guest memory
    if (!memchr(vx->mem + a, 0, GUEST_MEM_SIZE - a)) cpu_fault(&vx->cpu, "unterminated string at %#x", a);
    return (const char *)vx->mem + a;
}

// ---------------------------------------------------------------- heap
// Header word at block-8 holds the rounded size.

static uint32_t round_size(uint32_t n) { n = n < 16 ? 16 : n; return (n + 15) & ~15u; }

static uint32_t heap_alloc(Vx *vx, uint32_t n) {
    n = round_size(n);
    Cpu *c = &vx->cpu;
    uint32_t a = 0;
    if (n <= HEAP_BUCKETS * 16) {
        uint32_t *head = &vx->free_small[n / 16];
        if (*head) { a = *head; *head = g32(&c->k, a); }
    } else {
        uint32_t prev = 0, cur = vx->free_large;
        while (cur) {
            uint32_t next = g32(&c->k, cur);
            if (g32(&c->k, cur - 8) == n) {
                if (prev) s32(&c->k, prev, next); else vx->free_large = next;
                a = cur;
                break;
            }
            prev = cur;
            cur = next;
        }
    }
    if (!a) {
        a = vx->heap_next + 16;
        if (a + n > HEAP_HI) cpu_fault(c, "guest heap exhausted (request %u)", n);
        vx->heap_next = a + n;
        s32(&c->k, a - 8, n);
    }
    return a;
}

static void heap_free(Vx *vx, uint32_t a) {
    if (!a) return;
    Cpu *c = &vx->cpu;
    if (a < HEAP_LO || a >= HEAP_HI) return;  // not ours (e.g. mmap region)
    uint32_t n = g32(&c->k, a - 8);
    if (n <= HEAP_BUCKETS * 16) {
        s32(&c->k, a, vx->free_small[n / 16]);
        vx->free_small[n / 16] = a;
    } else {
        s32(&c->k, a, vx->free_large);
        vx->free_large = a;
    }
}

static uint32_t heap_size(Vx *vx, uint32_t a) {
    return (a >= HEAP_LO && a < HEAP_HI) ? g32(&vx->cpu.k, a - 8) : 0;
}

static uint32_t new_cstr(Vx *vx, const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t a = heap_alloc(vx, (uint32_t)n);
    memcpy(vx->mem + a, s, n);
    return a;
}

static uint32_t new_zeroed(Vx *vx, uint32_t n) {
    uint32_t a = heap_alloc(vx, n);
    memset(vx->mem + a, 0, n);
    return a;
}

// ---------------------------------------------------------------- libc HLE

static void h_malloc(Vx *vx) { ret(vx, heap_alloc(vx, arg(vx, 0))); }
static void h_calloc(Vx *vx) { ret(vx, new_zeroed(vx, arg(vx, 0) * arg(vx, 1))); }
static void h_free(Vx *vx) { heap_free(vx, arg(vx, 0)); }
static void h_malloc_size(Vx *vx) { ret(vx, heap_size(vx, arg(vx, 0))); }
static void h_realloc(Vx *vx) {
    uint32_t old = arg(vx, 0), n = arg(vx, 1);
    uint32_t a = heap_alloc(vx, n);
    if (old) {
        uint32_t on = heap_size(vx, old);
        memmove(vx->mem + a, vx->mem + old, on < n ? on : n);
        heap_free(vx, old);
    }
    ret(vx, a);
}

static void check_range(Vx *vx, uint32_t a, uint32_t n) {
    if (n && ((uint64_t)a + n > GUEST_MEM_SIZE || a < 0x1000))
        cpu_fault(&vx->cpu, "bad range %#x+%u (lr=%#x)", a, n, vx->cpu.k.r[14]);
}
static void h_memcpy(Vx *vx) {
    uint32_t d = arg(vx, 0), s = arg(vx, 1), n = arg(vx, 2);
    check_range(vx, d, n); check_range(vx, s, n);
    memmove(vx->mem + d, vx->mem + s, n);
    ret(vx, d);
}
static void h_memset(Vx *vx) {
    uint32_t d = arg(vx, 0), n = arg(vx, 2);
    check_range(vx, d, n);
    memset(vx->mem + d, (int)arg(vx, 1), n);
    ret(vx, d);
}
static void h_memset_pattern16(Vx *vx) {
    uint32_t d = arg(vx, 0), p = arg(vx, 1), n = arg(vx, 2);
    check_range(vx, d, n);
    uint8_t pat[16];
    memcpy(pat, vx->mem + p, 16);
    for (uint32_t i = 0; i < n; i++) vx->mem[d + i] = pat[i & 15];
}
static void h_memcmp(Vx *vx) {
    uint32_t a = arg(vx, 0), b = arg(vx, 1), n = arg(vx, 2);
    check_range(vx, a, n); check_range(vx, b, n);
    int r = memcmp(vx->mem + a, vx->mem + b, n);
    ret(vx, (uint32_t)(r < 0 ? -1 : r > 0));
}
static void h_memchr(Vx *vx) {
    uint32_t s = arg(vx, 0), n = arg(vx, 2);
    check_range(vx, s, n);
    uint8_t *p = memchr(vx->mem + s, (int)(arg(vx, 1) & 0xff), n);
    ret(vx, p ? (uint32_t)(p - vx->mem) : 0);
}
static void h_strlen(Vx *vx) { ret(vx, (uint32_t)strlen(gstr(vx, arg(vx, 0)))); }
static void h_strcmp(Vx *vx) {
    int r = strcmp(gstr(vx, arg(vx, 0)), gstr(vx, arg(vx, 1)));
    ret(vx, (uint32_t)(r < 0 ? -1 : r > 0));
}
static void h_strcpy(Vx *vx) {
    uint32_t d = arg(vx, 0);
    const char *s = gstr(vx, arg(vx, 1));
    size_t n = strlen(s) + 1;
    check_range(vx, d, (uint32_t)n);
    memmove(vx->mem + d, s, n);
    ret(vx, d);
}
static void h_strlcpy(Vx *vx) {
    uint32_t d = arg(vx, 0), n = arg(vx, 2);
    const char *s = gstr(vx, arg(vx, 1));
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        check_range(vx, d, (uint32_t)k + 1);
        memmove(vx->mem + d, s, k);
        vx->mem[d + k] = 0;
    }
    ret(vx, (uint32_t)len);
}

static void h_stack_chk_fail(Vx *vx) { cpu_fault(&vx->cpu, "__stack_chk_fail (lr=%#x)", vx->cpu.k.r[14]); }
static void h_abort(Vx *vx) { cpu_fault(&vx->cpu, "abort() (lr=%#x)", vx->cpu.k.r[14]); }
static void h_assert_rtn(Vx *vx) {
    cpu_fault(&vx->cpu, "assert %s %s:%u %s", gstr(vx, arg(vx, 0)), gstr(vx, arg(vx, 1)), arg(vx, 2),
              gstr(vx, arg(vx, 3)));
}
static void h_exit(Vx *vx) { cpu_fault(&vx->cpu, "exit(%d)", (int)arg(vx, 0)); }

// Cortex-A8 has no hardware divide; these are compiler runtime helpers.
static void h_divsi3(Vx *vx) {
    int32_t a = (int32_t)arg(vx, 0), b = (int32_t)arg(vx, 1);
    ret(vx, b == 0 ? 0 : (b == -1 ? (uint32_t)(-(int64_t)a) : (uint32_t)(a / b)));
}
static void h_modsi3(Vx *vx) {
    int32_t a = (int32_t)arg(vx, 0), b = (int32_t)arg(vx, 1);
    ret(vx, b == 0 || b == -1 ? 0 : (uint32_t)(a % b));
}
static void h_udivsi3(Vx *vx) { uint32_t b = arg(vx, 1); ret(vx, b ? arg(vx, 0) / b : 0); }
static void h_umodsi3(Vx *vx) { uint32_t b = arg(vx, 1); ret(vx, b ? arg(vx, 0) % b : 0); }
// iOS 7: quotient returned, remainder stored through r2
static void h_divmodsi4(Vx *vx) {
    int32_t a = (int32_t)arg(vx, 0), b = (int32_t)arg(vx, 1);
    int32_t q = b == 0 ? 0 : (b == -1 ? (int32_t)(uint32_t)(-(int64_t)a) : a / b);
    s32(&vx->cpu.k, arg(vx, 2), (uint32_t)(a - (int32_t)((uint32_t)q * (uint32_t)b)));
    ret(vx, (uint32_t)q);
}
static void h_udivmodsi4(Vx *vx) {
    uint32_t a = arg(vx, 0), b = arg(vx, 1), q = b ? a / b : 0;
    s32(&vx->cpu.k, arg(vx, 2), a - q * b);
    ret(vx, q);
}
static void h_divdi3(Vx *vx) {
    int64_t a = (int64_t)(((uint64_t)arg(vx, 1) << 32) | arg(vx, 0));
    int64_t b = (int64_t)(((uint64_t)arg(vx, 3) << 32) | arg(vx, 2));
    ret64(vx, b == 0 ? 0 : (b == -1 ? (uint64_t)0 - (uint64_t)a : (uint64_t)(a / b)));
}

// softfp: doubles in r0:r1
static double darg(Vx *vx, int i) {
    uint64_t u = (uint64_t)arg(vx, i) | ((uint64_t)arg(vx, i + 1) << 32);
    double d; memcpy(&d, &u, 8); return d;
}
static void dret(Vx *vx, double d) { uint64_t u; memcpy(&u, &d, 8); ret64(vx, u); }
static void h_floor(Vx *vx) { dret(vx, floor(darg(vx, 0))); }
static void h_roundf(Vx *vx) {
    uint32_t u = arg(vx, 0); float f; memcpy(&f, &u, 4);
    f = roundf(f); memcpy(&u, &f, 4); ret(vx, u);
}
static void h_fixdfdi(Vx *vx) { ret64(vx, (uint64_t)(int64_t)darg(vx, 0)); }
static void h_floatundidf(Vx *vx) { dret(vx, (double)((uint64_t)arg(vx, 0) | ((uint64_t)arg(vx, 1) << 32))); }

static void h_error(Vx *vx) { ret(vx, vx->errno_addr); }
static void set_errno(Vx *vx, int e) { s32(&vx->cpu.k, vx->errno_addr, (uint32_t)e); }
static void h_zero(Vx *vx) { ret(vx, 0); }
static void h_getpagesize(Vx *vx) { ret(vx, 4096); }
static void h_pthread_self(Vx *vx) { ret(vx, 0x1234); }
static void h_times(Vx *vx) {
    if (arg(vx, 0)) memset(vx->mem + arg(vx, 0), 0, 16);
    ret(vx, 0);
}
static void h_ftime(Vx *vx) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t a = arg(vx, 0);
    s32(&vx->cpu.k, a, (uint32_t)ts.tv_sec);
    s16(&vx->cpu.k, a + 4, (uint32_t)(ts.tv_nsec / 1000000));
    s16(&vx->cpu.k, a + 6, 0);
    s16(&vx->cpu.k, a + 8, 0);
    ret(vx, 0);
}

static void h_pthread_key_create(Vx *vx) {
    uint32_t k = ++vx->next_key;
    if (k >= 64) cpu_fault(&vx->cpu, "too many pthread keys");
    s32(&vx->cpu.k, arg(vx, 0), k);
    ret(vx, 0);
}
static void h_pthread_getspecific(Vx *vx) { uint32_t k = arg(vx, 0); ret(vx, k < 64 ? vx->tls[k] : 0); }
static void h_pthread_setspecific(Vx *vx) {
    uint32_t k = arg(vx, 0);
    if (k < 64) vx->tls[k] = arg(vx, 1);
    ret(vx, 0);
}
static void h_pthread_once(Vx *vx) {
    uint32_t once = arg(vx, 0), fn = arg(vx, 1);
    if (g32(&vx->cpu.k, once + 4) != 0xD0E) {
        s32(&vx->cpu.k, once + 4, 0xD0E);
        cpu_call(&vx->cpu, fn, NULL, 0, NULL);
    }
    ret(vx, 0);
}

// setjmp/longjmp: r4-r11, sp, lr; resumption happens via the trap's `bx lr`.
static void h_setjmp(Vx *vx) {
    Cpu *c = &vx->cpu;
    uint32_t b = arg(vx, 0);
    for (int i = 0; i < 8; i++) s32(&c->k, b + 4 * i, c->k.r[4 + i]);
    s32(&c->k, b + 32, c->k.r[13]);
    s32(&c->k, b + 36, c->k.r[14]);
    ret(vx, 0);
}
static void h_longjmp(Vx *vx) {
    Cpu *c = &vx->cpu;
    uint32_t b = arg(vx, 0), v = arg(vx, 1);
    for (int i = 0; i < 8; i++) c->k.r[4 + i] = g32(&c->k, b + 4 * i);
    c->k.r[13] = g32(&c->k, b + 32);
    c->k.r[14] = g32(&c->k, b + 36);
    ret(vx, v ? v : 1);
}

// qsort with a guest comparator: stable merge sort over a host copy.
static void h_qsort(Vx *vx) {
    uint32_t base = arg(vx, 0), n = arg(vx, 1), sz = arg(vx, 2), cmp = arg(vx, 3);
    if (n < 2 || !sz) return;
    check_range(vx, base, n * sz);
    uint8_t *items = malloc((size_t)n * sz), *tmp = malloc((size_t)n * sz);
    memcpy(items, vx->mem + base, (size_t)n * sz);
    uint32_t ga = heap_alloc(vx, sz), gb = heap_alloc(vx, sz);
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            uint32_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                memcpy(vx->mem + ga, items + (size_t)i * sz, sz);
                memcpy(vx->mem + gb, items + (size_t)j * sz, sz);
                uint32_t args[2] = {ga, gb};
                int32_t r = (int32_t)cpu_call(&vx->cpu, cmp, args, 2, NULL);
                if (r <= 0) memcpy(tmp + (size_t)k++ * sz, items + (size_t)i++ * sz, sz);
                else memcpy(tmp + (size_t)k++ * sz, items + (size_t)j++ * sz, sz);
            }
            while (i < mid) memcpy(tmp + (size_t)k++ * sz, items + (size_t)i++ * sz, sz);
            while (j < hi) memcpy(tmp + (size_t)k++ * sz, items + (size_t)j++ * sz, sz);
        }
        uint8_t *t = items; items = tmp; tmp = t;
    }
    memcpy(vx->mem + base, items, (size_t)n * sz);
    heap_free(vx, ga);
    heap_free(vx, gb);
    free(items);
    free(tmp);
}

// ---- printf family over guest varargs
typedef struct { Vx *vx; int idx; uint32_t va; } VaCursor;
static uint32_t va_next(VaCursor *v) {
    if (v->va) { uint32_t x = g32(&v->vx->cpu.k, v->va); v->va += 4; return x; }
    return arg(v->vx, v->idx++);
}

static size_t guest_format(Vx *vx, char *out, size_t cap, const char *fmt, VaCursor *va) {
    size_t o = 0;
#define PUT(s, n) do { size_t _n = (n); if (o + _n < cap) memcpy(out + o, s, _n); o += _n; } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(fmt, 1); fmt++; continue; }
        const char *start = fmt++;
        char spec[32];
        while (*fmt && strchr("-+ #0123456789.", *fmt)) fmt++;
        int longs = 0;
        while (*fmt && strchr("lhzjtq", *fmt)) { if (*fmt == 'l' || *fmt == 'q') longs++; fmt++; }
        char conv = *fmt ? *fmt++ : '%';
        size_t flen = 0;
        for (const char *p = start; p < fmt - 1; p++)
            if (!strchr("lhzjtq", *p) && flen < sizeof spec - 3) spec[flen++] = *p;
        char buf[512];
        int n = 0;
        if (conv == '%') { PUT("%", 1); continue; }
        if (conv == 's') {
            spec[flen++] = 's'; spec[flen] = 0;
            const char *s = gstr(vx, va_next(va));
            n = snprintf(buf, sizeof buf, spec, s ? s : "(null)");
        } else if (strchr("dixXuoc", conv)) {
            spec[flen++] = 'l'; spec[flen++] = 'l'; spec[flen++] = conv; spec[flen] = 0;
            uint64_t v = va_next(va);
            if (longs >= 2) v |= (uint64_t)va_next(va) << 32;
            else if (conv == 'd' || conv == 'i') v = (uint64_t)(int64_t)(int32_t)v;
            n = snprintf(buf, sizeof buf, spec, v);
        } else if (conv == 'p') {
            n = snprintf(buf, sizeof buf, "0x%x", va_next(va));
        } else if (strchr("fgeE", conv)) {
            spec[flen++] = conv; spec[flen] = 0;
            uint64_t u = va_next(va); u |= (uint64_t)va_next(va) << 32;
            double d; memcpy(&d, &u, 8);
            n = snprintf(buf, sizeof buf, spec, d);
        } else {
            buf[0] = '?'; n = 1;
        }
        if (n > 0) PUT(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o;
#undef PUT
}

static void put_guest(Vx *vx, uint32_t dst, uint32_t cap, const char *s, size_t len) {
    if (!cap) return;
    size_t k = len < cap - 1 ? len : cap - 1;
    check_range(vx, dst, (uint32_t)k + 1);
    memcpy(vx->mem + dst, s, k);
    vx->mem[dst + k] = 0;
}
static void h_vsnprintf(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 0, arg(vx, 3)};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 2)), &va);
    put_guest(vx, arg(vx, 0), arg(vx, 1), buf, n);
    ret(vx, (uint32_t)n);
}
static void h_vsnprintf_chk(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 0, arg(vx, 5)};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 4)), &va);
    if (getenv("VX_DEBUG")) fprintf(stderr, "[vsnprintf] %.*s\n", (int)n, buf);
    put_guest(vx, arg(vx, 0), arg(vx, 1), buf, n);
    ret(vx, (uint32_t)n);
}
static void h_snprintf_chk(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 5, 0};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 4)), &va);
    put_guest(vx, arg(vx, 0), arg(vx, 1), buf, n);
    ret(vx, (uint32_t)n);
}
static void h_sprintf_chk(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 4, 0};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 3)), &va);
    put_guest(vx, arg(vx, 0), sizeof buf, buf, n);
    ret(vx, (uint32_t)n);
}
static void h_printf(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 1, 0};
    guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 0)), &va);
    if (getenv("VX_DEBUG")) fprintf(stderr, "[engine] %s", buf);
    ret(vx, 0);
}
static void h_putchar(Vx *vx) { ret(vx, arg(vx, 0)); }

// ---- more string / ctype / number helpers (used by the iOS 3 engine)
static void h_strncmp(Vx *vx) {
    int r = strncmp(gstr(vx, arg(vx, 0)), gstr(vx, arg(vx, 1)), arg(vx, 2));
    ret(vx, (uint32_t)(r < 0 ? -1 : r > 0));
}
static void h_strchr(Vx *vx) {
    uint32_t a = arg(vx, 0);
    const char *s = gstr(vx, a), *p = strchr(s, (int)(arg(vx, 1) & 0xff));
    ret(vx, p ? a + (uint32_t)(p - s) : 0);
}
static void h_strrchr(Vx *vx) {
    uint32_t a = arg(vx, 0);
    const char *s = gstr(vx, a), *p = strrchr(s, (int)(arg(vx, 1) & 0xff));
    ret(vx, p ? a + (uint32_t)(p - s) : 0);
}
static void h_strstr(Vx *vx) {
    uint32_t a = arg(vx, 0);
    const char *s = gstr(vx, a), *p = strstr(s, gstr(vx, arg(vx, 1)));
    ret(vx, p ? a + (uint32_t)(p - s) : 0);
}
static void h_strpbrk(Vx *vx) {
    uint32_t a = arg(vx, 0);
    const char *s = gstr(vx, a), *p = strpbrk(s, gstr(vx, arg(vx, 1)));
    ret(vx, p ? a + (uint32_t)(p - s) : 0);
}
static void h_strspn(Vx *vx) { ret(vx, (uint32_t)strspn(gstr(vx, arg(vx, 0)), gstr(vx, arg(vx, 1)))); }
static void h_strcat(Vx *vx) {
    uint32_t d = arg(vx, 0);
    const char *src = gstr(vx, arg(vx, 1));
    size_t dl = strlen(gstr(vx, d)), sl = strlen(src);
    check_range(vx, d, (uint32_t)(dl + sl + 1));
    memmove(vx->mem + d + dl, src, sl + 1);
    ret(vx, d);
}
static void h_strncat(Vx *vx) {
    uint32_t d = arg(vx, 0), n = arg(vx, 2);
    const char *src = gstr(vx, arg(vx, 1));
    size_t dl = strlen(gstr(vx, d)), sl = strnlen(src, n);
    check_range(vx, d, (uint32_t)(dl + sl + 1));
    memmove(vx->mem + d + dl, src, sl);
    vx->mem[d + dl + sl] = 0;
    ret(vx, d);
}
static void h_strncpy(Vx *vx) {
    uint32_t d = arg(vx, 0), n = arg(vx, 2);
    const char *src = gstr(vx, arg(vx, 1));
    size_t sl = strnlen(src, n);
    check_range(vx, d, n);
    memmove(vx->mem + d, src, sl);
    memset(vx->mem + d + sl, 0, n - sl);
    ret(vx, d);
}
static void h_strtol_common(Vx *vx, int is_unsigned) {
    uint32_t a = arg(vx, 0), endp = arg(vx, 1);
    const char *s = gstr(vx, a);
    char *end;
    uint32_t v = is_unsigned ? (uint32_t)strtoul(s, &end, (int)arg(vx, 2))
                             : (uint32_t)(int32_t)strtol(s, &end, (int)arg(vx, 2));
    if (endp) s32(&vx->cpu.k, endp, a + (uint32_t)(end - s));
    ret(vx, v);
}
static void h_strtol(Vx *vx) { h_strtol_common(vx, 0); }
static void h_strtoul(Vx *vx) { h_strtol_common(vx, 1); }
static void h_atoi(Vx *vx) { ret(vx, (uint32_t)atoi(gstr(vx, arg(vx, 0)))); }
static void h_tolower(Vx *vx) { int c = (int)arg(vx, 0); ret(vx, (uint32_t)(c >= 'A' && c <= 'Z' ? c + 32 : c)); }
static void h_toupper(Vx *vx) { int c = (int)arg(vx, 0); ret(vx, (uint32_t)(c >= 'a' && c <= 'z' ? c - 32 : c)); }
static void h_maskrune(Vx *vx) {
    uint32_t c = arg(vx, 0), mask = arg(vx, 1);
    ret(vx, c < 256 ? (uint32_t)(_DefaultRuneLocale.__runetype[c] & mask) : 0);
}
static void h_lroundf(Vx *vx) {
    uint32_t u = arg(vx, 0); float f; memcpy(&f, &u, 4);
    ret(vx, (uint32_t)(int32_t)lroundf(f));
}
static void h_sysconf(Vx *vx) { ret(vx, 4096); }
static void h_strerror(Vx *vx) {
    if (!vx->strerror_buf) vx->strerror_buf = new_cstr(vx, "error");
    ret(vx, vx->strerror_buf);
}
static void h_sprintf(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 2, 0};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 1)), &va);
    put_guest(vx, arg(vx, 0), sizeof buf, buf, n);
    ret(vx, (uint32_t)n);
}
static void h_vsprintf(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 0, arg(vx, 2)};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 1)), &va);
    put_guest(vx, arg(vx, 0), sizeof buf, buf, n);
    ret(vx, (uint32_t)n);
}
static void h_snprintf(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 3, 0};
    size_t n = guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 2)), &va);
    put_guest(vx, arg(vx, 0), arg(vx, 1), buf, n);
    ret(vx, (uint32_t)n);
}
static void h_fprintf(Vx *vx) {
    char buf[4096];
    VaCursor va = {vx, 2, 0};
    guest_format(vx, buf, sizeof buf, gstr(vx, arg(vx, 1)), &va);
    if (getenv("VX_DEBUG")) fprintf(stderr, "[engine] %s", buf);
    ret(vx, 0);
}

// ---- files (guest FILE* / fd are small handles into host tables)
static int new_slot(Vx *vx) {
    for (int i = 3; i < MAX_FILES; i++)
        if (!vx->files[i] && vx->fds[i] < 0) return i;
    cpu_fault(&vx->cpu, "out of file slots");
}
static void h_fopen(Vx *vx) {
    const char *path = gstr(vx, arg(vx, 0)), *mode = gstr(vx, arg(vx, 1));
    if (strchr(mode, 'w') || strchr(mode, 'a')) { set_errno(vx, EACCES); ret(vx, 0); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { set_errno(vx, errno); ret(vx, 0); return; }
    int slot = new_slot(vx);
    vx->files[slot] = f;
    uint32_t g = new_zeroed(vx, 0x60);
    s32(&vx->cpu.k, g, (uint32_t)slot);
    ret(vx, g);
}
static FILE *gfile(Vx *vx, uint32_t g) {
    uint32_t slot = g ? g32(&vx->cpu.k, g) : 0;
    if (slot >= MAX_FILES || !vx->files[slot]) cpu_fault(&vx->cpu, "bad FILE* %#x", g);
    return vx->files[slot];
}
static void h_fread(Vx *vx) {
    uint32_t buf = arg(vx, 0), sz = arg(vx, 1), n = arg(vx, 2);
    check_range(vx, buf, sz * n);
    ret(vx, sz ? (uint32_t)fread(vx->mem + buf, sz, n, gfile(vx, arg(vx, 3))) : 0);
}
static void h_fseek(Vx *vx) { ret(vx, (uint32_t)fseek(gfile(vx, arg(vx, 0)), (int32_t)arg(vx, 1), (int)arg(vx, 2))); }
static void h_ftell(Vx *vx) { ret(vx, (uint32_t)ftell(gfile(vx, arg(vx, 0)))); }
static void h_ferror(Vx *vx) { ret(vx, (uint32_t)ferror(gfile(vx, arg(vx, 0)))); }
static void h_fileno(Vx *vx) { ret(vx, g32(&vx->cpu.k, arg(vx, 0))); }
static void h_fwrite(Vx *vx) { ret(vx, arg(vx, 2)); }
static void h_fclose(Vx *vx) {
    uint32_t g = arg(vx, 0);
    uint32_t slot = g32(&vx->cpu.k, g);
    if (slot < MAX_FILES && vx->files[slot]) { fclose(vx->files[slot]); vx->files[slot] = NULL; }
    heap_free(vx, g);
    ret(vx, 0);
}
static void h_open(Vx *vx) {
    int fd = open(gstr(vx, arg(vx, 0)), O_RDONLY);
    if (fd < 0) { set_errno(vx, errno); ret(vx, 0xFFFFFFFF); return; }
    int slot = new_slot(vx);
    vx->fds[slot] = fd;
    ret(vx, (uint32_t)slot);
}
static int host_fd(Vx *vx, uint32_t slot) {
    if (slot >= MAX_FILES) return -1;
    if (vx->fds[slot] >= 0) return vx->fds[slot];
    if (vx->files[slot]) return fileno(vx->files[slot]);
    return -1;
}
static void h_close(Vx *vx) {
    uint32_t slot = arg(vx, 0);
    if (slot < MAX_FILES && vx->fds[slot] >= 0) { close(vx->fds[slot]); vx->fds[slot] = -1; }
    ret(vx, 0);
}
static void fill_stat(Vx *vx, uint32_t buf, const struct stat *st) {
    // iOS 6 armv7 `struct stat` (64-bit inode layout): st_mode @4, st_size @60
    check_range(vx, buf, 108);
    memset(vx->mem + buf, 0, 108);
    s16(&vx->cpu.k, buf + 4, st->st_mode);
    uint64_t size = (uint64_t)st->st_size;
    memcpy(vx->mem + buf + 60, &size, 8);
}
static void h_stat(Vx *vx) {
    struct stat st;
    if (stat(gstr(vx, arg(vx, 0)), &st) != 0) { set_errno(vx, errno); ret(vx, 0xFFFFFFFF); return; }
    fill_stat(vx, arg(vx, 1), &st);
    ret(vx, 0);
}
static void h_fstat(Vx *vx) {
    struct stat st;
    int fd = host_fd(vx, arg(vx, 0));
    if (fd < 0 || fstat(fd, &st) != 0) { set_errno(vx, EBADF); ret(vx, 0xFFFFFFFF); return; }
    fill_stat(vx, arg(vx, 1), &st);
    ret(vx, 0);
}

// Map a host file into the guest map region with MAP_FIXED: file-backed,
// clean pages, so voice data never counts as dirty memory.
static uint32_t map_file_fd(Vx *vx, int fd, uint64_t off, uint32_t len) {
    size_t page = (size_t)getpagesize();
    uint32_t a = (uint32_t)((vx->map_next + page - 1) & ~(page - 1));
    size_t span = (len + page - 1) & ~(page - 1);
    if (a + span > MAP_HI) cpu_fault(&vx->cpu, "guest map region exhausted");
    if (fd >= 0) {
        void *p = mmap(vx->mem + a, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, (off_t)off);
        if (p == MAP_FAILED) cpu_fault(&vx->cpu, "mmap failed: %s", strerror(errno));
    }
    vx->map_next = a + (uint32_t)span;
    return a;
}
static void h_mmap(Vx *vx) {
    uint32_t len = arg(vx, 1), fdslot = arg(vx, 4);
    uint64_t off = (uint64_t)arg(vx, 5) | ((uint64_t)arg(vx, 6) << 32);
    int fd = fdslot == 0xFFFFFFFF ? -1 : host_fd(vx, fdslot);
    ret(vx, map_file_fd(vx, fd, off, len));
}
static void h_munmap(Vx *vx) { ret(vx, 0); }
static void h_vm_allocate(Vx *vx) {
    uint32_t n = arg(vx, 2);
    s32(&vx->cpu.k, arg(vx, 1), map_file_fd(vx, -1, 0, n));
    ret(vx, 0);
}

// ---- data-mapping interface (replaces Apple's CFDictionary-based one)
static void m_open(Vx *vx) {
    const char *name = gstr(vx, arg(vx, 2));
    uint32_t out = arg(vx, 3);
    char fname[512];
    size_t k = 0;
    int has_dot = 0;
    for (const char *p = name; *p && k < sizeof fname - 5; p++) {
        fname[k++] = *p == '/' ? '_' : *p;
        if (*p == '.') has_dot = 1;
    }
    fname[k] = 0;
    if (!has_dot) strcat(fname, ".dat");
    char path[1600] = {0};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s", vx->dirs[i], fname);
        if (access(path, R_OK) == 0) break;
        path[0] = 0;
    }
    if (getenv("VX_FILES")) fprintf(stderr, "[map] %s -> %s\n", name, path[0] ? path : "(missing)");
    if (!path[0] || !out) { ret(vx, 0x80000014); return; }
    for (int i = 0; i < MAX_MAPS; i++)
        if (vx->maps[i].refs && strcmp(vx->maps[i].path, path) == 0) {
            vx->maps[i].refs++;
            s32(&vx->cpu.k, out, TRAP_HI + i);  // opaque handle
            ret(vx, 0);
            return;
        }
    for (int i = 0; i < MAX_MAPS; i++) {
        Mapping *m = &vx->maps[i];
        if (m->refs || m->guest) {
            if (m->guest && strcmp(m->path, path) == 0) {  // reuse a closed mapping
                m->refs = 1;
                s32(&vx->cpu.k, out, TRAP_HI + i);
                ret(vx, 0);
                return;
            }
            continue;
        }
        int fd = open(path, O_RDONLY);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); ret(vx, 0x8000010e); return; }
        snprintf(m->path, sizeof m->path, "%s", path);
        m->size = (uint32_t)st.st_size;
        m->guest = map_file_fd(vx, fd, 0, m->size + 16);
        close(fd);
        m->refs = 1;
        s32(&vx->cpu.k, out, TRAP_HI + i);
        ret(vx, 0);
        return;
    }
    cpu_fault(&vx->cpu, "too many mapped files");
}
static Mapping *mapping_of(Vx *vx, uint32_t h) {
    uint32_t i = h - TRAP_HI;
    if (i >= MAX_MAPS || !vx->maps[i].guest) cpu_fault(&vx->cpu, "bad mapping handle %#x", h);
    return &vx->maps[i];
}
static void m_close(Vx *vx) {
    Mapping *m = mapping_of(vx, arg(vx, 0));
    if (m->refs > 0) m->refs--;
    ret(vx, 0);
}
static void m_map(Vx *vx) {
    Mapping *m = mapping_of(vx, arg(vx, 0));
    uint32_t off = arg(vx, 1), pcb = arg(vx, 2), ppmem = arg(vx, 3);
    if (off >= m->size) {
        s32(&vx->cpu.k, ppmem, 0);
        if (pcb) s32(&vx->cpu.k, pcb, 0);
        ret(vx, 0);
        return;
    }
    uint32_t avail = m->size - off;
    if (pcb) {
        uint32_t req = g32(&vx->cpu.k, pcb);
        if (req && req < avail) avail = req;
        s32(&vx->cpu.k, pcb, avail);
    }
    s32(&vx->cpu.k, ppmem, m->guest + off);
    ret(vx, 0);
}

// ---------------------------------------------------------------- import table

typedef struct { const char *name; Hle fn; } HleEntry;
// ---------------------------------------------------------------- libc++ std::string (iOS 10)
// The iOS 10 engine's text path builds a few std::strings. 32-bit libc++
// layout, little-endian: short = {u8 size<<1, char[11]}, long = {u32 alloc|1,
// u32 size, char *data}.
#define STR_SHORT_CAP 10u
static int str_long(Vx *vx, uint32_t p) { return g8(&vx->cpu.k, p) & 1; }
static uint32_t str_size(Vx *vx, uint32_t p) { return str_long(vx, p) ? g32(&vx->cpu.k, p + 4) : g8(&vx->cpu.k, p) >> 1; }
static uint32_t str_data(Vx *vx, uint32_t p) { return str_long(vx, p) ? g32(&vx->cpu.k, p + 8) : p + 1; }
static uint32_t str_cap(Vx *vx, uint32_t p) { return str_long(vx, p) ? (g32(&vx->cpu.k, p) & ~1u) - 1 : STR_SHORT_CAP; }
static void str_set_size(Vx *vx, uint32_t p, uint32_t n) {
    if (str_long(vx, p)) s32(&vx->cpu.k, p + 4, n);
    else s8(&vx->cpu.k, p, n << 1);
    s8(&vx->cpu.k, str_data(vx, p) + n, 0);
}
static void str_reserve(Vx *vx, uint32_t p, uint32_t n) {
    uint32_t cap = str_cap(vx, p);
    if (n <= cap) return;
    uint32_t want = n < 2 * cap ? 2 * cap : n;
    uint32_t alloc = (want + 16) & ~15u;  // even, room for the NUL
    uint32_t mem = heap_alloc(vx, alloc), size = str_size(vx, p), old = str_data(vx, p);
    memmove(vx->mem + mem, vx->mem + old, size + 1);
    if (str_long(vx, p)) heap_free(vx, old);
    s32(&vx->cpu.k, p, alloc | 1);
    s32(&vx->cpu.k, p + 4, size);
    s32(&vx->cpu.k, p + 8, mem);
}
static void str_init_empty(Vx *vx, uint32_t p) { s32(&vx->cpu.k, p, 0); s32(&vx->cpu.k, p + 4, 0); s32(&vx->cpu.k, p + 8, 0); }
// Replace [pos, pos+len) with n bytes from host memory (may alias the string).
static void str_splice(Vx *vx, uint32_t p, uint32_t pos, uint32_t len, const uint8_t *src, uint32_t n) {
    uint32_t size = str_size(vx, p);
    if (pos > size) pos = size;
    if (len > size - pos) len = size - pos;
    uint8_t *tmp = malloc(n + 1);
    if (n) memcpy(tmp, src, n);
    str_reserve(vx, p, size - len + n);
    uint8_t *d = vx->mem + str_data(vx, p);
    memmove(d + pos + n, d + pos + len, size - pos - len);
    if (n) memcpy(d + pos, tmp, n);
    free(tmp);
    str_set_size(vx, p, size - len + n);
}
static void str_assign_host(Vx *vx, uint32_t p, const uint8_t *src, uint32_t n) { str_splice(vx, p, 0, str_size(vx, p), src, n); }
static const uint8_t *gbuf(Vx *vx, uint32_t a, uint32_t n) { check_range(vx, a, n); return vx->mem + a; }

static void h_str_init_pn(Vx *vx) {  // __init(const char*, size_t)
    uint32_t p = arg(vx, 0);
    str_init_empty(vx, p);
    str_assign_host(vx, p, gbuf(vx, arg(vx, 1), arg(vx, 2)), arg(vx, 2));
}
static void h_str_init_pnr(Vx *vx) {  // __init(const char*, size_t, size_t reserve)
    uint32_t p = arg(vx, 0);
    str_init_empty(vx, p);
    str_reserve(vx, p, arg(vx, 3));
    str_assign_host(vx, p, gbuf(vx, arg(vx, 1), arg(vx, 2)), arg(vx, 2));
}
static void h_str_init_nc(Vx *vx) {  // __init(size_t n, char c)
    uint32_t p = arg(vx, 0), n = arg(vx, 1);
    str_init_empty(vx, p);
    str_reserve(vx, p, n);
    memset(vx->mem + str_data(vx, p), (int)(arg(vx, 2) & 0xFF), n);
    str_set_size(vx, p, n);
}
static void h_str_copy(Vx *vx) {  // basic_string(const basic_string&)
    uint32_t p = arg(vx, 0), o = arg(vx, 1);
    str_init_empty(vx, p);
    str_assign_host(vx, p, vx->mem + str_data(vx, o), str_size(vx, o));
    ret(vx, p);
}
static void h_str_substr(Vx *vx) {  // basic_string(const basic_string&, pos, n, const allocator&)
    uint32_t p = arg(vx, 0), o = arg(vx, 1), pos = arg(vx, 2), n = arg(vx, 3), size = str_size(vx, o);
    str_init_empty(vx, p);
    if (pos > size) pos = size;
    if (n > size - pos) n = size - pos;
    str_assign_host(vx, p, vx->mem + str_data(vx, o) + pos, n);
    ret(vx, p);
}
static void h_str_dtor(Vx *vx) {
    uint32_t p = arg(vx, 0);
    if (str_long(vx, p)) heap_free(vx, g32(&vx->cpu.k, p + 8));
    ret(vx, p);
}
static void h_str_assign_str(Vx *vx) {  // operator=(const basic_string&)
    uint32_t p = arg(vx, 0), o = arg(vx, 1);
    if (p != o) str_assign_host(vx, p, vx->mem + str_data(vx, o), str_size(vx, o));
    ret(vx, p);
}
static void h_str_assign_c(Vx *vx) {  // operator=(char)
    uint8_t c = (uint8_t)arg(vx, 1);
    str_assign_host(vx, arg(vx, 0), &c, 1);
    ret(vx, arg(vx, 0));
}
static void h_str_assign_p(Vx *vx) {
    const char *s = gstr(vx, arg(vx, 1));
    str_assign_host(vx, arg(vx, 0), (const uint8_t *)s, (uint32_t)strlen(s));
    ret(vx, arg(vx, 0));
}
static void h_str_assign_pn(Vx *vx) {
    str_assign_host(vx, arg(vx, 0), gbuf(vx, arg(vx, 1), arg(vx, 2)), arg(vx, 2));
    ret(vx, arg(vx, 0));
}
static void h_str_append_p(Vx *vx) {
    uint32_t p = arg(vx, 0);
    const char *s = gstr(vx, arg(vx, 1));
    str_splice(vx, p, str_size(vx, p), 0, (const uint8_t *)s, (uint32_t)strlen(s));
    ret(vx, p);
}
static void h_str_append_pn(Vx *vx) {
    uint32_t p = arg(vx, 0);
    str_splice(vx, p, str_size(vx, p), 0, gbuf(vx, arg(vx, 1), arg(vx, 2)), arg(vx, 2));
    ret(vx, p);
}
static void h_str_push_back(Vx *vx) {
    uint32_t p = arg(vx, 0);
    uint8_t c = (uint8_t)arg(vx, 1);
    str_splice(vx, p, str_size(vx, p), 0, &c, 1);
}
static void h_str_insert_p(Vx *vx) {
    uint32_t p = arg(vx, 0);
    const char *s = gstr(vx, arg(vx, 2));
    str_splice(vx, p, arg(vx, 1), 0, (const uint8_t *)s, (uint32_t)strlen(s));
    ret(vx, p);
}
static void h_str_insert_pn(Vx *vx) {
    uint32_t p = arg(vx, 0);
    str_splice(vx, p, arg(vx, 1), 0, gbuf(vx, arg(vx, 2), arg(vx, 3)), arg(vx, 3));
    ret(vx, p);
}
static void h_str_erase(Vx *vx) {
    uint32_t p = arg(vx, 0);
    str_splice(vx, p, arg(vx, 1), arg(vx, 2), NULL, 0);
    ret(vx, p);
}
static void h_str_resize(Vx *vx) {
    uint32_t p = arg(vx, 0), n = arg(vx, 1), size = str_size(vx, p);
    if (n <= size) { str_set_size(vx, p, n); return; }
    str_reserve(vx, p, n);
    memset(vx->mem + str_data(vx, p) + size, (int)(arg(vx, 2) & 0xFF), n - size);
    str_set_size(vx, p, n);
}
static void h_str_reserve(Vx *vx) { str_reserve(vx, arg(vx, 0), arg(vx, 1)); }
static void h_str_compare_p(Vx *vx) {  // compare(const char*) const
    uint32_t p = arg(vx, 0), n = str_size(vx, p);
    const char *s = gstr(vx, arg(vx, 1));
    uint32_t m = (uint32_t)strlen(s);
    int r = memcmp(vx->mem + str_data(vx, p), s, n < m ? n : m);
    ret(vx, (uint32_t)(r ? r : n < m ? -1 : n > m ? 1 : 0));
}
static void str_ret_text(Vx *vx, const char *b) {  // std::to_string: result object in r0
    str_init_empty(vx, arg(vx, 0));
    str_assign_host(vx, arg(vx, 0), (const uint8_t *)b, (uint32_t)strlen(b));
}
static void h_to_string_i(Vx *vx) { char b[32]; snprintf(b, sizeof b, "%d", (int32_t)arg(vx, 1)); str_ret_text(vx, b); }
static void h_to_string_u(Vx *vx) { char b[32]; snprintf(b, sizeof b, "%u", arg(vx, 1)); str_ret_text(vx, b); }
static void h_to_string_f(Vx *vx) {
    char b[64];
    uint32_t u = arg(vx, 1);
    float f;
    memcpy(&f, &u, 4);
    snprintf(b, sizeof b, "%f", (double)f);
    str_ret_text(vx, b);
}

static const HleEntry kHle[] = {
    // libc++ std::string (iOS 10)
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm", h_str_init_pn},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcmm", h_str_init_pnr},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEmc", h_str_init_nc},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_", h_str_copy},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1ERKS5_mmRKS4_", h_str_substr},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev", h_str_dtor},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSERKS5_", h_str_assign_str},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEaSEc", h_str_assign_c},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKc", h_str_assign_p},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6assignEPKcm", h_str_assign_pn},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKc", h_str_append_p},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6appendEPKcm", h_str_append_pn},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE9push_backEc", h_str_push_back},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKc", h_str_insert_p},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6insertEmPKcm", h_str_insert_pn},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE5eraseEmm", h_str_erase},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6resizeEmc", h_str_resize},
    {"__ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7reserveEm", h_str_reserve},
    {"__ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc", h_str_compare_p},
    {"__ZNSt3__19to_stringEi", h_to_string_i},
    {"__ZNSt3__19to_stringEj", h_to_string_u},
    {"__ZNSt3__19to_stringEm", h_to_string_u},
    {"__ZNSt3__19to_stringEf", h_to_string_f},
    {"__Znwm", h_malloc}, {"__Znam", h_malloc}, {"__ZdlPv", h_free}, {"__ZdaPv", h_free},  // C++ new/delete (iOS 10)
    {"_malloc", h_malloc}, {"_calloc", h_calloc}, {"_realloc", h_realloc}, {"_free", h_free},
    {"_malloc_size", h_malloc_size},
    {"_memcpy", h_memcpy}, {"_memmove", h_memcpy}, {"___memcpy_chk", h_memcpy},
    {"_memset", h_memset}, {"___memset_chk", h_memset}, {"_memset_pattern16", h_memset_pattern16},
    {"_memcmp", h_memcmp}, {"_memchr", h_memchr},
    {"_strlen", h_strlen}, {"_strcmp", h_strcmp}, {"_strcpy", h_strcpy}, {"_strlcpy", h_strlcpy},
    {"___stack_chk_fail", h_stack_chk_fail}, {"___assert_rtn", h_assert_rtn}, {"_abort", h_abort},
    {"_exit", h_exit},
    {"___divmodsi4", h_divmodsi4}, {"___udivmodsi4", h_udivmodsi4}, {"___divdi3", h_divdi3},
    {"___divsi3", h_divsi3}, {"___modsi3", h_modsi3}, {"___udivsi3", h_udivsi3}, {"___umodsi3", h_umodsi3},
    {"_floor", h_floor}, {"_roundf", h_roundf}, {"___fixdfdi", h_fixdfdi}, {"___floatundidf", h_floatundidf},
    {"___error", h_error}, {"_getpagesize", h_getpagesize}, {"_usleep", h_zero}, {"_setpriority", h_zero},
    {"_getiopolicy_np", h_zero}, {"_setiopolicy_np", h_zero}, {"_times", h_times}, {"_ftime", h_ftime},
    {"_pthread_mutex_init", h_zero}, {"_pthread_mutex_destroy", h_zero}, {"_pthread_mutex_lock", h_zero},
    {"_pthread_mutex_unlock", h_zero}, {"_pthread_mutexattr_init", h_zero},
    {"_pthread_mutexattr_destroy", h_zero}, {"_pthread_mutexattr_settype", h_zero},
    {"_pthread_self", h_pthread_self}, {"_pthread_key_create", h_pthread_key_create},
    {"_pthread_getspecific", h_pthread_getspecific}, {"_pthread_setspecific", h_pthread_setspecific},
    {"_pthread_once", h_pthread_once},
    {"_setjmp", h_setjmp}, {"_longjmp", h_longjmp}, {"_qsort", h_qsort},
    {"_vsnprintf", h_vsnprintf}, {"___vsnprintf_chk", h_vsnprintf_chk}, {"___snprintf_chk", h_snprintf_chk},
    {"___sprintf_chk", h_sprintf_chk}, {"_printf", h_printf}, {"_putchar", h_putchar}, {"_syslog", h_zero},
    {"_fopen", h_fopen}, {"_fread", h_fread}, {"_fseek", h_fseek}, {"_ftell", h_ftell}, {"_ferror", h_ferror},
    {"_fflush", h_zero}, {"_fileno", h_fileno}, {"_fwrite", h_fwrite}, {"_fclose", h_fclose},
    {"_open", h_open}, {"_close", h_close}, {"_stat", h_stat}, {"_fstat", h_fstat},
    {"_mmap", h_mmap}, {"_munmap", h_munmap}, {"_vm_allocate", h_vm_allocate},
    {"_strncmp", h_strncmp}, {"_strchr", h_strchr}, {"_strrchr", h_strrchr}, {"_strstr", h_strstr},
    {"_strpbrk", h_strpbrk}, {"_strspn", h_strspn}, {"_strcat", h_strcat}, {"_strncat", h_strncat},
    {"_strncpy", h_strncpy}, {"_strtol", h_strtol}, {"_strtoul", h_strtoul}, {"_atoi", h_atoi},
    {"___tolower", h_tolower}, {"___toupper", h_toupper}, {"___maskrune", h_maskrune},
    {"_lroundf", h_lroundf}, {"_sysconf", h_sysconf}, {"_strerror", h_strerror},
    {"___memmove_chk", h_memcpy}, {"___strcpy_chk", h_strcpy}, {"___strcat_chk", h_strcat},
    {"___strncpy_chk", h_strncpy}, {"_snprintf", h_snprintf},
    {"_sprintf", h_sprintf}, {"_vsprintf", h_vsprintf}, {"_fprintf", h_fprintf}, {"_fputc", h_zero},
};

static const char *const kDataImports[] = {
    "___stack_chk_guard", "_NDR_record", "___CFConstantStringClassReference", "__NSConcreteGlobalBlock",
    "__NSConcreteStackBlock", "_kCFAllocatorDefault", "_kCFAllocatorNull", "_kCFBooleanFalse",
    "_kCFBooleanTrue", "_kCFNull", "_kCFRunLoopCommonModes", "_kCFRunLoopDefaultMode",
    "___stderrp", "___stdoutp", "___ActiveLogOptions", "__kCFSystemVersionBuildVersionKey",
};

static uint32_t data_sym(Vx *vx, const char *name) {
    uint32_t a = vx->data_sym_next;
    if (strcmp(name, "__DefaultRuneLocale") == 0) {
        // 32-bit _RuneLocale: magic[8], encoding[32], 3 fn ptrs, runetype[256],
        // maplower[256], mapupper[256] (then tables we leave empty)
        vx->data_sym_next += 0x1000;
        for (int i = 0; i < 256; i++) {
            s32(&vx->cpu.k, a + 52 + 4 * i, _DefaultRuneLocale.__runetype[i]);
            s32(&vx->cpu.k, a + 52 + 1024 + 4 * i, (uint32_t)_DefaultRuneLocale.__maplower[i]);
            s32(&vx->cpu.k, a + 52 + 2048 + 4 * i, (uint32_t)_DefaultRuneLocale.__mapupper[i]);
        }
        return a;
    }
    vx->data_sym_next += 64;
    if (strcmp(name, "___stack_chk_guard") == 0) s32(&vx->cpu.k, a, 0xC0FFEE42);
    return a;
}

static void on_trap(Cpu *c, uint32_t addr, void *ctx);

// ---------------------------------------------------------------- Mach-O loading

typedef struct { uint32_t cmd, cmdsize; } LoadCmd;

static int load_macho(Vx *vx, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(vx->err, sizeof vx->err, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return -1; }
    fclose(f);
    uint32_t magic, ncmds;
    memcpy(&magic, d, 4);
    memcpy(&ncmds, d + 16, 4);
    if (magic != 0xFEEDFACE) { snprintf(vx->err, sizeof vx->err, "not a 32-bit Mach-O"); free(d); return -1; }

    uint32_t symoff = 0, nsyms = 0, stroff = 0, indoff = 0, nind = 0;
    struct { uint32_t addr, size, flags, r1, r2; } ptrsecs[8];
    int nptr = 0;
    uint32_t stub_addr = 0, stub_size = 0;
    size_t off = 28;
    for (uint32_t i = 0; i < ncmds; i++) {
        LoadCmd lc;
        memcpy(&lc, d + off, sizeof lc);
        if (lc.cmd == 1) {  // LC_SEGMENT
            uint32_t vmaddr, vmsize, fileoff, filesize, nsects;
            memcpy(&vmaddr, d + off + 24, 4);
            memcpy(&vmsize, d + off + 28, 4);
            memcpy(&fileoff, d + off + 32, 4);
            memcpy(&filesize, d + off + 36, 4);
            memcpy(&nsects, d + off + 48, 4);
            if (filesize) {  // __PAGEZERO has no file data; iOS 10's bundle has __TEXT at 0
                if (vmaddr + vmsize > TRAP_LO) { snprintf(vx->err, sizeof vx->err, "image too large"); free(d); return -1; }
                memcpy(vx->mem + vmaddr, d + fileoff, filesize);
            }
            for (uint32_t s = 0; s < nsects; s++) {
                const uint8_t *sec = d + off + 56 + 68 * s;
                uint32_t addr, size, flags, r1, r2;
                memcpy(&addr, sec + 32, 4);
                memcpy(&size, sec + 36, 4);
                memcpy(&flags, sec + 56, 4);
                memcpy(&r1, sec + 60, 4);
                memcpy(&r2, sec + 64, 4);
                uint32_t type = flags & 0xff;
                if (type == 8) { stub_addr = addr; stub_size = size; }
                if ((type == 6 || type == 7) && nptr < 8) {
                    ptrsecs[nptr].addr = addr; ptrsecs[nptr].size = size;
                    ptrsecs[nptr].flags = type; ptrsecs[nptr].r1 = r1; ptrsecs[nptr].r2 = r2;
                    nptr++;
                }
            }
        } else if (lc.cmd == 2) {  // LC_SYMTAB
            memcpy(&symoff, d + off + 8, 4);
            memcpy(&nsyms, d + off + 12, 4);
            memcpy(&stroff, d + off + 16, 4);
        } else if (lc.cmd == 0xB) {  // LC_DYSYMTAB
            memcpy(&indoff, d + off + 56, 4);
            memcpy(&nind, d + off + 60, 4);
        }
        off += lc.cmdsize;
    }

    // named engine entry points
    struct { const char *name; uint32_t *dst; } want[] = {
        {"_vauto_ttsInitialize", &vx->fn_initialize}, {"_vauto_ttsOpen", &vx->fn_open},
        {"_vauto_ttsSetParamList", &vx->fn_setparams}, {"_vauto_ttsSetOutDevice", &vx->fn_setoutdevice},
        {"_vauto_ttsProcessText2Speech", &vx->fn_process},
        // iOS 7 calls the engine directly (same signatures)
        {"_ve_ttsInitialize", &vx->fn_initialize}, {"_ve_ttsOpen", &vx->fn_open},
        {"_ve_ttsSetParamList", &vx->fn_setparams}, {"_ve_ttsSetOutDevice", &vx->fn_setoutdevice},
        {"_ve_ttsProcessText2Speech", &vx->fn_process},
    };
    for (uint32_t i = 0; i < nsyms; i++) {
        const uint8_t *nl = d + symoff + 12 * i;
        uint32_t strx, value;
        memcpy(&strx, nl, 4);
        memcpy(&value, nl + 8, 4);
        const char *nm = (const char *)d + stroff + strx;
        for (size_t w = 0; w < sizeof want / sizeof want[0]; w++)
            if (strcmp(nm, want[w].name) == 0) *want[w].dst = value;
    }

    // bind every lazy / non-lazy pointer to a trap slot or a data slot
    uint32_t lazy_base = 0;
    for (int p = 0; p < nptr; p++) {
        if (ptrsecs[p].flags == 7 && !lazy_base) lazy_base = ptrsecs[p].addr;
        for (uint32_t k = 0; k < ptrsecs[p].size / 4; k++) {
            uint32_t ind;
            memcpy(&ind, d + indoff + 4 * (ptrsecs[p].r1 + k), 4);
            if (ind & 0xC0000000) continue;  // LOCAL / ABS: already-resolved pointer
            uint32_t strx;
            memcpy(&strx, d + symoff + 12 * ind, 4);
            const char *nm = (const char *)d + stroff + strx;
            uint32_t slot_addr = ptrsecs[p].addr + 4 * k;
            int is_data = 0;
            for (size_t q = 0; q < sizeof kDataImports / sizeof kDataImports[0]; q++)
                if (strcmp(nm, kDataImports[q]) == 0) is_data = 1;
            if (strcmp(nm, "__DefaultRuneLocale") == 0) is_data = 1;
            if (is_data || strncmp(nm, "_kCF", 4) == 0 || strncmp(nm, "_kVS", 4) == 0) {
                s32(&vx->cpu.k, slot_addr, data_sym(vx, nm));
                continue;
            }
            if (vx->nimports >= MAX_IMPORTS) continue;
            int idx = vx->nimports++;
            vx->hle_name[idx] = strdup(nm);
            for (size_t h = 0; h < sizeof kHle / sizeof kHle[0]; h++)
                if (strcmp(nm, kHle[h].name) == 0) vx->hle[idx] = kHle[h].fn;
            s32(&vx->cpu.k, slot_addr, (TRAP_LO + 4 * (uint32_t)idx) | 1);
        }
    }
    free(d);
    vx->cpu.stub_lo = stub_addr;
    vx->cpu.stub_hi = stub_addr + stub_size;
    vx->cpu.lazy_base = lazy_base;
    for (size_t b = 0; b < sizeof kBuilds / sizeof kBuilds[0]; b++)
        if (kBuilds[b].stub_addr == stub_addr) vx->build = &kBuilds[b];
    if (!vx->build) { snprintf(vx->err, sizeof vx->err, "unrecognized voiced build"); return -1; }
    if (vx->build->fn_initialize) {
        vx->fn_initialize = vx->build->fn_initialize;
        vx->fn_open = vx->build->fn_open;
        vx->fn_setparams = vx->build->fn_setparams;
        vx->fn_setoutdevice = vx->build->fn_setoutdevice;
        vx->fn_process = vx->build->fn_process;
    }
    if (!vx->fn_initialize || !vx->fn_process || !stub_addr) {
        snprintf(vx->err, sizeof vx->err, "voiced is missing the Vocalizer API symbols");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------- traps

static void outnotify(Vx *vx) {
    uint32_t msg = arg(vx, 3);
    uint32_t kind = g32(&vx->cpu.k, msg), param = g32(&vx->cpu.k, msg + 8);
    if (kind == VE_MSG_OUTBUFREQ && param) {
        // VE_OUTDATA {format, cntPcmBufLen, pOutPcmBuf, cntMrkListLen, pMrkList}
        s32(&vx->cpu.k, param + 4, vx->cancelled ? 0 : vx->pcm_buf_size);
        s32(&vx->cpu.k, param + 8, vx->cancelled ? 0 : vx->pcm_buf);
        s32(&vx->cpu.k, param + 12, 0x500);
        s32(&vx->cpu.k, param + 16, vx->mark_buf);
        ret(vx, vx->cancelled ? VE_USER_STOP : 0);
        return;
    }
    if (kind == VE_MSG_OUTBUFDONE && param) {
        uint32_t n = g32(&vx->cpu.k, param + 4), p = g32(&vx->cpu.k, param + 8);
        if (n && p && vx->cb && !vx->cancelled) {
            check_range(vx, p, n);
            if (vx->cb(vx->cb_ctx, (const int16_t *)(vx->mem + p), n / 2)) vx->cancelled = 1;
        }
        ret(vx, vx->cancelled ? VE_USER_STOP : 0);
        return;
    }
    ret(vx, 0);
}

static void on_trap(Cpu *c, uint32_t addr, void *ctx) {
    Vx *vx = ctx;
    (void)c;
    if (addr == TRAP_OUTNOTIFY) { outnotify(vx); return; }
    if (addr >= TRAP_MAPPING && addr < TRAP_MAPPING + 20) {
        switch ((addr - TRAP_MAPPING) / 4) {
        case 0: m_open(vx); return;
        case 1: m_close(vx); return;
        case 2: m_map(vx); return;
        default: ret(vx, 0); return;
        }
    }
    uint32_t idx = (addr - TRAP_LO) / 4;
    if (idx >= (uint32_t)vx->nimports) cpu_fault(c, "jump into trap region %#x (lr=%#x)", addr, c->k.r[14]);
    if (!vx->hle[idx]) cpu_fault(c, "unimplemented import %s (lr=%#x)", vx->hle_name[idx], c->k.r[14]);
    static int trace = -1;
    if (trace < 0) trace = getenv("VX_TRACE") != NULL;
    if (trace) fprintf(stderr, "%s(%x,%x,%x,%x) lr=%x", vx->hle_name[idx], c->k.r[0], c->k.r[1], c->k.r[2], c->k.r[3], c->k.r[14]);
    vx->hle[idx](vx);
    if (trace) fprintf(stderr, " -> %x\n", c->k.r[0]);
}

// ---------------------------------------------------------------- engine driver

static int set_params_raw(Vx *vx, const uint32_t *ids, const uint32_t *nums, const char *const *strs, int n) {
    uint32_t p = new_zeroed(vx, 0x84 * (uint32_t)n);
    for (int i = 0; i < n; i++) {
        s32(&vx->cpu.k, p + 0x84 * i, ids[i]);
        if (strs && strs[i]) snprintf((char *)vx->mem + p + 0x84 * i + 4, 128, "%s", strs[i]);
        else s16(&vx->cpu.k, p + 0x84 * i + 4, nums[i]);
    }
    uint32_t args[4] = {g32(&vx->cpu.k, vx->hinst), g32(&vx->cpu.k, vx->hinst + 4), p, (uint32_t)n};
    uint32_t rc = cpu_call(&vx->cpu, vx->fn_setparams, args, 4, NULL);
    heap_free(vx, p);
    return (int)rc;
}

static int xml_tag(const char *xml, const char *tag, char *out, size_t cap) {
    char open[64], close[64];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char *a = strstr(xml, open), *b;
    if (!a) return -1;
    a += strlen(open);
    b = strstr(a, close);
    if (!b) return -1;
    size_t n = (size_t)(b - a) < cap - 1 ? (size_t)(b - a) : cap - 1;
    memcpy(out, a, n);
    out[n] = 0;
    return 0;
}

#define GUARD(vx) \
    jmp_buf *prev_jmp = (vx)->cpu.fault_jmp; \
    (vx)->cpu.fault_jmp = &(vx)->fault; \
    if (setjmp((vx)->fault)) { \
        (vx)->cpu.fault_jmp = prev_jmp; \
        snprintf((vx)->err, sizeof (vx)->err, "%s", (vx)->cpu.fault_msg); \
        return -1; \
    }
#define UNGUARD(vx) ((vx)->cpu.fault_jmp = prev_jmp)

static int vx_start(Vx *vx, const char *voiced_path, const char *lang_dir, const char *common_dir) {
    GUARD(vx);
    if (load_macho(vx, voiced_path)) { UNGUARD(vx); return -1; }

    // Built-in voices ship `broker.hdr` ("defaultvoice:<Name>\n" + XML);
    // downloaded (MobileAsset) voices ship `broker.hdr.asset` (XML only).
    char bpath[1100];
    snprintf(bpath, sizeof bpath, "%s/broker.hdr", lang_dir);
    FILE *f = fopen(bpath, "rb");
    if (!f) {
        snprintf(bpath, sizeof bpath, "%s/broker.hdr.asset", lang_dir);
        f = fopen(bpath, "rb");
    }
    if (!f) { snprintf(vx->err, sizeof vx->err, "missing broker.hdr in %s", lang_dir); UNGUARD(vx); return -1; }
    char *broker = calloc(1, 65536);
    size_t blen = fread(broker, 1, 65535, f);
    fclose(f);
    broker[blen] = 0;
    char *nl = strncmp(broker, "defaultvoice:", 13) == 0 ? strchr(broker, '\n') : NULL;
    if (nl) {
        snprintf(vx->voice, sizeof vx->voice, "%.*s", (int)(nl - broker - 13), broker + 13);
    } else {
        nl = broker - 1;  // whole file is XML
        if (xml_tag(broker, "voice", vx->voice, sizeof vx->voice)) {
            snprintf(vx->err, sizeof vx->err, "bad broker header in %s", lang_dir);
            free(broker);
            UNGUARD(vx);
            return -1;
        }
    }
    const char *xml = nl + 1;
    char *joined = NULL;
    if (vx->build->split_broker) {
        // e.g. <BROKERSTRING>pipeline/American English/samantha/16/...
        const char *p = strstr(xml, "pipeline/");
        const char *e = p ? strchr(p + 9, '/') : NULL;
        if (p && e) snprintf(vx->language, sizeof vx->language, "%.*s", (int)(e - p - 9), p + 9);
        else snprintf(vx->language, sizeof vx->language, "?");
        char cpath[1100];
        snprintf(cpath, sizeof cpath, "%s/broker.hdr", common_dir);
        FILE *cf = fopen(cpath, "rb");
        if (cf) {
            joined = calloc(1, 131072);
            size_t cl = fread(joined, 1, 65535, cf);
            fclose(cf);
            snprintf(joined + cl, 131072 - cl, "%s", xml);
            xml = joined;
        }
    } else if (xml_tag(xml, "language", vx->language, sizeof vx->language)) {
        snprintf(vx->language, sizeof vx->language, "?");
    }

    snprintf(vx->dirs[0], sizeof vx->dirs[0], "%s", lang_dir);
    snprintf(vx->dirs[1], sizeof vx->dirs[1], "%s", common_dir);

    // Apple's data-class object (voiced 0x14270): +0x14 ndirs, +0x18 dirs[]
    uint32_t dirs = heap_alloc(vx, 8);
    s32(&vx->cpu.k, dirs, new_cstr(vx, vx->dirs[0]));
    s32(&vx->cpu.k, dirs + 4, new_cstr(vx, vx->dirs[1]));
    uint32_t dc = new_zeroed(vx, 0x1C);
    uint32_t gxml = new_cstr(vx, xml);
    s32(&vx->cpu.k, dc + 0x00, gxml);
    s32(&vx->cpu.k, dc + 0x08, new_cstr(vx, vx->voice));
    s32(&vx->cpu.k, dc + 0x0C, 0xFFFFFFFF);
    s32(&vx->cpu.k, dc + 0x14, 2);
    s32(&vx->cpu.k, dc + 0x18, dirs);
    free(broker);
    free(joined);

    // our mapping interface: 5 trap entry points
    uint32_t mapping = heap_alloc(vx, 20);
    for (uint32_t i = 0; i < 5; i++) s32(&vx->cpu.k, mapping + 4 * i, (TRAP_MAPPING + 4 * i) | 1);

    // VE_INSTALL as voiced builds it at 0x5c7e
    uint32_t inst = new_zeroed(vx, 0x2C);
    s16(&vx->cpu.k, inst + 0x00, 0x520);
    s32(&vx->cpu.k, inst + 0x04, gxml);
    s32(&vx->cpu.k, inst + 0x08, vx->build->iface_heap);
    s32(&vx->cpu.k, inst + 0x10, vx->build->iface_critsec);
    s32(&vx->cpu.k, inst + 0x18, vx->build->iface_stream);
    s32(&vx->cpu.k, inst + 0x1C, mapping);
    s32(&vx->cpu.k, inst + 0x20, dc);

    vx->hspeech = new_zeroed(vx, 8);
    uint32_t a1[2] = {inst, vx->hspeech};
    uint32_t rc = cpu_call(&vx->cpu, vx->fn_initialize, a1, 2, NULL);
    if (rc) { snprintf(vx->err, sizeof vx->err, "ttsInitialize failed: %#x", rc); UNGUARD(vx); return -1; }

    vx->hinst = new_zeroed(vx, 8);
    uint32_t a2[6] = {g32(&vx->cpu.k, vx->hspeech), g32(&vx->cpu.k, vx->hspeech + 4), 0, 0, vx->hinst, 0};
    rc = cpu_call(&vx->cpu, vx->fn_open, a2, 6, NULL);
    if (rc) { snprintf(vx->err, sizeof vx->err, "ttsOpen failed: %#x", rc); UNGUARD(vx); return -1; }

    // language, voice, 22 kHz, UTF-16 input — same as voiced at 0x65d6
    uint32_t ids[4] = {7, 8, 1, 12}, nums[4] = {0, 0, (uint32_t)vx->build->rate_khz, 1};
    const char *strs[4] = {vx->language, vx->voice, NULL, NULL};
    rc = (uint32_t)set_params_raw(vx, ids, nums, strs, 4);
    if (rc) { snprintf(vx->err, sizeof vx->err, "ttsSetParamList failed: %#x", rc); UNGUARD(vx); return -1; }

    uint32_t outdev = new_zeroed(vx, 8);
    s32(&vx->cpu.k, outdev + 4, TRAP_OUTNOTIFY | 1);
    uint32_t a3[3] = {g32(&vx->cpu.k, vx->hinst), g32(&vx->cpu.k, vx->hinst + 4), outdev};
    rc = cpu_call(&vx->cpu, vx->fn_setoutdevice, a3, 3, NULL);
    if (rc) { snprintf(vx->err, sizeof vx->err, "ttsSetOutDevice failed: %#x", rc); UNGUARD(vx); return -1; }

    // ~23 ms buffers keep time-to-first-audio low
    vx->pcm_buf_size = 1024;
    vx->pcm_buf = heap_alloc(vx, vx->pcm_buf_size);
    vx->mark_buf = new_zeroed(vx, 0x500 * 0x20);
    UNGUARD(vx);
    return 0;
}

Vx *vx_open(const char *voiced_path, const char *lang_dir, const char *common_dir, char *err, size_t errlen) {
    Vx *vx = calloc(1, sizeof *vx);
    void *mem = mmap(NULL, GUEST_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (!vx || mem == MAP_FAILED) {
        if (err) snprintf(err, errlen, "out of memory");
        free(vx);
        return NULL;
    }
    vx->mem = mem;
    for (int i = 0; i < MAX_FILES; i++) vx->fds[i] = -1;
    cpu_init(&vx->cpu, vx->mem);
    vx->cpu.trap_lo = TRAP_LO;
    vx->cpu.trap_hi = TRAP_HI;
    vx->cpu.trap = on_trap;
    vx->cpu.trap_ctx = vx;
    vx->cpu.return_addr = RETURN_TRAP;
    vx->cpu.k.r[13] = STACK_TOP - 0x100;
    vx->heap_next = HEAP_LO;
    vx->map_next = MAP_LO;
    vx->data_sym_next = DATA_SYMS;
    vx->errno_addr = data_sym(vx, "errno");
    if (vx_start(vx, voiced_path, lang_dir, common_dir)) {
        if (err) snprintf(err, errlen, "%s", vx->err);
        vx_close(vx);
        return NULL;
    }
    return vx;
}

void vx_close(Vx *vx) {
    if (!vx) return;
    for (int i = 0; i < MAX_FILES; i++) {
        if (vx->files[i]) fclose(vx->files[i]);
        if (vx->fds[i] >= 0) close(vx->fds[i]);
    }
    for (int i = 0; i < vx->nimports; i++) free((void *)vx->hle_name[i]);
    munmap(vx->mem, GUEST_MEM_SIZE);
    free(vx);
}

int vx_speak_utf16(Vx *vx, const uint16_t *text, size_t units, VxPcmCallback cb, void *ctx) {
    GUARD(vx);
    vx->cb = cb;
    vx->cb_ctx = ctx;
    vx->cancelled = 0;
    uint32_t bytes = (uint32_t)(units * 2);
    uint32_t buf = heap_alloc(vx, bytes + 2);
    memcpy(vx->mem + buf, text, bytes);
    s16(&vx->cpu.k, buf + bytes, 0);
    uint32_t in = new_zeroed(vx, 12);
    s32(&vx->cpu.k, in + 4, bytes);
    s32(&vx->cpu.k, in + 8, buf);
    uint32_t args[3] = {g32(&vx->cpu.k, vx->hinst), g32(&vx->cpu.k, vx->hinst + 4), in};
    uint32_t rc = cpu_call(&vx->cpu, vx->fn_process, args, 3, NULL);
    heap_free(vx, buf);
    heap_free(vx, in);
    vx->cb = NULL;
    UNGUARD(vx);
    if (rc == 0 || rc == 0x80000807 || (vx->cancelled && rc == VE_USER_STOP)) return 0;
    if (vx->cancelled) return 0;
    snprintf(vx->err, sizeof vx->err, "ttsProcessText2Speech failed: %#x", rc);
    return (int)rc;
}

int vx_speak_utf8(Vx *vx, const char *text, VxPcmCallback cb, void *ctx) {
    size_t n = strlen(text);
    uint16_t *u = malloc((n + 1) * 2);
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        uint32_t c = (uint8_t)text[i];
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (len == 2) c = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
        else if (len == 3) c = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        else if (len == 4)
            c = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
        i += (size_t)len;
        // iOS 7's pp/text_parser rejects the whole input on a surrogate pair
        // (emoji), where older engines just skip it.
        if (c >= 0x10000 && strncmp(vx->build->name, "iOS 7", 5) == 0) c = ' ';
        if (c >= 0x10000) { c -= 0x10000; u[k++] = (uint16_t)(0xD800 | (c >> 10)); u[k++] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
        else u[k++] = (uint16_t)c;
    }
    int rc = vx_speak_utf16(vx, u, k, cb, ctx);
    free(u);
    return rc;
}

int vx_set_param(Vx *vx, uint32_t id, uint32_t value) {
    GUARD(vx);
    int rc = set_params_raw(vx, &id, &value, NULL, 1);
    UNGUARD(vx);
    return rc;
}

int vx_sample_rate(Vx *vx) { return vx->build->rate_khz == 16 ? 16000 : 22050; }
const char *vx_build_name(Vx *vx) { return vx->build->name; }
const char *vx_voice_name(Vx *vx) { return vx->voice; }
const char *vx_language_name(Vx *vx) { return vx->language; }
uint64_t vx_instruction_count(Vx *vx) { return vx->cpu.icount; }

void vx_enable_profile(Vx *vx, uint8_t *bitmap) { vx->cpu.profile = bitmap; }
void vx_set_budget(Vx *vx, long long n) { vx->cpu.budget = n; vx->cpu.budget_on = 1; }
uint32_t vx_budget_exit_pc(Vx *vx) { return vx->cpu.budget_exit_pc; }
void vx_set_native(Vx *vx, const void *entries, size_t n) {
    vx->cpu.native = entries;
    vx->cpu.nnative = n;
}
const char *vx_last_error(Vx *vx) { return vx->err; }
