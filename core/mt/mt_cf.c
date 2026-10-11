// A small CoreFoundation for the MacinTalk guest. Objects live in guest
// memory so the engine can hold them; bundles and URLs are file paths (as in
// panthera-speech's tiger_host_cf.c). Released objects are not freed: the
// engine creates few of them, and some are used after their last release.
#include "mt_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Object layout (48 bytes). The first 16 bytes match a constant CFString
// ({isa, flags, cstr, length}), so the engine's CFSTR() constants and our
// strings go through the same accessors.
enum { O_ISA = 0, O_FLAGS = 4, O_CSTR = 8, O_LEN = 12, O_MAGIC = 16, O_KIND = 20, O_RC = 24, O_PTR = 28, O_N = 32, O_CAP = 36, O_NUM = 40, O_SIZE = 48 };
enum { K_STRING, K_NUMBER, K_BOOLEAN, K_DATA, K_ARRAY, K_DICT, K_URL, K_BUNDLE };
#define CF_MAGIC 0x4D544346u  // 'MTCF'

// real CoreFoundation type IDs (10.5), so comparisons the engine makes are sane
static const uint32_t kTypeID[] = {7, 22, 21, 20, 19, 18, 29, 31};

static uint32_t g_isa;          // ___CFConstantStringClassReference (or our own, if made first)
static uint32_t g_isa_import;   // the engine's ___CFConstantStringClassReference
static uint32_t g_true, g_false;
static uint32_t g_dict_bundle;  // SpeechDictionary.framework (CFBundleGetBundleWithIdentifier)

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }
static inline uint32_t f32(Mt *m, uint32_t o, int off) { uint32_t v; memcpy(&v, G(m, o + (uint32_t)off), 4); return v; }
static inline void s32_(Mt *m, uint32_t o, int off, uint32_t v) { memcpy(G(m, o + (uint32_t)off), &v, 4); }

static uint32_t isa(Mt *m) {
    if (!g_isa) g_isa = mt_static(m, 32);
    return g_isa;
}

static uint32_t cf_new(Mt *m, int kind) {
    uint32_t o = mt_calloc(m, O_SIZE);
    s32_(m, o, O_ISA, isa(m));
    s32_(m, o, O_FLAGS, 0x7C8);
    s32_(m, o, O_MAGIC, CF_MAGIC);
    s32_(m, o, O_KIND, (uint32_t)kind);
    s32_(m, o, O_RC, 1);
    return o;
}
static int ours(Mt *m, uint32_t o) { return o && f32(m, o, O_MAGIC) == CF_MAGIC; }
static int kind_of(Mt *m, X86 *c, uint32_t o) {
    if (!o) x86_fault(c, "CF call on NULL");
    if (ours(m, o)) return (int)f32(m, o, O_KIND);
    if (f32(m, o, O_ISA) == g_isa || (g_isa_import && f32(m, o, O_ISA) == g_isa_import)) return K_STRING;  // the engine's own CFSTR()
    x86_fault(c, "not a CF object: %#x", o);
}

uint32_t mt_cf_string(Mt *m, const char *s, size_t n) {
    uint32_t o = cf_new(m, K_STRING), buf = mt_malloc(m, (uint32_t)n + 1);
    memcpy(G(m, buf), s, n);
    G(m, buf)[n] = 0;
    s32_(m, o, O_CSTR, buf);
    s32_(m, o, O_LEN, (uint32_t)n);
    return o;
}
static uint32_t cf_cstr(Mt *m, X86 *c, uint32_t s) {
    if (kind_of(m, c, s) != K_STRING && kind_of(m, c, s) != K_URL && kind_of(m, c, s) != K_BUNDLE) x86_fault(c, "not a CFString: %#x", s);
    return f32(m, s, O_CSTR);
}
static uint32_t cf_len(Mt *m, uint32_t s) { return f32(m, s, O_LEN); }
const char *mt_cf_text(Mt *m, uint32_t s) { return (const char *)G(m, f32(m, s, O_CSTR)); }

static uint32_t cf_path_obj(Mt *m, int kind, const char *path) {
    uint32_t o = mt_cf_string(m, path, strlen(path));
    s32_(m, o, O_KIND, (uint32_t)kind);
    return o;
}
uint32_t mt_cf_bundle(Mt *m, const char *dir) { return cf_path_obj(m, K_BUNDLE, dir); }
void mt_cf_set_dictionary_bundle(Mt *m, const char *dir) { g_dict_bundle = mt_cf_bundle(m, dir); }

void mt_cf_init_data(Mt *m, const char *name, uint32_t addr) {
    if (!strcmp(name, "___CFConstantStringClassReference")) { g_isa_import = addr; if (!g_isa) g_isa = addr; return; }
    if (!strcmp(name, "_kCFBooleanTrue") || !strcmp(name, "_kCFBooleanFalse")) {
        uint32_t *slot = !strcmp(name, "_kCFBooleanTrue") ? &g_true : &g_false;
        if (!*slot) { *slot = cf_new(m, K_BOOLEAN); s32_(m, *slot, O_N, slot == &g_true); s32_(m, *slot, O_RC, 0x40000000); }
        memcpy(G(m, addr), slot, 4);
        return;
    }
    if (!strcmp(name, "_kCFRunLoopDefaultMode") || !strcmp(name, "_kCFRunLoopCommonModes")) {
        uint32_t s = mt_cf_string(m, name + 1, strlen(name + 1));
        memcpy(G(m, addr), &s, 4);
    }
}

// ---------------------------------------------------------------- generic

static void h_CFRetain(Mt *m, X86 *c) {
    uint32_t o = A(c, 0);
    if (ours(m, o)) s32_(m, o, O_RC, f32(m, o, O_RC) + 1);
    R(c, o);
}
static void h_CFRelease(Mt *m, X86 *c) {
    uint32_t o = A(c, 0);
    if (ours(m, o) && f32(m, o, O_RC) && f32(m, o, O_RC) < 0x40000000) s32_(m, o, O_RC, f32(m, o, O_RC) - 1);
}
static void h_CFGetTypeID(Mt *m, X86 *c) { R(c, kTypeID[kind_of(m, c, A(c, 0))]); }
static void h_CFStringTypeID(Mt *m, X86 *c) { (void)m; R(c, kTypeID[K_STRING]); }
static void h_CFNumberGetTypeID(Mt *m, X86 *c) { (void)m; R(c, kTypeID[K_NUMBER]); }
static void h_CFBooleanGetTypeID(Mt *m, X86 *c) { (void)m; R(c, kTypeID[K_BOOLEAN]); }
static void h_CFDictionaryGetTypeID(Mt *m, X86 *c) { (void)m; R(c, kTypeID[K_DICT]); }
static void h_CFArrayGetTypeID(Mt *m, X86 *c) { (void)m; R(c, kTypeID[K_ARRAY]); }
static void h_CFDataGetTypeID(Mt *m, X86 *c) { (void)m; R(c, kTypeID[K_DATA]); }

// ---------------------------------------------------------------- strings

static void h_CFStringCreateWithCString(Mt *m, X86 *c) {
    uint32_t s = A(c, 1);
    if (!s) { R(c, 0); return; }
    const char *p = mt_str(m, s);
    R(c, mt_cf_string(m, p, strlen(p)));
}
static void h_CFStringCreateWithSubstring(Mt *m, X86 *c) {
    uint32_t s = A(c, 1), loc = A(c, 2), len = A(c, 3);
    uint32_t p = cf_cstr(m, c, s), n = cf_len(m, s);
    if (loc > n) loc = n;
    if (len > n - loc) len = n - loc;
    R(c, mt_cf_string(m, (const char *)G(m, p + loc), len));
}
static void h_CFStringGetLength(Mt *m, X86 *c) { cf_cstr(m, c, A(c, 0)); R(c, cf_len(m, A(c, 0))); }
static void h_CFStringGetCStringPtr(Mt *m, X86 *c) { R(c, cf_cstr(m, c, A(c, 0))); }
static void h_CFStringGetCString(Mt *m, X86 *c) {
    uint32_t s = A(c, 0), buf = A(c, 1), size = A(c, 2);
    uint32_t p = cf_cstr(m, c, s), n = cf_len(m, s);
    if (n + 1 > size) { R(c, 0); return; }
    mt_check(m, c, buf, n + 1);
    memcpy(G(m, buf), G(m, p), n);
    G(m, buf)[n] = 0;
    R(c, 1);
}
static void h_CFStringGetMaximumSizeForEncoding(Mt *m, X86 *c) { (void)m; R(c, A(c, 0) * 3); }
static void h_CFStringGetMaximumSizeOfFileSystemRepresentation(Mt *m, X86 *c) { R(c, cf_len(m, A(c, 0)) * 3 + 1); }
static void h_CFStringGetFileSystemRepresentation(Mt *m, X86 *c) { h_CFStringGetCString(m, c); }
static void h_CFStringFind(Mt *m, X86 *c) {
    // CFRange CFStringFind(str, find, options): an 8-byte struct comes back in EAX:EDX
    uint32_t s = A(c, 0), f = A(c, 1);
    const char *hay = (const char *)G(m, cf_cstr(m, c, s)), *needle = (const char *)G(m, cf_cstr(m, c, f));
    uint32_t opt = A(c, 2);
    const char *hit = NULL;
    if (opt & 1) {  // kCFCompareCaseInsensitive
        size_t nl = strlen(needle);
        for (const char *p = hay; *p && !hit; p++) if (!strncasecmp(p, needle, nl)) hit = p;
    } else if (opt & 4) {  // kCFCompareBackwards
        size_t nl = strlen(needle);
        for (const char *p = hay + strlen(hay); p >= hay && !hit; p--) if (!strncmp(p, needle, nl)) hit = p;
    } else hit = strstr(hay, needle);
    if (hit) { c->r[EAX] = (uint32_t)(hit - hay); c->r[EDX] = (uint32_t)strlen(needle); }
    else { c->r[EAX] = ~0u; c->r[EDX] = 0; }  // kCFNotFound
}
static void h_CFStringCreateExternalRepresentation(Mt *m, X86 *c) {
    uint32_t s = A(c, 1), p = cf_cstr(m, c, s), n = cf_len(m, s);
    uint32_t d = cf_new(m, K_DATA), b = mt_malloc(m, n + 1);
    memcpy(G(m, b), G(m, p), n);
    s32_(m, d, O_PTR, b);
    s32_(m, d, O_N, n);
    R(c, d);
}

// ---------------------------------------------------------------- numbers / booleans

static void h_CFNumberCreate(Mt *m, X86 *c) {
    uint32_t type = A(c, 1), p = A(c, 2);
    double v;
    switch (type) {
    case 1: case 7: v = (int8_t)rd8(c, p); break;
    case 2: case 8: v = (int16_t)rd16(c, p); break;
    case 3: case 9: case 10: case 14: v = (int32_t)rd32(c, p); break;
    case 4: case 11: v = (double)(int64_t)rd64(c, p); break;
    case 5: case 12: { uint32_t u = rd32(c, p); float f; memcpy(&f, &u, 4); v = f; break; }
    default: { uint64_t u = rd64(c, p); memcpy(&v, &u, 8); break; }
    }
    uint32_t o = cf_new(m, K_NUMBER);
    memcpy(G(m, o + O_NUM), &v, 8);
    s32_(m, o, O_N, type);
    R(c, o);
}
static double num_of(Mt *m, X86 *c, uint32_t o) {
    int k = kind_of(m, c, o);
    if (k == K_BOOLEAN) return f32(m, o, O_N);
    if (k != K_NUMBER) x86_fault(c, "not a CFNumber: %#x", o);
    double v;
    memcpy(&v, G(m, o + O_NUM), 8);
    return v;
}
static void h_CFNumberGetValue(Mt *m, X86 *c) {
    double v = num_of(m, c, A(c, 0));
    uint32_t type = A(c, 1), p = A(c, 2);
    switch (type) {
    case 1: case 7: wr8(c, p, (uint8_t)(int8_t)v); break;
    case 2: case 8: wr16(c, p, (uint16_t)(int16_t)v); break;
    case 3: case 9: case 10: case 14: wr32(c, p, (uint32_t)(int32_t)v); break;
    case 4: case 11: wr64(c, p, (uint64_t)(int64_t)v); break;
    case 5: case 12: { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); wr32(c, p, u); break; }
    default: { uint64_t u; memcpy(&u, &v, 8); wr64(c, p, u); break; }
    }
    R(c, 1);
}
static void h_CFBooleanGetValue(Mt *m, X86 *c) { R(c, num_of(m, c, A(c, 0)) != 0); }

// ---------------------------------------------------------------- data

static uint32_t data_from(Mt *m, const void *bytes, uint32_t n) {
    uint32_t d = cf_new(m, K_DATA), b = mt_malloc(m, n ? n : 1);
    if (n) memcpy(G(m, b), bytes, n);
    s32_(m, d, O_PTR, b);
    s32_(m, d, O_N, n);
    return d;
}
static void h_CFDataCreate(Mt *m, X86 *c) { uint32_t n = A(c, 2); mt_check(m, c, A(c, 1), n); R(c, data_from(m, G(m, A(c, 1)), n)); }
static void h_CFDataGetBytePtr(Mt *m, X86 *c) { if (kind_of(m, c, A(c, 0)) != K_DATA) x86_fault(c, "not CFData"); R(c, f32(m, A(c, 0), O_PTR)); }
static void h_CFDataGetLength(Mt *m, X86 *c) { if (kind_of(m, c, A(c, 0)) != K_DATA) x86_fault(c, "not CFData"); R(c, f32(m, A(c, 0), O_N)); }

// ---------------------------------------------------------------- arrays / dictionaries

static void push_value(Mt *m, uint32_t o, uint32_t v) {
    uint32_t n = f32(m, o, O_N), cap = f32(m, o, O_CAP), p = f32(m, o, O_PTR);
    if (n == cap) {
        uint32_t ncap = cap ? cap * 2 : 8, np = mt_malloc(m, ncap * 4);
        if (n) memcpy(G(m, np), G(m, p), n * 4);
        s32_(m, o, O_PTR, np);
        s32_(m, o, O_CAP, ncap);
        p = np;
    }
    memcpy(G(m, p + 4 * n), &v, 4);
    s32_(m, o, O_N, n + 1);
    if (ours(m, v)) s32_(m, v, O_RC, f32(m, v, O_RC) + 1);
}
static void h_CFArrayCreate(Mt *m, X86 *c) {
    uint32_t vals = A(c, 1), n = A(c, 2), o = cf_new(m, K_ARRAY);
    for (uint32_t i = 0; i < n; i++) push_value(m, o, rd32(c, vals + 4 * i));
    R(c, o);
}
static void h_CFArrayCreateMutable(Mt *m, X86 *c) { (void)c; R(c, cf_new(m, K_ARRAY)); }
static void h_CFArrayAppendValue(Mt *m, X86 *c) { if (kind_of(m, c, A(c, 0)) != K_ARRAY) x86_fault(c, "not CFArray"); push_value(m, A(c, 0), A(c, 1)); }
static void h_CFArrayGetCount(Mt *m, X86 *c) { R(c, f32(m, A(c, 0), O_N)); }
static void h_CFArrayGetValueAtIndex(Mt *m, X86 *c) {
    uint32_t o = A(c, 0), i = A(c, 1);
    if (i >= f32(m, o, O_N)) x86_fault(c, "CFArray index %u out of range", i);
    R(c, rd32(c, f32(m, o, O_PTR) + 4 * i));
}

// dictionary storage: key, value pairs in one array (N counts pairs)
static int key_eq(Mt *m, X86 *c, uint32_t a, uint32_t b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    int ka = kind_of(m, c, a), kb = kind_of(m, c, b);
    if (ka == K_STRING && kb == K_STRING)
        return cf_len(m, a) == cf_len(m, b) && !memcmp(G(m, f32(m, a, O_CSTR)), G(m, f32(m, b, O_CSTR)), cf_len(m, a));
    if (ka == K_NUMBER && kb == K_NUMBER) return num_of(m, c, a) == num_of(m, c, b);
    return 0;
}
static void dict_set(Mt *m, X86 *c, uint32_t d, uint32_t k, uint32_t v) {
    uint32_t n = f32(m, d, O_N), p = f32(m, d, O_PTR);
    for (uint32_t i = 0; i < n; i++)
        if (key_eq(m, c, rd32(c, p + 8 * i), k)) { wr32(c, p + 8 * i + 4, v); return; }
    uint32_t cap = f32(m, d, O_CAP);
    if (n == cap) {
        uint32_t ncap = cap ? cap * 2 : 8, np = mt_malloc(m, ncap * 8);
        if (n) memcpy(G(m, np), G(m, p), n * 8);
        s32_(m, d, O_PTR, np);
        s32_(m, d, O_CAP, ncap);
        p = np;
    }
    wr32(c, p + 8 * n, k);
    wr32(c, p + 8 * n + 4, v);
    s32_(m, d, O_N, n + 1);
}
static uint32_t dict_get(Mt *m, X86 *c, uint32_t d, uint32_t k, int *found) {
    if (kind_of(m, c, d) != K_DICT) x86_fault(c, "not CFDictionary: %#x", d);
    uint32_t n = f32(m, d, O_N), p = f32(m, d, O_PTR);
    for (uint32_t i = 0; i < n; i++)
        if (key_eq(m, c, rd32(c, p + 8 * i), k)) { if (found) *found = 1; return rd32(c, p + 8 * i + 4); }
    if (found) *found = 0;
    return 0;
}
static void h_CFDictionaryCreate(Mt *m, X86 *c) {
    uint32_t keys = A(c, 1), vals = A(c, 2), n = A(c, 3), d = cf_new(m, K_DICT);
    for (uint32_t i = 0; i < n; i++) dict_set(m, c, d, rd32(c, keys + 4 * i), rd32(c, vals + 4 * i));
    R(c, d);
}
static void h_CFDictionaryCreateMutable(Mt *m, X86 *c) { R(c, cf_new(m, K_DICT)); }
static void h_CFDictionarySetValue(Mt *m, X86 *c) { dict_set(m, c, A(c, 0), A(c, 1), A(c, 2)); }
static void h_CFDictionaryCreateCopy(Mt *m, X86 *c) {
    uint32_t s = A(c, 1), d = cf_new(m, K_DICT), n = f32(m, s, O_N), p = f32(m, s, O_PTR);
    for (uint32_t i = 0; i < n; i++) dict_set(m, c, d, rd32(c, p + 8 * i), rd32(c, p + 8 * i + 4));
    R(c, d);
}
static void h_CFDictionaryGetValue(Mt *m, X86 *c) { R(c, dict_get(m, c, A(c, 0), A(c, 1), NULL)); }
static void h_CFDictionaryGetValueIfPresent(Mt *m, X86 *c) {
    int found;
    uint32_t v = dict_get(m, c, A(c, 0), A(c, 1), &found);
    if (found && A(c, 2)) wr32(c, A(c, 2), v);
    R(c, (uint32_t)found);
}
static void h_CFDictionaryGetCount(Mt *m, X86 *c) { R(c, f32(m, A(c, 0), O_N)); }

// ---------------------------------------------------------------- bundles / URLs / files

static void h_CFBundleGetBundleWithIdentifier(Mt *m, X86 *c) { (void)m; R(c, g_dict_bundle); }
static void h_CFBundleCopyResourceURL(Mt *m, X86 *c) {
    uint32_t b = A(c, 0), name = A(c, 1), type = A(c, 2);
    if (!b || !name) { R(c, 0); return; }
    const char *dir = (const char *)G(m, cf_cstr(m, c, b)), *nm = (const char *)G(m, cf_cstr(m, c, name));
    const char *ext = type ? (const char *)G(m, cf_cstr(m, c, type)) : NULL;
    static const char *layouts[] = {"Contents/Resources", "Resources", ""};
    char path[2048];
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s%s%s/%s%s%s", dir, *layouts[i] ? "/" : "", layouts[i], nm, ext && *ext ? "." : "", ext ? ext : "");
        if (access(path, R_OK) == 0) {
            if (m->verbose) fprintf(stderr, "[cf] resource %s\n", path);
            R(c, cf_path_obj(m, K_URL, path));
            return;
        }
    }
    if (m->verbose) fprintf(stderr, "[cf] no resource %s under %s\n", nm, dir);
    R(c, 0);
}
static void h_CFBundleGetVersionNumber(Mt *m, X86 *c) { (void)m; R(c, 0x01008000); }
static void h_CFURLCreateWithFileSystemPath(Mt *m, X86 *c) { R(c, cf_path_obj(m, K_URL, (const char *)G(m, cf_cstr(m, c, A(c, 1))))); }
static void h_CFURLCreateFromFileSystemRepresentation(Mt *m, X86 *c) {
    uint32_t n = A(c, 2);
    mt_check(m, c, A(c, 1), n);
    char path[2048];
    snprintf(path, sizeof path, "%.*s", (int)n, (const char *)G(m, A(c, 1)));
    R(c, cf_path_obj(m, K_URL, path));
}
static void h_CFURLCopyPath(Mt *m, X86 *c) { R(c, cf_path_obj(m, K_STRING, (const char *)G(m, cf_cstr(m, c, A(c, 0))))); }
static void h_CFURLCopyFileSystemPath(Mt *m, X86 *c) { R(c, cf_path_obj(m, K_STRING, (const char *)G(m, cf_cstr(m, c, A(c, 0))))); }
static void h_CFURLCreateDataAndPropertiesFromResource(Mt *m, X86 *c) {
    uint32_t url = A(c, 1), out = A(c, 2), props = A(c, 3), errp = A(c, 5);
    if (props) wr32(c, props, 0);
    const char *path = url ? (const char *)G(m, cf_cstr(m, c, url)) : NULL;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { if (errp) wr32(c, errp, (uint32_t)-15); R(c, 0); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t d = cf_new(m, K_DATA), b = mt_malloc(m, (uint32_t)(n > 0 ? n : 1));
    size_t got = fread(G(m, b), 1, (size_t)n, f);
    fclose(f);
    s32_(m, d, O_PTR, b);
    s32_(m, d, O_N, (uint32_t)got);
    if (out) wr32(c, out, d);
    if (errp) wr32(c, errp, 0);
    R(c, 1);
}
// FSRef (80 bytes): we keep a guest pointer to a path string in the first word
static void h_CFURLGetFSRef(Mt *m, X86 *c) { wr32(c, A(c, 1), mt_strdup(m, (const char *)G(m, cf_cstr(m, c, A(c, 0))))); R(c, 1); }
static void h_FSPathMakeRef(Mt *m, X86 *c) {
    const char *p = mt_str(m, A(c, 0));
    if (access(p, F_OK)) { R(c, (uint32_t)-43); return; }  // fnfErr
    wr32(c, A(c, 1), mt_strdup(m, p));
    if (A(c, 2)) wr8(c, A(c, 2), 0);
    R(c, 0);
}
static void h_FSDeleteObject(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)-54); }  // permErr: never delete anything
static void h_DisposeHandle(Mt *m, X86 *c) { (void)m; (void)c; }
static void h_CFPreferencesCopyAppValue(Mt *m, X86 *c) { (void)m; R(c, 0); }

// ---------------------------------------------------------------- XML property lists

typedef struct { Mt *m; X86 *c; const char *p, *end; } Px;
static void px_skip(Px *x) {
    for (;;) {
        while (x->p < x->end && isspace((unsigned char)*x->p)) x->p++;
        if (x->p + 4 <= x->end && !strncmp(x->p, "<!--", 4)) { const char *e = strstr(x->p, "-->"); x->p = e ? e + 3 : x->end; continue; }
        if (x->p + 2 <= x->end && (!strncmp(x->p, "<?", 2) || !strncmp(x->p, "<!", 2))) { const char *e = memchr(x->p, '>', (size_t)(x->end - x->p)); x->p = e ? e + 1 : x->end; continue; }
        return;
    }
}
static int px_tag(Px *x, char *name, size_t cap, int *closing, int *empty) {
    px_skip(x);
    if (x->p >= x->end || *x->p != '<') return 0;
    x->p++;
    *closing = *x->p == '/';
    if (*closing) x->p++;
    size_t n = 0;
    while (x->p < x->end && *x->p != '>' && *x->p != '/' && !isspace((unsigned char)*x->p)) { if (n + 1 < cap) name[n++] = *x->p; x->p++; }
    name[n] = 0;
    while (x->p < x->end && *x->p != '>' && *x->p != '/') x->p++;
    *empty = x->p < x->end && *x->p == '/';
    while (x->p < x->end && *x->p != '>') x->p++;
    if (x->p < x->end) x->p++;
    return 1;
}
static char *px_text(Px *x, const char *tag) {
    char close[64];
    snprintf(close, sizeof close, "</%s>", tag);
    const char *e = strstr(x->p, close);
    if (!e) e = x->end;
    size_t n = (size_t)(e - x->p);
    char *s = malloc(n + 1), *o = s;
    for (const char *q = x->p; q < e;) {
        if (*q == '&') {
            if (!strncmp(q, "&lt;", 4)) { *o++ = '<'; q += 4; continue; }
            if (!strncmp(q, "&gt;", 4)) { *o++ = '>'; q += 4; continue; }
            if (!strncmp(q, "&amp;", 5)) { *o++ = '&'; q += 5; continue; }
            if (!strncmp(q, "&quot;", 6)) { *o++ = '"'; q += 6; continue; }
            if (!strncmp(q, "&apos;", 6)) { *o++ = '\''; q += 6; continue; }
        }
        *o++ = *q++;
    }
    *o = 0;
    x->p = e < x->end ? e + strlen(close) : x->end;
    return s;
}
static uint32_t px_value(Px *x) {
    char tag[32];
    int closing, empty;
    if (!px_tag(x, tag, sizeof tag, &closing, &empty) || closing) return 0;
    Mt *m = x->m;
    if (!strcmp(tag, "dict")) {
        uint32_t d = cf_new(m, K_DICT);
        if (empty) return d;
        for (;;) {
            const char *save = x->p;
            if (!px_tag(x, tag, sizeof tag, &closing, &empty)) break;
            if (closing) break;
            if (strcmp(tag, "key")) { x->p = save; px_value(x); continue; }
            char *k = px_text(x, "key");
            uint32_t ks = mt_cf_string(m, k, strlen(k));
            free(k);
            uint32_t v = px_value(x);
            dict_set(m, x->c, d, ks, v);
        }
        return d;
    }
    if (!strcmp(tag, "array")) {
        uint32_t a = cf_new(m, K_ARRAY);
        if (empty) return a;
        for (;;) {
            const char *save = x->p;
            int cl, em;
            char t2[32];
            if (!px_tag(x, t2, sizeof t2, &cl, &em)) break;
            if (cl) break;
            x->p = save;
            push_value(m, a, px_value(x));
        }
        return a;
    }
    if (!strcmp(tag, "true") || !strcmp(tag, "false")) {
        if (!empty) { char *t = px_text(x, tag); free(t); }
        uint32_t *slot = tag[0] == 't' ? &g_true : &g_false;
        if (!*slot) { *slot = cf_new(m, K_BOOLEAN); s32_(m, *slot, O_N, slot == &g_true); s32_(m, *slot, O_RC, 0x40000000); }
        return *slot;
    }
    char *t = empty ? strdup("") : px_text(x, tag);
    uint32_t v = 0;
    if (!strcmp(tag, "string")) v = mt_cf_string(m, t, strlen(t));
    else if (!strcmp(tag, "integer") || !strcmp(tag, "real")) {
        double d = strtod(t, NULL);
        v = cf_new(m, K_NUMBER);
        memcpy(G(m, v + O_NUM), &d, 8);
        s32_(m, v, O_N, tag[0] == 'i' ? 4 : 6);
    } else if (!strcmp(tag, "data")) {
        // base64
        static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t n = strlen(t), k = 0;
        uint8_t *out = malloc(n * 3 / 4 + 4);
        uint32_t acc = 0;
        int bits = 0;
        for (size_t i = 0; i < n; i++) {
            const char *q = strchr(b64, t[i]);
            if (!q || !t[i]) continue;
            acc = acc << 6 | (uint32_t)(q - b64);
            bits += 6;
            if (bits >= 8) { bits -= 8; out[k++] = (uint8_t)(acc >> bits); }
        }
        v = data_from(m, out, (uint32_t)k);
        free(out);
    } else if (!strcmp(tag, "date")) v = mt_cf_string(m, t, strlen(t));
    free(t);
    return v;
}
static void h_CFPropertyListCreateFromXMLData(Mt *m, X86 *c) {
    uint32_t d = A(c, 1);
    if (kind_of(m, c, d) != K_DATA) x86_fault(c, "plist from non-CFData");
    Px x = {m, c, (const char *)G(m, f32(m, d, O_PTR)), NULL};
    x.end = x.p + f32(m, d, O_N);
    if (x.end - x.p >= 6 && !memcmp(x.p, "bplist", 6)) {
        if (A(c, 3)) wr32(c, A(c, 3), mt_cf_string(m, "binary plists unsupported", 25));
        R(c, 0);
        return;
    }
    // skip to <plist ...>
    char tag[32];
    int closing, empty;
    const char *save = x.p;
    if (px_tag(&x, tag, sizeof tag, &closing, &empty) && strcmp(tag, "plist")) x.p = save;
    R(c, px_value(&x));
}

uint32_t mt_cf_data_wrap(Mt *m, uint32_t bytes, uint32_t n) {
    uint32_t d = cf_new(m, K_DATA);
    s32_(m, d, O_PTR, bytes);
    s32_(m, d, O_N, n);
    return d;
}
uint32_t mt_cf_number(Mt *m, double v) {
    uint32_t o = cf_new(m, K_NUMBER);
    memcpy(G(m, o + O_NUM), &v, 8);
    s32_(m, o, O_N, 6);
    return o;
}
// value of a CFNumber/CFBoolean the engine handed back (0 if not one)
double mt_cf_number_value(Mt *m, uint32_t o) {
    if (!o || !ours(m, o)) return 0;
    int k = (int)f32(m, o, O_KIND);
    if (k == K_BOOLEAN) return f32(m, o, O_N);
    if (k != K_NUMBER) return 0;
    double v;
    memcpy(&v, G(m, o + O_NUM), 8);
    return v;
}

void mt_cf_reset(void) { g_isa = g_isa_import = g_true = g_false = g_dict_bundle = 0; }

const MtHleEntry mt_hle_cf[] = {
    {"_CFRetain", h_CFRetain}, {"_CFRelease", h_CFRelease}, {"_CFGetTypeID", h_CFGetTypeID},
    {"_CFStringGetTypeID", h_CFStringTypeID}, {"_CFNumberGetTypeID", h_CFNumberGetTypeID},
    {"_CFBooleanGetTypeID", h_CFBooleanGetTypeID}, {"_CFDictionaryGetTypeID", h_CFDictionaryGetTypeID},
    {"_CFArrayGetTypeID", h_CFArrayGetTypeID}, {"_CFDataGetTypeID", h_CFDataGetTypeID},
    {"_CFStringCreateWithCString", h_CFStringCreateWithCString},
    {"_CFStringCreateWithCStringNoCopy", h_CFStringCreateWithCString},
    {"_CFStringCreateWithSubstring", h_CFStringCreateWithSubstring}, {"_CFStringGetLength", h_CFStringGetLength},
    {"_CFStringGetCStringPtr", h_CFStringGetCStringPtr}, {"_CFStringGetCString", h_CFStringGetCString},
    {"_CFStringGetMaximumSizeForEncoding", h_CFStringGetMaximumSizeForEncoding},
    {"_CFStringGetMaximumSizeOfFileSystemRepresentation", h_CFStringGetMaximumSizeOfFileSystemRepresentation},
    {"_CFStringGetFileSystemRepresentation", h_CFStringGetFileSystemRepresentation},
    {"_CFStringFind", h_CFStringFind}, {"_CFStringCreateExternalRepresentation", h_CFStringCreateExternalRepresentation},
    {"_CFNumberCreate", h_CFNumberCreate}, {"_CFNumberGetValue", h_CFNumberGetValue},
    {"_CFBooleanGetValue", h_CFBooleanGetValue},
    {"_CFDataCreate", h_CFDataCreate}, {"_CFDataGetBytePtr", h_CFDataGetBytePtr}, {"_CFDataGetLength", h_CFDataGetLength},
    {"_CFArrayCreate", h_CFArrayCreate}, {"_CFArrayCreateMutable", h_CFArrayCreateMutable},
    {"_CFArrayAppendValue", h_CFArrayAppendValue}, {"_CFArrayGetCount", h_CFArrayGetCount},
    {"_CFArrayGetValueAtIndex", h_CFArrayGetValueAtIndex},
    {"_CFDictionaryCreate", h_CFDictionaryCreate}, {"_CFDictionaryCreateMutable", h_CFDictionaryCreateMutable},
    {"_CFDictionarySetValue", h_CFDictionarySetValue}, {"_CFDictionaryCreateCopy", h_CFDictionaryCreateCopy},
    {"_CFDictionaryGetValue", h_CFDictionaryGetValue}, {"_CFDictionaryGetValueIfPresent", h_CFDictionaryGetValueIfPresent},
    {"_CFDictionaryGetCount", h_CFDictionaryGetCount},
    {"_CFBundleGetBundleWithIdentifier", h_CFBundleGetBundleWithIdentifier},
    {"_CFBundleCopyResourceURL", h_CFBundleCopyResourceURL}, {"_CFBundleGetVersionNumber", h_CFBundleGetVersionNumber},
    {"_CFURLCreateWithFileSystemPath", h_CFURLCreateWithFileSystemPath}, {"_CFURLCopyFileSystemPath", h_CFURLCopyFileSystemPath},
    {"_CFURLCreateDataAndPropertiesFromResource", h_CFURLCreateDataAndPropertiesFromResource},
    {"_CFURLCreateFromFileSystemRepresentation", h_CFURLCreateFromFileSystemRepresentation},
    {"_CFURLGetFSRef", h_CFURLGetFSRef}, {"_CFURLCopyPath", h_CFURLCopyPath}, {"_FSPathMakeRef", h_FSPathMakeRef}, {"_FSDeleteObject", h_FSDeleteObject},
    {"_DisposeHandle", h_DisposeHandle}, {"_CFPreferencesCopyAppValue", h_CFPreferencesCopyAppValue},
    {"_CFPropertyListCreateFromXMLData", h_CFPropertyListCreateFromXMLData},
    {NULL, NULL},
};
