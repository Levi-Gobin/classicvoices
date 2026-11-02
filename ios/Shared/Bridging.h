#include "vxstream.h"
#include "vxphonemes.h"
#include "vxrules.h"
#include "vxlang.h"
#include "vxengine.h"
#include "mt/mtengine.h"
#include <bzlib.h>
#include <zlib.h>

// The released Mac app (CV_PUBLIC): fast code built on the Mac, loaded from
// the App Group (ports/posix/vx_native_dl.c).
void cv_set_native_dir(const char *dir);
void cv_mt_load_native(const char *dir);
