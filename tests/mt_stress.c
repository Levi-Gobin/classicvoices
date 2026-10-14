// Speaks with one MacinTalk voice the way VoiceOver does: short phrases, most
// cut off part way, at random rates and pitches, through the same stream
// layer as the audio unit. Reports a phrase that stops producing audio, and
// exits if speech never comes back. Build with tests/mt_stress.sh.
//   mt_stress <voices root> [voice key] [phrases] [asset root] [seed]
//   mt_stress extracted mt106/Alex 3000
#include "vxstream.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static const char *phrases[] = {
    "Settings", "Button", "Double tap to open.", "Messages, 3 new items", "Wi-Fi, on, button",
    "Battery 85 percent", "12:42 PM", "Mail. Inbox, 1,204 unread. Heading.", "Back", "Search field. Double tap to edit.",
    "Hello, my name is Alex. I am one of the voices of Mac OS X, and this is a much longer sentence to keep me talking for a while.",
    "Selected. Classic Voices, 2 of 14.", "Dock, Safari", "www.apple.com/ios", "Don't you think it's \xe2\x80\x9cwonderful\xe2\x80\x9d?",
    "\x1b\\pitch=60\\Notification Center\x1b\\pitch=50\\", "Control Center. 4 items.", "  ", "1 2 3 4 5 6 7 8 9 10", "@#$%^&*()",
    "Edit\x1b\\eos=1\\Delete\x1b\\eos=1\\Share", "Caf\xc3\xa9 na\xc3\xafve r\xc3\xa9sum\xc3\xa9", "\xf0\x9f\x98\x80 grinning face",
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <voices root> [voice key] [phrases] [asset root] [seed]\n", argv[0]); return 2; }
    const char *key = argc > 2 ? argv[2] : "mt106/Alex";
    int count = argc > 3 ? atoi(argv[3]) : 1000;
    VxStream *s = vxs_create(argv[1]);
    vxs_set_asset_root(s, argc > 4 ? argv[4] : "/nonexistent");
    srand(argc > 5 ? (unsigned)atoi(argv[5]) : 1);
    static float buf[4096];
    int n = sizeof phrases / sizeof *phrases, stalls = 0;
    double worst = 0;
    for (int i = 0; i < count; i++) {
        const char *text = phrases[rand() % n];
        int rate = 50 + rand() % 250, pitch = 80 + rand() % 40;
        vxs_start(s, key, text, rate, pitch, -1, 0);
        // VoiceOver cuts most phrases off: after 0-400 ms of audio, or not at all
        int cut = rand() % 3 ? (rand() % 400) * 22 : -1;
        size_t got = 0;
        int done = 0, stalled = 0;
        double t0 = now(), last = t0;
        while (!done) {
            size_t k = vxs_read(s, buf, 512, &done);
            got += k;
            if (k) last = now();
            if (cut >= 0 && (int)got >= cut) { vxs_cancel(s); break; }
            if (now() - last > 8 && !stalled) {
                stalled = 1;
                stalls++;
                printf("STALL phrase %d rate %d [%s] after %zu samples: %s\n", i, rate, text, got, vxs_last_error(s));
                fflush(stdout);
            }
            if (now() - last > 40) { printf("DEAD: no speech for 40 s\n"); return 1; }
            if (!k) { struct timespec ts = {0, 2000000}; nanosleep(&ts, 0); }
        }
        double d = now() - t0;
        if (d > worst) worst = d;
        if (i % 100 == 0) { printf("phrase %d ok (slowest %.2f s)\n", i, worst); fflush(stdout); }
    }
    printf("done: %d phrases, %d stalls, slowest %.2f s\n", count, stalls, worst);
    vxs_destroy(s);
    return stalls ? 1 : 0;
}
