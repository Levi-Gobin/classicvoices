// mtswitch <root:voice>...  -- loads each engine in turn in one process (as the
// speech extension does when the voice changes) and reports the audio length.
#include "mtengine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int cb(void *ctx, const float *p, size_t n) { (void)p; *(size_t *)ctx += n; return 0; }
int main(int argc, char **argv) {
    const char *text = "VoiceOver on. Settings. Button. Double tap to open. Messages, 3 new items. It is 4:15 PM on Saturday, October 10th, 2026.";
    // A generation stays loaded while consecutive entries use it, as in vxstream.
    MtEngine *e = NULL;
    char cur[1024] = "";
    for (int i = 1; i < argc; i++) {
        char root[1024], err[512];
        snprintf(root, sizeof root, "%s", argv[i]);
        char *voice = strrchr(root, ':');
        if (!voice) return 2;
        *voice++ = 0;
        if (strcmp(root, cur)) {
            if (e) mte_close(e);
            e = mte_open(root, err, sizeof err);
            if (!e) { printf("%s: open failed: %s\n", argv[i], err); cur[0] = 0; continue; }
            snprintf(cur, sizeof cur, "%s", root);
            // MT_EXTRA=<dir>: <dir>/<generation>/Voices holds extra voices (the app group layout)
            if (getenv("MT_EXTRA")) {
                char extra[1200];
                const char *gen = strrchr(root, '/');
                snprintf(extra, sizeof extra, "%s/%s/Voices", getenv("MT_EXTRA"), gen ? gen + 1 : root);
                mte_set_extra_voices(e, extra);
            }
        }
        size_t n = 0;
        int rc = mte_speak(e, voice, text, 100, 100, -1, cb, &n);
        printf("%-28s rc=%d audio=%.2fs %s\n", argv[i], rc, n / mte_sample_rate(e), rc ? mte_error(e) : "");
        fflush(stdout);
    }
    if (e) mte_close(e);
    return 0;
}
