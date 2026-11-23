#include "cv_dict.h"
#include "vxphonemes.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

// ------------------------------------------------------------------ dictionaries

// A phonetic span is a private-use character standing in the text from the
// dictionary until the phonemes go in: nothing (later entries, Apple's rules)
// can match inside it.
#define SPAN_BASE 0xF0000u
#define SPAN_MAX 0xFFFDu

// A choice in a span, (a|b|c): which option follows the alternative its capture group matched.
typedef struct {
    char **alt;  // the group's top-level alternatives in the pattern (matched whole against what it captured)
    int nalt;
    char **opt;  // the options; one more than the alternatives: for when the group took no part
    int nopt;
    int caseless;
} Choice;

struct CvDict {
    CvRules *rules;
    char **ipa;   // per span (with choices: its first options, a fallback)
    char **xs;    // per span: its X-SAMPA with \x01 where each choice goes, or NULL when it has none
    Choice **ch;  // per span: its choices, in order
    int *nch;
    size_t nipa;
    char key[4096];  // files, sizes and times it was read from ("=" and the text for cv_dict_from_text)
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static CvDict **g_dicts;
static size_t g_ndicts;

typedef struct { char *s; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void putc_(Buf *b, char c) { put(b, &c, 1); }

static void put_span(Buf *b, uint32_t k) {
    uint32_t cp = SPAN_BASE + k;
    char u[4] = {(char)(0xF0 | cp >> 18), (char)(0x80 | ((cp >> 12) & 0x3F)), (char)(0x80 | ((cp >> 6) & 0x3F)),
                 (char)(0x80 | (cp & 0x3F))};
    put(b, u, 4);
}

// The ~~ that closes a span opened before `from`: the last two of the first
// run of two or more tildes (X-SAMPA's own ~ never doubles). Returns the
// offset of that run's end, *content_end where the span's text stops; 0 if none.
static size_t span_close(const char *s, size_t from, size_t *content_end) {
    for (size_t j = from; s[j]; j++) {
        if (s[j] == '~' && s[j + 1] == '~') {
            size_t r = j;
            while (s[r] == '~') r++;
            *content_end = r - 2;
            return r;
        }
    }
    return 0;
}

// Markers around what a choice's capture group matched, after its span's placeholder (U+10FF00, U+10FF01).
static const char kChoiceOpen[] = "\xF4\x8F\xBC\x80", kChoiceClose[] = "\xF4\x8F\xBC\x81";

// The top-level alternatives of capturing group n (from 1) of a pattern (Python's syntax, as PCRE2 reads it), or
// NULL when it has no such group.
static char **group_alts(const char *p, int n, int *na) {
    int count = 0, depth = 0, target = -1, cls = 0;
    size_t start = 0;
    char **out = NULL;
    *na = 0;
    for (size_t i = 0; p[i]; i++) {
        char c = p[i];
        if (c == '\\' && p[i + 1]) { i++; continue; }
        if (cls) { if (c == ']') cls = 0; continue; }
        if (c == '[') { cls = 1; if (p[i + 1] == '^') i++; if (p[i + 1] == ']') i++; continue; }
        if (c == '(') {
            int capturing = 1;
            size_t body = i + 1;
            if (p[i + 1] == '?') {
                capturing = 0;
                if (p[i + 2] == 'P' && p[i + 3] == '<') capturing = 1;
                else if (p[i + 2] == '<' && p[i + 3] != '=' && p[i + 3] != '!') capturing = 1;
                if (capturing) { const char *gt = strchr(p + i, '>'); body = gt ? (size_t)(gt - p) + 1 : i + 1; }
            }
            depth++;
            if (capturing && ++count == n) { target = depth; start = body; }
            continue;
        }
        if (target == depth && (c == '|' || c == ')')) {
            out = realloc(out, (size_t)(*na + 1) * sizeof *out);
            out[(*na)++] = strndup(p + start, i - start);
            start = i + 1;
            if (c == ')') return out;
        }
        if (c == ')') depth--;
    }
    for (int k = 0; k < *na; k++) free(out[k]);
    free(out);
    *na = 0;
    return NULL;
}

// A span's X-SAMPA with each choice's option picked (which[k]); malloc'd.
static char *pick_options(const char *xs, const Choice *ch, const int *which) {
    Buf b = {0};
    int k = 0;
    for (const char *c = xs; *c; c++) {
        if (*c == 1) {
            const char *o = which[k] < ch[k].nopt ? ch[k].opt[which[k]] : "";
            put(&b, o, strlen(o));
            k++;
        } else putc_(&b, *c);
    }
    if (!b.s) put(&b, "", 0);
    return b.s;
}

static void free_choices(Choice *ch, int nch) {
    for (int k = 0; k < nch; k++) {
        for (int o = 0; o < ch[k].nopt; o++) free(ch[k].opt[o]);
        for (int a = 0; a < ch[k].nalt; a++) free(ch[k].alt[a]);
        free(ch[k].opt);
        free(ch[k].alt);
    }
    free(ch);
}

// The replacement as a PCRE2 template, spans as placeholders; 0 if a span isn't X-SAMPA.
static int make_template(CvDict *d, const char *rep, int regex, Buf *t, const char *line, int caseless) {
    int choices_seen = 0;  // the n-th choice of the replacement follows capture group n
    for (size_t i = 0; rep[i];) {
        if (rep[i] == '~' && rep[i + 1] == '~' && rep[i + 2] != '~') {
            size_t ce, end = span_close(rep, i + 2, &ce);
            if (end && ce > i + 2) {
                // choices, (a|b): their options split out, \x01 where they go
                Buf xs = {0};
                Choice *ch = NULL;
                int nch = 0, ok = 1;
                for (size_t j = i + 2; j < ce && ok; j++) {
                    if (rep[j] != '(') { putc_(&xs, rep[j]); continue; }
                    const char *close = memchr(rep + j, ')', ce - j);
                    if (!close) { ok = 0; break; }
                    ch = realloc(ch, (size_t)(nch + 1) * sizeof *ch);
                    Choice *c = &ch[nch++];
                    memset(c, 0, sizeof *c);
                    c->caseless = caseless;
                    for (const char *o = rep + j + 1;;) {
                        const char *bar = memchr(o, '|', (size_t)(close - o));
                        const char *oe = bar ? bar : close;
                        c->opt = realloc(c->opt, (size_t)(c->nopt + 1) * sizeof *c->opt);
                        c->opt[c->nopt++] = strndup(o, (size_t)(oe - o));
                        if (!bar) break;
                        o = bar + 1;
                    }
                    ++choices_seen;
                    if (regex) c->alt = group_alts(line, choices_seen, &c->nalt);
                    putc_(&xs, 1);
                    j = (size_t)(close - rep);
                }
                // checked as X-SAMPA: the span with its first options, and every option on its own
                int *first = calloc((size_t)nch + 1, sizeof *first);
                char *x0 = ok && xs.s ? pick_options(xs.s, ch, first) : NULL;
                size_t bad = 0;
                char *ipa = x0 && *x0 ? vx_xsampa_to_ipa(x0, strlen(x0), &bad) : x0 ? strdup("") : NULL;
                for (int k = 0; k < nch && ipa; k++)
                    for (int o = 0; o < ch[k].nopt && ipa; o++) {
                        if (!*ch[k].opt[o]) continue;
                        char *t2 = vx_xsampa_to_ipa(ch[k].opt[o], strlen(ch[k].opt[o]), &bad);
                        if (!t2) { free(ipa); ipa = NULL; }
                        free(t2);
                    }
                free(first);
                free(x0);
                if (!ipa || d->nipa > SPAN_MAX) {
                    if (!ipa) fprintf(stderr, "classicvoices: not X-SAMPA in: %s\n", line);
                    free(ipa);
                    free(xs.s);
                    free_choices(ch, nch);
                    return 0;
                }
                d->ipa = realloc(d->ipa, (d->nipa + 1) * sizeof *d->ipa);
                d->xs = realloc(d->xs, (d->nipa + 1) * sizeof *d->xs);
                d->ch = realloc(d->ch, (d->nipa + 1) * sizeof *d->ch);
                d->nch = realloc(d->nch, (d->nipa + 1) * sizeof *d->nch);
                d->ipa[d->nipa] = ipa;
                d->xs[d->nipa] = nch ? xs.s : NULL;
                if (!nch) free(xs.s);
                d->ch[d->nipa] = ch;
                d->nch[d->nipa] = nch;
                put_span(t, (uint32_t)d->nipa++);
                for (int k = 0; k < nch; k++) {  // what each choice's group matched, between markers
                    put(t, kChoiceOpen, 4);
                    if (ch[k].alt) {
                        char g[16];
                        snprintf(g, sizeof g, "${%d}", choices_seen - nch + k + 1);
                        put(t, g, strlen(g));
                    }
                    put(t, kChoiceClose, 4);
                }
                i = end;
                continue;
            }
        }

        char c = rep[i];
        if (c == '$') { put(t, "$$", 2); i++; continue; }
        if (regex && c == '\\' && rep[i + 1]) {  // Python's template escapes
            char e = rep[i + 1];
            if (isdigit((unsigned char)e)) {
                size_t j = i + 1;
                char num[8];
                size_t nn = 0;
                while (isdigit((unsigned char)rep[j]) && nn < 2) num[nn++] = rep[j++];
                num[nn] = 0;
                put(t, "${", 2); put(t, num, nn); putc_(t, '}');
                i = j;
                continue;
            }
            if (e == 'g' && rep[i + 2] == '<') {
                const char *gt = strchr(rep + i + 3, '>');
                if (gt) {
                    put(t, "${", 2); put(t, rep + i + 3, (size_t)(gt - (rep + i + 3))); putc_(t, '}');
                    i = (size_t)(gt - rep) + 1;
                    continue;
                }
            }
            if (e == '\\') { putc_(t, '\\'); i += 2; continue; }
            if (e == 'n') { putc_(t, '\n'); i += 2; continue; }
            if (e == 't') { putc_(t, '\t'); i += 2; continue; }
            // anything else stays as written, as Python leaves it
        }
        putc_(t, c);
        i++;
    }
    if (!t->s) put(t, "", 0);
    return 1;
}

static void escape_literal(Buf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80 && !isalnum(c) && c != ' ') putc_(b, '\\');
        putc_(b, (char)c);
    }
}

typedef struct { char *pat, *rep; int caseless, type; } Entry;

static char *slurp(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? malloc((size_t)n + 1) : NULL;
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[got] = 0;
    if (size) *size = (long)got;
    return buf;
}

// The entries of one file (pattern, replacement, case, type), in order.
static Entry *read_entries(const char *text, size_t *count) {
    Entry *e = NULL;
    size_t n = 0;
    const char *p = text;
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
    while (*p) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(len + 1);
        memcpy(line, p, len);
        line[len] = 0;
        p += len;
        while (*p == '\r' || *p == '\n') p++;
        if (!*line || *line == '#') { free(line); continue; }
        char *f[4] = {line, NULL, NULL, NULL};
        int nf = 1;
        for (char *c = line; *c && nf < 4; c++)
            if (*c == '\t') { *c = 0; f[nf++] = c + 1; }
        if (nf < 2 || !*f[0]) { free(line); continue; }
        e = realloc(e, (n + 1) * sizeof *e);
        e[n].pat = strdup(f[0]);
        e[n].rep = strdup(f[1]);
        e[n].caseless = !(nf > 2 && (!strcmp(f[2], "1") || !strcmp(f[2], "True")));
        e[n].type = nf > 3 ? atoi(f[3]) : 0;
        n++;
        free(line);
    }
    *count = n;
    return e;
}

static int file_exists(const char *path, struct stat *st) { return stat(path, st) == 0 && S_ISREG(st->st_mode); }

// One file of a dictionary: the fetched one (path, "" when the community dictionaries are off or there's none), the
// person's own (mine: Mine/<file>, "" for none) and the fetched entries they removed (Mine/<file>.removed: pattern
// TAB type per line; "" for none).
#define MAXF 4
typedef struct { char path[1400], removed[1400], mine[1400]; } DictFile;

static int removed_has(const char *text, const Entry *e) {
    if (!text) return 0;
    for (const char *p = text; *p;) {
        const char *eol = strpbrk(p, "\r\n");
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        const char *tab = memchr(p, '\t', len);
        size_t pl = tab ? (size_t)(tab - p) : len;
        int type = tab ? atoi(tab + 1) : 0;
        if (pl == strlen(e->pat) && !memcmp(p, e->pat, pl) && type == e->type) return 1;
        p += len;
        while (*p == '\r' || *p == '\n') p++;
    }
    return 0;
}

static void add_entries(CvDict *d, Entry *e, size_t n, const char *removed, Entry **seen, size_t *nseen,
                        size_t *capseen) {
    size_t first_of_file = *nseen;
    for (size_t i = 0; i < n; i++) {
        int shadowed = removed_has(removed, &e[i]);
        for (size_t j = 0; j < first_of_file && !shadowed; j++)
            shadowed = (*seen)[j].type == e[i].type && !strcmp((*seen)[j].pat, e[i].pat);
        Buf pat = {0}, t = {0};
        if (e[i].type == 1) put(&pat, e[i].pat, strlen(e[i].pat));
        else {
            if (e[i].type == 2) put(&pat, "\\b", 2);
            escape_literal(&pat, e[i].pat);
            if (e[i].type == 2) put(&pat, "\\b", 2);
        }
        if (!shadowed && make_template(d, e[i].rep, e[i].type == 1, &t, e[i].pat, e[i].caseless))
            cv_rules_add(d->rules, pat.s, e[i].caseless, t.s);
        free(pat.s);
        free(t.s);
        if (*nseen == *capseen) { *capseen = *capseen ? *capseen * 2 : 256; *seen = realloc(*seen, *capseen * sizeof **seen); }
        (*seen)[(*nseen)++] = e[i];
    }
}

static Entry *read_file_entries(const char *path, size_t *n) {
    *n = 0;
    char *text = path[0] ? slurp(path, NULL) : NULL;
    if (!text) return NULL;
    Entry *e = read_entries(text, n);
    free(text);
    return e;
}

static void free_entry(Entry *e) { free(e->pat); free(e->rep); }

// A dictionary from its files, the first winning (an entry of a later file with the same pattern and type as one
// of an earlier file is left out). Within a file, the entries apply in the order they're written, as they will be
// once the person's changes are published (Levi, 2026-11-16): the fetched entries in order, one the person changed
// in its place, one they removed left out, then the person's own additions in their order.
static CvDict *build(const DictFile *files, int np, const char *key) {
    CvDict *d = calloc(1, sizeof *d);
    snprintf(d->key, sizeof d->key, "%s", key);
    d->rules = cv_rules_new();
    Entry *seen = NULL;
    size_t nseen = 0, capseen = 0;
    for (int f = 0; f < np; f++) {
        size_t cn, mn;
        Entry *ce = read_file_entries(files[f].path, &cn), *me = read_file_entries(files[f].mine, &mn);
        char *removed = files[f].removed[0] ? slurp(files[f].removed, NULL) : NULL;
        Entry *m = malloc((cn + mn + 1) * sizeof *m);
        char *used = calloc(mn + 1, 1);
        size_t k = 0;
        for (size_t i = 0; i < cn; i++) {
            if (removed_has(removed, &ce[i])) { free_entry(&ce[i]); continue; }
            size_t j = 0;
            while (j < mn && (used[j] || me[j].type != ce[i].type || strcmp(me[j].pat, ce[i].pat))) j++;
            if (j < mn) { m[k++] = me[j]; used[j] = 1; free_entry(&ce[i]); }  // the person's version, in its place
            else m[k++] = ce[i];
        }
        for (size_t j = 0; j < mn; j++)
            if (!used[j]) m[k++] = me[j];
        add_entries(d, m, k, NULL, &seen, &nseen, &capseen);
        free(m);
        free(used);
        free(removed);
        free(ce);
        free(me);
    }
    for (size_t j = 0; j < nseen; j++) { free(seen[j].pat); free(seen[j].rep); }
    free(seen);
    return d;
}

const CvDict *cv_dict_from_text(const char *dic_text) {
    if (!dic_text || !cv_rules_available()) return NULL;
    char key[4096];
    int fits = snprintf(key, sizeof key, "=%s", dic_text) < (int)sizeof key;
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; fits && i < g_ndicts; i++)
        if (!strcmp(g_dicts[i]->key, key)) { pthread_mutex_unlock(&g_mu); return g_dicts[i]; }
    pthread_mutex_unlock(&g_mu);
    CvDict *d = calloc(1, sizeof *d);
    snprintf(d->key, sizeof d->key, "%s", fits ? key : "");
    d->rules = cv_rules_new();
    Entry *seen = NULL;
    size_t n, nseen = 0, capseen = 0;
    Entry *e = read_entries(dic_text, &n);
    add_entries(d, e, n, NULL, &seen, &nseen, &capseen);
    free(e);
    for (size_t j = 0; j < nseen; j++) { free(seen[j].pat); free(seen[j].rep); }
    free(seen);
    pthread_mutex_lock(&g_mu);
    g_dicts = realloc(g_dicts, (g_ndicts + 1) * sizeof *g_dicts);  // kept: a caller may still use it
    g_dicts[g_ndicts++] = d;
    pthread_mutex_unlock(&g_mu);
    return d;
}

// Files up to this size are read while speech waits (Nico's en.dic: 7 KB, 2 ms); bigger ones in the background
// (50,000 entries: 134 ms), the voice speaking with the earlier copy, or none, until then.
#define SYNC_BYTES (64 * 1024)

// Per set of files: the newest dictionary read from them, and whether a newer one is being read.
typedef struct { char sig[MAXF * 1400 + 8]; CvDict *ready; int loading; } Latest;
static Latest *g_latest;
static size_t g_nlatest;

static Latest *latest(const char *sig) {  // (g_mu held)
    for (size_t i = 0; i < g_nlatest; i++) if (!strcmp(g_latest[i].sig, sig)) return &g_latest[i];
    g_latest = realloc(g_latest, (g_nlatest + 1) * sizeof *g_latest);
    Latest *l = &g_latest[g_nlatest++];
    memset(l, 0, sizeof *l);
    snprintf(l->sig, sizeof l->sig, "%s", sig);
    return l;
}

static void publish(Latest *l, CvDict *d) {  // (g_mu held) older copies stay: another thread may be using one
    g_dicts = realloc(g_dicts, (g_ndicts + 1) * sizeof *g_dicts);
    g_dicts[g_ndicts++] = d;
    (void)l;
    for (size_t i = 0; i < g_nlatest; i++)
        if (!strcmp(g_latest[i].sig, l->sig)) { g_latest[i].ready = d; g_latest[i].loading = 0; }
}

typedef struct { DictFile files[MAXF]; int np; char key[4096]; char sig[MAXF * 1400 + 8]; } Job;

static void *load_job(void *arg) {
    Job *j = arg;
    CvDict *d = build(j->files, j->np, j->key);
    pthread_mutex_lock(&g_mu);
    publish(latest(j->sig), d);
    pthread_mutex_unlock(&g_mu);
    free(j);
    return NULL;
}

static void start_loading(Latest *l, const DictFile *files, int np, const char *key) {  // (g_mu held)
    if (l->loading) return;
    Job *j = calloc(1, sizeof *j);
    memcpy(j->files, files, (size_t)np * sizeof *files);
    j->np = np;
    snprintf(j->key, sizeof j->key, "%s", key);
    snprintf(j->sig, sizeof j->sig, "%s", l->sig);
    pthread_t t;
    if (pthread_create(&t, NULL, load_job, j) == 0) {
        pthread_detach(t);
        l->loading = 1;
    } else {
        free(j);
    }
}

const CvDict *cv_dict_for_voice(const char *dir, const char *voice_key, const char *language) {
    return cv_dict_for_voice_ex(dir, voice_key, language, 1);
}

// While a dictionary editor is open (the Windows and Linux app's touches <dir>/editing every 15 seconds), the voices
// use no dictionary, so words are heard as they read them by themselves. A flag older than 45 seconds was left
// behind (the app quit or crashed) and doesn't count.
static int editing(const char *dir) {
    char p[1300];
    struct stat st;
    snprintf(p, sizeof p, "%s/editing", dir);
    if (stat(p, &st) != 0) return 0;
    double age = difftime(time(NULL), st.st_mtime);
    return age > -45 && age < 45;
}

// A file as the dictionary cache knows it: its path, size and time, and for a small one its contents too (an edit
// that keeps the length within the same second, or wherever the time isn't kept to the second, is still a change;
// Nico: an edited replacement was heard as before). Small files cost microseconds to read.
static void stamp(char *out, size_t cap, const char *prefix, const char *path, const struct stat *st) {
    uint32_t h = 2166136261u;
    if (st->st_size <= SYNC_BYTES) {
        FILE *f = fopen(path, "rb");
        if (f) {
            unsigned char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0)
                for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
            fclose(f);
        }
    }
    snprintf(out, cap, "%s%s:%lld:%lld:%08x|", prefix, path, (long long)st->st_size, (long long)st->st_mtime, h);
}

const CvDict *cv_dict_for_voice_ex(const char *dir, const char *voice_key, const char *language, int community) {
    if (!dir || !*dir || !voice_key || editing(dir)) return NULL;
    // the files' names, narrowest first
    char names[2][64];
    int nn = 0;
    if (!strncmp(voice_key, "mt", 2)) snprintf(names[nn++], sizeof names[0], "macintalk.dic");
    else if (language && *language) {
        char l[48];
        size_t k = 0;
        for (; language[k] && k < sizeof l - 1; k++) l[k] = language[k] == '_' ? '-' : (char)tolower((unsigned char)language[k]);
        l[k] = 0;
        snprintf(names[nn++], sizeof names[0], "%s.dic", l);
        char *dash = strchr(l, '-');
        if (dash) { *dash = 0; snprintf(names[nn++], sizeof names[0], "%s.dic", l); }
    }
    // each file: the fetched one when the community switch is on (less the entries the person removed), with the
    // person's own (Mine/) merged in; their own entries work with the switch off too
    DictFile files[MAXF];
    char key[4096] = "";
    int np = 0;
    long long total = 0;
    for (int i = 0; i < nn && np < MAXF; i++) {
        struct stat st;
        DictFile *f = &files[np];
        f->path[0] = f->removed[0] = f->mine[0] = 0;
        char part[3000] = "";
        snprintf(f->mine, sizeof f->mine, "%s/Mine/%s", dir, names[i]);
        if (file_exists(f->mine, &st)) {
            stamp(part, sizeof part, "", f->mine, &st);
            total += (long long)st.st_size;
        } else f->mine[0] = 0;
        if (community) {
            snprintf(f->path, sizeof f->path, "%s/%s", dir, names[i]);
            if (file_exists(f->path, &st)) {
                size_t pl = strlen(part);
                stamp(part + pl, sizeof part - pl, "", f->path, &st);
                total += (long long)st.st_size;
                snprintf(f->removed, sizeof f->removed, "%s/Mine/%s.removed", dir, names[i]);
                if (file_exists(f->removed, &st)) {
                    pl = strlen(part);
                    stamp(part + pl, sizeof part - pl, "-", f->removed, &st);
                } else f->removed[0] = 0;
            } else f->path[0] = 0;
        }
        if (!f->path[0] && !f->mine[0]) continue;
        strncat(key, part, sizeof key - strlen(key) - 1);
        np++;
    }
    if (!np || !cv_rules_available()) return NULL;
    char sig[MAXF * 1400 + 8] = "";  // the files, whatever their contents
    for (int i = 0; i < np; i++) {
        strncat(sig, files[i].path[0] ? files[i].path : files[i].mine, sizeof sig - strlen(sig) - 2);
        strcat(sig, files[i].mine[0] ? "+|" : "|");
    }
    pthread_mutex_lock(&g_mu);
    for (size_t i = 0; i < g_ndicts; i++)
        if (!strcmp(g_dicts[i]->key, key)) { pthread_mutex_unlock(&g_mu); return g_dicts[i]; }
    Latest *l = latest(sig);
    if (total <= SYNC_BYTES || !l->ready) {
        // A small set is read now (a few ms: the first words after a change have it). A big one never makes
        // speech wait, the first time included: it loads in the background, the voice going without meanwhile.
        if (total > SYNC_BYTES) {
            start_loading(l, files, np, key);
            pthread_mutex_unlock(&g_mu);
            return NULL;
        }
        pthread_mutex_unlock(&g_mu);
        CvDict *d = build(files, np, key);
        pthread_mutex_lock(&g_mu);
        publish(l, d);
        pthread_mutex_unlock(&g_mu);
        return d;
    }
    start_loading(l, files, np, key);  // the earlier copy is used until the new one is ready
    const CvDict *d = l->ready;
    pthread_mutex_unlock(&g_mu);
    return d;
}

// The span placeholder at s, as its number, or -1.
static long span_at(const CvDict *d, const unsigned char *s) {
    if (s[0] != 0xF3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return -1;
    uint32_t cp = (uint32_t)(s[0] & 7) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
    return cp >= SPAN_BASE && cp - SPAN_BASE < d->nipa ? (long)(cp - SPAN_BASE) : -1;
}

// A letter of a word (ASCII letters and apostrophes, any non-ASCII byte).
static int word_byte(unsigned char c) { return isalpha(c) || c == '\'' || c >= 0x80; }

// Phoneme input in the engine's text: not for Apple's rules.
static const char *const kPhonIn[] = {"\x1b\\toi=lhp\\", "\x1b/+", "\x1b\\cvsp=", "\x1b\\cvpp="};
static const char *const kPhonOut[] = {"\x1b\\toi=orth\\", "\x1b/+", "\\", "\\"};

// Bytes with where each came from (a position in the caller's text).
typedef struct { char *s; int32_t *m; size_t n, cap; } MBuf;
static void mput(MBuf *b, const char *s, size_t n, const int32_t *src, int32_t one) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->s = realloc(b->s, b->cap);
        b->m = realloc(b->m, b->cap * sizeof *b->m);
    }
    memcpy(b->s + b->n, s, n);
    for (size_t i = 0; i < n; i++) b->m[b->n + i] = src ? src[i] : one;
    b->n += n;
    b->s[b->n] = 0;
}

// Apple's rules on the text between phoneme input. want_map: positions are kept (cv_rules_apply_mapped); without,
// the plain cv_rules_apply, which also works with a PCRE2 too old to report positions.
static void rules_outside_phonemes(const CvRules *rules, const char *t, const int32_t *map, MBuf *o, int want_map) {
    const char *p = t;
    size_t len = strlen(t);
    while (*p) {
        const char *next = NULL;
        int kind = -1;
        for (int k = 0; k < 4; k++) {
            const char *f = strstr(p, kPhonIn[k]);
            if (f && (!next || f < next)) { next = f; kind = k; }
        }
        size_t plain = next ? (size_t)(next - p) : strlen(p);
        if (plain) {
            size_t at = (size_t)(p - t);
            if (!rules) mput(o, p, plain, map + at, 0);
            else {
                char *chunk = malloc(plain + 1);
                memcpy(chunk, p, plain);
                chunk[plain] = 0;
                int32_t *rm = NULL;
                char *r = want_map ? cv_rules_apply_mapped(rules, chunk, map + at, &rm) : cv_rules_apply(rules, chunk);
                if (r && rm) mput(o, r, strlen(r), rm, 0);
                else if (r) mput(o, r, strlen(r), NULL, map[at]);
                else mput(o, p, plain, map + at, 0);
                free(r);
                free(rm);
                free(chunk);
            }
        }
        if (!next) break;
        const char *body = next + strlen(kPhonIn[kind]);
        const char *end = strstr(body, kPhonOut[kind]);
        end = end ? end + strlen(kPhonOut[kind]) : t + len;
        mput(o, next, (size_t)(end - next), map + (next - t), 0);
        p = end;
    }
    if (!o->s) mput(o, "", 0, NULL, 0);
}

char *cv_dict_speech_text_mapped(const CvDict *d, const CvRules *rules, const char *text, const int32_t *map_in,
                                 int32_t **map_out, CvDictPhonemeFn phoneme, void *ctx) {
    if (!text) text = "";
    size_t tl = strlen(text);
    // positions in text (for finding a spliced word), then in the caller's terms (map_in)
    int32_t *ident = malloc((tl + 1) * sizeof *ident);
    for (size_t i = 0; i <= tl; i++) ident[i] = (int32_t)i;
    MBuf o = {0};
    if (!d) {
        mput(&o, text, tl, ident, 0);
    } else {
        int32_t *map = NULL;
        char *a = cv_rules_apply_mapped(d->rules, text, NULL, &map);
        if (!map) {  // a PCRE2 without REPLACEMENT_ONLY: no positions, so no dictionary
            free(a);
            mput(&o, text, tl, ident, 0);
        } else {
            for (const unsigned char *s = (const unsigned char *)a; *s;) {
                size_t at = (size_t)((const char *)s - a);
                long k = span_at(d, s);
                if (k < 0) { mput(&o, (const char *)s, 1, map + at, 0); s++; continue; }
                s += 4;
                // choices: each option picked by the alternative its group matched (the markers carry what it
                // captured); a group that matched none of them, or took no part, picks the option after them
                char *chosen = NULL;
                if (d->xs[k]) {
                    const Choice *ch = d->ch[k];
                    int nch = d->nch[k], *which = calloc((size_t)nch + 1, sizeof *which);
                    for (int c = 0; c < nch && !memcmp(s, kChoiceOpen, 4); c++) {
                        const char *cap = (const char *)s + 4, *end = strstr(cap, kChoiceClose);
                        if (!end) break;
                        char *captured = strndup(cap, (size_t)(end - cap));
                        int w = 0;
                        if (ch[c].alt) {
                            w = ch[c].nalt;
                            for (int a2 = 0; a2 < ch[c].nalt; a2++)
                                if (cv_regex_full_match(ch[c].alt[a2], ch[c].caseless, captured)) { w = a2; break; }
                        }
                        which[c] = w;
                        free(captured);
                        s = (const unsigned char *)end + 4;
                    }
                    char *x = pick_options(d->xs[k], ch, which);
                    chosen = *x ? vx_xsampa_to_ipa(x, strlen(x), NULL) : strdup("");
                    free(x);
                    free(which);
                    if (chosen && !*chosen) { free(chosen); continue; }  // nothing to say
                }
                int joined = o.n && !isspace((unsigned char)o.s[o.n - 1]) && o.s[o.n - 1] != '\\';
                char *ph = phoneme ? phoneme(ctx, chosen ? chosen : d->ipa[k], joined) : NULL;
                free(chosen);
                if (!ph) continue;
                // Letters right before the span: the word's start (a regular expression's \1, "d" + "ecompile").
                // The engine would say them as a word of their own ("dee"), so it's told the whole word and those
                // letters (ESC\cvpp=decompile|d\), to say them as it would at the start of it (core/vxstream.c
                // spliced_prefix). Only for L&H+ phoneme input.
                size_t pre = 0;
                while (pre < o.n && word_byte((unsigned char)o.s[o.n - 1 - pre])) pre++;
                if (pre && pre < 48 && !strncmp(ph, kPhonIn[0], strlen(kPhonIn[0]))) {
                    size_t src = (size_t)map[at];
                    size_t ws = src, we = src;
                    while (ws > 0 && word_byte((unsigned char)text[ws - 1])) ws--;
                    while (we < tl && word_byte((unsigned char)text[we])) we++;
                    size_t wl = we - ws;
                    int starts = wl > pre && wl < 64;
                    for (size_t i = 0; starts && i < pre; i++)
                        starts = tolower((unsigned char)text[ws + i]) == tolower((unsigned char)o.s[o.n - pre + i]);
                    if (starts) {  // the letters go into the command (Apple's rules can't touch them there)
                        char cmd[160];
                        int32_t first = o.m[o.n - pre];
                        int n = snprintf(cmd, sizeof cmd, "\x1b\\cvpp=%.*s|%.*s\\", (int)wl, text + ws, (int)pre,
                                         o.s + o.n - pre);
                        if (n > 0 && (size_t)n < sizeof cmd) {
                            o.n -= pre;
                            mput(&o, cmd, (size_t)n, NULL, first);
                        }
                    }
                }
                // Letters right after the span: the word's rest (a regular expression's \1, "boost" + "ed"). The
                // engine reads letters after phonemes as a word of their own, so it's told the whole word, to say
                // the rest as it would in it (core/vxstream.c spliced_suffix). Only for L&H+ phoneme input.
                size_t rl = 0;
                while (s[rl] && word_byte(s[rl]) && span_at(d, s + rl) < 0) rl++;
                if (rl && !strncmp(ph, kPhonIn[0], strlen(kPhonIn[0]))) {
                    size_t src = (size_t)map[at];
                    size_t ws = src, we = src;
                    while (ws > 0 && word_byte((unsigned char)text[ws - 1])) ws--;
                    while (we < tl && word_byte((unsigned char)text[we])) we++;
                    size_t wl = we - ws;
                    int ends = wl > rl && wl < 64;
                    for (size_t i = 0; ends && i < rl; i++)
                        ends = tolower((unsigned char)text[we - rl + i]) == tolower(s[i]);
                    if (ends) {
                        char cmd[96];
                        int n = snprintf(cmd, sizeof cmd, "\x1b\\cvsp=%.*s|%zu\\", (int)wl, text + ws, wl - rl);
                        if (n > 0 && (size_t)n < sizeof cmd) mput(&o, cmd, (size_t)n, NULL, map[at]);
                    }
                }
                // The phonemes stand for the replaced text: its first position, and its last from their last
                // sound on (the engine's word ends there, before the switch back to text: ESC\toi=orth\, ESC/+ or
                // MacinTalk's [[inpt TEXT]]), so a word event keeps the word's length.
                size_t pl = strlen(ph), body_end = pl;
                const char *close = strrchr(ph, 0x1b);
                if (!close) close = strstr(ph, "[[inpt TEXT]]");
                if (close && close > ph) body_end = (size_t)(close - ph);
                if (body_end > 1) mput(&o, ph, body_end - 1, NULL, map[at]);
                if (body_end > 0) mput(&o, ph + body_end - 1, pl - body_end + 1, NULL, body_end > 1 ? map[at + 3] : map[at]);
                free(ph);
            }
            free(a);
            free(map);
        }
    }
    if (!o.s) mput(&o, "", 0, NULL, 0);
    MBuf r = {0};
    rules_outside_phonemes(rules, o.s, o.m, &r, map_out != NULL);
    free(o.s);
    free(o.m);
    free(ident);
    if (map_out) {
        if (map_in && tl) {  // from positions in text to the caller's
            for (size_t i = 0; i < r.n; i++) {
                int32_t at = r.m[i] < 0 ? 0 : (size_t)r.m[i] >= tl ? (int32_t)tl - 1 : r.m[i];
                r.m[i] = map_in[at];
            }
        }
        *map_out = r.m;
    } else {
        free(r.m);
    }
    return r.s;
}

char *cv_dict_speech_text(const CvDict *d, const CvRules *rules, const char *text, CvDictPhonemeFn phoneme, void *ctx) {
    return cv_dict_speech_text_mapped(d, rules, text, NULL, NULL, phoneme, ctx);
}
