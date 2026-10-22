// Just enough of Darwin's <runetype.h> for vxengine.c: the engine was compiled
// against Darwin libc, so __maskrune results and the _DefaultRuneLocale table
// handed into guest memory must use Darwin's BSD _CTYPE_* bit values.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t __runetype[256];  // BSD rune flags; low byte holds the digit value
    int32_t __maplower[256];
    int32_t __mapupper[256];
} _RuneLocale;

extern _RuneLocale _DefaultRuneLocale;

#ifdef __cplusplus
}
#endif
