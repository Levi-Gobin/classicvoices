#include "cv_rules.h"
#include "vxrules.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CV_PCRE2_STATIC
// Windows: PCRE2 is compiled in (tools/pcre2_build.py).
#include "pcre2.h"
#else
// The few PCRE2 entry points used, resolved with dlsym (no build dependency).
#include <dlfcn.h>
typedef struct pcre2_code_8 pcre2_code;
typedef struct pcre2_match_data_8 pcre2_match_data;
#define PCRE2_CASELESS 0x00000008u
#define PCRE2_MULTILINE 0x00000400u
#define PCRE2_UCP 0x00020000u
#define PCRE2_UTF 0x00080000u
#define PCRE2_SUBSTITUTE_GLOBAL 0x00000100u
#define PCRE2_SUBSTITUTE_OVERFLOW_LENGTH 0x00001000u
#define PCRE2_SUBSTITUTE_MATCHED 0x00010000u
#define PCRE2_SUBSTITUTE_REPLACEMENT_ONLY 0x00020000u
#define PCRE2_ERROR_NOMEMORY (-48)
#define PCRE2_ZERO_TERMINATED (~(size_t)0)
#endif

static pcre2_code *(*p_compile)(const uint8_t *, size_t, uint32_t, int *, size_t *, void *);
static int (*p_substitute)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *,
                           const uint8_t *, size_t, uint8_t *, size_t *);
// for cv_rules_apply_mapped (optional: older libraries lack REPLACEMENT_ONLY)
static int (*p_match)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *);
static pcre2_match_data *(*p_md_create)(const pcre2_code *, void *);
static void (*p_md_free)(pcre2_match_data *);
static size_t *(*p_ovector)(pcre2_match_data *);
static int pcre_state;  // 0 untried, 1 loaded, -1 unavailable
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int load_pcre(void) {
    if (pcre_state) return pcre_state > 0;
#ifdef CV_PCRE2_STATIC
    p_compile = (pcre2_code * (*)(const uint8_t *, size_t, uint32_t, int *, size_t *, void *)) pcre2_compile_8;
    p_substitute = (int (*)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *,
                            const uint8_t *, size_t, uint8_t *, size_t *))pcre2_substitute_8;
    p_match = (int (*)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *))pcre2_match_8;
    p_md_create = (pcre2_match_data * (*)(const pcre2_code *, void *)) pcre2_match_data_create_from_pattern_8;
    p_md_free = pcre2_match_data_free_8;
    p_ovector = (size_t * (*)(pcre2_match_data *)) pcre2_get_ovector_pointer_8;
#else
    void *h = dlopen("libpcre2-8.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libpcre2-8.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        p_compile = (pcre2_code * (*)(const uint8_t *, size_t, uint32_t, int *, size_t *, void *)) dlsym(h, "pcre2_compile_8");
        p_substitute = (int (*)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *,
                                const uint8_t *, size_t, uint8_t *, size_t *))dlsym(h, "pcre2_substitute_8");
        p_match = (int (*)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *))dlsym(
            h, "pcre2_match_8");
        p_md_create = (pcre2_match_data * (*)(const pcre2_code *, void *)) dlsym(h, "pcre2_match_data_create_from_pattern_8");
        p_md_free = (void (*)(pcre2_match_data *))dlsym(h, "pcre2_match_data_free_8");
        p_ovector = (size_t * (*)(pcre2_match_data *)) dlsym(h, "pcre2_get_ovector_pointer_8");
    }
#endif
    pcre_state = p_compile && p_substitute ? 1 : -1;
    if (pcre_state < 0) fprintf(stderr, "classicvoices: libpcre2-8 not found; Apple's pronunciation rules are off\n");
    return pcre_state > 0;
}

typedef struct {
    char *pat;
    uint32_t opts;
    pcre2_code *re;  // compiled the first time the rule is needed
    int bad;         // didn't compile
    char *repl;      // PCRE2 replacement: $1 groups, $$ for a literal $
} Rule;

// Thousands of rules for some languages: each is compiled only when the
// pre-check (vxrules.h) says a text might contain a match, so loading and
// speaking stay quick.
struct CvRules {
    Rule *r;
    size_t n;
    VxRuleIndex *index;
    pthread_mutex_t mu;  // compiling
};

static pcre2_code *rule_code(CvRules *rs, size_t i) {
    Rule *r = &rs->r[i];
    pthread_mutex_lock(&rs->mu);
    if (!r->re && !r->bad) {
        int err;
        size_t erroff;
        r->re = p_compile((const uint8_t *)r->pat, strlen(r->pat), r->opts, &err, &erroff, NULL);
        r->bad = !r->re;
    }
    pthread_mutex_unlock(&rs->mu);
    return r->re;
}

// The rules worth running on text: marks[i] set (calloc'd, n bytes).
static uint8_t *candidates(const CvRules *rs, const char *text) {
    uint8_t *m = calloc(rs->n + 1, 1);
    vx_rule_index_mark(rs->index, text, m, rs->n);
    return m;
}

// C-style escapes in the file's replacement strings (\xHH, \n, \t, \e, \\, \").
static char *unescape(const char *s, size_t n) {
    char *o = malloc(n * 4 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c != '\\' || i + 1 >= n) { o[k++] = c; continue; }
        char e = s[++i];
        if (e == 'x') {
            unsigned v = 0, d = 0;
            while (d < 2 && i + 1 < n && isxdigit((unsigned char)s[i + 1])) {
                char h = s[++i];
                v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                d++;
            }
            if (d) {
                if (v < 0x80) o[k++] = (char)v;
                else { o[k++] = (char)(0xC0 | v >> 6); o[k++] = (char)(0x80 | (v & 0x3F)); }
            }
        } else if (e == 'n') o[k++] = '\n';
        else if (e == 't') o[k++] = '\t';
        else if (e == 'e') o[k++] = 0x1b;
        else o[k++] = e;
    }
    o[k] = 0;
    return o;
}

// A literal replacement as a PCRE2 template, keeping $1 .. $9.
static char *template(const char *s) {
    size_t n = strlen(s);
    char *o = malloc(n * 2 + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '$' && !isdigit((unsigned char)s[i + 1])) { o[k++] = '$'; o[k++] = '$'; }
        else o[k++] = s[i];
    }
    o[k] = 0;
    return o;
}

static CvRules *parse(const char *text) {
    CvRules *rs = calloc(1, sizeof *rs);
    rs->index = vx_rule_index_new();
    pthread_mutex_init(&rs->mu, NULL);
    const char *p = text;
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = 0;
        p += len;
        while (*p == '\r' || *p == '\n') p++;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *arrow = strstr(s, "-->");
        if (*s != '/' || !arrow) { free(line); continue; }
        char *left_end = arrow;
        while (left_end > s && isspace((unsigned char)left_end[-1])) left_end--;
        char *right = arrow + 3;
        while (isspace((unsigned char)*right)) right++;
        char *rend = right + strlen(right);
        while (rend > right && isspace((unsigned char)rend[-1])) rend--;
        char *hash = strstr(right, "\" #");  // trailing comment
        if (hash && hash < rend) rend = hash + 1;
        char *close = left_end - 1;
        while (close > s && *close != '/') close--;
        if (close <= s || rend - right < 2 || *right != '"' || rend[-1] != '"') { free(line); continue; }
        uint32_t opts = PCRE2_UTF | PCRE2_UCP;
        for (char *f = close + 1; f < left_end; f++) {
            if (*f == 'i') opts |= PCRE2_CASELESS;
            if (*f == 'm') opts |= PCRE2_MULTILINE;
        }
        {
            char *lit = unescape(right + 1, (size_t)(rend - right - 2));
            if (rs->n % 256 == 0) rs->r = realloc(rs->r, (rs->n + 256) * sizeof *rs->r);
            Rule *r = &rs->r[rs->n];
            memset(r, 0, sizeof *r);
            r->pat = malloc((size_t)(close - s));
            memcpy(r->pat, s + 1, (size_t)(close - s - 1));
            r->pat[close - s - 1] = 0;
            r->opts = opts;
            r->repl = template(lit);
            vx_rule_index_add(rs->index, (uint32_t)rs->n, r->pat, (opts & PCRE2_CASELESS) != 0);
            rs->n++;
            free(lit);
        }
        free(line);
    }
    return rs;
}

char *cv_rules_apply(const CvRules *rules, const char *text) {
    char *cur = strdup(text);
    if (!rules) return cur;
    CvRules *rs = (CvRules *)rules;
    uint8_t *want = candidates(rs, cur);
    for (size_t i = 0; i < rules->n; i++) {
        if (!want[i] || !rule_code(rs, i)) continue;
        size_t cap = strlen(cur) * 2 + 64;
        uint8_t *out = malloc(cap);
        size_t outlen = cap;
        int rc = p_substitute(rules->r[i].re, (const uint8_t *)cur, PCRE2_ZERO_TERMINATED, 0,
                              PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH, NULL, NULL,
                              (const uint8_t *)rules->r[i].repl, PCRE2_ZERO_TERMINATED, out, &outlen);
        if (rc == PCRE2_ERROR_NOMEMORY) {  // outlen now holds the size needed
            free(out);
            cap = outlen + 1;
            out = malloc(cap);
            outlen = cap;
            rc = p_substitute(rules->r[i].re, (const uint8_t *)cur, PCRE2_ZERO_TERMINATED, 0, PCRE2_SUBSTITUTE_GLOBAL,
                              NULL, NULL, (const uint8_t *)rules->r[i].repl, PCRE2_ZERO_TERMINATED, out, &outlen);
        }
        if (rc > 0) {  // rc = number of replacements
            free(cur);
            cur = (char *)out;
            vx_rule_index_mark(rs->index, cur, want, rs->n);  // what the new text brings into play
        } else {
            free(out);
        }
    }
    free(want);
    return cur;
}

// Grows a byte buffer with a parallel map.
typedef struct { char *s; int32_t *m; size_t n, cap; } Mapped;

static void mput(Mapped *o, const char *b, size_t n, const int32_t *src, int32_t one) {
    if (o->n + n + 1 > o->cap) {
        o->cap = (o->n + n + 1) * 2;
        o->s = realloc(o->s, o->cap);
        o->m = realloc(o->m, o->cap * sizeof *o->m);
    }
    memcpy(o->s + o->n, b, n);
    for (size_t i = 0; i < n; i++) o->m[o->n + i] = src ? src[i] : one;
    o->n += n;
    o->s[o->n] = 0;
}

char *cv_rules_apply_mapped(const CvRules *rules, const char *text, const int32_t *map_in, int32_t **map_out) {
    size_t n = strlen(text);
    Mapped cur = {0};
    if (map_in) mput(&cur, text, n, map_in, 0);
    else {
        int32_t *ident = malloc((n + 1) * sizeof *ident);
        for (size_t i = 0; i < n; i++) ident[i] = (int32_t)i;
        mput(&cur, text, n, ident, 0);
        free(ident);
    }
    if (!cur.s) mput(&cur, "", 0, NULL, 0);
    if (rules && p_match && p_md_create && p_md_free && p_ovector) {
        CvRules *rs = (CvRules *)rules;
        uint8_t *want = candidates(rs, cur.s);
        for (size_t i = 0; i < rules->n; i++) {
            if (!want[i] || !rule_code(rs, i)) continue;
            pcre2_match_data *md = p_md_create(rules->r[i].re, NULL);
            Mapped out = {0};
            size_t pos = 0, copied = 0;
            int changed = 0;
            while (pos <= cur.n && p_match(rules->r[i].re, (const uint8_t *)cur.s, cur.n, pos, 0, md, NULL) > 0) {
                size_t *ov = p_ovector(md);
                size_t ms = ov[0], me = ov[1];
                uint8_t rep[1024];
                size_t rl = sizeof rep;
                if (p_substitute(rules->r[i].re, (const uint8_t *)cur.s, cur.n, ms,
                                 PCRE2_SUBSTITUTE_MATCHED | PCRE2_SUBSTITUTE_REPLACEMENT_ONLY, md, NULL,
                                 (const uint8_t *)rules->r[i].repl, PCRE2_ZERO_TERMINATED, rep, &rl) < 0) break;
                mput(&out, cur.s + copied, ms - copied, cur.m + copied, 0);
                // the replacement stands for the text it replaced: map it there
                mput(&out, (const char *)rep, rl, NULL, ms < cur.n ? cur.m[ms] : (cur.n ? cur.m[cur.n - 1] : 0));
                copied = me;
                changed = 1;
                if (me > ms) pos = me;
                else {  // an empty match: step over one character
                    if (me >= cur.n) break;
                    pos = me + 1;
                    while (pos < cur.n && ((unsigned char)cur.s[pos] & 0xC0) == 0x80) pos++;
                }
            }
            p_md_free(md);
            if (changed) {
                mput(&out, cur.s + copied, cur.n - copied, cur.m + copied, 0);
                free(cur.s);
                free(cur.m);
                cur = out;
                vx_rule_index_mark(rs->index, cur.s, want, rs->n);
            } else {
                free(out.s);
                free(out.m);
            }
        }
        free(want);
    }
    if (map_out) *map_out = cur.m;
    else free(cur.m);
    return cur.s;
}

// ---------------------------------------------------------------- per voice

typedef struct { char path[1200 * 4]; CvRules *rules; } Cached;  // path: the files, "|"-separated
static Cached *g_cache;
static size_t g_ncache;

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 2);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\n';
    buf[got + 1] = 0;
    fclose(f);
    return buf;
}

// The rules of several files, in order, as one set (cached by their paths).
// NULL when none of them exists.
static CvRules *load_files(const char *const *paths, int n) {
    char key[1200 * 4] = "";
    for (int i = 0; i < n; i++) {
        strncat(key, paths[i], sizeof key - strlen(key) - 2);
        strncat(key, "|", sizeof key - strlen(key) - 1);
    }
    for (size_t i = 0; i < g_ncache; i++)
        if (!strcmp(g_cache[i].path, key)) return g_cache[i].rules;
    char *all = NULL;
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        char *t = slurp(paths[i]);
        if (!t) continue;
        size_t k = strlen(t);
        all = realloc(all, len + k + 1);
        memcpy(all + len, t, k + 1);
        len += k;
        free(t);
    }
    CvRules *r = all ? parse(all) : NULL;
    free(all);
    g_cache = realloc(g_cache, (g_ncache + 1) * sizeof *g_cache);
    snprintf(g_cache[g_ncache].path, sizeof g_cache[g_ncache].path, "%s", key);
    g_cache[g_ncache++].rules = r;
    return r;
}

static int exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The user_rules file a voice's own release had, written into out; 0 if none.
static int era_file(const char *voices_root, const char *downloads_root, const char *voice_key, const char *language,
                    char *out, size_t cap) {
    if (!voice_key || !*voice_key || !strncmp(voice_key, "mt", 2)) return 0;
    const char *slash = strchr(voice_key, '/');
    if (!slash) return 0;
    int bl = (int)(slash - voice_key);
    if (!strncmp(voice_key, "mac", 3)) {
        // a Mac OS X voice's own rules, in its folder (downloaded, or bundled)
        if (downloads_root) {
            snprintf(out, cap, "%s/%s/user_rules.dat", downloads_root, voice_key);
            if (exists(out)) return 1;
        }
        snprintf(out, cap, "%s/%s/user_rules.dat", voices_root, voice_key);
        return exists(out);
    }
    if (!language || !*language) return 0;
    if (voice_key[bl - 1] == 'e') bl--;  // an enhanced voice: its firmware's rules
    snprintf(out, cap, "%s/%.*s/TTSResources/%s/user_rules.txt", voices_root, bl, voice_key, language);
    return exists(out);
}

const CvRules *cv_rules_for_voice_pack(const char *voices_root, const char *downloads_root, const char *voice_key,
                                       const char *language, int own, const char *pack_dir) {
    char era[1200], user[1300], over[1300];
    const char *paths[3];
    int n = 0;
    if (own && era_file(voices_root, downloads_root, voice_key, language, era, sizeof era)) paths[n++] = era;
    if (pack_dir && *pack_dir && strncmp(voice_key ? voice_key : "", "mt", 2)) {
        // The engine runs the rule set loaded last first: iOS 12 loaded the
        // overrides last, so they come before the user rules (checked: the
        // same audio as with both loaded into the engine).
        snprintf(user, sizeof user, "%s/vocalizer-user-rules.txt", pack_dir);
        snprintf(over, sizeof over, "%s/vocalizer-override-rules.txt", pack_dir);
        paths[n++] = over;
        paths[n++] = user;
    }
    if (!n) return NULL;
    pthread_mutex_lock(&g_mu);
    const CvRules *out = load_pcre() ? load_files(paths, n) : NULL;
    pthread_mutex_unlock(&g_mu);
    return out;
}

const CvRules *cv_rules_for_voice(const char *voices_root, const char *downloads_root, const char *voice_key,
                                  const char *language) {
    return cv_rules_for_voice_pack(voices_root, downloads_root, voice_key, language, 1, NULL);
}

const char *cv_rules_pack_dir(const char *data_root, const char *voice_key, const char *language, char *out,
                              size_t cap) {
    if (!data_root || !voice_key || !strncmp(voice_key, "mt", 2) || !language || !*language) return NULL;
    snprintf(out, cap, "%s/Pronunciation/%s", data_root, language);
    char probe[1300];
    snprintf(probe, sizeof probe, "%s/vocalizer-user-rules.txt", out);
    return exists(probe) ? out : NULL;
}
