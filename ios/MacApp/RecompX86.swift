// Static recompiler: hot i386 functions of a MacinTalk generation -> C.
//
// A Swift port of tools/recomp_x86.py, so the Mac app can make its own fast
// MacinTalk code on the user's Mac. It writes exactly the same files, byte for
// byte, as `recomp_x86.py <dump> <profile> <outdir> --prefix <prefix>`; see
// that script for what the generated code does. Change the two together.
//
// Input is the engine as MacinTalk's loader left it in guest memory (MT_DUMP,
// see core/mt/mt_native.c) plus a profile (tools/profiles/mtNNN.txt). The
// output embeds code from the dumped images, so it is only ever made from the
// user's own copy of each engine, never shipped.
import Foundation

enum X86Recompiler {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    /// Same as `recomp_x86.py <dump> <profile> <outdir> --prefix <prefix>` with default --per-file.
    static func run(dump: URL, profile: URL, outdir: URL, prefix: String) throws {
        _ = try recompile(dump: dump, profile: profile, outdir: outdir, prefix: prefix)
    }

    /// The whole thing, with the script's other options. Returns the line the
    /// script prints ("N functions (...% of the profile), ...").
    @discardableResult
    static func recompile(dump dumpDir: URL, profile: URL, outdir: URL, prefix: String, perFile: Int = 600,
                          coverage: Double = 1.0, debugCalls: Bool = false) throws -> String {
        guard perFile > 0 else { throw Failure(description: "--per-file must be positive") }
        let dump = try Dump(dumpDir)
        // where control landed, in the order the profile first lists them
        var startOrder: [Int] = []
        var starts: [Int: Int] = [:]
        for raw in try pyLines(profile) {
            let ln = pySplit(String(raw.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first ?? ""))
            if ln.count != 2 && ln.count != 3 { continue }
            guard let im = dump.image(ln[0]) else { continue }
            let addr = try pyInt(ln[1], 16) + im.slide
            if dump.textOf(addr) != nil {
                let n = ln.count == 3 ? try pyInt(ln[2], 10) : 1
                if let old = starts[addr] {
                    starts[addr] = old + n
                } else {
                    starts[addr] = n
                    startOrder.append(addr)
                }
            }
        }

        // function boundaries: symbols in each __text section
        var bounds = Set<Int>()
        var names: [Int: String] = [:]
        for s in dump.texts { bounds.insert(s.addr) }
        for (_, addr, nm) in dump.syms where dump.textOf(addr) != nil {
            bounds.insert(addr)
            if names[addr] == nil { names[addr] = nm }
        }
        let blist = bounds.sorted()

        func extent(_ addr: Int) -> (Int, Int) {
            // bisect_right(blist, addr) - 1
            var lo = 0, hi = blist.count
            while lo < hi {
                let mid = (lo + hi) / 2
                if addr < blist[mid] { hi = mid } else { lo = mid + 1 }
            }
            let i = lo - 1
            let t = dump.textOf(addr)!
            let end = t.addr + t.size
            let h = i + 1 < blist.count ? blist[i + 1] : end
            return (blist[i], min(h, end))
        }

        var funcOrder: [Int] = []
        var funcs: [Int: Function] = [:]
        var weight: [Int: Int] = [:]
        for s in startOrder {
            let n = starts[s]!
            let (lo, hi) = extent(s)
            let f: Function
            if let g = funcs[lo] {
                f = g
            } else {
                f = Function(start: lo, end: hi, name: names[lo] ?? "?")
                funcs[lo] = f
                funcOrder.append(lo)
            }
            f.roots.insert(s)
            weight[lo, default: 0] += n
        }
        let totalW = weight.values.reduce(0, +)
        var kept: [Function] = []
        var acc = 0
        // sorted(funcs, key=-weight): stable
        let byWeight = funcOrder.enumerated().sorted { x, y in
            let wx = -weight[x.element]!, wy = -weight[y.element]!
            return wx != wy ? wx < wy : x.offset < y.offset
        }
        for (_, lo) in byWeight {
            if Double(acc) >= coverage * Double(totalW) { break }
            kept.append(funcs[lo]!)
            acc += weight[lo]!
        }
        for f in kept {
            if starts[f.start] != nil { f.roots.insert(f.start) }
            try explore(dump, f, starts)
            f.roots.formIntersection(f.insns.keys)
        }

        var entriesOf: [Int: String] = [:]
        for f in kept {
            for r in f.roots.union(f.branchTargets).union(f.returnSites) where f.insns[r] != nil {
                entriesOf[r] = "nx_\(prefix)_\(hex08(f.start))"
            }
        }

        try FileManager.default.createDirectory(at: outdir, withIntermediateDirectories: true)
        let order = kept.sorted { $0.start < $1.start }
        let header = [
            "// Generated by tools/recomp_x86.py from your own copy of MacinTalk. Do not edit or share.",
            "#define X86_OPS_INLINE static inline __attribute__((always_inline))",
            "#include \"x86/x86_ops.h\"",
            "#include \"mt/mt_native.h\"",
            "extern const int x86_rt_never;",
            "",
        ]
        let decls = order.map { "int nx_\(prefix)_\(hex08($0.start))(X86 *c, uint32_t start);" }.joined(separator: "\n")
        var files: [[String]] = []
        var cur: [String] = []
        var curN = 0
        var table: [(Int, String)] = []
        var total = 0
        for f in order {
            let (name, resume, units) = try emitFunction(dump, f, entriesOf, prefix, debugCalls, perFile)
            table += resume.map { ($0, name) }
            total += f.insns.count
            for (body, n) in units {
                if !cur.isEmpty && curN + n > perFile * 3 / 2 {
                    files.append(cur)
                    cur = []
                    curN = 0
                }
                cur += body
                cur.append("")
                curN += n
                if curN >= perFile {
                    files.append(cur)
                    cur = []
                    curN = 0
                }
            }
        }
        if !cur.isEmpty { files.append(cur) }
        for (i, body) in files.enumerated() {
            let text = (header + [decls, ""] + body).joined(separator: "\n")
            try write(text, outdir.appendingPathComponent("\(prefix)_\(pad(i, 3)).c"))
        }

        // the code ranges this was made from: the __text of every image it uses,
        // and the import stubs and pointers (stubTarget) as they were bound
        let used = Set(order.map { dump.textOf($0.start)!.image })
        var ranges: [(String, Int, Int, UInt64)] = []
        let stubKinds: Set<Int> = [S_SYMBOL_STUBS, S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS]
        for s in dump.texts + dump.sects.filter({ stubKinds.contains($0.flags & 0xFF) }) where used.contains(s.image) {
            let bytes = try dump.read(s.addr, s.size)
            var h: UInt64 = 0xcbf2_9ce4_8422_2325
            for byte in bytes { h = (h ^ UInt64(byte)) &* 0x100_0000_01b3 }
            ranges.append((s.image, s.addr, s.addr + s.size, h))
        }
        table.sort { $0.0 != $1.0 ? $0.0 < $1.0 : pyLess($0.1, $1.1) }
        var t = header + [decls, "", "static const X86Native table[] = {"]
        t += table.map { "    {\(hx($0.0)), \($0.1)}," }
        t += ["};", "static const MtNativeRange ranges[] = {"]
        t += ranges.map { "    {\"\($0.0)\", \(hx($0.1)), \(hx($0.2)), \(hx($0.3))ull}," }
        t += ["};",
              "static const MtNativeSet set = {\"\(prefix)\", ranges, \(ranges.count), table, \(table.count)};",
              "__attribute__((constructor)) static void register_\(prefix)(void) { mt_native_register(&set); }",
              ""]
        try write(t.joined(separator: "\n"), outdir.appendingPathComponent("\(prefix)_table.c"))
        let pct = String(format: "%.2f", Double(100 * acc) / Double(max(totalW, 1)))
        return "\(kept.count) functions (\(pct)% of the profile), \(total) instructions, "
            + "\(table.count) entry points, \(files.count) files"
    }

    // ------------------------------------------------------------ Python semantics

    // The script's exceptions: explore() catches ValueError and IndexError
    // from decoding; anything else (struct.error, say) stops it.
    private enum PyError: Error, CustomStringConvertible {
        case value(String), index, structure
        var description: String {
            switch self {
            case .value(let s): return "ValueError: \(s)"
            case .index: return "IndexError"
            case .structure: return "struct.error"
            }
        }
    }

    private static func write(_ s: String, _ url: URL) throws {
        try Data(s.utf8).write(to: url)
    }

    // Lines as Python's text mode reads them (universal newlines).
    private static func pyLines(_ url: URL) throws -> [Substring] {
        let s = String(decoding: try Data(contentsOf: url), as: UTF8.self)
        return s.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
            .split(separator: "\n", omittingEmptySubsequences: false)
    }

    // str.split() with no arguments
    private static func pySplit(_ s: String) -> [String] {
        var out: [String] = []
        var word = String.UnicodeScalarView()
        for u in s.unicodeScalars {
            if u.properties.isWhitespace || (0x1C...0x1F).contains(u.value) {
                if !word.isEmpty { out.append(String(word)); word = String.UnicodeScalarView() }
            } else {
                word.append(u)
            }
        }
        if !word.isEmpty { out.append(String(word)) }
        return out
    }

    // int(s, base) for base 10 and 16
    private static func pyInt(_ s: String, _ base: Int) throws -> Int {
        var u = Array(s.unicodeScalars)
        var neg = false
        if let c = u.first, c == "+" || c == "-" { neg = c == "-"; u.removeFirst() }
        var prefixed = false
        if base == 16, u.count >= 2, u[0] == "0", u[1] == "x" || u[1] == "X" { u.removeFirst(2); prefixed = true }
        var v = 0, digits = 0
        var lastUnderscore = !prefixed  // "_" may follow the 0x, not start the number
        for c in u {
            if c == "_" {
                if lastUnderscore { throw PyError.value("invalid literal for int(): \(s)") }
                lastUnderscore = true
                continue
            }
            guard let d = Int(String(c), radix: base) else { throw PyError.value("invalid literal for int(): \(s)") }
            let (m, o1) = v.multipliedReportingOverflow(by: base)
            let (a, o2) = m.addingReportingOverflow(d)
            if o1 || o2 { throw PyError.value("number too large: \(s)") }
            v = a
            digits += 1
            lastUnderscore = false
        }
        if digits == 0 || lastUnderscore { throw PyError.value("invalid literal for int(): \(s)") }
        return neg ? -v : v
    }

    // Python's str ordering: by code point
    private static func pyLess(_ a: String, _ b: String) -> Bool {
        a.unicodeScalars.lexicographicallyPrecedes(b.unicodeScalars) { $0.value < $1.value }
    }

    // f"{v:#x}"
    private static func hx(_ v: Int) -> String { v < 0 ? "-0x" + String(-v, radix: 16) : "0x" + String(v, radix: 16) }
    private static func hx(_ v: UInt64) -> String { "0x" + String(v, radix: 16) }
    // f"{v:x}"
    private static func hexs(_ v: Int) -> String { v < 0 ? "-" + String(-v, radix: 16) : String(v, radix: 16) }
    // f"{v:08x}"
    private static func hex08(_ v: Int) -> String {
        let s = String(abs(v), radix: 16)
        let body = s.count < (v < 0 ? 7 : 8) ? String(repeating: "0", count: (v < 0 ? 7 : 8) - s.count) + s : s
        return v < 0 ? "-" + body : body
    }
    // f"{v:03d}"
    private static func pad(_ v: Int, _ n: Int) -> String {
        let s = String(v)
        return s.count < n ? String(repeating: "0", count: n - s.count) + s : s
    }

    // ------------------------------------------------------------ the dump

    private struct Image { let name: String; let lo: Int; let hi: Int; let slide: Int; let data: [UInt8] }
    private struct Sect { let image: String; let seg: String; let sect: String; let addr: Int; let size: Int; let flags: Int }

    private final class Dump {
        var images: [Image] = []
        var sects: [Sect] = []
        var syms: [(String, Int, String)] = []
        var trap = (0, 0)
        var texts: [Sect] = []

        init(_ d: URL) throws {
            for ln in try X86Recompiler.pyLines(d.appendingPathComponent("images.txt")) {
                let f = X86Recompiler.pySplit(String(ln))
                guard let kind = f.first else { continue }
                switch kind {
                case "trap" where f.count >= 3:
                    trap = (try pyInt(f[1], 16), try pyInt(f[2], 16))
                case "image" where f.count >= 5:
                    let data = try Data(contentsOf: d.appendingPathComponent(f[1] + ".mem"))
                    images.append(Image(name: f[1], lo: try pyInt(f[2], 16), hi: try pyInt(f[3], 16),
                                        slide: try pyInt(f[4], 16), data: [UInt8](data)))
                case "sect" where f.count >= 7:
                    sects.append(Sect(image: f[1], seg: f[2], sect: f[3], addr: try pyInt(f[4], 16),
                                      size: try pyInt(f[5], 16), flags: try pyInt(f[6], 16)))
                case "sym" where f.count >= 3:
                    syms.append((f[1], try pyInt(f[2], 16), f[3...].joined(separator: " ")))
                case "trap", "image", "sect", "sym":
                    throw Failure(description: "images.txt: short line: \(ln)")
                default:
                    break
                }
            }
            texts = sects.filter { $0.sect == "__text" && $0.seg == "__TEXT" }
        }

        func image(_ name: String) -> Image? { images.first { $0.name == name } }

        func read(_ addr: Int, _ n: Int) throws -> [UInt8] {
            for im in images where im.lo <= addr && addr + n <= im.hi {
                let o = addr - im.lo
                let a = min(o, im.data.count), b = min(o + n, im.data.count)
                return Array(im.data[a..<max(a, b)])
            }
            throw PyError.value(hx(addr))
        }

        func textOf(_ addr: Int) -> Sect? { texts.first { $0.addr <= addr && addr < $0.addr + $0.size } }
    }

    // ------------------------------------------------------------ decoding

    private static func makeSet(_ items: [Any]) -> Set<Int> {
        var s = Set<Int>()
        for r in items {
            if let v = r as? Int { s.insert(v) } else if let p = r as? (Int, Int) { s.formUnion(p.0...p.1) }
        }
        return s
    }

    private static let PREFIXES: Set<Int> = [0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x66]
    // one-byte opcodes with a ModRM byte
    private static let MODRM1 = makeSet([(0x00, 0x03), (0x08, 0x0B), (0x10, 0x13), (0x18, 0x1B), (0x20, 0x23),
                                         (0x28, 0x2B), (0x30, 0x33), (0x38, 0x3B), 0x62, 0x63, 0x69, 0x6B,
                                         (0x80, 0x8F), (0xC0, 0xC1), (0xC4, 0xC7), (0xD0, 0xD3), (0xD8, 0xDF),
                                         (0xF6, 0xF7), (0xFE, 0xFF)])
    private static let IMM8_1 = makeSet([0x04, 0x0C, 0x14, 0x1C, 0x24, 0x2C, 0x34, 0x3C, 0x6A, 0x6B, (0x70, 0x7F),
                                         0x80, 0x82, 0x83, 0xA8, (0xB0, 0xB7), 0xC0, 0xC1, 0xC6, 0xCD, 0xD4, 0xD5,
                                         (0xE0, 0xE7), 0xEB])
    private static let IMMZ_1 = makeSet([0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D, 0x68, 0x69, 0x81, 0xA9,
                                         (0xB8, 0xBF), 0xC7, 0xE8, 0xE9])
    private static let NO_MODRM2 = makeSet([(0x04, 0x0C), 0x0E, (0x30, 0x37), 0x77, (0x80, 0x8F), 0xA0, 0xA1, 0xA2,
                                            0xA8, 0xA9, 0xAA, (0xC8, 0xCF)])
    private static let IMM8_2 = makeSet([0x0F, (0x70, 0x73), 0xA4, 0xAC, 0xBA, 0xC2, 0xC4, 0xC5, 0xC6])

    private static func isSSE(_ op2: Int) -> Bool {  // x86_is_sse in core/x86/x86_ops.h
        (0x10...0x17).contains(op2) || (0x28...0x2F).contains(op2) || (0x50...0x7F).contains(op2)
            || op2 == 0xC2 || op2 == 0xC6 || op2 >= 0xD0
    }

    /// Which x86_exec_* entry executes this instruction.
    private static func group(_ raw: [UInt8]) throws -> String {
        var i = 0
        while PREFIXES.contains(try at(raw, i)) { i += 1 }
        if raw[i] == 0x0F {
            let op2 = try at(raw, i + 1)
            return isSSE(op2) ? "sse" : "0f" + String(op2 >> 4, radix: 16)
        }
        return (0xD8...0xDF).contains(Int(raw[i])) ? "x87" : "m" + String(raw[i] >> 4, radix: 16)
    }

    private enum Kind { case op, jcc, jmp, call, icall, ret, ijmp, stop }

    private struct Insn {
        let addr: Int
        let raw: [UInt8]
        let kind: Kind
        let target: Int?
        var size: Int { raw.count }
    }

    // Each guest instruction becomes a small static function, which clang
    // simplifies on its own (its bytes are constants) before inlining it.
    private static func insnFn(_ defs: inout [String], _ name: String, _ raw: [UInt8]) throws -> String {
        defs.append("static void \(name)(X86Core *k) { x86_exec_\(try group(raw))(k, \(cbytes(raw))); }")
        return "\(name)(&K);"
    }

    private static func s32(_ v: Int) -> Int { v & 0x8000_0000 != 0 ? v - (1 << 32) : v }

    private static func at(_ b: [UInt8], _ i: Int) throws -> Int {
        guard i >= 0 && i < b.count else { throw PyError.index }
        return Int(b[i])
    }

    // struct.unpack_from("<I", b, o)
    private static func u32(_ b: [UInt8], _ o: Int) throws -> Int {
        guard o >= 0 && o + 4 <= b.count else { throw PyError.structure }
        return Int(b[o]) | Int(b[o + 1]) << 8 | Int(b[o + 2]) << 16 | Int(b[o + 3]) << 24
    }

    private static func decode(_ dump: Dump, _ addr: Int) throws -> Insn {
        var b: [UInt8]
        do {
            b = try dump.read(addr, 16)
        } catch PyError.value {
            b = try dump.read(addr, 1) + [UInt8](repeating: 0, count: 15)  // the last few bytes of an image
        }
        var i = 0, opsz = 32
        while PREFIXES.contains(try at(b, i)) {
            if b[i] == 0x66 { opsz = 16 }
            i += 1
            if i >= 15 { throw PyError.value("prefixes") }
        }
        let op = try at(b, i); i += 1
        if op == 0x67 { throw PyError.value("address-size prefix") }
        let immz = opsz == 16 ? 2 : 4

        func modrmLen(_ j: Int) throws -> (Int, Int) {
            let m = try at(b, j)
            let mod = m >> 6, rm = m & 7
            var n = 1
            if mod == 3 { return (n, m) }
            if rm == 4 {
                let sib = try at(b, j + 1)
                n += 1
                if sib & 7 == 5 && mod == 0 { n += 4 }
            } else if rm == 5 && mod == 0 {
                n += 4
            }
            n += mod == 1 ? 1 : mod == 2 ? 4 : 0
            return (n, m)
        }

        var kind = Kind.op
        var target: Int? = nil
        if op == 0x0F {
            let op2 = try at(b, i); i += 1
            if op2 == 0x38 || op2 == 0x3A {
                i += 1
                i += try modrmLen(i).0
                if op2 == 0x3A { i += 1 }
            } else {
                if !NO_MODRM2.contains(op2) { i += try modrmLen(i).0 }
                if IMM8_2.contains(op2) { i += 1 }
                if (0x80...0x8F).contains(op2) {
                    let rel = s32(try u32(b, i)); i += 4
                    kind = .jcc; target = (addr + i + rel) & 0xFFFF_FFFF
                } else if op2 == 0x0B {
                    kind = .stop
                }
            }
        } else {
            var m: Int? = nil
            if MODRM1.contains(op) { let r = try modrmLen(i); i += r.0; m = r.1 }
            if IMM8_1.contains(op) { i += 1 }
            if IMMZ_1.contains(op) { i += immz }
            if op == 0xF6 && (m! >> 3) & 7 < 2 { i += 1 }
            if op == 0xF7 && (m! >> 3) & 7 < 2 { i += immz }
            if (0xA0...0xA3).contains(op) { i += 4 }
            if op == 0xC2 || op == 0xCA { i += 2 }
            if op == 0xC8 { i += 3 }
            if op == 0x9A || op == 0xEA {
                i += 2 + immz
                kind = .stop
            }
            let reg = m.map { ($0 >> 3) & 7 }
            if (0x70...0x7F).contains(op) || (0xE0...0xE3).contains(op) {
                kind = .jcc; target = (addr + i + (try at(b, i - 1) ^ 0x80) - 0x80) & 0xFFFF_FFFF
            } else if op == 0xEB {
                kind = .jmp; target = (addr + i + (try at(b, i - 1) ^ 0x80) - 0x80) & 0xFFFF_FFFF
            } else if op == 0xE8 || op == 0xE9 {
                let rel = s32(try u32(b, i - 4))
                kind = op == 0xE8 ? .call : .jmp; target = (addr + i + rel) & 0xFFFF_FFFF
            } else if op == 0xC2 || op == 0xC3 {
                kind = .ret
            } else if op == 0xFF && reg == 2 {
                kind = .icall
            } else if op == 0xFF && reg == 4 {
                kind = .ijmp
            } else if [0xCC, 0xCD, 0xCE, 0xF4, 0xCA, 0xCB, 0xCF].contains(op)
                        || (op == 0xFF && [3, 5, 7].contains(reg!)) {
                kind = .stop
            }
        }
        if i > 15 { throw PyError.value("too long") }
        return Insn(addr: addr, raw: Array(b[0..<min(i, b.count)]), kind: kind, target: target)
    }

    // `call` to code that only copies its return address into a register:
    // mov reg, [esp] ; ret (PIC base). Executed inline.
    private static func pcThunk(_ dump: Dump, _ t: Int) throws -> ([UInt8], [UInt8])? {
        let b: [UInt8]
        do { b = try dump.read(t, 4) } catch PyError.value { return nil }
        if try at(b, 0) == 0x8B && at(b, 1) & 0xC7 == 0x04 && at(b, 2) == 0x24 && at(b, 3) == 0xC3 {
            return (Array(b[0..<3]), Array(b[3..<4]))
        }
        return nil
    }

    // An import stub as the loader bound it: where a call to it really goes, a
    // host function in the trap region or another image's code. nil if t isn't one.
    private static let S_SYMBOL_STUBS = 8, S_NON_LAZY_SYMBOL_POINTERS = 6, S_LAZY_SYMBOL_POINTERS = 7

    private static func stubTarget(_ dump: Dump, _ t: Int, _ depth: Int = 0) throws -> Int? {
        if depth > 3 || !dump.sects.contains(where: { $0.flags & 0xFF == S_SYMBOL_STUBS && $0.addr <= t && t < $0.addr + $0.size }) {
            return nil
        }
        let b: [UInt8]
        do { b = try dump.read(t, 6) } catch PyError.value { return nil }
        let d: Int
        if try at(b, 0) == 0xE9 {
            d = (t + 5 + s32(try u32(b, 1))) & 0xFFFF_FFFF
        } else if try at(b, 0) == 0xFF && at(b, 1) == 0x25 {
            let p: [UInt8]
            do { p = try dump.read(try u32(b, 2), 4) } catch PyError.value { return nil }
            guard p.count == 4 else { throw PyError.structure }
            d = try u32(p, 0)
        } else {
            return nil
        }
        if (dump.trap.0 <= d && d < dump.trap.1) || dump.textOf(d) != nil { return d }
        return try stubTarget(dump, d, depth + 1)
    }

    // ------------------------------------------------------------ functions

    private final class Function {
        let start: Int, end: Int, name: String
        var insns: [Int: Insn] = [:]
        var roots = Set<Int>()
        var branchTargets = Set<Int>()
        var returnSites = Set<Int>()
        init(start: Int, end: Int, name: String) { self.start = start; self.end = end; self.name = name }
    }

    /// Decode what can run from the roots. A branch target the profile never
    /// landed on was never taken: it is left out, and reaching it hands over to
    /// the interpreter.
    private static func explore(_ dump: Dump, _ fn: Function, _ landed: [Int: Int]) throws {
        var work = fn.roots.sorted()
        while let a = work.popLast() {
            if fn.insns[a] != nil || !(fn.start <= a && a < fn.end) { continue }
            let ins: Insn
            do {
                ins = try decode(dump, a)
            } catch PyError.value {
                fn.insns[a] = Insn(addr: a, raw: [], kind: .stop, target: nil); continue
            } catch PyError.index {
                fn.insns[a] = Insn(addr: a, raw: [], kind: .stop, target: nil); continue
            }
            fn.insns[a] = ins
            let nxt = a + ins.size
            if ins.kind == .jcc || ins.kind == .jmp, let t = ins.target, landed[t] != nil {
                fn.branchTargets.insert(t)
                if fn.start <= t && t < fn.end { work.append(t) }
            }
            if (ins.kind == .call || ins.kind == .icall) && !(ins.kind == .call && ins.target == nxt) {
                fn.returnSites.insert(nxt)
            }
            if ![.jmp, .ret, .ijmp, .stop].contains(ins.kind) { work.append(nxt) }
        }
    }

    private static func cbytes(_ raw: [UInt8]) -> String {
        var p = raw + [UInt8](repeating: 0, count: max(0, 16 - raw.count))
        p = Array(p.prefix(16))
        var lo: UInt64 = 0, hi: UInt64 = 0
        for k in 0..<8 {
            lo |= UInt64(p[k]) << (8 * UInt64(k))
            hi |= UInt64(p[8 + k]) << (8 * UInt64(k))
        }
        return "\(hx(lo))ull, \(hx(hi))ull"
    }

    /// One C function per guest function; a guest function longer than
    /// partSize instructions becomes several C functions (parts) that hand over
    /// to each other by tail calls. Returns the name, its entry points and the
    /// units to write: (lines, instructions).
    private static func emitFunction(_ dump: Dump, _ fn: Function, _ entriesOf: [Int: String], _ prefix: String,
                                     _ debugCalls: Bool, _ partSize: Int) throws -> (String, [Int], [([String], Int)]) {
        let name = "nx_\(prefix)_\(hex08(fn.start))"
        let labels = fn.insns.keys.sorted()
        let labelset = Set(labels)
        let resume = fn.roots.union(fn.returnSites).union(fn.branchTargets).intersection(labelset).sorted()
        let parts = stride(from: 0, to: labels.count, by: partSize).map { Array(labels[$0..<min($0 + partSize, labels.count)]) }
        guard !parts.isEmpty else { throw Failure(description: "\(name): no instructions") }
        let split = parts.count > 1
        var partOf: [Int: Int] = [:]
        for (p, ls) in parts.enumerated() { for a in ls { partOf[a] = p } }
        func pname(_ p: Int) -> String { split ? "\(name)_\(p)" : name }
        func args(_ s: String) -> String { "c, \(s), entry_sp, entry_ret" }
        var outFns: [(Int, Set<Int>, [String], [String])] = []
        var partsEntered = [Set<Int>](repeating: [], count: parts.count)  // entered from another part

        func leave(_ t: Int, _ indent: String) -> String {
            // Control goes to t, outside this function's code.
            if let e = entriesOf[t], !debugCalls { return "\(indent)c->k = K; return \(e)(c, \(hx(t)));" }
            return "\(indent)c->k = K; return 1;"
        }

        for (p, plabels) in parts.enumerated() {
            let here = Set(plabels)
            let entered = Set(resume.filter { here.contains($0) })  // where the part can be started
            var body: [String] = []
            var defs: [String] = []

            func goto(_ t: Int, _ src: Int, _ indent: String) -> String {
                var s = ""
                if t <= src {
                    // A loop: the barrier makes each pass read guest memory again,
                    // and an engine shutdown gets through.
                    s = "\(indent)__asm__ volatile(\"\" ::: \"memory\");\n"
                        + "\(indent)if (__builtin_expect(*abortp, 0)) { c->k = K; return 1; }\n"
                }
                if here.contains(t) { return s + "\(indent)goto L_\(hexs(t));" }
                if labelset.contains(t) {
                    partsEntered[partOf[t]!].insert(t)
                    return s + "\(indent)c->k = K; __attribute__((musttail)) return \(pname(partOf[t]!))(\(args(hx(t))));"
                }
                return s + leave(t, indent)
            }

            for (i, a) in plabels.enumerated() {
                let ins = fn.insns[a]!
                let nxt = a + ins.size
                body.append("L_\(hexs(a)):")
                if ins.kind == .stop {
                    body.append("    K.eip = \(hx(a)); c->k = K; return 1;")
                    continue
                }
                body.append("    K.eip = \(hx(a)); \(try insnFn(&defs, "i_\(hexs(a))", ins.raw))")
                if ins.kind == .jcc {
                    body.append("    if (K.eip != \(hx(nxt))) {")
                    body.append(goto(ins.target!, a, "        "))
                    body.append("    }")
                } else if ins.kind == .jmp {
                    body.append(goto(ins.target!, a, "    "))
                } else if ins.kind == .call && ins.target == nxt {
                    // call next / pop: the PIC base
                } else if ins.kind == .call, let th = try pcThunk(dump, ins.target!) {
                    let first = try insnFn(&defs, "t_\(hexs(a))_0", th.0)
                    let second = try insnFn(&defs, "t_\(hexs(a))_1", th.1)
                    body.append("    \(first) K.eip = \(hx(ins.target! + 3)); \(second)")
                } else if ins.kind == .call, let d = try stubTarget(dump, ins.target!), !debugCalls {
                    // through an import stub: straight to where it goes
                    body.append("    K.eip = \(hx(d)); c->k = K;")
                    if dump.trap.0 <= d && d < dump.trap.1 {
                        body.append("    x86_rt_trap(c);")
                        body.append("    K = c->k;")
                    } else {
                        if let e = entriesOf[d] {
                            body.append("    rc = \(e)(c, \(hx(d)));")
                        } else {
                            body.append("    rc = x86_rt_call(c, \(hx(nxt)));")
                        }
                        body.append("    K = c->k; if (rc) return 1;")
                    }
                    body.append("    if (K.eip != \(hx(nxt))) goto dyn;")
                } else if ins.kind == .call || ins.kind == .icall {
                    body.append("    c->k = K;")
                    if ins.kind == .call, let t = ins.target, let e = entriesOf[t], !debugCalls {
                        body.append("    rc = \(e)(c, \(hx(t)));")
                    } else {
                        body.append("    rc = x86_rt_call(c, \(hx(nxt)));")
                    }
                    body.append("    K = c->k; if (rc) return 1;")
                    body.append("    if (K.eip != \(hx(nxt))) goto dyn;")
                } else if ins.kind == .ret || ins.kind == .ijmp {
                    body.append("    goto dyn;")
                }
                if ins.kind != .jmp && ins.kind != .ret && ins.kind != .ijmp {
                    let following = i + 1 < plabels.count ? plabels[i + 1] : nil
                    if following != nxt {
                        body.append(labelset.contains(nxt) ? goto(nxt, a, "    ") : leave(nxt, "    "))
                    }
                }
            }
            body.append("dyn:")
            body.append("    if (K.eip == entry_ret && K.r[ESP] - (entry_sp + 4) <= X86_CALLEE_POP) { c->k = K; return 0; }")
            body.append("    switch (K.eip) {")
            for a in entered.sorted() { body.append("    case \(hx(a)): goto L_\(hexs(a));") }
            if split {
                body.append("    default: break;")
                body.append("    }")
                body.append("    c->k = K; __attribute__((musttail)) return \(name)_go(\(args("K.eip")));")
            } else {
                body.append("    default: c->k = K; return 1;")
                body.append("    }")
            }
            body.append("}")
            outFns.append((p, entered, defs, body))
        }

        var fns: [[String]] = []
        for (p, entered, defs, body) in outFns {
            var head: [String] = []
            if split {
                head.append("int \(pname(p))(X86 *c, uint32_t start, uint32_t entry_sp, uint32_t entry_ret) {")
            } else {
                head.append("// \(fn.name)")
                head.append("int \(name)(X86 *c, uint32_t start) {")
                head.append("    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) < c->stack_limit, 0)) return 1;")
            }
            head.append("    X86Core K = c->k;")
            head.append("    const volatile int *const abortp = c->abort ? c->abort : &x86_rt_never;")
            if !split { head.append("    const uint32_t entry_sp = K.r[ESP], entry_ret = k_rd32(&K, entry_sp);") }
            head.append("    int rc;")
            head.append("    (void)rc;")
            head.append("    switch (start) {")
            for a in entered.union(partsEntered[p]).sorted() { head.append("    case \(hx(a)): goto L_\(hexs(a));") }
            head.append("    default: return 1;")
            head.append("    }")
            fns.append(defs + head + body)
        }
        if !split { return (name, resume, [(fns[0], labels.count)]) }
        // parts call each other and the router below, from any file
        let sig = "(X86 *c, uint32_t start, uint32_t entry_sp, uint32_t entry_ret)"
        let decl = (0..<parts.count).map { "int \(pname($0))\(sig);" } + ["int \(name)_go\(sig);"]
        var go = ["int \(name)_go\(sig) {"]
        for p in 0..<(parts.count - 1) {
            go.append("    if (start < \(hx(parts[p + 1][0]))) { __attribute__((musttail)) return \(pname(p))(c, start, entry_sp, entry_ret); }")
        }
        go.append("    __attribute__((musttail)) return \(pname(parts.count - 1))(c, start, entry_sp, entry_ret);")
        go.append("}")
        let entry = ["// \(fn.name)",
                     "int \(name)(X86 *c, uint32_t start) {",
                     "    if (__builtin_expect((uintptr_t)__builtin_frame_address(0) < c->stack_limit, 0)) return 1;",
                     "    const uint32_t entry_sp = c->k.r[ESP];",
                     "    return \(name)_go(c, start, entry_sp, k_rd32(&c->k, entry_sp));",
                     "}"]
        var units: [([String], Int)] = [(decl + go + entry, 0)]
        for (f, ls) in zip(fns, parts) { units.append((decl + f, ls.count)) }
        return (name, resume, units)
    }
}
