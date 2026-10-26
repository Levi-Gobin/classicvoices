// MacinTalk host core: guest memory, heap, traps, guest calls, Mach-O loading.
// The loader follows panthera-speech's tiger_host_macho.c (MIT, Tamas Geczy).
#include "mt_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __APPLE__
#include <os/log.h>
#endif

// ---------------------------------------------------------------- Mach-O bits

#define FAT_CIGAM 0xbebafecau
#undef MH_MAGIC
#define MH_MAGIC 0xfeedfaceu
#undef CPU_TYPE_X86
#define CPU_TYPE_X86 7
#define LC_SEGMENT 0x1
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_DYLD_INFO 0x22
#undef LC_DYLD_INFO_ONLY
#define LC_DYLD_INFO_ONLY 0x80000022u
#define S_NON_LAZY_SYMBOL_PTR 0x6
#define S_LAZY_SYMBOL_PTR 0x7
#define S_SYMBOL_STUBS 0x8
#define S_MOD_INIT_FUNC 0x9
#define N_STAB 0xe0
#define N_TYPE 0x0e
#define N_EXT 0x01
#define N_UNDF 0x0
#define N_INDR 0xa
#undef INDIRECT_SYMBOL_LOCAL
#define INDIRECT_SYMBOL_LOCAL 0x80000000u
#undef INDIRECT_SYMBOL_ABS
#define INDIRECT_SYMBOL_ABS 0x40000000u
#define MH_SPLIT_SEGS 0x20
#define MH_PREBOUND 0x10
#define R_SCATTERED 0x80000000u
#define GENERIC_RELOC_VANILLA 0
#define GENERIC_RELOC_PB_LA_PTR 3

static inline uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// dysymtab_command field offsets
enum { DY_INDIRECTSYMOFF = 56, DY_NINDIRECTSYMS = 60, DY_EXTRELOFF = 64, DY_NEXTREL = 68, DY_LOCRELOFF = 72, DY_NLOCREL = 76 };

static void die(Mt *m, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void die(Mt *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->err, sizeof m->err, fmt, ap);
    va_end(ap);
    if (m->fault) longjmp(*m->fault, 1);
    fprintf(stderr, "mt: %s\n", m->err);
    exit(1);
}

// ---------------------------------------------------------------- strings

const char *mt_str(Mt *m, uint32_t a) {
    if (!a) return NULL;
    if (a >= m->mem_size || !memchr(m->mem + a, 0, m->mem_size - a)) die(m, "bad string pointer %#x", a);
    return (const char *)m->mem + a;
}

// ---------------------------------------------------------------- heap
// 16-byte aligned blocks with an 8-byte header {size, magic}; exact-size free
// lists up to 4 KB, first fit above.

#define HDR 16u
#define NBUCKET 257
static uint32_t g_free_small[NBUCKET];
static uint32_t g_free_large;

static pthread_mutex_t g_heap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t malloc_locked(Mt *m, uint32_t n);
uint32_t mt_malloc(Mt *m, uint32_t n) {
    pthread_mutex_lock(&g_heap_mu);
    uint32_t a = malloc_locked(m, n);
    pthread_mutex_unlock(&g_heap_mu);
    return a;
}
static uint32_t malloc_locked(Mt *m, uint32_t n) {
    uint32_t sz = (n + 15) & ~15u;
    if (!sz) sz = 16;
    uint32_t b = sz / 16;
    if (b < NBUCKET && g_free_small[b]) {
        uint32_t a = g_free_small[b];
        g_free_small[b] = u32(m->mem + a);
        return a;
    }
    if (b >= NBUCKET) {
        uint32_t prev = 0, cur = g_free_large;
        while (cur) {
            uint32_t csz = u32(m->mem + cur - HDR);
            uint32_t next = u32(m->mem + cur);
            if (csz >= sz) {
                if (prev) memcpy(m->mem + prev, &next, 4); else g_free_large = next;
                return cur;
            }
            prev = cur;
            cur = next;
        }
    }
    uint32_t a = m->heap_next + HDR;
    if (a + sz > m->heap_hi) die(m, "guest heap exhausted (%u bytes)", n);
    m->heap_next = a + sz;
    memcpy(m->mem + a - HDR, &sz, 4);
    memcpy(m->mem + a - HDR + 4, "BLK!", 4);
    return a;
}
uint32_t mt_msize(Mt *m, uint32_t a) { return a ? u32(m->mem + a - HDR) : 0; }
uint32_t mt_calloc(Mt *m, uint32_t n) {
    uint32_t a = mt_malloc(m, n);
    memset(m->mem + a, 0, mt_msize(m, a));
    return a;
}
void mt_free(Mt *m, uint32_t a) {
    if (!a) return;
    pthread_mutex_lock(&g_heap_mu);
    if (a < m->heap_lo || a >= m->heap_next || memcmp(m->mem + a - HDR + 4, "BLK!", 4)) { pthread_mutex_unlock(&g_heap_mu); return; }  // not ours
    uint32_t sz = u32(m->mem + a - HDR), b = sz / 16;
    if (b < NBUCKET) { memcpy(m->mem + a, &g_free_small[b], 4); g_free_small[b] = a; }
    else { memcpy(m->mem + a, &g_free_large, 4); g_free_large = a; }
    pthread_mutex_unlock(&g_heap_mu);
}
uint32_t mt_strdup(Mt *m, const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = mt_malloc(m, n);
    memcpy(m->mem + a, s, n);
    return a;
}
uint32_t mt_static(Mt *m, uint32_t n) {
    pthread_mutex_lock(&g_heap_mu);
    uint32_t a = (m->data_next + 15) & ~15u;
    if (a + n > MT_IMAGE_BASE) die(m, "static data region exhausted");
    m->data_next = a + n;
    pthread_mutex_unlock(&g_heap_mu);
    memset(m->mem + a, 0, n);
    return a;
}

// ---------------------------------------------------------------- traps / calls

static void on_trap(X86 *c, uint32_t addr, void *ctx) {
    Mt *m = ctx;
    uint32_t idx = (addr - MT_TRAP_BASE) / 16;
    if (addr == MT_STOP) return;
    if (idx >= (uint32_t)m->ntraps || (addr & 15)) x86_fault(c, "jump into trap region %#x", addr);
    if (!m->trap_fn[idx]) x86_fault(c, "unimplemented import %s (called from %#x)", m->trap_name[idx], rd32(c, c->r[ESP]));
    if (m->verbose > 1) fprintf(stderr, "[hle] %s\n", m->trap_name[idx]);
    m->trap_fn[idx](m, c);
}

static uint32_t trap_for(Mt *m, const char *name) {
    const uint32_t mask = 2 * MT_TRAP_SLOTS - 1;
    uint32_t h = mt_str_hash(name) & mask;
    for (; m->trap_index[h]; h = (h + 1) & mask)
        if (strcmp(m->trap_name[m->trap_index[h] - 1], name) == 0) return MT_TRAP_BASE + 16u * (uint32_t)(m->trap_index[h] - 1);
    if (m->ntraps >= (int)MT_TRAP_SLOTS) die(m, "too many imports");
    int i = m->ntraps++;
    m->trap_index[h] = (uint16_t)(i + 1);
    m->trap_name[i] = strdup(name);
    m->trap_fn[i] = mt_lookup_hle(name);
    if (!m->trap_fn[i]) {
        // Leopard's libSystem publishes conformance variants ($UNIX2003 ...)
        const char *dollar = strchr(name, '$');
        if (dollar) {
            char base[256];
            snprintf(base, sizeof base, "%.*s", (int)(dollar - name), name);
            m->trap_fn[i] = mt_lookup_hle(base);
        }
    }
    return MT_TRAP_BASE + 16u * (uint32_t)i;
}

uint32_t mt_call_on(Mt *m, X86 *c, uint32_t fn, int nargs, const uint32_t *args) {
    (void)m;
    uint32_t sp = c->r[ESP], eip = c->eip, stop = c->stop;
    // keep the i386 ABI's 16-byte stack alignment at the call
    uint32_t top = (sp - 64 - 4u * (uint32_t)nargs) & ~15u;
    c->r[ESP] = top;
    for (int i = 0; i < nargs; i++) wr32(c, top + 4u * (uint32_t)i, args[i]);
    x86_push(c, MT_STOP);
    c->eip = fn;
    c->stop = MT_STOP;
    x86_run(c);
    c->r[ESP] = sp;
    c->eip = eip;
    c->stop = stop;
    return c->r[EAX];
}

uint32_t mt_call(Mt *m, uint32_t fn, int nargs, const uint32_t *args) {
    return mt_call_on(m, &m->main->cpu, fn, nargs, args);
}

static pthread_mutex_t g_thread_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_next_stack;
// Stacks (with their errno and pthread_t objects) of threads that have ended,
// for the next thread: there is room below stack_top for only MT_STACK_SLOTS,
// and one more would grow down into the mapped voice data.
static struct { uint32_t top, errno_addr, self; } g_spare[MT_STACK_SLOTS];
static int g_nspare;

MtThread *mt_thread_new(Mt *m) {
    MtThread *t = calloc(1, sizeof *t);
    t->m = m;
    x86_init(&t->cpu, m->mem);
    t->cpu.mem_size = m->mem_size;
    t->cpu.trap_lo = MT_TRAP_BASE;
    t->cpu.trap_hi = MT_TRAP_BASE + 16u * MT_TRAP_SLOTS;
    t->cpu.trap = on_trap;
    t->cpu.trap_ctx = m;
    pthread_mutex_lock(&g_thread_mu);
    if (g_nspare) {
        g_nspare--;
        t->stack_top = g_spare[g_nspare].top;
        t->errno_addr = g_spare[g_nspare].errno_addr;
        t->self = g_spare[g_nspare].self;
    } else if (g_next_stack - 0x100000 >= m->map_hi) {
        t->stack_top = g_next_stack;
        g_next_stack -= 0x100000;  // 1 MB each
    } else {
        pthread_mutex_unlock(&g_thread_mu);
        free(t);
        return NULL;
    }
    pthread_mutex_unlock(&g_thread_mu);
    t->cpu.r[ESP] = t->stack_top - 16;
    t->cpu.abort = &m->dying;
    t->cpu.div0_as_one = 1;
    t->cpu.native = m->native;
    t->cpu.nnative = m->nnative;
    t->cpu.profile = m->profile;
    t->cpu.profile_lo = m->profile_lo;
    t->cpu.profile_hi = m->profile_hi;
    if (!t->errno_addr) t->errno_addr = mt_static(m, 4);
    if (!t->self) t->self = mt_static(m, 64);  // an opaque guest object standing for pthread_t
    return t;
}

int mt_thread_start(void *(*fn)(void *), void *arg) {
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 16 << 20);
#ifdef __APPLE__
    pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    int rc = pthread_create(&th, &at, fn, arg);
    pthread_attr_destroy(&at);
    return rc;
}

// Lock a host mutex for guest code; a thread stuck waiting here faults out
// when the engine shuts down instead of holding the shutdown up forever.
void mt_lock(Mt *m, X86 *c, pthread_mutex_t *mu) {
    for (int spins = 0; pthread_mutex_trylock(mu) != 0; spins++) {
        if (m->dying) x86_fault(c, "engine shut down");
        if (spins < 64) sched_yield();
        else usleep(200);
    }
}

// A guest thread has ended: its stack goes back for the next one.
void mt_thread_free(MtThread *t) {
    if (!t) return;
    pthread_mutex_lock(&g_thread_mu);
    if (g_nspare < (int)MT_STACK_SLOTS) {
        g_spare[g_nspare].top = t->stack_top;
        g_spare[g_nspare].errno_addr = t->errno_addr;
        g_spare[g_nspare].self = t->self;
        g_nspare++;
    }
    pthread_mutex_unlock(&g_thread_mu);
    free(t);
}

// ---------------------------------------------------------------- create

// Messages that must survive in an app extension (where stderr goes nowhere).
void mt_log(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
#ifdef __APPLE__
    os_log_error(OS_LOG_DEFAULT, "CLASSICVOICES MacinTalk: %{public}s", buf);
#endif
}

static void data_imports_init(Mt *m);

Mt *mt_create(char *err, size_t errlen) {
    Mt *m = calloc(1, sizeof *m);
    // The full 2 GB when the process may map it; otherwise the largest smaller layout it may.
    static const struct { uint32_t size, heap_hi; } layouts[] = {
        {0x80000000u, 0x40000000u}, {0x50000000u, 0x20000000u}, {0x40000000u, 0x18000000u},
    };
    m->mem = MAP_FAILED;
    const char *force = getenv("CLASSICVOICES_MT_GUEST_MB");  // testing: pretend only this much can be mapped
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0] && m->mem == MAP_FAILED; i++) {
        if (force && layouts[i].size > (uint32_t)atoi(force) << 20) continue;
        m->mem = mmap(NULL, layouts[i].size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m->mem == MAP_FAILED) continue;
        m->mem_size = layouts[i].size;
        m->heap_hi = m->map_lo = layouts[i].heap_hi;
        m->stack_top = m->mem_size - 0x100000u;
        m->map_hi = m->stack_top - MT_STACK_SLOTS * 0x100000u;
        if (i) mt_log("guest space reduced to %u MB (this process may not map more)", m->mem_size >> 20);
    }
    if (m->mem == MAP_FAILED) { snprintf(err, errlen, "cannot reserve guest memory"); free(m); return NULL; }
    m->data_next = MT_DATA_BASE;
    g_next_stack = m->stack_top;
    g_nspare = 0;
    m->main = mt_thread_new(m);
    m->heap_lo = m->heap_next = MT_IMAGE_BASE;  // moved past the images as they load
    m->map_next = m->map_lo;
    memset(g_free_small, 0, sizeof g_free_small);
    g_free_large = 0;
    // the stop address must be executable by the trap check: it lies below trap_lo,
    // so x86_run simply stops when eip reaches it
    data_imports_init(m);
    return m;
}

void mt_reset_globals(void);  // each module's state (mt_hle.c)
void mt_wake_all(void);
void mt_destroy(Mt *m) {
    if (!m) return;
    m->dying = 1;
    for (int i = 0; i < 2000 && m->live > 0; i++) { mt_wake_all(); usleep(1000); }
    if (m->live > 0) mt_log("%d engine threads did not stop; leaking the engine", m->live);
    if (m->live > 0) return;
    mt_reset_globals();
    munmap(m->mem, m->mem_size);
    for (int i = 0; i < m->nimages; i++) { free(m->images[i].file); free(m->images[i].exports); }
    for (int i = 0; i < m->ntraps; i++) free((void *)m->trap_name[i]);
    free(m);
}

// ---------------------------------------------------------------- loading

static uint8_t *read_file(Mt *m, const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) die(m, "cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) die(m, "cannot read %s", path);
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static const uint8_t *find_i386(Mt *m, const uint8_t *file, size_t len, const char *path) {
    uint32_t magic = u32(file);
    if (magic == MH_MAGIC) return file;
    if (magic != FAT_CIGAM) die(m, "%s is not a Mach-O", path);
    uint32_t n = be32(file + 4);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *a = file + 8 + i * 20;
        if (be32(a) == CPU_TYPE_X86) {
            uint32_t off = be32(a + 8);
            if (off >= len) die(m, "bad fat slice in %s", path);
            return file + off;
        }
    }
    die(m, "no i386 slice in %s", path);
}

static void map_image(Mt *m, MtImage *im) {
    const uint8_t *mh = im->slice;
    uint32_t ncmds = u32(mh + 16), lo = 0xFFFFFFFFu, hi = 0;
    im->flags = u32(mh + 24);
    const uint8_t *p = mh + 28;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = u32(p), size = u32(p + 4);
        if (cmd == LC_SEGMENT) {
            uint32_t vmaddr = u32(p + 24), vmsize = u32(p + 28), initprot = u32(p + 44);
            if (vmsize) {
                if (vmaddr < lo) lo = vmaddr;
                if (vmaddr + vmsize > hi) hi = vmaddr + vmsize;
                if ((initprot & 2) && !im->data_vmaddr) im->data_vmaddr = vmaddr;
            }
        }
        p += size;
    }
    im->lo = lo;
    im->hi = hi;
    uint32_t base = (m->heap_next + 0xFFFFFu) & ~0xFFFFFu;  // 1 MB aligned
    if (base + (hi - lo) > m->heap_hi) die(m, "no room for %s", im->path);
    im->slide = base - lo;
    m->heap_next = base + ((hi - lo + 0xFFFFFu) & ~0xFFFFFu);
    m->heap_lo = m->heap_next;
    if (m->verbose) fprintf(stderr, "%s: vm %08x..%08x -> %08x (slide %08x)\n", im->path, lo, hi, lo + im->slide, im->slide);

    p = mh + 28;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = u32(p), size = u32(p + 4);
        if (cmd == LC_SEGMENT) {
            uint32_t vmaddr = u32(p + 24), vmsize = u32(p + 28), fileoff = u32(p + 32), filesize = u32(p + 36);
            uint32_t nsects = u32(p + 48);
            if (vmsize && filesize) memcpy(m->mem + (uint32_t)(vmaddr + im->slide), im->slice + fileoff, filesize);
            if (im->nsegs < 16) im->segaddr[im->nsegs++] = vmaddr;
            const uint8_t *sec = p + 56;
            for (uint32_t j = 0; j < nsects && im->nsects < 64; j++, sec += 68) {
                typeof(im->sects[0]) *s = &im->sects[im->nsects++];
                memcpy(s->sectname, sec, 16); s->sectname[16] = 0;
                memcpy(s->segname, sec + 16, 16); s->segname[16] = 0;
                s->addr = u32(sec + 32); s->size = u32(sec + 36); s->offset = u32(sec + 40);
                s->flags = u32(sec + 56); s->r1 = u32(sec + 60); s->r2 = u32(sec + 64);
            }
        } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
            im->info = p;
        } else if (cmd == LC_SYMTAB) {
            im->syms = im->slice + u32(p + 8);
            im->nsyms = u32(p + 12);
            im->strs = (const char *)im->slice + u32(p + 16);
        } else if (cmd == LC_DYSYMTAB) {
            im->dys = p;
        }
        p += size;
    }
    if (!im->syms || !im->dys) die(m, "%s has no symbol table", im->path);
}

static uint32_t reloc_base(MtImage *im) { return (im->flags & MH_SPLIT_SEGS) ? im->data_vmaddr : im->lo; }

static void apply_relocs(Mt *m, MtImage *im) {
    const uint8_t *r = im->slice + u32(im->dys + DY_LOCRELOFF);
    uint32_t n = u32(im->dys + DY_NLOCREL), base = reloc_base(im), applied = 0, skipped = 0;
    for (uint32_t i = 0; i < n; i++, r += 8) {
        uint32_t addr = u32(r), info = u32(r + 4);
        if (addr & R_SCATTERED) {
            uint32_t type = (addr >> 24) & 0xF, a = addr & 0x00FFFFFF;
            uint8_t *at = m->mem + (uint32_t)(base + a + im->slide);
            if (type == GENERIC_RELOC_PB_LA_PTR) { uint32_t v = info + im->slide; memcpy(at, &v, 4); applied++; }
            else if (type == GENERIC_RELOC_VANILLA) { uint32_t v = u32(at) + im->slide; memcpy(at, &v, 4); applied++; }
            else skipped++;
        } else {
            uint32_t type = (info >> 28) & 0xF, len = (info >> 25) & 3;
            if (type == GENERIC_RELOC_VANILLA && len == 2) {
                uint8_t *at = m->mem + (uint32_t)(base + addr + im->slide);
                uint32_t v = u32(at) + im->slide;
                memcpy(at, &v, 4);
                applied++;
            } else skipped++;
        }
    }
    if (m->verbose) fprintf(stderr, "  %u local relocations applied, %u skipped\n", applied, skipped);
}

void mt_apply_rebases(Mt *m, MtImage *im);  // mt_dyldinfo.c
void mt_apply_binds(Mt *m, MtImage *im);

int mt_load(Mt *m, const char *path) {
    if (m->nimages >= 8) die(m, "too many images");
    MtImage *im = &m->images[m->nimages];
    memset(im, 0, sizeof *im);
    im->path = strdup(path);
    size_t len;
    im->file = read_file(m, path, &len);
    im->slice = find_i386(m, im->file, len, path);
    map_image(m, im);
    if (im->info) mt_apply_rebases(m, im);
    else apply_relocs(m, im);
    return m->nimages++;
}

// ---------------------------------------------------------------- symbols

static int exportable(const uint8_t *sy) {
    uint8_t type = sy[4];
    if (type & N_STAB) return 0;
    if ((type & N_TYPE) == N_UNDF || (type & N_TYPE) == 0xc) return 0;  // undefined / prebound undefined (N_PBUD)
    return (type & N_EXT) != 0;
}

// Hash index of an image's exports by name; the first symbol of a name wins,
// as in a scan of the table.
static void index_exports(MtImage *im) {
    uint32_t cap = 64;
    while (cap < 2 * im->nsyms) cap *= 2;
    im->exports = calloc(cap, sizeof *im->exports);
    im->exports_mask = cap - 1;
    for (uint32_t k = 0; k < im->nsyms; k++) {
        const uint8_t *sy = im->syms + 12 * k;
        if (!exportable(sy)) continue;
        const char *nm = im->strs + u32(sy);
        uint32_t i = mt_str_hash(nm) & im->exports_mask;
        while (im->exports[i] && strcmp(im->strs + u32(im->syms + 12 * (im->exports[i] - 1)), nm)) i = (i + 1) & im->exports_mask;
        if (!im->exports[i]) im->exports[i] = k + 1;
    }
}

static uint32_t lookup_in_depth(MtImage *im, const char *name, int depth) {
    if (!im->nsyms) return 0;
    if (!im->exports) index_exports(im);
    uint32_t k = 0;
    for (uint32_t i = mt_str_hash(name) & im->exports_mask;; i = (i + 1) & im->exports_mask) {
        if (!im->exports[i]) return 0;  // no exported symbol has this name
        k = im->exports[i] - 1;
        if (!strcmp(im->strs + u32(im->syms + 12 * k), name)) break;
    }
    // From the first symbol of that name on, as a scan of the table would
    for (; k < im->nsyms; k++) {
        const uint8_t *sy = im->syms + 12 * k;
        if (!exportable(sy) || strcmp(im->strs + u32(sy), name)) continue;
        if ((sy[4] & N_TYPE) == N_INDR) {  // an alias: n_value names the target
            const char *alias = im->strs + u32(sy + 8);
            if (depth > 0 && strcmp(alias, name)) return lookup_in_depth(im, alias, depth - 1);
            continue;
        }
        return u32(sy + 8) + im->slide;
    }
    return 0;
}

// "image:symbol+off" for a guest address, for fault messages.
const char *mt_symbolize(Mt *m, uint32_t a, char *out, size_t cap) {
    snprintf(out, cap, "%#x", a);
    for (int i = 0; i < m->nimages; i++) {
        MtImage *im = &m->images[i];
        if (a < im->lo + im->slide || a >= im->hi + im->slide) continue;
        uint32_t best = 0;
        const char *name = NULL;
        for (uint32_t k = 0; k < im->nsyms; k++) {
            const uint8_t *sy = im->syms + 12 * k;
            if ((sy[4] & N_STAB) || (sy[4] & N_TYPE) != 0xe) continue;  // N_SECT
            uint32_t v = u32(sy + 8) + im->slide;
            if (v <= a && v >= best) { best = v; name = im->strs + u32(sy); }
        }
        const char *base = strrchr(im->path, '/');
        snprintf(out, cap, "%s:%s+%#x", base ? base + 1 : im->path, name ? name : "?", a - (name ? best : im->lo + im->slide));
        break;
    }
    return out;
}

uint32_t mt_export(Mt *m, int image, const char *name) { return lookup_in_depth(&m->images[image], name, 4); }

static struct { char name[96]; uint32_t addr; } g_dcache[256];
static int g_ndcache;
static Mt *g_dcache_owner;
void mt_core_reset(void) { g_ndcache = 0; g_dcache_owner = NULL; }

// Resolution order (as panthera): host implementation, then the image itself,
// then any other loaded image; anything left becomes a named trap that faults
// when called.
uint32_t mt_resolve(Mt *m, MtImage *self, const char *name) {
    uint32_t size;
    if (mt_is_data_import(name, &size)) {
        if (g_dcache_owner != m) { g_ndcache = 0; g_dcache_owner = m; }
#define cache g_dcache
#define ncache g_ndcache
        for (int i = 0; i < ncache; i++) if (!strcmp(cache[i].name, name)) return cache[i].addr;
        uint32_t a = mt_static(m, size);
        mt_init_data_import(m, name, a);
        if (ncache < 256) { snprintf(cache[ncache].name, sizeof cache[0].name, "%s", name); cache[ncache++].addr = a; }
        return a;
#undef cache
#undef ncache
    }
    MtHle h = mt_lookup_hle(name);
    if (h) return trap_for(m, name);
    uint32_t a = lookup_in_depth(self, name, 4);
    if (a) return a;
    for (int i = 0; i < m->nimages; i++) {
        if (&m->images[i] == self) continue;
        a = lookup_in_depth(&m->images[i], name, 4);
        if (a) return a;
    }
    return trap_for(m, name);
}

static void apply_ext_relocs(Mt *m, MtImage *im) {
    const uint8_t *r = im->slice + u32(im->dys + DY_EXTRELOFF);
    uint32_t n = u32(im->dys + DY_NEXTREL), base = reloc_base(im), done = 0, missed = 0;
    int prebound = (im->flags & MH_PREBOUND) != 0;
    for (uint32_t i = 0; i < n; i++, r += 8) {
        uint32_t addr = u32(r), info = u32(r + 4);
        uint32_t symnum = info & 0x00FFFFFF, pcrel = (info >> 24) & 1, len = (info >> 25) & 3;
        uint32_t ext = (info >> 27) & 1, type = (info >> 28) & 0xF;
        if (!ext || type != GENERIC_RELOC_VANILLA || len != 2 || pcrel || symnum >= im->nsyms) { missed++; continue; }
        const uint8_t *sy = im->syms + 12 * symnum;
        const char *nm = im->strs + u32(sy);
        uint32_t target = mt_resolve(m, im, nm);
        uint8_t *at = m->mem + (uint32_t)(base + addr + im->slide);
        uint32_t v = u32(at);
        if (prebound) v -= u32(sy + 8);
        v += target;
        memcpy(at, &v, 4);
        done++;
    }
    if (m->verbose) fprintf(stderr, "  %u external relocations applied, %u skipped\n", done, missed);
}

static void bind_image(Mt *m, MtImage *im) {
    const uint8_t *ind = im->slice + u32(im->dys + DY_INDIRECTSYMOFF);
    int bound = 0, stubs = 0, local = 0;
    for (int i = 0; i < im->nsects; i++) {
        typeof(im->sects[0]) *s = &im->sects[i];
        uint32_t t = s->flags & 0xFF, stride = 4;
        if (t == S_SYMBOL_STUBS) {
            if (s->r2 != 5) continue;  // only the self-modifying 5-byte __IMPORT jump tables
            stride = 5;
        } else if (t != S_NON_LAZY_SYMBOL_PTR && t != S_LAZY_SYMBOL_PTR) {
            continue;
        } else if (im->info) {
            continue;  // the dyld info streams own the pointer sections (10.6+)
        }
        uint32_t n = s->size / stride;
        for (uint32_t j = 0; j < n; j++) {
            uint32_t isym = u32(ind + 4 * (s->r1 + j));
            uint32_t slot = s->addr + im->slide + j * stride;
            if (stride == 5) {
                if (isym & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) || isym >= im->nsyms) continue;
                const char *nm = im->strs + u32(im->syms + 12 * isym);
                uint32_t fn = mt_resolve(m, im, nm);
                m->mem[slot] = 0xE9;  // jmp rel32
                uint32_t rel = fn - (slot + 5);
                memcpy(m->mem + slot + 1, &rel, 4);
                stubs++;
                continue;
            }
            if (isym & INDIRECT_SYMBOL_LOCAL) {
                uint32_t v = u32(m->mem + slot) + im->slide;
                memcpy(m->mem + slot, &v, 4);
                local++;
                continue;
            }
            if (isym & INDIRECT_SYMBOL_ABS || isym >= im->nsyms) continue;
            const char *nm = im->strs + u32(im->syms + 12 * isym);
            uint32_t fn = mt_resolve(m, im, nm);
            memcpy(m->mem + slot, &fn, 4);
            bound++;
        }
    }
    if (m->verbose) fprintf(stderr, "%s: bound %d pointers, %d stubs, %d local\n", im->path, bound, stubs, local);
}

void mt_bind_all(Mt *m) {
    for (int i = 0; i < m->nimages; i++) {
        MtImage *im = &m->images[i];
        if (im->info) mt_apply_binds(m, im);
        bind_image(m, im);
        if (!im->info) apply_ext_relocs(m, im);
    }
}

void mt_run_initializers(Mt *m) {
    for (int i = 0; i < m->nimages; i++) {
        MtImage *im = &m->images[i];
        for (int k = 0; k < im->nsects; k++) {
            if ((im->sects[k].flags & 0xFF) != S_MOD_INIT_FUNC) continue;
            for (uint32_t j = 0; j < im->sects[k].size / 4; j++) {
                uint32_t fn = u32(m->mem + (uint32_t)(im->sects[k].addr + im->slide + 4 * j));
                if (m->verbose) fprintf(stderr, "%s: initializer %u -> %#x\n", im->path, j, fn);
                mt_call(m, fn, 0, NULL);
            }
        }
    }
}

// ---------------------------------------------------------------- data imports

static void data_imports_init(Mt *m) { (void)m; }
