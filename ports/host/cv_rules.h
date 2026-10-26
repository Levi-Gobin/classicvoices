// Apple's pronunciation rules for the Vocalizer voices on Linux (the iOS
// app's PronunciationRules.swift): iOS 5 and 6 shipped
// TTSResources/<language>/user_rules.txt, regular expressions VoiceServices
// ran over the text before the engine saw it ("iOS" -> "I O S", "OS X" ->
// "OS ten" ...). The expressions are run with PCRE2, loaded at run time
// (libpcre2-8.so.0, present on nearly every Linux system); without it the
// rules are simply skipped.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CvRules CvRules;

// The rules for a voice: iOS 6's file for its language (the most complete),
// else the voice's own firmware's, else iOS 5's. NULL when there are none or
// PCRE2 isn't available. Cached: don't free.
const CvRules *cv_rules_for(const char *voices_root, const char *build, const char *language);

// A malloc'd copy of text with the rules applied (text itself if rules is NULL).
char *cv_rules_apply(const CvRules *rules, const char *text);

// The same, keeping track of where each output byte came from: map_in gives
// a source position for every byte of text (NULL: the byte offsets), and
// *map_out (malloc'd) one for every byte of the result. Replaced text maps to
// where its match began. Needs a PCRE2 with REPLACEMENT_ONLY (10.40+);
// otherwise the text comes back unchanged.
char *cv_rules_apply_mapped(const CvRules *rules, const char *text, const int32_t *map_in, int32_t **map_out);

#ifdef __cplusplus
}
#endif
