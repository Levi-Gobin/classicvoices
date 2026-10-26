// Attaching recompiled code to an engine, and the dump/profile modes that
// produce it (tools/build_mt_native.sh).
#include "mt_native.h"
#include "mt_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>

#define MAX_SETS 8
static const MtNativeSet *g_sets[MAX_SETS];
static int g_nsets;

void mt_native_register(const MtNativeSet *s) {
    if (g_nsets < MAX_SETS) g_sets[g_nsets++] = s;
}

uint64_t mt_native_hash(const uint8_t *p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

// Hashing the code takes milliseconds; an engine reopened from the same files
// (after a cancel shut it down, say) reuses the last answer.
static struct { char key[2048]; const MtNativeSet *set; int valid; } g_last;
static pthread_mutex_t g_last_mu = PTHREAD_MUTEX_INITIALIZER;

static void files_key(Mt *m, char *key, size_t cap) {
    size_t n = 0;
    key[0] = 0;
    for (int i = 0; i < m->nimages && n < cap; i++) {
        struct stat st;
        if (stat(m->images[i].path, &st)) { key[0] = 0; return; }
        n += (size_t)snprintf(key + n, cap - n, "%s:%lld:%ld.%09ld;", m->images[i].path, (long long)st.st_size,
                              (long)st.st_mtimespec.tv_sec, (long)st.st_mtimespec.tv_nsec);
    }
    if (n >= cap) key[0] = 0;  // too long to remember
}

static int set_matches(Mt *m, const MtNativeSet *s) {
    for (size_t i = 0; i < s->nranges; i++) {
        const MtNativeRange *r = &s->ranges[i];
        if (r->hi <= r->lo || r->hi > m->mem_size) return 0;
        if (mt_native_hash(m->mem + r->lo, r->hi - r->lo) != r->hash) return 0;
    }
    return s->nranges > 0;
}

static void dump(Mt *m, const char *dir) {
    mkdir(dir, 0755);
    char p[1200];
    snprintf(p, sizeof p, "%s/images.txt", dir);
    FILE *f = fopen(p, "w");
    if (!f) { mt_log("MT_DUMP: cannot write %s", p); return; }
    fprintf(f, "trap %#x %#x\n", MT_TRAP_BASE, MT_TRAP_BASE + 16u * MT_TRAP_SLOTS);
    for (int i = 0; i < m->nimages; i++) {
        MtImage *im = &m->images[i];
        const char *name = base_name(im->path);
        uint32_t lo = im->lo + im->slide, hi = im->hi + im->slide;
        fprintf(f, "image %s %#x %#x %#x\n", name, lo, hi, im->slide);
        for (int k = 0; k < im->nsects; k++)
            fprintf(f, "sect %s %s %s %#x %#x %#x\n", name, im->sects[k].segname, im->sects[k].sectname,
                    im->sects[k].addr + im->slide, im->sects[k].size, im->sects[k].flags);
        for (uint32_t k = 0; k < im->nsyms; k++) {
            const uint8_t *sy = im->syms + 12 * k;
            uint32_t strx, value;
            memcpy(&strx, sy, 4);
            memcpy(&value, sy + 8, 4);
            uint8_t type = sy[4], sect = sy[5];
            if ((type & 0xE0) || (type & 0x0E) != 0x0E || !sect) continue;  // defined in a section, not a debug entry
            fprintf(f, "sym %s %#x %s\n", name, value + im->slide, im->strs + strx);
        }
        snprintf(p, sizeof p, "%s/%s.mem", dir, name);
        FILE *g = fopen(p, "wb");
        if (g) { fwrite(m->mem + lo, 1, hi - lo, g); fclose(g); }
    }
    fclose(f);
}

void mt_native_attach(Mt *m) {
    const char *d = getenv("MT_DUMP");
    if (d && *d) dump(m, d);
    if (getenv("MT_PROFILE") && m->nimages) {
        m->profile_lo = MT_IMAGE_BASE;
        m->profile_hi = m->heap_lo;
        m->profile = calloc(m->profile_hi - m->profile_lo, sizeof *m->profile);
    }
    const char *off = getenv("MT_NATIVE");
    if (!(off && !strcmp(off, "0"))) {
        const MtNativeSet *used = NULL;
        char key[2048];
        files_key(m, key, sizeof key);
        pthread_mutex_lock(&g_last_mu);
        if (key[0] && g_last.valid && !strcmp(key, g_last.key)) {
            used = g_last.set;
        } else {
            for (int i = 0; i < g_nsets && !used; i++)
                if (set_matches(m, g_sets[i])) used = g_sets[i];
            if (key[0]) { snprintf(g_last.key, sizeof g_last.key, "%s", key); g_last.set = used; g_last.valid = 1; }
        }
        pthread_mutex_unlock(&g_last_mu);
        if (used) {
            m->native = used->table;
            m->nnative = used->count;
            mt_log("recompiled code %s: %zu entry points", used->name, used->count);
        } else if (g_nsets) {
            mt_log("no recompiled code matches this engine; interpreting it");
        }
    }
    X86 *c = &m->main->cpu;
    c->native = m->native; c->nnative = m->nnative;
    c->profile = m->profile; c->profile_lo = m->profile_lo; c->profile_hi = m->profile_hi;
}

typedef struct { char *key; unsigned long long count; } Row;
static int cmp_row(const void *a, const void *b) { return strcmp(((const Row *)a)->key, ((const Row *)b)->key); }

// Lines "<image> <address before sliding> <instructions>": where control
// landed and how many instructions ran from there before it moved again;
// added to what the file had.
void mt_native_finish(Mt *m) {
    const char *path = getenv("MT_PROFILE");
    if (!path || !m->profile) return;
    size_t cap = 1 << 16, n = 0;
    Row *rows = malloc(cap * sizeof *rows);
    FILE *f = fopen(path, "r");
    char buf[256];
    while (f && fgets(buf, sizeof buf, f)) {
        char img[200];
        unsigned addr;
        unsigned long long count = 1;
        if (buf[0] == '#' || sscanf(buf, "%199s %x %llu", img, &addr, &count) < 2) continue;
        if (n == cap) rows = realloc(rows, (cap *= 2) * sizeof *rows);
        snprintf(buf, sizeof buf, "%s %08x", img, addr);
        rows[n].key = strdup(buf);
        rows[n++].count = count;
    }
    if (f) fclose(f);
    for (uint32_t a = m->profile_lo; a < m->profile_hi; a++) {
        uint32_t count = m->profile[a - m->profile_lo];
        if (!count) continue;
        for (int i = 0; i < m->nimages; i++) {
            MtImage *im = &m->images[i];
            if (a < im->lo + im->slide || a >= im->hi + im->slide) continue;
            if (n == cap) rows = realloc(rows, (cap *= 2) * sizeof *rows);
            snprintf(buf, sizeof buf, "%s %08x", base_name(im->path), a - im->slide);
            rows[n].key = strdup(buf);
            rows[n++].count = count;
            break;
        }
    }
    qsort(rows, n, sizeof *rows, cmp_row);
    f = fopen(path, "w");
    for (size_t i = 0; i < n;) {
        size_t j = i;
        unsigned long long sum = 0;
        for (; j < n && !strcmp(rows[j].key, rows[i].key); j++) sum += rows[j].count;
        if (f) fprintf(f, "%s %llu\n", rows[i].key, sum);
        i = j;
    }
    if (f) fclose(f);
    for (size_t i = 0; i < n; i++) free(rows[i].key);
    free(rows);
    free(m->profile);
    m->profile = NULL;
}
