import Compression
import Foundation

/// Something read in order with occasional seeks: a file, or a file inside a
/// disk image.
protocol ByteReader: AnyObject {
    func read(upToCount count: Int) throws -> Data?
    func seek(toOffset offset: UInt64) throws
    func offset() throws -> UInt64
}

extension FileHandle: ByteReader {}

/// A Mac OS X install disc image, read in place: UDIF (.dmg, compressed with
/// zlib, bzip2, ADC or LZFSE) or raw (.iso, .cdr), with an Apple, GUID or no
/// partition map, holding an HFS+ volume. Only what's asked for is read, so a
/// 6 GB disc needs no more space than the files taken from it.
final class DiskImage {
    enum ImageError: LocalizedError {
        case unreadable(String)
        var errorDescription: String? {
            switch self { case .unreadable(let what): return what }
        }
    }

    private struct Chunk {
        let type: UInt32
        let start: UInt64       // first byte in the image
        let length: UInt64
        let fileOffset: UInt64  // compressed bytes in the .dmg
        let fileLength: UInt64
    }

    private let fh: FileHandle
    private var chunks: [Chunk] = []   // UDIF only, sorted by start
    let size: UInt64
    private var cache: [(Int, Data)] = []  // a few decoded chunks

    init(_ url: URL) throws {
        fh = try FileHandle(forReadingFrom: url)
        let end = try fh.seekToEnd()
        try fh.seek(toOffset: end - 512)
        let koly = try fh.read(upToCount: 512) ?? Data()
        guard koly.count == 512, koly.prefix(4) == Data("koly".utf8) else {
            size = end  // raw image
            return
        }
        let dataFork = koly.be64(0x18)
        let xmlOffset = koly.be64(0xD8), xmlLength = koly.be64(0xE0)
        try fh.seek(toOffset: xmlOffset)
        guard let xml = try fh.read(upToCount: Int(xmlLength)),
              let plist = try PropertyListSerialization.propertyList(from: xml, format: nil) as? [String: Any],
              let rf = plist["resource-fork"] as? [String: Any], let blkx = rf["blkx"] as? [[String: Any]]
        else { throw ImageError.unreadable("That disk image has no table of its contents.") }
        var total: UInt64 = 0
        for entry in blkx {
            guard let d = entry["Data"] as? Data, d.count >= 204, d.prefix(4) == Data("mish".utf8) else { continue }
            let first = d.be64(8), dataOffset = d.be64(24)
            let n = Int(d.be32(200))
            for i in 0..<n {
                let o = 204 + 40 * i
                guard o + 40 <= d.count else { break }
                let type = d.be32(o)
                if type == 0x7FFF_FFFE || type == 0xFFFF_FFFF { continue }  // comment, end
                let start = (first + d.be64(o + 8)) * 512, length = d.be64(o + 16) * 512
                chunks.append(Chunk(type: type, start: start, length: length,
                                    fileOffset: dataFork + dataOffset + d.be64(o + 24), fileLength: d.be64(o + 32)))
                total = max(total, start + length)
            }
        }
        chunks.sort { $0.start < $1.start }
        size = total
    }

    deinit { try? fh.close() }

    /// `count` bytes at `offset` of the disk the image holds.
    func read(at offset: UInt64, count: Int) throws -> Data {
        if chunks.isEmpty {
            try fh.seek(toOffset: offset)
            return try fh.read(upToCount: count) ?? Data()
        }
        var out = Data(capacity: count)
        var pos = offset
        let end = min(offset + UInt64(count), size)
        while pos < end {
            // binary search for the chunk holding pos
            var lo = 0, hi = chunks.count - 1
            while lo < hi {
                let mid = (lo + hi + 1) / 2
                if chunks[mid].start <= pos { lo = mid } else { hi = mid - 1 }
            }
            let c = chunks[lo]
            guard pos >= c.start, pos < c.start + c.length else {
                out.append(Data(count: Int(min(end - pos, 512))))  // a gap: zeros
                pos += min(end - pos, 512)
                continue
            }
            let data = try decoded(lo)
            let from = Int(pos - c.start), take = Int(min(end - pos, c.start + c.length - pos))
            if from < data.count { out.append(data[data.startIndex + from ..< data.startIndex + min(from + take, data.count)]) }
            if from + take > data.count { out.append(Data(count: from + take - max(from, data.count))) }
            pos += UInt64(take)
        }
        return out
    }

    private func decoded(_ i: Int) throws -> Data {
        if let hit = cache.first(where: { $0.0 == i }) { return hit.1 }
        let c = chunks[i]
        let n = Int(c.length)
        var data: Data
        switch c.type {
        case 0, 2:  // zero fill
            data = Data(count: n)
        case 1:     // stored
            try fh.seek(toOffset: c.fileOffset)
            data = try fh.read(upToCount: Int(c.fileLength)) ?? Data()
        default:
            try fh.seek(toOffset: c.fileOffset)
            let packed = try fh.read(upToCount: Int(c.fileLength)) ?? Data()
            data = try Self.decompress(packed, type: c.type, size: n)
        }
        cache.append((i, data))
        if cache.count > 4 { cache.removeFirst() }
        return data
    }

    private static func decompress(_ packed: Data, type: UInt32, size: Int) throws -> Data {
        var out = Data(count: size)
        var produced = 0
        switch type {
        case 0x8000_0005:  // zlib
            var z = z_stream()
            guard inflateInit_(&z, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else { break }
            defer { inflateEnd(&z) }
            packed.withUnsafeBytes { src in
                out.withUnsafeMutableBytes { dst in
                    z.next_in = UnsafeMutablePointer(mutating: src.bindMemory(to: UInt8.self).baseAddress)
                    z.avail_in = UInt32(src.count)
                    z.next_out = dst.bindMemory(to: UInt8.self).baseAddress
                    z.avail_out = UInt32(dst.count)
                    _ = inflate(&z, Z_FINISH)
                    produced = dst.count - Int(z.avail_out)
                }
            }
        case 0x8000_0006:  // bzip2
            var len = UInt32(size)
            let rc = packed.withUnsafeBytes { src in
                out.withUnsafeMutableBytes { dst in
                    BZ2_bzBuffToBuffDecompress(dst.bindMemory(to: CChar.self).baseAddress, &len,
                                               UnsafeMutablePointer(mutating: src.bindMemory(to: CChar.self).baseAddress),
                                               UInt32(src.count), 0, 0)
                }
            }
            if rc == BZ_OK { produced = Int(len) }
        case 0x8000_0007:  // LZFSE
            produced = packed.withUnsafeBytes { src in
                out.withUnsafeMutableBytes { dst in
                    compression_decode_buffer(dst.bindMemory(to: UInt8.self).baseAddress!, dst.count,
                                              src.bindMemory(to: UInt8.self).baseAddress!, src.count, nil, COMPRESSION_LZFSE)
                }
            }
        case 0x8000_0004:  // ADC
            produced = adc(packed, into: &out)
        default:
            throw ImageError.unreadable("That disk image is compressed in a way Classic Voices can\u{2019}t read (\(String(type, radix: 16))).")
        }
        guard produced == size else { throw ImageError.unreadable("Part of that disk image didn\u{2019}t unpack; the file may be damaged.") }
        return out
    }

    /// Apple Data Compression, used by old disk images.
    private static func adc(_ src: Data, into out: inout Data) -> Int {
        let s = [UInt8](src)
        var o = 0, i = 0
        out.withUnsafeMutableBytes { raw in
            let d = raw.bindMemory(to: UInt8.self)
            while i < s.count && o < d.count {
                let b = s[i]
                if b & 0x80 != 0 {  // literal run
                    let n = Int(b & 0x7F) + 1
                    guard i + 1 + n <= s.count, o + n <= d.count else { i = s.count; break }
                    for k in 0..<n { d[o + k] = s[i + 1 + k] }
                    o += n; i += 1 + n
                } else {
                    var n: Int, dist: Int
                    if b & 0x40 != 0 {  // 3-byte copy
                        guard i + 2 < s.count else { i = s.count; break }
                        n = Int(b & 0x3F) + 4; dist = Int(s[i + 1]) << 8 | Int(s[i + 2]); i += 3
                    } else {            // 2-byte copy
                        guard i + 1 < s.count else { i = s.count; break }
                        n = Int((b >> 2) & 0x0F) + 3; dist = Int(b & 0x03) << 8 | Int(s[i + 1]); i += 2
                    }
                    let from = o - dist - 1
                    guard from >= 0, o + n <= d.count else { i = s.count; break }
                    for k in 0..<n { d[o + k] = d[from + k] }
                    o += n
                }
            }
        }
        return o
    }

    // MARK: - partitions

    /// Byte offsets of the HFS+ volumes on the disk.
    func hfsVolumes() throws -> [UInt64] {
        func isHFSPlus(_ at: UInt64) -> Bool {
            guard let h = try? read(at: at + 1024, count: 2) else { return false }
            return h == Data("H+".utf8) || h == Data("HX".utf8)
        }
        if isHFSPlus(0) { return [0] }
        var found: [UInt64] = []
        let head = try read(at: 0, count: 1024)
        // GUID partition table
        let gpt = try read(at: 512, count: 92)
        if gpt.prefix(8) == Data("EFI PART".utf8) {
            let entries = gpt.le64(72), n = Int(gpt.le32(80)), entrySize = Int(gpt.le32(84))
            let table = try read(at: entries * 512, count: min(n, 256) * entrySize)
            let hfsGUID = Data([0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0xAA, 0x11, 0xAA, 0x11, 0x00, 0x30, 0x65, 0x43, 0xEC, 0xAC])
            for k in 0..<min(n, 256) where (k + 1) * entrySize <= table.count {
                let e = table[table.startIndex + k * entrySize ..< table.startIndex + (k + 1) * entrySize]
                if Data(e.prefix(16)) == hfsGUID { found.append(Data(e).le64(32) * 512) }
            }
        }
        // Apple partition map
        if found.isEmpty, head.prefix(2) == Data("ER".utf8) {
            // Entries are one per block from block 1; CDs and DVDs use 2048-byte blocks.
            let bs = UInt64(head.be16(2) == 0 ? 512 : head.be16(2))
            let first = try read(at: bs, count: 512)
            let count = first.prefix(2) == Data("PM".utf8) ? Int(first.be32(4)) : 0
            for k in 1...max(1, min(count, 64)) {
                let pm = try read(at: UInt64(k) * bs, count: 512)
                guard pm.prefix(2) == Data("PM".utf8) else { break }
                let type = String(decoding: pm[48..<80].prefix { $0 != 0 }, as: UTF8.self)
                if type == "Apple_HFS" || type == "Apple_HFSX" { found.append(UInt64(pm.be32(8)) * bs) }
            }
        }
        return found.filter(isHFSPlus)
    }
}

// MARK: - HFS+

/// An HFS+ volume on a disk image: its files by path, and readers for them.
final class HFSVolume {
    struct Extent { let start: UInt32; let count: UInt32 }
    struct Fork { let size: UInt64; var extents: [Extent] }
    struct File { let path: String; let id: UInt32; var fork: Fork }

    private let disk: DiskImage
    private let base: UInt64
    private let blockSize: UInt64
    private var catalog: Fork
    private let overflow: Fork

    init(_ disk: DiskImage, at base: UInt64) throws {
        self.disk = disk
        self.base = base
        let vh = try disk.read(at: base + 1024, count: 512)
        blockSize = UInt64(vh.be32(40))
        guard blockSize >= 512 else { throw DiskImage.ImageError.unreadable("That disc\u{2019}s volume is damaged.") }
        catalog = Self.fork(vh, at: 272)
        overflow = Self.fork(vh, at: 192)
        catalog.extents += try overflowExtents(fileID: 4, have: catalog)
    }

    private static func fork(_ d: Data, at o: Int) -> Fork {
        Fork(size: d.be64(o), extents: (0..<8).map { Extent(start: d.be32(o + 16 + 8 * $0), count: d.be32(o + 20 + 8 * $0)) }
            .filter { $0.count > 0 })
    }

    /// Bytes `offset..<offset+count` of a fork.
    func read(_ fork: Fork, at offset: UInt64, count: Int) throws -> Data {
        var out = Data(capacity: count)
        var pos = offset
        let end = min(offset + UInt64(count), fork.size)
        var extentStart: UInt64 = 0
        for e in fork.extents where pos < end {
            let len = UInt64(e.count) * blockSize
            if pos < extentStart + len {
                let within = pos - extentStart
                let take = min(end - pos, len - within)
                out.append(try disk.read(at: base + UInt64(e.start) * blockSize + within, count: Int(take)))
                pos += take
            }
            extentStart += len
        }
        return out
    }

    /// Extents beyond the first eight, from the extents overflow file.
    private func overflowExtents(fileID: UInt32, have fork: Fork) throws -> [Extent] {
        let covered = fork.extents.reduce(UInt64(0)) { $0 + UInt64($1.count) }
        guard covered * blockSize < fork.size, overflow.size > 0 else { return [] }
        var out: [Extent] = []
        try walkLeaves(overflow) { rec in
            guard rec.count >= 12 + 64, rec.be16(0) == 10, rec[rec.startIndex + 2] == 0, rec.be32(4) == fileID else { return }
            for k in 0..<8 {
                let e = Extent(start: rec.be32(12 + 8 * k), count: rec.be32(16 + 8 * k))
                if e.count > 0 { out.append(e) }
            }
        }
        return out
    }

    /// Calls `body` with every leaf record (key and data) of a B-tree.
    private func walkLeaves(_ tree: Fork, _ body: (Data) throws -> Void) throws {
        let header = try read(tree, at: 0, count: 512)
        let nodeSize = Int(header.be16(14 + 18))
        var node = header.be32(14 + 10)  // first leaf
        var seen = 0
        while node != 0 && seen < 10_000_000 {
            seen += 1
            let d = try read(tree, at: UInt64(node) * UInt64(nodeSize), count: nodeSize)
            guard d.count == nodeSize, Int8(bitPattern: d[d.startIndex + 8]) == -1 else { break }
            let n = Int(d.be16(10))
            for r in 0..<n {
                let start = Int(d.be16(nodeSize - 2 * (r + 1)))
                let end = Int(d.be16(nodeSize - 2 * (r + 2)))
                guard start < end, end <= nodeSize else { continue }
                try body(d.subdata(in: d.startIndex + start ..< d.startIndex + end))
            }
            node = d.be32(0)
        }
    }

    /// Files whose path `want` accepts (paths are relative to the volume
    /// root, like "System/Installation/Packages/Essentials.pkg").
    func files(where want: (String) -> Bool) throws -> [File] {
        var folders: [UInt32: (UInt32, String)] = [:]
        var files: [(UInt32, String, UInt32, Fork)] = []
        try walkLeaves(catalog) { rec in
            guard rec.count >= 8 else { return }
            let keyLength = Int(rec.be16(0))
            let parent = rec.be32(2)
            let nameLength = Int(rec.be16(6))
            guard 8 + 2 * nameLength <= rec.count else { return }
            var units = [UInt16]()
            units.reserveCapacity(nameLength)
            for k in 0..<nameLength { units.append(rec.be16(8 + 2 * k)) }
            let name = String(decoding: units, as: UTF16.self).replacingOccurrences(of: "/", with: ":")
            var o = 2 + keyLength
            if o % 2 == 1 { o += 1 }
            guard o + 2 <= rec.count else { return }
            switch rec.be16(o) {
            case 1 where o + 12 <= rec.count:  // folder
                folders[rec.be32(o + 8)] = (parent, name)
            case 2 where o + 168 <= rec.count:  // file
                files.append((parent, name, rec.be32(o + 8), Self.fork(rec, at: o + 88)))
            default: break
            }
        }
        func path(_ id: UInt32) -> String? {
            var parts: [String] = []
            var id = id
            var hops = 0
            while id != 2 {  // the root folder
                guard let (parent, name) = folders[id], hops < 64 else { return nil }
                parts.append(name)
                id = parent
                hops += 1
            }
            return parts.reversed().joined(separator: "/")
        }
        var out: [File] = []
        for (parent, name, id, fork) in files {
            guard let dir = path(parent) else { continue }
            let p = dir.isEmpty ? name : dir + "/" + name
            guard want(p) else { continue }
            var f = File(path: p, id: id, fork: fork)
            f.fork.extents += try overflowExtents(fileID: id, have: fork)
            out.append(f)
        }
        return out
    }

    /// Reads a file in order, for the package readers.
    func reader(_ file: File) -> ByteReader { ForkReader(volume: self, fork: file.fork) }

    private final class ForkReader: ByteReader {
        let volume: HFSVolume
        let fork: Fork
        var pos: UInt64 = 0
        init(volume: HFSVolume, fork: Fork) { self.volume = volume; self.fork = fork }
        func read(upToCount count: Int) throws -> Data? {
            let d = try volume.read(fork, at: pos, count: count)
            pos += UInt64(d.count)
            return d
        }
        func seek(toOffset offset: UInt64) throws { pos = offset }
        func offset() throws -> UInt64 { pos }
    }
}

extension Data {
    func le32(_ o: Int) -> UInt32 {
        let i = startIndex + o
        return UInt32(self[i]) | UInt32(self[i + 1]) << 8 | UInt32(self[i + 2]) << 16 | UInt32(self[i + 3]) << 24
    }
    func le64(_ o: Int) -> UInt64 { UInt64(le32(o)) | UInt64(le32(o + 4)) << 32 }
}
