#include "cv_rules.h"

#include <ctype.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The few PCRE2 entry points used, resolved with dlsym (no build dependency).
typedef struct pcre2_code_8 pcre2_code;
typedef struct pcre2_match_data_8 pcre2_match_data;
#define PCRE2_CASELESS 0x00000008u
#define PCRE2_MULTILINE 0x00000400u
#define PCRE2_UCP 0x00020000u
#define PCRE2_UTF 0x00080000u
#define PCRE2_SUBSTITUTE_GLOBAL 0x00000100u
#define PCRE2_SUBSTITUTE_OVERFLOW_LENGTH 0x00001000u
#define PCRE2_ERROR_NOMEMORY (-48)
#define PCRE2_ZERO_TERMINATED (~(size_t)0)

static pcre2_code *(*p_compile)(const uint8_t *, size_t, uint32_t, int *, size_t *, void *);
static int (*p_substitute)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *,
                           const uint8_t *, size_t, uint8_t *, size_t *);
static int pcre_state;  // 0 untried, 1 loaded, -1 unavailable
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int load_pcre(void) {
    if (pcre_state) return pcre_state > 0;
    void *h = dlopen("libpcre2-8.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libpcre2-8.so", RTLD_NOW | RTLD_LOCAL);
    if (h) {
        p_compile = (pcre2_code * (*)(const uint8_t *, size_t, uint32_t, int *, size_t *, void *)) dlsym(h, "pcre2_compile_8");
        p_substitute = (int (*)(const pcre2_code *, const uint8_t *, size_t, size_t, uint32_t, pcre2_match_data *, void *,
                                const uint8_t *, size_t, uint8_t *, size_t *))dlsym(h, "pcre2_substitute_8");
    }
    pcre_state = p_compile && p_substitute ? 1 : -1;
    if (pcre_state < 0) fprintf(stderr, "classicvoices: libpcre2-8 not found; Apple's pronunciation rules are off\n");
    return pcre_state > 0;
}

typedef struct {
    pcre2_code *re;
    char *repl;  // PCRE2 replacement: $1 groups, $$ for a literal $
} Rule;

struct CvRules {
    Rule *r;
    size_t n;
};

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
        int err;
        size_t erroff;
        pcre2_code *re = p_compile((const uint8_t *)(s + 1), (size_t)(close - s - 1), opts, &err, &erroff, NULL);
        if (re) {
            char *lit = unescape(right + 1, (size_t)(rend - right - 2));
            rs->r = realloc(rs->r, (rs->n + 1) * sizeof *rs->r);
            rs->r[rs->n].re = re;
            rs->r[rs->n].repl = template(lit);
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
    for (size_t i = 0; i < rules->n; i++) {
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
        } else {
            free(out);
        }
    }
    return cur;
}

// ---------------------------------------------------------------- per voice

typedef struct { char path[1200]; CvRules *rules; } Cached;
static Cached *g_cache;
static size_t g_ncache;

static CvRules *load_file(const char *path, int *found) {
    for (size_t i = 0; i < g_ncache; i++)
        if (!strcmp(g_cache[i].path, path)) { *found = 1; return g_cache[i].rules; }
    *found = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    buf[fread(buf, 1, (size_t)n, f)] = 0;
    fclose(f);
    CvRules *r = parse(buf);
    free(buf);
    g_cache = realloc(g_cache, (g_ncache + 1) * sizeof *g_cache);
    snprintf(g_cache[g_ncache].path, sizeof g_cache[g_ncache].path, "%s", path);
    g_cache[g_ncache++].rules = r;
    *found = 1;
    return r;
}

const CvRules *cv_rules_for(const char *voices_root, const char *build, const char *language) {
    pthread_mutex_lock(&g_mu);
    const CvRules *out = NULL;
    if (load_pcre() && language && *language) {
        const char *builds[3] = {"ios6", build, "ios5"};
        for (int i = 0; i < 3 && !out; i++) {
            if (!builds[i]) continue;
            char path[1200];
            snprintf(path, sizeof path, "%s/%s/TTSResources/%s/user_rules.txt", voices_root, builds[i], language);
            int found;
            CvRules *r = load_file(path, &found);
            if (found && r) out = r;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return out;
}
