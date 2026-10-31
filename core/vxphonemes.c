// IPA -> L&H+ (checked against each voice's own symbols) or MacinTalk
// phonemes; see vxphonemes.h.
#include "vxphonemes.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

// ---------------------------------------------------------------- UTF-8

static size_t decode(const char *s, uint32_t *out, size_t cap) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap) {
        uint32_t c = *p;
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (len == 2) c &= 0x1F;
        else if (len == 3) c &= 0x0F;
        else if (len == 4) c &= 0x07;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = i; break; }
            c = c << 6 | (p[i] & 0x3F);
        }
        p += len;
        // c + combining cedilla, the one decomposed letter that matters here
        if (c == 0x327 && n && out[n - 1] == 'c') { out[n - 1] = 0xE7; continue; }
        out[n++] = c;
    }
    return n;
}

static size_t encode(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

typedef struct { char *s; size_t n, cap; } Str;

static void str_add(Str *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void str_puts(Str *b, const char *s) { str_add(b, s, strlen(s)); }

// ---------------------------------------------------------------- voice data

// The L&H+ symbols a voice accepts, sorted.
typedef struct {
    char **sym;
    size_t n;
} Inventory;

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int inv_has(const Inventory *inv, const char *s) {
    return bsearch(&s, inv->sym, inv->n, sizeof *inv->sym, cmp_str) != NULL;
}

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

static const unsigned char *find(const unsigned char *d, size_t n, size_t from, const unsigned char *what) {
    for (size_t i = from; i + 4 <= n; i++)
        if (!memcmp(d + i, what, 4)) return d + i;
    return NULL;
}

// The symbol table in an engine data file: of its key/value tables (chunks
// named STMC / STMF, XORed with 0xA9), the one whose keys hold ' and #.
static Inventory *read_inventory(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (64 << 20)) { fclose(f); return NULL; }
    unsigned char *d = malloc((size_t)size);
    size_t n = fread(d, 1, (size_t)size, f);
    fclose(f);
    unsigned char stmc[4], stmf[4];
    for (int i = 0; i < 4; i++) { stmc[i] = (unsigned char)("STMC"[i] ^ 0xA9); stmf[i] = (unsigned char)("STMF"[i] ^ 0xA9); }
    Inventory *inv = NULL;
    for (const unsigned char *c = find(d, n, 0, stmc); c && !inv; c = find(d, n, (size_t)(c - d) + 4, stmc)) {
        size_t at = (size_t)(c - d);
        if (at + 16 > n || rd32(c + 4) < 10) continue;
        unsigned k = rd16(c + 10), v = rd16(c + 12), count = rd16(c + 14);
        const unsigned char *t = find(d, n, at + 8, stmf);
        if (!k || !v || !t || (size_t)(t - d) + 8 > n || rd32(t + 4) != count * (k + v)) continue;
        const unsigned char *base = t + 8;
        if ((size_t)(base - d) + (size_t)count * (k + v) > n) continue;
        int quote = 0, hash = 0;
        for (unsigned j = 0; j < count; j++) {
            const unsigned char *key = base + (size_t)j * (k + v);
            if (key[0] == '\'' && (k == 1 || !key[1])) quote = 1;
            if (key[0] == '#' && (k == 1 || !key[1])) hash = 1;
        }
        if (!quote || !hash) continue;
        inv = calloc(1, sizeof *inv);
        inv->sym = calloc(count, sizeof *inv->sym);
        for (unsigned j = 0; j < count; j++) {
            const unsigned char *key = base + (size_t)j * (k + v);
            size_t len = strnlen((const char *)key, k);
            if (!len) continue;
            inv->sym[inv->n] = malloc(len + 1);
            memcpy(inv->sym[inv->n], key, len);
            inv->sym[inv->n++][len] = 0;
        }
        qsort(inv->sym, inv->n, sizeof *inv->sym, cmp_str);
    }
    free(d);
    return inv;
}

static int match_voice_file(const char *n) {
    // phonmap_<lang>_<voice>.dat (iOS 3-4, Lion), or clc_<lang>_<voice>_cfg<N>.dat
    // (iOS 5-6, Mountain Lion, Mavericks), not clc_<lang>_cfg...
    size_t len = strlen(n);
    if (len < 5 || strcmp(n + len - 4, ".dat")) return 0;
    if (!strncmp(n, "phonmap_", 8)) return 1;
    if (strncmp(n, "clc_", 4) || len < 8 || n[7] != '_' || !strncmp(n + 8, "cfg", 3)) return 0;
    const char *cfg = strstr(n + 8, "_cfg");
    if (!cfg) return 0;
    for (const char *p = n + 8; p < cfg; p++) if (*p < 'a' || *p > 'z') return 0;
    for (const char *p = cfg + 4; p < n + len - 4; p++) if (*p < '0' || *p > '9') return 0;
    return cfg + 4 < n + len - 4;
}

#define CACHE 16
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char *folder; Inventory *inv; } g_inv[CACHE];
static size_t g_ninv;

// The names in a folder that start with phonmap_ or clc_ (UTF-8 paths on Windows too).
static char **voice_files(const char *folder, size_t *count) {
    char **names = NULL;
    size_t nn = 0;
#ifdef _WIN32
    char pattern[2100];
    snprintf(pattern, sizeof pattern, "%s\\*", folder);
    WCHAR wp[2100];
    if (!MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wp, 2100)) { *count = 0; return NULL; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wp, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char name[1024];
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
            if (strncmp(name, "phonmap_", 8) && strncmp(name, "clc_", 4)) continue;
            names = realloc(names, (nn + 1) * sizeof *names);
            names[nn++] = strdup(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR *dir = opendir(folder);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (strncmp(e->d_name, "phonmap_", 8) && strncmp(e->d_name, "clc_", 4)) continue;
            names = realloc(names, (nn + 1) * sizeof *names);
            names[nn++] = strdup(e->d_name);
        }
        closedir(dir);
    }
#endif
    *count = nn;
    return names;
}

static Inventory *inventory(const char *folder) {
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_ninv; i++)
        if (!strcmp(g_inv[i].folder, folder)) { pthread_mutex_unlock(&g_mu); return g_inv[i].inv; }
    pthread_mutex_unlock(&g_mu);
    size_t nn;
    char **names = voice_files(folder, &nn);
    if (nn) qsort(names, nn, sizeof *names, cmp_str);
    Inventory *inv = NULL;
    for (int pass = 0; pass < 2 && !inv; pass++)  // the voice's own files first
        for (size_t i = 0; i < nn && !inv; i++) {
            if (match_voice_file(names[i]) != !pass) continue;
            if (pass && strncmp(names[i], "clc_", 4)) continue;
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", folder, names[i]);
            inv = read_inventory(path);
        }
    for (size_t i = 0; i < nn; i++) free(names[i]);
    free(names);
    pthread_mutex_lock(&g_mu);
    if (g_ninv < CACHE) {
        g_inv[g_ninv].folder = strdup(folder);
        g_inv[g_ninv++].inv = inv;
    }
    pthread_mutex_unlock(&g_mu);
    return inv;
}

// Apple's IPA -> L&H+ map for a locale: {"ipa": "l&h+", ...}, its values
// format strings ("l%%)" is l%)).
typedef struct {
    char **key, **val;
    size_t n;
} AppleMap;

static char *json_string(const char **pp) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    Str b = {0};
    str_puts(&b, "");
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u') {
                uint32_t c = (uint32_t)strtoul((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
                p += 5;
                if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u') {
                    uint32_t lo = (uint32_t)strtoul((char[5]){p[2], p[3], p[4], p[5], 0}, NULL, 16);
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                char o[4];
                str_add(&b, o, encode(c, o));
                continue;
            }
            char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
            str_add(&b, &c, 1);
            p++;
            continue;
        }
        str_add(&b, p, 1);
        p++;
    }
    if (*p == '"') p++;
    *pp = p;
    return b.s;
}

static AppleMap *read_apple_map(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (4 << 20)) { fclose(f); return NULL; }
    char *d = malloc((size_t)size + 1);
    d[fread(d, 1, (size_t)size, f)] = 0;
    fclose(f);
    AppleMap *m = calloc(1, sizeof *m);
    const char *p = strchr(d, '{');
    while (p && *p) {
        while (*p && *p != '"' && *p != '}') p++;
        if (*p != '"') break;
        char *k = json_string(&p);
        while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        char *v = json_string(&p);
        if (!k || !v) { free(k); free(v); break; }
        for (char *q = v, *w = v;; q++) {  // %% -> %
            *w++ = *q;
            if (*q == '%' && q[1] == '%') q++;
            if (!*q) break;
        }
        m->key = realloc(m->key, (m->n + 1) * sizeof *m->key);
        m->val = realloc(m->val, (m->n + 1) * sizeof *m->val);
        m->key[m->n] = k;
        m->val[m->n++] = v;
    }
    free(d);
    if (!m->n) { free(m->key); free(m->val); free(m); return NULL; }
    return m;
}

static struct { char *lang; AppleMap *map; } g_maps[CACHE];
static size_t g_nmaps;

static AppleMap *apple_map(const char *voices_root, const char *language) {
    if (!language || !*language) return NULL;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_nmaps; i++)
        if (!strcmp(g_maps[i].lang, language)) { pthread_mutex_unlock(&g_mu); return g_maps[i].map; }
    pthread_mutex_unlock(&g_mu);
    char path[2048];
    snprintf(path, sizeof path, "%s/ios10/phonemeMaps/phonemeMap-%s.json", voices_root, language);
    AppleMap *m = read_apple_map(path);
    pthread_mutex_lock(&g_mu);
    if (g_nmaps < CACHE) {
        g_maps[g_nmaps].lang = strdup(language);
        g_maps[g_nmaps++].map = m;
    }
    pthread_mutex_unlock(&g_mu);
    return m;
}

// ---------------------------------------------------------------- IPA

// Each IPA sound's L&H+ spellings, best first, split by '|'; the first the
// voice has wins. Symbols of one spelling are split by ' ' ("ɝ" is E0 then
// R+ in an American voice); an empty spelling drops the sound.
static const struct { uint32_t ipa; const char *lhp; int vowel; } kBase[] = {
    // vowels
    {'i', "i|I|i0|e", 1}, {'y', "y|Y|i|u", 1}, {0x268, "i0|i|I", 1}, {0x289, "u0|u", 1},
    {0x26F, "u-|u|U", 1}, {'u', "u|U|u-|u0|o", 1}, {0x26A, "I|i|e", 1}, {0x28F, "Y|y|I", 1},
    {0x28A, "U|u|u-|o", 1}, {'e', "e|E|e&I|i", 1}, {0xF8, "e+|E+|e|o", 1},
    {0x258, "$|e", 1}, {0x275, "o0|U0|u0|o", 1}, {0x264, "o-|o", 1},
    {'o', "o|O|o&U|o-|u", 1}, {0x259, "$|E0|e|E|a|@", 1}, {0x25B, "E|e|a", 1},
    {0x153, "E+|e+|E|e", 1}, {0x25C, "E0|$|E|e", 1}, {0x25E, "E0|$", 1},
    {0x28C, "^|$|a|A|o", 1}, {0x254, "O|o", 1}, {0xE6, "@|E|a|e", 1},
    {0x250, "a0|$|a|^", 1}, {'a', "a|A|@|a0|^", 1}, {0x276, "E+|a", 1},
    {0x251, "A|a|A+", 1}, {0x252, "A+|O|A|o", 1},
    {0x25A, "$ R+|$", 1}, {0x25D, "E0 R+|E0", 1},
    // plosives
    {'p', "p", 0}, {'b', "b", 0}, {'t', "t", 0}, {'d', "d", 0}, {0x288, "t+|t", 0},
    {0x256, "d+|d", 0}, {'c', "c|k", 0}, {0x25F, "j-|J|g", 0}, {'k', "k", 0},
    {'g', "g", 0}, {0x261, "g", 0}, {'q', "q|k", 0}, {0x262, "g", 0}, {0x294, "?|", 0},
    // nasals
    {'m', "m", 0}, {0x271, "M|m", 0}, {'n', "n", 0}, {0x273, "n+|n", 0}, {0x272, "n~|n", 0},
    {0x14B, "nK|N|n", 0}, {0x274, "N|nK|n", 0},
    // trills, taps
    {0x299, "b", 0}, {'r', "r|r6|R+|R|R\\", 0}, {0x280, "R\\|R|r", 0},
    {0x27E, "r6|r|d|R+", 0}, {0x27D, "r6|r", 0}, {0x27A, "r6|l", 0},
    // fricatives
    {0x278, "F|f|h", 0}, {0x3B2, "B|v|b", 0}, {'f', "f|F|p", 0}, {'v', "v|V|w|b", 0},
    {0x3B8, "T|t|s", 0}, {0xF0, "D|d|z", 0}, {'s', "s", 0}, {'z', "z|s", 0}, {0x283, "S|s+|c~|s", 0},
    {0x292, "Z|z~|S|z", 0}, {0x282, "s+|S|s", 0}, {0x290, "z+|Z|z", 0},
    {0x255, "c~|S", 0}, {0x291, "z~|Z", 0}, {0xE7, "C|S|c~|x|h", 0},
    {0x29D, "J|j", 0}, {'x', "x|X|h|k", 0}, {0x263, "G|g", 0}, {0x3C7, "X|x", 0},
    {0x281, "R|R\\|r|R+|r6", 0}, {0x127, "h-|h", 0}, {0x295, "?\\|?|", 0},
    {'h', "h|h6|x", 0}, {0x266, "h6|h", 0}, {0x267, "h6~|S", 0}, {0x26C, "l", 0}, {0x26E, "l", 0},
    // approximants
    {0x28B, "V|v|w", 0}, {0x279, "R+|r|R|r6", 0}, {0x27B, "R+|r|R|r6", 0},
    {'j', "j|i", 0}, {0x270, "G|w", 0}, {'l', "l|r6|r", 0}, {0x26D, "l+|l", 0},
    {0x28E, "l~|j|l", 0}, {0x29F, "l", 0}, {0x26B, "l|l+", 0}, {'w', "w|u|v", 0},
    {0x28D, "w", 0}, {0x265, "h\\|j", 0},
};

static int base_index(uint32_t c) {
    for (size_t i = 0; i < sizeof kBase / sizeof *kBase; i++)
        if (kBase[i].ipa == c) return (int)i;
    return -1;
}

// English diphthongs as dictionaries write them, when a voice spells them
// differently from their parts (a British "goat" is əʊ, o&U).
static const struct { uint32_t a, b; const char *lhp; } kDiphthongs[] = {
    {0x259, 0x28A, "o&U"}, {'o', 0x28A, "o&U"}, {'e', 0x26A, "e&I"}, {'a', 0x26A, "a&I"}, {0x251, 0x26A, "a&I"},
    {'a', 0x28A, "a&U"}, {0x254, 0x26A, "O&I"}, {0x26A, 0x259, "I&$"}, {'e', 0x259, "E&$"}, {0x25B, 0x259, "E&$"},
    {0x28A, 0x259, "U&$"}, {0x254, 0x259, "O"},
};
static const uint32_t kAffricates[][2] = {
    {'t', 0x283}, {'d', 0x292}, {'t', 0x255}, {'d', 0x291}, {'t', 's'}, {'d', 'z'}, {'p', 'f'},
};

// modifiers -> the L&H+ suffixes to try (in that order) on the symbol before
static const struct { uint32_t ipa; const char *suffix; } kSuffix[] = {
    {0x2D0, ":"}, {0x303, "%~|~"}, {0x329, "%)"}, {0x30D, "%)"}, {0x2B0, "%{|{"}, {0x2B2, "%j"},
    {0x2DE, ""},  // ˞ is a following R+
};
#define RHOTIC 0x2DE

static const char *suffix_of(uint32_t c, int *found) {
    for (size_t i = 0; i < sizeof kSuffix / sizeof *kSuffix; i++)
        if (kSuffix[i].ipa == c) { *found = 1; return kSuffix[i].suffix; }
    *found = 0;
    return NULL;
}

static int is_tie(uint32_t c) { return c == 0x35C || c == 0x361; }
static int is_tone(uint32_t c) { return c >= 0x2E5 && c <= 0x2E9; }
// everything else that can follow a sound and changes nothing L&H+ can say
static int is_ignored(uint32_t c) {
    static const uint32_t k[] = {0x32A, 0x325, 0x32C, 0x32F, 0x31A, 0x2BC, 0x2D1, 0x31F, 0x320, 0x308, 0x33D, 0x2E0,
                                 0x2E4, 0x2B7, 0x334, 0x339, 0x31C, 0x31D, 0x31E, 0x318, 0x319, 0x324, 0x330, 0x33C,
                                 0x33A, 0x33B, 0x306, 0x301, 0x300, 0x304, 0x30B, 0x30F, 0x30C, 0x302, 0x2191, 0x2193};
    if (is_tone(c)) return 1;
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (k[i] == c) return 1;
    return 0;
}
static int is_boundary(uint32_t c) { return c == ' ' || c == '_' || c == 0x203F || c == '|' || c == 0x2016 || c == '-'; }

enum { K_SOUND, K_STRESS, K_SYL, K_WORD };
typedef struct {
    int kind;
    int base[2], nbase;  // kBase indices: two when tied
    uint32_t mods[8];
    int nmods;
    int stress;          // 1 or 2
} Token;

// IPA -> tokens; -1 when a character has no meaning here.
static int tokens(const uint32_t *ipa, size_t n, Token *out, size_t cap) {
    size_t m = 0;
    for (size_t i = 0; i < n && m < cap;) {
        uint32_t c = ipa[i];
        int b = base_index(c), found;
        if (b >= 0) {
            Token t = {K_SOUND, {b, -1}, 1, {0}, 0, 0};
            i++;
            while (i < n) {
                uint32_t d = ipa[i];
                if (is_tie(d) && i + 1 < n && base_index(ipa[i + 1]) >= 0 && t.nbase == 1) {
                    t.base[t.nbase++] = base_index(ipa[i + 1]);
                    i += 2;
                } else if ((suffix_of(d, &found), found)) {
                    if (t.nmods < 8) t.mods[t.nmods++] = d;
                    i++;
                } else if (is_ignored(d)) i++;
                else break;
            }
            out[m++] = t;
        } else if (c == 0x2C8 || c == '\'') {
            out[m++] = (Token){.kind = K_STRESS, .stress = 1};
            i++;
        } else if (c == 0x2CC) {
            out[m++] = (Token){.kind = K_STRESS, .stress = 2};
            i++;
        } else if (c == '.') {
            out[m++] = (Token){.kind = K_SYL};
            i++;
        } else if (is_boundary(c)) {
            if (m && out[m - 1].kind != K_WORD) out[m++] = (Token){.kind = K_WORD};
            i++;
        } else if (is_ignored(c) || (suffix_of(c, &found), found)) i++;  // a stray modifier
        else return -1;
    }
    while (m && out[m - 1].kind == K_WORD) m--;
    return (int)m;
}

// One spelling: up to 4 symbols.
typedef struct { char s[4][16]; int n; } Spelling;
typedef struct { Spelling *v; size_t n, cap; } Spellings;

static void sp_add(Spellings *l, const Spelling *s) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = *s;
}

// "a b" -> {a, b}; "" -> {}
static Spelling parse_spelling(const char *s, size_t len) {
    Spelling o = {.n = 0};
    size_t i = 0;
    while (i < len && o.n < 4) {
        size_t j = i;
        while (j < len && s[j] != ' ') j++;
        if (j > i) snprintf(o.s[o.n++], sizeof o.s[0], "%.*s", (int)(j - i), s + i);
        i = j + 1;
    }
    return o;
}

// Calls f for each '|'-separated alternative of s.
#define EACH_ALT(s, a, alen, body) do { \
        const char *a = (s); \
        for (;;) { \
            const char *e_ = strchr(a, '|'); \
            size_t alen = e_ ? (size_t)(e_ - a) : strlen(a); \
            body; \
            if (!e_) break; \
            a = e_ + 1; \
        } \
    } while (0)

// Candidate L&H+ spellings for one sound (bases tied when nb is 2).
static void spellings(const int *base, int nb, const uint32_t *mods, int nmods, Spellings *out) {
    Spellings c = {0};
    if (nb == 2) {  // an affricate or a diphthong
        for (size_t i = 0; i < sizeof kDiphthongs / sizeof *kDiphthongs; i++)
            if (kDiphthongs[i].a == kBase[base[0]].ipa && kDiphthongs[i].b == kBase[base[1]].ipa) {
                Spelling s = parse_spelling(kDiphthongs[i].lhp, strlen(kDiphthongs[i].lhp));
                sp_add(&c, &s);
            }
        EACH_ALT(kBase[base[0]].lhp, a, al, {
            EACH_ALT(kBase[base[1]].lhp, b, bl, {
                if (al && bl && !memchr(a, ' ', al) && !memchr(b, ' ', bl)) {
                    Spelling s = {.n = 1};
                    snprintf(s.s[0], sizeof s.s[0], "%.*s&%.*s", (int)al, a, (int)bl, b);
                    sp_add(&c, &s);
                }
            });
        });
    } else {
        EACH_ALT(kBase[base[0]].lhp, a, al, {
            Spelling s = parse_spelling(a, al);
            sp_add(&c, &s);
        });
    }
    // the suffixes go on the last symbol: all of them, then fewer
    const char *suf[8];
    int ns = 0, rhotic = 0, found;
    for (int i = 0; i < nmods; i++) {
        const char *x = suffix_of(mods[i], &found);
        if (mods[i] == RHOTIC) rhotic = 1;
        else if (x && *x) suf[ns++] = x;
    }
    Spellings o = {0};
    for (size_t i = 0; i < c.n; i++) {
        for (int k = ns; k > 0 && c.v[i].n; k--) {
            // every combination of the first k suffixes' alternatives
            int idx[8] = {0}, cnt[8];
            for (int j = 0; j < k; j++) { cnt[j] = 1; for (const char *p = suf[j]; *p; p++) cnt[j] += *p == '|'; }
            for (;;) {
                Spelling s = c.v[i];
                char *last = s.s[s.n - 1];
                for (int j = 0; j < k; j++) {
                    const char *p = suf[j];
                    for (int q = 0; q < idx[j]; q++) p = strchr(p, '|') + 1;
                    const char *e = strchr(p, '|');
                    size_t l = e ? (size_t)(e - p) : strlen(p), have = strlen(last);
                    if (have + l < sizeof s.s[0]) { memcpy(last + have, p, l); last[have + l] = 0; }
                }
                sp_add(&o, &s);
                int j = k - 1;
                while (j >= 0 && ++idx[j] == cnt[j]) idx[j--] = 0;
                if (j < 0) break;
            }
        }
        sp_add(&o, &c.v[i]);
    }
    free(c.v);
    if (rhotic) {  // with a following R+ first
        Spellings r = {0};
        for (size_t i = 0; i < o.n; i++) {
            Spelling s = o.v[i];
            if (s.n < 4) snprintf(s.s[s.n++], sizeof s.s[0], "R+");
            sp_add(&r, &s);
        }
        for (size_t i = 0; i < o.n; i++) sp_add(&r, &o.v[i]);
        free(o.v);
        o = r;
    }
    *out = o;
}

static const Spelling *pick(const Spellings *l, const Inventory *inv) {
    for (size_t i = 0; i < l->n; i++) {
        int ok = 1;
        for (int j = 0; j < l->v[i].n && ok; j++) ok = inv_has(inv, l->v[i].s[j]);
        if (ok) return &l->v[i];
    }
    return NULL;
}

static int is_affricate(uint32_t a, uint32_t b) {
    for (size_t i = 0; i < sizeof kAffricates / sizeof *kAffricates; i++)
        if (kAffricates[i][0] == a && kAffricates[i][1] == b) return 1;
    return 0;
}

static void put_spelling(Str *o, const Spelling *s) {
    for (int j = 0; j < s->n; j++) str_puts(o, s->s[j]);
}

static char *convert(const Token *t, int n, const Inventory *inv) {
    Str o = {0};
    str_puts(&o, "");
    for (int i = 0; i < n; i++) {
        if (t[i].kind == K_STRESS) { str_puts(&o, t[i].stress == 1 ? "'" : inv_has(inv, "'2") ? "'2" : ""); continue; }
        if (t[i].kind == K_SYL) { str_puts(&o, "."); continue; }
        if (t[i].kind == K_WORD) { str_puts(&o, "_"); continue; }
        int got = 0;
        // two sounds the voice writes as one: a diphthong (aɪ) or an
        // affricate (tʃ) written without a tie bar
        if (i + 1 < n && t[i + 1].kind == K_SOUND && t[i].nbase == 1 && t[i + 1].nbase == 1) {
            uint32_t a = kBase[t[i].base[0]].ipa, b = kBase[t[i + 1].base[0]].ipa;
            if ((kBase[t[i].base[0]].vowel && kBase[t[i + 1].base[0]].vowel) || is_affricate(a, b)) {
                int pair[2] = {t[i].base[0], t[i + 1].base[0]};
                uint32_t mods[16];
                int nm = 0;
                for (int k = 0; k < t[i].nmods; k++) mods[nm++] = t[i].mods[k];
                for (int k = 0; k < t[i + 1].nmods && nm < 8; k++) mods[nm++] = t[i + 1].mods[k];
                Spellings l;
                spellings(pair, 2, mods, nm, &l);
                const Spelling *s = pick(&l, inv);
                if (s) { put_spelling(&o, s); got = 1; i++; }
                free(l.v);
            }
        }
        if (!got) {
            Spellings l;
            spellings(t[i].base, t[i].nbase, t[i].mods, t[i].nmods, &l);
            const Spelling *s = pick(&l, inv);
            if (s) { put_spelling(&o, s); got = 1; }
            free(l.v);
        }
        if (!got && t[i].nbase == 2) {  // say the tied parts one by one
            Str part = {0};
            str_puts(&part, "");
            got = 1;
            for (int k = 0; k < 2 && got; k++) {
                Spellings l;
                spellings(&t[i].base[k], 1, NULL, 0, &l);
                const Spelling *s = pick(&l, inv);
                if (s) put_spelling(&part, s);
                else got = 0;
                free(l.v);
            }
            if (got) str_puts(&o, part.s);
            free(part.s);
        }
        if (!got) { free(o.s); return NULL; }
    }
    // strip word boundaries at the ends
    size_t a = 0, z = o.n;
    while (a < z && o.s[a] == '_') a++;
    while (z > a && o.s[z - 1] == '_') z--;
    if (z == a) { free(o.s); return NULL; }
    memmove(o.s, o.s + a, z - a);
    o.s[z - a] = 0;
    return o.s;
}

// Apple's map, longest match first; NULL if it doesn't cover the text or
// gives a symbol this voice lacks.
static char *apple(const char *ipa, const AppleMap *m, const Inventory *inv) {
    Str o = {0};
    str_puts(&o, "");
    const char *p = ipa;
    while (*p) {
        if (*p == ' ') { str_puts(&o, "_"); p++; continue; }
        size_t best = 0, bi = 0;
        for (size_t i = 0; i < m->n; i++) {
            size_t l = strlen(m->key[i]);
            if (l > best && !strncmp(p, m->key[i], l)) { best = l; bi = i; }
        }
        if (!best) { free(o.s); return NULL; }
        const char *v = m->val[bi];
        if (!inv_has(inv, v) && strcmp(v, "_") && strcmp(v, ".")) { free(o.s); return NULL; }
        str_puts(&o, v);
        p += best;
    }
    if (!o.n) { free(o.s); return NULL; }
    return o.s;
}

static char *to_lhp(const char *ipa, const Inventory *inv, const AppleMap *m) {
    uint32_t cp[512];
    size_t n = decode(ipa, cp, 512);
    int tones = 0;
    for (size_t i = 0; i < n; i++) tones |= is_tone(cp[i]);
    char *r = NULL;
    if (m && tones && (r = apple(ipa, m, inv))) return r;  // Apple's map knows tones (Chinese)
    Token t[512];
    int nt = tokens(cp, n, t, 512);
    if (nt > 0) r = convert(t, nt, inv);
    if (!r && m) r = apple(ipa, m, inv);
    return r;
}

int vx_ipa_has_vowel(const char *ipa) {
    uint32_t cp[512];
    size_t n = decode(ipa, cp, 512);
    for (size_t i = 0; i < n; i++) {
        int b = base_index(cp[i]);
        if ((b >= 0 && kBase[b].vowel) || cp[i] == 0x329 || cp[i] == 0x30D) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- MacinTalk

// MacinTalk's (American English) phoneme for each IPA sound; vowels take the
// stress digit before them. Sounds English lacks become the nearest it has.
static const struct { uint32_t ipa; const char *mt; int vowel; } kMt[] = {
    {'i', "IY", 1}, {'y', "UW", 1}, {0x268, "IX", 1}, {0x289, "UW", 1}, {0x26F, "UW", 1}, {'u', "UW", 1},
    {0x26A, "IH", 1}, {0x28F, "IH", 1}, {0x28A, "UH", 1}, {'e', "EY", 1}, {0xF8, "UX", 1}, {0x258, "AX", 1},
    {0x275, "UH", 1}, {0x264, "UH", 1}, {'o', "OW", 1}, {0x259, "AX", 1}, {0x25B, "EH", 1}, {0x153, "UX", 1},
    {0x25C, "AXr", 1}, {0x25E, "AX", 1}, {0x28C, "UX", 1}, {0x254, "AO", 1}, {0xE6, "AE", 1}, {0x250, "AX", 1},
    {'a', "AA", 1}, {0x276, "AA", 1}, {0x251, "AA", 1}, {0x252, "AA", 1}, {0x25A, "AXr", 1}, {0x25D, "AXr", 1},
    {'p', "p", 0}, {'b', "b", 0}, {'t', "t", 0}, {'d', "d", 0}, {0x288, "t", 0}, {0x256, "d", 0}, {'c', "k", 0},
    {0x25F, "g", 0}, {'k', "k", 0}, {'g', "g", 0}, {0x261, "g", 0}, {'q', "k", 0}, {0x262, "g", 0}, {0x294, "", 0},
    {'m', "m", 0}, {0x271, "m", 0}, {'n', "n", 0}, {0x273, "n", 0}, {0x272, "ny", 0}, {0x14B, "N", 0}, {0x274, "N", 0},
    {0x299, "b", 0}, {'r', "r", 0}, {0x280, "r", 0}, {0x27E, "t", 0}, {0x27D, "r", 0}, {0x27A, "l", 0},
    {0x278, "f", 0}, {0x3B2, "v", 0}, {'f', "f", 0}, {'v', "v", 0}, {0x3B8, "T", 0}, {0xF0, "D", 0}, {'s', "s", 0},
    {'z', "z", 0}, {0x283, "S", 0}, {0x292, "Z", 0}, {0x282, "S", 0}, {0x290, "Z", 0}, {0x255, "S", 0},
    {0x291, "Z", 0}, {0xE7, "h", 0}, {0x29D, "y", 0}, {'x', "h", 0}, {0x263, "g", 0}, {0x3C7, "h", 0},
    {0x281, "r", 0}, {0x127, "h", 0}, {0x295, "", 0}, {'h', "h", 0}, {0x266, "h", 0}, {0x267, "S", 0},
    {0x26C, "l", 0}, {0x26E, "l", 0}, {0x28B, "v", 0}, {0x279, "r", 0}, {0x27B, "r", 0}, {'j', "y", 0},
    {0x270, "w", 0}, {'l', "l", 0}, {0x26D, "l", 0}, {0x28E, "ly", 0}, {0x29F, "l", 0}, {0x26B, "l", 0},
    {'w', "w", 0}, {0x28D, "w", 0}, {0x265, "w", 0},
};
static const struct { uint32_t a, b; const char *mt; } kMtPairs[] = {
    {'e', 0x26A, "EY"}, {'a', 0x26A, "AY"}, {0x251, 0x26A, "AY"}, {'a', 0x28A, "AW"}, {0x254, 0x26A, "OY"},
    {'o', 0x28A, "OW"}, {0x259, 0x28A, "OW"}, {'t', 0x283, "C"}, {'d', 0x292, "J"},
};

static int mt_index(uint32_t c) {
    for (size_t i = 0; i < sizeof kMt / sizeof *kMt; i++) if (kMt[i].ipa == c) return (int)i;
    return -1;
}

// IPA -> MacinTalk phonemes, or NULL.
static char *to_macintalk(const char *ipa) {
    uint32_t cp[512];
    size_t n = decode(ipa, cp, 512);
    Str o = {0};
    str_puts(&o, "");
    int stress = 0, found;
    for (size_t i = 0; i < n;) {
        uint32_t c = cp[i];
        if (c == 0x2C8 || c == '\'') { stress = 1; i++; continue; }
        if (c == 0x2CC) { stress = 2; i++; continue; }
        if (c == '.') { i++; continue; }
        if (is_boundary(c)) { if (o.n && o.s[o.n - 1] != ' ') str_puts(&o, " "); i++; continue; }
        if (is_tie(c) || is_ignored(c) || (suffix_of(c, &found), found)) { i++; continue; }
        int k = mt_index(c);
        if (k < 0) { free(o.s); return NULL; }
        const char *mt = kMt[k].mt;
        int vowel = kMt[k].vowel;
        size_t next = i + 1;
        while (next < n && (is_tie(cp[next]) || is_ignored(cp[next]))) next++;
        for (size_t p = 0; next < n && p < sizeof kMtPairs / sizeof *kMtPairs; p++)
            if (kMtPairs[p].a == c && kMtPairs[p].b == cp[next]) { mt = kMtPairs[p].mt; vowel = mt[0] != 'C' && mt[0] != 'J'; next++; break; }
        int syllabic = 0, rhotic = 0;
        for (; next < n; next++) {
            uint32_t d = cp[next];
            if (d == 0x329 || d == 0x30D) syllabic = 1;
            else if (d == RHOTIC) rhotic = 1;
            else if (!((suffix_of(d, &found), found) || is_ignored(d))) break;
        }
        if (syllabic && !vowel) {  // button's n̩ is IXn; bottle's l̩ AXl
            if (stress) { char d = (char)('0' + stress); str_add(&o, &d, 1); stress = 0; }
            str_puts(&o, c == 'n' ? "IX" : "AX");
        }
        if (vowel && stress) { char d = (char)('0' + stress); str_add(&o, &d, 1); stress = 0; }
        str_puts(&o, mt);
        if (rhotic && mt[strlen(mt) - 1] != 'r') str_puts(&o, "r");
        i = next;
    }
    while (o.n && o.s[o.n - 1] == ' ') o.s[--o.n] = 0;
    if (!o.n) { free(o.s); return NULL; }
    return o.s;
}

// ---------------------------------------------------------------- voices

// Where a voice's data files are, from its key ("ios3/en-US", "ios5e/en-GB",
// "mac108/Samantha").
static void voice_dir(const char *voices_root, const char *asset_root, const char *key, char *out, size_t cap) {
    const char *slash = strchr(key, '/');
    size_t bl = slash ? (size_t)(slash - key) : strlen(key);
    const char *rest = slash ? slash + 1 : "";
    if (!strncmp(key, "mac", 3)) snprintf(out, cap, "%s/%.*s/%s", asset_root ? asset_root : "", (int)bl, key, rest);
    else if (bl && key[bl - 1] == 'e') snprintf(out, cap, "%s/%.*s/%s", asset_root ? asset_root : "", (int)bl - 1, key, rest);
    else snprintf(out, cap, "%s/%.*s/TTSResources/%s", voices_root, (int)bl, key, rest);
}

char *vx_phoneme_text(const char *voices_root, const char *asset_root, const char *voice_key, const char *language,
                      const char *ipa, int joined) {
    if (!voice_key || !ipa || !*ipa) return NULL;
    if (voice_key[0] == 'm' && voice_key[1] == 't') {
        char *mt = to_macintalk(ipa);
        if (!mt) return NULL;
        size_t n = strlen(mt) + 64;
        char *o = malloc(n);
        snprintf(o, n, "[[inpt PHON]]%s[[inpt TEXT]]", mt);
        free(mt);
        return o;
    }
    // iOS 4 reads ESC\toi=lhp\ input out as text but takes the older
    // ESC/+ ... ESC/+ switch, exactly as iOS 3 does.
    int ios4 = !strncmp(voice_key, "ios4/", 5);
    if ((ios4 || !strncmp(voice_key, "ios3/", 5)) && !joined && !vx_ipa_has_vowel(ipa)) return NULL;
    char dir[2048];
    voice_dir(voices_root, asset_root, voice_key, dir, sizeof dir);
    const Inventory *inv = inventory(dir);
    if (!inv) return NULL;
    char *lhp = to_lhp(ipa, inv, apple_map(voices_root, language));
    if (!lhp) return NULL;
    size_t n = strlen(lhp) + 32;
    char *o = malloc(n);
    if (ios4) snprintf(o, n, "\x1b/+%s\x1b/+", lhp);
    else snprintf(o, n, "\x1b\\toi=lhp\\%s\x1b\\toi=orth\\", lhp);
    free(lhp);
    return o;
}
