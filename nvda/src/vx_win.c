// Windows implementations for the Classic Voices engine port: UTF-8 file
// access, the mmap shim, Darwin's default rune table, and small stubs.
// This file is NOT compiled with vx_win.h force-included.
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "compat/runetype.h"
#include "compat/sys/mman.h"

// ---------------------------------------------------------------- UTF-8 paths

static wchar_t *wide(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) { free(w); w = NULL; }
    return w;
}

FILE *vx_fopen_utf8(const char *path, const char *mode) {
    wchar_t *w = wide(path);
    if (!w) return NULL;
    wchar_t wmode[8] = L"rb";
    for (int i = 0; mode[i] && i < 6; i++) { wmode[i] = (wchar_t)mode[i]; wmode[i + 1] = 0; }
    FILE *f = _wfopen(w, wmode);
    free(w);
    return f;
}

int vx_open_rd(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return -1;
    int fd = _wopen(w, _O_RDONLY | _O_BINARY);
    free(w);
    return fd;
}

int vx_access_utf8(const char *path, int mode) {
    wchar_t *w = wide(path);
    if (!w) return -1;
    int rc = _waccess(w, mode & 6);  // no X_OK on Windows
    free(w);
    return rc;
}

int vx_stat_utf8(const char *path, struct stat *st) {
    wchar_t *w = wide(path);
    if (!w) return -1;
    struct _stat64 s64;
    int rc = _wstat64(w, &s64);
    free(w);
    if (rc == 0) {
        memset(st, 0, sizeof *st);
        st->st_mode = s64.st_mode;
        st->st_size = (off_t)s64.st_size;  // voice files are < 2 GB
    }
    return rc;
}

void vx_sleep_ns(long long ns) {
    Sleep(ns <= 0 ? 0 : (DWORD)((ns + 999999) / 1000000));
}

// ---------------------------------------------------------------- mmap shim

void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    (void)prot;
    if (flags & MAP_FIXED) {
        // File content read into the already-committed guest region: private
        // copy-on-write semantics come for free.
        if (fd >= 0) {
            if (_lseeki64(fd, off, SEEK_SET) < 0) return MAP_FAILED;
            char *p = addr;
            size_t got = 0;
            while (got < len) {
                unsigned chunk = len - got > (1u << 24) ? (1u << 24) : (unsigned)(len - got);
                int n = _read(fd, p + got, chunk);
                if (n < 0) return MAP_FAILED;
                if (n == 0) break;  // mapping may extend past EOF; rest stays zero
                got += (size_t)n;
            }
        }
        return addr;
    }
    void *p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p ? p : MAP_FAILED;
}

int munmap(void *addr, size_t len) {
    (void)len;
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

// ---------------------------------------------------------------- os_log shim

void vx_winlog(int dummy, const char *fmt, ...) {
    (void)dummy;
    static int on = -1;
    if (on < 0) { const char *e = getenv("CV_DEBUG"); on = e && *e && *e != '0'; }
    if (!on) return;
    // rewrite Apple's %{public}s / %{private}s to plain %s
    char f[512];
    size_t o = 0;
    for (const char *p = fmt; *p && o < sizeof f - 2; p++) {
        if (*p == '%' && p[1] == '{') {
            const char *close = strchr(p, '}');
            if (close) { f[o++] = '%'; p = close; continue; }
        }
        f[o++] = *p;
    }
    f[o] = 0;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, f, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------- rune table
// Darwin/BSD _CTYPE_* bit values; the low byte of a digit's runetype holds its
// numeric value (__maskrune(c, 0xFF), as digittoint uses).

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

static void rune_init(void) {
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

__attribute__((constructor)) static void vx_win_init(void) { rune_init(); }

#ifndef __clang__
unsigned int vx_bitrev32(unsigned int v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
#endif
