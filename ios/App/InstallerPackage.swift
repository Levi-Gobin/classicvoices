import Foundation

/// Reads the speech files out of a Mac OS X install disc's packages, without
/// the installer: the MacinTalk engine, its dictionary, the C++ libraries it
/// loads, and the voices. Everything else in the package is skipped as it
/// streams past.
///
/// - Tiger and Leopard: bundle packages, Contents/Archive.pax.gz (gzip'd cpio)
/// - Snow Leopard and Lion: flat (xar) packages whose Payload is a bzip2'd or
///   gzip'd cpio
///
/// Files keep their paths below `tree`, so SpeechDataImport.scan finds them as
/// it would on a disk.
enum InstallerPackage {
    /// The packages on an install disc that hold speech files.
    static let names: Set<String> = ["Essentials.pkg", "BaseSystem.pkg", "BaseSystemBinaries.pkg",
                                     "AdditionalSpeechVoices.pkg", "AdditionalEssentials.pkg"]

    static func wants(_ path: String) -> Bool {
        var p = Substring(path)
        if p.hasPrefix("./") { p = p.dropFirst(2) }
        return p.hasPrefix("System/Library/Speech/Voices/")
            || p.hasPrefix("System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/")
            || p.hasPrefix("System/Library/PrivateFrameworks/SpeechDictionary.framework/")
            || SpeechDataImport.libraryNames.contains { p == "usr/lib/\($0)" }
    }

    static func extract(_ pkg: URL, into tree: URL) throws {
        var isDir: ObjCBool = false
        FileManager.default.fileExists(atPath: pkg.path, isDirectory: &isDir)
        let file = isDir.boolValue ? pkg.appendingPathComponent("Contents/Archive.pax.gz") : pkg
        let fh = try FileHandle(forReadingFrom: file)
        defer { try? fh.close() }
        let size = try fh.seekToEnd()
        try fh.seek(toOffset: 0)
        try extract(fh, size: size, archive: isDir.boolValue, name: pkg.lastPathComponent, into: tree)
    }

    /// `archive`: the reader is a bundle package's Archive.pax.gz rather than
    /// a flat package. `keep` narrows what's written (paths as in `wants`);
    /// `progress` is the part of the package read so far.
    static func extract(_ r: ByteReader, size: UInt64, archive: Bool, name: String, into tree: URL,
                        keep: ((String) -> Bool)? = nil, progress: ((Double) -> Void)? = nil) throws {
        let sink = TreeSink(root: tree, keep: keep ?? wants)
        let length = archive ? size : try MacVoicePackage.seekToPayload(r)
        let start = try r.offset()
        let magic = try r.read(upToCount: 3) ?? Data()
        try r.seek(toOffset: start)
        if magic.starts(with: [0x1F, 0x8B]) {
            try Inflate.stream(from: r, count: length, into: sink.feed, progress: progress)
        } else if magic == Data("BZh".utf8) {
            try Bunzip2.stream(from: r, count: length, into: sink.feed, progress: progress)
        } else {
            throw MacVoicePackage.PackageError.corrupt("\(name) uses a compression Classic Voices can\u{2019}t read")
        }
    }

    // MARK: - gzip, with zlib itself (the Compression framework stops early
    // on some of the install discs' larger payloads)

    enum Inflate {
        static func stream(from fh: ByteReader, count: UInt64, into sink: (UnsafeRawBufferPointer) throws -> Void,
                           progress: ((Double) -> Void)? = nil) throws {
            var z = z_stream()
            guard inflateInit2_(&z, 15 + 16, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
                throw MacVoicePackage.PackageError.corrupt("inflate init")
            }
            defer { inflateEnd(&z) }
            let outSize = 1 << 20
            let out = UnsafeMutablePointer<UInt8>.allocate(capacity: outSize)
            defer { out.deallocate() }
            var left = count
            while true {
                let chunk = try fh.read(upToCount: Int(min(UInt64(1 << 20), left))) ?? Data()
                left -= UInt64(chunk.count)
                if chunk.isEmpty { throw MacVoicePackage.PackageError.corrupt("payload truncated") }
                progress?(Double(count - left) / Double(max(count, 1)))
                var ended = false
                try chunk.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
                    z.next_in = UnsafeMutablePointer(mutating: src.bindMemory(to: UInt8.self).baseAddress)
                    z.avail_in = UInt32(src.count)
                    repeat {
                        z.next_out = out
                        z.avail_out = UInt32(outSize)
                        let rc = inflate(&z, Z_NO_FLUSH)
                        if rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR {
                            throw MacVoicePackage.PackageError.corrupt("gzip data")
                        }
                        let produced = outSize - Int(z.avail_out)
                        if produced > 0 { try sink(UnsafeRawBufferPointer(start: out, count: produced)) }
                        if rc == Z_STREAM_END { ended = true; return }
                    } while z.avail_in > 0 || z.avail_out == 0
                }
                if ended || left == 0 { return }
            }
        }
    }

    // MARK: - bzip2

    enum Bunzip2 {
        static func stream(from fh: ByteReader, count: UInt64, into sink: (UnsafeRawBufferPointer) throws -> Void,
                           progress: ((Double) -> Void)? = nil) throws {
            var bz = bz_stream()
            guard BZ2_bzDecompressInit(&bz, 0, 0) == BZ_OK else { throw MacVoicePackage.PackageError.corrupt("bzip2 init") }
            defer { BZ2_bzDecompressEnd(&bz) }
            let outSize = 1 << 20
            let out = UnsafeMutablePointer<CChar>.allocate(capacity: outSize)
            defer { out.deallocate() }
            var left = count
            while true {
                let chunk = try fh.read(upToCount: Int(min(UInt64(1 << 20), left))) ?? Data()
                left -= UInt64(chunk.count)
                if chunk.isEmpty { throw MacVoicePackage.PackageError.corrupt("payload truncated") }
                progress?(Double(count - left) / Double(max(count, 1)))
                var ended = false
                try chunk.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
                    bz.next_in = UnsafeMutablePointer(mutating: src.bindMemory(to: CChar.self).baseAddress)
                    bz.avail_in = UInt32(src.count)
                    repeat {
                        bz.next_out = out
                        bz.avail_out = UInt32(outSize)
                        let rc = BZ2_bzDecompress(&bz)
                        if rc != BZ_OK && rc != BZ_STREAM_END { throw MacVoicePackage.PackageError.corrupt("bzip2 data") }
                        let produced = outSize - Int(bz.avail_out)
                        if produced > 0 { try sink(UnsafeRawBufferPointer(start: out, count: produced)) }
                        if rc == BZ_STREAM_END { ended = true; return }
                    } while bz.avail_in > 0 || bz.avail_out == 0
                }
                if ended || left == 0 { return }
            }
        }
    }

    // MARK: - cpio (odc) → files under `root`

    private final class TreeSink {
        let root: URL
        private var header = Data()
        private var name = Data()
        private var nameLeft = 0
        private var dataLeft: UInt64 = 0
        private var out: FileHandle?
        private var finished = false
        private enum State { case header, name, data }
        private var state = State.header

        let keep: (String) -> Bool
        init(root: URL, keep: @escaping (String) -> Bool) { self.root = root; self.keep = keep }

        func feed(_ buf: UnsafeRawBufferPointer) throws {
            var i = 0
            while i < buf.count && !finished {
                switch state {
                case .header:
                    let take = min(76 - header.count, buf.count - i)
                    header.append(buf.baseAddress!.advanced(by: i).assumingMemoryBound(to: UInt8.self), count: take)
                    i += take
                    if header.count == 76 {
                        guard header.prefix(6) == Data("070707".utf8),
                              let mode = Int(String(decoding: header[18..<24], as: UTF8.self), radix: 8),
                              let ns = Int(String(decoding: header[59..<65], as: UTF8.self), radix: 8),
                              let fs = UInt64(String(decoding: header[65..<76], as: UTF8.self), radix: 8)
                        else { throw MacVoicePackage.PackageError.corrupt("cpio header") }
                        nameLeft = ns
                        dataLeft = fs
                        name = Data()
                        isFile = mode & 0o170000 == 0o100000
                        state = .name
                    }
                case .name:
                    let take = min(nameLeft, buf.count - i)
                    name.append(buf.baseAddress!.advanced(by: i).assumingMemoryBound(to: UInt8.self), count: take)
                    i += take
                    nameLeft -= take
                    if nameLeft == 0 {
                        let path = String(decoding: name.prefix { $0 != 0 }, as: UTF8.self)
                        if path == "TRAILER!!!" { finished = true; return }
                        if isFile, keep(path), !path.split(separator: "/").contains("..") {
                            let url = root.appendingPathComponent(path.hasPrefix("./") ? String(path.dropFirst(2)) : path)
                            try FileManager.default.createDirectory(at: url.deletingLastPathComponent(),
                                                                    withIntermediateDirectories: true)
                            FileManager.default.createFile(atPath: url.path, contents: nil)
                            out = try FileHandle(forWritingTo: url)
                        }
                        header = Data()
                        state = dataLeft == 0 ? .header : .data
                        if dataLeft == 0 { try out?.close(); out = nil }
                    }
                case .data:
                    let take = Int(min(dataLeft, UInt64(buf.count - i)))
                    if let out { try out.write(contentsOf: Data(UnsafeRawBufferPointer(rebasing: buf[i..<(i + take)]))) }
                    i += take
                    dataLeft -= UInt64(take)
                    if dataLeft == 0 { try out?.close(); out = nil; state = .header }
                }
            }
        }

        private var isFile = false
    }
}
