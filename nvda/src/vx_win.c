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

void vx_win_speech_priority(void) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}

// ---------------------------------------------------------------- mmap shim
// The engine's guest memory (vx_mmap_reserve): its low part committed, the
// rest a placeholder that each mapping takes the next piece of. A voice file
// is mapped there from the file, copy on write, as on macOS and Linux: its
// pages are read from disk only where they are used, shared by every program
// using the voice, and dropped rather than paged out, instead of the whole
// file being read into this process's memory (415 MB for a premium Mac voice).
// Pieces are carved upward, without gaps; a stray access above the last one
// (a read past a mapping) gets zero pages there, as the region committed in
// one piece had. Placeholders need Windows 10 1803 (VirtualAlloc2,
// MapViewOfFile3); before that, or when mapping a file fails, it is read into
// committed memory as before.

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#define GRAIN 0x10000  // zero pages for a stray access: this much at a time

typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
static VirtualAlloc2Fn va2;
static MapViewOfFile3Fn mv3;
static INIT_ONCE regions_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION regions_lock;

typedef struct { char *addr; int view; } Piece;
typedef struct Region {
    char *base, *end, *top;  // [base, end) carved into pieces; [end, top) one placeholder
    Piece *pieces;
    size_t n, cap;
    struct Region *next;
} Region;
static Region *regions;

static size_t page_up(size_t n) { return (n + 4095) & ~(size_t)4095; }

static Region *region_of(const void *p) {
    for (Region *r = regions; r; r = r->next)
        if ((const char *)p >= r->base && (const char *)p < r->top) return r;
    return NULL;
}

static int add_piece(Region *r, char *addr, int view) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 16;
        Piece *p = realloc(r->pieces, cap * sizeof *p);
        if (!p) return 0;
        r->pieces = p;
        r->cap = cap;
    }
    r->pieces[r->n].addr = addr;
    r->pieces[r->n++].view = view;
    return 1;
}

// The placeholder's first `len` bytes as one of their own (len page-sized);
// undone by merge_back.
static int split(Region *r, size_t len) {
    return len == (size_t)(r->top - r->end) || VirtualFree(r->end, len, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
}
static void merge_back(Region *r, size_t len) {
    if (len != (size_t)(r->top - r->end)) VirtualFree(r->end, r->top - r->end, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
}

// Zero pages from the end of the pieces up to hi (page-aligned, at most top).
static int commit_to(Region *r, char *hi) {
    if (hi <= r->end) return 1;
    size_t len = (size_t)(hi - r->end);
    if (!split(r, len)) return 0;
    if (!va2(GetCurrentProcess(), r->end, len, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
             NULL, 0)) {
        merge_back(r, len);
        return 0;
    }
    if (!add_piece(r, r->end, 0)) {
        VirtualFree(r->end, 0, MEM_RELEASE);  // can't keep track of it: leave the space unusable, not leaked
        return 0;
    }
    r->end = hi;
    return 1;
}

// fd's bytes from off into [addr, addr + len), as a view of the file when
// the pieces end at addr or below it (a gap gets zero pages), then zero pages
// past the end of the file. 0: nothing mapped (the caller reads the file).
// Placeholder views take any page-aligned address and file offset, but not
// a size past the end of the file: the view has the file's exact bytes, the
// rest of its last page reads as zeros.
static int map_file(Region *r, char *addr, size_t len, int fd, int64_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    LARGE_INTEGER size;
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size) || off < 0 || off >= size.QuadPart || (off & 4095) ||
        ((uintptr_t)addr & 4095))
        return 0;
    char *at = addr;
    if (at < r->end || !commit_to(r, at) || at != r->end) return 0;
    uint64_t from = (uint64_t)off;
    uint64_t avail = (uint64_t)size.QuadPart - from;
    size_t bytes = len < avail ? len : (size_t)avail;
    size_t span = page_up(bytes);
    HANDLE sec = CreateFileMappingW(h, NULL, PAGE_WRITECOPY, 0, 0, NULL);
    if (!sec) return 0;
    void *v = NULL;
    if (split(r, span)) {
        v = mv3(sec, GetCurrentProcess(), at, from, bytes, MEM_REPLACE_PLACEHOLDER, PAGE_WRITECOPY, NULL, 0);
        if (!v) merge_back(r, span);
    }
    CloseHandle(sec);  // the view keeps the file mapped
    if (!v) return 0;
    if (!add_piece(r, at, 1)) {
        UnmapViewOfFile(v);
        return 0;
    }
    r->end = at + span;
    return commit_to(r, addr + len);  // past the end of the file: zeros
}

// A stray access above the pieces: zero pages there, up to the next boundary.
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    char *p = (char *)e->ExceptionRecord->ExceptionInformation[1];
    LONG out = EXCEPTION_CONTINUE_SEARCH;
    EnterCriticalSection(&regions_lock);
    Region *r = region_of(p);
    if (r && p >= r->end) {
        char *hi = r->base + (((size_t)(p - r->base) + GRAIN) & ~(size_t)(GRAIN - 1));
        if (hi > r->top) hi = r->top;
        if (commit_to(r, hi)) out = EXCEPTION_CONTINUE_EXECUTION;
    }
    LeaveCriticalSection(&regions_lock);
    return out;
}

static BOOL CALLBACK init_regions(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once, (void)param, (void)ctx;
    InitializeCriticalSection(&regions_lock);
    HMODULE k = GetModuleHandleW(L"kernelbase.dll");
    if (k) {
        va2 = (VirtualAlloc2Fn)(void *)GetProcAddress(k, "VirtualAlloc2");
        mv3 = (MapViewOfFile3Fn)(void *)GetProcAddress(k, "MapViewOfFile3");
    }
    if (va2 && mv3 && !AddVectoredExceptionHandler(1, on_fault)) va2 = NULL;
    return TRUE;
}

void *vx_mmap_reserve(size_t len, size_t committed) {
    InitOnceExecuteOnce(&regions_once, init_regions, NULL, NULL);
    if (!va2 || !mv3 || getenv("CV_NO_MAP")) return NULL;
    char *base = va2(GetCurrentProcess(), NULL, len, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    Region *r = calloc(1, sizeof *r);
    if (!base || !r) {
        if (base) VirtualFree(base, 0, MEM_RELEASE);
        free(r);
        return NULL;
    }
    r->base = r->end = base;
    r->top = base + len;
    EnterCriticalSection(&regions_lock);
    int ok = commit_to(r, base + page_up(committed));
    if (ok) {
        r->next = regions;
        regions = r;
    }
    LeaveCriticalSection(&regions_lock);
    if (!ok) {
        if (r->n) VirtualFree(base, 0, MEM_RELEASE);
        VirtualFree(r->end, 0, MEM_RELEASE);
        free(r->pieces);
        free(r);
        return NULL;
    }
    return base;
}

static int read_into(char *p, size_t len, int fd, int64_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return 0;
    size_t got = 0;
    while (got < len) {
        unsigned chunk = len - got > (1u << 24) ? (1u << 24) : (unsigned)(len - got);
        int n = _read(fd, p + got, chunk);
        if (n < 0) return 0;
        if (n == 0) break;  // mapping may extend past EOF; rest stays zero
        got += (size_t)n;
    }
    return 1;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    (void)prot;
    if (flags & MAP_FIXED) {
        if (va2) {
            EnterCriticalSection(&regions_lock);
            Region *r = region_of(addr);
            int mapped = r && fd >= 0 && map_file(r, addr, len, fd, off);
            int ok = !r || mapped || commit_to(r, (char *)addr + page_up(len));
            LeaveCriticalSection(&regions_lock);
            if (!ok) return MAP_FAILED;
            if (mapped) return addr;
        }
        // Outside a region, or not mapped from the file: read it into the
        // committed memory (private copy-on-write semantics for free).
        if (fd >= 0 && !read_into(addr, len, fd, off)) return MAP_FAILED;
        return addr;
    }
    void *p = VirtualAlloc(NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p ? p : MAP_FAILED;
}

int munmap(void *addr, size_t len) {
    (void)len;
    if (va2) {
        EnterCriticalSection(&regions_lock);
        Region **pp = &regions;
        while (*pp && (*pp)->base != addr) pp = &(*pp)->next;
        Region *r = *pp;
        if (r) *pp = r->next;
        LeaveCriticalSection(&regions_lock);
        if (r) {
            for (size_t i = 0; i < r->n; i++)
                if (r->pieces[i].view) UnmapViewOfFile(r->pieces[i].addr);
                else VirtualFree(r->pieces[i].addr, 0, MEM_RELEASE);
            if (r->end < r->top) VirtualFree(r->end, 0, MEM_RELEASE);
            free(r->pieces);
            free(r);
            return 0;
        }
    }
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
