// Minimal stand-in for Speech Dispatcher's speechd_types.h: the types the
// vendored module helpers use (see README.md).
#pragma once

typedef struct {
    char *name;      // the voice's name, as clients select it
    char *language;  // BCP 47 language
    char *variant;   // dialect or similar, may be NULL
} SPDVoice;

typedef enum {
    SPD_MSGTYPE_TEXT = 0,
    SPD_MSGTYPE_SOUND_ICON = 1,
    SPD_MSGTYPE_CHAR = 2,
    SPD_MSGTYPE_KEY = 3,
    SPD_MSGTYPE_SPELL = 99
} SPDMessageType;
