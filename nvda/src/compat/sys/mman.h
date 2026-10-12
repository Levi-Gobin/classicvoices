// Minimal mmap shim for the Windows build of the Classic Voices engine.
// The engine uses exactly two shapes:
//   mmap(NULL, GUEST_MEM_SIZE, RW, MAP_PRIVATE|MAP_ANON, -1, 0)   guest memory
//   mmap(mem+a, span, RW, MAP_PRIVATE|MAP_FIXED, fd, off)         file contents
// The fixed-address form is satisfied by reading the file into the already
// committed guest region (private copy-on-write semantics are then trivial).
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

#ifdef __cplusplus
}
#endif
