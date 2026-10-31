// Automatic language switching: the voice for text in another language
// (see vxlang.h).
#include "vxlang.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char lang[8];    // "de"
    char region[8];  // "DE", or ""
} Tag;

// Codes the voices don't use for languages they speak.
static const char *const kAlias[][2] = {
    {"nb", "no"}, {"nn", "no"}, {"iw", "he"}, {"in", "id"}, {"yue", "zh"}, {"fil", "tl"},
};

// The region a bare language means; otherwise the voice in use decides.
static const char *const kRegion[][2] = {
    {"ar", "SA"}, {"cs", "CZ"}, {"da", "DK"}, {"de", "DE"}, {"el", "GR"}, {"en", "US"}, {"es", "ES"},
    {"fi", "FI"}, {"fr", "FR"}, {"he", "IL"}, {"hi", "IN"}, {"hu", "HU"}, {"id", "ID"}, {"it", "IT"},
    {"ja", "JP"}, {"ko", "KR"}, {"nl", "NL"}, {"no", "NO"}, {"pl", "PL"}, {"pt", "BR"}, {"ro", "RO"},
    {"ru", "RU"}, {"sk", "SK"}, {"sv", "SE"}, {"th", "TH"}, {"tr", "TR"}, {"zh", "CN"},
};

static Tag parse(const char *s) {
    Tag t = {{0}, {0}};
    if (!s) return t;
    size_t i = 0, j = 0;
    while (s[i] && s[i] != '-' && s[i] != '_' && j < sizeof t.lang - 1) t.lang[j++] = (char)tolower((unsigned char)s[i++]);
    for (size_t a = 0; a < sizeof kAlias / sizeof *kAlias; a++)
        if (!strcmp(t.lang, kAlias[a][0])) {
            strcpy(t.lang, kAlias[a][1]);
            if (!strcmp(kAlias[a][0], "yue")) strcpy(t.region, "HK");
        }
    // the region is the first 2-letter (or 3-digit) subtag; a script
    // (zh-Hant) stands for its usual region
    while (s[i]) {
        while (s[i] == '-' || s[i] == '_') i++;
        size_t start = i;
        while (s[i] && s[i] != '-' && s[i] != '_') i++;
        size_t len = i - start;
        if (len == 2 || (len == 3 && isdigit((unsigned char)s[start]))) {
            for (size_t k = 0; k < len && k < sizeof t.region - 1; k++)
                t.region[k] = (char)toupper((unsigned char)s[start + k]);
            t.region[len < sizeof t.region ? len : sizeof t.region - 1] = 0;
            break;
        }
        if (len == 4 && !t.region[0] && !strcmp(t.lang, "zh"))
            strcpy(t.region, tolower((unsigned char)s[start + 3]) == 't' ? "TW" : "CN");  // Hant / Hans
    }
    return t;
}

static const char *default_region(const char *lang) {
    for (size_t i = 0; i < sizeof kRegion / sizeof *kRegion; i++)
        if (!strcmp(lang, kRegion[i][0])) return kRegion[i][1];
    return "";
}

// When a voice's release came out, and its family: voices of the same family
// and year are nearest.
typedef struct {
    int year, family, enhanced;
} Era;

static Era era_of(const char *key) {
    static const struct { const char *build; int year, family; } kEra[] = {
        {"ios3", 2009, 0}, {"ios4", 2010, 0}, {"ios5", 2011, 0}, {"ios6", 2012, 0}, {"ios70", 2013, 0},
        {"mac107", 2011, 1}, {"mac108", 2012, 1}, {"mac109", 2013, 1},
        {"mt104", 2005, 2}, {"mt105", 2007, 2}, {"mt106", 2009, 2}, {"mt107", 2011, 2},
    };
    Era e = {2012, 0, 0};
    if (!key) return e;
    const char *slash = strchr(key, '/');
    size_t bl = slash ? (size_t)(slash - key) : strlen(key);
    int enh = 0;
    if (bl > 4 && !strncmp(key, "ios", 3) && key[bl - 1] == 'e') { enh = 1; bl--; }
    for (size_t i = 0; i < sizeof kEra / sizeof *kEra; i++)
        if (strlen(kEra[i].build) == bl && !strncmp(key, kEra[i].build, bl)) {
            e.year = kEra[i].year;
            e.family = kEra[i].family;
            break;
        }
    if (!strncmp(key, "mac", 3) && slash) {  // Mac OS X: "Karen" is premium, "KarenCompact" compact
        size_t n = strlen(slash + 1);
        enh = !(n > 7 && !strcmp(slash + 1 + n - 7, "Compact"));
    }
    e.enhanced = enh;
    return e;
}

int vx_lang_same(const char *a, const char *b) {
    Tag x = parse(a), y = parse(b);
    return x.lang[0] && !strcmp(x.lang, y.lang);
}

int vx_lang_pick(const char *current_key, const char *language, const char *const *keys,
                 const char *const *languages, size_t n) {
    Tag want = parse(language);
    if (!want.lang[0]) return -1;
    int cur = -1;
    for (size_t i = 0; current_key && i < n; i++)
        if (keys[i] && !strcmp(keys[i], current_key)) { cur = (int)i; break; }
    if (cur >= 0) {
        Tag have = parse(languages[cur]);
        // the voice in use already speaks it (and the text names no other region)
        if (!strcmp(have.lang, want.lang) && (!want.region[0] || !strcmp(have.region, want.region))) return cur;
    }
    const char *region = want.region[0] ? want.region : default_region(want.lang);
    Era here = era_of(current_key);
    int best = -1;
    long best_score = 0;
    for (size_t i = 0; i < n; i++) {
        if (!keys[i] || !languages[i]) continue;
        Tag have = parse(languages[i]);
        if (strcmp(have.lang, want.lang)) continue;
        Era e = era_of(keys[i]);
        long score = 0;
        if (region[0] && strcmp(have.region, region)) score += want.region[0] ? 10000 : 5000;
        score += labs((long)(e.year - here.year)) * 100;
        if (e.family != here.family) score += 50;
        if (e.enhanced != here.enhanced) score += 20;
        if (e.year == 2012 && !e.enhanced) score -= 1;  // ties: iOS 6's compacts, VoiceOver's own
        if (best < 0 || score < best_score) { best = (int)i; best_score = score; }
    }
    return best;
}
