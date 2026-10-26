// Extra exports for the NVDA synth driver (the vxs_* API is exported as-is;
// see cvengine.def). ctypes-friendly helpers live here.
#include "vxstream.h"
#include "cv_catalog.h"
#include "cv_rules.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CV_ABI 1

int cv_abi_version(void) { return CV_ABI; }

// Apple's pronunciation rules (user_rules.txt) for a voice, applied to text:
// a malloc'd UTF-8 string to give back with cv_free (text itself, copied, when
// the voice's language has none).
char *cv_rules_text(const char *voices_root, const char *voice_key, const char *language, const char *text) {
    CvVoice v;
    memset(&v, 0, sizeof v);
    snprintf(v.key, sizeof v.key, "%s", voice_key ? voice_key : "");
    const CvRules *r = cv_rules_for(voices_root, cv_voice_build(&v), language);
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
