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


def emit_function(mo, fn, entries_of, out, prefix, debug_calls=False, debug_budget=False):
    name = f"nf_{prefix}_{fn.start:06x}"
    labels = sorted(fn.insns)
    labelset = set(labels)
    resume = sorted((fn.roots | fn.return_sites | fn.branch_targets) & labelset)
    w = out.append
    w(f"int {name}(Cpu *c, uint32_t start) {{")
    w("    Core K = c->k;")
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
            return f"{indent}{{ c->k = K; c->k.r[15] = {t:#x}; c->k.thumb = 1; return {entries_of[t]}(c, {t:#x}); }}"
        return f"{indent}{{ c->k = K; c->k.r[15] = {t:#x}; c->k.thumb = 1; return 1; }}"

    for i, a in enumerate(labels):
        ins = fn.insns[a]
        nxt = a + ins.size
        w(f"L_{a:x}:")
        if debug_budget and ins.cond is None:
            w(f"    if (c->budget_on && --c->budget < 0) {{ c->k = K; c->k.r[15] = {a:#x}; c->budget_exit_pc = {a:#x}; return 1; }}")
        if ins.kind == "it":
            pass
        else:
            ind = "    "
            body = []
            if ins.kind == "bl":
                t = ins.target
                body.append(f"K.r[14] = {nxt | 1:#x};")
                body.append("c->k = K;")
                if t in entries_of and not debug_calls:
                    body.append(f"rc = {entries_of[t]}(c, {t:#x});")
                else:
                    body.append(f"c->k.npc = {t:#x}; c->k.thumb = 1; rc = rt_call(c, {nxt | 1:#x});")
                body.append("K = c->k; if (rc) return 1;")
                body.append(f"if (K.r[15] != {nxt:#x}) {{ K.npc = K.r[15]; goto dyn; }}")
            elif ins.kind == "blx_stub":
                body.append(f"K.r[14] = {nxt | 1:#x};")
                if ins.stub in DIV_HELPERS:
                    body.append(DIV_HELPERS[ins.stub])
                else:
                    body.append(f"c->k = K; rc = rt_import(c, {ins.target:#x}, {nxt | 1:#x}); K = c->k; if (rc) return 1;")
                    body.append(f"if (K.r[15] != {nxt:#x}) {{ K.npc = K.r[15]; goto dyn; }}")
            elif ins.kind == "icall":
                body.append(f"K.r[15] = {a + 4:#x}; K.npc = {nxt:#x};")
                body.append(f"exec{16 if ins.size == 2 else 32}(&K, " +
                            (f"{ins.hw1:#06x}, {a:#x}, {ins.in_it});" if ins.size == 2 else f"{ins.hw1:#06x}, {ins.hw2:#06x}, {a:#x});"))
                body.append(f"c->k = K; rc = rt_call(c, {nxt | 1:#x}); K = c->k; if (rc) return 1;")
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
                w(f"    c->k = K; c->k.r[15] = {a:#x}; return 1;")
        # fallthrough
        terminator = ins.kind in ("b", "ijump", "tb", "stop") and (ins.cond is None or ins.cond == 14)
        if not terminator:
            following = labels[i + 1] if i + 1 < len(labels) else None
            if following != nxt:
                w(exit_to(nxt, "    "))
    w("dyn: {")
    w("    uint32_t t = K.npc;")
    w("    if (K.thumb && t == entry_lr && K.r[13] == entry_sp) { c->k = K; c->k.r[15] = t; return 0; }")
    w("    if (K.thumb) switch (t) {")
    for a in resume:
        w(f"    case {a:#x}: goto L_{a:x};")
    w("    default: break;")
    w("    }")
    w("    c->k = K; c->k.r[15] = t; return 1;")
    w("}")
    w("}")
    return name, resume


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
    print(f"{len(funcs)} functions, {total} instructions, {len(table)} resume points, {len(files)} files")


if __name__ == "__main__":
    main()
