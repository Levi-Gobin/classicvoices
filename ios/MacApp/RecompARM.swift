// tools/recomp.py in Swift, so the Mac app can make its own fast engine code
// on the user's Mac: the C it writes is derived from Apple's engine binaries,
// so it is only ever generated from the user's own copy, never shipped.
//
// It must write exactly what recomp.py writes, byte for byte (like the
// Android port, Recompiler.java). Check with a small command-line wrapper:
//
//   swiftc -O ios/MacApp/RecompARM.swift main.swift -o recomp
//   ./recomp voiced tools/profiles/ios6.txt out ios6
//
// and `diff -r` against recomp.py's output. Thumb-2 functions in the engine
// become C that calls the interpreter's own instruction semantics with
// constant encodings, which the compiler folds into native code.

import Foundation

enum ArmRecompiler {
    struct Failure: Error, LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    /// Same as `recomp.py <voiced> <profile> <outdir> --prefix <prefix> [--scan-bl]` with default --per-file.
    static func run(voiced: URL, profile: URL, outdir: URL, prefix: String, scanBL: Bool) throws {
        let mo = try MachO(Data(contentsOf: voiced))
        let seen: [Int]
        if profile.path.hasSuffix(".txt") {
            seen = try readTextProfile(profile)
        } else {
            // build/profile output: one byte per halfword
            let prof = [UInt8](try Data(contentsOf: profile))
            var s = [Int]()
            for (i, b) in prof.enumerated() where b != 0 { s.append(i * 2) }
            seen = s
        }
        let gen = Generator(mo: mo, prefix: prefix)
        try gen.run(seen: seen, outdir: outdir, scanBL: scanBL, perFile: 4000)
    }

    // tools/profiles/*.txt: one hex address per line (block starts seen while profiling)
    private static func readTextProfile(_ url: URL) throws -> [Int] {
        let data = try Data(contentsOf: url)
        guard let text = String(data: data, encoding: .utf8) else {
            throw Failure(message: "\(url.path): not UTF-8")
        }
        var set = Set<Int>()
        // Python's text mode: "\r\n" and a lone "\r" end lines too
        let lines = text.replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n").split(separator: "\n", omittingEmptySubsequences: false)
        for line in lines {
            let body = line.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first ?? ""
            let ln = body.trimmingCharacters(in: .whitespacesAndNewlines)
            if ln.isEmpty { continue }
            guard let v = parsePythonHex(ln) else {
                throw Failure(message: "\(url.path): bad address \"\(ln)\"")
            }
            set.insert(v)
        }
        return set.sorted()
    }

    /// int(s, 16): an optional sign, an optional 0x, digits with single underscores between them.
    private static func parsePythonHex(_ s: String) -> Int? {
        var t = Substring(s)
        var neg = false
        if t.hasPrefix("+") || t.hasPrefix("-") { neg = t.hasPrefix("-"); t = t.dropFirst() }
        if t.hasPrefix("0x") || t.hasPrefix("0X") {
            t = t.dropFirst(2)
            if t.hasPrefix("_") { t = t.dropFirst() }
        }
        if t.isEmpty || t.hasPrefix("_") || t.hasSuffix("_") || t.contains("__") { return nil }
        guard let v = Int(t.replacingOccurrences(of: "_", with: ""), radix: 16) else { return nil }
        return neg ? -v : v
    }
}

// MARK: - Tables (recomp.py's)

private let DIV_HELPERS: [String: String] = [
    "___divsi3": "K.r[0] = rt_sdiv(K.r[0], K.r[1]);",
    "___modsi3": "K.r[0] = rt_smod(K.r[0], K.r[1]);",
    "___udivsi3": "K.r[0] = K.r[1] ? K.r[0] / K.r[1] : 0;",
    "___umodsi3": "K.r[0] = K.r[1] ? K.r[0] % K.r[1] : 0;",
]

// Guest functions whose calls become host code (core/recomp_rt.h), by symbol,
// in the builds whose code was checked against them (recomp.py HOST_FUNCS).
private let HOST_FUNCS: [String: String] = [
    "_cstdlib_strlen": "rt_strlen(&K);",
    "_cstdlib_strchr": "rt_strchr(&K);",
    "_cstdlib_strcmp": "rt_strcmp(&K);",
    "_cstdlib_strncmp": "rt_strncmp(&K);",
    "_cstdlib_strstr": "rt_strstr(&K);",
    "_cstdlib_memcpy": "rt_memcpy(&K);",
    "_cstdlib_memmove": "rt_memmove(&K);",
    "_log_HasTraceTuningDataSubscriber": "K.r[0] = 0;",
    "_fxd_S32ShMultRndS32S32": "rt_fxd_mult_rnd(&K);",
]
private let HOST_FUNC_BUILDS: Set<String> = ["ios70", "ios7"]

// While a stop is pending (Cpu.interrupt) these functions are answered by
// host code instead of being called (recomp.py INTERRUPT_FUNCS).
private let INTERRUPT_FUNCS: [String: [String]] = [
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
]

// Functions whose leaf calls pass a copy of the registers (recomp.py COPY_FUNCS),
// and the builds whose indirect calls to leaves do (ICALL_COPY_BUILDS).
private let COPY_FUNCS: Set<String> = ["_activeprompt_Consult"]
private let ICALL_COPY_BUILDS: Set<String> = ["ios3", "ios4", "ios5", "ios6", "ios70", "ios7", "ios10"]
private let TINY_LEAF = 160

private func hx(_ v: Int) -> String { "0x" + String(v, radix: 16) }

private struct HostLoop: Sendable {
    let chunks: [(Int, [(Int, Int?)])]
    let after: Bool
    let exits: [Int]
    let code: @Sendable (Int) -> [String]
}

// recomp.py HOST_LOOPS: hot loops run as host code (see its comments).
private let HOST_LOOPS: [HostLoop] = [
    HostLoop(
        chunks: [
            (0, [(0xb2a3, nil), (0x5d, nil), (0xfa15, 0xf584), (0xf852, 0x5035), (0x2d22, nil), (0xd106, nil), (0xeb03, 0x343), (0xeb02, 0x3c3), (0x68db, nil), (0x428b, nil), (0xd003, nil), (0x3401, nil), (0xb2a3, nil), (0x4283, nil), (0xd3ec, nil)]),
        ],
        after: true, exits: [],
        code: { _ in [
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
        ] }),
    HostLoop(
        chunks: [
            (0, [(0x4621, nil), (0x9818, nil), (0x9121, nil), (0xfa10, 0xf081), (0xf8cd, 0x8088), (0xb280, nil), (0x901a, nil), (0xb288, nil), (0x902e, nil), (0x2000, nil), (0x2800, nil), (0xf041, 0x8126), (0x981b, nil), (0x9a1a, nil), (0x6800, nil), (0x4440, nil), (0x4282, nil), (0xf201, 0x82bd), (0xf8db, 0x0), (0x2300, nil), (0x9a2e, nil), (0xf04f, 0xb00), (0x9132, nil), (0x5c80, nil), (0x2800, nil), (0xf001, 0x82d0), (0x9d32, nil), (0x9330, nil), (0xe00a, nil)]),
            (94, [(0x9836, nil), (0xb2ae, nil), (0x4286, nil), (0xf080, 0x8095), (0xf64f, 0x70ff), (0xea1b, 0xf00), (0xd018, nil), (0x1c70, nil), (0x9936, nil), (0x4288, nil), (0xd214, nil), (0x9934, nil), (0x6809, nil), (0x5d8a, nil), (0x2a20, nil), (0xd10f, nil), (0xf241, 0x848), (0x180c, nil), (0xf2c0, 0x814), (0x44f8, nil), (0x4640, nil), (0xf765, 0xf8c1), (0x4602, nil), (0x4620, nil), (0x4641, nil), (0xf765, 0xf8de), (0x2800, nil), (0xd0d1, nil), (0x46a8, nil), (0x68bd, nil), (0xf64f, 0x70ff), (0xea18, 0xf00), (0xd011, nil), (0x9834, nil), (0x882c, nil), (0x6800, nil), (0x4430, nil), (0xf910, 0x1c01), (0xf64c, 0x2016), (0xf2c0, 0x14), (0x4478, nil), (0xf765, 0xf87e), (0xb918, nil), (0xf8cd, 0x80c8), (0x882a, nil), (0xe01e, nil), (0x8828, nil), (0x2200, nil), (0x2400, nil), (0xf8cd, 0x80c8), (0xb1c0, nil)]),
            (276, [(0xf8dd, 0x80d4), (0xb2a3, nil), (0x4293, nil), (0xd218, nil)]),
            (336, [(0x9b30, nil), (0x9824, nil), (0xb299, nil), (0x4281, nil), (0xd21a, nil), (0x9834, nil), (0x2400, nil), (0xf8d8, 0x201c), (0x3301, nil), (0x6800, nil), (0x5c51, nil), (0x5d80, nil), (0x4281, nil), (0xbf08, nil), (0x2401, nil), (0x9a32, nil), (0x4281, nil), (0xf102, 0x201), (0x9232, nil), (0xf43f, 0xaf62), (0x9832, nil), (0x4646, nil), (0x992e, nil), (0xf8dd, 0xb0d0), (0xb280, nil), (0x1a40, nil), (0x901e, nil), (0xe1c5, nil)]),
            (1308, [(0x9831, nil), (0x6900, nil), (0xf761, 0xfef0), (0x2801, nil), (0xd174, nil)]),
            (1554, [(0x2c01, nil), (0xf040, 0x84d3)]),
            (4030, [(0xf8dd, 0x8088), (0x4621, nil), (0x4608, nil), (0x9921, nil), (0xf64b, 0x350a), (0x2801, nil), (0xf2c0, 0x514), (0x447d, nil), (0xf000, 0x8149), (0x2800, nil), (0xf47f, 0xa81d), (0xe126, nil)]),
            (4654, [(0xf8db, 0x0), (0x460c, nil), (0xb2a1, nil), (0x5c40, nil), (0xb140, nil), (0xb241, nil), (0x4628, nil), (0xf763, 0xffc3), (0x1c61, nil), (0x2800, nil), (0xd0f2, nil), (0xe000, nil), (0x3401, nil), (0xf8db, 0x0), (0xb2a1, nil), (0x5c40, nil), (0x2800, nil), (0xb241, nil), (0xf43e, 0xaed2), (0x4628, nil), (0xf763, 0xffb3), (0x2800, nil), (0xf43e, 0xaecc), (0xe7ef, nil)]),
        ],
        after: false, exits: [],
        code: { a in [
            "{   // recomp.py HOST_LOOPS: a phrase against each word, until a character differs",
            "    const uint32_t sp = K.r[13], cntp = g32(&K, K.r[7] + 8), D = \(hx(a + 0x14cae0));",
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
            "            if (c16) { lr = \(hx((a + 0xcc) | 1)); scan = rt_in_str(&K, D, (uint8_t)g8(&K, text + c16 - 1)); }",
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
            "        g32(&K, g32(&K, sp + 0xc4) + 0x10); lr = \(hx((a + 0x524) | 1));",
            "        const uint32_t tx = g32(&K, r11);",
            "        uint32_t b;",
            "        for (;;) { b = g8(&K, tx + (r4 & 0xffffu)); if (!b) break; lr = \(hx((a + 0x1242) | 1)); if (rt_in_str(&K, D, (uint8_t)b)) break; r4++; }",
            "        for (;;) { b = g8(&K, tx + (r4 & 0xffffu)); if (!b) break; lr = \(hx((a + 0x1262) | 1)); if (!rt_in_str(&K, D, (uint8_t)b)) break; r4++; }",
            "        K.r[0] = 0; K.r[1] = (uint32_t)(int32_t)(int8_t)b; K.r[2] = cur + 1; K.r[3] = k + 1; K.r[4] = r4;",
            "        K.r[5] = D; K.r[6] = prompt; K.r[8] = r8; K.r[11] = r11; K.r[14] = lr;",
            "        K.n = 0; K.z = 1; K.c = 1; K.v = 0;",
            "    }",
            "}",
        ] }),
    HostLoop(
        chunks: [
            (-4, [(0xf8db, 0xb000)]),
            (0, [(0xf1bb, 0xf00), (0xd04b, nil), (0x9825, nil), (0xf9bb, 0x1010), (0x4281, nil), (0xd1f5, nil)]),
        ],
        after: false, exits: [0x9e, 0x10],
        code: { a in [
            "{   // recomp.py HOST_LOOPS: the parser's hash chain, to the entry with a key",
            "    uint32_t n = K.r[11];",
            "    for (;;) {",
            "        if (!n) { K.r[11] = 0; K.n = 0; K.z = 1; K.c = 1; K.v = 0; goto L_\(String(a + 0x9e, radix: 16)); }",
            "        const uint32_t key = g32(&K, K.r[13] + 0x94), f = (uint32_t)(int32_t)(int16_t)g16(&K, n + 0x10);",
            "        K.r[0] = key; K.r[1] = f;",
            "        if (f == key) { K.r[11] = n; K.n = 0; K.z = 1; K.c = 1; K.v = 0; goto L_\(String(a + 0x10, radix: 16)); }",
            "        n = g32(&K, n);",
            "    }",
            "}",
        ] }),
]

// MARK: - Mach-O

private struct MachO {
    let d: [UInt8]
    var segs: [(vm: Int, fo: Int, fs: Int)] = []
    var syms: [(addr: Int, name: [UInt8])] = []  // sorted, unique
    var text: (Int, Int) = (0, 0)
    var stubNames: [Int: [UInt8]] = [:]
    var stubRange: (Int, Int) = (0, 0)

    init(_ data: Data) throws {
        d = [UInt8](data)
        func u32(_ o: Int) throws -> Int {
            guard o >= 0, o + 4 <= d.count else { throw ArmRecompiler.Failure(message: "truncated Mach-O") }
            return Int(d[o]) | Int(d[o + 1]) << 8 | Int(d[o + 2]) << 16 | Int(d[o + 3]) << 24
        }
        guard try u32(0) == 0xFEEDFACE else { throw ArmRecompiler.Failure(message: "not a 32-bit Mach-O") }
        let ncmds = try u32(16)
        var off = 28
        var symoff = 0, nsyms = 0, stroff = 0, indoff = 0
        var haveText = false
        var stubs: (Int, Int, Int, Int)? = nil
        for _ in 0..<ncmds {
            let cmd = try u32(off), size = try u32(off + 4)
            if cmd == 1 {
                let vmaddr = try u32(off + 24), fileoff = try u32(off + 32), filesize = try u32(off + 36)
                let nsects = try u32(off + 48)
                segs.append((vmaddr, fileoff, filesize))
                for s in 0..<nsects {
                    let so = off + 56 + 68 * s
                    guard so + 68 <= d.count else { throw ArmRecompiler.Failure(message: "truncated Mach-O") }
                    var nameEnd = so + 16
                    while nameEnd > so && d[nameEnd - 1] == 0 { nameEnd -= 1 }
                    let sect = Array(d[so..<nameEnd])
                    let addr = try u32(so + 32), ssize = try u32(so + 36), flags = try u32(so + 56)
                    let r1 = try u32(so + 60), r2 = try u32(so + 64)
                    if sect == Array("__text".utf8) { text = (addr, addr + ssize); haveText = true }
                    if flags & 0xFF == 8 { stubs = (addr, ssize, r1, r2) }
                }
            } else if cmd == 2 {
                symoff = try u32(off + 8); nsyms = try u32(off + 12); stroff = try u32(off + 16)
            } else if cmd == 0xB {
                indoff = try u32(off + 56)
            }
            off += size
        }
        var names = [[UInt8]]()
        names.reserveCapacity(nsyms)
        struct Sym: Hashable { let addr: Int; let name: [UInt8] }
        var symSet = Set<Sym>()
        for i in 0..<nsyms {
            let e = symoff + 12 * i
            let strx = try u32(e), value = try u32(e + 8)
            guard e + 12 <= d.count else { throw ArmRecompiler.Failure(message: "truncated Mach-O") }
            let ntype = Int(d[e + 4]), nsect = Int(d[e + 5])
            var end = stroff + strx
            guard end >= 0, end < d.count else { throw ArmRecompiler.Failure(message: "bad symbol name") }
            while end < d.count && d[end] != 0 { end += 1 }
            guard end < d.count else { throw ArmRecompiler.Failure(message: "bad symbol name") }
            let nm = Array(d[(stroff + strx)..<end])
            names.append(nm)
            if ntype & 0x0E == 0x0E && nsect != 0 && haveText && text.0 <= value && value < text.1 {
                symSet.insert(Sym(addr: value & ~1, name: nm))
            }
        }
        // sorted(set(...)): by address, then name (Python compares latin-1 strings by code point = byte)
        syms = symSet.sorted { $0.addr != $1.addr ? $0.addr < $1.addr : $0.name.lexicographicallyPrecedes($1.name) }
            .map { ($0.addr, $0.name) }
        if let (addr, ssize, r1, r2) = stubs {
            guard r2 > 0 else { throw ArmRecompiler.Failure(message: "bad stub size") }
            for k in 0..<(ssize / r2) {
                let ind = try u32(indoff + 4 * (r1 + k))
                if ind & 0xC000_0000 == 0 {
                    guard ind < names.count else { throw ArmRecompiler.Failure(message: "bad indirect symbol") }
                    stubNames[addr + k * r2] = names[ind]
                }
            }
            stubRange = (addr, addr + ssize)
        }
        guard haveText else { throw ArmRecompiler.Failure(message: "no __text section") }
    }

    /// File offset of `n` bytes at `addr`, or nil where no segment holds them.
    @inline(__always) func offset(_ addr: Int, _ n: Int) -> Int? {
        for s in segs where s.vm <= addr && addr + n <= s.vm + s.fs {
            let o = s.fo + addr - s.vm
            return o + n <= d.count ? o : nil
        }
        return nil
    }

    @inline(__always) func h16(_ addr: Int) -> Int? {
        guard let o = offset(addr, 2) else { return nil }
        return Int(d[o]) | Int(d[o + 1]) << 8
    }

    @inline(__always) func u8(_ addr: Int) -> Int? {
        guard let o = offset(addr, 1) else { return nil }
        return Int(d[o])
    }
}

@inline(__always) private func sext(_ v: Int, _ bits: Int) -> Int {
    let m = 1 << (bits - 1)
    return (v ^ m) - m
}

/// BL targets in __text, found the way llvm-objdump's linear sweep sees them
/// (an IT block's conditional BLs left out): a stripped engine's function boundaries.
private func scanBL(_ mo: MachO) throws -> Set<Int> {
    let (lo, hi) = mo.text
    guard let base = mo.offset(lo, hi - lo) else { throw ArmRecompiler.Failure(message: "__text outside the segments") }
    let d = mo.d
    var out = Set<Int>()
    var a = 0
    let n = hi - lo
    var itLeft = 0
    while a + 2 <= n {
        let hw1 = Int(d[base + a]) | Int(d[base + a + 1]) << 8
        let inIt = itLeft > 0
        if inIt { itLeft -= 1 }
        if hw1 & 0xFF00 == 0xBF00 && hw1 & 0xF != 0 {  // IT
            let mask = hw1 & 0xF
            itLeft = (hw1 >> 4) & 0xF == 0xE ? 0 : 5 - (mask.trailingZeroBitCount + 1)
            a += 2
            continue
        }
        let top = hw1 >> 11
        if (top == 0x1D || top == 0x1E || top == 0x1F) && a + 4 <= n {  // 32-bit
            let hw2 = Int(d[base + a + 2]) | Int(d[base + a + 3]) << 8
            if top == 0x1E && hw2 & 0xD000 == 0xD000 && !inIt {  // BL
                let s = (hw1 >> 10) & 1
                let i1 = 1 ^ (((hw2 >> 13) & 1) ^ s), i2 = 1 ^ (((hw2 >> 11) & 1) ^ s)
                let t = lo + a + 4 + sext(s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1, 25)
                if lo <= t && t < hi { out.insert(t) }
            }
            a += 4
        } else {
            a += 2
        }
    }
    return out
}

// MARK: - Decoding

private enum Kind: UInt8 { case op, it, b, bcond, bl, blxStub, icall, ijump, tb, stop }

private struct Insn {
    let addr: Int
    let hw1: Int
    let hw2: Int
    let size: Int
    var kind: Kind = .op
    var target: Int = -1   // -1: none
    var cond: Int = -1     // IT condition (-1 = None, always)
    var inIt: Int = 0
    var stub: [UInt8]? = nil

    init(_ addr: Int, _ hw1: Int, _ hw2: Int, _ size: Int) {
        self.addr = addr; self.hw1 = hw1; self.hw2 = hw2; self.size = size
    }

    /// cond in (None, 14)
    @inline(__always) var always: Bool { cond == -1 || cond == 14 }

    /// VFP/NEON (coprocessor) encodings, and the NEON loads and stores.
    @inline(__always) var usesVFP: Bool {
        size == 4 && ((hw1 & 0xEC00) == 0xEC00 || (hw1 & 0xFF10) == 0xF900)
    }
}

private func decode(_ mo: MachO, _ addr: Int) -> Insn? {
    guard let hw1 = mo.h16(addr) else { return nil }
    if (hw1 >> 11) >= 0x1D {
        guard let hw2 = mo.h16(addr + 2) else { return nil }
        var ins = Insn(addr, hw1, hw2, 4)
        if (hw1 & 0xF800) == 0xF000 && hw2 & 0x8000 != 0 {
            let s = (hw1 >> 10) & 1, j1 = (hw2 >> 13) & 1, j2 = (hw2 >> 11) & 1
            let kind = hw2 & 0x5000
            if kind == 0 {
                let cond = (hw1 >> 6) & 15
                if cond & 0xE == 0xE { return ins }  // misc control (hints, barriers)
                let imm = (s << 20) | (j2 << 19) | (j1 << 18) | ((hw1 & 0x3F) << 12) | ((hw2 & 0x7FF) << 1)
                ins.kind = .bcond; ins.target = (addr + 4 + sext(imm, 21)) & 0xFFFF_FFFF
                return ins
            }
            let i1 = 1 - (j1 ^ s), i2 = 1 - (j2 ^ s)
            let imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1)
            let off = sext(imm, 25)
            if kind == 0x1000 {
                ins.kind = .b; ins.target = (addr + 4 + off) & 0xFFFF_FFFF
            } else if kind == 0x5000 {
                ins.kind = .bl; ins.target = (addr + 4 + off) & 0xFFFF_FFFF
            } else {
                let t = (((addr + 4) & ~3) + off) & 0xFFFF_FFFF
                if mo.stubRange.0 <= t && t < mo.stubRange.1 {
                    ins.kind = .blxStub; ins.target = t; ins.stub = mo.stubNames[t]
                } else {
                    ins.kind = .icall  // BLX to ARM code: let the dispatcher handle it
                }
            }
            return ins
        }
        if (hw1 & 0xFE40) == 0xE800 && hw1 & 0x10 != 0 && hw2 & 0x8000 != 0 {
            ins.kind = .ijump  // LDM with PC
        } else if (hw1 & 0xFFF0) == 0xE8D0 && (hw2 & 0xFFE0) == 0xF000 {
            ins.kind = .tb
        } else if (hw1 & 0xFE00) == 0xF800 && hw1 & 0x10 != 0 && (hw1 >> 5) & 3 == 2 && hw2 >> 12 == 15 {
            ins.kind = .ijump  // LDR PC
        } else if (hw1 & 0xFE00) == 0xEA00 && (hw2 >> 8) & 15 == 15 && hw1 & 0x10 == 0 {
            ins.kind = .ijump  // data-processing into PC
        }
        return ins
    }
    var ins = Insn(addr, hw1, 0, 2)
    if (hw1 & 0xF000) == 0xD000 {
        let cond = (hw1 >> 8) & 15
        if cond >= 14 {
            ins.kind = .stop
        } else {
            ins.kind = .bcond; ins.target = (addr + 4 + sext((hw1 & 0xFF) << 1, 9)) & 0xFFFF_FFFF
        }
    } else if (hw1 & 0xF800) == 0xE000 {
        ins.kind = .b; ins.target = (addr + 4 + sext((hw1 & 0x7FF) << 1, 12)) & 0xFFFF_FFFF
    } else if (hw1 & 0xF500) == 0xB100 {
        let off = (((hw1 >> 9) & 1) << 6) | (((hw1 >> 3) & 31) << 1)
        ins.kind = .bcond; ins.target = addr + 4 + off
    } else if (hw1 & 0xFF00) == 0x4700 {
        ins.kind = hw1 & 0x80 != 0 ? .icall : .ijump
    } else if (hw1 & 0xFF00) == 0xBD00 {
        ins.kind = .ijump
    } else if (hw1 & 0xFC00) == 0x4400 && ((hw1 >> 8) & 3 == 0 || (hw1 >> 8) & 3 == 2)
                && (((hw1 >> 4) & 8) | (hw1 & 7)) == 15 {
        ins.kind = .ijump
    } else if (hw1 & 0xFF00) == 0xBF00 && hw1 & 0xF != 0 {
        ins.kind = .it
    } else if (hw1 & 0xFF00) == 0xBE00 {
        ins.kind = .stop
    }
    return ins
}

/// Decode a TBB/TBH table that directly follows the instruction.
private func tbTargets(_ mo: MachO, _ ins: Insn) throws -> [Int] {
    let base = ins.addr + 4
    let half = ins.hw2 & 0x10 != 0
    if (ins.hw1 & 15) != 15 { return [] }  // table not PC-relative
    var targets = [Int]()
    var pos = base
    var end: Int? = nil
    while end == nil || pos < end! {
        let e: Int
        if half {
            guard let v = mo.h16(pos) else { throw ArmRecompiler.Failure(message: "jump table outside the segments at \(hx(pos))") }
            e = v; pos += 2
        } else {
            guard let v = mo.u8(pos) else { throw ArmRecompiler.Failure(message: "jump table outside the segments at \(hx(pos))") }
            e = v; pos += 1
        }
        let t = base + 2 * e
        targets.append(t)
        end = end == nil ? t : min(end!, t)
        if targets.count > 4096 { break }
    }
    return targets
}

private final class Function {
    let start: Int, end: Int
    var insns: [Int: Insn] = [:]
    var roots = Set<Int>()
    var branchTargets = Set<Int>()
    var returnSites = Set<Int>()
    init(_ start: Int, _ end: Int) { self.start = start; self.end = end }
}

private func explore(_ mo: MachO, _ fn: Function) throws {
    var work = fn.roots.sorted()
    var itCond = [Int: Int]()  // addr -> cond (in_it = 1)
    while let a = work.popLast() {
        if fn.insns[a] != nil || !(fn.start <= a && a < fn.end) { continue }
        guard var ins = decode(mo, a) else { continue }
        if let c = itCond[a] { ins.cond = c; ins.inIt = 1 }
        fn.insns[a] = ins
        let nxt = a + ins.size
        if ins.kind == .it {
            var state = ins.hw1 & 0xFF, p = nxt
            while state & 0xF != 0 {
                itCond[p] = state >> 4
                state = state & 7 != 0 ? ((state & 0xE0) | ((state << 1) & 0x1F)) : 0
                guard let n = decode(mo, p) else { break }
                p += n.size
            }
        }
        let conditional = !ins.always
        if (ins.kind == .b || ins.kind == .bcond) && ins.target != -1 {
            fn.branchTargets.insert(ins.target)
            if fn.start <= ins.target && ins.target < fn.end { work.append(ins.target) }
        }
        if ins.kind == .tb {
            for t in try tbTargets(mo, ins) {
                fn.branchTargets.insert(t)
                if fn.start <= t && t < fn.end { work.append(t) }
            }
        }
        if ins.kind == .bl || ins.kind == .blxStub || ins.kind == .icall { fn.returnSites.insert(nxt) }
        let terminator = (ins.kind == .b || ins.kind == .ijump || ins.kind == .tb || ins.kind == .stop) && !conditional
        if !terminator { work.append(nxt) }
    }
}

/// The instructions reachable from `root` if they make a leaf: no calls, no
/// branches out of the function, no VFP/NEON, at most `limit` of them; else nil.
private func leafRegion(_ fn: Function, _ root: Int, limit: Int = 96) -> Set<Int>? {
    var seen = Set<Int>()
    var work = [root]
    while let a = work.popLast() {
        if seen.contains(a) { continue }
        guard let ins = fn.insns[a] else { return nil }
        switch ins.kind {
        case .bl, .blxStub, .icall, .stop: return nil
        default: break
        }
        if ins.usesVFP { return nil }
        seen.insert(a)
        if seen.count > limit { return nil }
        let nxt = a + ins.size
        let conditional = !ins.always
        if ins.kind == .b || ins.kind == .bcond {
            if !(fn.start <= ins.target && ins.target < fn.end) { return nil }
            work.append(ins.target)
        }
        if ins.kind == .tb { return nil }  // its targets aren't kept per instruction
        if !((ins.kind == .b || ins.kind == .ijump) && !conditional) { work.append(nxt) }
    }
    return seen
}

// MARK: - Output

/// A byte buffer with the number formats recomp.py's f-strings use.
private final class Buf {
    var b = [UInt8]()
    init(capacity: Int = 0) { b.reserveCapacity(capacity) }

    @inline(__always) func s(_ x: StaticString) {
        x.withUTF8Buffer { b.append(contentsOf: $0) }
    }
    @inline(__always) func str(_ x: String) { b.append(contentsOf: x.utf8) }
    @inline(__always) func bytes(_ x: [UInt8]) { b.append(contentsOf: x) }
    @inline(__always) func nl() { b.append(10) }

    /// {v:x}, zero-padded to `width` digits ({v:06x})
    func x(_ v: Int, width: Int = 1) {
        precondition(v >= 0)
        var digits = 1
        var t = v >> 4
        while t != 0 { digits += 1; t >>= 4 }
        if width > digits { for _ in 0..<(width - digits) { b.append(0x30) } }
        var shift = (digits - 1) * 4
        while shift >= 0 {
            let n = (v >> shift) & 15
            b.append(n < 10 ? UInt8(0x30 + n) : UInt8(0x57 + n))
            shift -= 4
        }
    }
    /// {v:#x}
    @inline(__always) func hx(_ v: Int) { b.append(0x30); b.append(0x78); x(v) }
    /// {v:#06x}
    @inline(__always) func hx04(_ v: Int) { b.append(0x30); b.append(0x78); x(v, width: 4) }
    @inline(__always) func dec(_ v: Int) { str(String(v)) }
}

private final class Generator {
    let mo: MachO
    let prefix: String
    let pfx: [UInt8]
    var hostAt = [Int: String]()         // guest address -> host code
    var interruptAt = [Int: [String]]()  // guest address -> host code while a stop is pending
    var leafOf = [Int: Bool]()           // guest address -> its leaf (true: lfi_, always inlined)
    var copyFuncs = Set<Int>()
    var nocopyFuncs = Set<Int>()
    var entriesOf = [Int: Int]()         // guest address -> start of the function that has it

    init(mo: MachO, prefix: String) {
        self.mo = mo
        self.prefix = prefix
        self.pfx = Array(prefix.utf8)
    }

    // names
    @inline(__always) func nf(_ o: Buf, _ start: Int) { o.s("nf_"); o.bytes(pfx); o.s("_"); o.x(start, width: 6) }
    @inline(__always) func leafName(_ o: Buf, _ e: Int) {
        o.s(leafOf[e]! ? "lfi_" : "lf_"); o.bytes(pfx); o.s("_"); o.x(e, width: 6)
    }

    func run(seen: [Int], outdir: URL, scanBL scan: Bool, perFile: Int) throws {
        let isHostBuild = HOST_FUNC_BUILDS.contains(prefix)
        for (addr, nmBytes) in mo.syms {
            let nm = String(decoding: nmBytes, as: Unicode.ASCII.self)
            let ascii = !nmBytes.contains { $0 >= 0x80 }
            if isHostBuild && ascii {
                if let h = HOST_FUNCS[nm] { hostAt[addr] = h }
                if let i = INTERRUPT_FUNCS[nm] { interruptAt[addr] = i }
                if COPY_FUNCS.contains(nm) { copyFuncs.insert(addr) }
            }
            if nmBytes.starts(with: Array("_uselect_".utf8)) { nocopyFuncs.insert(addr) }
        }
        let starts = seen.filter { mo.text.0 <= $0 && $0 < mo.text.1 }

        var boundSet = Set(mo.syms.map { $0.addr })
        boundSet.insert(mo.text.0)
        if scan { boundSet.formUnion(try scanBL(mo)) }
        let bounds = boundSet.sorted()

        func extent(_ addr: Int) -> (Int, Int) {
            // bisect_right(bounds, addr) - 1
            var lo = 0, hi = bounds.count
            while lo < hi {
                let mid = (lo + hi) / 2
                if addr < bounds[mid] { hi = mid } else { lo = mid + 1 }
            }
            let i = lo - 1
            let l = bounds[i >= 0 ? i : bounds.count - 1]  // Python's bounds[-1]
            let h = i + 1 < bounds.count ? bounds[i + 1] : mo.text.1
            return (l, h)
        }

        // dict insertion order: starts are sorted, so this is ascending start
        var funcsByStart = [Int: Function]()
        var order = [Function]()
        for s in starts {
            let (lo, hi) = extent(s)
            let f: Function
            if let g = funcsByStart[lo] { f = g } else { f = Function(lo, hi); funcsByStart[lo] = f; order.append(f) }
            f.roots.insert(s)
        }
        for f in order {
            if boundSet.contains(f.start) { f.roots.insert(f.start) }
            try explore(mo, f)
            f.roots = f.roots.filter { f.insns[$0] != nil }
        }
        let sortedOrder = order.sorted { $0.start < $1.start }

        for f in order {
            for r in f.roots.union(f.branchTargets).union(f.returnSites) where f.insns[r] != nil {
                entriesOf[r] = f.start
            }
        }

        // Leaves: the targets of direct calls, and entry points no other code
        // of their function reaches.
        var leaves = [(Function, Int, [Int])]()  // (function, root, sorted region)
        var blTargets = Set<Int>()
        for f in order { for ins in f.insns.values where ins.kind == .bl { blTargets.insert(ins.target) } }
        for f in order {
            var reached = Set<Int>()
            for ins in f.insns.values {
                if (ins.kind == .b || ins.kind == .bcond) && ins.target != -1 { reached.insert(ins.target) }
                if !((ins.kind == .b || ins.kind == .ijump || ins.kind == .tb || ins.kind == .stop) && ins.always) {
                    reached.insert(ins.addr + ins.size)
                }
            }
            var cand = f.roots.subtracting(reached)
            cand.insert(f.start)
            for a in f.insns.keys where blTargets.contains(a) { cand.insert(a) }
            for e in cand.sorted() {
                if hostAt[e] != nil { continue }
                if let region = leafRegion(f, e) {
                    leafOf[e] = region.count <= TINY_LEAF
                    leaves.append((f, e, region.sorted()))
                }
            }
        }

        try FileManager.default.createDirectory(at: outdir, withIntermediateDirectories: true)
        let header = Buf()
        header.str("// Generated by tools/recomp.py — do not edit.\n")
        header.s("#define VX_OPS_INLINE static inline __attribute__((always_inline))\n")
        header.s("#include \"thumb.h\"\n")
        header.s("#include \"recomp_rt.h\"\n")
        header.s("\n")

        let decls = Buf(capacity: 1 << 20)
        for f in sortedOrder {
            decls.s("int "); nf(decls, f.start); decls.s("(Cpu *c, uint32_t start);\n")
        }
        for (_, e, _) in leaves {
            decls.s("int lf_"); decls.bytes(pfx); decls.s("_"); decls.x(e, width: 6); decls.s("(Core *Kp);\n")
        }
        if !leaves.isEmpty {
            decls.s("static inline LeafFn "); decls.bytes(pfx); decls.s("_leaf(uint32_t t) {\n")
            decls.s("    switch (t) {\n")
            for (_, e, _) in leaves {
                decls.s("    case "); decls.hx(e); decls.s(": return lf_"); decls.bytes(pfx); decls.s("_")
                decls.x(e, width: 6); decls.s(";\n")
            }
            decls.s("    default: return 0;\n")
            decls.s("    }\n")
            decls.s("}\n")
        }
        for (f, e, region) in leaves where region.count <= TINY_LEAF {
            emitLeaf(f, e, region, decls, inline: true)
        }
        // decl_text: the lines joined, without the last one's newline
        var declText = decls.b
        if !declText.isEmpty { declText.removeLast() }
        decls.b = []

        var fileIndex = 0
        func writeFile(_ name: String, _ body: [UInt8]) throws {
            // "\n".join(header + [decl_text, ""] + body)
            var out = Data(capacity: header.b.count + declText.count + body.count + 2)
            out.append(contentsOf: header.b)
            out.append(contentsOf: declText)
            out.append(contentsOf: [10, 10])
            out.append(contentsOf: body)
            try out.write(to: outdir.appendingPathComponent(name))
        }
        func fileName(_ i: Int) -> String {
            let n = String(i)
            return prefix + "_" + String(repeating: "0", count: max(0, 3 - n.count)) + n + ".c"
        }
        // body lines each end in "\n"; the joined file drops the last one
        func flush(_ cur: Buf) throws {
            var body = cur.b
            if !body.isEmpty { body.removeLast() }
            try writeFile(fileName(fileIndex), body)
            fileIndex += 1
        }

        var cur = Buf(capacity: 4 << 20), curN = 0
        var table = [(Int, Int)]()
        for f in sortedOrder {
            let resume = emitFunction(f, cur)
            for r in resume { table.append((r, f.start)) }
            cur.nl()
            curN += f.insns.count
            if curN >= perFile {
                try flush(cur)
                cur = Buf(capacity: 4 << 20); curN = 0
            }
        }
        for (f, e, region) in leaves {
            if region.count <= TINY_LEAF {
                cur.s("int lf_"); cur.bytes(pfx); cur.s("_"); cur.x(e, width: 6)
                cur.s("(Core *Kp) { return lfi_"); cur.bytes(pfx); cur.s("_"); cur.x(e, width: 6); cur.s("(Kp); }\n")
            } else {
                emitLeaf(f, e, region, cur, inline: false)
            }
            cur.nl()
            curN += region.count
            if curN >= perFile {
                try flush(cur)
                cur = Buf(capacity: 4 << 20); curN = 0
            }
        }
        if !cur.b.isEmpty { try flush(cur) }

        // table.sort(): by address, then name (nf_<prefix>_<start:06x>)
        table.sort { $0.0 != $1.0 ? $0.0 < $1.0 : ($0.1 < $1.1) }
        let t = Buf(capacity: table.count * 32)
        t.s("const NativeEntry vx_native_"); t.bytes(pfx); t.s("[] = {\n")
        for (addr, start) in table {
            t.s("    {"); t.hx(addr); t.s(", "); nf(t, start); t.s("},\n")
        }
        t.s("};\n")
        t.s("const size_t vx_native_"); t.bytes(pfx); t.s("_count = "); t.dec(table.count); t.s(";\n")
        try writeFile(prefix + "_table.c", t.b)
    }

    // exit_to(t, indent), without a newline
    func exitTo(_ o: Buf, _ fn: Function, _ t: Int, _ indent: StaticString, _ store: StaticString) {
        o.s(indent)
        if fn.insns[t] != nil {
            o.s("goto L_"); o.x(t); o.s(";")
        } else if let s = entriesOf[t] {
            o.s("{ "); o.s(store); o.s(" c->k.r[15] = "); o.hx(t); o.s("; c->k.thumb = 1; return ")
            nf(o, s); o.s("(c, "); o.hx(t); o.s("); }")
        } else {
            o.s("{ "); o.s(store); o.s(" c->k.r[15] = "); o.hx(t); o.s("; c->k.thumb = 1; return 1; }")
        }
    }

    func hostLoopAt(_ fn: Function, _ a: Int) -> ([String], Int?)? {
        loops: for loop in HOST_LOOPS {
            var end: Int? = nil
            for (rel, pattern) in loop.chunks {
                var at = a + rel
                for (hw1, hw2) in pattern {
                    guard let ins = fn.insns[at], ins.hw1 == hw1, ins.size == (hw2 != nil ? 4 : 2),
                          hw2 == nil || ins.hw2 == hw2! else { continue loops }
                    at += ins.size
                }
                if end == nil { end = at }
            }
            if !loop.exits.allSatisfy({ fn.insns[a + $0] != nil }) { continue }
            if !loop.after { return (loop.code(a), nil) }
            if let e = end, fn.insns[e] != nil { return (loop.code(a), e) }
        }
        return nil
    }

    func execLine(_ o: Buf, _ ins: Insn) {
        if ins.size == 2 {
            o.s("exec16(&K, "); o.hx04(ins.hw1); o.s(", "); o.hx(ins.addr); o.s(", "); o.dec(ins.inIt); o.s(");")
        } else {
            o.s("exec32(&K, "); o.hx04(ins.hw1); o.s(", "); o.hx04(ins.hw2); o.s(", "); o.hx(ins.addr); o.s(");")
        }
    }

    /// emit_function: returns the resume points.
    func emitFunction(_ fn: Function, _ o: Buf) -> [Int] {
        let copyHere = copyFuncs.contains(fn.start)
        let icallCopy = ICALL_COPY_BUILDS.contains(prefix) && !nocopyFuncs.contains(fn.start)
        let labels = fn.insns.keys.sorted()
        let resume = fn.roots.union(fn.returnSites).union(fn.branchTargets).filter { fn.insns[$0] != nil }.sorted()
        let vfp = fn.insns.values.contains { $0.usesVFP }
        let load: StaticString = vfp ? "K = c->k;" : "RT_IN(K, c);"
        let store: StaticString = vfp ? "c->k = K;" : "RT_OUT(c, K);"
        let anyLeaves = !leafOf.isEmpty

        o.s("int "); nf(o, fn.start); o.s("(Cpu *c, uint32_t start) {\n")
        o.s(vfp ? "    Core K = c->k;\n" : "    Core K; RT_IN(K, c);\n")
        o.s("    const uint32_t entry_lr = K.r[14] & ~1u, entry_sp = K.r[13];\n")
        o.s("    int rc;\n")
        o.s("    switch (start) {\n")
        for a in resume { o.s("    case "); o.hx(a); o.s(": goto L_"); o.x(a); o.s(";\n") }
        o.s("    default: c->k.r[15] = start; return 1;\n")
        o.s("    }\n")

        // the tail shared by every call: back at the return address, or dispatch
        func checkReturn(_ ind: StaticString, _ nxt: Int) {
            o.s(ind); o.s("if (K.r[15] != "); o.hx(nxt); o.s(") { K.npc = K.r[15]; goto dyn; }\n")
        }
        func rtCallTail(_ nxt: Int) {  // "{store} c->k.npc = K.r[15]; rc = rt_call(c, ...); {load} if (rc) return 1; }"
            o.s(store); o.s(" c->k.npc = K.r[15]; rc = rt_call(c, "); o.hx(nxt | 1); o.s("); "); o.s(load)
            o.s(" if (rc) return 1; }")
        }

        for (i, a) in labels.enumerated() {
            let ins = fn.insns[a]!
            let nxt = a + ins.size
            o.s("L_"); o.x(a); o.s(":\n")
            if let loop = hostLoopAt(fn, a) {
                for line in loop.0 { o.s("    "); o.str(line); o.nl() }
                if let e = loop.1 {
                    o.s("    goto L_"); o.x(e); o.s(";\n")
                    continue
                }
            }
            if ins.kind != .it {
                if ins.kind == .bl, let lines = interruptAt[ins.target], ins.always {
                    o.s("    if (c->interrupt) { K.r[14] = "); o.hx(nxt | 1); o.s(";\n")
                    for line in lines { o.s("        "); o.str(line); o.nl() }
                    o.s("        goto L_"); o.x(nxt); o.s("; }\n")
                }
                // PC state must be set even when an IT condition skips the instruction
                o.s("    K.r[15] = "); o.hx(a + 4); o.s("; K.npc = "); o.hx(nxt); o.s(";\n")
                let conditional = !ins.always
                let ind: StaticString = conditional ? "        " : "    "
                if conditional { o.s("    if (cond_pass(&K, "); o.dec(ins.cond); o.s(")) {\n") }
                // the body (its "K.r[15] = ..." lines left out: written above)
                switch ins.kind {
                case .bl where hostAt[ins.target] != nil:
                    o.s(ind); o.s("K.r[14] = "); o.hx(nxt | 1); o.s(";\n")
                    o.s(ind); o.str(hostAt[ins.target]!); o.nl()
                case .bl where leafOf[ins.target] != nil:
                    let inline = leafOf[ins.target]!
                    o.s(ind); o.s("K.r[14] = "); o.hx(nxt | 1); o.s(";\n")
                    if copyHere && !inline {
                        o.s(ind); o.s("{ Core T; memcpy(&T, &K, RT_INT_PART); int lr = "); leafName(o, ins.target)
                        o.s("(&T); memcpy(&K, &T, RT_INT_PART);\n")
                        o.s(ind); o.s("  if (lr) { "); rtCallTail(nxt); o.s(" }\n")
                    } else {
                        o.s(ind); o.s("if ("); leafName(o, ins.target); o.s("(&K)) { "); rtCallTail(nxt); o.nl()
                    }
                    checkReturn(ind, nxt)
                case .bl:
                    let t = ins.target
                    o.s(ind); o.s("K.r[14] = "); o.hx(nxt | 1); o.s(";\n")
                    o.s(ind); o.s(store); o.nl()
                    o.s(ind)
                    if let s = entriesOf[t] {
                        o.s("rc = "); nf(o, s); o.s("(c, "); o.hx(t); o.s(");\n")
                    } else {
                        o.s("c->k.npc = "); o.hx(t); o.s("; c->k.thumb = 1; rc = rt_call(c, "); o.hx(nxt | 1); o.s(");\n")
                    }
                    o.s(ind); o.s(load); o.s(" if (rc) return 1;\n")
                    checkReturn(ind, nxt)
                case .blxStub:
                    o.s(ind); o.s("K.r[14] = "); o.hx(nxt | 1); o.s(";\n")
                    if let st = ins.stub, st.allSatisfy({ $0 < 0x80 }),
                       let h = DIV_HELPERS[String(decoding: st, as: Unicode.ASCII.self)] {
                        o.s(ind); o.str(h); o.nl()
                    } else {
                        o.s(ind); o.s(store); o.s(" rc = rt_import(c, "); o.hx(ins.target); o.s(", "); o.hx(nxt | 1)
                        o.s("); "); o.s(load); o.s(" if (rc) return 1;\n")
                        checkReturn(ind, nxt)
                    }
                case .icall:
                    o.s(ind); execLine(o, ins); o.nl()
                    if anyLeaves {
                        // a leaf target runs on these registers; anything else through the dispatcher
                        o.s(ind); o.s("{ LeafFn lf = K.thumb ? "); o.bytes(pfx); o.s("_leaf(K.npc) : 0;\n")
                        if icallCopy || copyHere {
                            o.s(ind); o.s("  if (lf) { K.r[15] = K.npc; Core T; memcpy(&T, &K, RT_INT_PART); int lr = lf(&T); memcpy(&K, &T, RT_INT_PART);\n")
                            o.s(ind); o.s("    if (lr) { "); rtCallTail(nxt); o.s(" }\n")
                        } else {
                            o.s(ind); o.s("  if (lf) { K.r[15] = K.npc; if (lf(&K)) { "); rtCallTail(nxt); o.s(" }\n")
                        }
                        o.s(ind); o.s("  else { "); o.s(store); o.s(" rc = rt_icall(c, "); o.hx(nxt | 1); o.s("); ")
                        o.s(load); o.s(" if (rc) return 1; } }\n")
                    } else {
                        o.s(ind); o.s(store); o.s(" rc = rt_icall(c, "); o.hx(nxt | 1); o.s("); "); o.s(load)
                        o.s(" if (rc) return 1;\n")
                    }
                    checkReturn(ind, nxt)
                default:
                    o.s(ind); execLine(o, ins); o.nl()
                }
                if conditional { o.s("    }\n") }
                // control flow after the instruction
                switch ins.kind {
                case .b, .bcond:
                    if ins.kind == .b && ins.always {
                        exitTo(o, fn, ins.target, "    ", store); o.nl()
                    } else {
                        o.s("    if (K.npc != "); o.hx(nxt); o.s(") "); exitTo(o, fn, ins.target, "", store); o.nl()
                    }
                case .ijump, .tb, .op:
                    if ins.kind != .op && ins.always {
                        // Unconditional indirect jump: always dispatch on the target,
                        // even when it happens to be the next address
                        o.s("    goto dyn;\n")
                    } else {
                        o.s("    if (K.npc != "); o.hx(nxt); o.s(") goto dyn;\n")
                    }
                case .stop:
                    o.s("    "); o.s(store); o.s(" c->k.r[15] = "); o.hx(a); o.s("; return 1;\n")
                default:
                    break
                }
            }
            // fallthrough
            let terminator = (ins.kind == .b || ins.kind == .ijump || ins.kind == .tb || ins.kind == .stop) && ins.always
            if !terminator {
                let following = i + 1 < labels.count ? labels[i + 1] : nil
                if following != nxt { exitTo(o, fn, nxt, "    ", store); o.nl() }
            }
        }
        o.s("dyn: {\n")
        o.s("    uint32_t t = K.npc;\n")
        o.s("    if (K.thumb && t == entry_lr && K.r[13] == entry_sp) { "); o.s(store); o.s(" c->k.r[15] = t; return 0; }\n")
        o.s("    if (K.thumb) switch (t) {\n")
        for a in resume { o.s("    case "); o.hx(a); o.s(": goto L_"); o.x(a); o.s(";\n") }
        o.s("    default: break;\n")
        o.s("    }\n")
        o.s("    "); o.s(store); o.s(" c->k.r[15] = t; return 1;\n")
        o.s("}\n")
        o.s("}\n")
        return resume
    }

    /// emit_leaf: lf_<prefix>_<root>(Core *), or lfi_ (always inlined).
    func emitLeaf(_ fn: Function, _ root: Int, _ labels: [Int], _ o: Buf, inline: Bool) {
        if inline {
            o.s("static inline __attribute__((always_inline)) int lfi_")
        } else {
            o.s("int lf_")
        }
        o.bytes(pfx); o.s("_"); o.x(root, width: 6); o.s("(Core *Kp) {\n")
        o.s("#define K (*Kp)\n")
        o.s("    const uint32_t entry_lr = K.r[14] & ~1u, entry_sp = K.r[13];\n")
        o.s("    goto L_"); o.x(root); o.s(";\n")
        for (i, a) in labels.enumerated() {
            let ins = fn.insns[a]!
            let nxt = a + ins.size
            o.s("L_"); o.x(a); o.s(":\n")
            if ins.kind != .it {
                o.s("    K.r[15] = "); o.hx(a + 4); o.s("; K.npc = "); o.hx(nxt); o.s(";\n")
                if !ins.always {
                    o.s("    if (cond_pass(&K, "); o.dec(ins.cond); o.s(")) { "); execLine(o, ins); o.s(" }\n")
                } else {
                    o.s("    "); execLine(o, ins); o.nl()
                }
                if ins.kind == .b || ins.kind == .bcond {
                    o.s("    ")
                    if !(ins.kind == .b && ins.always) { o.s("if (K.npc != "); o.hx(nxt); o.s(") ") }
                    o.s("goto L_"); o.x(ins.target); o.s(";\n")
                } else if ins.kind == .ijump && ins.always {
                    o.s("    goto dyn;\n")
                } else {
                    o.s("    if (K.npc != "); o.hx(nxt); o.s(") goto dyn;\n")
                }
            }
            let terminator = (ins.kind == .b || ins.kind == .ijump) && ins.always
            if !terminator {
                let following = i + 1 < labels.count ? labels[i + 1] : nil
                if following != nxt { o.s("    goto L_"); o.x(nxt); o.s(";\n") }
            }
        }
        o.s("dyn:\n")
        o.s("    K.r[15] = K.npc;\n")
        o.s("    return !(K.thumb && K.npc == entry_lr && K.r[13] == entry_sp);\n")
        o.s("#undef K\n")
        o.s("}\n")
    }
}
