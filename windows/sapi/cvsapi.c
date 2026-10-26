// Classic Voices as a SAPI 5 text-to-speech engine: every voice the NVDA
// add-on can speak, for any Windows program that speaks through SAPI (JAWS,
// NVDA's SAPI5 driver, Balabolka ...). cvsapi64.dll serves 64-bit programs,
// cvsapi32.dll 32-bit ones; each registers its own CLSID and voice tokens, and
// the registry keeps the two apart (WOW64 redirects the 32-bit DLL's keys).
//
//   regsvr32 /i:"<data folder>" cvsapi64.dll   register, reading voices from there
//   regsvr32 cvsapi64.dll                      register again (rescan) with the saved folder
//   regsvr32 /u cvsapi64.dll                   remove the voices and the CLSID
//
// The data folder has the NVDA add-on's layout (Voices/, Downloads/, Native/
// with cvnative_<build>_<bits>.dll fast code); it is saved in the 64-bit view
// of HKLM\SOFTWARE\Classic Voices (DataDir), shared by both DLLs. Speech goes
// through the same stream as everywhere else (core/vxstream.c).
#define COBJMACROS
#define CONST_VTABLE
#include <initguid.h>  // instantiate sapi.h's IIDs here

#include "sapi_engine.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cv_catalog.h"
#include "cv_rules.h"
#include "vxstream.h"

void cv_set_native_dir(const char *utf8);

// {F7515027-A7BE-4BAA-88B6-8DC13ED0F733}
static const CLSID CLSID_ClassicVoicesEngine = {0xf7515027, 0xa7be, 0x4baa, {0x88, 0xb6, 0x8d, 0xc1, 0x3e, 0xd0, 0xf7, 0x33}};
#define CLSID_STR L"{F7515027-A7BE-4BAA-88B6-8DC13ED0F733}"
#define TOKEN_PREFIX L"ClassicVoices_"
#define TOKENS_KEY L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens"
#define OUR_KEY L"SOFTWARE\\Classic Voices"
#define OUT_RATE 22050
#ifndef SPERR_ALREADY_INITIALIZED
#define SPERR_ALREADY_INITIALIZED ((HRESULT)0x80045002L)
#endif

static HMODULE g_module;
static LONG g_objects;

// ---------------------------------------------------------------- small helpers

static char *utf8_of(const WCHAR *w, int len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)n + 1);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, len, s, n, NULL, NULL);
    s[n] = 0;
    return s;
}

static WCHAR *wide_of(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc((size_t)n * sizeof(WCHAR));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// CLASSICVOICES_SAPI_LOG=<file>: timings, for latency work.
static void logf_(const char *fmt, ...) {
    static int checked;
    static FILE *f;
    if (!checked) {
        const char *p = getenv("CLASSICVOICES_SAPI_LOG");
        if (p && *p) f = fopen(p, "a");
        checked = 1;
    }
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%.3f ", now_ms());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
    va_end(ap);
}

// Waiting for the engine's next samples: Sleep(1) can take a whole 15.6 ms
// timer tick, so wait on a high-resolution timer (Windows 10 1803+) instead.
static void short_wait(void) {
    static __thread HANDLE timer;
    static __thread int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -5000;  // 0.5 ms, in 100 ns units
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep(1);
}

// The data folder: HKLM\SOFTWARE\Classic Voices\DataDir (64-bit view), else
// %ProgramData%\Classic Voices.
static void data_dir(WCHAR *out, DWORD cap) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, OUR_KEY, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        DWORD type = 0, size = (cap - 1) * sizeof(WCHAR);
        LONG rc = RegQueryValueExW(k, L"DataDir", NULL, &type, (BYTE *)out, &size);
        RegCloseKey(k);
        if (rc == ERROR_SUCCESS && type == REG_SZ) {
            out[size / sizeof(WCHAR)] = 0;
            if (out[0]) return;
        }
    }
    if (!GetEnvironmentVariableW(L"ProgramData", out, cap)) wcscpy(out, L"C:\\ProgramData");
    wcsncat(out, L"\\Classic Voices", cap - wcslen(out) - 1);
}

// ---------------------------------------------------------------- the engine object

typedef struct {
    ISpTTSEngine eng;          // first: the object's identity
    ISpObjectWithToken tok;
    LONG refs;
    ISpObjectToken *token;
    VxStream *vxs;
    char key[96], fallback[96], build[16], language[24];
    char voices[MAX_PATH * 3 + 16];
} Engine;

static Engine *from_tok(ISpObjectWithToken *t) { return (Engine *)((char *)t - offsetof(Engine, tok)); }

static HRESULT STDMETHODCALLTYPE eng_QueryInterface(ISpTTSEngine *This, REFIID riid, void **ppv) {
    Engine *e = (Engine *)This;
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &CV_IID_ISpTTSEngine)) *ppv = &e->eng;
    else if (IsEqualIID(riid, &IID_ISpObjectWithToken)) *ppv = &e->tok;
    else { *ppv = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&e->refs);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE eng_AddRef(ISpTTSEngine *This) { return (ULONG)InterlockedIncrement(&((Engine *)This)->refs); }

static ULONG STDMETHODCALLTYPE eng_Release(ISpTTSEngine *This) {
    Engine *e = (Engine *)This;
    LONG r = InterlockedDecrement(&e->refs);
    if (r == 0) {
        if (e->vxs) vxs_destroy(e->vxs);
        if (e->token) ISpObjectToken_Release(e->token);
        free(e);
        InterlockedDecrement(&g_objects);
    }
    return (ULONG)r;
}

typedef struct {
    Engine *e;
    ISpTTSEngineSite *site;
    ULONGLONG bytes;  // audio written so far in this Speak call: event offsets
    double t0;
    int first;
} Run;

static int aborted(Run *r) { return (r->site->lpVtbl->GetActions(r->site) & SPVES_ABORT) != 0; }

static int write_pcm(Run *r, const int16_t *pcm, size_t n) {
    ULONG done = 0;
    if (FAILED(r->site->lpVtbl->Write(r->site, pcm, (ULONG)(n * 2), &done))) return 1;
    r->bytes += n * 2;
    return 0;
}

// The text of one engine utterance, and where each of its bytes came from in
// the text SAPI was given (characters), for word positions.
typedef struct {
    char *s;
    ULONG *src;
    size_t len, cap;
    ULONG end;  // just past the last character
} Text;

static int text_put(Text *t, char c, ULONG src) {
    if (t->len + 2 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        char *s = realloc(t->s, cap);
        if (!s) return 0;
        t->s = s;
        ULONG *m = realloc(t->src, cap * sizeof *m);
        if (!m) return 0;
        t->src = m;
        t->cap = cap;
    }
    t->src[t->len] = src;
    t->s[t->len++] = c;
    t->s[t->len] = 0;
    return 1;
}

static void text_fragment(Text *t, const SPVTEXTFRAG *f) {
    if (t->len && f->ulTextSrcOffset != t->end) text_put(t, ' ', f->ulTextSrcOffset);  // text that markup separated
    for (ULONG i = 0; i < f->ulTextLen;) {
        int n = (f->pTextStart[i] >= 0xD800 && f->pTextStart[i] < 0xDC00 && i + 1 < f->ulTextLen) ? 2 : 1;
        char b[8];
        int len = WideCharToMultiByte(CP_UTF8, 0, f->pTextStart + i, n, b, sizeof b, NULL, NULL);
        for (int j = 0; j < len; j++) text_put(t, b[j], f->ulTextSrcOffset + i);
        i += (ULONG)n;
        if (f->State.eAction == SPVA_SpellOut && i < f->ulTextLen) text_put(t, ' ', f->ulTextSrcOffset + i);  // letter by letter
    }
    t->end = f->ulTextSrcOffset + f->ulTextLen;
}

// Word boundaries. SAPI places an event where the audio written so far
// ends, so each word waits until the audio before it is written (the engine
// knows a word's position before its audio).
typedef struct {
    VxsWord w[64];
    size_t n;
    ULONG last;  // the last word sent: a word the rules rewrote into several counts once
} Words;

static void word_event(Run *r, const Text *t, Words *wq, const VxsWord *w) {
    if (w->text_pos < 0 || (size_t)w->text_pos >= t->len) return;
    size_t e = (size_t)w->text_pos + (size_t)(w->text_len > 0 ? w->text_len : 1);
    if (e > t->len) e = t->len;
    size_t last = e - 1;  // the word ends with the character its last byte belongs to
    while (last > (size_t)w->text_pos && ((unsigned char)t->s[last] & 0xC0) == 0x80) last--;
    ULONG pos = t->src[w->text_pos], end = t->src[last] + ((unsigned char)t->s[last] >= 0xF0 ? 2 : 1);
    if (pos + 1 == wq->last) return;
    wq->last = pos + 1;
    SPEVENT ev;
    memset(&ev, 0, sizeof ev);
    ev.eEventId = SPEI_WORD_BOUNDARY;
    ev.elParamType = SPET_LPARAM_IS_UNDEFINED;
    ev.ullAudioStreamOffset = r->bytes;
    ev.lParam = (LPARAM)pos;
    ev.wParam = (WPARAM)(end > pos ? end - pos : 1);
    r->site->lpVtbl->AddEvents(r->site, &ev, 1);
}

// Write n samples of the utterance that began at byte `start`, sending each
// word as the audio reaches it.
static int write_with_words(Run *r, const Text *t, Words *wq, ULONGLONG start, const int16_t *pcm, size_t n) {
    size_t done = 0;
    while (wq->n) {
        ULONGLONG at = start + (ULONGLONG)wq->w[0].frame * 2;
        if (at >= r->bytes + (n - done) * 2) break;  // later than this audio
        if (at > r->bytes) {
            size_t k = (size_t)((at - r->bytes) / 2);
            if (write_pcm(r, pcm + done, k)) return 1;
            done += k;
        }
        word_event(r, t, wq, &wq->w[0]);
        memmove(wq->w, wq->w + 1, --wq->n * sizeof wq->w[0]);
    }
    return n > done && write_pcm(r, pcm + done, n - done);
}

// One engine utterance. Returns 1 when aborted.
static int speak_text(Run *r, const Text *t, int rate, int pitch, int volume, int fallback) {
    Engine *e = r->e;
    float buf[1024];
    int16_t pcm[1024];
    ULONGLONG start = r->bytes;
    Words wq = {.n = 0};
    vxs_start_with_fallback(e->vxs, e->key, fallback && e->fallback[0] ? e->fallback : NULL, t->s, rate, pitch, volume, 0);
    for (;;) {
        int done = 0;
        if (wq.n < 64) wq.n += vxs_read_words(e->vxs, wq.w + wq.n, 64 - wq.n);  // before the audio they belong to
        size_t n = vxs_read(e->vxs, buf, 1024, &done);
        if (n) {
            for (size_t i = 0; i < n; i++) {
                float x = buf[i] * 32767.0f;
                pcm[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            }
            if (!r->first) {
                r->first = 1;
                logf_("first audio %.1f ms (%s)", now_ms() - r->t0, e->key);
            }
            if (write_with_words(r, t, &wq, start, pcm, n)) { vxs_cancel(e->vxs); return 1; }
        }
        if (aborted(r)) {
            double t = now_ms();
            vxs_cancel(e->vxs);
            logf_("aborted, stopped in %.1f ms", now_ms() - t);
            return 1;
        }
        if (done) {
            for (size_t i = 0; i < wq.n; i++) word_event(r, t, &wq, &wq.w[i]);  // past the end of the audio
            return 0;
        }
        if (!n) short_wait();
    }
}

static int silence(Run *r, ULONG ms) {
    static const int16_t zeros[1024];
    size_t left = (size_t)ms * OUT_RATE / 1000;
    while (left) {
        size_t n = left < 1024 ? left : 1024;
        if (aborted(r) || write_pcm(r, zeros, n)) return 1;
        left -= n;
    }
    return 0;
}

static void bookmark(Run *r, const SPVTEXTFRAG *f) {
    WCHAR *s = malloc(((size_t)f->ulTextLen + 1) * sizeof(WCHAR));
    if (!s) return;
    memcpy(s, f->pTextStart, (size_t)f->ulTextLen * sizeof(WCHAR));
    s[f->ulTextLen] = 0;
    SPEVENT ev;
    memset(&ev, 0, sizeof ev);
    ev.eEventId = SPEI_TTS_BOOKMARK;
    ev.elParamType = SPET_LPARAM_IS_STRING;
    ev.ullAudioStreamOffset = r->bytes;
    ev.lParam = (LPARAM)s;  // SAPI copies the string
    ev.wParam = (WPARAM)_wtol(s);
    r->site->lpVtbl->AddEvents(r->site, &ev, 1);
    free(s);
}

// SAPI's -10..10 onto the engine's percentages: rate +10 is the engine's
// fastest (400 %), -10 its slowest (50 %); pitch +-10 is an octave either way.
static int rate_pct(long adj) {
    if (adj > 10) adj = 10;
    if (adj < -10) adj = -10;
    return (int)lround(100.0 * pow(adj >= 0 ? 4.0 : 2.0, adj / 10.0));
}

static int pitch_pct(long adj) {
    if (adj > 10) adj = 10;
    if (adj < -10) adj = -10;
    return (int)lround(100.0 * pow(2.0, adj / 10.0));
}

static int same_prosody(const SPVSTATE *a, const SPVSTATE *b) {
    return a->RateAdj == b->RateAdj && a->Volume == b->Volume && a->PitchAdj.MiddleAdj == b->PitchAdj.MiddleAdj;
}

static int is_text(SPVACTIONS a) { return a == SPVA_Speak || a == SPVA_SpellOut || a == SPVA_Pronounce; }

static int ensure_stream(Engine *e) {
    if (e->vxs) return 1;
    WCHAR dir[MAX_PATH];
    data_dir(dir, MAX_PATH);
    char *d = utf8_of(dir, -1);
    if (!d) return 0;
    char downloads[MAX_PATH * 3 + 16], native[MAX_PATH * 3 + 16];
    snprintf(e->voices, sizeof e->voices, "%s\\Voices", d);
    snprintf(downloads, sizeof downloads, "%s\\Downloads", d);
    snprintf(native, sizeof native, "%s\\Native", d);
    free(d);
    static LONG native_set;
    if (!InterlockedExchange(&native_set, 1)) cv_set_native_dir(native);
    e->vxs = vxs_create(e->voices);
    if (!e->vxs) return 0;
    vxs_set_asset_root(e->vxs, downloads);
    return 1;
}

// HKCU\Software\Classic Voices: the user's settings (the app writes them).
static DWORD user_setting(const WCHAR *name, DWORD fallback) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classic Voices", name, RRF_RT_REG_DWORD, NULL, &v, &size) ==
        ERROR_SUCCESS)
        return v;
    return fallback;
}

// Apple's pronunciation rules (user_rules.txt), unless turned off. Word
// positions stay on the text SAPI gave: rewritten text maps to its original.
static void apply_rules(Engine *e, Text *t) {
    if (!user_setting(L"PronunciationRules", 1) || !e->build[0]) return;
    const CvRules *rules = cv_rules_for(e->voices, e->build, e->language);
    if (!rules) return;
    int32_t *map = NULL;
    char *s = cv_rules_apply_mapped(rules, t->s, (const int32_t *)t->src, &map);
    if (!s || !map) { free(s); free(map); return; }
    free(t->s);
    free(t->src);
    t->s = s;
    t->src = (ULONG *)map;
    t->len = t->cap = strlen(s);
}

static HRESULT STDMETHODCALLTYPE eng_Speak(ISpTTSEngine *This, DWORD flags, REFGUID fmt, const WAVEFORMATEX *wfx,
                                           const SPVTEXTFRAG *frags, ISpTTSEngineSite *site) {
    (void)flags; (void)fmt; (void)wfx;
    Engine *e = (Engine *)This;
    if (!site) return E_INVALIDARG;
    if (!e->key[0] || !ensure_stream(e)) return E_FAIL;
    Run r = {e, site, 0, now_ms(), 0};
    int fallback = (int)user_setting(L"PitchFallback", 1);  // the compact voice speaks pitch changes
    long rate = 0;
    USHORT volume = 100;
    site->lpVtbl->GetRate(site, &rate);
    site->lpVtbl->GetVolume(site, &volume);
    const SPVTEXTFRAG *f = frags;
    while (f) {
        DWORD actions = site->lpVtbl->GetActions(site);
        if (actions & SPVES_ABORT) break;
        if (actions & SPVES_SKIP) {  // sentence skipping isn't supported: report none skipped
            SPVSKIPTYPE type;
            long items = 0;
            site->lpVtbl->GetSkipInfo(site, &type, &items);
            site->lpVtbl->CompleteSkip(site, 0);
        }
        if (actions & SPVES_RATE) site->lpVtbl->GetRate(site, &rate);
        if (actions & SPVES_VOLUME) site->lpVtbl->GetVolume(site, &volume);
        if (f->State.eAction == SPVA_Bookmark) {
            bookmark(&r, f);
            f = f->pNext;
            continue;
        }
        if (f->State.eAction == SPVA_Silence) {
            if (silence(&r, f->State.SilenceMSecs)) break;
            f = f->pNext;
            continue;
        }
        if (!is_text(f->State.eAction)) { f = f->pNext; continue; }
        // Consecutive text with the same prosody is one engine utterance, so
        // the engine phrases it as a whole.
        Text text = {0};
        const SPVTEXTFRAG *first = f;
        for (; f && is_text(f->State.eAction) && same_prosody(&f->State, &first->State); f = f->pNext)
            text_fragment(&text, f);
        if (text.len) apply_rules(e, &text);
        int vol = (int)lround(80.0 * volume / 100.0 * first->State.Volume / 100.0);
        int stop = text.len && speak_text(&r, &text, rate_pct(rate + first->State.RateAdj),
                                          pitch_pct(first->State.PitchAdj.MiddleAdj), vol, fallback);
        free(text.s);
        free(text.src);
        if (stop) break;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE eng_GetOutputFormat(ISpTTSEngine *This, const GUID *target, const WAVEFORMATEX *twfx,
                                                     GUID *out_id, WAVEFORMATEX **out_wfx) {
    (void)This; (void)target; (void)twfx;
    if (!out_id || !out_wfx) return E_POINTER;
    WAVEFORMATEX *w = CoTaskMemAlloc(sizeof *w);
    if (!w) return E_OUTOFMEMORY;
    memset(w, 0, sizeof *w);
    w->wFormatTag = WAVE_FORMAT_PCM;
    w->nChannels = 1;
    w->nSamplesPerSec = OUT_RATE;
    w->wBitsPerSample = 16;
    w->nBlockAlign = 2;
    w->nAvgBytesPerSec = OUT_RATE * 2;
    *out_id = CV_SPDFID_WaveFormatEx;
    *out_wfx = w;
    return S_OK;
}

static const ISpTTSEngineVtbl kEngineVtbl = {
    eng_QueryInterface, eng_AddRef, eng_Release, eng_Speak, eng_GetOutputFormat,
};

// ---- ISpObjectWithToken: which voice this object speaks

static HRESULT STDMETHODCALLTYPE tok_QueryInterface(ISpObjectWithToken *This, REFIID riid, void **ppv) {
    return eng_QueryInterface(&from_tok(This)->eng, riid, ppv);
}
static ULONG STDMETHODCALLTYPE tok_AddRef(ISpObjectWithToken *This) { return eng_AddRef(&from_tok(This)->eng); }
static ULONG STDMETHODCALLTYPE tok_Release(ISpObjectWithToken *This) { return eng_Release(&from_tok(This)->eng); }

static void token_string(ISpObjectToken *t, const WCHAR *name, char *out, size_t cap) {
    WCHAR *w = NULL;
    out[0] = 0;
    if (SUCCEEDED(ISpObjectToken_GetStringValue(t, name, &w)) && w) {
        char *s = utf8_of(w, -1);
        if (s) snprintf(out, cap, "%s", s);
        free(s);
        CoTaskMemFree(w);
    }
}

static HRESULT STDMETHODCALLTYPE tok_SetObjectToken(ISpObjectWithToken *This, ISpObjectToken *token) {
    Engine *e = from_tok(This);
    if (!token) return E_INVALIDARG;
    if (e->token) return SPERR_ALREADY_INITIALIZED;
    ISpObjectToken_AddRef(token);
    e->token = token;
    token_string(token, L"CVKey", e->key, sizeof e->key);
    token_string(token, L"CVFallback", e->fallback, sizeof e->fallback);
    token_string(token, L"CVBuild", e->build, sizeof e->build);
    token_string(token, L"CVLanguage", e->language, sizeof e->language);
    if (!e->key[0]) return E_INVALIDARG;
    // Programs pick the voice well before they speak: load it now.
    if (ensure_stream(e)) vxs_preload(e->vxs, e->key);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE tok_GetObjectToken(ISpObjectWithToken *This, ISpObjectToken **token) {
    Engine *e = from_tok(This);
    if (!token) return E_POINTER;
    *token = e->token;
    if (e->token) ISpObjectToken_AddRef(e->token);
    return e->token ? S_OK : S_FALSE;
}

static const ISpObjectWithTokenVtbl kTokenVtbl = {
    tok_QueryInterface, tok_AddRef, tok_Release, tok_SetObjectToken, tok_GetObjectToken,
};

// ---------------------------------------------------------------- class factory

static HRESULT STDMETHODCALLTYPE cf_QueryInterface(IClassFactory *This, REFIID riid, void **ppv) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *ppv = This; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cf_AddRef(IClassFactory *This) { (void)This; return 2; }
static ULONG STDMETHODCALLTYPE cf_Release(IClassFactory *This) { (void)This; return 1; }

static HRESULT STDMETHODCALLTYPE cf_CreateInstance(IClassFactory *This, IUnknown *outer, REFIID riid, void **ppv) {
    (void)This;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    Engine *e = calloc(1, sizeof *e);
    if (!e) return E_OUTOFMEMORY;
    e->eng.lpVtbl = &kEngineVtbl;
    e->tok.lpVtbl = &kTokenVtbl;
    e->refs = 1;
    InterlockedIncrement(&g_objects);
    HRESULT hr = eng_QueryInterface(&e->eng, riid, ppv);
    eng_Release(&e->eng);
    return hr;
}

static HRESULT STDMETHODCALLTYPE cf_LockServer(IClassFactory *This, BOOL lock) { (void)This; (void)lock; return S_OK; }

static const IClassFactoryVtbl kFactoryVtbl = {
    cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance, cf_LockServer,
};
static IClassFactory g_factory = {&kFactoryVtbl};

// ---------------------------------------------------------------- DLL entry points

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv) {
    if (!IsEqualCLSID(clsid, &CLSID_ClassicVoicesEngine)) return CLASS_E_CLASSNOTAVAILABLE;
    return cf_QueryInterface(&g_factory, riid, ppv);
}

// The engine threads and any fast-code DLLs stay with the process: never unload.
HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }

static LONG set_str(HKEY k, const WCHAR *name, const WCHAR *value) {
    return RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
}

static void remove_tokens(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TOKENS_KEY, 0, KEY_READ | KEY_WRITE, &k) != ERROR_SUCCESS) return;
    WCHAR name[256];
    for (DWORD i = 0;;) {
        DWORD n = 256;
        if (RegEnumKeyExW(k, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (!wcsncmp(name, TOKEN_PREFIX, wcslen(TOKEN_PREFIX))) RegDeleteTreeW(k, name);  // the index stays put
        else i++;
    }
    RegCloseKey(k);
}

static int add_token(const CvVoice *v, const WCHAR *clsid) {
    WCHAR *lang = wide_of(v->language);
    if (!lang) return 0;
    for (WCHAR *p = lang; *p; p++) if (*p == '_') *p = '-';
    LCID lcid = LocaleNameToLCID(lang, 0);
    free(lang);
    if (!lcid) return 0;  // SAPI needs an LCID; a language Windows doesn't know can't be listed
    WCHAR path[512], lcid_hex[16];
    WCHAR *key = wide_of(v->key), *name = wide_of(v->name), *voice = wide_of(v->voice);
    WCHAR *fallback = wide_of(v->fallback), *build = wide_of(cv_voice_build(v)), *language = wide_of(v->language);
    int ok = 0;
    if (key && name && voice && fallback && build && language) {
        swprintf(path, 512, L"%ls\\%ls", TOKENS_KEY, TOKEN_PREFIX);
        size_t at = wcslen(path);
        for (const WCHAR *p = key; *p && at < 500; p++) path[at++] = (*p == '/' || *p == '\\') ? '_' : *p;
        path[at] = 0;
        swprintf(lcid_hex, 16, L"%X", (unsigned)LANGIDFROMLCID(lcid));
        HKEY t, a;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &t, NULL) == ERROR_SUCCESS) {
            set_str(t, NULL, name);
            set_str(t, lcid_hex, name);
            set_str(t, L"CLSID", clsid);
            set_str(t, L"CVKey", key);
            set_str(t, L"CVFallback", fallback);
            set_str(t, L"CVBuild", build);
            set_str(t, L"CVLanguage", language);
            if (RegCreateKeyExW(t, L"Attributes", 0, NULL, 0, KEY_WRITE, NULL, &a, NULL) == ERROR_SUCCESS) {
                set_str(a, L"Name", name);
                set_str(a, L"Gender", v->female == 1 ? L"Female" : v->female == 0 ? L"Male" : L"Neutral");
                set_str(a, L"Age", L"Adult");
                set_str(a, L"Language", lcid_hex);
                set_str(a, L"Vendor", L"Classic Voices");
                RegCloseKey(a);
                ok = 1;
            }
            RegCloseKey(t);
        }
    }
    free(key); free(name); free(voice); free(fallback); free(build); free(language);
    return ok;
}

static HRESULT register_all(void) {
    WCHAR dll[MAX_PATH], dir[MAX_PATH];
    if (!GetModuleFileNameW(g_module, dll, MAX_PATH)) return E_FAIL;
    HKEY k, ip;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" CLSID_STR, 0, NULL, 0, KEY_WRITE, NULL, &k,
                        NULL) != ERROR_SUCCESS)
        return E_ACCESSDENIED;
    set_str(k, NULL, L"Classic Voices");
    if (RegCreateKeyExW(k, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &ip, NULL) == ERROR_SUCCESS) {
        set_str(ip, NULL, dll);
        set_str(ip, L"ThreadingModel", L"Both");
        RegCloseKey(ip);
    }
    RegCloseKey(k);

    data_dir(dir, MAX_PATH);
    // The catalogue scans with the C library's ANSI calls: give it the 8.3
    // name of a folder with other characters in it (C:\Users\José\...).
    for (const WCHAR *p = dir; *p; p++) {
        if (*p > 127) {
            WCHAR shortp[MAX_PATH];
            DWORD n = GetShortPathNameW(dir, shortp, MAX_PATH);
            if (n && n < MAX_PATH) wcscpy(dir, shortp);
            break;
        }
    }
    char *d = utf8_of(dir, -1);
    if (!d) return E_OUTOFMEMORY;
    char voices[MAX_PATH * 3 + 16], downloads[MAX_PATH * 3 + 16];
    snprintf(voices, sizeof voices, "%s/Voices", d);
    snprintf(downloads, sizeof downloads, "%s/Downloads", d);
    free(d);
    CvVoice *v = NULL;
    size_t n = cv_scan_voices(voices, downloads, &v);
    remove_tokens();
    size_t added = 0;
    for (size_t i = 0; i < n; i++) added += (size_t)add_token(&v[i], CLSID_STR);
    cv_free_voices(v);
    return added ? S_OK : S_FALSE;
}

static void unregister_all(void) {
    remove_tokens();
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" CLSID_STR);
}

HRESULT WINAPI DllRegisterServer(void) { return register_all() == E_FAIL ? E_FAIL : S_OK; }

HRESULT WINAPI DllUnregisterServer(void) {
    unregister_all();
    return S_OK;
}

// regsvr32 /i:"<data folder>" cvsapiNN.dll: remember the folder, then register.
HRESULT WINAPI DllInstall(BOOL install, LPCWSTR cmdline) {
    if (!install) {
        unregister_all();
        return S_OK;
    }
    if (cmdline && *cmdline) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, OUR_KEY, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &k, NULL) !=
            ERROR_SUCCESS)
            return E_ACCESSDENIED;
        set_str(k, L"DataDir", cmdline);
        RegCloseKey(k);
    }
    return DllRegisterServer();
}
