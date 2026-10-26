// Test client for the SAPI engine, through SAPI itself (ISpVoice), so it works
// on a machine without an audio device: speech goes to WAV files.
//   cvsapitest list
//   cvsapitest speak <voice name part> <out.wav> <text>      (SAPI XML allowed)
//   cvsapitest bench <voice name part> <out dir>             first audio and interrupts
//   cvsapitest register <cvsapiNN.dll> [data folder]          what regsvr32 /n /i: does, for a
//   cvsapitest unregister <cvsapiNN.dll>                      DLL regsvr32 can't load (x64 on Arm)
// Set CLASSICVOICES_SAPI_LOG=<file> to have the engine log its own timings.
#define COBJMACROS
#define CONST_VTABLE
#include <initguid.h>

#include <windows.h>
#include <sapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const GUID kWaveFormatEx = {0xc31adbae, 0x527f, 0x4ff5, {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static WCHAR *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc((size_t)n * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void print_w(const WCHAR *w) {
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, NULL, NULL);
    fputs(buf, stdout);
}

// The first voice token whose name contains `part` (any vendor if part is NULL: list them all).
static ISpObjectToken *find_voice(const WCHAR *part, int list) {
    ISpObjectTokenCategory *cat = NULL;
    IEnumSpObjectTokens *en = NULL;
    ISpObjectToken *found = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL, &IID_ISpObjectTokenCategory,
                                (void **)&cat)))
        return NULL;
    ISpObjectTokenCategory_SetId(cat, SPCAT_VOICES, FALSE);
    if (SUCCEEDED(ISpObjectTokenCategory_EnumTokens(cat, NULL, NULL, &en))) {
        ISpObjectToken *t;
        ULONG got;
        while (IEnumSpObjectTokens_Next(en, 1, &t, &got) == S_OK && got) {
            WCHAR *name = NULL, *vendor = NULL;
            ISpObjectToken_GetStringValue(t, NULL, &name);
            ISpDataKey *attrs = NULL;
            if (SUCCEEDED(ISpObjectToken_OpenKey(t, L"Attributes", &attrs))) {
                ISpDataKey_GetStringValue(attrs, L"Vendor", &vendor);
                ISpDataKey_Release(attrs);
            }
            if (list) {
                print_w(name ? name : L"?");
                fputs("  [", stdout);
                print_w(vendor ? vendor : L"");
                fputs("]\n", stdout);
            }
            int ours = vendor && !wcscmp(vendor, L"Classic Voices");
            if (!found && part && ours && name && wcsstr(name, part)) {
                found = t;
                ISpObjectToken_AddRef(t);
            }
            CoTaskMemFree(name);
            CoTaskMemFree(vendor);
            ISpObjectToken_Release(t);
        }
        IEnumSpObjectTokens_Release(en);
    }
    ISpObjectTokenCategory_Release(cat);
    return found;
}

static ISpStream *wav_out(ISpVoice *v, const WCHAR *path) {
    ISpStream *s = NULL;
    WAVEFORMATEX w = {WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
    if (FAILED(CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_ALL, &IID_ISpStream, (void **)&s))) return NULL;
    if (FAILED(ISpStream_BindToFile(s, path, SPFM_CREATE_ALWAYS, &kWaveFormatEx, &w, 0))) {
        ISpStream_Release(s);
        return NULL;
    }
    ISpVoice_SetOutput(v, (IUnknown *)s, TRUE);
    return s;
}

static void close_out(ISpVoice *v, ISpStream *s) {
    ISpVoice_SetOutput(v, NULL, TRUE);
    if (s) {
        ISpStream_Close(s);
        ISpStream_Release(s);
    }
}

static int speak_file(ISpVoice *v, const WCHAR *path, const WCHAR *text, DWORD flags) {
    ISpStream *s = wav_out(v, path);
    if (!s) return 1;
    double t0 = now_ms();
    HRESULT hr = ISpVoice_Speak(v, text, flags, NULL);
    printf("speak hr=0x%08lx in %.1f ms (t0 %.3f)\n", (unsigned long)hr, now_ms() - t0, t0);
    close_out(v, s);
    return FAILED(hr);
}

static void bench(ISpVoice *v, const WCHAR *dir) {
    static const WCHAR *phrases[] = {L"Back.", L"Settings. Button.", L"Messages, 3 new items.",
                                     L"Email from john.appleseed@icloud.com: Re: Lunch on Tuesday?"};
    WCHAR path[600];
    swprintf(path, 600, L"%ls\\warm.wav", dir);
    speak_file(v, path, L"Hello.", SPF_DEFAULT);
    for (int p = 0; p < 4; p++) {
        for (int r = 0; r < 3; r++) {
            swprintf(path, 600, L"%ls\\p%d_%d.wav", dir, p, r);
            printf("phrase %d: ", p);
            speak_file(v, path, phrases[p], SPF_DEFAULT);
        }
    }
    // interrupt: start a long one, then purge it with a short one
    for (int r = 0; r < 3; r++) {
        swprintf(path, 600, L"%ls\\interrupt%d.wav", dir, r);
        ISpStream *s = wav_out(v, path);
        ISpVoice_Speak(v, L"It is 4:15 PM on Saturday, October 10th, 2026. Battery 85 percent. Wi-Fi, three of three "
                          L"bars. Messages, 3 new items. Double tap to open.", SPF_ASYNC, NULL);
        Sleep(300);
        double t0 = now_ms();
        printf("interrupt at %.3f\n", t0);
        ISpVoice_Speak(v, L"Settings. Button.", SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);
        ISpVoice_WaitUntilDone(v, 10000);
        printf("interrupt %d: both done %.1f ms after the interrupt\n", r, now_ms() - t0);
        close_out(v, s);
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: cvsapitest list | speak <voice> <out.wav> <text> | bench <voice> <dir>\n");
        return 2;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (!strcmp(argv[1], "register") || !strcmp(argv[1], "unregister")) {
        if (argc < 3) return 2;
        // as COM loads an in-process server: its own folder is searched for what it needs
        HMODULE m = LoadLibraryExW(wide(argv[2]), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) {
            fprintf(stderr, "can't load %s (%lu)\n", argv[2], GetLastError());
            return 1;
        }
        HRESULT(WINAPI * install)(BOOL, LPCWSTR) = (void *)GetProcAddress(m, "DllInstall");
        if (!install) {
            fprintf(stderr, "no DllInstall in %s (%lu)\n", argv[2], GetLastError());
            return 1;
        }
        HRESULT hr = install(!strcmp(argv[1], "register"), argc > 3 ? wide(argv[3]) : L"");
        printf("%s hr=0x%08lx\n", argv[1], (unsigned long)hr);
        return FAILED(hr);
    }
    if (!strcmp(argv[1], "list")) {
        find_voice(NULL, 1);
        return 0;
    }
    if (argc < 4) return 2;
    WCHAR *part = wide(argv[2]);
    ISpObjectToken *t = find_voice(part, 0);
    if (!t) {
        fprintf(stderr, "no Classic Voices voice matching %s\n", argv[2]);
        return 1;
    }
    ISpVoice *v = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void **)&v))) return 1;
    double t0 = now_ms();
    HRESULT hr = ISpVoice_SetVoice(v, t);
    printf("SetVoice hr=0x%08lx in %.1f ms\n", (unsigned long)hr, now_ms() - t0);
    if (!strcmp(argv[1], "speak") && argc >= 5) {
        WCHAR *out = wide(argv[3]), *text = wide(argv[4]);
        return speak_file(v, out, text, SPF_IS_XML);
    }
    if (!strcmp(argv[1], "bench")) {
        WCHAR *dir = wide(argv[3]);
        CreateDirectoryW(dir, NULL);
        bench(v, dir);
        return 0;
    }
    return 2;
}
