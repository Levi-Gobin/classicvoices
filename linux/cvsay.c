// classicvoices-say: speak text with a Classic Voices voice to a WAV file or
// stdout, list voices, and time the engine. Linux (and macOS, for testing).
//
//   classicvoices-say -l
//   classicvoices-say [-v VOICE] [-L LANG] [-r RATE] [-p PITCH] [-n] [-o out.wav] TEXT...
//
// RATE and PITCH are percent of normal (100). VOICE is a name from -l
// ("Samantha (iOS 6)"), a key ("ios6/en-US") or just "Samantha". TEXT may be
// SSML. -n leaves out Apple's pronunciation rules. Without -o the WAV goes to stdout. Data: $CLASSICVOICES_DATA, else
// $XDG_DATA_HOME/classicvoices (~/.local/share/classicvoices).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cv_catalog.h"
#include "cv_paths.h"
#include "cv_rules.h"
#include "cv_ssml.h"
#include "vxstream.h"

void cv_set_native_dir(const char *dir);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void put32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

int main(int argc, char **argv) {
    const char *voice = NULL, *lang = NULL, *outp = NULL;
    int rate = 100, pitch = 100, list = 0, quiet = 0, norules = 0, opt;
    while ((opt = getopt(argc, argv, "lv:L:r:p:o:qn")) != -1) {
        switch (opt) {
        case 'l': list = 1; break;
        case 'v': voice = optarg; break;
        case 'L': lang = optarg; break;
        case 'r': rate = atoi(optarg); break;
        case 'p': pitch = atoi(optarg); break;
        case 'o': outp = optarg; break;
        case 'q': quiet = 1; break;
        case 'n': norules = 1; break;
        default:
            fprintf(stderr, "usage: %s -l | [-v VOICE] [-L LANG] [-r RATE%%] [-p PITCH%%] [-o out.wav] TEXT...\n", argv[0]);
            return 2;
        }
    }
    CvPaths paths;
    cv_default_paths(&paths);
    CvVoice *voices;
    size_t nv = cv_scan_voices(paths.voices, paths.downloads, &voices);
    if (list) {
        for (size_t i = 0; i < nv; i++)
            printf("%s\t%s\t%s%s%s\n", voices[i].name, voices[i].language, voices[i].key,
                   voices[i].fallback[0] ? "\tpitch fallback " : "", voices[i].fallback);
        if (!nv) fprintf(stderr, "no voices under %s (import firmware with classicvoices-data first)\n", paths.root);
        return nv ? 0 : 1;
    }
    if (optind >= argc) { fprintf(stderr, "no text\n"); return 2; }
    const CvVoice *v = voice ? cv_find_voice(voices, nv, voice) : cv_pick_voice(voices, nv, lang ? lang : "en-US");
    if (!v) { fprintf(stderr, "voice not found: %s (try -l)\n", voice ? voice : lang ? lang : "en-US"); return 1; }
    size_t tl = 0;
    for (int i = optind; i < argc; i++) tl += strlen(argv[i]) + 1;
    char *text = calloc(tl + 1, 1);
    for (int i = optind; i < argc; i++) { strcat(text, argv[i]); if (i + 1 < argc) strcat(text, " "); }

    cv_set_native_dir(paths.native);
    VxStream *s = vxs_create(paths.voices);
    vxs_set_asset_root(s, paths.downloads);
    CvUtterance u;
    cv_parse_ssml_rules(text, rate, pitch, -1, norules ? NULL : cv_rules_for(paths.voices, cv_voice_build(v), v->language), &u);

    FILE *out = outp ? fopen(outp, "wb") : stdout;
    if (!out) { perror(outp); return 1; }
    unsigned char hdr[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\0\0\0\0\0\0\0\0\x02\0\x10\0data\0\0\0\0";
    put32(hdr + 24, 22050);
    put32(hdr + 28, 22050 * 2);
    put32(hdr + 4, 0xFFFFFFFFu);  // streaming: fixed up below when seekable
    put32(hdr + 40, 0xFFFFFFFFu);
    fwrite(hdr, 1, 44, out);
    size_t frames = 0;
    double t0 = now_ms(), first = -1;
    float buf[2048];
    int16_t pcm[2048];
    for (size_t i = 0; i < u.n; i++) {
        if (!u.seg[i].text[0] && !u.seg[i].trailing_pause_ms) continue;
        vxs_start_with_fallback(s, v->key, v->fallback[0] ? v->fallback : NULL, u.seg[i].text, u.seg[i].rate,
                                u.seg[i].pitch, u.seg[i].volume, u.seg[i].trailing_pause_ms);
        int done = 0, idle = 0;
        while (!done && idle < 20000) {
            size_t n = vxs_read(s, buf, 2048, &done);
            if (!n) { usleep(1000); idle++; continue; }
            idle = 0;
            if (first < 0) first = now_ms() - t0;
            for (size_t k = 0; k < n; k++) {
                float x = buf[k] * 32767.0f;
                pcm[k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            }
            fwrite(pcm, 2, n, out);
            frames += n;
        }
        if (!quiet && u.seg[i].mark) fprintf(stderr, "mark %s at %.0f ms of audio\n", u.seg[i].mark, frames / 22.05);
    }
    double total = now_ms() - t0;
    if (outp && fseek(out, 0, SEEK_SET) == 0) {
        put32(hdr + 4, (uint32_t)(36 + frames * 2));
        put32(hdr + 40, (uint32_t)(frames * 2));
        fwrite(hdr, 1, 44, out);
    }
    if (out != stdout) fclose(out);
    if (!quiet)
        fprintf(stderr, "%s: %.0f ms of audio, first audio after %.0f ms, %.1fx real time%s%s\n", v->name, frames / 22.05,
                first, total > 0 ? frames / 22.05 / total : 0, frames ? "" : ", error: ", frames ? "" : vxs_last_error(s));
    cv_free_utterance(&u);
    vxs_destroy(s);
    cv_free_voices(voices);
    free(text);
    return frames ? 0 : 1;
}
