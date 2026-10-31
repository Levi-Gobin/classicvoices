// See vxrules.h.
#include "vxrules.h"

#include <stdlib.h>
#include <string.h>

// ---- the literal a pattern requires

typedef struct {
    char *b;
    size_t n, cap, last;  // last: where the most recent character began
} Run;

static void run_put(Run *r, const char *s, size_t k) {
    if (r->n + k + 1 > r->cap) {
        r->cap = (r->n + k + 1) * 2;
        r->b = realloc(r->b, r->cap);
    }
    r->last = r->n;
    memcpy(r->b + r->n, s, k);
    r->n += k;
}

static size_t utf8_put(char *o, uint32_t c) {
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

static size_t utf8_get(const char *s, uint32_t *c) {
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *c = u[0]; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && u[1]) { *c = (uint32_t)(u[0] & 0x1F) << 6 | (u[1] & 0x3F); return 2; }
    if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        *c = (uint32_t)(u[0] & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F);
        return 3;
    }
    if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) {
        *c = (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3F) << 12 | (uint32_t)(u[2] & 0x3F) << 6 | (u[3] & 0x3F);
        return 4;
    }
    *c = u[0];
    return 1;
}

// Letters with upper and lower case outside ASCII (Latin, Greek, Cyrillic,
// Armenian, Georgian, full-width ...): a case-insensitive rule can match them
// in either case, so they don't make a literal.
static int cased(uint32_t c) {
    return (c >= 0xC0 && c <= 0x24F) || (c >= 0x370 && c <= 0x58F) || (c >= 0x10A0 && c <= 0x10FF) ||
           (c >= 0x1E00 && c <= 0x1FFF) || (c >= 0x2C00 && c <= 0x2D2F) || (c >= 0xA640 && c <= 0xA69F) ||
           (c >= 0xA720 && c <= 0xA7FF) || (c >= 0xFF21 && c <= 0xFF5A) || c == 0x212A || c == 0x17F;
}

// Which groups every match goes through as written: not optional, no
// alternatives of their own, not a negative look-around. Their literal text
// counts; anything inside another group doesn't. sure[i] for the '(' at i.
static void sure_groups(const char *p, size_t len, uint8_t *sure) {
    size_t *stack = malloc((len + 1) * sizeof *stack);
    uint8_t *alt = calloc(len + 1, 1);
    int top = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\\') { i++; continue; }
        if (p[i] == '[') {  // a class: skip it whole
            size_t j = i + 1;
            if (j < len && p[j] == '^') j++;
            if (j < len && p[j] == ']') j++;
            while (j < len && p[j] != ']') {
                if (p[j] == '\\') j += 2;
                else if (p[j] == '[' && p[j + 1] == ':') {
                    const char *e = strstr(p + j + 2, ":]");
                    j = e ? (size_t)(e - p) + 2 : j + 1;
                } else j++;
            }
            i = j;
            continue;
        }
        if (p[i] == '(') { stack[top++] = i; continue; }
        if (p[i] == '|' && top) { alt[stack[top - 1]] = 1; continue; }
        if (p[i] == ')' && top) {
            size_t o = stack[--top];
            char q = i + 1 < len ? p[i + 1] : 0;
            int optional = q == '?' || q == '*' || (q == '{' && (p[i + 2] == '0' || p[i + 2] == ','));
            int negative = !strncmp(p + o, "(?!", 3) || !strncmp(p + o, "(?<!", 4);
            int other = p[o + 1] == '?' && p[o + 2] == '(';  // a conditional
            sure[o] = !alt[o] && !optional && !negative && !other;
        }
    }
    free(stack);
    free(alt);
}

char *vx_rule_literal(const char *p, int icase) {
    if (!p) return NULL;
    if (strstr(p, "(?i")) icase = 1;
    Run cur = {0}, best = {0};
    int depth = 0, alt = 0;
    size_t i = 0, len = strlen(p);
    uint8_t *sure = calloc(len + 1, 1);
    sure_groups(p, len, sure);
    int unsure = 0;           // open groups that aren't sure
    uint8_t gstack[256];      // whether each open group is sure
    int gtop = 0;
#define BREAK() do { if (cur.n > best.n) { best.n = 0; run_put(&best, cur.b, cur.n); } cur.n = 0; } while (0)
    // A quantifier after a literal character: optional ones take the
    // character back; any of them ends the run (what follows needn't be next).
#define QUANT() do { \
        if (i < len && (p[i] == '?' || p[i] == '*')) { if (cur.n) cur.n = cur.last; BREAK(); i++; } \
        else if (i < len && p[i] == '+') { BREAK(); i++; } \
        else if (i < len && p[i] == '{') { \
            if (p[i + 1] == '0' || p[i + 1] == ',') { if (cur.n) cur.n = cur.last; } \
            BREAK(); \
            while (i < len && p[i] != '}') i++; \
            if (i < len) i++; \
        } \
        while (i < len && (p[i] == '?' || p[i] == '+')) i++; /* lazy, possessive */ \
    } while (0)
    while (i < len) {
        char c = p[i];
        if (c == '\\' && i + 1 < len) {
            char n = p[i + 1];
            uint32_t cp = 0;
            int lit = 0;
            if (n == 'x') {
                size_t j = i + 2;
                if (p[j] == '{') {
                    j++;
                    while (j < len && p[j] != '}') {
                        char h = p[j++];
                        cp = cp * 16 + (uint32_t)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    }
                    j++;
                } else {
                    for (int k = 0; k < 2 && j < len; k++) {
                        char h = p[j++];
                        cp = cp * 16 + (uint32_t)(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    }
                }
                i = j;
                lit = 1;
            } else if (n == 'e') {
                cp = 0x1B, lit = 1, i += 2;
            } else if (n == 'p' || n == 'P') {
                i += 2;
                if (p[i] == '{') { while (i < len && p[i] != '}') i++; i++; } else i++;
            } else if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z') || (n >= '0' && n <= '9')) {
                i += 2;  // \b \s \d \w \n \r ... and back references: not literal text
            } else {
                cp = (unsigned char)n, lit = 1, i += 2;  // an escaped punctuation mark
            }
            if (lit && !unsure) {
                if (icase && cp < 0x80 && cp >= 'A' && cp <= 'Z') cp |= 0x20;
                if (icase && cased(cp)) BREAK();
                else { char u[4]; run_put(&cur, u, utf8_put(u, cp)); }
                QUANT();
            } else {
                BREAK();
                if (!lit) {  // a class escape may be quantified too
                    if (i < len && (p[i] == '?' || p[i] == '*' || p[i] == '+')) i++;
                    else if (i < len && p[i] == '{') { while (i < len && p[i] != '}') i++; i++; }
                }
            }
            continue;
        }
        if (c == '[') {
            size_t j = i + 1;
            if (j < len && p[j] == '^') j++;
            if (j < len && p[j] == ']') j++;
            while (j < len && p[j] != ']') {
                if (p[j] == '\\') j += 2;
                else if (p[j] == '[' && p[j + 1] == ':') {
                    const char *e = strstr(p + j + 2, ":]");
                    j = e ? (size_t)(e - p) + 2 : j + 1;
                } else j++;
            }
            i = j + 1;
            BREAK();
            if (i < len && (p[i] == '?' || p[i] == '*' || p[i] == '+')) i++;
            else if (i < len && p[i] == '{') { while (i < len && p[i] != '}') i++; i++; }
            continue;
        }
        if (c == '(') {
            depth++;
            if (gtop < 256) gstack[gtop++] = sure[i];
            if (!sure[i]) unsure++;
            BREAK();
            i++;
            // skip the group's kind: (?: (?= (?<= (?<name> (?i) ...
            if (i < len && p[i] == '?') {
                i++;
                if (i < len && p[i] == '<' && (p[i + 1] == '=' || p[i + 1] == '!')) i += 2;
                else if (i < len && (p[i] == '<' || p[i] == '\'' || p[i] == 'P')) { while (i < len && p[i] != '>' && p[i] != '\'') i++; i++; }
                else while (i < len && p[i] != ':' && p[i] != ')' && p[i] != '=' && p[i] != '!' && p[i] != '>') i++, (void)0;
                if (i < len && (p[i] == ':' || p[i] == '=' || p[i] == '!' || p[i] == '>')) i++;
            }
            continue;
        }
        if (c == ')') {
            if (depth) depth--;
            if (gtop && !gstack[--gtop]) unsure--;
            BREAK();
            i++;
            if (i < len && (p[i] == '?' || p[i] == '*' || p[i] == '+')) i++;
            else if (i < len && p[i] == '{') { while (i < len && p[i] != '}') i++; i++; }
            continue;
        }
        if (c == '|') { if (!depth) alt = 1; BREAK(); i++; continue; }
        if (c == '.' || c == '^' || c == '$' || c == '?' || c == '*' || c == '+' || c == '{') {
            BREAK();
            if (c == '{') { while (i < len && p[i] != '}') i++; }
            i++;
            continue;
        }
        uint32_t cp;
        size_t k = utf8_get(p + i, &cp);
        if (unsure) { BREAK(); i += k; continue; }
        if (icase && cp >= 'A' && cp <= 'Z') cp |= 0x20;
        i += k;
        if (icase && cased(cp)) BREAK();
        else { char u[4]; run_put(&cur, u, utf8_put(u, cp)); }
        QUANT();
    }
    BREAK();
#undef BREAK
#undef QUANT
    free(sure);
    free(cur.b);
    if (alt || !best.n) { free(best.b); return NULL; }
    best.b[best.n] = 0;
    return best.b;
}

// ---- the index

typedef struct {
    uint32_t rule;
    uint32_t len;
    char *lit;
} Entry;

typedef struct {
    Entry *e;
    size_t n, cap;
} Bucket;

#define NBUCKETS 4096

struct VxRuleIndex {
    Bucket two[2][NBUCKETS];  // [icase] literals of 2+ bytes, by their first two
    Bucket one[2][256];       // [icase] one-byte literals
    uint32_t *always;         // rules without a literal
    size_t nalways, capalways;
};

static unsigned key2(unsigned char a, unsigned char b) { return (a * 131u + b) & (NBUCKETS - 1); }

VxRuleIndex *vx_rule_index_new(void) { return calloc(1, sizeof(VxRuleIndex)); }

void vx_rule_index_free(VxRuleIndex *x) {
    if (!x) return;
    for (int c = 0; c < 2; c++) {
        for (int b = 0; b < NBUCKETS; b++) {
            for (size_t i = 0; i < x->two[c][b].n; i++) free(x->two[c][b].e[i].lit);
            free(x->two[c][b].e);
        }
        for (int b = 0; b < 256; b++) {
            for (size_t i = 0; i < x->one[c][b].n; i++) free(x->one[c][b].e[i].lit);
            free(x->one[c][b].e);
        }
    }
    free(x->always);
    free(x);
}

void vx_rule_index_add(VxRuleIndex *x, uint32_t rule, const char *pattern, int icase) {
    if (pattern && strstr(pattern, "(?i")) icase = 1;
    char *lit = vx_rule_literal(pattern, icase);
    if (!lit) {
        if (x->nalways == x->capalways) {
            x->capalways = x->capalways ? x->capalways * 2 : 16;
            x->always = realloc(x->always, x->capalways * sizeof *x->always);
        }
        x->always[x->nalways++] = rule;
        return;
    }
    size_t n = strlen(lit);
    unsigned char *u = (unsigned char *)lit;
    Bucket *b = n >= 2 ? &x->two[icase != 0][key2(u[0], u[1])] : &x->one[icase != 0][u[0]];
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->e = realloc(b->e, b->cap * sizeof *b->e);
    }
    b->e[b->n++] = (Entry){rule, (uint32_t)n, lit};
}

void vx_rule_index_mark(const VxRuleIndex *x, const char *text, uint8_t *marks, size_t nrules) {
    if (!x || !text) return;
    for (size_t i = 0; i < x->nalways; i++)
        if (x->always[i] < nrules) marks[x->always[i]] = 1;
    size_t len = strlen(text);
    char *lower = malloc(len + 1);
    for (size_t i = 0; i <= len; i++) {
        char c = text[i];
        lower[i] = c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c;
    }
    for (int ic = 0; ic < 2; ic++) {
        const unsigned char *t = (const unsigned char *)(ic ? lower : text);
        for (size_t p = 0; p < len; p++) {
            const Bucket *one = &x->one[ic][t[p]];
            for (size_t i = 0; i < one->n; i++)
                if (one->e[i].rule < nrules) marks[one->e[i].rule] = 1;
            if (p + 1 >= len) continue;
            const Bucket *b = &x->two[ic][key2(t[p], t[p + 1])];
            for (size_t i = 0; i < b->n; i++) {
                const Entry *e = &b->e[i];
                if (e->rule < nrules && !marks[e->rule] && e->len <= len - p && !memcmp(t + p, e->lit, e->len))
                    marks[e->rule] = 1;
            }
        }
    }
    free(lower);
}
