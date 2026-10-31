// JNI bridge between the Android app (com.levigobin.classicvoices.Native)
// and the Classic Voices engine: one engine stream for the process, the
// shared voice catalogue, and an utterance cursor the synthesis thread reads
// PCM from segment by segment.
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cv_catalog.h"
#include "cv_ssml.h"
#include "vxstream.h"
#include "vxrules.h"

void cv_set_native_dir(const char *dir);
long long cv_native_status(const char *build);

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// g_stream is replaced (init, reload) only with g_life held for writing;
// calls that don't take g_mu (cancel can't wait for it) hold it for reading.
static pthread_rwlock_t g_life = PTHREAD_RWLOCK_INITIALIZER;
static VxStream *g_stream;
static char g_voices_root[1024], g_downloads_root[1024], g_native_dir[1024];

// the utterance being read (one at a time: Android synthesizes serially)
static CvUtterance g_utt;
static size_t g_seg;          // next segment to start
static int g_seg_running;     // a segment is being drained
static char g_key[96], g_fallback[96];
static char g_pack[1024];     // Apple's later pronunciation pack for the next utterance, or ""
static atomic_int g_cancel;

static char *dup_utf(JNIEnv *env, jstring s) {
    if (!s) return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *d = strdup(c ? c : "");
    if (c) (*env)->ReleaseStringUTFChars(env, s, c);
    return d;
}

// Java's modified UTF-8 can't carry 4-byte characters; build strings from
// real UTF-8 bytes through String(byte[], "UTF-8") instead.
static jstring utf8_string(JNIEnv *env, const char *s) {
    size_t n = strlen(s);
    jbyteArray b = (*env)->NewByteArray(env, (jsize)n);
    (*env)->SetByteArrayRegion(env, b, 0, (jsize)n, (const jbyte *)s);
    jclass str = (*env)->FindClass(env, "java/lang/String");
    jmethodID ctor = (*env)->GetMethodID(env, str, "<init>", "([BLjava/lang/String;)V");
    jstring cs = (*env)->NewStringUTF(env, "UTF-8");
    jstring r = (jstring)(*env)->NewObject(env, str, ctor, b, cs);
    (*env)->DeleteLocalRef(env, b);
    (*env)->DeleteLocalRef(env, cs);
    return r;
}

// Text from Java as real UTF-8 (String.getBytes("UTF-8") on the Java side).
static char *bytes_utf(JNIEnv *env, jbyteArray a) {
    jsize n = (*env)->GetArrayLength(env, a);
    char *s = malloc((size_t)n + 1);
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)s);
    s[n] = 0;
    return s;
}

JNIEXPORT jboolean JNICALL Java_com_levigobin_classicvoices_Native_init(JNIEnv *env, jclass cls, jstring voices,
                                                                        jstring downloads, jstring native_dir) {
    (void)cls;
    char *v = dup_utf(env, voices), *d = dup_utf(env, downloads), *n = dup_utf(env, native_dir);
    pthread_mutex_lock(&g_mu);
    int same = g_stream && !strcmp(v, g_voices_root) && !strcmp(d, g_downloads_root);
    if (!same) {
        pthread_rwlock_wrlock(&g_life);
        if (g_stream) vxs_destroy(g_stream);
        snprintf(g_voices_root, sizeof g_voices_root, "%s", v);
        snprintf(g_downloads_root, sizeof g_downloads_root, "%s", d);
        snprintf(g_native_dir, sizeof g_native_dir, "%s", n);
        cv_set_native_dir(n);
        g_stream = vxs_create(g_voices_root);
        if (g_stream) vxs_set_asset_root(g_stream, g_downloads_root);
        pthread_rwlock_unlock(&g_life);
    }
    jboolean ok = g_stream != NULL;
    pthread_mutex_unlock(&g_mu);
    free(v); free(d); free(n);
    return ok;
}

// Fast engine code was built: start the engine afresh at the next utterance,
// so it's used from then on (never cutting off speech under way).
static atomic_int g_reload;

JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_reload(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    atomic_store(&g_reload, 1);
}

static void reload_if_asked(void) {  // with g_mu held
    if (!atomic_exchange(&g_reload, 0) || !g_stream) return;
    pthread_rwlock_wrlock(&g_life);
    vxs_destroy(g_stream);
    cv_set_native_dir(g_native_dir);
    g_stream = vxs_create(g_voices_root);
    if (g_stream) vxs_set_asset_root(g_stream, g_downloads_root);
    pthread_rwlock_unlock(&g_life);
}

// One string per voice: key \t name \t voice \t language \t fallback \t quality \t female
JNIEXPORT jobjectArray JNICALL Java_com_levigobin_classicvoices_Native_voices(JNIEnv *env, jclass cls) {
    (void)cls;
    CvVoice *v;
    size_t n = cv_scan_voices(g_voices_root, g_downloads_root, &v);
    jclass str = (*env)->FindClass(env, "java/lang/String");
    jobjectArray out = (*env)->NewObjectArray(env, (jsize)n, str, NULL);
    for (size_t i = 0; i < n; i++) {
        char line[512];
        snprintf(line, sizeof line, "%s\t%s\t%s\t%s\t%s\t%d\t%d", v[i].key, v[i].name, v[i].voice, v[i].language,
                 v[i].fallback, v[i].quality, v[i].female);
        jstring s = utf8_string(env, line);
        (*env)->SetObjectArrayElement(env, out, (jsize)i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    cv_free_voices(v);
    return out;
}

// The pronunciation pack folder (Pronunciation/<language>) for the utterances
// begun from now on, or null for none.
JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_setRulesPack(JNIEnv *env, jclass cls, jstring dir) {
    (void)cls;
    char *d = dup_utf(env, dir);
    pthread_mutex_lock(&g_mu);
    snprintf(g_pack, sizeof g_pack, "%s", d ? d : "");
    pthread_mutex_unlock(&g_mu);
    free(d);
}

// The pronunciation rules' pre-check (core/vxrules.h), for Rules.java.
JNIEXPORT jlong JNICALL Java_com_levigobin_classicvoices_Native_newRuleIndex(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    return (jlong)(intptr_t)vx_rule_index_new();
}

JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_ruleIndexAdd(JNIEnv *env, jclass cls, jlong index,
                                                                            jint rule, jbyteArray pattern, jboolean icase) {
    (void)cls;
    if (!index || !pattern || rule < 0) return;
    char *p = bytes_utf(env, pattern);
    vx_rule_index_add((VxRuleIndex *)(intptr_t)index, (uint32_t)rule, p, icase ? 1 : 0);
    free(p);
}

JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_ruleIndexMark(JNIEnv *env, jclass cls, jlong index,
                                                                             jbyteArray text, jbyteArray marks) {
    (void)cls;
    if (!index || !text || !marks) return;
    char *t = bytes_utf(env, text);
    jsize n = (*env)->GetArrayLength(env, marks);
    jbyte *m = (*env)->GetByteArrayElements(env, marks, NULL);
    if (m) {
        vx_rule_index_mark((const VxRuleIndex *)(intptr_t)index, t, (uint8_t *)m, (size_t)n);
        (*env)->ReleaseByteArrayElements(env, marks, m, 0);
    }
    free(t);
}

JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_freeRuleIndex(JNIEnv *env, jclass cls, jlong index) {
    (void)env; (void)cls;
    if (index) vx_rule_index_free((VxRuleIndex *)(intptr_t)index);
}

// Start an utterance. text is UTF-8; ssml != 0 parses markup.
JNIEXPORT jint JNICALL Java_com_levigobin_classicvoices_Native_begin(JNIEnv *env, jclass cls, jstring key, jstring fallback,
                                                                     jbyteArray text, jboolean ssml, jint rate,
                                                                     jint pitch, jint volume) {
    (void)cls;
    if (!g_stream) return -1;
    char *k = dup_utf(env, key), *f = dup_utf(env, fallback), *t = bytes_utf(env, text);
    pthread_mutex_lock(&g_mu);
    reload_if_asked();
    if (!g_stream) {
        pthread_mutex_unlock(&g_mu);
        free(k); free(f); free(t);
        return -1;
    }
    vxs_set_rules_pack(g_stream, g_pack);  // for each segment's vxs_start
    cv_free_utterance(&g_utt);
    if (ssml) cv_parse_ssml(t, rate, pitch, volume, &g_utt);
    else cv_parse_plain(t, rate, pitch, volume, &g_utt);
    snprintf(g_key, sizeof g_key, "%s", k);
    snprintf(g_fallback, sizeof g_fallback, "%s", f ? f : "");
    g_seg = 0;
    g_seg_running = 0;
    atomic_store(&g_cancel, 0);
    jint n = (jint)g_utt.n;
    pthread_mutex_unlock(&g_mu);
    free(k); free(f); free(t);
    return n;
}

// Up to buf.length bytes of 16-bit little-endian PCM at 22050 Hz. Waits up to
// ~50 ms for audio; returns 0 if none came yet, -1 once the utterance is over.
JNIEXPORT jint JNICALL Java_com_levigobin_classicvoices_Native_read(JNIEnv *env, jclass cls, jbyteArray buf) {
    (void)cls;
    jsize cap = (*env)->GetArrayLength(env, buf) / 2;
    if (cap > 4096) cap = 4096;
    float f[4096];
    int16_t pcm[4096];
    for (int waited = 0; waited < 50; waited++) {
        if (atomic_load(&g_cancel)) return -1;
        pthread_mutex_lock(&g_mu);
        if (!g_seg_running) {
            while (g_seg < g_utt.n && !g_utt.seg[g_seg].text[0] && !g_utt.seg[g_seg].trailing_pause_ms) g_seg++;
            if (g_seg >= g_utt.n) { pthread_mutex_unlock(&g_mu); return -1; }
            CvSegment *s = &g_utt.seg[g_seg++];
            vxs_start_with_fallback(g_stream, g_key, g_fallback[0] ? g_fallback : NULL, s->text, s->rate, s->pitch,
                                    s->volume, s->trailing_pause_ms);
            g_seg_running = 1;
        }
        int done = 0;
        size_t n = vxs_read(g_stream, f, (size_t)cap, &done);
        if (done && !n) g_seg_running = 0;
        pthread_mutex_unlock(&g_mu);
        if (n) {
            for (size_t i = 0; i < n; i++) {
                float x = f[i] * 32767.0f;
                pcm[i] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            }
            (*env)->SetByteArrayRegion(env, buf, 0, (jsize)(n * 2), (const jbyte *)pcm);
            return (jint)(n * 2);
        }
        if (!done) usleep(1000);
    }
    return 0;
}

JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_cancel(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    atomic_store(&g_cancel, 1);
    pthread_rwlock_rdlock(&g_life);
    if (g_stream) vxs_cancel(g_stream);
    pthread_rwlock_unlock(&g_life);
}

JNIEXPORT void JNICALL Java_com_levigobin_classicvoices_Native_preload(JNIEnv *env, jclass cls, jstring key) {
    (void)cls;
    char *k = dup_utf(env, key);
    pthread_mutex_lock(&g_mu);
    if (g_stream && k) {
        vxs_set_rules_pack(g_stream, g_pack);
        vxs_preload(g_stream, k);
    }
    pthread_mutex_unlock(&g_mu);
    free(k);
}

JNIEXPORT jstring JNICALL Java_com_levigobin_classicvoices_Native_lastError(JNIEnv *env, jclass cls) {
    (void)cls;
    pthread_rwlock_rdlock(&g_life);
    jstring r = utf8_string(env, g_stream ? vxs_last_error(g_stream) : "engine not started");
    pthread_rwlock_unlock(&g_life);
    return r;
}

// -1 not tried yet, 0 tried and absent, else the number of native functions.
JNIEXPORT jlong JNICALL Java_com_levigobin_classicvoices_Native_nativeStatus(JNIEnv *env, jclass cls, jstring build) {
    (void)cls;
    char *b = dup_utf(env, build);
    long long r = cv_native_status(b);
    free(b);
    return (jlong)r;
}

// Timing of the last utterance: {first audio ms, synthesis ms, audio ms}.
JNIEXPORT jdoubleArray JNICALL Java_com_levigobin_classicvoices_Native_lastTiming(JNIEnv *env, jclass cls) {
    (void)cls;
    jdoubleArray a = (*env)->NewDoubleArray(env, 3);
    pthread_rwlock_rdlock(&g_life);
    double t[3] = {0, 0, 0};
    if (g_stream) t[0] = vxs_last_first_audio_ms(g_stream), t[1] = vxs_last_synth_ms(g_stream), t[2] = vxs_last_audio_ms(g_stream);
    pthread_rwlock_unlock(&g_life);
    (*env)->SetDoubleArrayRegion(env, a, 0, 3, t);
    return a;
}
