// libcvzig.so: not a library but a small program, shipped in the APK's lib
// folder because that is the one place Android lets an app start programs
// from. It runs the Zig compiler the app downloaded (Android won't start a
// downloaded program itself, but lets an app map one into memory), so the
// phone can build its own fast engine code:
//
//   CV_ZIG=<path to zig> libcvzig.so cc -c file.c ...
//
// Zig is one static executable: its segments are mapped where it was linked,
// a fresh stack gets the arguments, environment and auxiliary vector a kernel
// would have given it, and control jumps to its entry point. When Zig starts
// itself again (it runs clang that way), /proc/self/exe is this program,
// which loads Zig again from CV_ZIG.
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__LP64__)
typedef Elf64_Ehdr Ehdr;
typedef Elf64_Phdr Phdr;
typedef Elf64_auxv_t Auxv;
#else
typedef Elf32_Ehdr Ehdr;
typedef Elf32_Phdr Phdr;
typedef Elf32_auxv_t Auxv;
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void die(const char *what) {
    fprintf(stderr, "libcvzig: %s: %s\n", what, strerror(errno));
    _exit(127);
}

static int prot_of(uint32_t f) {
    return (f & PF_R ? PROT_READ : 0) | (f & PF_W ? PROT_WRITE : 0) | (f & PF_X ? PROT_EXEC : 0);
}

__attribute__((noreturn)) static void jump(void *entry, void *sp) {
#if defined(__aarch64__)
    __asm__ volatile("mov sp, %0\n mov x0, #0\n mov x29, #0\n mov x30, #0\n br %1" ::"r"(sp), "r"(entry) : "x0", "memory");
#elif defined(__x86_64__)
    __asm__ volatile("mov %0, %%rsp\n xor %%edx, %%edx\n xor %%ebp, %%ebp\n jmp *%1" ::"r"(sp), "r"(entry) : "rdx", "memory");
#elif defined(__arm__)
    __asm__ volatile("mov sp, %0\n mov r0, #0\n mov lr, #0\n bx %1" ::"r"(sp), "r"(entry) : "r0", "memory");
#elif defined(__i386__)
    __asm__ volatile("mov %0, %%esp\n xor %%edx, %%edx\n xor %%ebp, %%ebp\n jmp *%1" ::"r"(sp), "r"(entry) : "edx", "memory");
#else
#error "unsupported processor"
#endif
    __builtin_unreachable();
}

int main(int argc, char **argv, char **envp) {
    const char *path = getenv("CV_ZIG");
    if (!path) {
        fprintf(stderr, "libcvzig: CV_ZIG isn't set\n");
        return 127;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die(path);
    Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_type != ET_EXEC || eh.e_phentsize != sizeof(Phdr) || eh.e_phnum > 32) {
        fprintf(stderr, "libcvzig: %s isn't a static executable for this processor\n", path);
        return 127;
    }
    Phdr ph[32];
    if (pread(fd, ph, eh.e_phnum * sizeof(Phdr), eh.e_phoff) != (ssize_t)(eh.e_phnum * sizeof(Phdr))) die("reading");
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE), phdr_addr = 0;
    size_t stack_size = 8 << 20;
    for (int i = 0; i < eh.e_phnum; i++) {
        const Phdr *p = &ph[i];
        if (p->p_type == PT_GNU_STACK && p->p_memsz > stack_size) stack_size = p->p_memsz;
        if (p->p_type == PT_PHDR) phdr_addr = p->p_vaddr;
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        uintptr_t start = p->p_vaddr & ~(page - 1), file_end = p->p_vaddr + p->p_filesz;
        uintptr_t mem_end = (p->p_vaddr + p->p_memsz + page - 1) & ~(page - 1);
        int prot = prot_of(p->p_flags);
        if (p->p_filesz) {
            uintptr_t map_end = (file_end + page - 1) & ~(page - 1);
            void *m = mmap((void *)start, map_end - start, prot | (p->p_memsz > p->p_filesz ? PROT_WRITE : 0),
                           MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, p->p_offset & ~(page - 1));
            if (m == MAP_FAILED || m != (void *)start) die("mapping zig");
            if (p->p_memsz > p->p_filesz) {  // the rest of the last file page is .bss
                memset((void *)file_end, 0, map_end - file_end);
                if (!(prot & PROT_WRITE)) mprotect(m, map_end - start, prot);
            }
            start = map_end;
        }
        if (mem_end > start) {
            void *m = mmap((void *)start, mem_end - start, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m == MAP_FAILED || m != (void *)start) die("mapping zig's memory");
        }
        if (!phdr_addr && p->p_offset == 0) phdr_addr = p->p_vaddr + eh.e_phoff;
    }
    close(fd);

    // The new stack: argc, argv, envp, auxv, then the strings and 16 random bytes.
    stack_size = (stack_size + page - 1) & ~(page - 1);
    char *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) die("the stack");
    char *top = stack + stack_size;
    int envc = 0;
    while (envp[envc]) envc++;
    char **nargv = malloc((argc + 1) * sizeof *nargv), **nenv = malloc((envc + 1) * sizeof *nenv);
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        top -= n;
        memcpy(top, argv[i], n);
        nargv[i] = top;
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t n = strlen(envp[i]) + 1;
        top -= n;
        memcpy(top, envp[i], n);
        nenv[i] = top;
    }
    top -= 16;
    char *random = top;
    const void *r = (const void *)getauxval(AT_RANDOM);
    if (r) memcpy(random, r, 16);
    Auxv aux[] = {
        {AT_PHDR, {phdr_addr}}, {AT_PHENT, {sizeof(Phdr)}}, {AT_PHNUM, {eh.e_phnum}}, {AT_PAGESZ, {page}},
        {AT_ENTRY, {eh.e_entry}}, {AT_UID, {getuid()}}, {AT_EUID, {geteuid()}}, {AT_GID, {getgid()}},
        {AT_EGID, {getegid()}}, {AT_SECURE, {0}}, {AT_RANDOM, {(uintptr_t)random}},
        {AT_HWCAP, {getauxval(AT_HWCAP)}}, {AT_HWCAP2, {getauxval(AT_HWCAP2)}},
        {AT_SYSINFO_EHDR, {getauxval(AT_SYSINFO_EHDR)}}, {AT_EXECFN, {(uintptr_t)nargv[0]}}, {AT_NULL, {0}},
    };
    size_t words = 1 + argc + 1 + envc + 1 + (sizeof aux) / sizeof(uintptr_t);
    uintptr_t *sp = (uintptr_t *)(((uintptr_t)top - words * sizeof(uintptr_t)) & ~(uintptr_t)15);
    uintptr_t *w = sp;
    *w++ = argc;
    for (int i = 0; i < argc; i++) *w++ = (uintptr_t)nargv[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = (uintptr_t)nenv[i];
    *w++ = 0;
    memcpy(w, aux, sizeof aux);
    jump((void *)eh.e_entry, sp);
}
