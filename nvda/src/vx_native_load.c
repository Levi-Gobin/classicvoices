// Loads per-firmware recompiled engine DLLs (cvnative_<build>_<bits>.dll)
// from a directory the host sets with cv_set_native_dir. The DLLs are built
// from the user's own firmware by nvda/build_native.py and never distributed;
// without them the engine silently stays on the interpreter.
#include <windows.h>

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

static SRWLOCK g_lock = SRWLOCK_INIT;
static wchar_t g_dir[1024];

typedef struct {
    char build[16];
    const NativeEntry *table;  // NULL if the DLL was tried and not usable
    size_t count;
    int tried;
} Slot;
static Slot g_slots[16];

__declspec(dllexport) void cv_set_native_dir(const char *utf8) {
    AcquireSRWLockExclusive(&g_lock);
    g_dir[0] = 0;
    if (utf8 && *utf8)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, g_dir, (int)(sizeof g_dir / sizeof g_dir[0]));
    memset(g_slots, 0, sizeof g_slots);  // a new dir means a fresh look
    ReleaseSRWLockExclusive(&g_lock);
}

// Called from vxstream's install_native (VX_DYNAMIC_NATIVE). Returns 1 when a
// native table was installed for this build.
int vx_try_native_dll(Vx *vx, const char *build) {
    AcquireSRWLockExclusive(&g_lock);
    Slot *slot = NULL;
    for (size_t i = 0; i < sizeof g_slots / sizeof g_slots[0]; i++) {
        if (g_slots[i].tried && strcmp(g_slots[i].build, build) == 0) { slot = &g_slots[i]; break; }
        if (!slot && !g_slots[i].tried) slot = &g_slots[i];
    }
    if (slot && !slot->tried && g_dir[0]) {
        slot->tried = 1;
        snprintf(slot->build, sizeof slot->build, "%s", build);
        wchar_t path[1200];
        _snwprintf(path, 1200, L"%s\\cvnative_%hs_%d.dll", g_dir, build, (int)(sizeof(void *) * 8));
        HMODULE m = LoadLibraryW(path);  // kept loaded for the process lifetime
        if (m) {
            CvAbi abi = (CvAbi)GetProcAddress(m, "cv_native_abi");
            CvBind bind = (CvBind)GetProcAddress(m, "cv_native_bind");
            CvTable table = (CvTable)GetProcAddress(m, "cv_native_table");
            if (abi && bind && table && abi() == 1) {
                bind(rt_call, rt_import, vx_fault_mem, vx_undef);
                slot->table = table(&slot->count);
            } else {
                FreeLibrary(m);
            }
        }
    }
    int ok = slot && slot->table && slot->count;
    if (ok) vx_set_native(vx, slot->table, slot->count);
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

// How the host asks what is loaded (for the data-manager status line):
// returns the entry count, 0 = tried and absent, -1 = not asked for yet.
__declspec(dllexport) long long cv_native_status(const char *build) {
    long long r = -1;
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < sizeof g_slots / sizeof g_slots[0]; i++)
        if (g_slots[i].tried && strcmp(g_slots[i].build, build) == 0)
            r = (long long)(g_slots[i].table ? g_slots[i].count : 0);
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}
