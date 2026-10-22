// Minimal stand-in for Speech Dispatcher's audio types (spd_audio_plugin.h).
#pragma once

typedef enum { SPD_AUDIO_LE, SPD_AUDIO_BE } AudioFormat;

typedef struct {
    int bits;
    int num_channels;
    int sample_rate;
    int num_samples;
    signed short *samples;
} AudioTrack;
