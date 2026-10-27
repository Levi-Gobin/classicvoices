#!/usr/bin/env python3
"""Static recompiler: hot i386 functions of a MacinTalk generation -> C.

Input is the engine as MacinTalk's loader left it in guest memory (MT_DUMP,
see tools/build_mt_native.sh) plus a profile (tools/profiles/mtNNN.txt: lines
"<image> <address> <instructions>": where control landed while speaking, and
how many instructions ran from there before it moved again). Each function that
ran becomes `int nx_<gen>_<addr>(X86 *c, uint32_t start)`. Every guest
instruction is emitted as a call to the interpreter's own semantics (x86_exec
in core/x86/x86_ops.h) with its bytes as constants, which clang inlines and
folds, so the translation cannot drift from the interpreter. The guest core
lives in a local that clang keeps in registers; it is written back to the X86
only around calls and exits. Calls keep pushing guest return addresses, so the
guest stack is exactly the interpreter's.

The output embeds code from the dumped images, so it is built from your own
copy of each engine and never committed. At run time a set is used only if
the code ranges it was made from hash the same in the loaded engine.

Every function that ran while profiling is translated, unless --coverage asks
for only the hottest ones that together ran that share of the instructions
(the rest stay interpreted).

Usage: recomp_x86.py <dumpdir> <profile.txt> <outdir> --prefix mt105 [--coverage 0.995] [--per-file N]
"""
import argparse
import bisect
import os
import struct

# ---------------------------------------------------------------- the dump


class Dump:
    def __init__(self, d):
        self.images, self.sects, self.syms = [], [], []
        self.trap = (0, 0)
        for ln in open(os.path.join(d, "images.txt")):
            f = ln.split()
            if f[0] == "trap":
                self.trap = (int(f[1], 16), int(f[2], 16))
            elif f[0] == "image":
                lo, hi, slide = int(f[2], 16), int(f[3], 16), int(f[4], 16)
                data = open(os.path.join(d, f[1] + ".mem"), "rb").read()
                self.images.append({"name": f[1], "lo": lo, "hi": hi, "slide": slide, "data": data})
            elif f[0] == "sect":
                self.sects.append({"image": f[1], "seg": f[2], "sect": f[3], "addr": int(f[4], 16),
                                   "size": int(f[5], 16), "flags": int(f[6], 16)})
            elif f[0] == "sym":
                self.syms.append((f[1], int(f[2], 16), " ".join(f[3:])))
        self.texts = [s for s in self.sects if s["sect"] == "__text" and s["seg"] == "__TEXT"]

    def image(self, name):
        return next(i for i in self.images if i["name"] == name)

    def read(self, addr, n):
        for im in self.images:
            if im["lo"] <= addr and addr + n <= im["hi"]:
                o = addr - im["lo"]
                return im["data"][o:o + n]
        raise ValueError(hex(addr))

    def text_of(self, addr):
        for s in self.texts:
            if s["addr"] <= addr < s["addr"] + s["size"]:
                return s
        return None


# ---------------------------------------------------------------- decoding

def _set(*ranges):
    s = set()
    for r in ranges:
        if isinstance(r, int):
            s.add(r)
        else:
            s.update(range(r[0], r[1] + 1))
    return s


PREFIXES = {0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x66}
# one-byte opcodes with a ModRM byte
MODRM1 = _set((0x00, 0x03), (0x08, 0x0B), (0x10, 0x13), (0x18, 0x1B), (0x20, 0x23), (0x28, 0x2B),
              (0x30, 0x33), (0x38, 0x3B), 0x62, 0x63, 0x69, 0x6B, (0x80, 0x8F), (0xC0, 0xC1),
              (0xC4, 0xC7), (0xD0, 0xD3), (0xD8, 0xDF), (0xF6, 0xF7), (0xFE, 0xFF))
IMM8_1 = _set(0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x6A, 0x6B, (0x70, 0x7F), 0x80, 0x82, 0x83,
              0xA8, (0xB0, 0xB7), 0xC0, 0xC1, 0xC6, 0xCD, 0xD4, 0xD5, (0xE0, 0xE7), 0xEB)
IMMZ_1 = _set(0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0x68, 0x69, 0x81, 0xA9, (0xB8, 0xBF), 0xC7,
              0xE8, 0xE9)
NO_MODRM2 = _set((0x04, 0x0C), 0x0E, (0x30, 0x37), 0x77, (0x80, 0x8F), 0xA0, 0xA1, 0xA2, 0xA8, 0xA9, 0xAA,
                 (0xC8, 0xCF))
IMM8_2 = _set(0x0F, (0x70, 0x73), 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6)


def is_sse(op2):  # x86_is_sse in core/x86/x86_ops.h
    return 0x10 <= op2 <= 0x17 or 0x28 <= op2 <= 0x2F or 0x50 <= op2 <= 0x7F or op2 in (0xC2, 0xC6) or op2 >= 0xD0


def group(raw):
    """Which x86_exec_* entry executes this instruction."""
    i = 0
    while raw[i] in PREFIXES:
        i += 1
    if raw[i] == 0x0F:
        return "sse" if is_sse(raw[i + 1]) else f"0f{raw[i + 1] >> 4:x}"
    return "x87" if 0xD8 <= raw[i] <= 0xDF else f"m{raw[i] >> 4:x}"


class Insn:
    __slots__ = ("addr", "raw", "size", "kind", "target")

    def __init__(self, addr, raw, kind="op", target=None):
        self.addr, self.raw, self.size, self.kind, self.target = addr, raw, len(raw), kind, target



# Each guest instruction becomes a small static function, which clang
# simplifies on its own (its bytes are constants) before inlining it into the
# function's body; inlining every instruction's whole interpreter first and
# simplifying after costs minutes per function.
def insn_fn(defs, name, raw):
    defs.append(f"static void {name}(X86Core *k) {{ x86_exec_{group(raw)}(k, {cbytes(raw)}); }}")
    return f"{name}(&K);"


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def decode(dump, addr):
    try:
        b = dump.read(addr, 16)
    except ValueError:
        b = dump.read(addr, 1) + bytes(15)  # the last few bytes of an image
    i, opsz = 0, 32
    while b[i] in PREFIXES:
        if b[i] == 0x66:
            opsz = 16
        i += 1
        if i >= 15:
            raise ValueError("prefixes")
    op = b[i]; i += 1
    if op == 0x67:
        raise ValueError("address-size prefix")
    immz = 2 if opsz == 16 else 4

    def modrm_len(j):
        m = b[j]
        mod, rm = m >> 6, m & 7
        n = 1
        if mod == 3:
            return n, m
        if rm == 4:
            sib = b[j + 1]
            n += 1
            if sib & 7 == 5 and mod == 0:
                n += 4
        elif rm == 5 and mod == 0:
            n += 4
        n += 1 if mod == 1 else 4 if mod == 2 else 0
        return n, m

    kind, target = "op", None
    if op == 0x0F:
        op2 = b[i]; i += 1
        if op2 in (0x38, 0x3A):
            i += 1
            n, _ = modrm_len(i); i += n
            if op2 == 0x3A:
                i += 1
        else:
            if op2 not in NO_MODRM2:
                n, _ = modrm_len(i); i += n
            if op2 in IMM8_2:
                i += 1
            if 0x80 <= op2 <= 0x8F:
                rel = s32(struct.unpack_from("<I", b, i)[0]); i += 4
                kind, target = "jcc", (addr + i + rel) & 0xFFFFFFFF
            elif op2 == 0x0B:
                kind = "stop"
    else:
        m = None
        if op in MODRM1:
            n, m = modrm_len(i); i += n
        if op in IMM8_1:
            i += 1
        if op in IMMZ_1:
            i += immz
        if op == 0xF6 and (m >> 3) & 7 < 2:
            i += 1
        if op == 0xF7 and (m >> 3) & 7 < 2:
            i += immz
        if op in (0xA0, 0xA1, 0xA2, 0xA3):
            i += 4
        if op in (0xC2, 0xCA):
            i += 2
        if op == 0xC8:
            i += 3
        if op in (0x9A, 0xEA):
            i += 2 + immz
            kind = "stop"
        if 0x70 <= op <= 0x7F or 0xE0 <= op <= 0xE3:
            kind, target = "jcc", (addr + i + (b[i - 1] ^ 0x80) - 0x80) & 0xFFFFFFFF
        elif op == 0xEB:
            kind, target = "jmp", (addr + i + (b[i - 1] ^ 0x80) - 0x80) & 0xFFFFFFFF
        elif op in (0xE8, 0xE9):
            rel = s32(struct.unpack_from("<I", b, i - 4)[0])
            kind, target = ("call" if op == 0xE8 else "jmp"), (addr + i + rel) & 0xFFFFFFFF
        elif op in (0xC2, 0xC3):
            kind = "ret"
        elif op == 0xFF and (m >> 3) & 7 == 2:
            kind = "icall"
        elif op == 0xFF and (m >> 3) & 7 == 4:
            kind = "ijmp"
        elif op in (0xCC, 0xCD, 0xCE, 0xF4, 0xCA, 0xCB, 0xCF) or (op == 0xFF and (m >> 3) & 7 in (3, 5, 7)):
            kind = "stop"
    if i > 15:
        raise ValueError("too long")
    return Insn(addr, b[:i], kind, target)


# `call` to code that only copies its return address into a register:
# mov reg, [esp] ; ret (PIC base). Executed inline.
def pc_thunk(dump, t):
    try:
        b = dump.read(t, 4)
    except ValueError:
        return None
    if b[0] == 0x8B and b[1] & 0xC7 == 0x04 and b[2] == 0x24 and b[3] == 0xC3:
        return [b[0:3], b[3:4]]
    return None


# ---------------------------------------------------------------- functions

class Function:
    def __init__(self, start, end, name):
        self.start, self.end, self.name = start, end, name
        self.insns = {}
        self.roots = set()
        self.branch_targets = set()
        self.return_sites = set()


def explore(dump, fn, landed):
    """Decode what can run from the roots. A branch target the profile never
    landed on was never taken: it is left out, and reaching it hands over to
    the interpreter."""
    work = sorted(fn.roots)
    while work:
        a = work.pop()
        if a in fn.insns or not (fn.start <= a < fn.end):
            continue
        try:
            ins = decode(dump, a)
        except (ValueError, IndexError):
            fn.insns[a] = Insn(a, b"", "stop")
            continue
        fn.insns[a] = ins
        nxt = a + ins.size
        if ins.kind in ("jcc", "jmp") and ins.target in landed:
            fn.branch_targets.add(ins.target)
            if fn.start <= ins.target < fn.end:
                work.append(ins.target)
        if ins.kind in ("call", "icall") and not (ins.kind == "call" and ins.target == nxt):
            fn.return_sites.add(nxt)
        if ins.kind not in ("jmp", "ret", "ijmp", "stop"):
            work.append(nxt)


def cbytes(raw):
    p = raw + bytes(16 - len(raw))
    lo, hi = struct.unpack("<QQ", p)
    return f"{lo:#x}ull, {hi:#x}ull"


def emit_function(dump, fn, entries_of, prefix, debug_calls=False, part_size=600):
    """One C function per guest function. A guest function longer than
    part_size instructions becomes several C functions (parts) of consecutive
    instructions that hand over to each other by tail calls, each of which can
    go in a file of its own: clang needs about 1.5 MB per instruction in a
    file, more for one long function (a 10000-instruction one took 10 GB,
    more than a GitHub runner has). Returns the name, its entry points and
    the units to write: (lines, instructions)."""
    name = f"nx_{prefix}_{fn.start:08x}"
    labels = sorted(fn.insns)
    labelset = set(labels)
    resume = sorted((fn.roots | fn.return_sites | fn.branch_targets) & labelset)
    parts = [labels[i:i + part_size] for i in range(0, len(labels), part_size)]
    split = len(parts) > 1
    part_of = {a: p for p, ls in enumerate(parts) for a in ls}
    pname = lambda p: f"{name}_{p}" if split else name
    args = "c, {}, entry_sp, entry_ret" if split else None
    out_fns = []
    parts_entered = [set() for _ in parts]  # entered from another part

    def leave(t, indent):
        """Control goes to t, outside this function's code."""
        if t in entries_of and not debug_calls:
            return f"{indent}c->k = K; return {entries_of[t]}(c, {t:#x});"
        return f"{indent}c->k = K; return 1;"

    for p, plabels in enumerate(parts):
        here = set(plabels)
        entered = set(a for a in resume if a in here)  # where the part can be started
        body = []
        defs = []
        w = body.append

        def goto(t, src, indent):
            s = ""
            if t <= src:
                # A loop. Guest memory is shared with the engine's other threads:
                # the barrier makes each pass read it again (a wait for another
                # thread would otherwise spin forever on a value loaded once),
                # and an engine shutdown gets through.
                s = (f"{indent}__asm__ volatile(\"\" ::: \"memory\");\n"
                     f"{indent}if (__builtin_expect(*abortp, 0)) {{ c->k = K; return 1; }}\n")
            if t in here:
                return s + f"{indent}goto L_{t:x};"
            if t in labelset:
                parts_entered[part_of[t]].add(t)
                return s + (f"{indent}c->k = K; __attribute__((musttail)) "
                            f"return {pname(part_of[t])}({args.format(f'{t:#x}')});")
            return s + leave(t, indent)

        for i, a in enumerate(plabels):
            ins = fn.insns[a]
            nxt = a + ins.size
            w(f"L_{a:x}:")
            if ins.kind == "stop":
                w(f"    K.eip = {a:#x}; c->k = K; return 1;")
                continue
            w(f"    K.eip = {a:#x}; {insn_fn(defs, f'i_{a:x}', ins.raw)}")
            if ins.kind == "jcc":
                w(f"    if (K.eip != {nxt:#x}) {{")
                w(goto(ins.target, a, "        "))
                w("    }")
            elif ins.kind == "jmp":
                w(goto(ins.target, a, "    "))
            elif ins.kind == "call" and ins.target == nxt:
                pass  # call next / pop: the PIC base
            elif ins.kind == "call" and pc_thunk(dump, ins.target):
                t0, t1 = pc_thunk(dump, ins.target)
                w(f"    {insn_fn(defs, f't_{a:x}_0', t0)} K.eip = {ins.target + 3:#x}; {insn_fn(defs, f't_{a:x}_1', t1)}")
            elif ins.kind in ("call", "icall"):
                t = ins.target
                w("    c->k = K;")
                if ins.kind == "call" and t in entries_of and not debug_calls:
                    w(f"    rc = {entries_of[t]}(c, {t:#x});")
                else:
                    w(f"    rc = x86_rt_call(c, {nxt:#x});")
                w("    K = c->k; if (rc) return 1;")
                w(f"    if (K.eip != {nxt:#x}) goto dyn;")
            elif ins.kind in ("ret", "ijmp"):
                w("    goto dyn;")
            if ins.kind not in ("jmp", "ret", "ijmp"):
                following = plabels[i + 1] if i + 1 < len(plabels) else None
                if following != nxt:
                    w(goto(nxt, a, "    ") if nxt in labelset else leave(nxt, "    "))
        w("dyn:")
        w("    if (K.eip == entry_ret && K.r[ESP] - (entry_sp + 4) <= X86_CALLEE_POP) { c->k = K; return 0; }")
        w("    switch (K.eip) {")
        for a in sorted(entered):
            w(f"    case {a:#x}: goto L_{a:x};")
        if split:
            w("    default: break;")
            w("    }")
            w(f"    c->k = K; __attribute__((musttail)) return {name}_go({args.format('K.eip')});")
        else:
            w("    default: c->k = K; return 1;")
            w("    }")
        w("}")
        out_fns.append((p, entered, defs, body))

    fns = []
    for p, entered, defs, body in out_fns:
        head = []
        h = head.append
        if split:
            h(f"int {pname(p)}(X86 *c, uint32_t start, uint32_t entry_sp, uint32_t entry_ret) {{")
        else:
            h(f"// {fn.name}")
            h(f"int {name}(X86 *c, uint32_t start) {{")
            h("    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) < c->stack_limit, 0)) return 1;")
        h("    X86Core K = c->k;")
        h("    const volatile int *const abortp = c->abort ? c->abort : &x86_rt_never;")
        if not split:
            h("    const uint32_t entry_sp = K.r[ESP], entry_ret = k_rd32(&K, entry_sp);")
        h("    int rc;")
        h("    (void)rc;")
        h("    switch (start) {")
        for a in sorted(entered | parts_entered[p]):
            h(f"    case {a:#x}: goto L_{a:x};")
        h("    default: return 1;")
        h("    }")
        fns.append(defs + head + body)
    if not split:
        return name, resume, [(fns[0], len(labels))]
    # parts call each other and the router below, from any file
    sig = "(X86 *c, uint32_t start, uint32_t entry_sp, uint32_t entry_ret)"
    decl = [f"int {pname(p)}{sig};" for p in range(len(parts))] + [f"int {name}_go{sig};"]
    go = [f"int {name}_go{sig} {{"]
    for p in range(len(parts) - 1):
        go.append(f"    if (start < {parts[p + 1][0]:#x}) {{ __attribute__((musttail)) return {pname(p)}(c, start, entry_sp, entry_ret); }}")
    go.append(f"    __attribute__((musttail)) return {pname(len(parts) - 1)}(c, start, entry_sp, entry_ret);")
    go.append("}")
    entry = [f"// {fn.name}",
             f"int {name}(X86 *c, uint32_t start) {{",
             "    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) < c->stack_limit, 0)) return 1;",
             "    const uint32_t entry_sp = c->k.r[ESP];",
             f"    return {name}_go(c, start, entry_sp, k_rd32(&c->k, entry_sp));",
             "}"]
    units = [(decl + go + entry, 0)]
    units += [(decl + f, len(ls)) for f, ls in zip(fns, parts)]
    return name, resume, units


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("profile")
    ap.add_argument("outdir")
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--per-file", type=int, default=600, help="instructions per C file")
    ap.add_argument("--debug-calls", action="store_true",
                    help="route every call and jump between functions through the dispatcher "
                         "(for bisecting with MT_NATIVE_RANGE)")
    ap.add_argument("--coverage", type=float, default=1.0,
                    help="translate only the hottest functions that together ran this share of the "
                         "instructions (default: all that ran; code that runs once per utterance or "
                         "voice change is cold in the profile but still delays speech)")
    a = ap.parse_args()

    dump = Dump(a.dump)
    starts = {}
    for ln in open(a.profile):
        ln = ln.split("#")[0].split()
        if len(ln) not in (2, 3):
            continue
        try:
            im = dump.image(ln[0])
        except StopIteration:
            continue
        addr = int(ln[1], 16) + im["slide"]
        if dump.text_of(addr):
            starts[addr] = starts.get(addr, 0) + (int(ln[2]) if len(ln) == 3 else 1)

    # function boundaries: symbols in each __text section
    bounds, names = {}, {}
    for s in dump.texts:
        bounds[s["addr"]] = s["addr"]
    for image, addr, nm in dump.syms:
        if dump.text_of(addr):
            bounds[addr] = addr
            names.setdefault(addr, nm)
    blist = sorted(bounds)

    def extent(addr):
        i = bisect.bisect_right(blist, addr) - 1
        lo = blist[i]
        t = dump.text_of(addr)
        hi = blist[i + 1] if i + 1 < len(blist) else t["addr"] + t["size"]
        return lo, min(hi, t["addr"] + t["size"])

    funcs, weight = {}, {}
    for s, n in starts.items():
        lo, hi = extent(s)
        f = funcs.get(lo)
        if not f:
            f = funcs[lo] = Function(lo, hi, names.get(lo, "?"))
        f.roots.add(s)
        weight[lo] = weight.get(lo, 0) + n
    total_w, kept, acc = sum(weight.values()), {}, 0
    for lo in sorted(funcs, key=lambda lo: -weight[lo]):
        if acc >= a.coverage * total_w:
            break
        kept[lo] = funcs[lo]
        acc += weight[lo]
    funcs = kept
    for f in funcs.values():
        if f.start in starts:
            f.roots.add(f.start)
        explore(dump, f, starts)
        f.roots &= set(f.insns)

    entries_of = {}
    for f in funcs.values():
        for r in f.roots | f.branch_targets | f.return_sites:
            if r in f.insns:
                entries_of[r] = f"nx_{a.prefix}_{f.start:08x}"

    os.makedirs(a.outdir, exist_ok=True)
    order = sorted(funcs.values(), key=lambda f: f.start)
    header = [
        "// Generated by tools/recomp_x86.py from your own copy of MacinTalk. Do not edit or share.",
        "#define X86_OPS_INLINE static inline __attribute__((always_inline))",
        '#include "x86/x86_ops.h"',
        '#include "mt/mt_native.h"',
        "extern const int x86_rt_never;",
        "",
    ]
    decls = "\n".join(f"int nx_{a.prefix}_{f.start:08x}(X86 *c, uint32_t start);" for f in order)
    files, cur, cur_n, table, total = [], [], 0, [], 0
    for f in order:
        name, resume, units = emit_function(dump, f, entries_of, a.prefix, a.debug_calls, a.per_file)
        table += [(r, name) for r in resume]
        total += len(f.insns)
        for body, n in units:
            if cur and cur_n + n > a.per_file * 3 // 2:
                files.append(cur)
                cur, cur_n = [], 0
            cur += body + [""]
            cur_n += n
            if cur_n >= a.per_file:
                files.append(cur)
                cur, cur_n = [], 0
    if cur:
        files.append(cur)
    for i, body in enumerate(files):
        with open(os.path.join(a.outdir, f"{a.prefix}_{i:03d}.c"), "w") as fh:
            fh.write("\n".join(header + [decls, ""] + body))

    # the code ranges this was made from: the __text of every image it uses
    used = sorted({dump.text_of(f.start)["image"] for f in order})
    ranges = []
    for s in dump.texts:
        if s["image"] in used:
            from_dump = dump.read(s["addr"], s["size"])
            h = 0xcbf29ce484222325
            for byte in from_dump:
                h = ((h ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
            ranges.append((s["image"], s["addr"], s["addr"] + s["size"], h))
    table.sort()
    with open(os.path.join(a.outdir, f"{a.prefix}_table.c"), "w") as fh:
        fh.write("\n".join(header + [decls, "",
                 f"static const X86Native table[] = {{"] +
                 [f"    {{{addr:#x}, {fn}}}," for addr, fn in table] +
                 ["};", "static const MtNativeRange ranges[] = {"] +
                 [f'    {{"{n}", {lo:#x}, {hi:#x}, {h:#x}ull}},' for n, lo, hi, h in ranges] +
                 ["};",
                  f'static const MtNativeSet set = {{"{a.prefix}", ranges, {len(ranges)}, table, {len(table)}}};',
                  f"__attribute__((constructor)) static void register_{a.prefix}(void) {{ mt_native_register(&set); }}",
                  ""]))
    print(f"{len(funcs)} functions ({100 * acc / max(total_w, 1):.2f}% of the profile), {total} instructions, "
          f"{len(table)} entry points, {len(files)} files")


if __name__ == "__main__":
    main()
