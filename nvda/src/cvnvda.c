// Extra exports for the NVDA synth driver (the vxs_* API is exported as-is;
// see cvengine.def). ctypes-friendly helpers live here.
#include "vxstream.h"
#include "cv_catalog.h"
#include "cv_rules.h"
#include "cv_dict.h"
#include "vxphonemes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CV_ABI 10  // 4: vxs_set_apple_tone; 5: vxs_set_steady_pauses, vxs_set_spelling_pauses;
                  // 6: vxs_take_notice, vxs_set_safe_voice; 7: cv_speech_text; 8: its own entries, community and
                  // dic_text arguments, vx_xsampa_to_ipa, vx_ipa_has_vowel; 9: vxs_set_spell_lowercase; 10: vxs_text_xsampa

int cv_abi_version(void) { return CV_ABI; }

typedef struct { const char *voices_root, *downloads_root, *voice_key, *language; } PhonemeCtx;
static char *voice_phonemes(void *ctx, const char *ipa, int joined) {
    PhonemeCtx *c = ctx;
    return vx_phoneme_text(c->voices_root, c->downloads_root, c->voice_key, c->language, ipa, joined);
}

// cv_rules_text with the dictionaries (cv_dict.h) in dict_dir applied first (NULL: none): the person's own
// entries, and the fetched ones when community is set; or, with dic_text, just the entries in it (trying one out).
// Their phonetic spans are said in this voice.
char *cv_speech_text(const char *voices_root, const char *downloads_root, const char *voice_key, const char *language,
                     int own, const char *pack_dir, const char *dict_dir, int community, const char *dic_text,
                     const char *text) {
    const CvRules *r = own || pack_dir ? cv_rules_for_voice_pack(voices_root, downloads_root, voice_key, language, own,
                                                                 pack_dir)
                                       : NULL;
    const CvDict *d = dic_text ? cv_dict_from_text(dic_text) : cv_dict_for_voice_ex(dict_dir, voice_key, language, community);
    PhonemeCtx c = {voices_root, downloads_root, voice_key, language};
    return cv_dict_speech_text(d, r, text ? text : "", voice_phonemes, &c);
}

// Apple's pronunciation rules for a voice (its own release's user_rules.txt,
// or a Mac OS X voice's user_rules.dat, when own is set) and a newer pack's
// rules (pack_dir, or NULL), applied to text: a malloc'd UTF-8 string to give
// back with cv_free (text itself, copied, when there are none).
char *cv_rules_text(const char *voices_root, const char *downloads_root, const char *voice_key, const char *language,
                    int own, const char *pack_dir, const char *text) {
    const CvRules *r = cv_rules_for_voice_pack(voices_root, downloads_root, voice_key, language, own, pack_dir);
    return cv_rules_apply(r, text ? text : "");
}

void cv_free(void *p) { free(p); }

// vxs_read in int16, for feeding nvwave.WavePlayer directly.
size_t cv_read_s16(VxStream *s, int16_t *out, size_t frames, int *done) {
    float buf[2048];
    size_t total = 0;
    while (total < frames) {
        size_t want = frames - total;
        if (want > 2048) want = 2048;
        int d = 0;
        size_t n = vxs_read(s, buf, want, &d);
        for (size_t i = 0; i < n; i++) {
            float v = buf[i] * 32767.0f;
            out[total + i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        total += n;
        if (done) *done = d;
        if (n < want) break;  // drained what's there; caller decides to wait or stop
    }
    return total;
}
