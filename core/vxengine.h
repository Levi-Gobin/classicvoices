// Runs Apple's iOS 6 Nuance Vocalizer engine (from the `voiced` daemon) in a
// Thumb-2 interpreter and streams 16-bit mono PCM.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Vx Vx;

// Receives PCM as soon as the engine finishes each buffer. Return nonzero to
// cancel the rest of the utterance.
typedef int (*VxPcmCallback)(void *ctx, const int16_t *samples, size_t count);

// voiced_path: the iOS 6 voiced binary. lang_dir: e.g. TTSResources/en-US.
// common_dir: TTSResources/common. Returns NULL and fills err on failure.
Vx *vx_open(const char *voiced_path, const char *lang_dir, const char *common_dir,
            char *err, size_t errlen);
void vx_close(Vx *vx);

// 0 on success (including cancellation), engine error code otherwise.
int vx_speak_utf16(Vx *vx, const uint16_t *text, size_t units, VxPcmCallback cb, void *ctx);
int vx_speak_utf8(Vx *vx, const char *text, VxPcmCallback cb, void *ctx);

// Sentence and word positions as the engine reports them, before the audio
// they belong to: the place in the text (UTF-16 units of what vx_speak_* was
// given) and the sample, counted from the start of that call, where it begins.
// iOS 3 and 4 report no word at the start of a sentence, only the sentence.
// A bookmark (ESC\mrk=N\ in the text) comes as its own place in the text;
// the engines drop some (iOS 3 and 4 one at the very start or right after
// another, iOS 5-7 one at the very end), so it is no more than a hint.
// Stays set until changed; NULL turns it off.
enum { VX_MARK_SENTENCE = 1, VX_MARK_WORD = 2, VX_MARK_BOOKMARK = 8 };
typedef void (*VxWordCallback)(void *ctx, uint32_t kind, uint32_t text_pos, uint32_t text_len, uint32_t sample);
void vx_set_word_callback(Vx *vx, VxWordCallback cb, void *ctx);

// Load a user dictionary ("application/edct-bin-dictionary") or rule set
// ("application/x-vocalizer-rettt+text") into the open instance, as iOS 7 and
// later loaded Apple's pronunciation packs. 0 on success; -1 where the
// engine's loader isn't known, else the engine's error code.
int vx_load_resource(Vx *vx, const char *mime, const void *data, size_t len);

// Set a numeric VE_PARAM (e.g. rate/volume/pitch) on the open instance.
int vx_set_param(Vx *vx, uint32_t id, uint32_t value);

int vx_sample_rate(Vx *vx);           // 22050 for iOS 6 voices, 16000 for iOS 3
const char *vx_build_name(Vx *vx);   // e.g. "iOS 3.1.3 (7E18)"
const char *vx_voice_name(Vx *vx);    // e.g. "Samantha"
const char *vx_language_name(Vx *vx); // e.g. "American English"
uint64_t vx_instruction_count(Vx *vx);
const char *vx_last_error(Vx *vx);

// Development hooks: block-start profiling (bitmap of 2 MB, one byte per
// halfword below guest 4 MB) and installing recompiled code tables.
void vx_enable_profile(Vx *vx, uint8_t *bitmap);
void vx_set_native(Vx *vx, const void *entries, size_t n);
void vx_set_budget(Vx *vx, long long n);  // debug builds of recompiled code only
uint32_t vx_budget_exit_pc(Vx *vx);

#ifdef __cplusplus
}
#endif
