// Automatic language switching: which voice speaks text marked as another
// language. By default the voice nearest the one in use: the same release
// (iOS 6 Samantha -> iOS 6 Anna for German), else the closest release of the
// same family (iOS, Mac OS X); that release's own voice for the language
// (Mac OS X: the most desirable of its several), its enhanced or premium
// version when that is here. A language without a region takes the voice in use when it
// is that language already, else its usual region ("de" -> de-DE).
//
// Every platform passes its own voice list (keys and BCP 47 languages); the
// choices people make for a language (globally or for one voice) are the
// platform's to apply before asking here.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Index into keys[] of the voice for `language` ("de", "de-DE", "pt_BR"),
// given the voice in use (current_key, may be NULL); -1 when no voice speaks
// that language. Returns the current voice's index when it speaks it.
int vx_lang_pick(const char *current_key, const char *language, const char *const *keys,
                 const char *const *languages, size_t n);

// Whether two BCP 47 tags name the same language (ignoring region).
int vx_lang_same(const char *a, const char *b);

// With the choices people made applied: the contents of <data>/languages.txt
// (NULL or "" for none), one choice per line,
//     <for> <language> <voice key>
// <for> is "*" (whichever voice is in use) or a voice key (only while that
// voice is in use, and it wins over "*"); <language> a bare code ("de"), so a
// choice covers de-DE and de-AT text alike, or one with its region ("zh-TW",
// winning over "zh" for that region's text), or "*" for every language not
// chosen otherwise; <voice key> "-" keeps the voice in use reading it (so
// "* * -" turns language switching off); '#' starts a comment. The voice
// in use still speaks a language it speaks, and a chosen voice that isn't
// there any more falls back to the automatic one.
int vx_lang_choose(const char *choices, const char *current_key, const char *language,
                   const char *const *keys, const char *const *languages, size_t n);

#ifdef __cplusplus
}
#endif
