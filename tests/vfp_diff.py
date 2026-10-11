#!/usr/bin/env python3
"""Differential test: every distinct VFP encoding in a voiced binary is run
through core/thumb_ops.h (via build/libvfpstep.dylib) and Unicorn on random
states; registers, flags, FPSCR NZCV and memory must match.

Usage: vfp_diff.py <voiced.dis> [trials]
"""
import ctypes
import random
import re
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UcError
from unicorn.arm_const import *

MEM = 0x40000000  # = GUEST_MEM_SIZE
CODE = 0x1000
DATA = 0x100000

lib = ctypes.CDLL("build/libvfpstep.dylib")
lib.step.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint64),
                     ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
host_mem = ctypes.create_string_buffer(MEM)

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
uc.mem_map(0, 0x200000)
uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

DREGS = [UC_ARM_REG_D0 + i for i in range(32)]


def rand_double():
    k = random.random()
    if k < 0.1:
        return random.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e10, -1e-10, 32767.0, -32768.5, 2.5, 3.5])
    if k < 0.5:
        return random.uniform(-1000, 1000)
    if k < 0.7:
        return float(random.randint(-100000, 100000))
    return random.uniform(-1, 1) * 10 ** random.randint(-5, 9)


def rand_dreg():
    # mix: a double, or two floats packed (for single-precision ops)
    if random.random() < 0.5:
        return struct.unpack("<Q", struct.pack("<d", rand_double()))[0]
    f1, f2 = rand_double(), rand_double()
    return struct.unpack("<Q", struct.pack("<ff", max(min(f1, 3e38), -3e38), max(min(f2, 3e38), -3e38)))[0]


def collect(dis):
    encs = {}
    for line in open(dis):
        m = re.match(r"\s+([0-9a-f]+):\s+(v\S*)", line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        if addr < 0x13000:
            continue
        encs.setdefault(None, None)
    # read encodings straight from the binary via the address list
    return encs


def main():
    dis = sys.argv[1]
    trials = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    binpath = sys.argv[3]
    data = open(binpath, "rb").read()
    addrs = []
    for line in open(dis):
        m = re.match(r"\s+([0-9a-f]+):\s+(v[a-z0-9.]*)\s*(.*)", line)
        if m and int(m.group(1), 16) >= 0x13000:
            addrs.append((int(m.group(1), 16), m.group(2) + " " + m.group(3)))
    seen = {}
    for a, text in addrs:
        off = a - 0x1000
        hw1, hw2 = struct.unpack_from("<HH", data, off)
        if (hw1 >> 11) < 0x1D:
            continue
        seen.setdefault((hw1, hw2), (a, text))
    print(f"{len(seen)} distinct 32-bit VFP/NEON encodings")
    unsupported, mismatches, ok = [], [], 0
    for (hw1, hw2), (a, text) in sorted(seen.items(), key=lambda kv: kv[1][0]):
        bad = None
        for t in range(trials):
            regs = [random.choice([DATA + random.randrange(0, 0x8000, 4), random.getrandbits(32),
                                   random.randint(-70000, 70000) & 0xFFFFFFFF]) for _ in range(13)]
            regs += [DATA + 0x10000, 0x2001, 0]  # sp, lr, pc (set below)
            flags = [random.getrandbits(1) for _ in range(4)]
            fpscr = random.getrandbits(4) << 28
            d = [rand_dreg() for _ in range(32)]
            blob = bytes(random.getrandbits(8) for _ in range(0x20000))
            # host side
            ctypes.memmove(ctypes.addressof(host_mem) + DATA, blob, len(blob))
            r = (ctypes.c_uint32 * 21)(*(regs[:15] + [CODE + 4] + flags + [fpscr]))
            dd = (ctypes.c_uint64 * 32)(*d)
            rc = lib.step(r, dd, host_mem, hw1, hw2, CODE)
            if rc == 1:
                continue  # host bounds check (same situation)
            if rc == 2:
                unsupported.append((a, text, hw1, hw2))
                bad = "unsupported"
                break
            # unicorn side
            uc.mem_write(CODE, struct.pack("<HH", hw1, hw2) + b"\x00\xbf" * 4)
            uc.ctl_flush_tb()  # Unicorn may otherwise run the previous encoding cached at CODE
            uc.mem_write(DATA, blob)
            for i in range(13):
                uc.reg_write(UC_ARM_REG_R0 + i, regs[i])
            uc.reg_write(UC_ARM_REG_SP, regs[13])
            uc.reg_write(UC_ARM_REG_LR, regs[14])
            cpsr = uc.reg_read(UC_ARM_REG_CPSR) & 0x0FFFFFFF
            cpsr |= (flags[0] << 31) | (flags[1] << 30) | (flags[2] << 29) | (flags[3] << 28)
            uc.reg_write(UC_ARM_REG_CPSR, cpsr | 0x20)
            uc.reg_write(UC_ARM_REG_FPSCR, fpscr)
            for i in range(32):
                uc.reg_write(DREGS[i], d[i])
            try:
                uc.emu_start(CODE | 1, CODE + 4, count=1)
            except UcError:
                continue  # random base register pointed outside mapped memory
            u_regs = [uc.reg_read(UC_ARM_REG_R0 + i) for i in range(13)] + [uc.reg_read(UC_ARM_REG_SP), uc.reg_read(UC_ARM_REG_LR)]
            u_cpsr = uc.reg_read(UC_ARM_REG_CPSR)
            u_flags = [(u_cpsr >> 31) & 1, (u_cpsr >> 30) & 1, (u_cpsr >> 29) & 1, (u_cpsr >> 28) & 1]
            u_fpscr = uc.reg_read(UC_ARM_REG_FPSCR) & 0xF0000000
            u_d = [uc.reg_read(DREGS[i]) for i in range(32)]
            u_mem = bytes(uc.mem_read(DATA, len(blob)))
            h_mem = ctypes.string_at(ctypes.addressof(host_mem) + DATA, len(blob))
            diffs = []
            if list(r[:15]) != u_regs:
                diffs.append(f"r {[hex(x) for x in r[:15]]} vs {[hex(x) for x in u_regs]}")
            if list(r[16:20]) != u_flags:
                diffs.append(f"flags {list(r[16:20])} vs {u_flags}")
            if (r[20] & 0xF0000000) != u_fpscr:
                diffs.append(f"fpscr {r[20] >> 28:x} vs {u_fpscr >> 28:x}")
            for i in range(32):
                if dd[i] != u_d[i]:
                    diffs.append(f"d{i} {dd[i]:016x} vs {u_d[i]:016x}")
            if h_mem != u_mem:
                diffs.append("memory")
            if diffs:
                bad = "; ".join(diffs[:3])
                break
        if bad is None:
            ok += 1
        elif bad != "unsupported":
            mismatches.append((a, text, hw1, hw2, bad))
    print(f"ok {ok}, mismatched {len(mismatches)}, unsupported {len(unsupported)}")
    for m in mismatches[:25]:
        print(f"  MISMATCH {m[0]:#x} {m[2]:04x} {m[3]:04x} {m[1]}: {m[4]}")
    for u in unsupported[:40]:
        print(f"  unsupported {u[0]:#x} {u[2]:04x} {u[3]:04x} {u[1]}")


if __name__ == "__main__":
    main()
