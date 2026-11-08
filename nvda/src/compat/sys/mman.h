// Minimal mmap shim for the Windows build of the Classic Voices engine.
// The engine uses exactly two shapes:
//   mmap(NULL, GUEST_MEM_SIZE, RW, MAP_PRIVATE|MAP_ANON, -1, 0)   guest memory
//   mmap(mem+a, span, RW, MAP_PRIVATE|MAP_FIXED, fd, off)         file contents
// The fixed-address form maps the file into a region from vx_mmap_reserve
// (copy on write), or reads it into the already committed guest memory.
// The anonymous fixed form (fd -1) makes a region's pages there usable.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE 0x02
#define MAP_FIXED   0x10
#define MAP_ANON    0x1000
#define MAP_FAILED  ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int munmap(void *addr, size_t len);
// Guest memory whose first `committed` bytes are usable and whose rest takes
// fixed mappings in order, upward (vx_win.c); NULL where Windows can't, and
// then plain mmap is used. munmap frees it.
void *vx_mmap_reserve(size_t len, size_t committed);

#ifdef __cplusplus
}
#endif
