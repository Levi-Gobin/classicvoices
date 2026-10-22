#include "cv_ssml.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cv_rules.h"
#include "vxstream.h"

typedef struct { char *s; size_t n, cap; } Buf;

static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

// One token stream per segment, assembled the way SSMLParser.assemble does.
typedef enum { T_TEXT, T_PAUSE, T_EOS, T_PITCH } TokKind;
typedef struct { TokKind kind; int value; char *text; } Tok;
typedef struct {
    Tok *t;
    size_t n, cap;
    int rate, volume, pitch, captured;
} Seg;

static void tok(Seg *s, TokKind k, int value, const char *text, size_t len) {
    if (k == T_TEXT && s->n && s->t[s->n - 1].kind == T_TEXT) {  // merge adjacent text
        Tok *last = &s->t[s->n - 1];
        size_t old = strlen(last->text);
        last->text = realloc(last->text, old + len + 1);
        memcpy(last->text + old, text, len);
        last->text[old + len] = 0;
        return;
    }
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->t = realloc(s->t, s->cap * sizeof *s->t); }
    Tok *t = &s->t[s->n++];
    t->kind = k;
    t->value = value;
    t->text = NULL;
    if (k == T_TEXT) { t->text = malloc(len + 1); memcpy(t->text, text, len); t->text[len] = 0; }
}

static int blank(const char *s) {
    for (; *s; s++) if (!isspace((unsigned char)*s)) return 0;
    return 1;
}

static void finish_rules(Seg *s, const char *mark, const CvRules *rules, CvUtterance *out);

static void finish(Seg *s, const char *mark, CvUtterance *out) { finish_rules(s, mark, NULL, out); }

static void finish_rules(Seg *s, const char *mark, const CvRules *rules, CvUtterance *out) {
    ssize_t first = -1, last = -1;
    for (size_t i = 0; i < s->n; i++)
        if (s->t[i].kind == T_TEXT && !blank(s->t[i].text)) { if (first < 0) first = (ssize_t)i; last = (ssize_t)i; }
    Buf b = {0};
    puts_(&b, "");
    int pause = 0, start_pitch = s->pitch;
    int seen_pitch = 0;
    for (size_t i = 0; i < s->n; i++) {
        Tok *t = &s->t[i];
        if (last >= 0 && (ssize_t)i > last) {
            if (t->kind == T_PAUSE) pause += t->value;
            continue;
        }
        if (first < 0 || (ssize_t)i < first) {
            // leading pauses add latency; a leading pitch sets the start pitch
            if (t->kind == T_PITCH && !seen_pitch) { start_pitch = t->value; seen_pitch = 1; continue; }
            if (first < 0 && t->kind == T_PAUSE) pause += t->value;
            if (t->kind != T_PITCH) continue;
        }
        char esc[48];
        switch (t->kind) {
        case T_TEXT: {
            for (char *p = t->text; *p; p++) if (*p == 0x1b) *p = ' ';
            char *r = cv_rules_apply(rules, t->text);
            puts_(&b, r);
            free(r);
            break;
        }
        case T_PAUSE: snprintf(esc, sizeof esc, " \x1b\\pause=%d\\ ", t->value); puts_(&b, esc); break;
        case T_EOS: puts_(&b, " \x1b\\eos=1\\ "); break;
        case T_PITCH: snprintf(esc, sizeof esc, "\x1b\\pitch=%d\\", vxs_engine_pitch(t->value)); puts_(&b, esc); break;
        }
    }
    // trim
    size_t a = 0, z = b.n;
    while (a < z && isspace((unsigned char)b.s[a])) a++;
    while (z > a && isspace((unsigned char)b.s[z - 1])) z--;
    int has_text = first >= 0;
    if (has_text || mark || pause) {
        out->seg = realloc(out->seg, (out->n + 1) * sizeof *out->seg);
        CvSegment *g = &out->seg[out->n++];
        g->text = malloc(z - a + 1);
        memcpy(g->text, b.s + a, z - a);
        g->text[z - a] = 0;
        g->mark = mark ? strdup(mark) : NULL;
        g->rate = s->rate;
        g->pitch = start_pitch;
        g->volume = s->volume;
        g->trailing_pause_ms = pause > 5000 ? 5000 : pause;
    }
    free(b.s);
    for (size_t i = 0; i < s->n; i++) free(s->t[i].text);
    s->n = 0;
    s->captured = 0;
}

// ---------------------------------------------------------------- prosody
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int scale(const char *raw, int base, const char *const *kw, const int *kv) {
    char s[32];
    size_t i = 0;
    while (*raw == ' ') raw++;
    for (; raw[i] && i < sizeof s - 1; i++) s[i] = (char)tolower((unsigned char)raw[i]);
    s[i] = 0;
    for (int k = 0; kw[k]; k++) if (strcmp(s, kw[k]) == 0) return kv[k];
    size_t n = strlen(s);
    if (n && s[n - 1] == '%') {
        double v = atof(s);
        return (int)lround((s[0] == '+' || s[0] == '-') ? base * (1 + v / 100) : v);
    }
    if (n && (isdigit((unsigned char)s[0]) || s[0] == '.')) return (int)lround(base * atof(s));
    return base;
}

// Pitch changes as fractions of an octave (see SSMLParser.pitchScale).
static int pitch_scale(const char *raw, int base) {
    static const char *kw[] = {"x-low", "low", "medium", "default", "high", "x-high", NULL};
    static const int kv[] = {60, 80, 100, 100, 120, 140};
    char s[32];
    size_t i = 0;
    while (*raw == ' ') raw++;
    for (; raw[i] && i < sizeof s - 1; i++) s[i] = (char)tolower((unsigned char)raw[i]);
    s[i] = 0;
    for (int k = 0; kw[k]; k++) if (strcmp(s, kw[k]) == 0) return base * kv[k] / 100;
    size_t n = strlen(s);
    double f = 1;
    if (n && s[n - 1] == '%') f = (s[0] == '+' || s[0] == '-') ? pow(2, atof(s) / 100) : atof(s) / 100;
    else if (n > 2 && strcmp(s + n - 2, "st") == 0) f = pow(2, atof(s) / 12);
    else if (n && s[n - 1] == 'z') return base;  // Hz values: no reference pitch to scale against
    else if (n) f = atof(s);
    if (!isfinite(f) || f <= 0) return base;
    if (f < 0.56) f = 0.56;
    if (f > 1.58) f = 1.58;
    return (int)lround(base * f);
}

static int break_ms(const char *time, const char *strength) {
    if (time && *time) {
        double v = atof(time);
        size_t n = strlen(time);
        if (n > 2 && strcasecmp(time + n - 2, "ms") == 0) return (int)v;
        if (n && (time[n - 1] == 's' || time[n - 1] == 'S')) return (int)(v * 1000);
        return (int)v;
    }
    if (!strength) return 400;
    if (!strcmp(strength, "none")) return 0;
    if (!strcmp(strength, "x-weak")) return 100;
    if (!strcmp(strength, "weak")) return 200;
    if (!strcmp(strength, "strong")) return 700;
    if (!strcmp(strength, "x-strong")) return 1000;
    return 400;
}

// ---------------------------------------------------------------- XML
// attribute value from a tag body ("prosody rate='fast' pitch=\"+10%\"")
static int attr(const char *body, size_t len, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < len; i++) {
        if ((i == 0 || isspace((unsigned char)body[i - 1])) && strncmp(body + i, name, nl) == 0) {
            size_t j = i + nl;
            while (j < len && isspace((unsigned char)body[j])) j++;
            if (j >= len || body[j] != '=') continue;
            j++;
            while (j < len && isspace((unsigned char)body[j])) j++;
            if (j >= len || (body[j] != '"' && body[j] != '\'')) continue;
            char q = body[j++];
            size_t k = j;
            while (k < len && body[k] != q) k++;
            snprintf(out, cap, "%.*s", (int)(k - j), body + j);
            return 1;
        }
    }
    out[0] = 0;
    return 0;
}

// Decode XML entities into dst (UTF-8); returns bytes written.
static size_t decode(const char *s, size_t n, char *dst) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { dst[o++] = s[i]; continue; }
        const char *semi = memchr(s + i, ';', n - i < 12 ? n - i : 12);
        if (!semi) { dst[o++] = '&'; continue; }
        size_t el = (size_t)(semi - (s + i));
        unsigned long cp = 0;
        if (el == 3 && !strncmp(s + i, "&lt", 3)) cp = '<';
        else if (el == 3 && !strncmp(s + i, "&gt", 3)) cp = '>';
        else if (el == 4 && !strncmp(s + i, "&amp", 4)) cp = '&';
        else if (el == 5 && !strncmp(s + i, "&quot", 5)) cp = '"';
        else if (el == 5 && !strncmp(s + i, "&apos", 5)) cp = '\'';
        else if (el > 2 && s[i + 1] == '#') cp = (s[i + 2] == 'x' || s[i + 2] == 'X') ? strtoul(s + i + 3, NULL, 16) : strtoul(s + i + 2, NULL, 10);
        if (!cp) { dst[o++] = '&'; continue; }
        if (cp < 0x80) dst[o++] = (char)cp;
        else if (cp < 0x800) { dst[o++] = (char)(0xC0 | cp >> 6); dst[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { dst[o++] = (char)(0xE0 | cp >> 12); dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { dst[o++] = (char)(0xF0 | cp >> 18); dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[o++] = (char)(0x80 | (cp & 0x3F)); }
        i += el;
    }
    return o;
}

void cv_parse_ssml(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out) {
    cv_parse_ssml_rules(in, base_rate, base_pitch, base_volume, NULL, out);
}

void cv_parse_ssml_rules(const char *in, int base_rate, int base_pitch, int base_volume, const CvRules *rules,
                         CvUtterance *out) {
    out->seg = NULL;
    out->n = 0;
    Seg s = {0};
    s.rate = base_rate;
    s.volume = base_volume;
    s.pitch = base_pitch;
    const char *p = in;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '<') {  // plain text
        tok(&s, T_TEXT, 0, in, strlen(in));
        finish_rules(&s, NULL, rules, out);
        free(s.t);
        return;
    }
    struct { int rate, pitch, volume; } cur = {base_rate, base_pitch, base_volume}, stack[32];
    int depth = 0, emitted = base_pitch;
    char *tmp = malloc(strlen(in) * 4 + 16);
    while (*p) {
        if (*p == '<') {
            if (!strncmp(p, "<!--", 4)) { const char *e = strstr(p, "-->"); p = e ? e + 3 : p + strlen(p); continue; }
            const char *e = strchr(p, '>');
            if (!e) break;
            const char *body = p + 1;
            size_t blen = (size_t)(e - body);
            int closing = blen && body[0] == '/';
            if (closing) { body++; blen--; }
            size_t nl = 0;
            while (nl < blen && !isspace((unsigned char)body[nl]) && body[nl] != '/') nl++;
            char name[32];
            snprintf(name, sizeof name, "%.*s", (int)(nl < 31 ? nl : 31), body);
            char *colon = strchr(name, ':');  // ssml:prosody
            const char *nm = colon ? colon + 1 : name;
            char v[64];
            if (!closing && !strcmp(nm, "mark")) {
                attr(body, blen, "name", v, sizeof v);
                finish_rules(&s, v, rules, out);
                s.rate = cur.rate; s.volume = cur.volume; s.pitch = cur.pitch;
                emitted = cur.pitch;
            } else if (!closing && !strcmp(nm, "break")) {
                char t[32], st[32];
                attr(body, blen, "time", t, sizeof t);
                attr(body, blen, "strength", st, sizeof st);
                int ms = break_ms(t, st[0] ? st : NULL);
                if (ms > 0) tok(&s, T_PAUSE, ms, NULL, 0);
            } else if (!strcmp(nm, "prosody")) {
                if (closing) { if (depth) cur = stack[--depth]; }
                else if (blen && body[blen - 1] != '/') {
                    if (depth < 32) stack[depth++] = cur;
                    static const char *rkw[] = {"x-slow", "slow", "medium", "default", "fast", "x-fast", NULL};
                    static const int rkv[] = {50, 75, 100, 100, 150, 200};
                    static const char *vkw[] = {"silent", "x-soft", "soft", "medium", "default", "loud", "x-loud", NULL};
                    static const int vkv[] = {0, 20, 40, 60, 80, 90, 100};
                    if (attr(body, blen, "rate", v, sizeof v)) cur.rate = scale(v, cur.rate, rkw, rkv);
                    if (attr(body, blen, "pitch", v, sizeof v)) cur.pitch = pitch_scale(v, cur.pitch);
                    if (attr(body, blen, "volume", v, sizeof v) && !strstr(v, "dB"))
                        cur.volume = scale(v, cur.volume < 0 ? 80 : cur.volume, vkw, vkv);
                }
            } else if (closing && (!strcmp(nm, "s") || !strcmp(nm, "p"))) {
                tok(&s, T_EOS, 0, NULL, 0);
            }
            p = e + 1;
            continue;
        }
        const char *e = strchr(p, '<');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        size_t dn = decode(p, n, tmp);
        tmp[dn] = 0;
        if (!blank(tmp)) {
            if (!s.captured) { s.rate = cur.rate; s.volume = cur.volume; s.captured = 1; }
            if (cur.pitch != emitted) { tok(&s, T_PITCH, cur.pitch, NULL, 0); emitted = cur.pitch; }
        }
        tok(&s, T_TEXT, 0, tmp, dn);
        p += n;
    }
    finish_rules(&s, NULL, rules, out);
    free(tmp);
    free(s.t);
    for (size_t i = 0; i < out->n; i++) {
        out->seg[i].rate = clampi(out->seg[i].rate, 50, 400);
        out->seg[i].pitch = clampi(out->seg[i].pitch, 50, 200);
        if (out->seg[i].volume > 100) out->seg[i].volume = 100;
    }
}

void cv_parse_plain(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out) {
    out->seg = NULL;
    out->n = 0;
    Seg s = {0};
    s.rate = base_rate;
    s.volume = base_volume;
    s.pitch = base_pitch;
    tok(&s, T_TEXT, 0, in, strlen(in));
    finish(&s, NULL, out);
    free(s.t);
    for (size_t i = 0; i < out->n; i++) {
        out->seg[i].rate = clampi(out->seg[i].rate, 50, 400);
        out->seg[i].pitch = clampi(out->seg[i].pitch, 50, 200);
        if (out->seg[i].volume > 100) out->seg[i].volume = 100;
    }
}

void cv_free_utterance(CvUtterance *u) {
    for (size_t i = 0; i < u->n; i++) { free(u->seg[i].text); free(u->seg[i].mark); }
    free(u->seg);
    u->seg = NULL;
    u->n = 0;
}
