#include "cv_ssml.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cv_rules.h"
#include "vxphonemes.h"
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

// The token stream of an utterance, assembled the way SSMLParser.assemble
// does. T_SPELL's value is 1 for on, 0 for off; T_MARK's the bookmark's id;
// T_PHON's text is the IPA, alt the element's own text (said when the voice
// can't say the IPA).
typedef enum { T_TEXT, T_PAUSE, T_EOS, T_PITCH, T_RATE, T_VOL, T_SPELL, T_MARK, T_PHON } TokKind;
typedef struct { TokKind kind; int value; char *text, *alt; } Tok;
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
    t->text = t->alt = NULL;
    if (text) { t->text = malloc(len + 1); memcpy(t->text, text, len); t->text[len] = 0; }
}

static int speaks(const Tok *t);

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int blank(const char *s) {
    for (; *s; s++) if (!isspace((unsigned char)*s)) return 0;
    return 1;
}

typedef struct {
    const CvRules *rules;
    CvPhonemeFn phoneme;
    void *ctx;
} Opts;

// A token that is heard: text with something in it, or phonemes.
static int speaks(const Tok *t) {
    return (t->kind == T_TEXT && !blank(t->text)) || t->kind == T_PHON;
}

// The utterance from its tokens: one segment, its text with ESC commands.
static void finish(Seg *s, const Opts *o, CvUtterance *out) {
    ssize_t first = -1, last = -1;
    for (size_t i = 0; i < s->n; i++)
        if (speaks(&s->t[i])) { if (first < 0) first = (ssize_t)i; last = (ssize_t)i; }
    Buf b = {0};
    puts_(&b, "");
    int pause = 0, start_pitch = s->pitch;
    int seen_pitch = 0, spell = 0;
    for (size_t i = 0; i < s->n; i++) {
        Tok *t = &s->t[i];
        char esc[48];
        if (t->kind == T_MARK) {  // wherever it is: it is reached where it is written
            snprintf(esc, sizeof esc, "\x1b\\mrk=%d\\", t->value);
            puts_(&b, esc);
            continue;
        }
        if (last >= 0 && (ssize_t)i > last) {
            if (t->kind == T_PAUSE) pause += t->value;
            continue;
        }
        if (first < 0 || (ssize_t)i < first) {
            // leading pauses add latency; a leading pitch sets the start pitch;
            // rate and volume before the first text are the segment's own
            if (t->kind == T_PITCH && !seen_pitch) { start_pitch = t->value; seen_pitch = 1; continue; }
            if (first < 0 && t->kind == T_PAUSE) pause += t->value;
            if (t->kind != T_PITCH && t->kind != T_SPELL) continue;
        }
        switch (t->kind) {
        case T_TEXT: {
            for (char *p = t->text; *p; p++) if (*p == 0x1b) *p = ' ';
            char *r = cv_rules_apply(spell ? NULL : o->rules, t->text);  // spelled text is read as it is
            puts_(&b, r);
            free(r);
            break;
        }
        case T_PHON: {
            // joined: right after a word, nothing between them
            int joined = b.n && !isspace((unsigned char)b.s[b.n - 1]) && b.s[b.n - 1] != '\\';
            char *ph = o->phoneme ? o->phoneme(o->ctx, t->text, joined) : NULL;
            if (ph) puts_(&b, ph);
            else if (t->alt) {
                char *r = cv_rules_apply(spell ? NULL : o->rules, t->alt);
                puts_(&b, r);
                free(r);
            }
            free(ph);
            break;
        }
        case T_PAUSE: snprintf(esc, sizeof esc, " \x1b\\pause=%d\\ ", t->value); puts_(&b, esc); break;
        case T_EOS: puts_(&b, " \x1b\\eos=1\\ "); break;
        case T_PITCH: snprintf(esc, sizeof esc, "\x1b\\pitch=%d\\", vxs_engine_pitch(t->value)); puts_(&b, esc); break;
        case T_RATE: snprintf(esc, sizeof esc, "\x1b\\rate=%d\\", clampi(t->value, 50, 400)); puts_(&b, esc); break;
        case T_VOL: snprintf(esc, sizeof esc, "\x1b\\vol=%d\\", clampi(t->value < 0 ? 80 : t->value, 0, 100)); puts_(&b, esc); break;
        case T_SPELL:
            if (t->value != spell) puts_(&b, t->value ? "\x1b\\tn=spell\\" : "\x1b\\tn=normal\\");
            spell = t->value;
            break;
        case T_MARK: break;
        }
    }
    // trim
    size_t a = 0, z = b.n;
    while (a < z && isspace((unsigned char)b.s[a])) a++;
    while (z > a && isspace((unsigned char)b.s[z - 1])) z--;
    if (z > a || pause) {
        out->seg = realloc(out->seg, (out->n + 1) * sizeof *out->seg);
        CvSegment *g = &out->seg[out->n++];
        g->text = malloc(z - a + 1);
        memcpy(g->text, b.s + a, z - a);
        g->text[z - a] = 0;
        g->rate = s->rate;
        g->pitch = start_pitch;
        g->volume = s->volume;
        g->trailing_pause_ms = pause > 5000 ? 5000 : pause;
    }
    free(b.s);
    for (size_t i = 0; i < s->n; i++) { free(s->t[i].text); free(s->t[i].alt); }
    s->n = 0;
    s->captured = 0;
}

// ---------------------------------------------------------------- prosody

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

char *cv_voice_phonemes(void *voice, const char *ipa, int joined) {
    const CvPhonemeVoice *v = voice;
    return vx_phoneme_text(v->voices_root, v->asset_root, v->voice_key, v->language, ipa, joined);
}

void cv_parse_ssml(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out) {
    cv_parse_ssml_ex(in, base_rate, base_pitch, base_volume, NULL, NULL, NULL, out);
}

void cv_parse_ssml_rules(const char *in, int base_rate, int base_pitch, int base_volume, const CvRules *rules,
                         CvUtterance *out) {
    cv_parse_ssml_ex(in, base_rate, base_pitch, base_volume, rules, NULL, NULL, out);
}

static void clamp_segments(CvUtterance *out) {
    for (size_t i = 0; i < out->n; i++) {
        out->seg[i].rate = clampi(out->seg[i].rate, 50, 400);
        out->seg[i].pitch = clampi(out->seg[i].pitch, 50, 200);
        if (out->seg[i].volume > 100) out->seg[i].volume = 100;
    }
}

// What is in effect while walking the markup, and what the tokens have said so far.
typedef struct {
    int rate, pitch, volume;
} Prosody;
typedef struct {
    Prosody cur, emitted;
    int spell[32], sdepth, emitted_spell;  // say-as: whether each open one spells
} Walk;

// Before something heard: the changes since the last thing heard.
static void heard(Seg *s, Walk *w) {
    if (!s->captured) {  // the segment starts with the prosody around its first text
        s->rate = w->cur.rate;
        s->volume = w->cur.volume;
        s->captured = 1;
        w->emitted.rate = w->cur.rate;
        w->emitted.volume = w->cur.volume;
    }
    if (w->cur.pitch != w->emitted.pitch) tok(s, T_PITCH, w->emitted.pitch = w->cur.pitch, NULL, 0);
    if (w->cur.rate != w->emitted.rate) tok(s, T_RATE, w->emitted.rate = w->cur.rate, NULL, 0);
    if (w->cur.volume != w->emitted.volume) tok(s, T_VOL, w->emitted.volume = w->cur.volume, NULL, 0);
    int sp = w->sdepth && w->spell[w->sdepth - 1];
    if (sp != w->emitted_spell) tok(s, T_SPELL, w->emitted_spell = sp, NULL, 0);
}

static int spelled(const char *interpret_as) {
    return !strcasecmp(interpret_as, "characters") || !strcasecmp(interpret_as, "spell-out") ||
           !strcasecmp(interpret_as, "verbatim") || !strcasecmp(interpret_as, "letters");
}

void cv_parse_ssml_ex(const char *in, int base_rate, int base_pitch, int base_volume, const CvRules *rules,
                      CvPhonemeFn phoneme, void *ctx, CvUtterance *out) {
    memset(out, 0, sizeof *out);
    Opts o = {rules, phoneme, ctx};
    Seg s = {0};
    s.rate = base_rate;
    s.volume = base_volume;
    s.pitch = base_pitch;
    const char *p = in;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '<') {  // plain text
        tok(&s, T_TEXT, 0, in, strlen(in));
        finish(&s, &o, out);
        free(s.t);
        clamp_segments(out);
        return;
    }
    Walk w = {{base_rate, base_pitch, base_volume}, {base_rate, base_pitch, base_volume}, {0}, 0, 0};
    Prosody stack[32];
    int depth = 0;
    // inside <phoneme>: its IPA, and its own text (collected, said if the IPA can't be)
    char *ph = NULL;
    Buf alt = {0};
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
            int empty = blen && body[blen - 1] == '/';  // <x ... />
            size_t nl = 0;
            while (nl < blen && !isspace((unsigned char)body[nl]) && body[nl] != '/') nl++;
            char name[32];
            snprintf(name, sizeof name, "%.*s", (int)(nl < 31 ? nl : 31), body);
            char *colon = strchr(name, ':');  // ssml:prosody
            const char *nm = colon ? colon + 1 : name;
            char v[256];
            if (!closing && !strcmp(nm, "mark")) {
                attr(body, blen, "name", v, sizeof v);
                out->marks = realloc(out->marks, (out->nmarks + 1) * sizeof *out->marks);
                out->marks[out->nmarks] = strdup(v);
                tok(&s, T_MARK, (int)out->nmarks++, NULL, 0);
            } else if (!closing && !strcmp(nm, "break")) {
                char t[32], st[32];
                attr(body, blen, "time", t, sizeof t);
                attr(body, blen, "strength", st, sizeof st);
                int ms = break_ms(t, st[0] ? st : NULL);
                if (ms > 0) tok(&s, T_PAUSE, ms, NULL, 0);
            } else if (!strcmp(nm, "prosody")) {
                if (closing) { if (depth) w.cur = stack[--depth]; }
                else if (!empty) {
                    if (depth < 32) stack[depth++] = w.cur;
                    static const char *rkw[] = {"x-slow", "slow", "medium", "default", "fast", "x-fast", NULL};
                    static const int rkv[] = {50, 75, 100, 100, 150, 200};
                    static const char *vkw[] = {"silent", "x-soft", "soft", "medium", "default", "loud", "x-loud", NULL};
                    static const int vkv[] = {0, 20, 40, 60, 80, 90, 100};
                    if (attr(body, blen, "rate", v, sizeof v)) w.cur.rate = scale(v, w.cur.rate, rkw, rkv);
                    if (attr(body, blen, "pitch", v, sizeof v)) w.cur.pitch = pitch_scale(v, w.cur.pitch);
                    if (attr(body, blen, "volume", v, sizeof v) && !strstr(v, "dB"))
                        w.cur.volume = scale(v, w.cur.volume < 0 ? 80 : w.cur.volume, vkw, vkv);
                }
            } else if (!strcmp(nm, "say-as")) {
                if (closing) { if (w.sdepth) w.sdepth--; }
                else if (!empty && w.sdepth < 32) {
                    attr(body, blen, "interpret-as", v, sizeof v);
                    w.spell[w.sdepth++] = spelled(v);
                }
            } else if (!strcmp(nm, "phoneme")) {
                if (closing && ph) {
                    heard(&s, &w);
                    tok(&s, T_PHON, 0, ph, strlen(ph));
                    s.t[s.n - 1].alt = alt.s ? strdup(alt.s) : NULL;
                    free(ph);
                    ph = NULL;
                    alt.n = 0;
                    if (alt.s) alt.s[0] = 0;
                } else if (!closing) {
                    char a[32];
                    attr(body, blen, "alphabet", a, sizeof a);
                    if (attr(body, blen, "ph", v, sizeof v) && (!a[0] || !strcasecmp(a, "ipa"))) {
                        size_t dn = decode(v, strlen(v), tmp);
                        tmp[dn] = 0;
                        free(ph);
                        ph = strdup(tmp);
                        if (empty) {  // no text of its own
                            heard(&s, &w);
                            tok(&s, T_PHON, 0, ph, strlen(ph));
                            free(ph);
                            ph = NULL;
                        }
                    }
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
        p += n;
        if (ph) {  // a phoneme element's own text
            put(&alt, tmp, dn);
            continue;
        }
        if (!blank(tmp)) heard(&s, &w);
        tok(&s, T_TEXT, 0, tmp, dn);
    }
    finish(&s, &o, out);
    free(tmp);
    free(ph);
    free(alt.s);
    free(s.t);
    clamp_segments(out);
}

void cv_parse_plain(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out) {
    memset(out, 0, sizeof *out);
    Opts o = {0};
    Seg s = {0};
    s.rate = base_rate;
    s.volume = base_volume;
    s.pitch = base_pitch;
    tok(&s, T_TEXT, 0, in, strlen(in));
    finish(&s, &o, out);
    free(s.t);
    clamp_segments(out);
}

void cv_free_utterance(CvUtterance *u) {
    for (size_t i = 0; i < u->n; i++) free(u->seg[i].text);
    for (size_t i = 0; i < u->nmarks; i++) free(u->marks[i]);
    free(u->seg);
    free(u->marks);
    memset(u, 0, sizeof *u);
}
