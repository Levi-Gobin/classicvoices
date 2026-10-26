// Speaks a fixed set of utterances with every voice of one MacinTalk generation
// and prints a hash of each one's PCM, so interpreter and native-code builds can
// be compared (tests/mt_native.sh).
//
//   mt_golden <generation root> [voice...]
//
// MT_GOLDEN_CORPUS=<file> adds each line of the file as one more utterance
// (default rate and pitch); MT_GOLDEN_TIME=1 prints how long each one took;
// MT_GOLDEN_PCM=<dir> writes each utterance's PCM there.
#include "mtengine.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t h; size_t n; FILE *pcm; } Acc;
static int cb(void *ctx, const float *p, size_t n) {
    Acc *a = ctx;
    if (a->pcm) fwrite(p, sizeof *p, n, a->pcm);
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n * sizeof *p; i++) a->h = (a->h ^ b[i]) * 0x100000001b3ull;
    a->n += n;
    return 0;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static const char *texts[] = {
    "Hello there.",
    "The quick brown fox jumps over the lazy dog, 42 times; isn't that remarkable?",
    ("In 1984, Apple introduced the Macintosh, and it said hello to the world in a voice of its own. "
     "Decades later, the same voices read web pages, messages, and menus aloud."),
};
static const int settings[][2] = {{100, 100}, {250, 100}, {100, 160}, {60, 40}};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: mt_golden <root> [voice...]\n"); return 2; }
    char err[512];
    MtEngine *e = mte_open(argv[1], err, sizeof err);
    if (!e) { fprintf(stderr, "%s\n", err); return 1; }
    if (getenv("EXTRA")) mte_set_extra_voices(e, getenv("EXTRA"));
    char *voices[64];
    int nv = 0;
    if (argc > 2) for (int i = 2; i < argc && nv < 64; i++) voices[nv++] = argv[i];
    else {
        char d[1200];
        snprintf(d, sizeof d, "%s/Voices", argv[1]);
        DIR *dir = opendir(d);
        struct dirent *de;
        while (dir && (de = readdir(dir)) && nv < 64) {
            char *dot = strstr(de->d_name, ".SpeechVoice");
            if (dot && de->d_name[0] != '.') voices[nv++] = strndup(de->d_name, (size_t)(dot - de->d_name));
        }
        if (dir) closedir(dir);
        for (int i = 0; i < nv; i++)  // stable order
            for (int j = i + 1; j < nv; j++)
                if (strcmp(voices[j], voices[i]) < 0) { char *t = voices[i]; voices[i] = voices[j]; voices[j] = t; }
    }
    const char *text[512];
    int nt = 0;
    for (int t = 0; t < 3; t++) text[nt++] = texts[t];
    char line[1024];
    FILE *cf = getenv("MT_GOLDEN_CORPUS") ? fopen(getenv("MT_GOLDEN_CORPUS"), "r") : NULL;
    while (cf && nt < 512 && fgets(line, sizeof line, cf)) {
        line[strcspn(line, "\n")] = 0;
        if (*line) text[nt++] = strdup(line);
    }
    if (cf) fclose(cf);
    double total = 0;
    for (int v = 0; v < nv; v++) {
        for (int t = 0; t < nt; t++) {
            int ns = t == 1 ? 4 : 1;
            for (int s = 0; s < ns; s++) {
                Acc a = {0xcbf29ce484222325ull, 0, NULL};
                if (getenv("MT_GOLDEN_PCM")) {  // <dir>: also write each utterance's float PCM there
                    char pp[1200];
                    snprintf(pp, sizeof pp, "%s/%s_t%d_s%d.f32", getenv("MT_GOLDEN_PCM"), voices[v], t, s);
                    a.pcm = fopen(pp, "wb");
                }
                double t0 = now();
                int rc = mte_speak(e, voices[v], text[t], settings[s][0], settings[s][1], -1, cb, &a);
                double dt = now() - t0;
                if (a.pcm) fclose(a.pcm);
                total += dt;
                printf("%-12s t%d r%-3d p%-3d rc=%d frames=%-7zu %016llx", voices[v], t, settings[s][0], settings[s][1], rc, a.n,
                       (unsigned long long)a.h);
                if (getenv("MT_GOLDEN_TIME") && *getenv("MT_GOLDEN_TIME")) printf("  %.0f ms", dt * 1e3);
                printf("\n");
                fflush(stdout);
                if (rc) fprintf(stderr, "%s t%d: %s\n", voices[v], t, mte_error(e));
                if (mte_broken(e)) { fprintf(stderr, "engine broke: %s\n", mte_error(e)); return 1; }
            }
        }
    }
    if (getenv("MT_GOLDEN_TIME") && *getenv("MT_GOLDEN_TIME")) fprintf(stderr, "total %.2f s\n", total);
    mte_close(e);
    return 0;
}
