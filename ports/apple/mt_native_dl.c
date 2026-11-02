// The released Mac app's recompiled MacinTalk: cvmtnative_<gen>.so, built on
// the Mac from the user's own MacinTalk (MacApp/FastCodeBuilder.swift) and
// left in the App Group. Each library hands its set over through
// cv_mt_native_bind (mt_native_glue.c); engines opened after that use it.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "mt/mt_native.h"

typedef void (*CvKFault)(const char *, ...);
typedef int (*CvAbi)(void);
typedef const MtNativeSet *(*CvMtBind)(CvKFault, int (*)(X86 *, uint32_t), void (*)(X86 *));

void vx_posix_log(int level, const char *fmt, ...);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_dir[1024];
static char g_loaded[8][16];
static int g_nloaded;

__attribute__((visibility("default"))) void cv_mt_load_native(const char *dir) {
    static const char *gens[] = {"mt104", "mt105", "mt106", "mt107"};
    pthread_mutex_lock(&g_lock);
    if (dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    dir = g_dir;
    for (size_t i = 0; i < sizeof gens / sizeof gens[0]; i++) {
        int have = 0;
        for (int k = 0; k < g_nloaded; k++) have |= !strcmp(g_loaded[k], gens[i]);
        if (have || g_nloaded >= 8) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/cvmtnative_%s.so", dir, gens[i]);
        void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);  // kept loaded for the process lifetime
        if (!m) continue;
        CvAbi abi = (CvAbi)dlsym(m, "cv_mt_native_abi");
        CvMtBind bind = (CvMtBind)dlsym(m, "cv_mt_native_bind");
        const MtNativeSet *set = abi && bind && abi() == 1 ? bind(x86_kfault, x86_rt_call, x86_rt_trap) : NULL;
        if (set) {
            mt_native_register(set);
            snprintf(g_loaded[g_nloaded++], 16, "%s", gens[i]);
            vx_posix_log(1, "native MacinTalk code for %s: %zu functions", gens[i], set->count);
        } else {
            dlclose(m);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

// Before each MacinTalk engine opens (vxstream.c): libraries built since.
void cv_mt_native_refresh(void) {
    if (g_dir[0]) cv_mt_load_native(NULL);
}
