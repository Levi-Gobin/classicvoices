// IPA (SSML <phoneme>, NVDA's PhonemeCommand, AVSpeechSynthesizer's IPA
// notation) -> what a voice reads as phonemes: L&H+ between ESC\toi=lhp\ and
// ESC\toi=orth\ for the Vocalizer engines, MacinTalk's own phonemes between
// [[inpt PHON]] and [[inpt TEXT]] for MacinTalk.
//
// L&H+ is a SAMPA-like alphabet whose symbols differ from language to
// language, so the conversion is checked against the voice itself: every
// voice's data holds the table of L&H+ symbols its engine accepts, and only
// those are ever sent. An IPA sound the voice's language doesn't have becomes
// the nearest one it does (an English voice says the French nasal vowel of
// "vin" as "E"); one with nothing near fails the conversion, and the caller
// says the text instead. Where the user imported iOS 10, Apple's own IPA maps
// for its languages (<voices root>/ios10/phonemeMaps/phonemeMap-<locale>.json)
// are tried first for tones and as a fallback.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Engine text that says `ipa` (UTF-8) in the voice `voice_key` ("ios6/en-US",
// "ios5e/en-GB", "mac108/Samantha", "mt105/Alex"), malloc'd; NULL when the
// voice can't say it, and the caller should say the text instead: iOS 3 and 4
// say nothing for a consonant on its own (joined to a word before it, with
// nothing between, it is fine; `joined` says so). iOS 4 gets the older
// ESC/+ ... ESC/+ switch, since it reads ESC\toi=lhp\ input out as text. voices_root and asset_root are vxs_create's and
// vxs_set_asset_root's; language ("en-US", may be NULL) picks Apple's map.
char *vx_phoneme_text(const char *voices_root, const char *asset_root, const char *voice_key, const char *language,
                      const char *ipa, int joined);

// Whether the IPA has a vowel or a syllabic consonant.
int vx_ipa_has_vowel(const char *ipa);

#ifdef __cplusplus
}
#endif
