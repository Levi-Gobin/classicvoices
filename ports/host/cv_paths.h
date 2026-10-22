// Where Classic Voices keeps its data on Linux: $CLASSICVOICES_DATA, else
// $XDG_DATA_HOME/classicvoices, else ~/.local/share/classicvoices. Android
// passes its own app-private paths instead.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char root[1024], voices[1100], downloads[1100], native[1200];
} CvPaths;

static inline const char *cv_abi_name(void) {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

static inline void cv_paths_from_root(CvPaths *p, const char *root) {
    snprintf(p->root, sizeof p->root, "%s", root);
    snprintf(p->voices, sizeof p->voices, "%s/Voices", root);
    snprintf(p->downloads, sizeof p->downloads, "%s/Downloads", root);
    snprintf(p->native, sizeof p->native, "%s/Native/%s", root, cv_abi_name());
}

static inline void cv_default_paths(CvPaths *p) {
    const char *d = getenv("CLASSICVOICES_DATA");
    char root[1024];
    if (d && *d) snprintf(root, sizeof root, "%s", d);
    else if ((d = getenv("XDG_DATA_HOME")) && *d) snprintf(root, sizeof root, "%s/classicvoices", d);
    else snprintf(root, sizeof root, "%s/.local/share/classicvoices", getenv("HOME") ? getenv("HOME") : ".");
    cv_paths_from_root(p, root);
}
