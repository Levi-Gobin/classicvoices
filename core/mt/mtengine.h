// MacinTalk (Mac OS X 10.4-10.6) as a speech engine for the stream layer.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MtEngine MtEngine;
// Return nonzero to stop.
typedef int (*MtPcmCallback)(void *ctx, const float *pcm, size_t n);

// `root` is one generation's directory:
//   MacinTalk                      the engine (Speech/Synthesizers/MacinTalk.SpeechSynthesizer/Contents/MacOS/MacinTalk)
//   SpeechDictionary/              SpeechDictionary.framework/Versions/A (binary + Resources)
//   lib/                           libc++abi.dylib / libstdc++.*.dylib as the generation needs
//   Voices/<Name>.SpeechVoice/     the voices
MtEngine *mte_open(const char *root, char *err, size_t errlen);
// A second place to look for <Name>.SpeechVoice folders (voices copied onto
// the device separately, e.g. Alex). Optional.
void mte_set_extra_voices(MtEngine *e, const char *dir);
void mte_close(MtEngine *e);
// After an utterance with embedded commands ([[inpt PHON]], [[rate 300]] ...),
// which would otherwise stay in effect, reset the channel before the next
// one. On by default.
void mte_set_reset_commands(MtEngine *e, int on);
// Leopard reads 24-hour times (13:45, 00:30) as 12-hour ones; respell them
// the way Snow Leopard reads them. On by default; no effect on other releases.
void mte_set_24_hour(MtEngine *e, int on);
// Rate as today's Alex maps it (see mte_speak). Off by default.
void mte_set_modern_rate(MtEngine *e, int on);

// Load `voice` ahead of the first utterance. 0 or an error.
int mte_prepare(MtEngine *e, const char *voice);

// Speak UTF-8 text with `voice` (folder name without .SpeechVoice), streaming
// 22.05 kHz float PCM to cb. rate/pitch are percent of the voice's defaults;
// volume 0-100 or -1. Inline commands: ESC\pause=N\ (ms), ESC\eos=1\
// (sentence boundary: a separate utterance), ESC\pitch=N\ (1-100, 50 = normal).
// Returns 0, or an error (mte_error).
int mte_speak(MtEngine *e, const char *voice, const char *utf8, int rate, int pitch, int volume,
              MtPcmCallback cb, void *ctx);
// What the engine would say for `utf8`, as MacinTalk phonemes. 0 or an error.
int mte_phonemes(MtEngine *e, const char *voice, const char *utf8, char *out, size_t cap);
const char *mte_error(MtEngine *e);
// The engine faulted or stopped responding and has shut itself down: close it
// and open a new one.
int mte_broken(MtEngine *e);
double mte_sample_rate(MtEngine *e);

#ifdef __cplusplus
}
#endif
