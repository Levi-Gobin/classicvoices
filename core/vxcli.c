// Mac test driver: vxcli <voiced> <lang_dir> <common_dir> "text" out.wav
#include "vxengine.h"
#ifdef VX_HAVE_NATIVE
#include "thumb.h"
extern const NativeEntry vx_native_ios10[], vx_native_ios70[], vx_native_ios7[], vx_native_ios6[], vx_native_ios5[], vx_native_ios4[], vx_native_ios3[];
extern const size_t vx_native_ios10_count, vx_native_ios70_count, vx_native_ios7_count, vx_native_ios6_count, vx_native_ios5_count, vx_native_ios4_count,
    vx_native_ios3_count;
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int16_t *pcm;
    size_t n, cap;
    double t0, first;
} Sink;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int on_pcm(void *ctx, const int16_t *s, size_t n) {
    Sink *k = ctx;
    if (!k->first) k->first = now() - k->t0;
    if (k->n + n > k->cap) {
        k->cap = (k->n + n) * 2;
        k->pcm = realloc(k->pcm, k->cap * 2);
    }
    memcpy(k->pcm + k->n, s, n * 2);
    k->n += n;
    return 0;
}

static void write_wav(const char *path, const int16_t *pcm, size_t n, int rate) {
    FILE *f = fopen(path, "wb");
    uint32_t data = (uint32_t)(n * 2), riff = 36 + data, fmt = 16, br = (uint32_t)rate * 2;
    uint16_t pcmfmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcmfmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f); fwrite(pcm, 2, n, f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <voiced> <lang_dir> <common_dir> <text> <out.wav>\n", argv[0]);
        return 2;
    }
    char err[512];
    double t = now();
    Vx *vx = vx_open(argv[1], argv[2], argv[3], err, sizeof err);
    if (!vx) { fprintf(stderr, "open failed: %s\n", err); return 1; }
    double topen = now() - t;
#ifdef VX_HAVE_NATIVE
    if (!getenv("VX_INTERP")) {
        const char *b = vx_build_name(vx);
        if (strstr(b, "iOS 3")) vx_set_native(vx, vx_native_ios3, vx_native_ios3_count);
        else if (strstr(b, "iOS 4")) vx_set_native(vx, vx_native_ios4, vx_native_ios4_count);
        else if (strstr(b, "iOS 5")) vx_set_native(vx, vx_native_ios5, vx_native_ios5_count);
        else if (strstr(b, "iOS 7.0")) vx_set_native(vx, vx_native_ios70, vx_native_ios70_count);
        else if (strstr(b, "iOS 7")) vx_set_native(vx, vx_native_ios7, vx_native_ios7_count);
        else if (strstr(b, "iOS 10")) vx_set_native(vx, vx_native_ios10, vx_native_ios10_count);
        else vx_set_native(vx, vx_native_ios6, vx_native_ios6_count);
    }
#endif
    if (getenv("VX_PITCH")) vx_set_param(vx, 4, (uint32_t)atoi(getenv("VX_PITCH")));
    if (getenv("VX_BUDGET")) vx_set_budget(vx, atoll(getenv("VX_BUDGET")));
    Sink k = {0};
    k.t0 = now();
    uint64_t ic0 = vx_instruction_count(vx);
    int rc = vx_speak_utf8(vx, argv[4], on_pcm, &k);
    double tspeak = now() - k.t0;
    if (rc) { fprintf(stderr, "speak failed: %s\n", vx_last_error(vx)); return 1; }
    double secs = (double)k.n / vx_sample_rate(vx);
    uint64_t ic = vx_instruction_count(vx) - ic0;
    write_wav(argv[5], k.pcm, k.n, vx_sample_rate(vx));
    printf("%s (%s): %.2fs audio | open %.0f ms | first audio %.0f ms | total %.0f ms | %.1fx realtime | %.1f M interpreted instr\n",
           vx_voice_name(vx), vx_language_name(vx), secs, topen * 1e3, k.first * 1e3, tspeak * 1e3,
           secs / tspeak, ic / 1e6);
    if (getenv("VX_BUDGET")) fprintf(stderr, "budget exit pc %#x\n", vx_budget_exit_pc(vx));
    vx_close(vx);
    return 0;
}
