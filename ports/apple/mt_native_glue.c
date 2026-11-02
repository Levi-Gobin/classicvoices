// Compiled into every cvmtnative_<gen>.so next to the recompiled MacinTalk
// code (the released Mac app builds them on the Mac, MacApp/FastCodeBuilder).
// The generated code calls the x86 runtime and registers its set from a
// constructor; here those land in the library itself, and the loader
// (mt_native_dl.c) hands the runtime in and takes the set out, as
// nvda/src/native_glue.c does for the iPhone engines.
#include <stdarg.h>
#include <stdio.h>

#include "mt/mt_native.h"

#define CV_EXPORT __attribute__((visibility("default")))

typedef void (*CvKFault)(const char *, ...);
typedef int (*CvRtCall)(X86 *, uint32_t);
typedef void (*CvRtTrap)(X86 *);

static CvKFault p_kfault;
static CvRtCall p_rt_call;
static CvRtTrap p_rt_trap;
static const MtNativeSet *g_set;

const int x86_rt_never = 0;

void x86_kfault(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    p_kfault("%s", buf);
    __builtin_trap();
}

int x86_rt_call(X86 *c, uint32_t ret) { return p_rt_call(c, ret); }
void x86_rt_trap(X86 *c) { p_rt_trap(c); }
void mt_native_register(const MtNativeSet *s) { g_set = s; }

CV_EXPORT int cv_mt_native_abi(void) { return 1; }

CV_EXPORT const MtNativeSet *cv_mt_native_bind(CvKFault kfault, CvRtCall call, CvRtTrap trap) {
    p_kfault = kfault;
    p_rt_call = call;
    p_rt_trap = trap;
    return g_set;
}
