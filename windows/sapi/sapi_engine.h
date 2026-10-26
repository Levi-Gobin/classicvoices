// The engine side of SAPI 5 (sapiddk.h), which MinGW-w64 doesn't ship: just
// what a text-to-speech engine needs, laid out exactly as the Windows SDK's
// sapiddk.h (10.0.26100). sapi.h (MinGW's) supplies SPVSTATE, SPEVENT,
// ISpObjectToken and ISpObjectWithToken.
#pragma once
#include <windows.h>
#include <sapi.h>

typedef enum SPVSKIPTYPE { SPVST_SENTENCE = 1L << 0 } SPVSKIPTYPE;

typedef enum SPVESACTIONS {
    SPVES_CONTINUE = 0,
    SPVES_ABORT = 1L << 0,
    SPVES_SKIP = 1L << 1,
    SPVES_RATE = 1L << 2,
    SPVES_VOLUME = 1L << 3
} SPVESACTIONS;

typedef struct ISpTTSEngineSite ISpTTSEngineSite;
typedef struct ISpTTSEngineSiteVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ISpTTSEngineSite *This, REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(ISpTTSEngineSite *This);
    ULONG(STDMETHODCALLTYPE *Release)(ISpTTSEngineSite *This);
    // ISpEventSink
    HRESULT(STDMETHODCALLTYPE *AddEvents)(ISpTTSEngineSite *This, const SPEVENT *pEventArray, ULONG ulCount);
    HRESULT(STDMETHODCALLTYPE *GetEventInterest)(ISpTTSEngineSite *This, ULONGLONG *pullEventInterest);
    // ISpTTSEngineSite
    DWORD(STDMETHODCALLTYPE *GetActions)(ISpTTSEngineSite *This);
    HRESULT(STDMETHODCALLTYPE *Write)(ISpTTSEngineSite *This, const void *pBuff, ULONG cb, ULONG *pcbWritten);
    HRESULT(STDMETHODCALLTYPE *GetRate)(ISpTTSEngineSite *This, long *pRateAdjust);
    HRESULT(STDMETHODCALLTYPE *GetVolume)(ISpTTSEngineSite *This, USHORT *pusVolume);
    HRESULT(STDMETHODCALLTYPE *GetSkipInfo)(ISpTTSEngineSite *This, SPVSKIPTYPE *peType, long *plNumItems);
    HRESULT(STDMETHODCALLTYPE *CompleteSkip)(ISpTTSEngineSite *This, long ulNumSkipped);
} ISpTTSEngineSiteVtbl;
struct ISpTTSEngineSite { const ISpTTSEngineSiteVtbl *lpVtbl; };

typedef struct SPVTEXTFRAG {
    struct SPVTEXTFRAG *pNext;
    SPVSTATE State;
    LPCWSTR pTextStart;
    ULONG ulTextLen;
    ULONG ulTextSrcOffset;
} SPVTEXTFRAG;

typedef struct ISpTTSEngine ISpTTSEngine;
typedef struct ISpTTSEngineVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ISpTTSEngine *This, REFIID riid, void **ppvObject);
    ULONG(STDMETHODCALLTYPE *AddRef)(ISpTTSEngine *This);
    ULONG(STDMETHODCALLTYPE *Release)(ISpTTSEngine *This);
    HRESULT(STDMETHODCALLTYPE *Speak)(ISpTTSEngine *This, DWORD dwSpeakFlags, REFGUID rguidFormatId,
                                      const WAVEFORMATEX *pWaveFormatEx, const SPVTEXTFRAG *pTextFragList,
                                      ISpTTSEngineSite *pOutputSite);
    HRESULT(STDMETHODCALLTYPE *GetOutputFormat)(ISpTTSEngine *This, const GUID *pTargetFmtId,
                                                const WAVEFORMATEX *pTargetWaveFormatEx, GUID *pOutputFormatId,
                                                WAVEFORMATEX **ppCoMemOutputWaveFormatEx);
} ISpTTSEngineVtbl;
struct ISpTTSEngine { const ISpTTSEngineVtbl *lpVtbl; };

// {A74D7C8E-4CC5-4F2F-A6EB-804DEE18500E}
static const IID CV_IID_ISpTTSEngine = {0xa74d7c8e, 0x4cc5, 0x4f2f, {0xa6, 0xeb, 0x80, 0x4d, 0xee, 0x18, 0x50, 0x0e}};
// SPDFID_WaveFormatEx {C31ADBAE-527F-4FF5-A230-F62BB61FF70C}: sapi.h declares it, no MinGW library defines it
static const GUID CV_SPDFID_WaveFormatEx = {0xc31adbae, 0x527f, 0x4ff5, {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};
