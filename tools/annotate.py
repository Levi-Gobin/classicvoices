#!/usr/bin/env python3
"""Annotate an llvm-objdump Thumb disassembly of a stripped Mach-O: resolve
pc-relative literal loads (ldr rX,[pc,#] / add rX,pc) to selectors, C strings,
CFStrings, class refs and stub names. Usage: annotate.py <macho> <dis> <lo> <hi>"""
import re, struct, subprocess, sys

binpath, dispath, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
d = open(binpath, "rb").read()
out = subprocess.run(["otool", "-l", binpath], capture_output=True, text=True).stdout
secs, cur = [], {}
for line in out.splitlines():
    t = line.split()
    if len(t) >= 2 and t[0] in ("sectname", "addr", "size", "offset"):
        cur[t[0]] = t[1] if t[0] == "sectname" else int(t[1], 0)
        if t[0] == "offset":
            secs.append(dict(cur))
def sec_of(a):
    for s in secs:
        if s["addr"] <= a < s["addr"] + s["size"]:
            return s
def rd32(a):
    s = sec_of(a)
    if not s or s["offset"] == 0: return None
    return struct.unpack_from("<I", d, s["offset"] + a - s["addr"])[0]
def cstr(a):
    s = sec_of(a)
    if not s: return None
    o = s["offset"] + a - s["addr"]
    return d[o:d.index(b"\0", o)].decode("latin1")
stubs = {}
ind = subprocess.run(["otool", "-Iv", binpath], capture_output=True, text=True).stdout
for line in ind.splitlines():
    p = line.split()
    if len(p) >= 3 and p[0].startswith("0x"):
        stubs[int(p[0], 16)] = p[-1]
def describe(a):
    s = sec_of(a)
    if not s: return None
    n = s["sectname"]
    if n == "__cstring": return repr(cstr(a))
    if n == "__cfstring":
        p = rd32(a + 8)
        return "@" + repr(cstr(p) if p else None)
    if n == "__objc_selrefs":
        v = rd32(a)
        return "SEL " + ((cstr(v) if v else None) or "?")
    if n == "__objc_classrefs":
        return "CLASS@" + hex(rd32(a) or 0)
    if n in ("__nl_symbol_ptr", "__lazy_symbol"): return "ptr " + stubs.get(a, "?")
    return n + "+" + hex(a - s["addr"])
lines = open(dispath).read().splitlines()
ins = []
for l in lines:
    m = re.match(r"\s+([0-9a-f]+):\s+(\S+)\s*(.*)", l)
    if m: ins.append((int(m.group(1), 16), m.group(2), m.group(3)))
pending = {}
for i, (a, mn, ops) in enumerate(ins):
    if not (lo <= a < hi): continue
    note = ""
    m = re.match(r"(r\d+), \[pc, #(-?0x[0-9a-f]+|-?\d+)\]", ops)
    if mn.startswith("ldr") and m:
        v = rd32(((a + 4) & ~3) + int(m.group(2), 0))
        if v is not None:
            pending[m.group(1)] = v
            dsc = describe(v)
            if dsc: note = dsc
    mw = re.match(r"(r\d+), #(0x[0-9a-f]+|\d+)", ops)
    if mn == "movw" and mw:
        pending[mw.group(1)] = int(mw.group(2), 0)
    if mn == "movt" and mw and mw.group(1) in pending:
        pending[mw.group(1)] = (pending[mw.group(1)] & 0xFFFF) | (int(mw.group(2), 0) << 16)
    if mn == "add" and ops.replace(" ", "").endswith(",pc"):
        reg = ops.split(",")[0].strip()
        if reg in pending:
            dsc = describe((pending[reg] + a + 4) & 0xFFFFFFFF)
            if dsc: note = dsc
    m3 = re.search(r"blx?\s+0x([0-9a-f]+)", mn + " " + ops)
    if m3 and int(m3.group(1), 16) in stubs: note = stubs[int(m3.group(1), 16)]
    print(f"{a:6x}: {mn:8} {ops[:40]:40} {('; ' + note) if note else ''}")
