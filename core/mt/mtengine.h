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

// Speak UTF-8 text with `voice` (folder name without .SpeechVoice), streaming
// 22.05 kHz float PCM to cb. rate/pitch are percent of the voice's defaults;
// volume 0-100 or -1. Inline commands: ESC\pause=N\ (ms), ESC\eos=1\
// (sentence boundary: a separate utterance), ESC\pitch=N\ (1-100, 50 = normal).
// Returns 0, or an error (mte_error).
int mte_speak(MtEngine *e, const char *voice, const char *utf8, int rate, int pitch, int volume,
              MtPcmCallback cb, void *ctx);
const char *mte_error(MtEngine *e);
double mte_sample_rate(MtEngine *e);

#ifdef __cplusplus
}
#endif
