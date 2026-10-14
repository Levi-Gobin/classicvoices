// Accelerate, regex and SQLite for the MacinTalk guest.
#include "mt_internal.h"

#include <math.h>
#include <regex.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t A(X86 *c, int i) { return x86_arg(c, i); }
static inline void R(X86 *c, uint32_t v) { c->r[EAX] = v; }
static inline float argf(X86 *c, int i) { uint32_t u = A(c, i); float f; memcpy(&f, &u, 4); return f; }
static inline float *F(Mt *m, uint32_t a) { return (float *)G(m, a); }

// ---------------------------------------------------------------- BLAS (plain C, deterministic)

static void h_isamax(Mt *m, X86 *c) {
    int n = (int)A(c, 0), inc = (int)A(c, 2);
    float *x = F(m, A(c, 1));
    int best = 0;
    float bv = -1;
    for (int i = 0; i < n; i++) { float v = fabsf(x[i * inc]); if (v > bv) { bv = v; best = i; } }
    R(c, (uint32_t)best);
}
static void h_saxpy(Mt *m, X86 *c) {
    int n = (int)A(c, 0), ix = (int)A(c, 3), iy = (int)A(c, 5);
    float a = argf(c, 1), *x = F(m, A(c, 2)), *y = F(m, A(c, 4));
    for (int i = 0; i < n; i++) y[i * iy] += a * x[i * ix];
}
static void h_scopy(Mt *m, X86 *c) {
    int n = (int)A(c, 0), ix = (int)A(c, 2), iy = (int)A(c, 4);
    float *x = F(m, A(c, 1)), *y = F(m, A(c, 3));
    for (int i = 0; i < n; i++) y[i * iy] = x[i * ix];
}
static void h_sdot(Mt *m, X86 *c) {
    int n = (int)A(c, 0), ix = (int)A(c, 2), iy = (int)A(c, 4);
    float *x = F(m, A(c, 1)), *y = F(m, A(c, 3)), s = 0;
    for (int i = 0; i < n; i++) s += x[i * ix] * y[i * iy];
    x86_fpush(c, s);
}
static void h_sscal(Mt *m, X86 *c) {
    int n = (int)A(c, 0), ix = (int)A(c, 3);
    float a = argf(c, 1), *x = F(m, A(c, 2));
    for (int i = 0; i < n; i++) x[i * ix] *= a;
}
// cblas_sgemv(order, trans, M, N, alpha, A, lda, X, incX, beta, Y, incY)
static void h_sgemv(Mt *m, X86 *c) {
    int order = (int)A(c, 0), trans = (int)A(c, 1), M = (int)A(c, 2), N = (int)A(c, 3);
    float alpha = argf(c, 4), *a = F(m, A(c, 5)), *x = F(m, A(c, 7)), beta = argf(c, 9), *y = F(m, A(c, 10));
    int lda = (int)A(c, 6), ix = (int)A(c, 8), iy = (int)A(c, 11);
    int t = trans != 111;  // CblasNoTrans = 111
    if (order == 102) t = !t;  // column major: swap roles
    int rows = order == 101 ? M : N, cols = order == 101 ? N : M;  // stored matrix shape
    // y = alpha*op(A)*x + beta*y, with A stored row-major rows x cols (lda)
    int ylen = t ? cols : rows, xlen = t ? rows : cols;
    for (int i = 0; i < ylen; i++) {
        float s = 0;
        for (int j = 0; j < xlen; j++) s += (t ? a[j * lda + i] : a[i * lda + j]) * x[j * ix];
        y[i * iy] = alpha * s + (beta == 0 ? 0 : beta * y[i * iy]);
    }
}

static void h_snrm2(Mt *m, X86 *c) {  // cblas_snrm2(N, X, incX)
    int n = (int)A(c, 0), ix = (int)A(c, 2);
    float *x = F(m, A(c, 1));
    double ss = 0;
    for (int i = 0; i < n; i++) ss += (double)x[i * ix] * x[i * ix];
    x86_fpush(c, (float)sqrt(ss));
}
static void h_catlas_sset(Mt *m, X86 *c) {  // (N, alpha, X, incX)
    int n = (int)A(c, 0), ix = (int)A(c, 3);
    float a = argf(c, 1), *x = F(m, A(c, 2));
    for (int i = 0; i < n; i++) x[i * ix] = a;
}
// cblas_sgemm(order, transA, transB, M, N, K, alpha, A, lda, B, ldb, beta, C, ldc)
static void h_sgemm(Mt *m, X86 *c) {
    int order = (int)A(c, 0), ta = (int)A(c, 1) != 111, tb = (int)A(c, 2) != 111;
    int M = (int)A(c, 3), N = (int)A(c, 4), K = (int)A(c, 5);
    float alpha = argf(c, 6), *a = F(m, A(c, 7)), *b = F(m, A(c, 9)), beta = argf(c, 11), *cc = F(m, A(c, 12));
    int lda = (int)A(c, 8), ldb = (int)A(c, 10), ldc = (int)A(c, 13);
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++) {
            float s = 0;
            for (int k = 0; k < K; k++) {
                float av, bv;
                if (order == 101) { av = ta ? a[k * lda + i] : a[i * lda + k]; bv = tb ? b[j * ldb + k] : b[k * ldb + j]; }
                else { av = ta ? a[i * lda + k] : a[k * lda + i]; bv = tb ? b[k * ldb + j] : b[j * ldb + k]; }
                s += av * bv;
            }
            float *dst = order == 101 ? &cc[i * ldc + j] : &cc[j * ldc + i];
            *dst = alpha * s + (beta == 0 ? 0 : beta * *dst);
        }
}
extern void ssyevr_(char *jobz, char *range, char *uplo, int *n, float *a, int *lda, float *vl, float *vu, int *il, int *iu,
                    float *abstol, int *m, float *w, float *z, int *ldz, int *isuppz, float *work, int *lwork, int *iwork,
                    int *liwork, int *info);
static void h_ssyevr(Mt *m, X86 *c) {
    #define P(i) (A(c, i) ? (void *)G(m, A(c, i)) : NULL)
    ssyevr_(P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9), P(10), P(11), P(12), P(13), P(14), P(15), P(16),
            P(17), P(18), P(19), P(20));
    #undef P
    R(c, 0);
}
static void h_vsve(Mt *m, X86 *c) {  // vDSP_sve(A, IA, C*, N)
    float *a = F(m, A(c, 0)), s = 0;
    int ia = (int)A(c, 1), n = (int)A(c, 3);
    for (int i = 0; i < n; i++) s += a[i * ia];
    *F(m, A(c, 2)) = s;
}
static void h_fail50(Mt *m, X86 *c) { (void)m; R(c, (uint32_t)-50); }
static void h_zero_ok(Mt *m, X86 *c) { (void)m; R(c, 0); }
static void h_atomic_add32(Mt *m, X86 *c) {  // OSAtomicAdd32Barrier(amount, int*) -> new value
    uint32_t p = A(c, 1);
    mt_check(m, c, p, 4);
    R(c, (uint32_t)__sync_add_and_fetch((int32_t *)(void *)G(m, p), (int32_t)A(c, 0)));
}

// LAPACK sgesvd_: every argument is a pointer (Fortran)
extern void sgesvd_(char *jobu, char *jobvt, int *m, int *n, float *a, int *lda, float *s, float *u, int *ldu,
                    float *vt, int *ldvt, float *work, int *lwork, int *info);
static void h_sgesvd(Mt *m, X86 *c) {
    float *nullp = NULL;
    #define P(i) (A(c, i) ? (void *)G(m, A(c, i)) : (void *)nullp)
    sgesvd_(P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9), P(10), P(11), P(12), P(13));
    #undef P
    R(c, 0);
}

// ---------------------------------------------------------------- vDSP

static void h_hann(Mt *m, X86 *c) {  // vDSP_hann_window(C, N, flag)
    float *out = F(m, A(c, 0));
    int n = (int)A(c, 1), flag = (int)A(c, 2);
    int half = flag & 1, denorm = (flag & 2) != 0;  // vDSP_HALF_WINDOW = 1, vDSP_HANN_DENORM = 0, NORM = 2
    int len = half ? (n + 1) / 2 : n;
    double w = denorm ? 1.0 : 0.8165;
    if (flag & 2) w = 0.8165;
    if (!(flag & 2)) w = 1.0;
    for (int i = 0; i < len; i++) out[i] = (float)(w * 0.5 * (1.0 - cos(2.0 * M_PI * i / n)));
}
static void h_svemg(Mt *m, X86 *c) {  // vDSP_svemg(A, IA, C*, N): sum of magnitudes
    float *a = F(m, A(c, 0)), s = 0;
    int ia = (int)A(c, 1), n = (int)A(c, 3);
    for (int i = 0; i < n; i++) s += fabsf(a[i * ia]);
    *F(m, A(c, 2)) = s;
}
static void h_vmma(Mt *m, X86 *c) {  // vDSP_vmma(A,IA,B,IB,C,IC,D,ID,E,IE,N): E = A*B + C*D
    float *a = F(m, A(c, 0)), *b = F(m, A(c, 2)), *cc = F(m, A(c, 4)), *d = F(m, A(c, 6)), *e = F(m, A(c, 8));
    int ia = (int)A(c, 1), ib = (int)A(c, 3), ic = (int)A(c, 5), id = (int)A(c, 7), ie = (int)A(c, 9), n = (int)A(c, 10);
    for (int i = 0; i < n; i++) e[i * ie] = a[i * ia] * b[i * ib] + cc[i * ic] * d[i * id];
}
static void h_vmsb(Mt *m, X86 *c) {  // vDSP_vmsb(A,IA,B,IB,C,IC,D,ID,N): D = A*B - C
    float *a = F(m, A(c, 0)), *b = F(m, A(c, 2)), *cc = F(m, A(c, 4)), *d = F(m, A(c, 6));
    int ia = (int)A(c, 1), ib = (int)A(c, 3), ic = (int)A(c, 5), id = (int)A(c, 7), n = (int)A(c, 8);
    for (int i = 0; i < n; i++) d[i * id] = a[i * ia] * b[i * ib] - cc[i * ic];
}
static void h_vmul(Mt *m, X86 *c) {  // vmul(A,IA,B,IB,C,IC,N): C = A*B
    float *a = F(m, A(c, 0)), *b = F(m, A(c, 2)), *cc = F(m, A(c, 4));
    int ia = (int)A(c, 1), ib = (int)A(c, 3), ic = (int)A(c, 5), n = (int)A(c, 6);
    for (int i = 0; i < n; i++) cc[i * ic] = a[i * ia] * b[i * ib];
}

// ---------------------------------------------------------------- regex
// Darwin i386 regex_t = {int re_magic; size_t re_nsub; const char *re_endp; void *re_g}
// regmatch_t = {off_t rm_so; off_t rm_eo} (16 bytes)

#define MAX_RE 256
static regex_t *g_re[MAX_RE];
static pthread_mutex_t g_re_mu = PTHREAD_MUTEX_INITIALIZER;

static void h_regcomp(Mt *m, X86 *c) {
    regex_t *re = calloc(1, sizeof *re);
    int rc = regcomp(re, mt_str(m, A(c, 1)), (int)A(c, 2));
    if (rc) { free(re); R(c, (uint32_t)rc); return; }
    pthread_mutex_lock(&g_re_mu);
    int i;
    for (i = 1; i < MAX_RE && g_re[i]; i++) {}
    if (i < MAX_RE) g_re[i] = re;
    pthread_mutex_unlock(&g_re_mu);
    if (i == MAX_RE) { regfree(re); free(re); R(c, REG_ESPACE); return; }
    uint32_t g = A(c, 0);
    wr32(c, g, 0x52454758u);  // our magic
    wr32(c, g + 4, (uint32_t)re->re_nsub);
    wr32(c, g + 8, 0);
    wr32(c, g + 12, (uint32_t)i);
    R(c, 0);
}
static void h_regexec(Mt *m, X86 *c) {
    uint32_t g = A(c, 0), n = A(c, 2), out = A(c, 3);
    uint32_t i = rd32(c, g + 12);
    if (rd32(c, g) != 0x52454758u || i >= MAX_RE || !g_re[i]) { R(c, REG_BADPAT); return; }
    regmatch_t mt[32];
    if (n > 32) n = 32;
    int rc = regexec(g_re[i], mt_str(m, A(c, 1)), n, n ? mt : NULL, (int)A(c, 4));
    if (!rc && out)
        for (uint32_t k = 0; k < n; k++) { wr64(c, out + 16 * k, (uint64_t)(int64_t)mt[k].rm_so); wr64(c, out + 16 * k + 8, (uint64_t)(int64_t)mt[k].rm_eo); }
    R(c, (uint32_t)rc);
}
static void h_regfree(Mt *m, X86 *c) {
    (void)m;
    uint32_t g = A(c, 0), i = rd32(c, g + 12);
    if (rd32(c, g) == 0x52454758u && i < MAX_RE && g_re[i]) { regfree(g_re[i]); free(g_re[i]); g_re[i] = NULL; }
    wr32(c, g, 0);
}

// ---------------------------------------------------------------- SQLite
// Handles are small integers in guest variables, tagged with what they hold
// so a stale one can't pass a statement where a connection belongs. The
// engine's threads share a connection, as Mac OS X's serialized SQLite
// allowed, so connections open with FULLMUTEX.

#define MAX_SQL 256
static void *g_sql[MAX_SQL];
static char g_sql_db[MAX_SQL];  // 1: connection, 0: statement
static pthread_mutex_t g_sql_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t sql_put(void *p, int db) {
    pthread_mutex_lock(&g_sql_mu);
    uint32_t i;
    for (i = 1; i < MAX_SQL && g_sql[i]; i++) {}
    if (i < MAX_SQL) { g_sql[i] = p; g_sql_db[i] = (char)db; }
    pthread_mutex_unlock(&g_sql_mu);
    return i < MAX_SQL ? i : 0;
}
static void *sql_take(uint32_t h, int db, int remove) {
    if (!h || h >= MAX_SQL) return NULL;
    pthread_mutex_lock(&g_sql_mu);
    void *p = g_sql_db[h] == db ? g_sql[h] : NULL;
    if (p && remove) g_sql[h] = NULL;
    pthread_mutex_unlock(&g_sql_mu);
    return p;
}
static void *sql_get(uint32_t h) { return sql_take(h, 0, 0); }  // statements

static void h_sqlite3_open(Mt *m, X86 *c) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(mt_str(m, A(c, 0)), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    wr32(c, A(c, 1), db ? sql_put(db, 1) : 0);
    R(c, (uint32_t)rc);
}
static void h_sqlite3_close(Mt *m, X86 *c) { (void)m; sqlite3 *db = sql_take(A(c, 0), 1, 1); if (db) sqlite3_close(db); R(c, 0); }
static void h_sqlite3_prepare(Mt *m, X86 *c) {
    sqlite3 *db = sql_take(A(c, 0), 1, 0);
    sqlite3_stmt *st = NULL;
    const char *sql = mt_str(m, A(c, 1)), *tail = NULL;
    int rc = db ? sqlite3_prepare_v2(db, sql, (int)A(c, 2), &st, &tail) : SQLITE_MISUSE;
    wr32(c, A(c, 3), st ? sql_put(st, 0) : 0);
    if (A(c, 4)) wr32(c, A(c, 4), tail ? A(c, 1) + (uint32_t)(tail - sql) : 0);
    R(c, (uint32_t)rc);
}
static void h_sqlite3_bind_text(Mt *m, X86 *c) {
    sqlite3_stmt *st = sql_get(A(c, 0));
    R(c, st ? (uint32_t)sqlite3_bind_text(st, (int)A(c, 1), mt_str(m, A(c, 2)), (int)A(c, 3), SQLITE_TRANSIENT) : SQLITE_MISUSE);
}
static void h_sqlite3_step(Mt *m, X86 *c) { (void)m; sqlite3_stmt *st = sql_get(A(c, 0)); R(c, st ? (uint32_t)sqlite3_step(st) : SQLITE_MISUSE); }
static void h_sqlite3_column_int(Mt *m, X86 *c) { (void)m; sqlite3_stmt *st = sql_get(A(c, 0)); R(c, st ? (uint32_t)sqlite3_column_int(st, (int)A(c, 1)) : 0); }
int64_t mt_sqlite_col64(uint32_t h, int col) { sqlite3_stmt *st = sql_get(h); return st ? sqlite3_column_int64(st, col) : 0; }
static void h_sqlite3_reset(Mt *m, X86 *c) { (void)m; sqlite3_stmt *st = sql_get(A(c, 0)); R(c, st ? (uint32_t)sqlite3_reset(st) : SQLITE_MISUSE); }
static void h_sqlite3_finalize(Mt *m, X86 *c) { (void)m; sqlite3_stmt *st = sql_take(A(c, 0), 0, 1); if (st) sqlite3_finalize(st); R(c, 0); }

// ---------------------------------------------------------------- C++ exceptions
// The engine is not expected to throw; if it does, that is the error.
static void h_unwind(Mt *m, X86 *c) { (void)m; x86_fault(c, "guest raised a C++ exception (unwinding is not implemented)"); }

void mt_misc_reset(void) {
    for (int i = 0; i < MAX_RE; i++) if (g_re[i]) { regfree(g_re[i]); free(g_re[i]); g_re[i] = NULL; }
    for (int i = 0; i < MAX_SQL; i++) g_sql[i] = NULL;  // handles die with the engine (read-only databases)
}

const MtHleEntry mt_hle_misc[] = {
    {"_cblas_isamax", h_isamax}, {"_cblas_saxpy", h_saxpy}, {"_cblas_scopy", h_scopy}, {"_cblas_sdot", h_sdot},
    {"_cblas_sscal", h_sscal}, {"_cblas_sgemv", h_sgemv}, {"_sgesvd_", h_sgesvd},
    {"_cblas_snrm2", h_snrm2}, {"_catlas_sset", h_catlas_sset}, {"_cblas_sgemm", h_sgemm}, {"_ssyevr_", h_ssyevr}, {"_vDSP_sve", h_vsve},
    {"_ExtAudioFileCreateWithURL", h_fail50}, {"_ExtAudioFileDispose", h_zero_ok}, {"_ExtAudioFileGetProperty", h_fail50},
    {"_ExtAudioFileSetProperty", h_fail50}, {"_ExtAudioFileWrite", h_fail50},
    {"_MPDeleteCriticalRegion", h_zero_ok}, {"_OSAtomicAdd32Barrier", h_atomic_add32}, {"_OSAtomicAdd32", h_atomic_add32},
    {"_vDSP_hann_window", h_hann}, {"_vDSP_svemg", h_svemg}, {"_vDSP_vmma", h_vmma}, {"_vDSP_vmsb", h_vmsb},
    {"_vmul", h_vmul}, {"_vDSP_vmul", h_vmul},
    {"_regcomp", h_regcomp}, {"_regexec", h_regexec}, {"_regfree", h_regfree},
    {"_sqlite3_open", h_sqlite3_open}, {"_sqlite3_close", h_sqlite3_close}, {"_sqlite3_prepare", h_sqlite3_prepare},
    {"_sqlite3_prepare_v2", h_sqlite3_prepare}, {"_sqlite3_bind_text", h_sqlite3_bind_text},
    {"_sqlite3_step", h_sqlite3_step}, {"_sqlite3_column_int", h_sqlite3_column_int},
    {"_sqlite3_reset", h_sqlite3_reset}, {"_sqlite3_finalize", h_sqlite3_finalize},
    {"__Unwind_Resume", h_unwind}, {"__Unwind_RaiseException", h_unwind}, {"__Unwind_Resume_or_Rethrow", h_unwind},
    {NULL, NULL},
};
