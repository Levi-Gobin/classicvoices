// SSML (or plain text) to engine utterances for the Linux and Android hosts,
// the C counterpart of the iOS extension's SSMLParser: prosody pitch becomes
// inline ESC\pitch=N\ commands, <break> becomes ESC\pause=N\ (a trailing one
// becomes the utterance's trailing pause), </s> and </p> end sentences, and
// every <mark> ends a segment, so the host can report it once the audio
// before it has been played.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *text;            // engine text (may be empty when only a mark is left)
    char *mark;            // mark name reached after this segment, or NULL
    int rate;              // percent of normal, for vxs_start
    int pitch;             // percent of normal (the segment's starting pitch)
    int volume;            // 0-100, or -1 for the voice default
    int trailing_pause_ms;
} CvSegment;

typedef struct {
    CvSegment *seg;
    size_t n;
} CvUtterance;

struct CvRules;

// base_rate/base_pitch in percent (100 = normal), base_volume 0-100 or -1.
// Text that doesn't start with '<' is taken as plain text. rules (may be
// NULL) are Apple's pronunciation rules, applied to the text, never to the
// markup around it.
void cv_parse_ssml(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out);
void cv_parse_ssml_rules(const char *in, int base_rate, int base_pitch, int base_volume, const struct CvRules *rules,
                         CvUtterance *out);
// The same for text that is never markup (Android hands engines plain text,
// which may well start with '<').
void cv_parse_plain(const char *in, int base_rate, int base_pitch, int base_volume, CvUtterance *out);
void cv_free_utterance(CvUtterance *u);

#ifdef __cplusplus
}
#endif
