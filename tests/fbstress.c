// fbstress <voices root> <asset root> <iterations> <seed>: VoiceOver-like use of
// vxstream with compact fallbacks, pitch changes, interruptions and preloads.
#include "vxstream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *pairs[][2] = {
    {"ios6e/en-US", "ios6/en-US"}, {"ios5e/en-US", "ios5/en-US"}, {"mac107/Karen", "mac107/KarenCompact"},
    {"mac108/Karen", "mac108/KarenCompact"}, {"mac109/Karen", NULL}, {"ios6/en-US", NULL}, {"ios3/en-US", NULL},
    {"mac109/KarenCompact", NULL}, {"ios70/en-US", NULL}, {"ios70e/en-US", NULL},
};
static const char *texts[] = {
    "Hello there, how are you today.",
    "Open \x1b\\pitch=70\\I O S\x1b\\pitch=50\\ on OS ten. \x1b\\eos=1\\ Next sentence here.",
    "\x1b\\pitch=60\\Capital\x1b\\pitch=50\\ letters change the pitch of a word.",
    "A",
    "(",
    "The quick brown fox jumps over the lazy dog, again and again, until the sentence is long enough to be interrupted.",
    "\x1b\\pitch=84\\Whole\x1b\\pitch=84\\ thing \x1b\\pitch=30\\low\x1b\\pitch=50\\ normal.",
    "Numbers 1 2 3 4 5 and symbols @#$% here.",
};
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
    int iters = atoi(argv[3]); srand((unsigned)atoi(argv[4]));
    VxStream *s = vxs_create(argv[1]);
    vxs_set_asset_root(s, argv[2]);
    float buf[1024]; int done; size_t total = 0; int completed = 0, cut = 0, errors = 0;
    for (int it = 0; it < iters; it++) {
        int pi = rand() % (int)(sizeof pairs / sizeof pairs[0]);
        const char *text = texts[rand() % (int)(sizeof texts / sizeof texts[0])];
        int pitch = (rand() % 3 == 0) ? 100 : 60 + rand() % 120;
        int rate = 80 + rand() % 250;
        if (rand() % 10 == 0) vxs_preload(s, pairs[rand() % (int)(sizeof pairs / sizeof pairs[0])][0]);
        vxs_start_with_fallback(s, pairs[pi][0], pairs[pi][1], text, rate, pitch, -1, rand() % 2 ? 0 : 200);
        int mode = rand() % 3;  // 0 complete, 1 interrupt after a while, 2 cancel at once
        if (mode == 2) { vxs_cancel(s); cut++; continue; }
        int idle = 0; done = 0; long budget = mode == 1 ? (rand() % 400) : 1000000;
        while (!done && idle < 4000) {
            size_t n = vxs_read(s, buf, 1024, &done);
            if (n) { total += n; idle = 0; } else { usleep(500); idle++; }
            if (mode == 1 && --budget <= 0) break;
        }
        if (done) completed++; else cut++;
        if (idle >= 4000) { errors++; fprintf(stderr, "it %d: timeout %s err=[%s]\n", it, pairs[pi][0], vxs_last_error(s)); }
        if (it % 25 == 0) { fprintf(stderr, "."); }
    }
    vxs_cancel(s);
    vxs_destroy(s);
    printf("\n%d iterations: %d completed, %d cut, %d timeouts, %zu frames\n", iters, completed, cut, errors, total);
    return errors ? 1 : 0;
}
