// mtsay <generation root> <voice> "text" out.wav  -- MtEngine test driver
#include "mtengine.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { float *pcm; size_t n, cap; double t0, first; } Buf;
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static int cb(void *ctx, const float *p, size_t n) {
    Buf *b = ctx;
    if (!b->first) for (size_t i = 0; i < n; i++) if (p[i] > 1e-3f || p[i] < -1e-3f) { b->first = now() - b->t0; break; }
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->pcm = realloc(b->pcm, b->cap * sizeof(float)); }
    memcpy(b->pcm + b->n, p, n * sizeof(float));
    b->n += n;
    return 0;
}
int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: mtsay <root> <voice> text out.wav [rate pitch]\n"); return 2; }
    char err[512];
    double t = now();
    MtEngine *e = mte_open(argv[1], err, sizeof err);
    if (!e) { fprintf(stderr, "open: %s\n", err); return 1; }
    double topen = now() - t;
    Buf b = {0};
    int rate = argc > 5 ? atoi(argv[5]) : 100, pitch = argc > 6 ? atoi(argv[6]) : 100;
    for (int rep = 0; rep < (getenv("MT_REPEAT") ? atoi(getenv("MT_REPEAT")) : 1); rep++) {
        b.n = 0; b.first = 0; b.t0 = now();
        if (mte_speak(e, argv[2], argv[3], rate, pitch, -1, cb, &b)) { fprintf(stderr, "speak: %s\n", mte_error(e)); return 1; }
        double total = now() - b.t0, secs = b.n / mte_sample_rate(e);
        printf("%s: %.2fs audio | open %.0f ms | first audio %.0f ms | total %.0f ms | %.1fx realtime\n", argv[2], secs, topen * 1e3, b.first * 1e3, total * 1e3, secs / total);
    }
    FILE *f = fopen(argv[4], "wb");
    uint32_t data = (uint32_t)b.n * 2, riff = 36 + data, fmt = 16, rate_hz = 22050, br = 44100;
    uint16_t pf = 1, ch = 1, al = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f); fwrite(&pf, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&rate_hz, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (size_t i = 0; i < b.n; i++) { double v = b.pcm[i]; if (v > 1) v = 1; if (v < -1) v = -1; int16_t s = (int16_t)(v * 32767.0); fwrite(&s, 2, 1, f); }
    fclose(f);
    mte_close(e);
    fprintf(stderr, "closed cleanly\n");
    return 0;
}
