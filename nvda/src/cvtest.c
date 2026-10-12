// Smoke test for the Windows engine build: synthesize to a WAV through the
// same streaming path the NVDA driver uses.
//   cvtest <voices root> <asset root|-> <voice key> <text> <out.wav> [rate%] [pitch%]
#include "vxstream.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

size_t cv_read_s16(VxStream *s, int16_t *out, size_t frames, int *done);

static void wav_header(FILE *f, uint32_t data_bytes) {
    uint32_t rate = 22050, byterate = rate * 2;
    uint16_t align = 2, bits = 16, fmt = 1, ch = 1;
    fwrite("RIFF", 1, 4, f);
    uint32_t sz = 36 + data_bytes;
    fwrite(&sz, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmtlen = 16;
    fwrite(&fmtlen, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byterate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s root assets|- key text out.wav [rate] [pitch]\n", argv[0]); return 2; }
    VxStream *s = vxs_create(argv[1]);
    if (strcmp(argv[2], "-") != 0) vxs_set_asset_root(s, argv[2]);
    int rate = argc > 6 ? atoi(argv[6]) : 100, pitch = argc > 7 ? atoi(argv[7]) : 100;
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    vxs_start(s, argv[3], argv[4], rate, pitch, -1, 0);
    FILE *out = fopen(argv[5], "wb");
    wav_header(out, 0);
    int16_t buf[4096];
    uint32_t total = 0;
    for (;;) {
        int done = 0;
        size_t n = cv_read_s16(s, buf, 4096, &done);
        if (n) { fwrite(buf, 2, n, out); total += (uint32_t)n; }
        if (done) break;
        if (!n) Sleep(2);
    }
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f0.QuadPart;
    // warm run: engine stays cached in the stream, as in NVDA
    vxs_start(s, argv[3], "This is the warm second utterance.", rate, pitch, -1, 0);
    for (;;) {
        int done = 0;
        size_t n = cv_read_s16(s, buf, 4096, &done);
        if (done) break;
        if (!n) Sleep(1);
    }
    fprintf(stderr, "warm first audio %.0f ms, synth %.0f ms for %.0f ms audio\n",
            vxs_last_first_audio_ms(s), vxs_last_synth_ms(s), vxs_last_audio_ms(s));
    fseek(out, 0, SEEK_SET);
    wav_header(out, total * 2);
    fclose(out);
    const char *err = vxs_last_error(s);
    fprintf(stderr, "%u frames (%.2f s audio) in %.0f ms (%.1fx real time), first audio %.0f ms%s%s\n",
            total, total / 22050.0, ms, (total / 22050.0) / (ms / 1000.0),
            vxs_last_first_audio_ms(s), err[0] ? ", error: " : "", err);
    vxs_destroy(s);
    return total ? 0 : 1;
}
