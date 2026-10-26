// The voices Classic Voices can speak with on Linux and Android, found by
// scanning the data folders (the same layout as the NVDA add-on's):
//
//   <data>/Voices/<build>/voiced, TTSResources/<lang>/broker.hdr   firmware
//   <data>/Voices/mac10x/common/                                   Mac common files
//   <data>/Downloads/<build>/<lang>/broker.hdr.asset               enhanced iOS 5/6
//   <data>/Downloads/mac10x/<Voice>[Compact]/broker.hdr            Mac OS X voices
//   <data>/Native/<abi>/cvnative_<build>.so                        optional fast code
//
// MacinTalk is not here: Panthera Speech provides it on these platforms.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char key[96];        // vxstream key: "ios6/en-US", "ios6e/en-US", "mac107/Karen"
    char name[96];       // "Samantha (iOS 6)"
    char voice[48];      // "Samantha"
    char language[24];   // BCP 47, "en-US"
    char fallback[96];   // compact voice for pitch changes, or ""
    int quality;         // 1 compact, 2 enhanced / premium
    int female;          // 1 female, 0 male, -1 unknown
} CvVoice;

// Fills a malloc'd array (free with cv_free_voices); returns the count.
// Leaves out the voices listed in <data>/hidden.txt (the app's "Use as a
// system voice" switch).
size_t cv_scan_voices(const char *voices_root, const char *downloads_root, CvVoice **out);
void cv_free_voices(CvVoice *v);

// Best voice for a BCP 47 language ("en", "en-GB", "pt_BR"): an exact match
// first, then the same language; among equals, iOS 6 then enhanced. NULL if none.
const CvVoice *cv_pick_voice(const CvVoice *v, size_t n, const char *language);
const CvVoice *cv_find_voice(const CvVoice *v, size_t n, const char *name_or_key);

// The firmware build whose engine speaks a voice ("ios6" for "ios6e/en-US"
// and the Lion voices, "ios10" for Mavericks premium ...).
const char *cv_voice_build(const CvVoice *v);

#ifdef __cplusplus
}
#endif
