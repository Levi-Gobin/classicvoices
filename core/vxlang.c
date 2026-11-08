// Automatic language switching: the voice for text in another language
// (see vxlang.h).
#include "vxlang.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

// A Mac OS X release's own voice for a language, among the several it offers:
// the one with the highest VoiceRelativeDesirability in its Info.plist (read
// from Apple's voice packages); where Mavericks has a tie, iOS 7.0's voice (its
// compacts are iOS 7.0's). NULL: a tie with no such voice.
static const struct { const char *locale, *name[3]; } kMacDefault[] = {
    {"ar-SA", {NULL, "Maged", "Tarik"}},
    {"cs-CZ", {"Zuzana", "Zuzana", "Zuzana"}},
    {"da-DK", {"Ida", "Ida", "Sara"}},
    {"de-DE", {"Yannick", "Yannick", "Anna"}},
    {"el-GR", {"Alexandros", "Alexandros", "Melina"}},
    {"en-AU", {"Lee", "Lee", "Karen"}},
    {"en-GB", {"Daniel", "Daniel", "Daniel"}},
    {"en-IE", {"Moira", "Moira", "Moira"}},
    {"en-IN", {"Sangeeta", "Sangeeta", "Veena"}},
    {"en-US", {"Samantha", "Samantha", "Samantha"}},
    {"en-ZA", {"Tessa", "Tessa", "Tessa"}},
    {"en-scotland", {"Fiona", "Fiona", "Fiona"}},
    {"es-AR", {NULL, NULL, "Diego"}},
    {"es-CO", {NULL, NULL, NULL}},
    {"es-ES", {"Diego", "Diego", "Monica"}},
    {"es-MX", {"Javier", "Paulina", "Paulina"}},
    {"fi-FI", {"Mikko", "Mikko", "Satu"}},
    {"fr-CA", {"Felix", "Felix", "Amelie"}},
    {"fr-FR", {"Virginie", "Virginie", "Thomas"}},
    {"hi-IN", {"Lekha", "Lekha", "Lekha"}},
    {"hu-HU", {"Eszter", "Eszter", "Mariska"}},
    {"id-ID", {"Damayanti", "Damayanti", "Damayanti"}},
    {"it-IT", {"Paolo", "Paolo", "Alice"}},
    {"ja-JP", {"Kyoko", "Kyoko", "Kyoko"}},
    {"ko-KR", {"Yuna", "Yuna", "Yuna"}},
    {"nl-BE", {"Ellen", "Ellen", "Ellen"}},
    {"nl-NL", {"Xander", "Xander", "Xander"}},
    {"no-NO", {"Stine", "Stine", "Nora"}},
    {"pl-PL", {"Agata", "Agata", "Zosia"}},
    {"pt-BR", {"Raquel", "Raquel", "Luciana"}},
    {"pt-PT", {"Joana", "Joana", "Joana"}},
    {"ro-RO", {"Simona", "Simona", "Ioana"}},
    {"ru-RU", {"Milena", "Milena", "Milena"}},
    {"sk-SK", {"Laura", "Laura", "Laura"}},
    {"sv-SE", {"Oskar", "Oskar", "Alva"}},
    {"th-TH", {"Narisa", "Narisa", "Kanya"}},
    {"tr-TR", {"Aylin", "Aylin", "Yelda"}},
    {"zh-CN", {"Ting-Ting", "Ting-Ting", "Ting-Ting"}},
    {"zh-HK", {"Sin-Ji", "Sin-Ji", "Sin-ji"}},
    {"zh-TW", {"Ya-Ling", "Ya-Ling", "Mei-Jia"}},
};

// Whether a voice is its release's own voice for its language: every iOS voice
// is; a Mac OS X voice ("mac109/AnnaCompact", "de-DE") when kMacDefault says so;
// iOS 7's Siri voices never were (VoiceOver couldn't use them).
static int release_default(const char *key, const char *language) {
    if (key && !strncmp(key, "siri7/", 6)) return 0;
    if (!key || strncmp(key, "mac10", 5) || !language) return !key || strncmp(key, "mac", 3);
    int era = key[5] - '7';  // mac107 / mac108 / mac109
    const char *slash = strchr(key, '/');
    if (era < 0 || era > 2 || !slash) return 0;
    char loc[16];
    size_t i = 0;
    for (; language[i] && i < sizeof loc - 1; i++) loc[i] = language[i] == '_' ? '-' : language[i];
    loc[i] = 0;
    for (size_t r = 0; r < sizeof kMacDefault / sizeof *kMacDefault; r++) {
        if (strcasecmp(kMacDefault[r].locale, loc)) continue;
        if (!kMacDefault[r].name[era]) return 1;  // a tie: each is as much its own
        size_t n = strlen(kMacDefault[r].name[era]);
        return !strncasecmp(slash + 1, kMacDefault[r].name[era], n) &&
               (!slash[1 + n] || !strcmp(slash + 1 + n, "Compact"));
    }
    return 0;
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
        {"siri7", 2013, 3},
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
    if ((!strncmp(key, "mac", 3) || !strncmp(key, "siri7/", 6)) && slash) {  // "Karen" premium, "KarenCompact" compact
        size_t n = strlen(slash + 1);
        enh = !(n > 7 && !strcmp(slash + 1 + n - 7, "Compact"));
    }
    e.enhanced = enh;
    return e;
}

// iOS 7's Siri voices come in pairs per language: from one Siri voice, the
// other language's voice of the same gender is nearest.
static int siri_female(const char *key) {
    static const char *const female[] = {"Nicky", "Martha", "Catherine", "Marie", "Helena", "O-ren", "Yu-shu"};
    if (!key || strncmp(key, "siri7/", 6)) return -1;
    for (size_t i = 0; i < sizeof female / sizeof *female; i++)
        if (!strncmp(key + 6, female[i], strlen(female[i]))) return 1;
    return 0;
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
    int here_female = siri_female(current_key);
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
        if (!e.enhanced) score += 20;  // the enhanced (premium) version of a voice, when it is here
        if (!release_default(keys[i], languages[i])) score += 30;  // a release's own voice for it first
        if (e.year == 2012 && !e.enhanced) score -= 1;  // ties: iOS 6's compacts, VoiceOver's own
        int f = siri_female(keys[i]);
        if (here_female >= 0 && f >= 0 && f != here_female) score += 5;
        if (best < 0 || score < best_score) { best = (int)i; best_score = score; }
    }
    return best;
}

// The voice key a choices line gives `want` for `who` ("*" or a voice key);
// writes it into out (empty: none). A line naming the region ("zh-TW") wins
// over one for the whole language ("zh").
static void chosen(const char *choices, const char *who, Tag want, char *out, size_t cap) {
    out[0] = 0;
    int best = 0;  // 1: the line for every language, 2: the language's, 3: its region's
    const char *region = want.region[0] ? want.region : default_region(want.lang);
    for (const char *line = choices; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char f[3][128];
        int nf = 0;
        size_t i = 0;
        while (nf < 3 && i < len && line[i] != '#') {
            while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
            if (i >= len || line[i] == '#') break;
            size_t j = 0;
            while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && j < sizeof f[0] - 1)
                f[nf][j++] = line[i++];
            f[nf++][j] = 0;
        }
        if (nf == 3 && !strcmp(f[0], who)) {
            Tag t = parse(f[1]);
            int rank = !strcmp(f[1], "*") ? 1  // every language
                     : strcmp(t.lang, want.lang) ? 0 : !t.region[0] ? 2 : !strcmp(t.region, region) ? 3 : 0;
            if (rank > best) {
                best = rank;
                snprintf(out, cap, "%s", f[2]);
            }
        }
        line = end ? end + 1 : NULL;
    }
}

int vx_lang_choose(const char *choices, const char *current_key, const char *language,
                   const char *const *keys, const char *const *languages, size_t n) {
    int autov = vx_lang_pick(current_key, language, keys, languages, n);
    if (!choices || !*choices) return autov;
    if (autov >= 0 && current_key && !strcmp(keys[autov], current_key)) return autov;
    Tag want = parse(language);
    char k[128];
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 && !current_key) continue;
        chosen(choices, pass == 0 ? current_key : "*", want, k, sizeof k);
        if (!k[0]) continue;
        const char *find = strcmp(k, "-") ? k : current_key;  // "-": the voice in use reads it
        if (!find) continue;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(keys[i], find)) return (int)i;
    }
    return autov;
}
