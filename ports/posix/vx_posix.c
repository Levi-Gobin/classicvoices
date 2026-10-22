// Linux and Android support for the Classic Voices engine: logging and
// Darwin's default rune table (the engine was compiled against Darwin libc, so
// __maskrune and the _DefaultRuneLocale table handed into guest memory must
// use Darwin's BSD _CTYPE_* bit values). Not compiled with vx_posix.h.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/runetype.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

// ---------------------------------------------------------------- logging
// Level 0 debug, 1 info, 2 error. Linux: errors always, the rest with
// CV_DEBUG=1. Android: everything to logcat under the tag ClassicVoices.
void vx_posix_log(int level, const char *fmt, ...) {
#ifndef __ANDROID__
    static int verbose = -1;
    if (verbose < 0) { const char *e = getenv("CV_DEBUG"); verbose = e && *e && *e != '0'; }
    if (level < 2 && !verbose) return;
#endif
    char f[512];
    size_t o = 0;
    for (const char *p = fmt; *p && o < sizeof f - 2; p++) {
        if (*p == '%' && p[1] == '{') {  // %{public}s -> %s
            const char *close = strchr(p, '}');
            if (close) { f[o++] = '%'; p = close; continue; }
        }
        f[o++] = *p;
    }
    f[o] = 0;
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(level >= 2 ? ANDROID_LOG_ERROR : level == 1 ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG,
                         "ClassicVoices", f, ap);
#else
    vfprintf(stderr, f, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
}

// ---------------------------------------------------------------- rune table
// Darwin/BSD _CTYPE_* bit values; the low byte of a digit's runetype holds its
// numeric value (__maskrune(c, 0xFF), as digittoint uses). Same table as the
// Windows port (nvda/src/vx_win.c).
#define CT_A 0x00000100u
#define CT_C 0x00000200u
#define CT_D 0x00000400u
#define CT_G 0x00000800u
#define CT_L 0x00001000u
#define CT_P 0x00002000u
#define CT_S 0x00004000u
#define CT_U 0x00008000u
#define CT_X 0x00010000u
#define CT_B 0x00020000u
#define CT_R 0x00040000u

_RuneLocale _DefaultRuneLocale;

__attribute__((constructor)) static void rune_init(void) {
    _RuneLocale *r = &_DefaultRuneLocale;
    for (int c = 0; c < 256; c++) {
        r->__maplower[c] = c;
        r->__mapupper[c] = c;
        if (c >= 128) continue;  // like Darwin's C locale: no flags past ASCII
        uint32_t t = 0;
        if (c < 0x20 || c == 0x7F) t |= CT_C;
        if (c == '\t') t |= CT_S | CT_B;
        if (c == '\n' || c == '\v' || c == '\f' || c == '\r') t |= CT_S;
        if (c == ' ') t = CT_S | CT_B | CT_R;
        if (c >= '0' && c <= '9') t = CT_D | CT_R | CT_G | CT_X | (uint32_t)(c - '0');
        if (c >= 'A' && c <= 'Z') {
            t = CT_U | CT_A | CT_R | CT_G;
            if (c <= 'F') t |= CT_X | (uint32_t)(c - 'A' + 10);
            r->__maplower[c] = c + 32;
        }
        if (c >= 'a' && c <= 'z') {
            t = CT_L | CT_A | CT_R | CT_G;
            if (c <= 'f') t |= CT_X | (uint32_t)(c - 'a' + 10);
            r->__mapupper[c] = c - 32;
        }
        if (c > 0x20 && c < 0x7F && !(t & (CT_A | CT_D))) t |= CT_P | CT_R | CT_G;
        r->__runetype[c] = t;
    }
}
