// C bridge to the Glint AAC-LC decoder (third_party/glint, MIT). Used instead
// of AudioToolbox because inside an audio-unit extension iOS insists on an
// out-of-process AAC decoder the extension may not connect to.
#include "../../third_party/glint/aac_decoder.hpp"
#include <cstdint>
#include <cstring>
#include <new>

extern "C" void *mtg_open(void) {
    auto *d = new (std::nothrow) glint::aac::AacDecoder();
    if (d) d->init();
    return d;
}
extern "C" void mtg_reset(void *p) { static_cast<glint::aac::AacDecoder *>(p)->init(); }
extern "C" void mtg_close(void *p) { delete static_cast<glint::aac::AacDecoder *>(p); }

// Decode one raw AAC-LC access unit (no ADTS header); returns samples per
// channel written to `out` (interleaved floats), or < 0.
extern "C" int mtg_decode(void *p, const uint8_t *au, int len, int rate, int channels, float *out) {
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
    int sri = 7;
    for (int i = 0; i < 13; i++) if (rates[i] == rate) sri = i;
    if (len < 0 || len > 8184) return -1;
    uint8_t buf[8192];
    int flen = len + 7;
    buf[0] = 0xFF;
    buf[1] = 0xF1;                                       // MPEG-4, no CRC
    buf[2] = (uint8_t)((1 << 6) | (sri << 2) | ((channels >> 2) & 1));  // profile LC (1)
    buf[3] = (uint8_t)(((channels & 3) << 6) | ((flen >> 11) & 3));
    buf[4] = (uint8_t)((flen >> 3) & 0xFF);
    buf[5] = (uint8_t)(((flen & 7) << 5) | 0x1F);
    buf[6] = 0xFC;
    memcpy(buf + 7, au, (size_t)len);
    try {
        return static_cast<glint::aac::AacDecoder *>(p)->decode_frame(buf, flen, out, nullptr);
    } catch (...) {
        return -3;
    }
}
