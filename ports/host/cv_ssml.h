// SSML (or plain text) to an engine utterance for the Linux and Android
// hosts, the C counterpart of the iOS extension's SSMLParser. Prosody changes
// become inline commands (ESC\pitch=N\, ESC\rate=N\, ESC\vol=N\), <break>
// becomes ESC\pause=N\ (a trailing one becomes the utterance's trailing
// pause), </s> and </p> end sentences, <say-as interpret-as="characters">
// (spell-out, verbatim, letters) is spelled (ESC\tn=spell\), <phoneme
// alphabet="ipa"> goes to the host's converter, and each <mark> becomes a
// bookmark (ESC\mrk=N\, N its place in CvUtterance.marks), reported by the
// stream (vxs_read_marks) without breaking the sentence.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *text;            // engine text (may be no more than bookmarks)
    int rate;              // percent of normal, for vxs_start
    int pitch;             // percent of normal (the segment's starting pitch)
    int volume;            // 0-100, or -1 for the voice default
    int trailing_pause_ms;
} CvSegment;

typedef struct {
    CvSegment *seg;        // one, or none when there is nothing to say
    size_t n;
    char **marks;          // each <mark>'s name: ESC\mrk=N\ is marks[N]
    size_t nmarks;
} CvUtterance;

// For <phoneme>: engine text that says the IPA (ESC\toi=lhp\ ... for a
// Vocalizer voice), malloc'd, or NULL to say the element's own text instead.
// joined: it comes right after a word, with nothing between them.
typedef char *(*CvPhonemeFn)(void *ctx, const char *ipa, int joined);

// A CvPhonemeFn for one voice (core/vxphonemes.c), with this as its ctx.
typedef struct {
    const char *voices_root, *asset_root;  // vxs_create's and vxs_set_asset_root's
    const char *voice_key, *language;
} CvPhonemeVoice;
char *cv_voice_phonemes(void *voice, const char *ipa, int joined);

struct CvRules;

// base_rate/base_pitch in percent (100 = normal), base_volume 0-100 or -1.
// Text that doesn't start with '<' is taken as plain text. rules (may be
// NULL) are Apple's pronunciation rules, applied to the text, never to the
// markup around it.
void cv_parse_ssml(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out);
void cv_parse_ssml_rules(const char *in, int base_rate, int base_pitch, int base_volume, const struct CvRules *rules,
                         CvUtterance *out);
// ... and phonemes through `phoneme` (NULL: each element's own text is said).
void cv_parse_ssml_ex(const char *in, int base_rate, int base_pitch, int base_volume, const struct CvRules *rules,
                      CvPhonemeFn phoneme, void *ctx, CvUtterance *out);
// The same for text that is never markup (Android hands engines plain text,
// which may well start with '<').
void cv_parse_plain(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out);
void cv_free_utterance(CvUtterance *u);

#ifdef __cplusplus
}
#endif
