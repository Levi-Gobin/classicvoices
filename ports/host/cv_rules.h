// Apple's pronunciation rules for the Vocalizer voices on Linux, Windows and
// NVDA (the iOS app's PronunciationRules.swift): each iOS release shipped
// TTSResources/<language>/user_rules.txt, and each Mac OS X voice its own
// user_rules.dat: regular expressions run over the text before the engine saw
// it ("iOS" -> "I O S", "OS X" -> "OS ten" ...). The expressions are run with PCRE2, loaded at run time
// (libpcre2-8.so.0, present on nearly every Linux system); without it the
// rules are simply skipped.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CvRules CvRules;

// The rules a voice had: its own firmware's file for its language
// (<voices_root>/<build>/TTSResources/<language>/user_rules.txt; an enhanced
// voice uses its firmware's), or a Mac OS X voice's own user_rules.dat in its
// folder (<downloads_root>/<era>/<voice>, else <voices_root>/<era>/<voice>).
// NULL when it had none (MacinTalk never did) or PCRE2 isn't available.
// Cached: don't free.
const CvRules *cv_rules_for_voice(const char *voices_root, const char *downloads_root, const char *voice_key,
                                  const char *language);

// The voice's own rules (unless own is 0) followed by Apple's later rules
// from a pronunciation pack folder ("Newer pronunciation rules":
// vocalizer-override-rules.txt, then vocalizer-user-rules.txt), as one set;
// pack_dir NULL: none. Rules are compiled only when a text might match them
// (vxrules.h).
const CvRules *cv_rules_for_voice_pack(const char *voices_root, const char *downloads_root, const char *voice_key,
                                       const char *language, int own, const char *pack_dir);

// Apple's later pronunciation pack for a voice ("Newer pronunciation rules"):
// <data_root>/Pronunciation/<language>, written into out, when it has been
// downloaded; NULL otherwise and for MacinTalk. Its rules go to
// cv_rules_for_voice_pack, the folder to vxs_set_rules_pack (its dictionary).
const char *cv_rules_pack_dir(const char *data_root, const char *voice_key, const char *language, char *out,
                              size_t cap);

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
