// MacinTalk host: Apple's i386 Mac OS X 10.4-10.6 speech engine, loaded into
// the x86 interpreter (core/x86) with the Mac OS APIs it imports implemented
// here (HLE). The loader and much of the API behaviour follow panthera-speech
// by Tamas Geczy (MIT; https://github.com/tgeczy/panthera-speech), which runs
// the same engines natively on Windows; this is a re-implementation for a
// guest address space that is NOT the host's (iOS reserves the low 4 GB of a
// 64-bit process, so identity mapping is impossible).
#pragma once
#include "../x86/x86.h"
#include <stddef.h>
#include <stdint.h>

// Guest memory map (2 GB)
#define MT_TRAP_BASE   0x00100000u  // 16 bytes per imported function
#define MT_TRAP_SLOTS  8192u
#define MT_DATA_BASE   0x00300000u  // imported data symbols, host-owned objects (to 0x01000000)
#define MT_IMAGE_BASE  0x01000000u  // images, slid here one after another
#define MT_HEAP_HI     0x40000000u  // heap grows from the end of the images up to here
#define MT_MAP_LO      0x40000000u  // file mappings (Alex's sample bank is ~650 MB)
#define MT_MAP_HI      0x7E000000u
#define MT_STACK_TOP   0x7FF00000u  // 1 MB stacks downward from here, one per thread
#define MT_STOP        0x000FFFF0u  // return address that ends a host->guest call

typedef struct Mt Mt;
typedef void (*MtHle)(Mt *m, X86 *c);

typedef struct {
    const char *path;
    uint8_t *file;
    const uint8_t *slice;
    uint32_t lo, hi, data_vmaddr, slide;
    const uint8_t *syms;  // nlist[]
    uint32_t nsyms;
    const char *strs;
    const uint8_t *dys;   // dysymtab_command
    struct { char sectname[17], segname[17]; uint32_t addr, size, offset, flags, r1, r2; } sects[64];
    int nsects;
    uint32_t segaddr[16];
    int nsegs;
    const uint8_t *info;  // dyld_info_command, 10.6+
    uint32_t flags;
} MtImage;

struct MtThread;
struct Mt {
    uint8_t *mem;
    struct MtThread *main;  // the thread that loads the engine and calls into it
    jmp_buf *fault;
    char err[512];
    int verbose;

    // imports
    const char *trap_name[MT_TRAP_SLOTS];
    MtHle trap_fn[MT_TRAP_SLOTS];
    int ntraps;
    uint32_t data_next;

    // heap
    uint32_t heap_lo, heap_next;
    uint32_t map_next;

    MtImage images[8];
    int nimages;

    volatile int dying;      // shutting down: guest threads fault out
    volatile int live;       // guest threads other than the caller's
    volatile int broken;     // one of them faulted: its locks and state are lost
};

Mt *mt_create(char *err, size_t errlen);
void mt_destroy(Mt *m);  // stops the engine's threads, frees everything

// load / bind / initialise
int mt_load(Mt *m, const char *path);           // returns image index
void mt_bind_all(Mt *m);                        // after every image is loaded
void mt_run_initializers(Mt *m);
uint32_t mt_export(Mt *m, int image, const char *name);

// call a guest function (cdecl) on the main CPU; returns eax
uint32_t mt_call(Mt *m, uint32_t fn, int nargs, const uint32_t *args);

// guest allocation
uint32_t mt_malloc(Mt *m, uint32_t n);
uint32_t mt_calloc(Mt *m, uint32_t n);
void mt_free(Mt *m, uint32_t a);
uint32_t mt_msize(Mt *m, uint32_t a);
uint32_t mt_strdup(Mt *m, const char *s);
// host-lifetime storage (never freed) in the data region
uint32_t mt_static(Mt *m, uint32_t n);

static inline uint8_t *G(Mt *m, uint32_t a) { return m->mem + a; }
const char *mt_str(Mt *m, uint32_t a);  // checked NUL-terminated guest string

// HLE registry (mt_hle.c)
MtHle mt_lookup_hle(const char *name);
int mt_is_data_import(const char *name, uint32_t *size);
void mt_init_data_import(Mt *m, const char *name, uint32_t addr);
