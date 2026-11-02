#!/usr/bin/env python3
"""Static recompiler: Thumb-2 functions in `voiced` -> C.

Each selected function becomes `int nf_XXXX(Cpu *c, uint32_t start)`. Every
guest instruction is emitted as a call to the interpreter's own semantics
(exec16/exec32 from core/thumb_ops.h) with constant encodings, which clang
inlines and constant-folds, so the translation cannot drift from the
interpreter. Guest registers live in a local Core that clang promotes to
machine registers; it is spilled to the Cpu only around calls and exits.

Usage: recomp.py <voiced> <profile.bin|profile.txt> <outdir> [--prefix ios6] [--per-file N]
"""
import argparse
import os
import struct
from collections import defaultdict

DIV_HELPERS = {
    "___divsi3": "K.r[0] = rt_sdiv(K.r[0], K.r[1]);",
    "___modsi3": "K.r[0] = rt_smod(K.r[0], K.r[1]);",
    "___udivsi3": "K.r[0] = K.r[1] ? K.r[0] / K.r[1] : 0;",
    "___umodsi3": "K.r[0] = K.r[1] ? K.r[0] % K.r[1] : 0;",
}

# Guest functions whose calls become host code (core/recomp_rt.h), by symbol,
# in the builds whose code was checked against them: iOS 7.0.6's and 7.1.2's
# engines (the same code). log_HasTraceTuningDataSubscriber asks the engine's
# loggers, through up to six indirect calls, whether any wants tuning traces;
# none does here (its last logger's level check always says no), so it is 0.
HOST_FUNCS = {
    "_cstdlib_strlen": "rt_strlen(&K);",
    "_cstdlib_strchr": "rt_strchr(&K);",
    "_cstdlib_strcmp": "rt_strcmp(&K);",
    "_cstdlib_strncmp": "rt_strncmp(&K);",
    "_cstdlib_strstr": "rt_strstr(&K);",
    "_cstdlib_memcpy": "rt_memcpy(&K);",
    "_cstdlib_memmove": "rt_memmove(&K);",
    "_log_HasTraceTuningDataSubscriber": "K.r[0] = 0;",
    "_fxd_S32ShMultRndS32S32": "rt_fxd_mult_rnd(&K);",
}
HOST_FUNC_BUILDS = ("ios70", "ios7")
# While a stop is pending (Cpu.interrupt, vx_request_stop), these functions
# are answered by host code instead of being called: a long sentence's parse
# (Vocalizer 5.2's mosynt) takes up to 210 ms and the engine looks at its stop
# only after it. Every unification failing at once, as mosyntknowl_Unif2's own
# failure path does (the bindings it recorded undone, "no match" written
# through its seventh argument, 0 returned), leaves the parse nothing to build
# on: it ends within a few hundred steps, through its normal path, and frees
# what it made. (The engine's interrupt hooks, mosyntpal_CheckInterrupt, end
# it through the error path, which leaks ~100 KB a sentence.) Checked against
# 7.0.6's and 7.1.2's code (HOST_FUNC_BUILDS); Mavericks' differs.
INTERRUPT_FUNCS = {
    "_mosyntknowl_Unif2": [
        "{   // recomp.py INTERRUPT_FUNCS: mosyntknowl_Unif2 failing",
        "    uint32_t w = K.r[3];",
        "    if (w && (int32_t)g32(&K, w + 0x258) >= 1) {",
        "        uint32_t i = 0, v;",
        "        do { v = g32(&K, w + 0x25c + 4 * i); i++; s32(&K, w + 4 * v, v); } while ((int32_t)i < (int32_t)g32(&K, w + 0x258));",
        "    }",
        "    if (w) s32(&K, w + 0x258, 0);",
        "    s8(&K, g32(&K, K.r[13] + 0x18), 0);",
        "    K.r[0] = 0;",
        "}",
    ],
}
interrupt_at = {}  # guest address -> host code while a stop is pending
host_at = {}  # guest address -> host code, for the build being translated
leaf_of = {}  # guest address -> its leaf function's name (emit_leaf)


class MachO:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        magic, _, _, _, ncmds = struct.unpack_from("<IiiII", d, 0)
        assert magic == 0xFEEDFACE
        self.segs, self.syms, self.text = [], [], None
        stubs = lazy = None
        off = 28
        symoff = nsyms = stroff = indoff = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", d, off)
            if cmd == 1:
                vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = struct.unpack_from("<8I", d, off + 24)
                self.segs.append((vmaddr, fileoff, filesize))
                for s in range(nsects):
                    so = off + 56 + 68 * s
                    sect = d[so:so + 16].rstrip(b"\0").decode()
                    addr, ssize, _, _, _, _, flags, r1, r2 = struct.unpack_from("<9I", d, so + 32)
                    if sect == "__text":
                        self.text = (addr, addr + ssize)
                    if flags & 0xFF == 8:
                        stubs = (addr, ssize, r1, r2)
                    if flags & 0xFF == 7:
                        lazy = (addr, ssize, r1)
            elif cmd == 2:
                symoff, nsyms, stroff, _ = struct.unpack_from("<4I", d, off + 8)
            elif cmd == 0xB:
                indoff, nind = struct.unpack_from("<II", d, off + 56)
            off += size
        names = []
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, value = struct.unpack_from("<IBBhI", d, symoff + 12 * i)
            end = d.index(b"\0", stroff + strx)
            nm = d[stroff + strx:end].decode("latin1")
            names.append(nm)
            if ntype & 0x0E == 0x0E and nsect and self.text and self.text[0] <= value < self.text[1]:
                self.syms.append((value & ~1, nm))
        self.syms = sorted(set(self.syms))
        self.stub_names = {}
        if stubs:
            addr, ssize, r1, r2 = stubs
            for k in range(ssize // r2):
                ind = struct.unpack_from("<I", d, indoff + 4 * (r1 + k))[0]
                if not ind & 0xC0000000:
                    self.stub_names[addr + k * r2] = names[ind]
        self.stub_range = (stubs[0], stubs[0] + stubs[1]) if stubs else (0, 0)

    def read(self, addr, n):
        for vm, fo, fs in self.segs:
            if vm <= addr and addr + n <= vm + fs:
                return self.d[fo + addr - vm: fo + addr - vm + n]
        raise ValueError(hex(addr))

    def h16(self, addr):
        return struct.unpack("<H", self.read(addr, 2))[0]


def scan_bl(mo):
    """BL targets in __text, found the way llvm-objdump's linear sweep sees
    them (an IT block's conditional BLs left out, as they print as "blne" ...):
    the function boundaries of a stripped engine, without needing objdump."""
    lo, hi = mo.text
    data = mo.read(lo, hi - lo)
    out = set()
    a, n = 0, len(data)
    it_left = 0  # instructions left in an IT block whose condition isn't "always"
    while a + 2 <= n:
        hw1 = data[a] | data[a + 1] << 8
        in_it = it_left > 0
        if in_it:
            it_left -= 1
        if hw1 & 0xFF00 == 0xBF00 and hw1 & 0xF:  # IT
            mask = hw1 & 0xF
            it_left = 0 if hw1 >> 4 & 0xF == 0xE else 5 - (mask & -mask).bit_length()
            a += 2
            continue
        if hw1 >> 11 in (0x1D, 0x1E, 0x1F) and a + 4 <= n:  # 32-bit
            hw2 = data[a + 2] | data[a + 3] << 8
            if hw1 >> 11 == 0x1E and hw2 & 0xD000 == 0xD000 and not in_it:  # BL
                s = hw1 >> 10 & 1
                i1, i2 = 1 ^ (hw2 >> 13 & 1 ^ s), 1 ^ (hw2 >> 11 & 1 ^ s)
                t = lo + a + 4 + sext(s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1, 25)
                if lo <= t < hi:
                    out.add(t)
            a += 4
        else:
            a += 2
    return out


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


class Insn:
    __slots__ = ("addr", "hw1", "hw2", "size", "kind", "target", "cond", "in_it", "stub")

    def __init__(self, addr, hw1, hw2, size):
        self.addr, self.hw1, self.hw2, self.size = addr, hw1, hw2, size
        self.kind = "op"     # op | it | b | bcond | bl | blx_stub | icall | ijump | stop
        self.target = None
        self.cond = None     # IT condition (None = always)
        self.in_it = 0
        self.stub = None


def decode(mo, addr):
    hw1 = mo.h16(addr)
    if (hw1 >> 11) >= 0x1D:
        hw2 = mo.h16(addr + 2)
        ins = Insn(addr, hw1, hw2, 4)
        if (hw1 & 0xF800) == 0xF000 and hw2 & 0x8000:
            s, j1, j2 = (hw1 >> 10) & 1, (hw2 >> 13) & 1, (hw2 >> 11) & 1
            kind = hw2 & 0x5000
            if kind == 0:
                cond = (hw1 >> 6) & 15
                if cond & 0xE == 0xE:
                    return ins  # misc control (hints, barriers)
                imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((hw1 & 0x3F) << 12) | ((hw2 & 0x7FF) << 1)
                ins.kind, ins.target = "bcond", (addr + 4 + sext(imm, 21)) & 0xFFFFFFFF
                return ins
            i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
            imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1)
            off = sext(imm, 25)
            if kind == 0x1000:
                ins.kind, ins.target = "b", (addr + 4 + off) & 0xFFFFFFFF
            elif kind == 0x5000:
                ins.kind, ins.target = "bl", (addr + 4 + off) & 0xFFFFFFFF
            else:
                t = ((addr + 4) & ~3) + off & 0xFFFFFFFF
                if mo.stub_range[0] <= t < mo.stub_range[1]:
                    ins.kind, ins.target, ins.stub = "blx_stub", t, mo.stub_names.get(t)
                else:
                    ins.kind = "icall"  # BLX to ARM code: let the dispatcher handle it
            return ins
        if (hw1 & 0xFE40) == 0xE800 and hw1 & 0x10 and hw2 & 0x8000:
            ins.kind = "ijump"  # LDM with PC
        elif (hw1 & 0xFFF0) == 0xE8D0 and (hw2 & 0xFFE0) == 0xF000:
            ins.kind = "tb"
        elif (hw1 & 0xFE00) == 0xF800 and hw1 & 0x10 and (hw1 >> 5) & 3 == 2 and hw2 >> 12 == 15:
            ins.kind = "ijump"  # LDR PC
        elif (hw1 & 0xFE00) == 0xEA00 and (hw2 >> 8) & 15 == 15 and not (hw1 & 0x10):
            ins.kind = "ijump"  # data-processing into PC
        return ins
    ins = Insn(addr, hw1, 0, 2)
    if (hw1 & 0xF000) == 0xD000:
        cond = (hw1 >> 8) & 15
        if cond >= 14:
            ins.kind = "stop"
        else:
            ins.kind, ins.target = "bcond", (addr + 4 + sext((hw1 & 0xFF) << 1, 9)) & 0xFFFFFFFF
    elif (hw1 & 0xF800) == 0xE000:
        ins.kind, ins.target = "b", (addr + 4 + sext((hw1 & 0x7FF) << 1, 12)) & 0xFFFFFFFF
    elif (hw1 & 0xF500) == 0xB100:
        off = (((hw1 >> 9) & 1) << 6) | (((hw1 >> 3) & 31) << 1)
        ins.kind, ins.target = "bcond", addr + 4 + off
    elif (hw1 & 0xFF00) == 0x4700:
        ins.kind = "icall" if hw1 & 0x80 else "ijump"
    elif (hw1 & 0xFF00) == 0xBD00:
        ins.kind = "ijump"
    elif (hw1 & 0xFC00) == 0x4400 and ((hw1 >> 8) & 3) in (0, 2) and (((hw1 >> 4) & 8) | (hw1 & 7)) == 15:
        ins.kind = "ijump"
    elif (hw1 & 0xFF00) == 0xBF00 and hw1 & 0xF:
        ins.kind = "it"
    elif (hw1 & 0xFF00) == 0xBE00:
        ins.kind = "stop"
    return ins


def tb_targets(mo, ins):
    """Decode a TBB/TBH table that directly follows the instruction."""
    base = ins.addr + 4
    half = ins.hw2 & 0x10
    if (ins.hw1 & 15) != 15:
        return []  # table not PC-relative
    targets, pos, end = [], base, None
    while end is None or pos < end:
        if half:
            e = mo.h16(pos); pos += 2
        else:
            e = mo.read(pos, 1)[0]; pos += 1
        t = base + 2 * e
        targets.append(t)
        end = t if end is None else min(end, t)
        if len(targets) > 4096:
            break
    return targets


class Function:
    def __init__(self, start, end):
        self.start, self.end = start, end
        self.insns = {}
        self.roots = set()
        self.branch_targets = set()
        self.return_sites = set()


def explore(mo, fn):
    work = sorted(fn.roots)
    it_cond = {}  # addr -> (cond, in_it)
    while work:
        a = work.pop()
        if a in fn.insns or not (fn.start <= a < fn.end):
            continue
        try:
            ins = decode(mo, a)
        except ValueError:
            continue
        if a in it_cond:
            ins.cond, ins.in_it = it_cond[a]
        fn.insns[a] = ins
        nxt = a + ins.size
        if ins.kind == "it":
            state, p = ins.hw1 & 0xFF, nxt
            while state & 0xF:
                it_cond[p] = (state >> 4, 1)
                state = ((state & 0xE0) | ((state << 1) & 0x1F)) if state & 7 else 0
                try:
                    p += decode(mo, p).size
                except ValueError:
                    break
        conditional = ins.cond is not None and ins.cond != 14
        if ins.kind in ("b", "bcond") and ins.target is not None:
            fn.branch_targets.add(ins.target)
            if fn.start <= ins.target < fn.end:
                work.append(ins.target)
        if ins.kind == "tb":
            for t in tb_targets(mo, ins):
                fn.branch_targets.add(t)
                if fn.start <= t < fn.end:
                    work.append(t)
        if ins.kind in ("bl", "blx_stub", "icall"):
            fn.return_sites.add(nxt)
        terminator = ins.kind in ("b", "ijump", "tb", "stop") and not conditional
        if not terminator:
            work.append(nxt)


def uses_vfp(ins):
    """VFP/NEON (coprocessor) encodings: 0xEC00-0xEFFF / 0xFC00-0xFFFF, and
    the NEON loads and stores (0xF9xx with bit 4 clear)."""
    if ins.size != 4:
        return False
    return (ins.hw1 & 0xEC00) == 0xEC00 or (ins.hw1 & 0xFF10) == 0xF900


def emit_function(mo, fn, entries_of, out, prefix, debug_calls=False, debug_budget=False):
    name = f"nf_{prefix}_{fn.start:06x}"
    copy_here = fn.start in copy_funcs
    icall_copy = prefix in ICALL_COPY_BUILDS and fn.start not in nocopy_funcs
    labels = sorted(fn.insns)
    labelset = set(labels)
    resume = sorted((fn.roots | fn.return_sites | fn.branch_targets) & labelset)
    w = out.append
    # Without VFP/NEON instructions the function only ever touches the
    # integer registers: copy just those (recomp_rt.h RT_IN / RT_OUT).
    vfp = any(uses_vfp(ins) for ins in fn.insns.values())
    load, store = ("K = c->k;", "c->k = K;") if vfp else ("RT_IN(K, c);", "RT_OUT(c, K);")
    w(f"int {name}(Cpu *c, uint32_t start) {{")
    w("    Core K = c->k;" if vfp else "    Core K; RT_IN(K, c);")
    w("    const uint32_t entry_lr = K.r[14] & ~1u, entry_sp = K.r[13];")
    w("    int rc;")
    w("    switch (start) {")
    for a in resume:
        w(f"    case {a:#x}: goto L_{a:x};")
    w("    default: c->k.r[15] = start; return 1;")
    w("    }")

    def exit_to(t, indent):
        if t in labelset:
            return f"{indent}goto L_{t:x};"
        if t in entries_of and t not in labelset and not debug_calls:
            return f"{indent}{{ {store} c->k.r[15] = {t:#x}; c->k.thumb = 1; return {entries_of[t]}(c, {t:#x}); }}"
        return f"{indent}{{ {store} c->k.r[15] = {t:#x}; c->k.thumb = 1; return 1; }}"

    for i, a in enumerate(labels):
        ins = fn.insns[a]
        nxt = a + ins.size
        w(f"L_{a:x}:")
        if debug_budget and ins.cond is None:
            w(f"    if (c->budget_on && --c->budget < 0) {{ {store} c->k.r[15] = {a:#x}; c->budget_exit_pc = {a:#x}; return 1; }}")
        loop = None if debug_calls or debug_budget else host_loop_at(fn, a, labelset)
        if loop:
            for line in loop[0]:
                w("    " + line)
            if loop[1] is not None:
                w(f"    goto L_{loop[1]:x};")
                continue
        if ins.kind == "it":
            pass
        else:
            ind = "    "
            body = []
            if ins.kind == "bl" and ins.target in interrupt_at and ins.cond in (None, 14) and not debug_calls:
                w(f"    if (c->interrupt) {{ K.r[14] = {nxt | 1:#x};")
                for line in interrupt_at[ins.target]:
                    w("        " + line)
                w(f"        goto L_{nxt:x}; }}")
            if ins.kind == "bl" and ins.target in host_at:
                body.append(f"K.r[14] = {nxt | 1:#x};")
                body.append(host_at[ins.target])
            elif ins.kind == "bl" and ins.target in leaf_of and not debug_calls:
                body.append(f"K.r[14] = {nxt | 1:#x};")
                if copy_here and not leaf_of[ins.target].startswith("lfi_"):
                    body.append(f"{{ Core T; memcpy(&T, &K, RT_INT_PART); int lr = {leaf_of[ins.target]}(&T); memcpy(&K, &T, RT_INT_PART);")
                    body.append(f"  if (lr) {{ {store} c->k.npc = K.r[15]; rc = rt_call(c, {nxt | 1:#x}); {load} if (rc) return 1; }} }}")
                else:
                    body.append(f"if ({leaf_of[ins.target]}(&K)) {{ {store} c->k.npc = K.r[15]; rc = rt_call(c, {nxt | 1:#x}); {load} if (rc) return 1; }}")
                body.append(f"if (K.r[15] != {nxt:#x}) {{ K.npc = K.r[15]; goto dyn; }}")
            elif ins.kind == "bl":
                t = ins.target
                body.append(f"K.r[14] = {nxt | 1:#x};")
                body.append(store)
                if t in entries_of and not debug_calls:
                    body.append(f"rc = {entries_of[t]}(c, {t:#x});")
                else:
                    body.append(f"c->k.npc = {t:#x}; c->k.thumb = 1; rc = rt_call(c, {nxt | 1:#x});")
                body.append(load + " if (rc) return 1;")
                body.append(f"if (K.r[15] != {nxt:#x}) {{ K.npc = K.r[15]; goto dyn; }}")
            elif ins.kind == "blx_stub":
                body.append(f"K.r[14] = {nxt | 1:#x};")
                if ins.stub in DIV_HELPERS:
                    body.append(DIV_HELPERS[ins.stub])
                else:
                    body.append(f"{store} rc = rt_import(c, {ins.target:#x}, {nxt | 1:#x}); {load} if (rc) return 1;")
                    body.append(f"if (K.r[15] != {nxt:#x}) {{ K.npc = K.r[15]; goto dyn; }}")
            elif ins.kind == "icall":
                body.append(f"K.r[15] = {a + 4:#x}; K.npc = {nxt:#x};")
                body.append(f"exec{16 if ins.size == 2 else 32}(&K, " +
                            (f"{ins.hw1:#06x}, {a:#x}, {ins.in_it});" if ins.size == 2 else f"{ins.hw1:#06x}, {ins.hw2:#06x}, {a:#x});"))
                if leaf_of and not debug_calls:
                    # a leaf target runs on these registers; anything else through the dispatcher
                    body.append(f"{{ LeafFn lf = K.thumb ? {prefix}_leaf(K.npc) : 0;")
                    if icall_copy or copy_here:
                        # on a copy of the integer registers: handing over K's
                        # own address keeps K in memory for the whole function
                        body.append(f"  if (lf) {{ K.r[15] = K.npc; Core T; memcpy(&T, &K, RT_INT_PART); int lr = lf(&T); memcpy(&K, &T, RT_INT_PART);")
                        body.append(f"    if (lr) {{ {store} c->k.npc = K.r[15]; rc = rt_call(c, {nxt | 1:#x}); {load} if (rc) return 1; }} }}")
                    else:
                        body.append(f"  if (lf) {{ K.r[15] = K.npc; if (lf(&K)) {{ {store} c->k.npc = K.r[15]; rc = rt_call(c, {nxt | 1:#x}); {load} if (rc) return 1; }} }}")
                    body.append(f"  else {{ {store} rc = rt_icall(c, {nxt | 1:#x}); {load} if (rc) return 1; }} }}")
                else:
                    body.append(f"{store} rc = rt_icall(c, {nxt | 1:#x}); {load} if (rc) return 1;")
                body.append(f"if (K.r[15] != {nxt:#x}) {{ K.npc = K.r[15]; goto dyn; }}")
            else:
                body.append(f"K.r[15] = {a + 4:#x}; K.npc = {nxt:#x};")
                if ins.size == 2:
                    body.append(f"exec16(&K, {ins.hw1:#06x}, {a:#x}, {ins.in_it});")
                else:
                    body.append(f"exec32(&K, {ins.hw1:#06x}, {ins.hw2:#06x}, {a:#x});")
            # PC state must be set even when an IT condition skips the instruction
            w(f"    K.r[15] = {a + 4:#x}; K.npc = {nxt:#x};")
            body = [b for b in body if not b.startswith("K.r[15] = ")]
            if ins.cond is not None and ins.cond != 14:
                w(f"    if (cond_pass(&K, {ins.cond})) {{")
                for b in body:
                    w("        " + b)
                w("    }")
            else:
                for b in body:
                    w(ind + b)
            # control flow after the instruction
            if ins.kind in ("b", "bcond"):
                guard = "" if ins.kind == "b" and ins.cond in (None, 14) else f"if (K.npc != {nxt:#x}) "
                if guard:
                    w(f"    {guard}" + exit_to(ins.target, "").strip())
                else:
                    w(exit_to(ins.target, "    "))
            elif ins.kind in ("ijump", "tb") and ins.cond in (None, 14):
                # Unconditional indirect jump: always dispatch on the target, even
                # when it happens to be the next address (e.g. jump table entry 0).
                w("    goto dyn;")
            elif ins.kind in ("ijump", "tb", "op"):
                w(f"    if (K.npc != {nxt:#x}) goto dyn;")
            elif ins.kind == "stop":
                w(f"    {store} c->k.r[15] = {a:#x}; return 1;")
        # fallthrough
        terminator = ins.kind in ("b", "ijump", "tb", "stop") and (ins.cond is None or ins.cond == 14)
        if not terminator:
            following = labels[i + 1] if i + 1 < len(labels) else None
            if following != nxt:
                w(exit_to(nxt, "    "))
    w("dyn: {")
    w("    uint32_t t = K.npc;")
    w("    if (K.thumb && t == entry_lr && K.r[13] == entry_sp) { " + store + " c->k.r[15] = t; return 0; }")
    w("    if (K.thumb) switch (t) {")
    for a in resume:
        w(f"    case {a:#x}: goto L_{a:x};")
    w("    default: break;")
    w("    }")
    w(f"    {store} c->k.r[15] = t; return 1;")
    w("}")
    w("}")
    return name, resume


# Hot loops run as host code: where a function's code is exactly one of these
# instruction sequences (so in any build that has it), the loop runs on host
# variables and goes on at the instruction after it, with the registers and
# flags the guest code leaves there. Inside a function whose registers stay
# in memory (an indirect call or a non-leaf call anywhere in it), each guest
# instruction otherwise costs several stores.
#
# iOS 7's activeprompt_Consult, for each text position: the first of r0
# 24-byte records at r2 (from index r4, 16 bits) whose type (+0) is 0x22 and
# whose position (+0xc) is r1. Most of Siri enhanced voices' prompt lookup,
# which grows with the text's length times the records: Nicky's first sound
# on a 140-character path 646 ms, 29% of it in activeprompt_Consult.
HOST_LOOPS = [
    {   # the record search above; replaces the loop, goes on after it
        "chunks": [(0, (0xb2a3, 0x005d, (0xfa15, 0xf584), (0xf852, 0x5035), 0x2d22, 0xd106, (0xeb03, 0x0343),
                        (0xeb02, 0x03c3), 0x68db, 0x428b, 0xd003, 0x3401, 0xb2a3, 0x4283, 0xd3ec))],
        "after": True,
        "code": lambda a: [
            "{   // recomp.py HOST_LOOPS: the first type-0x22 record at position r1",
            "    const uint32_t n = K.r[0], want = K.r[1], base = K.r[2];",
            "    uint32_t i = K.r[4], t = K.r[5], h, x, y;",
            "    for (;;) {",
            "        h = i & 0xffffu;",
            "        t = g32(&K, base + h * 24u);",
            "        if (t == 0x22u) { h = g32(&K, base + h * 24u + 12u); if (h == want) { x = h; y = want; break; } }",
            "        i++;",
            "        h = i & 0xffffu;",
            "        if (h >= n) { x = h; y = n; break; }",
            "    }",
            "    K.r[3] = h; K.r[4] = i; K.r[5] = t;",
            "    { uint32_t d = x - y; K.n = d >> 31; K.z = d == 0; K.c = x >= y; K.v = ((x ^ y) & (x ^ d)) >> 31; }",
            "}",
        ],
    },
    {   # 7.1.2's activeprompt_Consult, trying one phrase at each word of the
        # text: 58,000 phrases a sentence, ~19 words each, nearly all failing
        # at the first character (a post: 225 M guest instructions, a third
        # of its first sound). Each such try runs here: the phrase's first
        # character against the word, then on past the word and the
        # separators after it (a strchr set at a + 0x14cae0), as the guest
        # code does, ending at a again with the registers, flags and stack
        # slots it leaves. Any other case (a match, a record of type 0x22 at
        # the position, the text's end, a bound) leaves at that try's start,
        # where the guest code below runs it (redoing the same stores).
        "chunks": [
            (0x0, (0x4621, 0x9818, 0x9121, (0xfa10, 0xf081), (0xf8cd, 0x8088), 0xb280, 0x901a, 0xb288, 0x902e, 0x2000,
                   0x2800, (0xf041, 0x8126), 0x981b, 0x9a1a, 0x6800, 0x4440, 0x4282, (0xf201, 0x82bd), (0xf8db, 0x0000),
                   0x2300, 0x9a2e, (0xf04f, 0x0b00), 0x9132, 0x5c80, 0x2800, (0xf001, 0x82d0), 0x9d32, 0x9330, 0xe00a)),
            (0x5e, (0x9836, 0xb2ae, 0x4286, (0xf080, 0x8095), (0xf64f, 0x70ff), (0xea1b, 0x0f00), 0xd018, 0x1c70, 0x9936,
                    0x4288, 0xd214, 0x9934, 0x6809, 0x5d8a, 0x2a20, 0xd10f, (0xf241, 0x0848), 0x180c, (0xf2c0, 0x0814),
                    0x44f8, 0x4640, (0xf765, 0xf8c1), 0x4602, 0x4620, 0x4641, (0xf765, 0xf8de), 0x2800, 0xd0d1, 0x46a8,
                    0x68bd, (0xf64f, 0x70ff), (0xea18, 0x0f00), 0xd011, 0x9834, 0x882c, 0x6800, 0x4430, (0xf910, 0x1c01),
                    (0xf64c, 0x2016), (0xf2c0, 0x0014), 0x4478, (0xf765, 0xf87e), 0xb918, (0xf8cd, 0x80c8), 0x882a,
                    0xe01e, 0x8828, 0x2200, 0x2400, (0xf8cd, 0x80c8), 0xb1c0)),
            (0x114, ((0xf8dd, 0x80d4), 0xb2a3, 0x4293, 0xd218)),
            (0x150, (0x9b30, 0x9824, 0xb299, 0x4281, 0xd21a, 0x9834, 0x2400, (0xf8d8, 0x201c), 0x3301, 0x6800, 0x5c51,
                     0x5d80, 0x4281, 0xbf08, 0x2401, 0x9a32, 0x4281, (0xf102, 0x0201), 0x9232, (0xf43f, 0xaf62), 0x9832,
                     0x4646, 0x992e, (0xf8dd, 0xb0d0), 0xb280, 0x1a40, 0x901e, 0xe1c5)),
            (0x51c, (0x9831, 0x6900, (0xf761, 0xfef0), 0x2801, 0xd174)),
            (0x612, (0x2c01, (0xf040, 0x84d3))),
            (0xfbe, ((0xf8dd, 0x8088), 0x4621, 0x4608, 0x9921, (0xf64b, 0x350a), 0x2801, (0xf2c0, 0x0514), 0x447d,
                     (0xf000, 0x8149), 0x2800, (0xf47f, 0xa81d), 0xe126)),
            (0x122e, ((0xf8db, 0x0000), 0x460c, 0xb2a1, 0x5c40, 0xb140, 0xb241, 0x4628, (0xf763, 0xffc3), 0x1c61, 0x2800,
                      0xd0f2, 0xe000, 0x3401, (0xf8db, 0x0000), 0xb2a1, 0x5c40, 0x2800, 0xb241, (0xf43e, 0xaed2), 0x4628,
                      (0xf763, 0xffb3), 0x2800, (0xf43e, 0xaecc), 0xe7ef)),
        ],
        "after": False,
        "code": lambda a: [
            "{   // recomp.py HOST_LOOPS: a phrase against each word, until a character differs",
            f"    const uint32_t sp = K.r[13], cntp = g32(&K, K.r[7] + 8), D = {a + 0x14cae0:#x};",
            "    uint32_t r4 = K.r[4], r8 = K.r[8], r11 = K.r[11], lr = K.r[14];",
            "    for (;;) {",
            "        const uint32_t pos = r4, p16 = pos & 0xffffu, t16 = (g32(&K, sp + 0x60) + p16) & 0xffffu;",
            "        s32(&K, sp + 0x84, pos); s32(&K, sp + 0x88, r8); s32(&K, sp + 0x68, t16); s32(&K, sp + 0xb8, p16);",
            "        if (t16 > g32(&K, g32(&K, sp + 0x6c)) + r8) break;",
            "        const uint32_t t0 = g32(&K, r11);",
            "        s32(&K, sp + 0xc8, pos);",
            "        if (!g8(&K, t0 + p16)) break;",
            "        uint32_t k = 0, cur = pos, prompt;",
            "        int other = 0;",
            "        for (;; k++, cur++) {  // compare k, at text position cur",
            "            s32(&K, sp + 0xc0, k);",
            "            const uint32_t c16 = cur & 0xffffu;",
            "            if (c16 >= g32(&K, sp + 0xd8)) { other = 1; break; }",
            "            const uint32_t text = g32(&K, g32(&K, sp + 0xd0));",
            "            int scan = 1;",
            f"            if (c16) {{ lr = {a + 0xcc | 1:#x}; scan = rt_in_str(&K, D, (uint8_t)g8(&K, text + c16 - 1)); }}",
            "            if (scan) {",
            "                const uint32_t n = g16(&K, cntp);",
            "                if (n) {",
            "                    const uint32_t base = g32(&K, g32(&K, sp + 0xcc)), want = g32(&K, sp + 0xb4) + c16;",
            "                    uint32_t i = 0;",
            "                    while (i < n && !(g32(&K, base + i * 24u) == 0x22u && g32(&K, base + i * 24u + 12u) == want)) i++;",
            "                    if (i < n) { other = 1; break; }",
            "                }",
            "            }",
            "            prompt = g32(&K, sp + 0xd4);",
            "            if ((k & 0xffffu) >= g32(&K, sp + 0x90)) { other = 1; break; }",
            "            const uint32_t pc = g8(&K, g32(&K, prompt + 0x1c) + (k & 0xffffu)), tc = g8(&K, text + c16);",
            "            s32(&K, sp + 0xc8, cur + 1);",
            "            if (pc != tc) break;",
            "        }",
            "        if (other) break;",
            "        s32(&K, sp + 0x78, ((cur + 1) & 0xffffu) - p16);",
            "        r11 = g32(&K, sp + 0xd0);",
            f"        g32(&K, g32(&K, sp + 0xc4) + 0x10); lr = {a + 0x524 | 1:#x};",
            "        const uint32_t tx = g32(&K, r11);",
            "        uint32_t b;",
            f"        for (;;) {{ b = g8(&K, tx + (r4 & 0xffffu)); if (!b) break; lr = {a + 0x1242 | 1:#x}; if (rt_in_str(&K, D, (uint8_t)b)) break; r4++; }}",
            f"        for (;;) {{ b = g8(&K, tx + (r4 & 0xffffu)); if (!b) break; lr = {a + 0x1262 | 1:#x}; if (!rt_in_str(&K, D, (uint8_t)b)) break; r4++; }}",
            "        K.r[0] = 0; K.r[1] = (uint32_t)(int32_t)(int8_t)b; K.r[2] = cur + 1; K.r[3] = k + 1; K.r[4] = r4;",
            "        K.r[5] = D; K.r[6] = prompt; K.r[8] = r8; K.r[11] = r11; K.r[14] = lr;",
            "        K.n = 0; K.z = 1; K.c = 1; K.v = 0;",
            "    }",
            "}",
        ],
    },
    {   # Vocalizer 5.2's sentence parser (mosyntsentana_DoParse): a walk
        # down one of its hash table's 37 chains for the entry with a key
        # (+0x10, 16 bits signed) equal to [sp + 0x94]; long chains on a long
        # sentence (2.4 M steps for a 140-character line). Two ways out: the
        # end of the chain (a + 0x9e) or the entry (a + 0x10). a - 4 steps on.
        "chunks": [(-4, ((0xf8db, 0xb000),)), (0, ((0xf1bb, 0x0f00), 0xd04b, 0x9825, (0xf9bb, 0x1010), 0x4281, 0xd1f5))],
        "after": False,
        "exits": (0x9e, 0x10),
        "code": lambda a: [
            "{   // recomp.py HOST_LOOPS: the parser's hash chain, to the entry with a key",
            "    uint32_t n = K.r[11];",
            "    for (;;) {",
            f"        if (!n) {{ K.r[11] = 0; K.n = 0; K.z = 1; K.c = 1; K.v = 0; goto L_{a + 0x9e:x}; }}",
            "        const uint32_t key = g32(&K, K.r[13] + 0x94), f = (uint32_t)(int32_t)(int16_t)g16(&K, n + 0x10);",
            "        K.r[0] = key; K.r[1] = f;",
            f"        if (f == key) {{ K.r[11] = n; K.n = 0; K.z = 1; K.c = 1; K.v = 0; goto L_{a + 0x10:x}; }}",
            "        n = g32(&K, n);",
            "    }",
            "}",
        ],
    },
]


def host_loop_at(fn, a, labelset):
    """(host code, the address it goes on at, or None to go on with the guest
    code at a) if one of HOST_LOOPS is at a. Its IT instructions are in the
    pattern, so the ones inside IT blocks are too."""
    for loop in HOST_LOOPS:
        end = None
        for rel, pattern in loop["chunks"]:
            at = a + rel
            for p in pattern:
                ins = fn.insns.get(at)
                hw1, hw2 = p if isinstance(p, tuple) else (p, None)
                if ins is None or ins.hw1 != hw1 or ins.size != (4 if hw2 is not None else 2) or \
                        (hw2 is not None and ins.hw2 != hw2):
                    break
                at += ins.size
            else:
                end = end if end is not None else at
                continue
            break
        else:
            if not all(a + e in labelset for e in loop.get("exits", ())):
                continue
            if not loop["after"]:
                return loop["code"](a), None
            if end in labelset:
                return loop["code"](a), end
    return None


# Functions whose leaf calls, direct and indirect, pass a copy of the
# registers, in HOST_FUNC_BUILDS: with no call taking K's address left (the
# recomp_rt.h helpers are always inlined) K stays in host registers. iOS 7's
# prompt lookup, its own registers otherwise stored at every instruction:
# Nicky's first sound on a 140-character path 578 -> 517 ms.
COPY_FUNCS = ("_activeprompt_Consult",)
copy_funcs = set()  # their addresses, in the build being translated
nocopy_funcs = set()  # unit selection's, which call through pointers ~1 M times a sentence

# Builds whose indirect calls to leaves pass a copy of the registers: one
# such call taking K's address keeps all of a function's registers in memory
# (pcre_exec2 has one), and the copies cost less, except where a hot loop
# makes the calls themselves (unit selection's distance functions, ~1 M a
# sentence: nocopy_funcs). Post-length sentence, first sound: iOS 3 32 -> 26
# ms, Nicky 170 -> 160, the others the same or better.
ICALL_COPY_BUILDS = ("ios3", "ios4", "ios5", "ios6", "ios70", "ios7", "ios10")

# Leaves of at most this many instructions (the fixed-point multiplies and
# the like) are also written as always-inlined functions that direct calls
# use: a caller that passes its registers' address to a real function must
# keep them in memory, and reload them after every guest store.
TINY_LEAF = 160


def leaf_region(fn, root, limit=96):
    """The instructions reachable from `root` if they make a leaf: no calls,
    no branches out of the function, no VFP/NEON, at most `limit` of them;
    else None. Its exits are returns (BX LR, POP {..., PC}), checked at run
    time like any function's."""
    seen, work = set(), [root]
    while work:
        a = work.pop()
        if a in seen:
            continue
        ins = fn.insns.get(a)
        if ins is None or ins.kind in ("bl", "blx_stub", "icall", "stop") or uses_vfp(ins):
            return None
        seen.add(a)
        if len(seen) > limit:
            return None
        nxt = a + ins.size
        conditional = ins.cond is not None and ins.cond != 14
        if ins.kind in ("b", "bcond"):
            if not (fn.start <= ins.target < fn.end):
                return None
            work.append(ins.target)
        if ins.kind == "tb":
            return None  # its targets aren't kept per instruction
        if not (ins.kind in ("b", "ijump") and not conditional):
            work.append(nxt)
    return seen


def emit_leaf(fn, root, region, out, prefix, inline=False):
    """lf_<prefix>_<root>(Core *): the leaf on the caller's registers, with no
    copies in or out. 0 when it returned (K->r[15] = the return address),
    1 when it left any other way (K->r[15] = where; the caller hands over).
    inline: lfi_<prefix>_<root>, always inlined (TINY_LEAF)."""
    name = f"{'lfi' if inline else 'lf'}_{prefix}_{root:06x}"
    labels = sorted(region)
    w = out.append
    w(f"static inline __attribute__((always_inline)) int {name}(Core *Kp) {{" if inline else f"int {name}(Core *Kp) {{")
    w("#define K (*Kp)")
    w("    const uint32_t entry_lr = K.r[14] & ~1u, entry_sp = K.r[13];")
    w(f"    goto L_{root:x};")
    for i, a in enumerate(labels):
        ins = fn.insns[a]
        nxt = a + ins.size
        w(f"L_{a:x}:")
        if ins.kind != "it":
            w(f"    K.r[15] = {a + 4:#x}; K.npc = {nxt:#x};")
            ex = (f"exec16(&K, {ins.hw1:#06x}, {a:#x}, {ins.in_it});" if ins.size == 2 else
                  f"exec32(&K, {ins.hw1:#06x}, {ins.hw2:#06x}, {a:#x});")
            if ins.cond is not None and ins.cond != 14:
                w(f"    if (cond_pass(&K, {ins.cond})) {{ {ex} }}")
            else:
                w("    " + ex)
            if ins.kind in ("b", "bcond"):
                guard = "" if ins.kind == "b" and ins.cond in (None, 14) else f"if (K.npc != {nxt:#x}) "
                w(f"    {guard}goto L_{ins.target:x};")
            elif ins.kind == "ijump" and ins.cond in (None, 14):
                w("    goto dyn;")
            else:
                w(f"    if (K.npc != {nxt:#x}) goto dyn;")
        terminator = ins.kind in ("b", "ijump") and (ins.cond is None or ins.cond == 14)
        if not terminator:
            following = labels[i + 1] if i + 1 < len(labels) else None
            if following != nxt:
                w(f"    goto L_{nxt:x};")
    w("dyn:")
    w("    K.r[15] = K.npc;")
    w("    return !(K.thumb && K.npc == entry_lr && K.r[13] == entry_sp);")
    w("#undef K")
    w("}")
    return name


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("voiced")
    ap.add_argument("profile")
    ap.add_argument("outdir")
    ap.add_argument("--prefix", default="ios6")
    ap.add_argument("--per-file", type=int, default=4000, help="instructions per C file")
    ap.add_argument("--debug-calls", action="store_true",
                    help="route every call/tail call through the dispatcher (for bisecting with VX_NATIVE_RANGE)")
    ap.add_argument("--debug-budget", action="store_true",
                    help="leave native code after VX_BUDGET instructions (bisecting translation bugs)")
    ap.add_argument("--dis", help="llvm-objdump disassembly; its BL targets become function boundaries "
                                  "(needed for stripped binaries)")
    ap.add_argument("--scan-bl", action="store_true",
                    help="find those BL targets without a disassembly (the same set)")
    a = ap.parse_args(argv)

    mo = MachO(a.voiced)
    if a.prefix in HOST_FUNC_BUILDS:
        host_at.update({addr: HOST_FUNCS[nm] for addr, nm in mo.syms if nm in HOST_FUNCS})
        interrupt_at.update({addr: INTERRUPT_FUNCS[nm] for addr, nm in mo.syms if nm in INTERRUPT_FUNCS})
        copy_funcs.update(addr for addr, nm in mo.syms if nm in COPY_FUNCS)
    nocopy_funcs.update(addr for addr, nm in mo.syms if nm.startswith("_uselect_"))
    if a.profile.endswith(".txt"):
        # tools/profiles/*.txt: one hex address per line (block starts seen while profiling)
        seen = sorted({int(ln, 16) for ln in (l.split("#")[0].strip() for l in open(a.profile)) if ln})
    else:
        prof = open(a.profile, "rb").read()  # build/profile output: one byte per halfword
        seen = [i * 2 for i, b in enumerate(prof) if b]
    starts = [x for x in seen if mo.text[0] <= x < mo.text[1]]

    bound_set = {s for s, _ in mo.syms} | {mo.text[0]}
    if a.dis:
        import re
        for line in open(a.dis):
            m = re.search(r"\bbl\s+0x([0-9a-f]+)", line)
            if m:
                t = int(m.group(1), 16)
                if mo.text[0] <= t < mo.text[1]:
                    bound_set.add(t)
    if a.scan_bl:
        bound_set |= scan_bl(mo)
    bounds = sorted(bound_set)
    import bisect

    def extent(addr):
        i = bisect.bisect_right(bounds, addr) - 1
        lo = bounds[i]
        hi = bounds[i + 1] if i + 1 < len(bounds) else mo.text[1]
        return lo, hi

    funcs = {}
    for s in starts:
        lo, hi = extent(s)
        f = funcs.setdefault(lo, Function(lo, hi))
        f.roots.add(s)
    for f in funcs.values():
        if f.start in bound_set:
            f.roots.add(f.start)
        explore(mo, f)
        f.roots &= set(f.insns)

    entries_of = {}
    for f in funcs.values():
        for r in f.roots | f.branch_targets | f.return_sites:
            if r in f.insns:
                entries_of[r] = f"nf_{a.prefix}_{f.start:06x}"

    # Leaves (leaf_region): the targets of direct calls, and entry points no
    # other code of their function reaches (what the dispatcher was given,
    # e.g. the unit-selection distance functions called through pointers).
    leaves = []
    if not (a.debug_calls or a.debug_budget):
        bl_targets = {ins.target for f in funcs.values() for ins in f.insns.values() if ins.kind == "bl"}
        for f in funcs.values():
            reached = set()
            for ins in f.insns.values():
                if ins.kind in ("b", "bcond") and ins.target is not None:
                    reached.add(ins.target)
                if not (ins.kind in ("b", "ijump", "tb", "stop") and (ins.cond is None or ins.cond == 14)):
                    reached.add(ins.addr + ins.size)
            for e in sorted((f.roots - reached) | {f.start} | (bl_targets & set(f.insns))):
                if e in host_at:
                    continue
                region = leaf_region(f, e)
                if region:
                    leaf_of[e] = f"{'lfi' if len(region) <= TINY_LEAF else 'lf'}_{a.prefix}_{e:06x}"
                    leaves.append((f, e, region))

    os.makedirs(a.outdir, exist_ok=True)
    order = sorted(funcs.values(), key=lambda f: f.start)
    header = [
        "// Generated by tools/recomp.py — do not edit.",
        "#define VX_OPS_INLINE static inline __attribute__((always_inline))",
        '#include "thumb.h"',
        '#include "recomp_rt.h"',
        "",
    ]
    files, cur, cur_n, table, decls = [], [], 0, [], []
    for f in order:
        decls.append(f"int nf_{a.prefix}_{f.start:06x}(Cpu *c, uint32_t start);")
    for _f, e, _r in leaves:
        decls.append(f"int lf_{a.prefix}_{e:06x}(Core *Kp);")
    if leaves:
        decls.append(f"static inline LeafFn {a.prefix}_leaf(uint32_t t) {{")
        decls.append("    switch (t) {")
        decls += [f"    case {e:#x}: return lf_{a.prefix}_{e:06x};" for _f, e, _r in leaves]
        decls.append("    default: return 0;")
        decls.append("    }")
        decls.append("}")
    for f, e, region in leaves:
        if len(region) <= TINY_LEAF:
            emit_leaf(f, e, region, decls, a.prefix, inline=True)
    decl_text = "\n".join(decls)
    total = 0
    for f in order:
        body = []
        name, resume = emit_function(mo, f, entries_of, body, a.prefix, a.debug_calls, a.debug_budget)
        for r in resume:
            table.append((r, name))
        cur.extend(body)
        cur.append("")
        cur_n += len(f.insns)
        total += len(f.insns)
        if cur_n >= a.per_file:
            files.append(cur)
            cur, cur_n = [], 0
    for f, e, region in leaves:
        body = []
        if len(region) <= TINY_LEAF:
            body.append(f"int lf_{a.prefix}_{e:06x}(Core *Kp) {{ return lfi_{a.prefix}_{e:06x}(Kp); }}")
        else:
            emit_leaf(f, e, region, body, a.prefix)
        cur.extend(body)
        cur.append("")
        cur_n += len(region)
        if cur_n >= a.per_file:
            files.append(cur)
            cur, cur_n = [], 0
    if cur:
        files.append(cur)
    for i, body in enumerate(files):
        with open(os.path.join(a.outdir, f"{a.prefix}_{i:03d}.c"), "w") as fh:
            fh.write("\n".join(header + [decl_text, ""] + body))
    table.sort()
    with open(os.path.join(a.outdir, f"{a.prefix}_table.c"), "w") as fh:
        fh.write("\n".join(header + [decl_text, "",
                 f"const NativeEntry vx_native_{a.prefix}[] = {{"] +
                 [f"    {{{addr:#x}, {fn}}}," for addr, fn in table] +
                 ["};", f"const size_t vx_native_{a.prefix}_count = {len(table)};", ""]))
    print(f"{len(funcs)} functions, {total} instructions, {len(table)} resume points, {len(leaves)} leaves, {len(files)} files")


if __name__ == "__main__":
    main()
