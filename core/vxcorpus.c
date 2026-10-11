// Decodes the whole speech corpus (SPCH chunk) of an enhanced Vocalizer voice
// font (synth_<voice>_full_155mrf22_*.dat) to 16-bit PCM by running the
// engine's own smc155mrf22 decoder in the Thumb interpreter.
//
//   vxcorpus <voiced> <lang_dir> <common_dir> <synth.dat> <out.raw> [max_frames]
//
// The voice font is a RIFF-like container ("RIF4"/"VFNT") whose chunk tags and
// text are XORed with 0xA9. CODR holds the decoder configuration (4 u16, then
// the LSP codebooks), SPCH holds fixed-size coded frames (49 bytes = 320
// samples at 22050 Hz for bit-rate mode 3). The unit table (UNIT) indexes that
// stream in samples; it is not needed to reconstruct the audio itself.
#include "vxengine.c"  // needs the guest heap and the Cpu
#include <time.h>
static double now_sec(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

#ifdef VX_HAVE_NATIVE
extern const NativeEntry vx_native_ios6[];
extern const size_t vx_native_ios6_count;
#endif

// Decoders in iOS 6.1.6 voiced. Both share the same spi calling convention.
typedef struct { const char *coder; uint32_t open, process, getinfo; uint16_t hdr_magic; int mode_mask; } Codec;
static const Codec kCodecs[] = {
    // 27 kbps CELP: 49-byte frames of 320 samples (mode 3), LSP codebooks in CODR
    { "155mrf22", 0xB642Cu, 0xB66E4u, 0xB6D04u, 0x0FA5, 3 },
    // "vssq5" = smcadpcmf5: 5-bit ADPCM, 3 + 5*N/8 bytes per N-sample frame (N from CODR)
    { "vssq5", 0xC538Cu, 0xC5480u, 0xC553Cu, 0x2710, 0xFFFF },
};
#define CODEC_MAGIC        0x00102102u

typedef struct { const uint8_t *p; uint32_t len; } Chunk;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int find_chunk(const uint8_t *d, size_t n, const char *name, Chunk *out) {
    uint8_t t[4];
    for (int i = 0; i < 4; i++) t[i] = (uint8_t)(name[i] ^ 0xA9);
    size_t off = 12;  // skip RIF4 header + form
    while (off + 8 <= n) {
        uint32_t len = rd32(d + off + 4);
        if (!memcmp(d + off, t, 4)) { out->p = d + off + 8; out->len = len; return 0; }
        off = (off + 8 + len + 3) & ~(size_t)3;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <voiced> <lang_dir> <common_dir> <synth.dat> <out.raw> [max_frames]\n", argv[0]);
        return 2;
    }
    long max_frames = argc > 6 ? atol(argv[6]) : 0;

    // Read the voice font.
    FILE *f = fopen(argv[4], "rb");
    if (!f) { perror(argv[4]); return 1; }
    fseek(f, 0, SEEK_END);
    size_t fsz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *font = malloc(fsz);
    if (fread(font, 1, fsz, f) != fsz) { perror("read"); return 1; }
    fclose(f);
    if (fsz < 12 || (font[0] ^ 0xA9) != 'R' || (font[8] ^ 0xA9) != 'V') { fprintf(stderr, "not a RIF4/VFNT voice font\n"); return 1; }
    Chunk vinf, codr, spch;
    if (find_chunk(font, fsz, "VINF", &vinf) || find_chunk(font, fsz, "CODR", &codr) || find_chunk(font, fsz, "SPCH", &spch)) {
        fprintf(stderr, "missing VINF/CODR/SPCH chunk\n");
        return 1;
    }
    // VINF: NUL-separated key/value pairs (XORed).
    char info[1024] = {0};
    for (uint32_t i = 0; i < vinf.len && i < sizeof info - 1; i++) { char c = (char)(vinf.p[i] ^ 0xA9); info[i] = c ? c : ' '; }
    fprintf(stderr, "font: %s\n", info);
    const Codec *codec = NULL;
    for (size_t i = 0; i < sizeof kCodecs / sizeof *kCodecs; i++) {
        char key[64];
        snprintf(key, sizeof key, "Coder %s ", kCodecs[i].coder);
        if (strstr(info, key)) codec = &kCodecs[i];
    }
    if (!codec) { fprintf(stderr, "unsupported coder\n"); return 1; }

    uint16_t cfg[4] = {0};
    for (uint32_t i = 0; i < 4 && 2 * i + 1 < codr.len; i++) cfg[i] = (uint16_t)(codr.p[2 * i] | codr.p[2 * i + 1] << 8);
    if (cfg[0] != codec->hdr_magic) { fprintf(stderr, "bad CODR header %#x\n", cfg[0]); return 1; }
    uint32_t mode = cfg[1] & codec->mode_mask;
    uint32_t cb_len = codr.len > 8 ? codr.len - 8 : 0;
    fprintf(stderr, "codr: %s mode %u, overhead frames %u, codebooks %u bytes; spch: %u bytes\n", codec->coder, mode, cfg[2], cb_len, spch.len);

    // Bring up the emulated engine.
    char err[512];
    Vx *vx = vx_open(argv[1], argv[2], argv[3], err, sizeof err);
    if (!vx) { fprintf(stderr, "open failed: %s\n", err); return 1; }
#ifdef VX_HAVE_NATIVE
    if (!getenv("VX_INTERP")) vx_set_native(vx, vx_native_ios6, vx_native_ios6_count);
#endif
    jmp_buf jb;
    vx->cpu.fault_jmp = &jb;
    if (setjmp(jb)) { fprintf(stderr, "guest fault: %s\n", vx->cpu.fault_msg); return 1; }

    // Ask the codec for its frame geometry.
    uint32_t ginfo = new_zeroed(vx, 0x24);
    uint32_t gargs[6] = { CODEC_MAGIC, cfg[0], mode, 0, 0, ginfo };
    int rc = (int)cpu_call(&vx->cpu, codec->getinfo, gargs, 6, NULL);
    if (rc) { fprintf(stderr, "GetInfo failed %#x\n", rc); return 1; }
    uint32_t bps = g32(&vx->cpu.k, ginfo), delay = g32(&vx->cpu.k, ginfo + 8);
    uint16_t frame_bytes = g16(&vx->cpu.k, ginfo + 0x16), out_bytes = g16(&vx->cpu.k, ginfo + 0x14);
    uint16_t state_size = g16(&vx->cpu.k, ginfo + 0x1e), scratch_size = g16(&vx->cpu.k, ginfo + 0x20);
    uint32_t frames = spch.len / frame_bytes;
    fprintf(stderr, "codec: %u bps, %u bytes/frame -> %u samples, delay %u samples, state %u, scratch %u; %u frames (%.1f min)\n",
            bps, frame_bytes, out_bytes / 2, delay, state_size, scratch_size, frames, frames * (out_bytes / 2) / 22050.0 / 60);
    if (spch.len % frame_bytes) fprintf(stderr, "warning: SPCH has %u trailing bytes\n", spch.len % frame_bytes);
    if (max_frames > 0 && (uint32_t)max_frames < frames) frames = (uint32_t)max_frames;

    // Guest buffers.
    uint32_t cb = heap_alloc(vx, cb_len + 16);
    memcpy(vx->mem + cb, codr.p + 8, cb_len);
    uint32_t state = new_zeroed(vx, state_size + 16), scratch = new_zeroed(vx, scratch_size + 16);
    uint32_t out = new_zeroed(vx, out_bytes), handle = new_zeroed(vx, 8), vars = new_zeroed(vx, 16);
    const uint32_t CHUNK = 4096;
    uint32_t inbuf = heap_alloc(vx, CHUNK * frame_bytes);

    uint32_t oargs[8] = { CODEC_MAGIC, mode, 0, 0, cb, cb_len, state, handle };
    rc = (int)cpu_call(&vx->cpu, codec->open, oargs, 8, NULL);
    if (rc) { fprintf(stderr, "DecoderOpen failed %#x\n", rc); return 1; }
    uint32_t h0 = g32(&vx->cpu.k, handle), h1 = g32(&vx->cpu.k, handle + 4);

    FILE *o = fopen(argv[5], "wb");
    if (!o) { perror(argv[5]); return 1; }
    int16_t *pcm = malloc(out_bytes);
    uint64_t total = 0, skip = delay;
    double t0 = now_sec();
    for (uint32_t base = 0; base < frames; base += CHUNK) {
        uint32_t n = frames - base < CHUNK ? frames - base : CHUNK;
        memcpy(vx->mem + inbuf, spch.p + (size_t)base * frame_bytes, (size_t)n * frame_bytes);
        for (uint32_t i = 0; i < n; i++) {
            s16(&vx->cpu.k, vars, frame_bytes);
            s16(&vx->cpu.k, vars + 4, out_bytes);
            uint32_t pargs[10] = { h0, h1, 0, inbuf + i * frame_bytes, vars, scratch, out, vars + 4, 0, 0 };
            rc = (int)cpu_call(&vx->cpu, codec->process, pargs, 10, NULL);
            uint32_t got = g16(&vx->cpu.k, vars + 4);
            if (rc || got != out_bytes) { fprintf(stderr, "frame %u: rc %#x, %u bytes\n", base + i, rc, got); return 1; }
            memcpy(pcm, vx->mem + out, out_bytes);
            uint32_t ns = out_bytes / 2, s0 = 0;
            if (skip) { s0 = skip < ns ? (uint32_t)skip : ns; skip -= s0; }
            fwrite(pcm + s0, 2, ns - s0, o);
            total += ns - s0;
        }
        fprintf(stderr, "\r%u / %u frames (%.1f%%), %.1fx realtime", base + n, frames, 100.0 * (base + n) / frames,
                total / 22050.0 / (now_sec() - t0));
    }
    fclose(o);
    fprintf(stderr, "\nwrote %llu samples (%.2f s) to %s\n", (unsigned long long)total, total / 22050.0, argv[5]);
    return 0;
}
