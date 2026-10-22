// Loads per-firmware recompiled engine libraries (cvnative_<build>.so) from a
// directory the host sets with cv_set_native_dir: the Linux and Android
// counterpart of nvda/src/vx_native_load.c. The libraries are built from the
// user's own firmware (linux/classicvoices-data native, or the user's own
// GitHub build) and never distributed; without them the engine stays on the
// interpreter, which is bit-identical, only slower.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "recomp_rt.h"
#include "thumb.h"
#include "vxengine.h"

typedef int (*CvAbi)(void);
typedef void (*CvBind)(int (*)(Cpu *, uint32_t), int (*)(Cpu *, uint32_t, uint32_t),
                       void (*)(const char *, uint32_t, uint32_t),
                       void (*)(uint32_t, uint32_t, uint32_t));
typedef const NativeEntry *(*CvTable)(size_t *);

void vx_fault_mem(const char *kind, uint32_t addr, uint32_t pc);
void vx_undef(uint32_t hw1, uint32_t hw2, uint32_t pc);
void vx_posix_log(int level, const char *fmt, ...);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_dir[1024];

typedef struct {
    char build[16];
    const NativeEntry *table;  // NULL if the library was tried and not usable
    size_t count;
    int tried;
} Slot;
static Slot g_slots[16];

__attribute__((visibility("default"))) void cv_set_native_dir(const char *dir) {
    pthread_mutex_lock(&g_lock);
    snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
    memset(g_slots, 0, sizeof g_slots);  // a new dir means a fresh look
    pthread_mutex_unlock(&g_lock);
}

// Called from vxstream's install_native (VX_DYNAMIC_NATIVE). Returns 1 when a
// native table was installed for this build.
int vx_try_native_dll(Vx *vx, const char *build) {
    pthread_mutex_lock(&g_lock);
    Slot *slot = NULL;
    for (size_t i = 0; i < sizeof g_slots / sizeof g_slots[0]; i++) {
        if (g_slots[i].tried && strcmp(g_slots[i].build, build) == 0) { slot = &g_slots[i]; break; }
        if (!slot && !g_slots[i].tried) slot = &g_slots[i];
    }
    if (slot && !slot->tried && g_dir[0]) {
        slot->tried = 1;
        snprintf(slot->build, sizeof slot->build, "%s", build);
        char path[1200];
        snprintf(path, sizeof path, "%s/cvnative_%s.so", g_dir, build);
        void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);  // kept loaded for the process lifetime
        if (m) {
            CvAbi abi = (CvAbi)dlsym(m, "cv_native_abi");
            CvBind bind = (CvBind)dlsym(m, "cv_native_bind");
            CvTable table = (CvTable)dlsym(m, "cv_native_table");
            if (abi && bind && table && abi() == 1) {
                bind(rt_call, rt_import, vx_fault_mem, vx_undef);
                slot->table = table(&slot->count);
                vx_posix_log(1, "native engine code for %s: %zu functions", build, slot->count);
            } else {
                dlclose(m);
            }
        } else {
            const char *e = dlerror();
            vx_posix_log(0, "no native engine code for %s (%s)", build, e ? e : "?");
        }
    }
    int ok = slot && slot->table && slot->count;
    if (ok) vx_set_native(vx, slot->table, slot->count);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

// 0 = tried and absent, -1 = not asked for yet, else the function count.
__attribute__((visibility("default"))) long long cv_native_status(const char *build) {
    long long r = -1;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_slots / sizeof g_slots[0]; i++)
        if (g_slots[i].tried && strcmp(g_slots[i].build, build) == 0)
            r = (long long)(g_slots[i].table ? g_slots[i].count : 0);
    pthread_mutex_unlock(&g_lock);
    return r;
}
