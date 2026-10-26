// Unpacks a .tar.xz (the Zig compiler, as ziglang.org publishes it for Linux)
// into a folder, keeping only the paths asked for: Android has no xz
// unpacker an app can use. The first path component (zig-aarch64-linux-0.17.0/)
// is dropped. Decoding is XZ Embedded (third_party/xz-embedded).
#include <errno.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "xz.h"

typedef struct {
    FILE *in;
    struct xz_dec *dec;
    struct xz_buf b;
    uint8_t inbuf[1 << 16];
    long long consumed, total;
    int eof;
} XzIn;

// Reads exactly n bytes of the tar stream; 1 on success, 0 at its end, -1 on an error.
static int xz_read(XzIn *x, uint8_t *out, size_t n, const char **err) {
    x->b.out = out;
    x->b.out_pos = 0;
    x->b.out_size = n;
    while (x->b.out_pos < n) {
        if (x->b.in_pos == x->b.in_size && !x->eof) {
            x->consumed += (long long)x->b.in_size;
            x->b.in_size = fread(x->inbuf, 1, sizeof x->inbuf, x->in);
            x->b.in_pos = 0;
            if (x->b.in_size == 0) x->eof = 1;
        }
        enum xz_ret r = xz_dec_run(x->dec, &x->b);
        if (r == XZ_STREAM_END) return x->b.out_pos == n ? 1 : 0;
        if (r == XZ_UNSUPPORTED_CHECK) continue;  // the download's SHA-256 was checked already
        if (r != XZ_OK) {
            *err = r == XZ_MEM_ERROR || r == XZ_MEMLIMIT_ERROR ? "not enough memory to unpack" : "the download is damaged";
            return -1;
        }
        if (x->eof && x->b.in_pos == x->b.in_size && x->b.out_pos < n) {
            *err = "the download ends early";
            return -1;
        }
    }
    return 1;
}

static long long octal(const char *p, size_t n) {
    if ((unsigned char)p[0] & 0x80) {  // base-256, for sizes past 8 GB
        long long v = 0;
        for (size_t i = 1; i < n; i++) v = v << 8 | (unsigned char)p[i];
        return v;
    }
    long long v = 0;
    for (size_t i = 0; i < n && p[i]; i++) {
        if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (p[i] - '0');
        else if (p[i] != ' ') break;
    }
    return v;
}

static int mkdirs(char *path) {  // the folders above path
    for (char *s = path + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        int r = mkdir(path, 0700);
        *s = '/';
        if (r && errno != EEXIST) return -1;
    }
    return 0;
}

static int wanted(const char *rel, char **keep, int nkeep) {
    if (!*rel || strstr(rel, "..")) return 0;
    for (int i = 0; i < nkeep; i++) {
        size_t n = strlen(keep[i]);
        if (n && keep[i][n - 1] == '/' ? !strncmp(rel, keep[i], n) : !strcmp(rel, keep[i])) return 1;
    }
    return 0;
}

static const char *untar(XzIn *x, const char *dest, char **keep, int nkeep, JNIEnv *env, jobject progress,
                         jmethodID fraction, jmethodID cancelled) {
    static uint8_t block[512], data[1 << 16];
    char longname[4096] = "", path[8192];
    const char *err = NULL;
    int last_percent = -1;
    for (;;) {
        int r = xz_read(x, block, 512, &err);
        if (r < 0) return err;
        if (r == 0) return NULL;
        if (!block[0]) return NULL;  // the two empty blocks at the end
        char type = (char)block[156];
        long long size = octal((const char *)block + 124, 12);
        char name[4096];
        if (longname[0]) {
            snprintf(name, sizeof name, "%s", longname);
            longname[0] = 0;
        } else if (!memcmp(block + 257, "ustar", 5) && block[345]) {
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)block + 345, (const char *)block);
        } else {
            snprintf(name, sizeof name, "%.100s", (const char *)block);
        }
        const char *rel = strchr(name, '/');
        rel = rel ? rel + 1 : "";
        FILE *out = NULL;
        int is_meta = type == 'x' || type == 'L';
        char meta[8192];
        size_t meta_len = 0;
        if ((type == '0' || type == '\0' || type == '7') && wanted(rel, keep, nkeep)) {
            snprintf(path, sizeof path, "%s/%s", dest, rel);
            if (mkdirs(path)) return "can't make a folder";
            out = fopen(path, "wb");
            if (!out) return "can't write the compiler's files";
        }
        for (long long left = size; left > 0;) {
            size_t chunk = (size_t)((left + 511) / 512 * 512);
            if (chunk > sizeof data) chunk = sizeof data;
            r = xz_read(x, data, chunk, &err);
            if (r <= 0) {
                if (out) fclose(out);
                return r < 0 ? err : "the download ends early";
            }
            size_t use = (size_t)(left < (long long)chunk ? left : (long long)chunk);
            if (out && fwrite(data, 1, use, out) != use) {
                fclose(out);
                return "the phone is out of space";
            }
            if (is_meta && meta_len + use < sizeof meta) {
                memcpy(meta + meta_len, data, use);
                meta_len += use;
            }
            left -= (long long)use;
        }
        if (out && fclose(out)) return "the phone is out of space";
        if (out && (octal((const char *)block + 100, 8) & 0100)) chmod(path, 0700);
        if (type == 'L') {
            meta[meta_len < sizeof meta ? meta_len : sizeof meta - 1] = 0;
            snprintf(longname, sizeof longname, "%s", meta);
        } else if (type == 'x') {  // pax: "<len> path=<name>\n" records
            meta[meta_len < sizeof meta ? meta_len : sizeof meta - 1] = 0;
            for (char *p = meta; p < meta + meta_len;) {
                char *sp = strchr(p, ' ');
                long len = strtol(p, NULL, 10);
                if (!sp || len <= 0) break;
                if (!strncmp(sp + 1, "path=", 5)) {
                    int n = (int)(p + len - (sp + 6)) - 1;  // without the newline
                    if (n > 0 && n < (int)sizeof longname) snprintf(longname, sizeof longname, "%.*s", n, sp + 6);
                }
                p += len;
            }
        }
        if (progress) {
            int pc = x->total > 0 ? (int)(x->consumed * 100 / x->total) : -1;
            if (pc != last_percent) {
                last_percent = pc;
                (*env)->CallVoidMethod(env, progress, fraction, x->total > 0 ? (double)x->consumed / x->total : -1.0);
            }
            if ((*env)->CallBooleanMethod(env, progress, cancelled)) return "cancelled";
            if ((*env)->ExceptionCheck(env)) return "cancelled";
        }
    }
}

// Native.untarXz(archive, dest, keep[], progress): null when done, else what went wrong
// ("cancelled" when progress.cancelled() said so).
JNIEXPORT jstring JNICALL Java_com_levigobin_classicvoices_Native_untarXz(JNIEnv *env, jclass cls, jstring archive,
                                                                          jstring dest, jobjectArray keep,
                                                                          jobject progress) {
    (void)cls;
    const char *a = (*env)->GetStringUTFChars(env, archive, NULL), *d = (*env)->GetStringUTFChars(env, dest, NULL);
    int nkeep = (*env)->GetArrayLength(env, keep);
    char **k = calloc((size_t)nkeep + 1, sizeof *k);
    for (int i = 0; i < nkeep; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, keep, i);
        const char *c = (*env)->GetStringUTFChars(env, s, NULL);
        k[i] = strdup(c);
        (*env)->ReleaseStringUTFChars(env, s, c);
        (*env)->DeleteLocalRef(env, s);
    }
    jmethodID fraction = NULL, cancelled = NULL;
    if (progress) {
        jclass pc = (*env)->GetObjectClass(env, progress);
        fraction = (*env)->GetMethodID(env, pc, "fraction", "(D)V");
        cancelled = (*env)->GetMethodID(env, pc, "cancelled", "()Z");
    }
    const char *err = NULL;
    XzIn *x = calloc(1, sizeof *x);
    x->in = fopen(a, "rb");
    if (!x->in) {
        err = "can't open the download";
    } else {
        struct stat st;
        if (!fstat(fileno(x->in), &st)) x->total = st.st_size;
        xz_crc32_init();
        xz_crc64_init();
        x->dec = xz_dec_init(XZ_DYNALLOC, 1u << 27);
        x->b.in = x->inbuf;
        err = x->dec ? untar(x, d, k, nkeep, env, progress, fraction, cancelled) : "not enough memory to unpack";
        if (x->dec) xz_dec_end(x->dec);
        fclose(x->in);
    }
    free(x);
    for (int i = 0; i < nkeep; i++) free(k[i]);
    free(k);
    (*env)->ReleaseStringUTFChars(env, archive, a);
    (*env)->ReleaseStringUTFChars(env, dest, d);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    return err ? (*env)->NewStringUTF(env, err) : NULL;
}
