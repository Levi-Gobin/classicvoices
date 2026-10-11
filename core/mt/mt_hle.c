// Registry of host implementations (HLE) of the Mac OS APIs MacinTalk imports.
#include "mt_internal.h"
#include <string.h>

extern const MtHleEntry mt_hle_libc[], mt_hle_mp[], mt_hle_cf[], mt_hle_aac[], mt_hle_audio[], mt_hle_misc[], mt_hle_gcd[], mt_hle_lion[];
// mt_hle_aac comes before mt_hle_audio, which only has placeholders for the converter
static const MtHleEntry *const kTables[] = {mt_hle_libc, mt_hle_mp, mt_hle_cf, mt_hle_aac, mt_hle_audio, mt_hle_misc, mt_hle_gcd, mt_hle_lion};

MtHle mt_lookup_hle(const char *name) {
    for (size_t t = 0; t < sizeof kTables / sizeof kTables[0]; t++)
        for (const MtHleEntry *e = kTables[t]; e->name; e++)
            if (!strcmp(e->name, name)) return e->fn;
    return NULL;
}

// Imported variables and the storage each needs; contents in mt_init_data_import (mt_mp.c).
static const struct { const char *name; uint32_t size; } kData[] = {
    {"___stack_chk_guard", 4}, {"___stderrp", 4}, {"___stdoutp", 4}, {"___stdinp", 4},
    {"__DefaultRuneLocale", 0x1000}, {"_kCFAllocatorDefault", 4}, {"_kCFAllocatorNull", 4},
    {"_kCFAllocatorSystemDefault", 4}, {"_kCFAllocatorMalloc", 4},
    {"_kCFTypeDictionaryKeyCallBacks", 24}, {"_kCFTypeDictionaryValueCallBacks", 20},
    {"_kCFCopyStringDictionaryKeyCallBacks", 24}, {"_kCFTypeArrayCallBacks", 20},
    {"_kCFBooleanTrue", 4}, {"_kCFBooleanFalse", 4}, {"_kCFRunLoopDefaultMode", 4}, {"_kCFRunLoopCommonModes", 4},
    {"___CFConstantStringClassReference", 32}, {"_NXArgc", 4}, {"_NXArgv", 4}, {"_environ", 4},
    {"___progname", 4}, {"_mach_task_self_", 4}, {"__dyld_mod_term_funcs", 4}, {"___mb_cur_max", 4},
    {"_NDR_record", 8}, {"___sF", 3 * 88}, {"___keymgr_global", 64}, {"__dispatch_source_type_timer", 16}, {"__dispatch_main_q", 64},
    {NULL, 0},
};

int mt_lion_is_data(const char *name);
int mt_is_data_import(const char *name, uint32_t *size) {
    if (mt_lion_is_data(name)) { *size = !strncmp(name, "__NSConcrete", 12) ? 32 : 4; return 1; }
    for (int i = 0; kData[i].name; i++)
        if (!strcmp(kData[i].name, name)) { *size = kData[i].size; return 1; }
    return 0;
}

void mt_libc_wake(void), mt_mp_wake(void), mt_audio_wake(void), mt_gcd_wake(void), mt_gcd_reset(void);
void mt_libc_reset(void), mt_mp_reset(void), mt_audio_reset(void), mt_cf_reset(void), mt_misc_reset(void),
    mt_aac_reset(void), mt_lion_reset(void), mt_core_reset(void);
void mt_wake_all(void) { mt_libc_wake(); mt_mp_wake(); mt_audio_wake(); mt_gcd_wake(); }
void mt_reset_globals(void) {
    mt_libc_reset(); mt_mp_reset(); mt_gcd_reset(); mt_audio_reset(); mt_cf_reset(); mt_misc_reset(); mt_aac_reset(); mt_lion_reset(); mt_core_reset();
}
