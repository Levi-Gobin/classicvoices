// Cross-check for vxcorpus: decodes single units the way the engine does
// (DecoderResetMemory, then from 6 frames before the unit) and compares them
// with the continuously decoded stream.
//   vxcorpus_check <voiced> <lang_dir> <common_dir> <synth.dat> <stream.raw> off:len ...
#include "vxengine.c"
#ifdef VX_HAVE_NATIVE
extern const NativeEntry vx_native_ios6[];
extern const size_t vx_native_ios6_count;
#endif
#define FN_DECODER_OPEN    0xB642Cu
#define FN_DECODER_PROCESS 0xB66E4u
#define FN_DECODER_RESET   0xB65A0u
#define CODEC_MAGIC        0x00102102u
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static const uint8_t *find_chunk(const uint8_t *d, size_t n, const char *name, uint32_t *len) {
    size_t off = 12;
    while (off + 8 <= n) {
        uint32_t l = rd32(d + off + 4);
        if ((d[off] ^ 0xA9) == name[0] && (d[off+1] ^ 0xA9) == name[1] && (d[off+2] ^ 0xA9) == name[2] && (d[off+3] ^ 0xA9) == name[3]) { *len = l; return d + off + 8; }
        off = (off + 8 + l + 3) & ~(size_t)3;
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage\n"); return 2; }
    FILE *f = fopen(argv[4], "rb"); fseek(f, 0, SEEK_END); size_t fsz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *font = malloc(fsz); fread(font, 1, fsz, f); fclose(f);
    uint32_t codr_len, spch_len;
    const uint8_t *codr = find_chunk(font, fsz, "CODR", &codr_len), *spch = find_chunk(font, fsz, "SPCH", &spch_len);
    uint32_t mode = (codr[2] | codr[3] << 8) & 3;
    const uint32_t FB = 49, NS = 320, DELAY = 40, OVER = getenv("VX_OVER") ? (uint32_t)atoi(getenv("VX_OVER")) : (codr[4] | codr[5] << 8);
    FILE *sf = fopen(argv[5], "rb");
    char err[512];
    Vx *vx = vx_open(argv[1], argv[2], argv[3], err, sizeof err);
    if (!vx) { fprintf(stderr, "open failed: %s\n", err); return 1; }
#ifdef VX_HAVE_NATIVE
    vx_set_native(vx, vx_native_ios6, vx_native_ios6_count);
#endif
    jmp_buf jb; vx->cpu.fault_jmp = &jb;
    if (setjmp(jb)) { fprintf(stderr, "guest fault: %s\n", vx->cpu.fault_msg); return 1; }
    uint32_t cb = heap_alloc(vx, codr_len - 8); memcpy(vx->mem + cb, codr + 8, codr_len - 8);
    uint32_t state = new_zeroed(vx, 0x65c), scratch = new_zeroed(vx, 0x360), out = new_zeroed(vx, 0x280), handle = new_zeroed(vx, 8), vars = new_zeroed(vx, 16);
    uint32_t inbuf = heap_alloc(vx, FB);
    uint32_t oargs[8] = { CODEC_MAGIC, mode, 0, 0, cb, codr_len - 8, state, handle };
    if (cpu_call(&vx->cpu, FN_DECODER_OPEN, oargs, 8, NULL)) { fprintf(stderr, "open fail\n"); return 1; }
    uint32_t h0 = g32(&vx->cpu.k, handle), h1 = g32(&vx->cpu.k, handle + 4);
    for (int a = 6; a < argc; a++) {
        uint32_t off, len; sscanf(argv[a], "%u:%u", &off, &len);
        uint32_t s = off + DELAY, startF = s / NS, endF = (s + len - 1) / NS + 1;
        startF = startF > OVER ? startF - OVER : 0;
        uint32_t skip = s - startF * NS;
        uint32_t rargs[2] = { h0, h1 };
        cpu_call(&vx->cpu, FN_DECODER_RESET, rargs, 2, NULL);
        int16_t *eng = malloc((endF - startF) * NS * 2); uint32_t n = 0;
        for (uint32_t fr = startF; fr < endF; fr++) {
            memcpy(vx->mem + inbuf, spch + (size_t)fr * FB, FB);
            uint32_t flag = 0;
            if (skip >= NS) flag = skip >= 3 * NS;
            s16(&vx->cpu.k, vars, FB); s16(&vx->cpu.k, vars + 4, 0x280);
            uint32_t pargs[10] = { h0, h1, flag, inbuf, vars, scratch, out, vars + 4, 0, 0 };
            int rc = (int)cpu_call(&vx->cpu, FN_DECODER_PROCESS, pargs, 10, NULL);
            if (rc) { fprintf(stderr, "rc %#x\n", rc); return 1; }
            if (skip >= NS) { skip -= NS; continue; }
            memcpy(eng + n, vx->mem + out + skip * 2, (NS - skip) * 2); n += NS - skip; skip = 0;
        }
        int16_t *ref = malloc(len * 2);
        fseek(sf, (long)off * 2, SEEK_SET); fread(ref, 2, len, sf);
        double se = 0, sr = 0; int maxd = 0, firstbad = -1;
        for (uint32_t i = 0; i < len && i < n; i++) {
            int d = eng[i] - ref[i]; if (abs(d) > maxd) maxd = abs(d);
            if (abs(d) > 64 && firstbad < 0) firstbad = (int)i;
            se += (double)d * d; sr += (double)ref[i] * ref[i];
        }
        printf("unit %u:%u frames %u..%u: max|diff| %d, SNR %.1f dB, first |diff|>64 at %d; eng[0..8]=", off, len, startF, endF, maxd, 10 * log10(sr / (se + 1e-9)), firstbad);
        for (int i = 0; i < 8; i++) printf(" %d", eng[i]); printf(" ref[0..8]=");
        for (int i = 0; i < 8; i++) printf(" %d", ref[i]); printf("\n");
        free(eng); free(ref);
    }
    return 0;
}
