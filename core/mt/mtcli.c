// mtcli: run Apple's i386 MacinTalk (Mac OS X 10.4-10.6) on the x86 interpreter.
//   mtcli <MacinTalk> <SpeechDictionary.framework/Versions/A> <Voice.SpeechVoice> "text" out.wav [libs...]
#include "mt_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void write_wav(const char *path, const float *pcm, uint32_t n, int rate) {
    FILE *f = fopen(path, "wb");
    uint32_t data = n * 2, riff = 36 + data, fmt = 16, br = (uint32_t)rate * 2;
    uint16_t pf = 1, ch = 1, al = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pf, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) {
        double v = pcm[i];  // as panthera-speech: clamp, then truncate
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        int16_t s = (int16_t)(v * 32767.0);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s <MacinTalk> <SpeechDictionary dir> <Voice.SpeechVoice> text out.wav [dylibs...]\n", argv[0]); return 2; }
    char err[512];
    Mt *m = mt_create(err, sizeof err);
    if (!m) { fprintf(stderr, "%s\n", err); return 1; }
    m->verbose = getenv("MT_VERBOSE") ? atoi(getenv("MT_VERBOSE")) : 0;
    jmp_buf jb;
    m->fault = &jb;
    m->main->cpu.fault = &jb;
    if (setjmp(jb)) { fprintf(stderr, "fault: %s%s\n", m->err, m->main->cpu.err); return 1; }

    char sd[1024];
    snprintf(sd, sizeof sd, "%s/SpeechDictionary", argv[2]);
    for (int i = 6; i < argc; i++) mt_load(m, argv[i]);
    mt_load(m, sd);
    int mt = mt_load(m, argv[1]);
    mt_bind_all(m);
    mt_cf_set_dictionary_bundle(m, argv[2]);
    double t0 = now();
    mt_run_initializers(m);

    uint32_t open = mt_export(m, mt, "_SEOpenSpeechChannel"), use = mt_export(m, mt, "_SEUseVoice");
    uint32_t speak = mt_export(m, mt, "_SESpeakBuffer"), speakcf = mt_export(m, mt, "_SESpeakCFString");
    if (!open || !use || (!speak && !speakcf)) { fprintf(stderr, "engine API not found\n"); return 1; }
    uint32_t chanp = mt_static(m, 4);
    uint32_t rc = mt_call(m, open, 1, &chanp);
    uint32_t chan = rd32(&m->main->cpu, chanp);
    fprintf(stderr, "SEOpenSpeechChannel -> %d, channel %#x\n", (int)rc, chan);
    if (rc || !chan) return 1;

    char vd[1100];
    snprintf(vd, sizeof vd, "%s/Contents/Resources/VoiceDescription", argv[3]);
    FILE *f = fopen(vd, "rb");
    uint8_t h[12] = {0};
    if (!f || fread(h, 1, 12, f) != 12) { fprintf(stderr, "cannot read %s\n", vd); return 1; }
    fclose(f);
    uint32_t spec = mt_static(m, 8);
    wr32(&m->main->cpu, spec, be32(h + 4));
    wr32(&m->main->cpu, spec + 4, be32(h + 8));
    uint32_t args3[3] = {chan, spec, mt_cf_bundle(m, argv[3])};
    rc = mt_call(m, use, 3, args3);
    fprintf(stderr, "SEUseVoice -> %d\n", (int)rc);
    if (rc) return 1;
    double t1 = now();

    if (getenv("MT_RATE")) {  // SESetSpeechInfo(chan, 'rate', &Fixed)
        uint32_t setinfo = mt_export(m, mt, "_SESetSpeechInfo"), fx = mt_static(m, 4);
        wr32(&m->main->cpu, fx, (uint32_t)atoi(getenv("MT_RATE")) << 16);
        uint32_t a[3] = {chan, 0x72617465u, fx};
        fprintf(stderr, "SESetSpeechInfo rate -> %d\n", (int)mt_call(m, setinfo, 3, a));
    }
    const char *text = argv[4];
    uint32_t n = (uint32_t)strlen(text), buf = mt_malloc(m, n + 1);
    memcpy(G(m, buf), text, n + 1);
    mt_audio_begin();
    if (speak) {
        uint32_t args4[4] = {chan, buf, n, 0};
        rc = mt_call(m, speak, 4, args4);
        fprintf(stderr, "SESpeakBuffer -> %d\n", (int)rc);
    } else {  // 10.7: a CFString and an options dictionary
        uint32_t args3s[3] = {chan, mt_cf_string(m, text, n), 0};
        rc = mt_call(m, speakcf, 3, args3s);
        fprintf(stderr, "SESpeakCFString -> %d\n", (int)rc);
    }
    uint32_t frames = mt_audio_wait(0.5, 120);
    double t2 = now();
    uint32_t np;
    const float *pcm = mt_audio_pcm(&np);
    write_wav(argv[5], pcm, np, (int)mt_audio_rate());
    double secs = np / mt_audio_rate();
    extern double mt_audio_first_ms(void);
    printf("%u frames (%.2f s) | open %.0f ms | first audio %.0f ms | speak %.0f ms | %.1fx realtime | %.1f M instr\n", frames, secs,
           (t1 - t0) * 1e3, mt_audio_first_ms(), (t2 - t1) * 1e3, secs / (t2 - t1), m->main->cpu.icount / 1e6);
    return 0;
}
