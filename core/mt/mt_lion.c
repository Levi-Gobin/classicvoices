// Mac OS X 10.7 (Lion) additions for MacinTalk: the speech property API's
// constant keys, more CoreFoundation strings, blocks and the rest of GCD,
// vDSP's FFT, and small libc pieces. Behaviour follows panthera-speech's
// tiger_host_cf.c / tiger_host_gcd.c.
#include "mt_internal.h"

#include <Accelerate/Accelerate.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }
static inline double argd(X86 *c, int w) { uint64_t u = (uint64_t)A(c, w) | (uint64_t)A(c, w + 1) << 32; double d; memcpy(&d, &u, 8); return d; }
static inline float argf(X86 *c, int i) { uint32_t u = A(c, i); float f; memcpy(&f, &u, 4); return f; }
static inline float *F(Mt *m, uint32_t a) { return (float *)G(m, a); }

uint32_t mt_cf_string(Mt *m, const char *s, size_t n);
const char *mt_cf_text(Mt *m, uint32_t s);
uint64_t mt_wall_us(void);

// ---------------------------------------------------------------- speech property keys

static const struct { const char *sym, *text; } kSpeechConsts[] = {
    {"_kSpeechRateProperty", "rate"}, {"_kSpeechPitchBaseProperty", "pitchBase"}, {"_kSpeechPitchModProperty", "pitchMod"},
    {"_kSpeechVolumeProperty", "volume"}, {"_kSpeechResetProperty", "reset"}, {"_kSpeechStatusProperty", "status"},
    {"_kSpeechErrorsProperty", "errors"}, {"_kSpeechInputModeProperty", "inputMode"},
    {"_kSpeechCharacterModeProperty", "characterMode"}, {"_kSpeechNumberModeProperty", "numberMode"},
    {"_kSpeechCommandDelimiterProperty", "commandDelimiter"}, {"_kSpeechRecentSyncProperty", "recentSync"},
    {"_kSpeechPhonemeSymbolsProperty", "phonemeSymbols"}, {"_kSpeechPhonemeOptionsProperty", "phonemeOptions"},
    {"_kSpeechOutputToFileURLProperty", "outputToFileURL"}, {"_kSpeechOfflineModeProperty", "offlineMode"},
    {"_kSpeechAudioGraphProperty", "audioGraph"}, {"_kSpeechAudioUnitProperty", "audioUnit"},
    {"_kSpeechRefConProperty", "refCon"}, {"_kSpeechCommandPrefix", "commandPrefix"}, {"_kSpeechCommandSuffix", "commandSuffix"},
    {"_kSpeechTextDoneCallBack", "textDoneCallBack"}, {"_kSpeechSpeechDoneCallBack", "speechDoneCallBack"},
    {"_kSpeechSyncCallBack", "syncCallBack"}, {"_kSpeechErrorCFCallBack", "errorCallBack"},
    {"_kSpeechPhonemeCallBack", "phonemeCallBack"}, {"_kSpeechWordCFCallBack", "wordCallBack"},
    {"_kSpeechModeText", "TEXT"}, {"_kSpeechModePhoneme", "PHON"}, {"_kSpeechModeTune", "TUNE"},
    {"_kSpeechModeNormal", "NORM"}, {"_kSpeechModeLiteral", "LTRL"}, {"_kSpeechNoSpeechInterrupt", "NoSpeechInterrupt"},
    {"_kSpeechPreflightThenPause", "PreflightThenPause"}, {"_kSpeechErrorCount", "count"},
    {"_kSpeechErrorNewest", "newest"}, {"_kSpeechErrorNewestCharacterOffset", "newestCharacterOffset"},
    {"_kSpeechErrorOldest", "oldest"}, {"_kSpeechErrorOldestCharacterOffset", "oldestCharacterOffset"},
    {"_kSpeechErrorCallbackSpokenString", "spokenString"}, {"_kSpeechErrorCallbackCharacterOffset", "characterOffset"},
    {"_kSpeechStatusOutputBusy", "outputBusy"}, {"_kSpeechStatusOutputPaused", "outputPaused"},
    {"_kSpeechStatusNumberOfCharactersLeft", "numberOfCharactersLeft"}, {"_kSpeechStatusPhonemeCode", "phonemeCode"},
    {"_kSpeechPhonemeInfoOpcode", "opcode"}, {"_kSpeechPhonemeInfoSymbol", "symbol"},
    {"_kSpeechPhonemeInfoExample", "example"}, {"_kSpeechPhonemeInfoHiliteStart", "hiliteStart"},
    {"_kSpeechPhonemeInfoHiliteEnd", "hiliteEnd"},
    {"_kCFLocaleLanguageCode", "kCFLocaleLanguageCode"}, {"_kCFErrorDomainOSStatus", "NSOSStatusErrorDomain"},
    {NULL, NULL},
};
static uint32_t g_key_obj[64];

int mt_lion_is_data(const char *name) {
    for (int i = 0; kSpeechConsts[i].sym; i++) if (!strcmp(kSpeechConsts[i].sym, name)) return 1;
    return !strcmp(name, "__NSConcreteStackBlock") || !strcmp(name, "__NSConcreteGlobalBlock") || !strcmp(name, "__NSConcreteMallocBlock");
}
void mt_lion_init_data(Mt *m, const char *name, uint32_t addr) {
    for (int i = 0; kSpeechConsts[i].sym; i++)
        if (!strcmp(kSpeechConsts[i].sym, name)) {
            if (!g_key_obj[i]) g_key_obj[i] = mt_cf_string(m, kSpeechConsts[i].text, strlen(kSpeechConsts[i].text));
            memcpy(G(m, addr), &g_key_obj[i], 4);
            return;
        }
}
// the object for a key, e.g. "rate", so the host can call SESetSpeechProperty
uint32_t mt_speech_key(Mt *m, const char *sym) {
    for (int i = 0; kSpeechConsts[i].sym; i++)
        if (!strcmp(kSpeechConsts[i].sym, sym)) {
            if (!g_key_obj[i]) g_key_obj[i] = mt_cf_string(m, kSpeechConsts[i].text, strlen(kSpeechConsts[i].text));
            return g_key_obj[i];
        }
    return 0;
}

// ---------------------------------------------------------------- more CFString
// Our strings are bytes (MacRoman for utterances). Object layout is mt_cf.c's:
// +8 cstr, +12 length.

static const uint16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};
static uint16_t uni(uint8_t b) { return b < 0x80 ? b : kMacRoman[b - 0x80]; }
static uint8_t from_uni(uint32_t u) {
    if (u < 0x80) return (uint8_t)u;
    for (int i = 0; i < 128; i++) if (kMacRoman[i] == u) return (uint8_t)(0x80 + i);
    return '?';
}
static uint32_t s_ptr(Mt *m, uint32_t s) { uint32_t v; memcpy(&v, G(m, s + 8), 4); return v; }
static uint32_t s_len(Mt *m, uint32_t s) { uint32_t v; memcpy(&v, G(m, s + 12), 4); return v; }
static void s_set(Mt *m, uint32_t s, uint32_t p, uint32_t n) { memcpy(G(m, s + 8), &p, 4); memcpy(G(m, s + 12), &n, 4); }

static void append_bytes(Mt *m, uint32_t s, const uint8_t *b, uint32_t n) {
    uint32_t old = s_ptr(m, s), len = s_len(m, s);
    uint32_t np = mt_malloc(m, len + n + 1);
    if (len) memcpy(G(m, np), G(m, old), len);
    memcpy(G(m, np + len), b, n);
    G(m, np)[len + n] = 0;
    s_set(m, s, np, len + n);
}
static void h_CFStringCreateMutable(Mt *m, X86 *c) { R(c, mt_cf_string(m, "", 0)); }
static void h_CFStringCreateCopy(Mt *m, X86 *c) { uint32_t s = A(c, 1); R(c, mt_cf_string(m, (const char *)G(m, s_ptr(m, s)), s_len(m, s))); }
static void h_CFStringAppend(Mt *m, X86 *c) { uint32_t d = A(c, 0), s = A(c, 1); append_bytes(m, d, G(m, s_ptr(m, s)), s_len(m, s)); }
static void h_CFStringAppendCharacters(Mt *m, X86 *c) {
    uint32_t d = A(c, 0), u = A(c, 1), n = A(c, 2);
    uint8_t *tmp = malloc(n + 1);
    for (uint32_t i = 0; i < n; i++) tmp[i] = from_uni(rd16(c, u + 2 * i));
    append_bytes(m, d, tmp, n);
    free(tmp);
}
static void h_CFStringCreateWithCharactersNoCopy(Mt *m, X86 *c) {
    uint32_t u = A(c, 1), n = A(c, 2);
    char *tmp = malloc(n + 1);
    for (uint32_t i = 0; i < n; i++) tmp[i] = (char)from_uni(rd16(c, u + 2 * i));
    R(c, mt_cf_string(m, tmp, n));
    free(tmp);
}
static void h_CFStringCreateWithPascalString(Mt *m, X86 *c) {
    uint32_t p = A(c, 1);
    uint8_t n = rd8(c, p);
    R(c, mt_cf_string(m, (const char *)G(m, p + 1), n));
}
static void h_CFStringGetCharacterAtIndex(Mt *m, X86 *c) {
    uint32_t s = A(c, 0), i = A(c, 1);
    R(c, i < s_len(m, s) ? uni(G(m, s_ptr(m, s))[i]) : 0);
}
// CFStringGetCharacters(str, CFRange{loc,len}, UniChar *buffer)
static void h_CFStringGetCharacters(Mt *m, X86 *c) {
    uint32_t s = A(c, 0), loc = A(c, 1), len = A(c, 2), buf = A(c, 3), n = s_len(m, s);
    const uint8_t *b = G(m, s_ptr(m, s));
    for (uint32_t i = 0; i < len && loc + i < n; i++) wr16(c, buf + 2 * i, uni(b[loc + i]));
}
static void h_CFStringCompare(Mt *m, X86 *c) {
    uint32_t a = A(c, 0), b = A(c, 1), flags = A(c, 2);
    uint32_t na = s_len(m, a), nb = s_len(m, b), n = na < nb ? na : nb;
    int r = (flags & 1) ? strncasecmp((const char *)G(m, s_ptr(m, a)), (const char *)G(m, s_ptr(m, b)), n)
                        : memcmp(G(m, s_ptr(m, a)), G(m, s_ptr(m, b)), n);
    if (!r) r = na < nb ? -1 : na > nb;
    R(c, (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0));
}
size_t mt_format(Mt *m, char *out, size_t cap, const char *fmt, void *va);
// CFStringCreateWithFormat(alloc, options, format, ...): %@ prints a CF object's text
static void h_CFStringCreateWithFormat(Mt *m, X86 *c) {
    uint32_t fs = A(c, 2);
    const char *fmt = (const char *)G(m, s_ptr(m, fs));
    char out[2048];
    size_t n = 0;
    uint32_t sp = c->r[ESP] + 16;
    for (const char *p = fmt; *p && n < sizeof out - 64;) {
        if (*p != '%') { out[n++] = *p++; continue; }
        const char *spec = p++;
        while (*p && strchr("-+ #0123456789.hlqLzjt", *p)) p++;
        char conv = *p ? *p++ : 0;
        char f[32];
        snprintf(f, sizeof f, "%.*s", (int)(p - spec), spec);
        if (conv == '@') {
            uint32_t o = rd32(c, sp); sp += 4;
            n += (size_t)snprintf(out + n, sizeof out - n, "%.*s", o ? (int)s_len(m, o) : 6, o ? (const char *)G(m, s_ptr(m, o)) : "(null)");
        } else if (conv == 'f' || conv == 'g' || conv == 'e') {
            uint64_t u = rd32(c, sp) | (uint64_t)rd32(c, sp + 4) << 32; sp += 8;
            double d; memcpy(&d, &u, 8);
            n += (size_t)snprintf(out + n, sizeof out - n, f, d);
        } else if (conv == 's') {
            uint32_t a = rd32(c, sp); sp += 4;
            n += (size_t)snprintf(out + n, sizeof out - n, f, a ? mt_str(m, a) : "(null)");
        } else if (conv == '%') {
            out[n++] = '%';
        } else {
            uint32_t v = rd32(c, sp); sp += 4;
            n += (size_t)snprintf(out + n, sizeof out - n, f, v);
        }
    }
    R(c, mt_cf_string(m, out, n < sizeof out ? n : sizeof out - 1));
}
static void h_CFDataCreateWithBytesNoCopy(Mt *m, X86 *c) {
    extern uint32_t mt_cf_data_wrap(Mt *m, uint32_t bytes, uint32_t n);
    R(c, mt_cf_data_wrap(m, A(c, 1), A(c, 2)));
}
static void h_CFLocaleCreate(Mt *m, X86 *c) { R(c, mt_cf_string(m, "en_US", 5)); }
static void h_CFLocaleGetValue(Mt *m, X86 *c) { R(c, mt_cf_string(m, "en", 2)); }
static void h_CFErrorCreate(Mt *m, X86 *c) { R(c, mt_cf_string(m, "error", 5)); }
static void h_CFAbsoluteTimeGetCurrent(Mt *m, X86 *c) { (void)m; x86_fpush(c, mt_wall_us() / 1e6 - 978307200.0); }

// ---------------------------------------------------------------- blocks
// struct Block_layout { isa, flags, reserved, invoke, descriptor }; descriptor { reserved, size, copy, dispose }
#define BLK_NEEDS_FREE (1 << 24)
#define BLK_HAS_COPY_DISPOSE (1 << 25)
#define BLK_IS_GLOBAL (1 << 28)

static void block_invoke(Mt *m, X86 *c, uint32_t blk) {
    if (!blk) return;
    uint32_t fn = rd32(c, blk + 12);
    mt_call_on(m, c, fn, 1, &blk);
}
static void h_Block_copy(Mt *m, X86 *c) {
    uint32_t src = A(c, 0);
    if (!src) { R(c, 0); return; }
    uint32_t flags = rd32(c, src + 4);
    if (flags & (BLK_IS_GLOBAL | BLK_NEEDS_FREE)) { R(c, src); return; }
    uint32_t desc = rd32(c, src + 16), size = rd32(c, desc + 4);
    uint32_t dst = mt_malloc(m, size);
    memcpy(G(m, dst), G(m, src), size);
    wr32(c, dst + 4, (flags & ~0xFFFFu) | BLK_NEEDS_FREE | 2);
    if (flags & BLK_HAS_COPY_DISPOSE) { uint32_t a[2] = {dst, src}; mt_call_on(m, c, rd32(c, desc + 8), 2, a); }
    R(c, dst);
}
static void h_Block_release(Mt *m, X86 *c) { (void)m; (void)c; }  // never freed: blocks are few and small
// _Block_object_assign(dest*, object, flags): byref variables move to the heap
static void h_Block_object_assign(Mt *m, X86 *c) {
    uint32_t dest = A(c, 0), obj = A(c, 1), flags = A(c, 2) & 0x7F;
    if (flags == 8) {  // BLOCK_FIELD_IS_BYREF: {isa, forwarding, flags, size, keep, destroy}
        uint32_t src = rd32(c, obj + 4);
        uint32_t f = rd32(c, src + 8);
        if (!(f & BLK_NEEDS_FREE)) {
            uint32_t size = rd32(c, src + 12), heap = mt_malloc(m, size);
            memcpy(G(m, heap), G(m, src), size);
            wr32(c, heap + 4, heap);
            wr32(c, heap + 8, (f & ~0xFFFFu) | BLK_NEEDS_FREE | 2);
            if (f & BLK_HAS_COPY_DISPOSE) { uint32_t a[2] = {heap, src}; mt_call_on(m, c, rd32(c, src + 16), 2, a); }
            wr32(c, src + 4, heap);
            src = heap;
        }
        wr32(c, dest, src);
        return;
    }
    if (flags == 7) {  // a block: copy it
        uint32_t sp = c->r[ESP];
        (void)sp;
        wr32(c, dest, obj);
        return;
    }
    wr32(c, dest, obj);  // objects: no retain counting here
}
static void h_Block_object_dispose(Mt *m, X86 *c) { (void)m; (void)c; }

// ---------------------------------------------------------------- dispatch (synchronous)
static void h_dispatch_sync(Mt *m, X86 *c) { block_invoke(m, c, A(c, 1)); }
static void h_dispatch_async(Mt *m, X86 *c) { block_invoke(m, c, A(c, 1)); }
static void h_dispatch_once(Mt *m, X86 *c) {
    uint32_t pred = A(c, 0);
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int run = !pred || rd32(c, pred) == 0;
    if (pred) wr32(c, pred, ~0u);
    pthread_mutex_unlock(&mu);
    if (run) block_invoke(m, c, A(c, 1));
}
// dispatch_after(when, queue, block). Lion uses it to restart its audio graph
// no sooner than 300 ms after the last stop, spinning on a flag the block sets;
// there is no real device to protect, so the block runs now.
static void h_dispatch_after(Mt *m, X86 *c) { block_invoke(m, c, A(c, 3)); }
static uint32_t g_global_queue;
static void h_dispatch_get_global_queue(Mt *m, X86 *c) {
    if (!g_global_queue) g_global_queue = mt_static(m, 16);
    R(c, g_global_queue);
}

// ---------------------------------------------------------------- FFT (host vDSP)
#define MAX_FFT 32
static FFTSetup g_fft[MAX_FFT];
static void h_create_fftsetup(Mt *m, X86 *c) {
    (void)m;
    for (uint32_t i = 1; i < MAX_FFT; i++)
        if (!g_fft[i]) { g_fft[i] = vDSP_create_fftsetup(A(c, 0), (FFTRadix)A(c, 1)); R(c, 0x46460000u + i); return; }
    R(c, 0);
}
static FFTSetup fft_of(uint32_t h) { uint32_t i = h - 0x46460000u; return i < MAX_FFT ? g_fft[i] : NULL; }
static void h_destroy_fftsetup(Mt *m, X86 *c) {
    (void)m;
    uint32_t i = A(c, 0) - 0x46460000u;
    if (i < MAX_FFT && g_fft[i]) { vDSP_destroy_fftsetup(g_fft[i]); g_fft[i] = NULL; }
}
static DSPSplitComplex split(Mt *m, X86 *c, uint32_t g) { DSPSplitComplex z = {F(m, rd32(c, g)), F(m, rd32(c, g + 4))}; return z; }
// vDSP_fft_zrip(setup, DSPSplitComplex *C, IC, Log2N, Direction)
static void h_fft_zrip(Mt *m, X86 *c) {
    FFTSetup s = fft_of(A(c, 0));
    if (!s) x86_fault(c, "bad FFT setup");
    DSPSplitComplex z = split(m, c, A(c, 1));
    vDSP_fft_zrip(s, &z, (vDSP_Stride)(int32_t)A(c, 2), A(c, 3), (FFTDirection)(int32_t)A(c, 4));
}
// vDSP_ctoz(DSPComplex *C, IC, DSPSplitComplex *Z, IZ, N)
static void h_ctoz(Mt *m, X86 *c) {
    DSPSplitComplex z = split(m, c, A(c, 2));
    vDSP_ctoz((const DSPComplex *)F(m, A(c, 0)), (vDSP_Stride)(int32_t)A(c, 1), &z, (vDSP_Stride)(int32_t)A(c, 3), A(c, 4));
}
static void h_ztoc(Mt *m, X86 *c) {
    DSPSplitComplex z = split(m, c, A(c, 0));
    vDSP_ztoc(&z, (vDSP_Stride)(int32_t)A(c, 1), (DSPComplex *)F(m, A(c, 2)), (vDSP_Stride)(int32_t)A(c, 3), A(c, 4));
}
// vDSP_zvcmul(A, IA, B, IB, C, IC, N): C = conj(A) * B
static void h_zvcmul(Mt *m, X86 *c) {
    DSPSplitComplex a = split(m, c, A(c, 0)), b = split(m, c, A(c, 2)), o = split(m, c, A(c, 4));
    vDSP_zvcmul(&a, (vDSP_Stride)(int32_t)A(c, 1), &b, (vDSP_Stride)(int32_t)A(c, 3), &o, (vDSP_Stride)(int32_t)A(c, 5), A(c, 6));
}
// vDSP_maxvi(A, IA, C*, I* (vDSP_Length, 4 bytes on i386), N)
static void h_maxvi(Mt *m, X86 *c) {
    float *a = F(m, A(c, 0)), best = -INFINITY;
    int ia = (int)A(c, 1);
    uint32_t n = A(c, 4), bi = 0;
    for (uint32_t i = 0; i < n; i++) if (a[i * ia] > best) { best = a[i * ia]; bi = i * (uint32_t)ia; }
    *F(m, A(c, 2)) = best;
    wr32(c, A(c, 3), bi);
}
// vDSP_vclip(A, IA, B*, C*, D, ID, N)
static void h_vclip(Mt *m, X86 *c) {
    float *a = F(m, A(c, 0)), lo = *F(m, A(c, 2)), hi = *F(m, A(c, 3)), *d = F(m, A(c, 4));
    int ia = (int)A(c, 1), id = (int)A(c, 5);
    uint32_t n = A(c, 6);
    for (uint32_t i = 0; i < n; i++) { float v = a[i * ia]; d[i * id] = v < lo ? lo : v > hi ? hi : v; }
}
// vDSP_vma(A, IA, B, IB, C, IC, D, ID, N): D = A*B + C
static void h_vma(Mt *m, X86 *c) {
    float *a = F(m, A(c, 0)), *b = F(m, A(c, 2)), *cc = F(m, A(c, 4)), *d = F(m, A(c, 6));
    int ia = (int)A(c, 1), ib = (int)A(c, 3), ic = (int)A(c, 5), id = (int)A(c, 7);
    uint32_t n = A(c, 8);
    for (uint32_t i = 0; i < n; i++) d[i * id] = a[i * ia] * b[i * ib] + cc[i * ic];
}
// vDSP_vramp(A*, B*, C, IC, N): C[i] = A + i*B
static void h_vramp(Mt *m, X86 *c) {
    float a = *F(m, A(c, 0)), b = *F(m, A(c, 1)), *o = F(m, A(c, 2));
    int ic = (int)A(c, 3);
    uint32_t n = A(c, 4);
    for (uint32_t i = 0; i < n; i++) o[i * ic] = a + (float)i * b;
}

// ---------------------------------------------------------------- libc odds

static void h_exp(Mt *m, X86 *c) { (void)m; x86_fpush(c, exp(argd(c, 0))); }
static void h_exp2(Mt *m, X86 *c) { (void)m; x86_fpush(c, exp2(argd(c, 0))); }
static void h_exp2f(Mt *m, X86 *c) { (void)m; x86_fpush(c, exp2f(argf(c, 0))); }
static void h_log2f(Mt *m, X86 *c) { (void)m; x86_fpush(c, log2f(argf(c, 0))); }
static void h_clock(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)(mt_wall_us() & 0x7FFFFFFF)); }
static void h_strncmp(Mt *m, X86 *c) { R(c, (uint32_t)strncmp(mt_str(m, A(c, 0)), mt_str(m, A(c, 1)), A(c, 2))); }
static void h_strpbrk(Mt *m, X86 *c) {
    const char *s = mt_str(m, A(c, 0)), *p = strpbrk(s, mt_str(m, A(c, 1)));
    R(c, p ? A(c, 0) + (uint32_t)(p - s) : 0);
}
static void h_strncasecmp_l(Mt *m, X86 *c) { R(c, (uint32_t)strncasecmp(mt_str(m, A(c, 0)), mt_str(m, A(c, 1)), A(c, 2))); }
uint32_t mt_rune_mask(uint32_t ch);
static void h_maskrune_l(Mt *m, X86 *c) { (void)m; R(c, mt_rune_mask(A(c, 0)) & A(c, 1)); }
static void h_toupper_l(Mt *m, X86 *c) { (void)m; uint32_t v = A(c, 0); R(c, v < 128 ? (uint32_t)toupper((int)v) : v); }
static uint32_t g_locale;
static void h_newlocale(Mt *m, X86 *c) { if (!g_locale) g_locale = mt_static(m, 64); R(c, g_locale); }
static void h_noop(Mt *m, X86 *c) { (void)m; (void)c; }
static void h_zero(Mt *m, X86 *c) { (void)m; R(c, 0); }
// localtime_r(const time_t*, struct tm*): i386 struct tm is 11 ints + tm_zone pointer (44 bytes)
static void h_localtime_r(Mt *m, X86 *c) {
    (void)m;
    time_t t = (time_t)(int32_t)rd32(c, A(c, 0));
    struct tm tm;
    localtime_r(&t, &tm);
    int v[11] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst, (int)tm.tm_gmtoff, 0};
    for (int i = 0; i < 11; i++) wr32(c, A(c, 1) + 4 * (uint32_t)i, (uint32_t)v[i]);
    R(c, A(c, 1));
}
// vasprintf(char **out, fmt, va_list)
typedef struct { X86 *c; uint32_t sp; } Va;
static void h_vasprintf(Mt *m, X86 *c) {
    char buf[4096];
    Va va = {c, A(c, 2)};
    size_t n = mt_format(m, buf, sizeof buf, mt_str(m, A(c, 1)), &va);
    if (n >= sizeof buf) n = sizeof buf - 1;
    uint32_t p = mt_malloc(m, (uint32_t)n + 1);
    memcpy(G(m, p), buf, n);
    G(m, p)[n] = 0;
    wr32(c, A(c, 0), p);
    R(c, (uint32_t)n);
}
static void h_isinit(Mt *m, X86 *c) { (void)m; if (A(c, 1)) wr8(c, A(c, 1), 1); R(c, 0); }
static void h_sqlite3_column_int64(Mt *m, X86 *c) {
    extern int64_t mt_sqlite_col64(uint32_t stmt, int col);
    (void)m;
    int64_t v = mt_sqlite_col64(A(c, 0), (int)A(c, 1));
    c->r[EAX] = (uint32_t)v;
    c->r[EDX] = (uint32_t)((uint64_t)v >> 32);
}

const MtHleEntry mt_hle_lion[] = {
    {"_CFStringCreateMutable", h_CFStringCreateMutable}, {"_CFStringCreateCopy", h_CFStringCreateCopy},
    {"_CFStringAppend", h_CFStringAppend}, {"_CFStringAppendCharacters", h_CFStringAppendCharacters},
    {"_CFStringCreateWithCharactersNoCopy", h_CFStringCreateWithCharactersNoCopy},
    {"_CFStringCreateWithCharacters", h_CFStringCreateWithCharactersNoCopy},
    {"_CFStringCreateWithPascalString", h_CFStringCreateWithPascalString},
    {"_CFStringGetCharacterAtIndex", h_CFStringGetCharacterAtIndex}, {"_CFStringGetCharacters", h_CFStringGetCharacters},
    {"_CFStringCompare", h_CFStringCompare}, {"_CFStringCreateWithFormat", h_CFStringCreateWithFormat},
    {"_CFDataCreateWithBytesNoCopy", h_CFDataCreateWithBytesNoCopy}, {"_CFLocaleCreate", h_CFLocaleCreate},
    {"_CFLocaleGetValue", h_CFLocaleGetValue}, {"_CFErrorCreateWithUserInfoKeysAndValues", h_CFErrorCreate},
    {"_CFAbsoluteTimeGetCurrent", h_CFAbsoluteTimeGetCurrent},
    {"__Block_copy", h_Block_copy}, {"__Block_release", h_Block_release}, {"__Block_object_assign", h_Block_object_assign},
    {"__Block_object_dispose", h_Block_object_dispose},
    {"_dispatch_sync", h_dispatch_sync}, {"_dispatch_async", h_dispatch_async}, {"_dispatch_once", h_dispatch_once},
    {"_dispatch_after", h_dispatch_after}, {"_dispatch_get_global_queue", h_dispatch_get_global_queue},
    {"_create_fftsetup", h_create_fftsetup}, {"_vDSP_create_fftsetup", h_create_fftsetup},
    {"_destroy_fftsetup", h_destroy_fftsetup}, {"_vDSP_destroy_fftsetup", h_destroy_fftsetup},
    {"_fft_zrip", h_fft_zrip}, {"_vDSP_fft_zrip", h_fft_zrip}, {"_ctoz", h_ctoz}, {"_vDSP_ctoz", h_ctoz},
    {"_ztoc", h_ztoc}, {"_vDSP_ztoc", h_ztoc}, {"_vDSP_zvcmul", h_zvcmul}, {"_vDSP_maxvi", h_maxvi},
    {"_vDSP_vclip", h_vclip}, {"_vDSP_vma", h_vma}, {"_vDSP_vramp", h_vramp},
    {"_exp", h_exp}, {"_exp2", h_exp2}, {"_exp2f", h_exp2f}, {"_log2f", h_log2f}, {"_clock", h_clock},
    {"_strncmp", h_strncmp}, {"_strpbrk", h_strpbrk}, {"_strncasecmp_l", h_strncasecmp_l},
    {"___maskrune_l", h_maskrune_l}, {"____toupper_l", h_toupper_l}, {"_newlocale", h_newlocale},
    {"_freelocale", h_noop}, {"_localtime_r", h_localtime_r}, {"_vasprintf", h_vasprintf},
    {"_dlsym", h_zero}, {"_funopen", h_zero}, {"_pthread_key_delete", h_zero},
    {"_AUGraphIsInitialized", h_isinit}, {"_sqlite3_column_int64", h_sqlite3_column_int64},
    {NULL, NULL},
};

// Everything above that caches guest addresses belongs to one engine instance:
// cleared whenever a new one is loaded (mt_reset_globals).
void mt_lion_reset(void) {
    memset(g_key_obj, 0, sizeof g_key_obj);
    g_global_queue = g_locale = 0;
    for (int i = 0; i < MAX_FFT; i++)
        if (g_fft[i]) { vDSP_destroy_fftsetup(g_fft[i]); g_fft[i] = NULL; }
}
