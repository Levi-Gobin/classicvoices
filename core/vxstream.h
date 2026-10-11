// Streaming front end for audio-unit use: a worker thread runs the engine and
// pushes float samples into a lock-free ring buffer that a real-time render
// callback drains with vxs_read().
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VxStream VxStream;

// root holds one directory per firmware build, e.g. ios6/ and ios3/, each with
// `voiced` and TTSResources/<lang>/... Voices are addressed as "<build>/<lang>".
VxStream *vxs_create(const char *root);
void vxs_destroy(VxStream *s);

// Where downloaded enhanced voices live: <path>/<build>/<lang>/broker.hdr.asset,
// addressed with keys like "ios6e/en-US".
void vxs_set_asset_root(VxStream *s, const char *path);

// Begin speaking (cancels anything in progress). rate/pitch are percentages
// of normal (100); volume is 0-100, or -1 for the voice default (80).
// trailing_pause_ms of silence is appended after the speech (SSML <break> at
// the end of a request). Leading/trailing engine silence is trimmed.
void vxs_start(VxStream *s, const char *voice_key, const char *utf8_text, int rate, int pitch, int volume,
               int trailing_pause_ms);
void vxs_cancel(VxStream *s);

// Real-time safe. Copies up to `frames` samples into `out`. *done is set to 1
// once the current utterance has been fully produced and drained.
size_t vxs_read(VxStream *s, float *out, size_t frames, int *done);

// Engine pitch value (1-100, 50 = normal) for a pitch in percent of normal;
// also the value for an inline ESC\\pitch=N\\ command.
int vxs_engine_pitch(int percent);

// Warm up a language's engine on the worker thread so the first utterance is fast.
void vxs_preload(VxStream *s, const char *voice_key);

// Timing of the most recent utterance (milliseconds), for diagnostics.
double vxs_last_first_audio_ms(VxStream *s);
double vxs_last_synth_ms(VxStream *s);
double vxs_last_audio_ms(VxStream *s);
const char *vxs_last_error(VxStream *s);

// Synchronous one-shot timing run on the calling thread (fresh engine).
int vxs_benchmark(const char *root, const char *asset_root, const char *voice_key, const char *text,
                  int rate, double *open_ms, double *first_ms, double *total_ms, double *audio_ms);

#ifdef __cplusplus
}
#endif
