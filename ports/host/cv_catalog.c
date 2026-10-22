// Voice discovery for the Linux and Android hosts; follows the NVDA add-on's
// voices.py and the iOS app's VoiceCatalog (compact fallbacks, fixed pitch).
#include "cv_catalog.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static const struct { const char *build, *label; } kIos[] = {
    {"ios3", "iOS 3"}, {"ios4", "iOS 4"}, {"ios5", "iOS 5"}, {"ios6", "iOS 6"}};
static const struct { const char *era, *label, *compact_engine, *premium_engine; } kMac[] = {
    {"mac107", "Lion", "ios6", "ios6"},
    {"mac108", "Mountain Lion", "ios6", "ios6"},
    {"mac109", "Mavericks", "ios7", "ios10"}};

// Female voices among Apple's Vocalizer and Mac voices (for Android's gender
// hint and speech-dispatcher's voice types); everything else counts as male.
static const char *kFemale[] = {
    "agata", "alice", "alva", "amelie", "angelica", "anna", "audrey", "aurelie", "carmit", "catarina",
    "chantal", "claire", "damayanti", "ellen", "empar", "ewa", "federica", "ioana", "iveta", "joana",
    "kanya", "karen", "kate", "katya", "kyoko", "laila", "lekha", "luciana", "mariska", "mei-jia",
    "melina", "milena", "moira", "monica", "nora", "paola", "paulina", "petra", "samantha", "sara",
    "satu", "serena", "sin-ji", "tessa", "ting-ting", "veena", "victoria", "yelda", "yuna", "zosia",
    "zuzana", "silvia", "susan", "allison", "ava", "fiona", "kathy", "princess", "vicki", "agnes",
    "sinji", "tingting", "meijia", "yuri", "narisa", "lana", "soledad", "isabela", "sora", NULL};

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }

static int has_engine(const char *voices_root, const char *build) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s/voiced", voices_root, build);
    return is_file(p);
}

// The voice name from a broker.hdr / broker.hdr.asset: "defaultvoice:Name"
// (iOS 3/4) or <voice>Name</voice>. Also the Nuance langcode when present.
static int broker_info(const char *path, char *name, size_t ncap, char *langcode, size_t lcap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    name[0] = 0;
    if (langcode) langcode[0] = 0;
    if (strncmp(buf, "defaultvoice:", 13) == 0) {
        const char *s = buf + 13, *e = strchr(s, '\n');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        while (len && isspace((unsigned char)s[len - 1])) len--;
        while (len && isspace((unsigned char)*s)) { s++; len--; }
        snprintf(name, ncap, "%.*s", (int)len, s);
    } else {
        const char *s = strstr(buf, "<voice>");
        if (s) {
            s += 7;
            const char *e = strchr(s, '<');
            if (e) snprintf(name, ncap, "%.*s", (int)(e - s), s);
        }
    }
    if (langcode) {
        const char *s = strstr(buf, "<langcode>");
        if (s) {
            s += 10;
            const char *e = strchr(s, '<');
            if (e) snprintf(langcode, lcap, "%.*s", (int)(e - s), s);
        }
    }
    if (name[0]) name[0] = (char)toupper((unsigned char)name[0]);
    return name[0] != 0;
}

// Nuance language codes as Apple's Mac voices use them, for folders that have
// no `locale` file (data shared with the NVDA add-on).
static const char *langcode_locale(const char *lc) {
    static const char *map[][2] = {
        {"ENU", "en-US"}, {"ENG", "en-GB"}, {"ENA", "en-AU"}, {"ENI", "en-IN"}, {"ENZ", "en-ZA"}, {"ENE", "en-IE"},
        {"ENS", "en-GB"}, {"FRF", "fr-FR"}, {"FRC", "fr-CA"}, {"GED", "de-DE"}, {"ITI", "it-IT"}, {"SPE", "es-ES"},
        {"SPM", "es-MX"}, {"PTP", "pt-PT"}, {"PTB", "pt-BR"}, {"DUN", "nl-NL"}, {"DUB", "nl-BE"}, {"SWS", "sv-SE"},
        {"NON", "nb-NO"}, {"DAD", "da-DK"}, {"FIF", "fi-FI"}, {"PLP", "pl-PL"}, {"CZC", "cs-CZ"}, {"SKS", "sk-SK"},
        {"HUH", "hu-HU"}, {"RUR", "ru-RU"}, {"TRT", "tr-TR"}, {"GRG", "el-GR"}, {"ARW", "ar-SA"}, {"HEI", "he-IL"},
        {"JPJ", "ja-JP"}, {"KOK", "ko-KR"}, {"MNC", "zh-CN"}, {"MNT", "zh-TW"}, {"CAH", "zh-HK"}, {"THT", "th-TH"},
        {"IDI", "id-ID"}, {"ROR", "ro-RO"}, {"HII", "hi-IN"}, {"CAE", "ca-ES"}, {"SPC", "es-AR"}, {NULL, NULL}};
    for (int i = 0; map[i][0]; i++)
        if (strcmp(map[i][0], lc) == 0) return map[i][1];
    return "";
}

static int female_name(const char *voice) {
    char lower[64];
    size_t i = 0;
    for (; voice[i] && i < sizeof lower - 1; i++) lower[i] = (char)tolower((unsigned char)voice[i]);
    lower[i] = 0;
    for (int k = 0; kFemale[k]; k++)
        if (strcmp(lower, kFemale[k]) == 0) return 1;
    return 0;
}

typedef struct { CvVoice *v; size_t n, cap; } List;

static CvVoice *push(List *l) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = realloc(l->v, l->cap * sizeof *l->v);
    }
    CvVoice *v = &l->v[l->n++];
    memset(v, 0, sizeof *v);
    v->female = -1;
    return v;
}

static int cmp_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

// Sorted subdirectory names of dir (malloc'd array of malloc'd strings).
static size_t list_dirs(const char *dir, char ***out) {
    *out = NULL;
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!is_dir(p)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; *out = realloc(*out, cap * sizeof **out); }
        (*out)[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n) qsort(*out, n, sizeof **out, cmp_name);
    return n;
}

static void free_dirs(char **d, size_t n) {
    for (size_t i = 0; i < n; i++) free(d[i]);
    free(d);
}

static void read_small(const char *path, char *out, size_t cap) {
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && isspace((unsigned char)out[n - 1])) out[--n] = 0;
}

size_t cv_scan_voices(const char *voices_root, const char *downloads_root, CvVoice **out) {
    List l = {0};
    char p[1200];
    // firmware voices
    for (size_t b = 0; b < sizeof kIos / sizeof kIos[0]; b++) {
        if (!has_engine(voices_root, kIos[b].build)) continue;
        snprintf(p, sizeof p, "%s/%s/TTSResources", voices_root, kIos[b].build);
        char **langs;
        size_t nl = list_dirs(p, &langs);
        for (size_t i = 0; i < nl; i++) {
            if (strcmp(langs[i], "common") == 0) continue;
            char name[64], hdr[1300];
            snprintf(hdr, sizeof hdr, "%s/%s/broker.hdr", p, langs[i]);
            if (!broker_info(hdr, name, sizeof name, NULL, 0)) continue;
            CvVoice *v = push(&l);
            snprintf(v->key, sizeof v->key, "%s/%s", kIos[b].build, langs[i]);
            snprintf(v->name, sizeof v->name, "%s (%s)", name, kIos[b].label);
            snprintf(v->voice, sizeof v->voice, "%s", name);
            snprintf(v->language, sizeof v->language, "%s", langs[i]);
            v->quality = 1;
            v->female = female_name(name);
        }
        free_dirs(langs, nl);
    }
    // enhanced iOS 5 / 6 voices
    for (size_t b = 2; b < 4; b++) {
        if (!has_engine(voices_root, kIos[b].build) || !downloads_root) continue;
        snprintf(p, sizeof p, "%s/%s", downloads_root, kIos[b].build);
        char **langs;
        size_t nl = list_dirs(p, &langs);
        for (size_t i = 0; i < nl; i++) {
            char name[64], hdr[1300];
            snprintf(hdr, sizeof hdr, "%s/%s/broker.hdr.asset", p, langs[i]);
            if (!broker_info(hdr, name, sizeof name, NULL, 0)) continue;
            CvVoice *v = push(&l);
            snprintf(v->key, sizeof v->key, "%se/%s", kIos[b].build, langs[i]);
            snprintf(v->name, sizeof v->name, "%s (%s, enhanced)", name, kIos[b].label);
            snprintf(v->voice, sizeof v->voice, "%s", name);
            snprintf(v->language, sizeof v->language, "%s", langs[i]);
            v->quality = 2;
            v->female = female_name(name);
            // enhanced voices can't change pitch: their compact voice speaks pitched text
            char compact[1300];
            snprintf(compact, sizeof compact, "%s/%s/TTSResources/%s/broker.hdr", voices_root, kIos[b].build, langs[i]);
            if (is_file(compact)) snprintf(v->fallback, sizeof v->fallback, "%s/%s", kIos[b].build, langs[i]);
        }
        free_dirs(langs, nl);
    }
    // Mac OS X voices
    for (size_t m = 0; m < sizeof kMac / sizeof kMac[0] && downloads_root; m++) {
        snprintf(p, sizeof p, "%s/%s/common", voices_root, kMac[m].era);
        if (!is_dir(p)) continue;
        snprintf(p, sizeof p, "%s/%s", downloads_root, kMac[m].era);
        char **dirs;
        size_t nd = list_dirs(p, &dirs);
        for (size_t i = 0; i < nd; i++) {
            size_t len = strlen(dirs[i]);
            int compact = len > 7 && strcmp(dirs[i] + len - 7, "Compact") == 0;
            const char *engine = compact ? kMac[m].compact_engine : kMac[m].premium_engine;
            if (!has_engine(voices_root, engine)) continue;
            char name[64], lc[16], hdr[1300], loc[32];
            snprintf(hdr, sizeof hdr, "%s/%s/broker.hdr", p, dirs[i]);
            if (!is_file(hdr)) continue;
            if (!broker_info(hdr, name, sizeof name, lc, sizeof lc))
                snprintf(name, sizeof name, "%.*s", (int)(compact ? len - 7 : len), dirs[i]);
            snprintf(hdr, sizeof hdr, "%s/%s/locale", p, dirs[i]);
            read_small(hdr, loc, sizeof loc);
            if (!loc[0]) snprintf(loc, sizeof loc, "%s", langcode_locale(lc));
            CvVoice *v = push(&l);
            snprintf(v->key, sizeof v->key, "%s/%s", kMac[m].era, dirs[i]);
            snprintf(v->name, sizeof v->name, "%s (Mac OS X %s, %s)", name, kMac[m].label, compact ? "compact" : "premium");
            snprintf(v->voice, sizeof v->voice, "%s", name);
            snprintf(v->language, sizeof v->language, "%s", loc);
            v->quality = compact ? 1 : 2;
            v->female = female_name(name);
            // Lion and Mountain Lion premium voices ignore pitch; Mavericks
            // premium voices (the iOS 10 engine) change it themselves.
            if (!compact && strcmp(kMac[m].era, "mac109") != 0) {
                char c[1300];
                snprintf(c, sizeof c, "%s/%sCompact/broker.hdr", p, dirs[i]);
                if (is_file(c)) snprintf(v->fallback, sizeof v->fallback, "%s/%sCompact", kMac[m].era, dirs[i]);
            }
        }
        free_dirs(dirs, nd);
    }
    *out = l.v;
    return l.n;
}

void cv_free_voices(CvVoice *v) { free(v); }

// "pt_BR" / "PT-br" -> "pt-BR"
static void norm_lang(const char *in, char *out, size_t cap) {
    size_t i = 0;
    int region = 0;
    for (; in[i] && i < cap - 1; i++) {
        char c = in[i];
        if (c == '_' || c == '-') { out[i] = '-'; region = 1; continue; }
        out[i] = region ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
    }
    out[i] = 0;
}

static int rank(const CvVoice *v) {
    // iOS 6 compact first (VoiceOver's own), then enhanced, then the rest
    if (strncmp(v->key, "ios6/", 5) == 0) return 0;
    if (strncmp(v->key, "ios6e/", 6) == 0) return 1;
    if (strncmp(v->key, "ios5", 4) == 0) return 2;
    return 3;
}

const CvVoice *cv_pick_voice(const CvVoice *v, size_t n, const char *language) {
    if (!language || !*language || !n) return n ? &v[0] : NULL;
    char want[24];
    norm_lang(language, want, sizeof want);
    size_t wl = strcspn(want, "-");
    const CvVoice *best = NULL;
    int best_score = 1 << 30;
    for (size_t i = 0; i < n; i++) {
        char have[24];
        norm_lang(v[i].language, have, sizeof have);
        int score;
        if (strcmp(have, want) == 0) score = 0;
        else if (strncmp(have, want, wl) == 0 && (have[wl] == '-' || have[wl] == 0)) score = 10;
        else continue;
        // "en" alone: prefer en-US
        if (!want[wl] && strcmp(have, "en-US") == 0) score -= 5;
        score += rank(&v[i]);
        if (score < best_score) { best_score = score; best = &v[i]; }
    }
    return best;
}

const CvVoice *cv_find_voice(const CvVoice *v, size_t n, const char *name_or_key) {
    if (!name_or_key) return NULL;
    for (size_t i = 0; i < n; i++)
        if (strcmp(v[i].name, name_or_key) == 0 || strcmp(v[i].key, name_or_key) == 0) return &v[i];
    for (size_t i = 0; i < n; i++)
        if (strcasecmp(v[i].voice, name_or_key) == 0) return &v[i];
    return NULL;
}

const char *cv_voice_build(const CvVoice *v) {
    static const char *builds[] = {"ios3", "ios4", "ios5", "ios6", "ios7", "ios10"};
    const char *k = v->key;
    if (!strncmp(k, "mac109/", 7)) {
        size_t n = strlen(k);
        return n > 7 && !strcmp(k + n - 7, "Compact") ? "ios7" : "ios10";
    }
    if (!strncmp(k, "mac10", 5)) return "ios6";
    for (size_t i = 0; i < sizeof builds / sizeof builds[0]; i++) {
        size_t n = strlen(builds[i]);
        if (!strncmp(k, builds[i], n) && (k[n] == '/' || (k[n] == 'e' && k[n + 1] == '/'))) return builds[i];
    }
    return "ios6";
}
