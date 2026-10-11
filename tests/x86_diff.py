#!/usr/bin/env python3
"""Differential test of core/x86 against Unicorn: every distinct instruction
encoding in the given i386 binaries runs from random states in both; integer
registers, defined flags, SSE registers, x87 registers and memory must match.

Usage: x86_diff.py [--trials N] [--only MNEMONIC] <binary>...
"""
import argparse
import ctypes
import math
import random
import re
import struct
import subprocess
import sys

from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn.x86_const import *

MEM = 0x80000000          # = X86_MEM_SIZE (reserved, untouched pages stay unbacked)
CODE = 0x1000
DATA = 0x00100000
DSIZE = 0x30000

# Never meaningful in user code / data mis-disassembled as code / not modelled.
SKIP = re.compile(r"^(in|out|ins|outs|int|int3|into|iret|hlt|cli|sti|sldt|lds|les|lfs|lgs|lss|bound|arpl|aaa|aas|aam|aad|"
                  r"daa|das|xlat|salc|ljmp|lcall|lret|wait|fwait|ud2|cpuid|rdtsc|enter|push[sd]?[cdefgs]s|pop[cdefgs]s|"
                  r"pushf|popf|sahf|lahf|loop|jecxz|mov[lw]?\s+%[cdefgs]s|cmc|std|cld|stc|clc|\.byte|nop|lock|rep|repne|"
                  r"emms|movq\s+%mm|.*%mm\d|fn?stenv|fn?save|frstor|fldenv|fxsave|fxrstor|syscall|sysenter|lret)")

ARITH = re.compile(r"^(and|or|xor|test)")
SHIFTS = re.compile(r"^(shl|shr|sal|sar|rol|ror|rcl|rcr|shld|shrd)")
MULS = re.compile(r"^(mul|imul)")
DIVS = re.compile(r"^(div|idiv)")
BITS = re.compile(r"^(bt|bts|btr|btc)")
BSX = re.compile(r"^(bsf|bsr)")

CF, PF, AF, ZF, SF, DF, OF = 1, 4, 16, 64, 128, 1024, 2048
ALLF = CF | PF | AF | ZF | SF | DF | OF


def flag_mask(mn):
    m = ALLF
    if ARITH.match(mn):
        m &= ~AF
    if SHIFTS.match(mn):
        m &= ~(OF | AF)
    if MULS.match(mn):
        m &= ~(SF | ZF | AF | PF)
    if DIVS.match(mn):
        m = 0
    if BITS.match(mn):
        m &= ~(OF | SF | AF | PF | ZF)
    if BSX.match(mn):
        m &= ~(CF | OF | SF | AF | PF)
    return m


def collect(paths):
    seen = {}
    for p in paths:
        out = subprocess.run(["xcrun", "llvm-objdump", "-d", "--arch=i386", p], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r"\s+[0-9a-f]+:\t([0-9a-f]{2}(?: [0-9a-f]{2})*)\t(\S+)\s*(.*)", line)
            if not m:
                continue
            enc = bytes.fromhex(m.group(1).replace(" ", ""))
            mn = m.group(2)
            text = (mn + " " + m.group(3)).strip()
            if SKIP.match(text) or "(bad)" in text or "<unknown>" in text:
                continue
            seen.setdefault(enc, text)
    return seen


def f80(t):
    """Unicorn's (mantissa, exponent) x87 register -> float."""
    mant, exp = t
    sign = -1.0 if exp & 0x8000 else 1.0
    e = exp & 0x7FFF
    if e == 0x7FFF:
        return float("nan") if (mant << 1) & 0xFFFFFFFFFFFFFFFF else sign * float("inf")
    if mant == 0:
        return sign * 0.0
    try:
        return sign * math.ldexp(mant, e - 16383 - 63)
    except OverflowError:
        return sign * float("inf")  # beyond double range: ours holds doubles


def to_f80(v):
    if v != v:
        return (0xC000000000000000, 0x7FFF)
    if math.isinf(v):
        return (0x8000000000000000, 0x7FFF | (0x8000 if v < 0 else 0))
    if v == 0:
        return (0, 0x8000 if math.copysign(1, v) < 0 else 0)
    f, e = math.frexp(abs(v))
    return (int(math.ldexp(f, 64)), (e - 1 + 16383) | (0x8000 if v < 0 else 0))


def rand_float_bits():
    k = random.random()
    if k < 0.15:
        v = random.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.5, 3.5, -2.5, 1e30, -1e-30, 65536.0, 1e10, 2147483647.0, -2147483649.0])
    else:
        v = random.uniform(-1000, 1000) if k < 0.7 else random.uniform(-1, 1) * 10 ** random.randint(-6, 9)
    return v


def rand_xmm():
    k = random.random()
    if k < 0.3:
        return struct.pack("<4f", *[rand_float_bits() for _ in range(4)])
    if k < 0.6:
        return struct.pack("<2d", rand_float_bits(), rand_float_bits())
    return bytes(random.getrandbits(8) for _ in range(16))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", type=int, default=30)
    ap.add_argument("--only")
    ap.add_argument("-v", action="store_true")
    ap.add_argument("bins", nargs="+")
    a = ap.parse_args()
    global VERBOSE
    VERBOSE = a.v

    lib = ctypes.CDLL("build/libx86step.dylib")
    lib.step.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint64),
                         ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_int), ctypes.c_void_p]
    libc = ctypes.CDLL(None)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    mem = libc.mmap(None, MEM, 3, 0x1002, -1, 0)  # PROT_READ|WRITE, MAP_PRIVATE|MAP_ANON

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(CODE, 0x1000)
    uc.mem_map(DATA, DSIZE)

    global BLOBS
    BLOBS = []
    for _ in range(6):
        blob = bytearray(random.getrandbits(8) for _ in range(DSIZE))
        for _ in range(4000):  # sprinkle floats so x87/SSE memory operands see sane values
            off = random.randrange(0, DSIZE - 16, 4)
            blob[off:off + 8] = struct.pack("<f", rand_float_bits()) * 2 if random.random() < 0.5 else struct.pack("<d", rand_float_bits())
        BLOBS.append(bytes(blob))
    encs = collect(a.bins)
    if a.only:
        encs = {k: v for k, v in encs.items() if v.split()[0] == a.only}
    print(f"{len(encs)} distinct encodings")
    ok, unsupported, mism = 0, [], []
    for enc, text in sorted(encs.items(), key=lambda kv: kv[1]):
        mn = text.split()[0]
        fmask = flag_mask(mn)
        bad = None
        ran = 0
        for t in range(a.trials):
            regs = [random.choice([DATA + random.randrange(0x1000, DSIZE - 0x1000, 4), DATA + random.randrange(0x1000, DSIZE - 0x1000, 16),
                                   random.getrandbits(32), random.randint(-300, 300) & 0xFFFFFFFF, random.randint(0, 40)])
                    for _ in range(8)]
            regs[ESP_I] = DATA + 0x20000 + random.randrange(0, 0x100, 4)
            eflags = 0x202 | (random.getrandbits(1) * CF) | (random.getrandbits(1) * PF) | (random.getrandbits(1) * AF) | \
                     (random.getrandbits(1) * ZF) | (random.getrandbits(1) * SF) | (random.getrandbits(1) * OF)
            xmm = [rand_xmm() for _ in range(8)]
            st = [rand_float_bits() for _ in range(8)]
            blob = random.choice(BLOBS)

            # ours
            ctypes.memmove(mem + DATA, blob, DSIZE)
            ctypes.memmove(mem + CODE, enc + b"\x90" * 16, len(enc) + 16)
            r = (ctypes.c_uint32 * 10)(*(regs + [CODE, eflags]))
            xq = (ctypes.c_uint64 * 16)(*struct.unpack("<16Q", b"".join(xmm)))
            sd = (ctypes.c_double * 8)(*st)
            top = ctypes.c_int(0)
            rc = lib.step(r, xq, sd, ctypes.byref(top), mem)

            # unicorn
            uc.mem_write(CODE, enc + b"\x90" * 16)
            uc.ctl_flush_tb()  # Unicorn otherwise keeps running the previous encoding at CODE
            uc.mem_write(DATA, blob)
            for i, reg in enumerate(REGS):
                uc.reg_write(reg, regs[i])
            uc.reg_write(UC_X86_REG_EFLAGS, eflags)
            for i in range(8):
                uc.reg_write(UC_X86_REG_XMM0 + i, int.from_bytes(xmm[i], "little"))
                uc.reg_write(UC_X86_REG_FP0 + i, to_f80(st[i]))
            uc.reg_write(UC_X86_REG_FPSW, 0)
            uc.reg_write(UC_X86_REG_FPTAG, 0)
            uc.reg_write(UC_X86_REG_FPCW, 0x037F)
            try:
                uc.emu_start(CODE, CODE + 0x100, count=1)
                urc = 0
            except UcError:
                urc = 1
            if rc == 1 and urc == 1:
                continue
            if rc == 1:
                msg = ctypes.c_char_p.in_dll(lib, "x") if False else None
                bad = "ours faulted, unicorn did not"
                break
            if urc == 1:
                continue  # touched memory Unicorn hasn't mapped (ours maps all 2 GB)
            ran += 1
            diffs = []
            ur = [uc.reg_read(reg) for reg in REGS] + [uc.reg_read(UC_X86_REG_EIP), uc.reg_read(UC_X86_REG_EFLAGS)]
            for i in range(9):
                if r[i] != ur[i]:
                    diffs.append(f"{NAMES[i]} {r[i]:#x} vs {ur[i]:#x}")
            if (r[9] ^ ur[9]) & fmask:
                diffs.append(f"flags {r[9] & fmask:#x} vs {ur[9] & fmask:#x}")
            for i in range(8):
                ux = uc.reg_read(UC_X86_REG_XMM0 + i)
                ox = xq[2 * i] | xq[2 * i + 1] << 64
                if ux != ox:
                    diffs.append(f"xmm{i} {ox:032x} vs {ux:032x}")
            fpsw = uc.reg_read(UC_X86_REG_FPSW)
            utop = (fpsw >> 11) & 7
            if utop != top.value:
                diffs.append(f"fp top {top.value} vs {utop}")
            for i in range(8):
                uv = f80(uc.reg_read(UC_X86_REG_FP0 + i))
                ov = sd[i]
                same = (uv != uv and ov != ov) or uv == ov or (abs(uv - ov) <= abs(uv) * 1e-15)
                if not same:
                    diffs.append(f"st[{i}] {ov!r} vs {uv!r}")
            ubytes = bytes(uc.mem_read(DATA, DSIZE))
            obytes = ctypes.string_at(mem + DATA, DSIZE)
            if ubytes != obytes:
                k = next(i for i in range(DSIZE) if ubytes[i] != obytes[i])
                diffs.append(f"memory @{DATA + k:#x}: {obytes[k]:02x} vs {ubytes[k]:02x}")
            if diffs:
                bad = "; ".join(diffs[:4]) + (f"  [in: {[hex(x) for x in regs]}]" if VERBOSE else "")
                break
        if bad is None:
            ok += 1
        else:
            mism.append((enc.hex(), text, bad))
    print(f"ok {ok}, mismatched {len(mism)}")
    for m in mism[:80]:
        print(f"  {m[0]:24} {m[1]:44} {m[2]}")


VERBOSE = False
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
        UC_X86_REG_ESI, UC_X86_REG_EDI]
NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip"]
ESP_I = 4

if __name__ == "__main__":
    main()
