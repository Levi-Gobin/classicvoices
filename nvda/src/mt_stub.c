// The NVDA add-on carries only the Vocalizer voices. MacinTalk on Windows is
// Panthera Speech's job (https://github.com/tgeczy/panthera-speech), so the
// MacinTalk hooks in vxstream.c resolve to these stubs, and the empty native
// tables below satisfy vxstream's weak recompiled-code references (the add-on
// ships interpreter-only; vx_set_native with a zero count changes nothing).
#include "mt/mtengine.h"
#include "thumb.h"

#include <stdio.h>

struct MtEngine { int unused; };

MtEngine *mte_open(const char *root, char *err, size_t errlen) {
    (void)root;
    snprintf(err, errlen, "MacinTalk voices are not part of this add-on; use Panthera Speech");
    return NULL;
}
void mte_set_extra_voices(MtEngine *e, const char *dir) { (void)e; (void)dir; }
void mte_close(MtEngine *e) { (void)e; }
int mte_speak(MtEngine *e, const char *voice, const char *utf8, int rate, int pitch, int volume,
              MtPcmCallback cb, void *ctx) {
    (void)e; (void)voice; (void)utf8; (void)rate; (void)pitch; (void)volume; (void)cb; (void)ctx;
    return -1;
}
const char *mte_error(MtEngine *e) { (void)e; return "MacinTalk not supported"; }
double mte_sample_rate(MtEngine *e) { (void)e; return 22050.0; }

#define EMPTY_TABLE(b) \
    const NativeEntry vx_native_##b[1] = {{0, 0}}; \
    const size_t vx_native_##b##_count = 0;
EMPTY_TABLE(ios3)
EMPTY_TABLE(ios4)
EMPTY_TABLE(ios5)
EMPTY_TABLE(ios6)
EMPTY_TABLE(ios7)
EMPTY_TABLE(ios10)
