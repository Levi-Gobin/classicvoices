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
// As vxs_start, with a second voice (normally the compact version of an
// enhanced or premium voice) that speaks every part of the text whose pitch
// isn't normal, the way VoiceOver on iOS 5 and 6 did. It is loaded on first
// use and kept warm afterwards. NULL or a MacinTalk voice_key: no fallback.
void vxs_start_with_fallback(VxStream *s, const char *voice_key, const char *fallback_key, const char *utf8_text,
                             int rate, int pitch, int volume, int trailing_pause_ms);
void vxs_cancel(VxStream *s);

// Real-time safe. Copies up to `frames` samples into `out`. *done is set to 1
// once the current utterance has been fully produced and drained.
size_t vxs_read(VxStream *s, float *out, size_t frames, int *done);

// Where each word of the current utterance starts, in the order spoken:
// frame counts output frames from the utterance's first one (what vxs_read
// returns), text_pos/text_len are bytes of the UTF-8 text given to vxs_start.
// Vocalizer voices only; read them as the audio arrives (a word's position is
// known before its audio is). Each vxs_start begins a new list.
typedef struct {
    uint32_t frame;
    int32_t text_pos, text_len;
} VxsWord;
size_t vxs_read_words(VxStream *s, VxsWord *out, size_t max);

// Bookmarks: ESC\mrk=N\ in the text (N 0-4294967295) is reached at `frame`,
// counted as for words, without breaking the sentence it is in. Every one is
// reported, in the order written, once the audio before it is known: where
// the engine itself loses one, it is placed with the next word. They come
// before the audio they belong to, as words do; read them as the audio
// arrives. MacinTalk voices: the text is split at each one (MacinTalk can't
// report a place inside an utterance). Each vxs_start begins a new list.
typedef struct {
    uint32_t frame;
    uint32_t id;
} VxsMark;
size_t vxs_read_marks(VxStream *s, VxsMark *out, size_t max);

// Engine pitch value (1-100, 50 = normal) for a pitch in percent of normal;
// also the value for an inline ESC\\pitch=N\\ command.
int vxs_engine_pitch(int percent);

// Apple's later pronunciation pack for the next utterances' voice (and its
// compact fallback): the folder holding vocalizer-user-dict.dat for the
// voice's language, which goes into the engine as iOS 12 loaded it. (The
// pack's rule files are the caller's to apply to the text, with the voice's
// own rules.) NULL or "": none. Takes
// effect from the next vxs_start or vxs_preload; an engine opened with a
// different pack starts again. Vocalizer voices only.
void vxs_set_rules_pack(VxStream *s, const char *dir);

// Warm up a language's engine on the worker thread so the first utterance is fast.
void vxs_preload(VxStream *s, const char *voice_key);
// MacinTalk: reset the speech channel after an utterance with embedded
// commands ([[inpt PHON]] ...), so they don't carry into the next. On by default;
// applies from the next utterance.
void vxs_set_mt_reset(VxStream *s, int on);
// MacinTalk: Leopard reads 24-hour times as 12-hour ones (13:45 is "one
// forty-five PM"); respell them the way Snow Leopard reads them. On by default.
void vxs_set_mt_24_hour(VxStream *s, int on);
// MacinTalk: map the rate (percent of normal, 10-400) to the words per minute
// today's Alex speaks at that rate, instead of a percent of each voice's own
// default rate (50-400%). Off by default.
void vxs_set_mt_modern_rate(VxStream *s, int on);

// Timing of the most recent utterance (milliseconds), for diagnostics.
double vxs_last_first_audio_ms(VxStream *s);
double vxs_last_synth_ms(VxStream *s);
double vxs_last_audio_ms(VxStream *s);
const char *vxs_last_error(VxStream *s);

// Synchronous one-shot timing run on the calling thread (fresh engine).
// Latency through the stream as the audio unit uses it, for each voice key:
// time from vxs_start to the first audible sample for VoiceOver-like phrases
// (median of several), and from interrupting a long utterance to the next
// one's first sound. Prints LAT lines to stdout.
void vxs_latency_bench(const char *root, const char *asset_root, const char *const *voice_keys, int nkeys);
int vxs_benchmark(const char *root, const char *asset_root, const char *voice_key, const char *text,
                  int rate, double *open_ms, double *first_ms, double *total_ms, double *audio_ms);

#ifdef __cplusplus
}
#endif
