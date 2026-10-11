// AudioConverter for the MacinTalk guest, bridged to the host's AudioToolbox.
// Alex's and Vicki's sample banks are AAC; Apple's own decoder (on macOS and
// iOS alike) decodes them as the original Mac did. Engine usage and the
// big-endian source format are documented in panthera-speech's
// tiger_host_aac.c.
#include "mt_internal.h"

#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }

static int aclog(void) { static int v = -1; if (v < 0) v = getenv("MT_ACLOG") != NULL; return v; }

// AAC decoding uses Glint (mt_glint.cpp), not AudioToolbox: inside an
// audio-unit extension iOS routes AAC to an out-of-process decoder the
// extension may not use. The engine expects Apple's behaviour, which drops
// the 2112-sample codec delay at the start of each stream (after Reset).
void *mtg_open(void);
void mtg_reset(void *p);
void mtg_close(void *p);
int mtg_decode(void *p, const uint8_t *au, int len, int rate, int channels, float *out);

#define AAC_PRIMING 2112
#define MAX_CONV 64
typedef struct {
    int used;
    void *dec;
    uint32_t rate, channels, out_bits, out_float, out_be;
    uint32_t prime_left;
    float pend[4096];
    uint32_t pend_n, pend_pos;  // decoded samples not yet given (interleaved)
} Conv;
static Conv g_conv[MAX_CONV];
static pthread_mutex_t g_conv_mu = PTHREAD_MUTEX_INITIALIZER;

static void read_asbd(Mt *m, uint32_t a, AudioStreamBasicDescription *d) {
    uint8_t b[40];
    memcpy(b, G(m, a), 40);
    memcpy(d, b, 40);
    uint32_t ch = d->mChannelsPerFrame, be = __builtin_bswap32(ch);
    if ((ch < 1 || ch > 64) && be >= 1 && be <= 64) {  // big-endian, straight from the voice file
        uint8_t r[8];
        for (int i = 0; i < 8; i++) r[i] = b[7 - i];
        memcpy(&d->mSampleRate, r, 8);
        d->mFormatID = __builtin_bswap32(d->mFormatID);
        d->mFormatFlags = __builtin_bswap32(d->mFormatFlags);
        d->mBytesPerPacket = __builtin_bswap32(d->mBytesPerPacket);
        d->mFramesPerPacket = __builtin_bswap32(d->mFramesPerPacket);
        d->mBytesPerFrame = __builtin_bswap32(d->mBytesPerFrame);
        d->mChannelsPerFrame = be;
        d->mBitsPerChannel = __builtin_bswap32(d->mBitsPerChannel);
    }
}

static Conv *conv_of(Mt *m, X86 *c, uint32_t h) {
    (void)m;
    uint32_t i = h - 0x41430000u;  // handles are 'AC' << 16 | index
    if (i >= MAX_CONV || !g_conv[i].used) x86_fault(c, "bad AudioConverter %#x", h);
    return &g_conv[i];
}

static void h_AudioConverterNew(Mt *m, X86 *c) {
    AudioStreamBasicDescription in, out;
    read_asbd(m, A(c, 0), &in);
    read_asbd(m, A(c, 1), &out);
    if (in.mFormatID != kAudioFormatMPEG4AAC || out.mFormatID != kAudioFormatLinearPCM) {
        mt_log("AudioConverterNew: unsupported %.4s -> %.4s", (char *)&(uint32_t){__builtin_bswap32(in.mFormatID)}, (char *)&(uint32_t){__builtin_bswap32(out.mFormatID)});
        R(c, (uint32_t)'fmt?');
        return;
    }
    pthread_mutex_lock(&g_conv_mu);
    uint32_t i;
    for (i = 0; i < MAX_CONV && g_conv[i].used; i++) {}
    if (i < MAX_CONV) { memset(&g_conv[i], 0, sizeof g_conv[i]); g_conv[i].used = 1; }
    pthread_mutex_unlock(&g_conv_mu);
    if (i == MAX_CONV) { R(c, (uint32_t)-108); return; }
    Conv *v = &g_conv[i];
    v->dec = mtg_open();
    v->rate = (uint32_t)in.mSampleRate;
    v->channels = in.mChannelsPerFrame ? in.mChannelsPerFrame : 1;
    v->out_bits = out.mBitsPerChannel;
    v->out_float = (out.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    v->out_be = (out.mFormatFlags & kAudioFormatFlagIsBigEndian) != 0;
    v->prime_left = AAC_PRIMING * v->channels;
    if (aclog()) fprintf(stderr, "[ac] New aac %u Hz %u ch -> %u-bit %s\n", v->rate, v->channels, v->out_bits, v->out_float ? "float" : "int");
    wr32(c, A(c, 2), 0x41430000u + i);
    R(c, 0);
}
static void h_AudioConverterDispose(Mt *m, X86 *c) {
    Conv *v = conv_of(m, c, A(c, 0));
    if (v->dec) mtg_close(v->dec);
    v->dec = NULL;
    v->used = 0;
    R(c, 0);
}
static void h_AudioConverterReset(Mt *m, X86 *c) {
    Conv *v = conv_of(m, c, A(c, 0));
    if (aclog()) fprintf(stderr, "[ac] Reset\n");
    mtg_reset(v->dec);
    v->prime_left = AAC_PRIMING * v->channels;
    v->pend_n = v->pend_pos = 0;
    R(c, 0);
}
// Properties: the cookie and prime method need nothing (Glint reads the
// configuration from the ADTS header we build; priming is always trimmed).
static void h_AudioConverterSetProperty(Mt *m, X86 *c) { conv_of(m, c, A(c, 0)); R(c, 0); }
static void h_AudioConverterGetProperty(Mt *m, X86 *c) { conv_of(m, c, A(c, 0)); if (A(c, 2)) wr32(c, A(c, 2), 0); R(c, (uint32_t)'prop'); }
static void h_AudioConverterGetPropertyInfo(Mt *m, X86 *c) {
    conv_of(m, c, A(c, 0));
    if (A(c, 2)) wr32(c, A(c, 2), 0);
    if (A(c, 3)) wr8(c, A(c, 3), 0);
    R(c, (uint32_t)'prop');
}
static void h_AudioFormatGetProperty(Mt *m, X86 *c) {
    uint32_t sel = A(c, 0), sizep = A(c, 3), out = A(c, 4);
    (void)m;
    if (sel == 0x66657866u) {  // 'fexf' FormatIsExternallyFramed: AAC is
        if (out && sizep && rd32(c, sizep) >= 4) wr32(c, out, 1);
        if (sizep) wr32(c, sizep, 4);
        R(c, 0);
        return;
    }
    if (sizep) wr32(c, sizep, 0);
    R(c, (uint32_t)'prop');
}

// FillComplexBuffer(conv, inputProc, inUserData, ioOutputDataPacketSize*, outOutputData*, outPacketDescription*)
static void h_AudioConverterFillComplexBuffer(Mt *m, X86 *c) {
    Conv *v = conv_of(m, c, A(c, 0));
    uint32_t proc = A(c, 1), udata = A(c, 2), packetsp = A(c, 3), abl = A(c, 4);
    uint32_t want = rd32(c, packetsp) * v->channels;  // samples (interleaved)
    uint32_t data = rd32(c, abl + 12), cap = rd32(c, abl + 8);
    uint32_t bps = v->out_float ? 4 : v->out_bits / 8;
    if (!bps) bps = 2;
    if (want * bps > cap) want = cap / bps;
    mt_check(m, c, data, want * bps);
    uint8_t *dst = G(m, data);
    uint32_t got = 0, scratch = mt_malloc(m, 64);
    OSStatus st = 0;
    while (got < want) {
        if (v->pend_pos >= v->pend_n) {  // ask the engine for another packet
            v->pend_n = v->pend_pos = 0;
            wr32(c, scratch, 1);
            wr32(c, scratch + 4, 0);
            wr32(c, scratch + 8, 1);
            wr32(c, scratch + 12, v->channels);
            wr32(c, scratch + 16, 0);
            wr32(c, scratch + 20, 0);
            uint32_t args[5] = {A(c, 0), scratch, scratch + 8, scratch + 4, udata};
            st = (OSStatus)mt_call_on(m, c, proc, 5, args);
            uint32_t npk = rd32(c, scratch), bytes = rd32(c, scratch + 16), p = rd32(c, scratch + 20), descs = rd32(c, scratch + 4);
            if (st || !npk || !p || !bytes) break;  // no more data (or the engine's error)
            mt_check(m, c, p, bytes);
            for (uint32_t k = 0, off = 0; k < npk; k++) {
                uint32_t sz = descs ? rd32(c, descs + 16 * k + 12) : bytes;
                uint32_t o = descs ? rd32(c, descs + 16 * k) : off;  // mStartOffset (low word)
                if (o + sz > bytes) break;
                float pcm[2048];
                int n = mtg_decode(v->dec, G(m, p + o), (int)sz, (int)v->rate, (int)v->channels, pcm);
                off = o + sz;
                if (n <= 0) continue;
                uint32_t ns = (uint32_t)n * v->channels, skip = ns < v->prime_left ? ns : v->prime_left;
                v->prime_left -= skip;
                if (v->pend_n + ns - skip > sizeof v->pend / sizeof v->pend[0]) break;
                memcpy(v->pend + v->pend_n, pcm + skip, (ns - skip) * sizeof(float));
                v->pend_n += ns - skip;
            }
            continue;
        }
        while (got < want && v->pend_pos < v->pend_n) {
            float f = v->pend[v->pend_pos++];
            if (v->out_float) memcpy(dst + 4 * got, &f, 4);
            else {
                float s = f * 32768.0f;
                int16_t q = s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : (int16_t)lrintf(s);
                if (v->out_be) q = (int16_t)__builtin_bswap16((uint16_t)q);
                memcpy(dst + 2 * got, &q, 2);
            }
            got++;
        }
    }
    mt_free(m, scratch);
    wr32(c, packetsp, got / v->channels);
    wr32(c, abl + 8, got * bps);
    if (aclog()) fprintf(stderr, "[ac] Fill want %u got %u\n", want, got);
    R(c, 0);
}

// ---------------------------------------------------------------- SoundConverter (Tiger)
// Tiger's Vicki: Sound Manager's SoundConverter with an extended (VBR) sound
// data record per AAC unit. Layout (i386): flags, format, numChannels(16),
// sampleSize(16), sampleRate(Fixed), sampleCount, buffer, reserved,
// recordSize, extendedFlags, bufferSize, frameCount, frameSizes.

#define kExtendedSoundData (1 << 14)
static struct {
    uint32_t channels, rate;
    uint8_t esds[256];
    uint32_t esds_len;
    int16_t *pcm;
    uint32_t pcm_n, pcm_pos, pcm_cap;
    uint32_t out_be;  // engine wants big-endian 16-bit ('twos')
} g_sc;

static void h_SoundConverterOpen(Mt *m, X86 *c) {
    uint32_t in = A(c, 0), out = A(c, 1);
    if (in) {
        uint16_t ch = rd16(c, in + 8);
        g_sc.channels = ch ? ch : 1;
        g_sc.rate = rd32(c, in + 12) >> 16;
        if (!g_sc.rate) g_sc.rate = 22050;
    }
    g_sc.out_be = out && rd32(c, out + 4) == 0x74776F73u;  // 'twos'
    if (aclog()) fprintf(stderr, "[snd] Open %u Hz %u ch, out %.4s\n", g_sc.rate, g_sc.channels, out ? (char *)&(uint32_t){__builtin_bswap32(rd32(c, out + 4))} : "?");
    (void)m;
    if (A(c, 2)) wr32(c, A(c, 2), 0x534E4443u);
    R(c, 0);
}
// 'wave' (siDecompressionParams): a QuickTime atom list containing 'esds';
// its payload (after version/flags) is the magic cookie AudioToolbox wants.
static void h_SoundConverterSetInfo(Mt *m, X86 *c) {
    uint32_t sel = A(c, 1), data = A(c, 2);
    if (sel == 0x77617665u && data) {
        mt_check(m, c, data, 256);
        const uint8_t *p = G(m, data);
        for (int i = 0; i + 8 < 256; i++)
            if (!memcmp(p + i + 4, "esds", 4)) {
                uint32_t len = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 | (uint32_t)p[i + 2] << 8 | p[i + 3];
                if (len > 12 && len - 12 <= sizeof g_sc.esds && i + len <= 256) {
                    memcpy(g_sc.esds, p + i + 12, len - 12);
                    g_sc.esds_len = len - 12;
                }
                break;
            }
        if (aclog()) fprintf(stderr, "[snd] SetInfo wave: esds %u bytes\n", g_sc.esds_len);
    }
    R(c, 0);
}
static void h_SoundConverterBeginConversion(Mt *m, X86 *c) { (void)m; g_sc.pcm_n = g_sc.pcm_pos = 0; R(c, 0); }
static void h_ok0(Mt *m, X86 *c) { (void)m; R(c, 0); }
static void h_upp_identity(Mt *m, X86 *c) { (void)m; R(c, A(c, 0)); }

typedef struct { const uint8_t *data; const uint32_t *sizes; uint32_t n, next, off; AudioStreamPacketDescription d; } UnitFeed;
static OSStatus unit_input(AudioConverterRef ac, UInt32 *ioPackets, AudioBufferList *io, AudioStreamPacketDescription **desc, void *ud) {
    (void)ac;
    UnitFeed *f = ud;
    if (f->next >= f->n) { *ioPackets = 0; io->mBuffers[0].mDataByteSize = 0; io->mBuffers[0].mData = NULL; return 0; }
    uint32_t sz = f->sizes[f->next];
    io->mBuffers[0].mData = (void *)(f->data + f->off);
    io->mBuffers[0].mDataByteSize = sz;
    f->d.mStartOffset = 0;
    f->d.mVariableFramesInPacket = 0;
    f->d.mDataByteSize = sz;
    if (desc) *desc = &f->d;
    *ioPackets = 1;
    f->off += sz;
    f->next++;
    return 0;
}

static void decode_unit(Mt *m, X86 *c, uint32_t rec) {
    uint32_t buffer = rd32(c, rec + 20), frames = rd32(c, rec + 40), sizes = rd32(c, rec + 44);
    if (!buffer || !sizes || !frames || frames > 100000) return;
    static void *dec;
    if (!dec) dec = mtg_open();
    mtg_reset(dec);  // every unit is its own stream
    uint32_t ch = g_sc.channels ? g_sc.channels : 1;
    uint32_t cap = frames * 1024 * ch;
    if (g_sc.pcm_cap < cap) { g_sc.pcm_cap = cap; g_sc.pcm = realloc(g_sc.pcm, cap * 2); }
    uint32_t n = 0, off = 0, prime = AAC_PRIMING * ch;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t sz = rd32(c, sizes + 4 * i);
        mt_check(m, c, buffer + off, sz);
        float pcm[2048];
        int got = mtg_decode(dec, G(m, buffer + off), (int)sz, (int)g_sc.rate, (int)ch, pcm);
        off += sz;
        if (got <= 0) continue;
        for (uint32_t k = 0; k < (uint32_t)got * ch; k++) {
            if (prime) { prime--; continue; }
            float s = pcm[k] * 32768.0f;
            int16_t q = s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : (int16_t)lrintf(s);
            g_sc.pcm[n++] = g_sc.out_be ? (int16_t)__builtin_bswap16((uint16_t)q) : q;
        }
    }
    g_sc.pcm_n = n;
    g_sc.pcm_pos = 0;
    if (aclog()) fprintf(stderr, "[snd] unit: %u frames -> %u samples (expected %u)\n", frames, n / ch, frames * 1024 - AAC_PRIMING);
}

// SoundConverterFillBuffer(sc, upp, refCon, outPtr, outBytes, *actualBytes, *actualFrames, *outFlags)
static void h_SoundConverterFillBuffer(Mt *m, X86 *c) {
    uint32_t upp = A(c, 1), refcon = A(c, 2), outp = A(c, 3), outbytes = A(c, 4);
    if (g_sc.pcm_pos >= g_sc.pcm_n) {
        g_sc.pcm_n = g_sc.pcm_pos = 0;
        if (upp) {
            uint32_t slot = mt_malloc(m, 4);
            wr32(c, slot, 0);
            uint32_t args[2] = {slot, refcon};
            uint32_t more = mt_call_on(m, c, upp, 2, args) & 0xFF;
            uint32_t rec = rd32(c, slot);
            mt_free(m, slot);
            if (aclog()) fprintf(stderr, "[snd] fill -> more %u rec %#x flags %#x recsize %u frames %u\n", more, rec, rec ? rd32(c, rec) : 0, rec ? rd32(c, rec + 28) : 0, rec ? rd32(c, rec + 40) : 0);
            if (more && rec && (rd32(c, rec) & kExtendedSoundData)) decode_unit(m, c, rec);
        }
    }
    uint32_t give = 0;
    if (g_sc.pcm_pos < g_sc.pcm_n) {
        give = (g_sc.pcm_n - g_sc.pcm_pos) * 2;
        if (give > outbytes) give = outbytes & ~1u;
        mt_check(m, c, outp, give);
        memcpy(G(m, outp), g_sc.pcm + g_sc.pcm_pos, give);
        g_sc.pcm_pos += give / 2;
    }
    if (A(c, 5)) wr32(c, A(c, 5), give);
    if (A(c, 6)) wr32(c, A(c, 6), give / 2 / (g_sc.channels ? g_sc.channels : 1));
    if (A(c, 7)) wr32(c, A(c, 7), g_sc.pcm_pos < g_sc.pcm_n ? 2 : 0);  // kSoundConverterHasLeftOverData
    R(c, 0);
}
static void h_SoundConverterEnd(Mt *m, X86 *c) {
    (void)m;
    if (A(c, 2)) wr32(c, A(c, 2), 0);
    if (A(c, 3)) wr32(c, A(c, 3), 0);
    R(c, 0);
}
static void h_SoundConverterConvertBuffer(Mt *m, X86 *c) {
    (void)m;
    if (A(c, 4)) wr32(c, A(c, 4), 0);
    if (A(c, 5)) wr32(c, A(c, 5), 0);
    R(c, 0);
}

void mt_aac_reset(void) {
    for (int i = 0; i < MAX_CONV; i++) if (g_conv[i].used) { if (g_conv[i].dec) mtg_close(g_conv[i].dec); memset(&g_conv[i], 0, sizeof g_conv[i]); }
}

const MtHleEntry mt_hle_aac[] = {
    {"_SoundConverterOpen", h_SoundConverterOpen}, {"_SoundConverterSetInfo", h_SoundConverterSetInfo},
    {"_SoundConverterBeginConversion", h_SoundConverterBeginConversion}, {"_SoundConverterClose", h_ok0},
    {"_SoundConverterFillBuffer", h_SoundConverterFillBuffer}, {"_SoundConverterEndConversion", h_SoundConverterEnd},
    {"_SoundConverterConvertBuffer", h_SoundConverterConvertBuffer},
    {"_NewSoundConverterFillBufferDataUPP", h_upp_identity}, {"_DisposeSoundConverterFillBufferDataUPP", h_ok0},
    {"_AudioConverterNew", h_AudioConverterNew}, {"_AudioConverterDispose", h_AudioConverterDispose},
    {"_AudioConverterReset", h_AudioConverterReset}, {"_AudioConverterSetProperty", h_AudioConverterSetProperty},
    {"_AudioConverterGetProperty", h_AudioConverterGetProperty},
    {"_AudioConverterGetPropertyInfo", h_AudioConverterGetPropertyInfo},
    {"_AudioFormatGetProperty", h_AudioFormatGetProperty},
    {"_AudioConverterFillComplexBuffer", h_AudioConverterFillComplexBuffer},
    {NULL, NULL},
};
