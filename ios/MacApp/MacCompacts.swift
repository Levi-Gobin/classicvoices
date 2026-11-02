import Foundation

/// The compact voices Lion, Mountain Lion and Mavericks installed with the
/// system (Agata, Daniel, Samantha ...), as the build gets them for the apps
/// that carry them (tools/mac_compacts.py). Apple's voice packages have only
/// the premium voices, so the compacts are read out of each release's
/// installer: MacCompacts.json says which 25 to 40 MB of it hold them and how
/// to unpack that, with a SHA-256 per voice. The steps are the NVDA add-on's
/// (nvda/.../maccompacts.py), which explains them.
///
/// They go beside the engines (VoiceCatalog.importedRoot/<era>/<Name>Compact),
/// where the bundled app had them, so they keep their identifiers.
enum MacCompacts {
    struct Recipe: Decodable {
        let releases: [String: Release]
    }

    struct Release: Decodable {
        let name: String
        let url: String
        let fetch: [[UInt64]]
        let streams: [Stream]
        let essentials: Essentials
        let voices: [String: Voice]
        let files: [File]
    }

    struct Stream: Decodable {
        let of: Int
        let map: [[UInt64]]?
        let udif: [[UInt64]]?
    }

    struct Essentials: Decodable {
        let codec: String
        let stream: Int
        let from: UInt64
        let to: UInt64
        let shift: Int?
        let level: Int?
        let bits: [Int]?
        let crc: UInt32?
        let skip: Int
    }

    struct Voice: Decodable {
        let sha256: String
        let locale: String
    }

    struct File: Decodable {
        let voice: String
        let stream: Int
        let size: UInt64
        let sha256: String
        let locale: String
    }

    struct CompactsError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    static let recipe: [String: Release] = {
        guard let url = Bundle.main.url(forResource: "MacCompacts", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return [:] }
        return (try? JSONDecoder().decode(Recipe.self, from: data))?.releases ?? [:]
    }()

    static var eras: [String] { recipe.keys.sorted() }

    static func voices(_ era: String) -> [String] {
        guard let r = recipe[era] else { return [] }
        return (Array(r.voices.keys) + r.files.map(\.voice)).sorted()
    }

    /// Releases whose compacts aren't all in `root` yet, and whose common files are
    /// (they come with the firmware import: EngineImport.assembleMacCommon).
    static func todo(_ root: URL) -> [String] {
        let fm = FileManager.default
        return eras.filter { era in
            fm.fileExists(atPath: root.appendingPathComponent("\(era)/common").path)
                && voices(era).contains { !fm.fileExists(atPath: root.appendingPathComponent("\(era)/\($0)/broker.hdr").path) }
        }
    }

    static func downloadSize(_ era: String) -> UInt64 {
        recipe[era]?.fetch.reduce(0) { $0 + $1[1] } ?? 0
    }

    /// Every compact of `era` into root/<era>/<Name>Compact/. Blocks.
    static func fetch(_ era: String, into root: URL, progress: @escaping EngineImport.Progress) throws {
        guard let r = recipe[era], let url = URL(string: r.url) else { throw CompactsError("There\u{2019}s no Mac OS X release called \(era).") }
        let total = downloadSize(era)
        progress("Downloading the \(r.name) compact voices (\(total / 1_000_000) MB)", 0)
        var spans: [(offset: UInt64, data: Data)] = []
        var done: UInt64 = 0
        for span in r.fetch {
            var data = Data(capacity: Int(span[1]))
            let base = done
            _ = try ParallelFetch.run(url, from: span[0], count: span[1], { data.append($0) },
                                      progress: { got in progress(nil, Double(base + got) / Double(max(total, 1))) })
            done += span[1]
            spans.append((span[0], data))
        }
        progress("Unpacking the \(r.name) compact voices", nil)
        let reader = Reader(release: r, spans: spans)
        let e = r.essentials
        let raw = try reader.read(e.stream, e.from, Int(e.to - e.from))
        let payload = try decode(e, raw)
        var found = try pcmWaves(payload, at: e.skip)
        for f in r.files { found[f.voice] = try reader.read(f.stream, 0, Int(f.size)) }
        var want = r.voices
        for f in r.files { want[f.voice] = Voice(sha256: f.sha256, locale: f.locale) }
        for (voice, info) in want {
            guard let d = found[voice] else { throw CompactsError("\(voice) isn\u{2019}t in the \(r.name) installer.") }
            var h = SHA256Stream()
            h.update(d)
            guard h.hex == info.sha256 else { throw CompactsError("\(voice) from the \(r.name) installer isn\u{2019}t the one expected.") }
        }
        let fm = FileManager.default
        let dir = root.appendingPathComponent(era, isDirectory: true)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        for (voice, info) in want.sorted(by: { $0.key < $1.key }) where !voice.contains("/") && !voice.contains("..") {
            let part = dir.appendingPathComponent(".\(voice).part", isDirectory: true)
            try? fm.removeItem(at: part)
            try fm.createDirectory(at: part, withIntermediateDirectories: true)
            try MacVoicePackage.unpackPCMWave(found[voice]!, into: part)
            try Data((info.locale + "\n").utf8).write(to: part.appendingPathComponent("locale"))
            let dest = dir.appendingPathComponent(voice, isDirectory: true)
            try? fm.removeItem(at: dest)
            try fm.moveItem(at: part, to: dest)
        }
        // the era's folder changed; the root's date tells every process to look again
        try? fm.setAttributes([.modificationDate: Date()], ofItemAtPath: root.path)
        progress("The \(r.name) compact voices are ready", 1)
    }

    /// The recipe's streams over the downloaded ranges: disk images (zlib
    /// chunks) and the files in them (runs of bytes).
    private final class Reader {
        let release: Release
        let spans: [(offset: UInt64, data: Data)]
        private var cache: [Int: (index: Int, data: Data)] = [:]

        init(release: Release, spans: [(offset: UInt64, data: Data)]) {
            self.release = release
            self.spans = spans
        }

        func read(_ stream: Int, _ off: UInt64, _ n: Int) throws -> Data {
            var out = Data(capacity: n)
            let end = off + UInt64(n)
            if stream < 0 {
                for s in spans {
                    let a = max(off, s.offset), b = min(end, s.offset + UInt64(s.data.count))
                    if a < b { out.append(s.data[(s.data.startIndex + Int(a - s.offset))..<(s.data.startIndex + Int(b - s.offset))]) }
                }
            } else {
                let spec = release.streams[stream]
                if let map = spec.map {
                    for p in map {
                        let a = max(off, p[0]), b = min(end, p[0] + p[1])
                        if a < b { out.append(try read(spec.of, p[2] + (a - p[0]), Int(b - a))) }
                    }
                } else if let chunks = spec.udif {
                    for (i, c) in chunks.enumerated() {
                        let a = max(off, c[1]), b = min(end, c[1] + c[2])
                        guard a < b else { continue }
                        let d = try chunk(stream, i, c, of: spec.of)
                        out.append(d[(d.startIndex + Int(a - c[1]))..<(d.startIndex + Int(b - c[1]))])
                    }
                }
            }
            guard out.count == n else { throw CompactsError("The voices\u{2019} recipe doesn\u{2019}t cover what it reads.") }
            return out
        }

        private func chunk(_ stream: Int, _ i: Int, _ c: [UInt64], of parent: Int) throws -> Data {
            if let hit = cache[stream], hit.index == i { return hit.data }
            let kind = UInt32(c[0]), length = Int(c[2])
            var data: Data
            switch kind {
            case 0x00000000, 0x00000002: data = Data(count: length)
            case 0x00000001: data = try read(parent, c[3], Int(c[4]))
            case 0x80000005: data = try inflate(read(parent, c[3], Int(c[4])), raw: false, dictionary: nil, size: length)
            default: throw CompactsError("A disk image chunk of type \(String(kind, radix: 16)).")
            }
            if data.count < length { data.append(Data(count: length - data.count)) }
            cache[stream] = (i, data)
            return data
        }
    }

    /// The voices' part of the payload, decompressed.
    private static func decode(_ e: Essentials, _ raw: Data) throws -> Data {
        if e.codec == "gzip" {
            // a deflate block that starts `shift` bits into the first byte
            let s = e.shift ?? 0
            var shifted = raw
            if s > 0 {
                raw.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
                    shifted.withUnsafeMutableBytes { (dst: UnsafeMutableRawBufferPointer) in
                        for i in 0..<src.count {
                            let next = i + 1 < src.count ? src[i + 1] : 0
                            dst[i] = (src[i] >> UInt8(s)) | (next << UInt8(8 - s))
                        }
                    }
                }
            }
            return try inflate(shifted, raw: true, dictionary: Data(count: 32768), size: nil)
        }
        // bzip2 blocks between two bit offsets, made a stream of their own
        guard let bits = e.bits, bits.count == 2, let crc = e.crc, let level = e.level else { throw CompactsError("The voices\u{2019} recipe is incomplete.") }
        let a = bits[0], b = bits[1]
        var stream = Data(Array("BZh".utf8) + [UInt8(0x30 + level)])
        let whole = (b - a) / 8
        var body = [UInt8](repeating: 0, count: whole)
        raw.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            let r = UInt8(a % 8), at = a / 8
            for j in 0..<whole {
                body[j] = r == 0 ? p[at + j] : (p[at + j] << r) | (p[at + j + 1] >> (8 - r))
            }
        }
        stream.append(contentsOf: body)
        var w = BitWriter()
        raw.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            for bit in (a + whole * 8)..<b { w.put(UInt64((p[bit >> 3] >> (7 - UInt8(bit & 7))) & 1), 1) }
        }
        w.put(0x177245385090, 48)
        w.put(UInt64(crc), 32)
        stream.append(w.finish())
        return try bunzip(stream)
    }

    private struct BitWriter {
        var bytes: [UInt8] = []
        var acc: UInt64 = 0
        var n = 0
        mutating func put(_ v: UInt64, _ count: Int) {
            for k in stride(from: count - 1, through: 0, by: -1) {
                acc = acc << 1 | ((v >> UInt64(k)) & 1)
                n += 1
                if n == 8 { bytes.append(UInt8(acc)); acc = 0; n = 0 }
            }
        }
        mutating func finish() -> Data {
            if n > 0 { bytes.append(UInt8(acc << UInt64(8 - n))); acc = 0; n = 0 }
            return Data(bytes)
        }
    }

    private static func inflate(_ input: Data, raw: Bool, dictionary: Data?, size: Int?) throws -> Data {
        var z = z_stream()
        guard inflateInit2_(&z, raw ? -15 : 15, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw CompactsError("zlib couldn\u{2019}t start.")
        }
        defer { inflateEnd(&z) }
        if let dictionary {
            let rc = dictionary.withUnsafeBytes { inflateSetDictionary(&z, $0.bindMemory(to: Bytef.self).baseAddress, uInt($0.count)) }
            guard rc == Z_OK else { throw CompactsError("zlib couldn\u{2019}t start.") }
        }
        var out = Data(capacity: size ?? input.count * 2)
        var buf = [UInt8](repeating: 0, count: 1 << 20)
        try input.withUnsafeBytes { (i: UnsafeRawBufferPointer) in
            z.next_in = UnsafeMutablePointer(mutating: i.bindMemory(to: Bytef.self).baseAddress)
            z.avail_in = uInt(i.count)
            while true {
                let rc = buf.withUnsafeMutableBufferPointer { b -> Int32 in
                    z.next_out = b.baseAddress
                    z.avail_out = uInt(b.count)
                    return zlib.inflate(&z, Z_NO_FLUSH)
                }
                out.append(contentsOf: buf[0 ..< buf.count - Int(z.avail_out)])
                if rc == Z_STREAM_END { break }
                // the voices' stretch ends inside the stream: running out of input is the end
                if rc == Z_BUF_ERROR || (rc == Z_OK && z.avail_in == 0 && z.avail_out > 0) { break }
                guard rc == Z_OK else { throw CompactsError("The download is damaged (zlib \(rc)).") }
            }
        }
        return out
    }

    private static func bunzip(_ input: Data) throws -> Data {
        var bz = bz_stream()
        guard BZ2_bzDecompressInit(&bz, 0, 0) == BZ_OK else { throw CompactsError("bzip2 couldn\u{2019}t start.") }
        defer { BZ2_bzDecompressEnd(&bz) }
        var out = Data()
        var buf = [CChar](repeating: 0, count: 1 << 20)
        try input.withUnsafeBytes { (i: UnsafeRawBufferPointer) in
            bz.next_in = UnsafeMutablePointer(mutating: i.bindMemory(to: CChar.self).baseAddress)
            bz.avail_in = UInt32(i.count)
            while true {
                let rc = buf.withUnsafeMutableBufferPointer { b -> Int32 in
                    bz.next_out = b.baseAddress
                    bz.avail_out = UInt32(b.count)
                    return BZ2_bzDecompress(&bz)
                }
                buf.withUnsafeBytes { out.append(contentsOf: $0[0 ..< $0.count - Int(bz.avail_out)]) }
                if rc == BZ_STREAM_END { break }
                guard rc == BZ_OK else { throw CompactsError("The download is damaged (bzip2 \(rc)).") }
                if bz.avail_in == 0 && bz.avail_out > 0 { throw CompactsError("The download ended early.") }
            }
        }
        return out
    }

    /// PCMWave of every *Compact.SpeechVoice entry of the cpio archive from `at` on.
    private static func pcmWaves(_ d: Data, at start: Int) throws -> [String: Data] {
        var out: [String: Data] = [:]
        var at = d.startIndex + start
        while at + 76 <= d.endIndex, d[at..<(at + 6)] == Data("070707".utf8) {
            guard let nl = Int(String(decoding: d[(at + 59)..<(at + 65)], as: UTF8.self), radix: 8),
                  let fs = Int(String(decoding: d[(at + 65)..<(at + 76)], as: UTF8.self), radix: 8),
                  at + 76 + nl + fs <= d.endIndex else { break }
            let name = String(decoding: d[(at + 76)..<(at + 75 + nl)], as: UTF8.self)
            let parts = name.split(separator: "/", omittingEmptySubsequences: false)
            guard parts.count >= 6, parts[5].hasSuffix("Compact.SpeechVoice") else { break }
            if name.hasSuffix("/Contents/Resources/PCMWave") {
                out[String(parts[5].dropLast(".SpeechVoice".count))] = d[(at + 76 + nl)..<(at + 76 + nl + fs)]
            }
            at += 76 + nl + fs
        }
        return out
    }
}
