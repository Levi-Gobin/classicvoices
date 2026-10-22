// Compiled into every cvnative_<build>_<bits>.dll next to the recompiled
// engine code. The generated code calls rt_call / rt_import / vx_fault_mem /
// vx_undef, which live in cvengine; rather than import-linking against it,
// the loader hands their addresses in through cv_native_bind, so the native
// DLL builds the same way with MSVC, clang or gcc and never needs an import
// library. CV_BUILD is the table prefix (ios6, ...), set on the command line.
#include "thumb.h"

#ifdef _WIN32
#define CV_EXPORT __declspec(dllexport)
#else
#define CV_EXPORT __attribute__((visibility("default")))
#endif

typedef int (*CvRtCall)(Cpu *, uint32_t);
typedef int (*CvRtImport)(Cpu *, uint32_t, uint32_t);
typedef void (*CvFaultMem)(const char *, uint32_t, uint32_t);
typedef void (*CvUndef)(uint32_t, uint32_t, uint32_t);

static CvRtCall p_rt_call;
static CvRtImport p_rt_import;
static CvFaultMem p_fault_mem;
static CvUndef p_undef;

int rt_call(Cpu *c, uint32_t ret) { return p_rt_call(c, ret); }
int rt_import(Cpu *c, uint32_t stub, uint32_t ret) { return p_rt_import(c, stub, ret); }
void vx_fault_mem(const char *kind, uint32_t addr, uint32_t pc) { p_fault_mem(kind, addr, pc); }
void vx_undef(uint32_t hw1, uint32_t hw2, uint32_t pc) { p_undef(hw1, hw2, pc); }

#define CV_CAT2(a, b) a##b
#define CV_CAT(a, b) CV_CAT2(a, b)
extern const NativeEntry CV_CAT(vx_native_, CV_BUILD)[];
extern const size_t CV_CAT(CV_CAT(vx_native_, CV_BUILD), _count);

CV_EXPORT int cv_native_abi(void) { return 1; }

CV_EXPORT void cv_native_bind(CvRtCall rc, CvRtImport ri, CvFaultMem fm, CvUndef ud) {
    p_rt_call = rc;
    p_rt_import = ri;
    p_fault_mem = fm;
    p_undef = ud;
}

CV_EXPORT const NativeEntry *cv_native_table(size_t *count) {
    *count = CV_CAT(CV_CAT(vx_native_, CV_BUILD), _count);
    return CV_CAT(vx_native_, CV_BUILD);
}
