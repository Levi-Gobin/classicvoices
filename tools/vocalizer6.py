#!/usr/bin/env python3
"""Run the iOS 6 Nuance Vocalizer engine (statically linked in `voiced`) under
Unicorn and synthesize text to a WAV file.

Usage: vocalizer6.py "text to speak" out.wav [--lang en-US]
"""
import argparse
import os
import struct
import subprocess
import sys
import wave

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE,
                     UC_HOOK_MEM_INVALID, UC_PROT_ALL)
from unicorn.arm_const import *

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "extracted/ios6/VoiceServices.framework")
VOICED = os.path.join(FW, "Support/voiced")
TTSRES = os.path.join(FW, "TTSResources")

# --- image layout (from otool -l) ---
IMG_BASE, IMG_END = 0x1000, 0x214000
TEXT_FILESIZE = 0x206000
DATA_VM, DATA_FILEOFF, DATA_FILESIZE = 0x207000, 0x206000, 0xA000
STUBS, LAZY, NLPTR = 0x206A54, 0x207000, 0x2075A0

# engine API (from symbol table)
vauto_ttsInitialize = 0xF82B0
vauto_ttsOpen = 0xF7424
vauto_ttsSetParamList = 0xF6620
vauto_ttsSetOutDevice = 0xF542C
vauto_ttsProcessText2Speech = 0xF70B4
vauto_ttsClose = 0xF71A0
vauto_ttsUnInitialize = 0xF8098

# Apple's platform interface tables passed in VE_INSTALL
IFACE_HEAP, IFACE_CRITSEC, IFACE_STREAM, IFACE_MAPPING = 0x208B08, 0x208B18, 0x208B28, 0x208B44

HEAP_BASE, HEAP_SIZE = 0x20000000, 0x10000000
STACK_BASE, STACK_SIZE = 0x30000000, 0x400000
TRAP_BASE, TRAP_SIZE = 0x40000000, 0x10000
DATA_SYMS_BASE = 0x40100000
RETURN_TRAP = TRAP_BASE + TRAP_SIZE - 4


class Heap:
    def __init__(self, base, size):
        self.next, self.end = base, base + size
        self.sizes, self.free = {}, {}

    def alloc(self, n, align=16):
        n = max(16, (n + 15) & ~15)
        if align <= 16 and self.free.get(n):
            a = self.free[n].pop()
        else:
            a = (self.next + align - 1) & ~(align - 1)
            self.next = a + n
            if self.next > self.end:
                raise MemoryError("emulated heap exhausted")
        self.sizes[a] = n
        return a

    def release(self, a):
        n = self.sizes.pop(a, None)
        if n:
            self.free.setdefault(n, []).append(a)


class Emu:
    def __init__(self, trace=False):
        self.trace = trace
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(IMG_BASE, IMG_END - IMG_BASE, UC_PROT_ALL)
        data = open(VOICED, "rb").read()
        uc.mem_write(IMG_BASE, data[IMG_BASE - 0x1000:TEXT_FILESIZE])
        uc.mem_write(DATA_VM, data[DATA_FILEOFF:DATA_FILEOFF + DATA_FILESIZE])
        uc.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_ALL)
        uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        uc.mem_map(TRAP_BASE, TRAP_SIZE, UC_PROT_ALL)
        uc.mem_map(DATA_SYMS_BASE, 0x10000, UC_PROT_ALL)
        # every trap slot is a Thumb `bx lr`
        uc.mem_write(TRAP_BASE, b"\x70\x47" * (TRAP_SIZE // 2))
        self.heap = Heap(HEAP_BASE, HEAP_SIZE)
        self.traps = {}       # addr -> (name, fn)
        self.files = {}       # handle -> python file / (bytes) info
        self.next_handle = 3
        self.tls = {}
        self.next_key = 1
        self.data_syms = {}
        self._enable_vfp()
        self._bind_imports()
        uc.hook_add(UC_HOOK_CODE, self._on_trap, begin=TRAP_BASE, end=TRAP_BASE + TRAP_SIZE - 1)
        uc.hook_add(UC_HOOK_MEM_INVALID, self._on_bad_mem)

    # ---------- low level ----------
    def _enable_vfp(self):
        uc = self.uc
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    def r32(self, a): return struct.unpack("<I", self.uc.mem_read(a, 4))[0]
    def w32(self, a, v): self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    def w16(self, a, v): self.uc.mem_write(a, struct.pack("<H", v & 0xFFFF))

    def cstr(self, a, maxlen=4096):
        if not a:
            return None
        out = bytearray()
        while len(out) < maxlen:
            chunk = self.uc.mem_read(a + len(out), 64)
            i = chunk.find(b"\0")
            if i >= 0:
                out += chunk[:i]
                break
            out += chunk
        return bytes(out)

    def new_cstr(self, s):
        if isinstance(s, str):
            s = s.encode()
        a = self.heap.alloc(len(s) + 1)
        self.uc.mem_write(a, s + b"\0")
        return a

    def _on_bad_mem(self, uc, access, addr, size, value, ud):
        pc = uc.reg_read(UC_ARM_REG_PC)
        print(f"!! invalid memory access type={access} addr={addr:#x} size={size} pc={pc:#x} "
              f"lr={uc.reg_read(UC_ARM_REG_LR):#x}", file=sys.stderr)
        return False

    # ---------- imports ----------
    def _bind_imports(self):
        out = subprocess.run(["otool", "-Iv", VOICED], capture_output=True, text=True).stdout
        stubs, lazies, nls = [], [], []
        section = None
        for line in out.splitlines():
            if line.startswith("Indirect symbols for"):
                section = line.split("(")[1].split(")")[0]
                continue
            parts = line.split()
            if len(parts) < 3 or not parts[0].startswith("0x"):
                continue
            addr, name = int(parts[0], 16), parts[-1]
            if name in ("LOCAL", "ABSOLUTE"):
                continue
            {"__TEXT,__symbolstub1": stubs, "__DATA,__lazy_symbol": lazies,
             "__DATA,__nl_symbol_ptr": nls}.get(section, []).append((addr, name))
        slot = 0
        for addr, name in lazies + nls:
            if name in DATA_IMPORTS:
                self.w32(addr, self._data_sym(name))
                continue
            t = TRAP_BASE + slot * 4
            slot += 1
            self.traps[t] = (name, getattr(self, "f" + name, None))
            self.w32(addr, t | 1)

    def _data_sym(self, name):
        if name not in self.data_syms:
            a = DATA_SYMS_BASE + len(self.data_syms) * 64
            self.data_syms[name] = a
            if name == "___stack_chk_guard":
                self.w32(a, 0xC0FFEE42)
        return self.data_syms[name]

    def arg(self, i):
        if i < 4:
            return self.uc.reg_read(UC_ARM_REG_R0 + i)
        return self.r32(self.uc.reg_read(UC_ARM_REG_SP) + 4 * (i - 4))

    def _on_trap(self, uc, addr, size, ud):
        if addr == RETURN_TRAP:
            uc.emu_stop()
            return
        name, fn = self.traps.get(addr, ("?", None))
        if fn is None:
            lr = uc.reg_read(UC_ARM_REG_LR)
            print(f"!! unimplemented import {name} called from {lr:#x}", file=sys.stderr)
            uc.emu_stop()
            self.fatal = f"unimplemented {name}"
            return
        if self.trace:
            print(f"  -> {name}({', '.join(hex(self.arg(i)) for i in range(4))}) lr={uc.reg_read(UC_ARM_REG_LR):#x}", file=sys.stderr)
        ret = fn()
        if ret is not None:
            if isinstance(ret, tuple):
                uc.reg_write(UC_ARM_REG_R0, ret[0] & 0xFFFFFFFF)
                uc.reg_write(UC_ARM_REG_R1, ret[1] & 0xFFFFFFFF)
            else:
                uc.reg_write(UC_ARM_REG_R0, ret & 0xFFFFFFFF)

    def call(self, fn, *args):
        """Call an emulated Thumb function with up to N word args; returns r0."""
        uc = self.uc
        saved = {r: uc.reg_read(r) for r in (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
                                            UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                                            UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                                            UC_ARM_REG_CPSR)}
        nested = getattr(self, "depth", 0) > 0
        sp = saved[UC_ARM_REG_SP] - 0x100 if nested else STACK_BASE + STACK_SIZE - 0x100
        stack_args = args[4:]
        sp -= 4 * len(stack_args)
        sp &= ~7
        for i, v in enumerate(stack_args):
            self.w32(sp + 4 * i, v)
        for i, v in enumerate(args[:4]):
            uc.reg_write(UC_ARM_REG_R0 + i, v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
        self.fatal = None
        self.depth = getattr(self, "depth", 0) + 1
        try:
            uc.emu_start(fn | 1, RETURN_TRAP)
        finally:
            self.depth -= 1
        if self.fatal:
            raise RuntimeError(self.fatal)
        r0 = uc.reg_read(UC_ARM_REG_R0)
        if nested:
            for r, v in saved.items():
                if r != UC_ARM_REG_PC:
                    uc.reg_write(r, v)
        return r0

    # ---------- libc ----------
    def f_malloc(self): return self.heap.alloc(self.arg(0))

    def f_calloc(self):
        n = self.arg(0) * self.arg(1)
        a = self.heap.alloc(n)
        self.uc.mem_write(a, b"\0" * n)
        return a

    def f_realloc(self):
        old, n = self.arg(0), self.arg(1)
        a = self.heap.alloc(n)
        if old:
            oldn = self.heap.sizes.get(old, 0)
            self.uc.mem_write(a, bytes(self.uc.mem_read(old, min(oldn, n))))
            self.heap.release(old)
        return a

    def f_free(self): self.heap.release(self.arg(0)); return 0
    def f_malloc_size(self): return self.heap.sizes.get(self.arg(0), 0)

    def f_memcpy(self):
        d, s, n = self.arg(0), self.arg(1), self.arg(2)
        if n:
            self.uc.mem_write(d, bytes(self.uc.mem_read(s, n)))
        return d
    f_memmove = f_memcpy
    f___memcpy_chk = f_memcpy

    def f_memset(self):
        d, c, n = self.arg(0), self.arg(1), self.arg(2)
        if n:
            self.uc.mem_write(d, bytes([c & 0xFF]) * n)
        return d
    f___memset_chk = f_memset

    def f_memset_pattern16(self):
        d, p, n = self.arg(0), self.arg(1), self.arg(2)
        pat = bytes(self.uc.mem_read(p, 16))
        self.uc.mem_write(d, (pat * (n // 16 + 1))[:n])
        return 0

    def f_memcmp(self):
        a, b, n = self.arg(0), self.arg(1), self.arg(2)
        x, y = bytes(self.uc.mem_read(a, n)), bytes(self.uc.mem_read(b, n))
        return 0 if x == y else (-1 if x < y else 1)

    def f_memchr(self):
        s, c, n = self.arg(0), self.arg(1) & 0xFF, self.arg(2)
        i = bytes(self.uc.mem_read(s, n)).find(bytes([c]))
        return s + i if i >= 0 else 0

    def f_strlen(self): return len(self.cstr(self.arg(0), 1 << 20))

    def f_strcmp(self):
        x, y = self.cstr(self.arg(0)), self.cstr(self.arg(1))
        return 0 if x == y else (-1 if x < y else 1)

    def f_strcpy(self):
        d = self.arg(0)
        self.uc.mem_write(d, self.cstr(self.arg(1), 1 << 20) + b"\0")
        return d

    def f_strlcpy(self):
        d, s, n = self.arg(0), self.cstr(self.arg(1), 1 << 20), self.arg(2)
        if n:
            self.uc.mem_write(d, s[:n - 1] + b"\0")
        return len(s)

    def f_strlcat(self):
        d, s, n = self.arg(0), self.cstr(self.arg(1), 1 << 20), self.arg(2)
        cur = self.cstr(d, n) or b""
        room = n - len(cur) - 1
        if room > 0:
            self.uc.mem_write(d + len(cur), s[:room] + b"\0")
        return len(cur) + len(s)

    def f___stack_chk_fail(self):
        raise RuntimeError("stack check failed")

    def f___assert_rtn(self):
        raise RuntimeError(f"assert: {self.cstr(self.arg(0))} {self.cstr(self.arg(1))}:{self.arg(2)} "
                           f"{self.cstr(self.arg(3))}")

    def f_abort(self): raise RuntimeError("abort() called")

    # integer helpers (no hardware divide on Cortex-A8)
    @staticmethod
    def _s(v): return v - (1 << 32) if v & 0x80000000 else v

    def f___divsi3(self):
        a, b = self._s(self.arg(0)), self._s(self.arg(1))
        q = abs(a) // abs(b) if b else 0
        return q if (a < 0) == (b < 0) else -q

    def f___modsi3(self):
        a, b = self._s(self.arg(0)), self._s(self.arg(1))
        q = self.f___divsi3()
        return a - self._s(q & 0xFFFFFFFF) * b

    def f___udivsi3(self):
        b = self.arg(1)
        return self.arg(0) // b if b else 0

    def f___umodsi3(self):
        b = self.arg(1)
        return self.arg(0) % b if b else 0

    # soft-float ABI helpers: doubles live in r0:r1
    def _darg(self, i=0):
        return struct.unpack("<d", struct.pack("<II", self.arg(i), self.arg(i + 1)))[0]

    def _dret(self, x):
        return struct.unpack("<II", struct.pack("<d", x))

    def f_floor(self):
        import math
        return self._dret(math.floor(self._darg()))

    def f_roundf(self):
        x = struct.unpack("<f", struct.pack("<I", self.arg(0)))[0]
        r = float(int(x + 0.5)) if x >= 0 else -float(int(-x + 0.5))
        return struct.unpack("<I", struct.pack("<f", r))[0]

    def f___fixdfdi(self):
        v = int(self._darg()) & 0xFFFFFFFFFFFFFFFF
        return (v & 0xFFFFFFFF, v >> 32)

    def f___floatundidf(self):
        return self._dret(float(self.arg(0) | (self.arg(1) << 32)))

    # errno / misc
    def f___error(self): return self._data_sym("errno")
    def f_getpagesize(self): return 4096
    def f_usleep(self): return 0
    def f_setpriority(self): return 0
    def f_getiopolicy_np(self): return 0
    def f_setiopolicy_np(self): return 0

    def f_times(self):
        if self.arg(0):
            self.uc.mem_write(self.arg(0), b"\0" * 16)
        return 0

    def f_ftime(self):
        import time
        t = time.time()
        self.uc.mem_write(self.arg(0), struct.pack("<IHhh", int(t), int(t * 1000) % 1000, 0, 0))
        return 0

    # threads: single-threaded, so locks are no-ops
    def _zero(self): return 0
    f_pthread_mutex_init = f_pthread_mutex_destroy = f_pthread_mutex_lock = _zero
    f_pthread_mutex_unlock = f_pthread_mutexattr_init = f_pthread_mutexattr_destroy = _zero
    f_pthread_mutexattr_settype = _zero

    def f_pthread_self(self): return 0x1234

    def f_pthread_key_create(self):
        k = self.next_key
        self.next_key += 1
        self.w32(self.arg(0), k)
        return 0

    def f_pthread_getspecific(self): return self.tls.get(self.arg(0), 0)

    def f_pthread_setspecific(self):
        self.tls[self.arg(0)] = self.arg(1)
        return 0

    def f_pthread_once(self):
        once, fn = self.arg(0), self.arg(1)
        if self.r32(once) != 0x30B1BCBA + 1:
            self.w32(once, 0x30B1BCBA + 1)
            self.call(fn)
        return 0

    # setjmp/longjmp: save callee-saved state, resume via bx lr in trap stub
    def f_setjmp(self):
        uc, buf = self.uc, self.arg(0)
        regs = [UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_SP, UC_ARM_REG_LR]
        for i, r in enumerate(regs):
            self.w32(buf + 4 * i, uc.reg_read(r))
        for i in range(8):
            uc.mem_write(buf + 40 + 8 * i, struct.pack("<Q", uc.reg_read(UC_ARM_REG_D8 + i)))
        return 0
    f__setjmp = f_setjmp

    def f_longjmp(self):
        uc, buf, val = self.uc, self.arg(0), self.arg(1)
        regs = [UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_SP, UC_ARM_REG_LR]
        for i, r in enumerate(regs):
            uc.reg_write(r, self.r32(buf + 4 * i))
        for i in range(8):
            uc.reg_write(UC_ARM_REG_D8 + i, struct.unpack("<Q", uc.mem_read(buf + 40 + 8 * i, 8))[0])
        return val or 1
    f__longjmp = f_longjmp

    def f_qsort(self):
        base, n, sz, cmp = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        if n < 2:
            return 0
        import functools
        items = [bytes(self.uc.mem_read(base + i * sz, sz)) for i in range(n)]
        a, b = self.heap.alloc(sz), self.heap.alloc(sz)

        def c(x, y):
            self.uc.mem_write(a, x)
            self.uc.mem_write(b, y)
            return self._s(self.call(cmp, a, b))
        items.sort(key=functools.cmp_to_key(c))
        self.uc.mem_write(base, b"".join(items))
        self.heap.release(a)
        self.heap.release(b)
        return 0

    # ---------- printf family ----------
    def _format(self, fmt, nextarg):
        out, i = bytearray(), 0
        while i < len(fmt):
            ch = fmt[i]
            if ch != 0x25:
                out.append(ch)
                i += 1
                continue
            j = i + 1
            while j < len(fmt) and fmt[j] in b"-+ #0123456789.lhzjtq":
                j += 1
            spec, conv = fmt[i + 1:j].decode(), chr(fmt[j]) if j < len(fmt) else "%"
            i = j + 1
            flags = spec.replace("l", "").replace("h", "").replace("z", "").replace("q", "")
            if conv == "%":
                out += b"%"
                continue
            if conv == "*":
                continue
            if conv == "s":
                s = self.cstr(nextarg()) or b"(null)"
                out += (("%" + flags + "s") % s.decode("latin1")).encode("latin1")
            elif conv in "dixXuc":
                v = nextarg()
                if "ll" in spec or "q" in spec:
                    v |= nextarg() << 32
                    if conv in "di" and v & (1 << 63):
                        v -= 1 << 64
                elif conv in "di":
                    v = self._s(v)
                if conv == "u":
                    conv = "d"
                out += (("%" + flags + conv) % v).encode()
            elif conv == "p":
                out += b"0x%x" % nextarg()
            elif conv in "fgeE":
                # doubles are 8-byte aligned in varargs; caller handles alignment approx
                lo, hi = nextarg(), nextarg()
                out += (("%" + flags + conv) % struct.unpack("<d", struct.pack("<II", lo, hi))[0]).encode()
            else:
                out += b"?"
        return bytes(out)

    def _va(self, first_index=None, va_ptr=None):
        st = {"i": first_index, "p": va_ptr}

        def nxt():
            if st["p"] is not None:
                v = self.r32(st["p"])
                st["p"] += 4
                return v
            v = self.arg(st["i"])
            st["i"] += 1
            return v
        return nxt

    def _put(self, buf, size, s):
        if size:
            self.uc.mem_write(buf, s[:size - 1] + b"\0")
        return len(s)

    def f_vsnprintf(self):
        s = self._format(self.cstr(self.arg(2)), self._va(va_ptr=self.arg(3)))
        return self._put(self.arg(0), self.arg(1), s)

    def f___vsnprintf_chk(self):
        s = self._format(self.cstr(self.arg(4)), self._va(va_ptr=self.arg(5)))
        return self._put(self.arg(0), self.arg(1), s)

    def f___snprintf_chk(self):
        s = self._format(self.cstr(self.arg(4)), self._va(first_index=5))
        return self._put(self.arg(0), self.arg(1), s)

    def f___sprintf_chk(self):
        s = self._format(self.cstr(self.arg(3)), self._va(first_index=4))
        return self._put(self.arg(0), 1 << 20, s)

    def f_printf(self):
        sys.stderr.write("[engine] " + self._format(self.cstr(self.arg(0)), self._va(first_index=1)).decode("latin1"))
        return 0

    def f_putchar(self): return self.arg(0)
    def f_syslog(self): return 0

    # ---------- files ----------
    def _host_path(self, p):
        return p.decode()

    def _new_handle(self, obj):
        h = self.next_handle
        self.next_handle += 1
        self.files[h] = obj
        return h

    def f_fopen(self):
        path, mode = self._host_path(self.cstr(self.arg(0))), self.cstr(self.arg(1)).decode()
        if "w" in mode or "a" in mode:
            return 0
        try:
            f = open(path, "rb")
        except OSError:
            self.w32(self._data_sym("errno"), 2)
            return 0
        if self.trace:
            print(f"  fopen {path}", file=sys.stderr)
        fake = self.heap.alloc(0x60)
        h = self._new_handle(f)
        self.w32(fake, h)
        self.files[("FILE", fake)] = h
        return fake

    def _fp(self, fake): return self.files[self.files[("FILE", fake)]]

    def f_fread(self):
        buf, size, n, fp = self.arg(0), self.arg(1), self.arg(2), self.arg(3)
        data = self._fp(fp).read(size * n)
        if data:
            self.uc.mem_write(buf, data)
        return len(data) // size if size else 0

    def f_fseek(self):
        off, whence = self._s(self.arg(1)), self.arg(2)
        self._fp(self.arg(0)).seek(off, whence)
        return 0

    def f_ftell(self): return self._fp(self.arg(0)).tell()
    def f_ferror(self): return 0
    def f_fflush(self): return 0
    def f_fileno(self): return self.files[("FILE", self.arg(0))]

    def f_fwrite(self):
        return self.arg(2)

    def f_fclose(self):
        fake = self.arg(0)
        h = self.files.pop(("FILE", fake))
        self.files.pop(h).close()
        self.heap.release(fake)
        return 0

    def f_open(self):
        path = self._host_path(self.cstr(self.arg(0)))
        try:
            f = open(path, "rb")
        except OSError:
            self.w32(self._data_sym("errno"), 2)
            return 0xFFFFFFFF
        if self.trace:
            print(f"  open {path}", file=sys.stderr)
        return self._new_handle(f)

    def f_close(self):
        f = self.files.pop(self.arg(0), None)
        if f:
            f.close()
        return 0

    def _fill_stat(self, buf, size, isdir=False):
        # struct stat (64-bit inode layout): st_mode @4, st_size @60
        self.uc.mem_write(buf, b"\0" * 108)
        self.w16(buf + 4, 0o40755 if isdir else 0o100644)
        self.uc.mem_write(buf + 60, struct.pack("<Q", size))

    def f_stat(self):
        path = self._host_path(self.cstr(self.arg(0)))
        if not os.path.exists(path):
            self.w32(self._data_sym("errno"), 2)
            return 0xFFFFFFFF
        self._fill_stat(self.arg(1), os.path.getsize(path), os.path.isdir(path))
        return 0

    def f_fstat(self):
        f = self.files[self.arg(0)]
        self._fill_stat(self.arg(1), os.fstat(f.fileno()).st_size)
        return 0

    def f_mmap(self):
        addr, length, prot, flags, fd = (self.arg(i) for i in range(5))
        off = self.arg(5) | (self.arg(6) << 32)
        a = self.heap.alloc(length, align=4096)
        if fd != 0xFFFFFFFF and fd in self.files:
            f = self.files[fd]
            f.seek(off)
            self.uc.mem_write(a, f.read(length))
        else:
            self.uc.mem_write(a, b"\0" * length)
        return a

    def f_munmap(self): self.heap.release(self.arg(0)); return 0

    def f_vm_allocate(self):
        n = self.arg(2)
        a = self.heap.alloc(n, align=4096)
        self.uc.mem_write(a, b"\0" * n)
        self.w32(self.arg(1), a)
        return 0


DATA_IMPORTS = {"___stack_chk_guard", "_NDR_record", "___CFConstantStringClassReference",
                "__NSConcreteGlobalBlock", "__NSConcreteStackBlock"}
# every other data-ish import (kCF*, kVS*) gets a harmless data slot too
for _n in ("_kCFAllocatorDefault", "_kCFAllocatorNull", "_kCFBooleanFalse", "_kCFBooleanTrue",
           "_kCFNull", "_kCFRunLoopCommonModes", "_kCFRunLoopDefaultMode"):
    DATA_IMPORTS.add(_n)


class Vocalizer:
    VE_MSG_OUTBUFREQ = 0x8
    VE_MSG_OUTBUFDONE = 0x10

    def __init__(self, lang="en-US", trace=False):
        self.e = e = Emu(trace=trace)
        langdir = os.path.join(TTSRES, lang)
        commondir = os.path.join(TTSRES, "common")
        broker = open(os.path.join(langdir, "broker.hdr"), "rb").read()
        assert broker.startswith(b"defaultvoice:")
        first, xml = broker.split(b"\n", 1)
        self.voice = first[len(b"defaultvoice:"):].decode()
        self.xml = xml.decode("latin1")

        # Apple's "data class" object (see voiced 0x14270 / stream open 0x14034)
        dirs = e.heap.alloc(8)
        e.w32(dirs, e.new_cstr(langdir))
        e.w32(dirs + 4, e.new_cstr(commondir))
        dc = e.heap.alloc(0x1C)
        e.uc.mem_write(dc, b"\0" * 0x1C)
        e.w32(dc + 0x0, e.new_cstr(xml))
        e.w32(dc + 0x8, e.new_cstr(self.voice))
        e.w32(dc + 0xC, 0xFFFFFFFF)
        e.w32(dc + 0x14, 2)
        e.w32(dc + 0x18, dirs)

        # VE_INSTALL, laid out exactly as voiced builds it at 0x5c7e
        inst = e.heap.alloc(0x2C)
        e.uc.mem_write(inst, b"\0" * 0x2C)
        e.w16(inst + 0x00, 0x520)
        e.w32(inst + 0x04, e.r32(dc))
        e.w32(inst + 0x08, IFACE_HEAP)
        e.w32(inst + 0x10, IFACE_CRITSEC)
        e.w32(inst + 0x18, IFACE_STREAM)
        e.w32(inst + 0x1C, self._install_mapping_iface(dirs=[langdir, commondir]))
        e.w32(inst + 0x20, dc)

        self.hspeech = e.heap.alloc(8)
        e.uc.mem_write(self.hspeech, b"\0" * 8)
        self.check("ttsInitialize", e.call(vauto_ttsInitialize, inst, self.hspeech))

        self.hinst = e.heap.alloc(8)
        e.uc.mem_write(self.hinst, b"\0" * 8)
        hs0, hs1 = e.r32(self.hspeech), e.r32(self.hspeech + 4)
        self.check("ttsOpen", e.call(vauto_ttsOpen, hs0, hs1, 0, 0, self.hinst, 0))

        lang_name = self._xml_param("language")
        params = [(7, lang_name), (8, self.voice), (1, 22), (12, 1)]
        self.set_params(params)

        # audio callback: a trap that Python services
        self.cb_addr = TRAP_BASE + 0x8000
        e.traps[self.cb_addr] = ("<outnotify>", self._outnotify)
        outdev = e.heap.alloc(8)
        e.w32(outdev, 0)
        e.w32(outdev + 4, self.cb_addr | 1)
        self.check("ttsSetOutDevice", e.call(vauto_ttsSetOutDevice, *self._h(), outdev))

        self.pcm_buf_size = 0x2000
        self.pcm_buf = e.heap.alloc(self.pcm_buf_size)
        self.mark_buf = e.heap.alloc(0x500 * 0x20)
        self.pcm = bytearray()

    # Replacement for Apple's data-mapping interface (voiced 0x146a0..), which
    # caches mappings in CFDictionaries. Same contract: open/close/map/unmap/freeze.
    def _install_mapping_iface(self, dirs):
        e = self.e
        self.maps = {}

        def resolve(name):
            name = name.replace("/", "_")
            if "." not in name:
                name += ".dat"
            for d in dirs:
                p = os.path.join(d, name)
                if os.path.exists(p):
                    return p
            return None

        def m_open():
            name, out = e.cstr(e.arg(2)).decode(), e.arg(3)
            path = resolve(name)
            if e.trace:
                print(f"  map-open {name} -> {path}", file=sys.stderr)
            if not path or not out:
                return 0x80000014
            data = open(path, "rb").read()
            base = e.heap.alloc(len(data) + 16, align=4096)
            e.uc.mem_write(base, data)
            h = e.heap.alloc(16)
            self.maps[h] = (base, len(data))
            e.w32(out, h)
            return 0

        def m_close():
            h = e.arg(0)
            base, _ = self.maps.pop(h, (0, 0))
            e.heap.release(base)
            e.heap.release(h)
            return 0

        def m_map():
            h, off, pcb, ppmem = e.arg(0), e.arg(1), e.arg(2), e.arg(3)
            base, size = self.maps[h]
            if off >= size:
                e.w32(ppmem, 0)
                if pcb:
                    e.w32(pcb, 0)
                return 0
            avail = size - off
            if pcb:
                req = e.r32(pcb)
                if req and req < avail:
                    avail = req
                e.w32(pcb, avail)
            e.w32(ppmem, base + off)
            return 0

        table = e.heap.alloc(20)
        for i, fn in enumerate((m_open, m_close, m_map, lambda: 0, lambda: 0)):
            addr = TRAP_BASE + 0x8100 + 4 * i
            e.traps[addr] = (f"<mapping{i}>", fn)
            e.w32(table + 4 * i, addr | 1)
        return table

    def _xml_param(self, tag):
        s = self.xml
        return s.split(f"<{tag}>")[1].split(f"</{tag}>")[0]

    def _h(self):
        return self.e.r32(self.hinst), self.e.r32(self.hinst + 4)

    def check(self, what, rc):
        if rc:
            raise RuntimeError(f"{what} failed: {rc:#010x}")

    def set_params(self, params):
        e = self.e
        n = len(params)
        p = e.heap.alloc(0x84 * n)
        e.uc.mem_write(p, b"\0" * (0x84 * n))
        for i, (pid, val) in enumerate(params):
            e.w32(p + 0x84 * i, pid)
            if isinstance(val, str):
                e.uc.mem_write(p + 0x84 * i + 4, val.encode()[:127] + b"\0")
            else:
                e.w16(p + 0x84 * i + 4, val)
        self.check("ttsSetParamList", e.call(vauto_ttsSetParamList, *self._h(), p, n))

    def _outnotify(self):
        e = self.e
        msg = e.arg(3)
        kind, param = e.r32(msg), e.r32(msg + 8)
        if kind == self.VE_MSG_OUTBUFREQ and param:
            # VE_OUTDATA: {format, cntPcmBufLen, pOutPcmBuf, cntMrkListLen, pMrkList}
            e.w32(param + 4, self.pcm_buf_size)
            e.w32(param + 8, self.pcm_buf)
            e.w32(param + 0xC, 0x500)
            e.w32(param + 0x10, self.mark_buf)
        elif kind == self.VE_MSG_OUTBUFDONE and param:
            n = e.r32(param + 4)
            if n:
                self.pcm += e.uc.mem_read(e.r32(param + 8), n)
        elif e.trace:
            print(f"  callback msg {kind:#x}", file=sys.stderr)
        return 0

    def speak(self, text):
        e = self.e
        self.pcm = bytearray()
        data = text.encode("utf-16-le")
        buf = e.heap.alloc(len(data) + 2)
        e.uc.mem_write(buf, data + b"\0\0")
        intext = e.heap.alloc(12)
        e.w32(intext, 0)
        e.w32(intext + 4, len(data))
        e.w32(intext + 8, buf)
        rc = e.call(vauto_ttsProcessText2Speech, *self._h(), intext)
        if rc not in (0, 0x80000807):
            raise RuntimeError(f"ttsProcessText2Speech failed: {rc:#010x}")
        e.heap.release(buf)
        e.heap.release(intext)
        return bytes(self.pcm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("out")
    ap.add_argument("--lang", default="en-US")
    ap.add_argument("--trace", action="store_true")
    a = ap.parse_args()
    v = Vocalizer(a.lang, trace=a.trace)
    pcm = v.speak(a.text)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(22050)
        w.writeframes(pcm)
    print(f"wrote {a.out}: {len(pcm) // 2 / 22050:.2f}s of audio ({v.voice})")


if __name__ == "__main__":
    main()
