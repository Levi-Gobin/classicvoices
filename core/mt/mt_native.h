// Recompiled MacinTalk code (tools/recomp_x86.py output, build/mtgen*/): each
// generated set registers itself at startup, and an engine uses the set whose
// code ranges hash to exactly what it loaded.
#pragma once
#include "../x86/x86.h"

// Only x86.h, so that the generated files don't depend on the engine's
// internals (and needn't all be rebuilt when they change).
typedef struct Mt Mt;

typedef struct { const char *image; uint32_t lo, hi; uint64_t hash; } MtNativeRange;
typedef struct {
    const char *name;  // mt104 ... mt107
    const MtNativeRange *ranges;
    size_t nranges;
    const X86Native *table;  // sorted by address
    size_t count;
} MtNativeSet;

void mt_native_register(const MtNativeSet *s);

// After mt_bind_all: use a matching set (unless MT_NATIVE=0). Also, for the
// tools: MT_DUMP=<dir> writes the loaded images for recomp_x86.py, and
// MT_PROFILE=<file> records where control goes (added to the file at
// mt_native_finish).
void mt_native_attach(Mt *m);
void mt_native_finish(Mt *m);
uint64_t mt_native_hash(const uint8_t *p, uint32_t n);
