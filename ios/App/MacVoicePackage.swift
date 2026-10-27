import Compression
import Foundation

/// Unpacks a Mac OS X 10.7-10.9 voice installer package (MLV_*.pkg /
/// MultiLingualVoice_*.pkg) without the system installer:
///
///   xar archive → "Payload" (gzip) → odc cpio → <Voice>.SpeechVoice/Contents/
///   Resources/PCMWave (ustar with ROT13 member names) → plain data files.
///
/// Everything is streamed, so a 400 MB premium voice needs no more memory than
/// a few buffers. Each voice folder found (e.g. "Karen", "KarenCompact") is
/// written to <dest>/<folder>/ when `keep(folder)` says so.
enum MacVoicePackage {
    enum PackageError: LocalizedError {
        case notXar, noPayload, corrupt(String)
        var errorDescription: String? {
            switch self {
            case .notXar: return "Not an installer package"
            case .noPayload: return "Package has no payload"
            case .corrupt(let what): return "Package problem: \(what)"
            }
        }
    }

    /// Returns the voice folders written.
    @discardableResult
    static func extract(_ pkg: URL, to dest: URL, keep: @escaping (String) -> Bool,
                        progress: ((Double) -> Void)? = nil) throws -> [String] {
        let fh = try FileHandle(forReadingFrom: pkg)
        defer { try? fh.close() }
        let length = try seekToPayload(fh)
        let sink = CpioSink(dest: dest, keep: keep)
        try Gunzip.stream(from: fh, count: length, into: sink.feed, progress: progress)
        guard sink.finished else { throw PackageError.corrupt("payload ended early") }
        return sink.written
    }

    /// Positions `fh` at the start of a xar package's Payload; returns its length.
    static func seekToPayload(_ fh: ByteReader) throws -> UInt64 {
        let (offset, length) = try member("Payload", of: fh)
        try fh.seek(toOffset: offset)
        return length
    }

    /// Where a stored (uncompressed) member of a xar archive is in it.
    static func member(_ name: String, of fh: ByteReader) throws -> (offset: UInt64, length: UInt64) {
        // xar header (big-endian): magic, header size, version, TOC lengths
        try fh.seek(toOffset: 0)
        let head = try fh.read(upToCount: 28) ?? Data()
        guard head.count == 28, head.prefix(4) == Data("xar!".utf8) else { throw PackageError.notXar }
        let headerSize = UInt64(head.be16(4))
        let tocCompressed = head.be64(8)
        try fh.seek(toOffset: headerSize)
        guard let tocZ = try fh.read(upToCount: Int(tocCompressed)), tocZ.count > 2,
              let tocData = try? (tocZ.dropFirst(2) as NSData).decompressed(using: .zlib) as Data
        else { throw PackageError.corrupt("table of contents") }
        let toc = TOCParser(data: tocData, want: name)
        guard let payload = toc.payload else { throw PackageError.noPayload }
        guard payload.encoding.hasSuffix("octet-stream") else { throw PackageError.corrupt("\(name) encoding \(payload.encoding)") }
        return (headerSize + tocCompressed + payload.offset, payload.length)
    }

    // MARK: - xar TOC

    private final class TOCParser: NSObject, XMLParserDelegate {
        struct Entry { var offset: UInt64; var length: UInt64; var encoding: String }
        var payload: Entry?
        private let want: String
        private var path: [String] = []
        private var text = ""
        private var name = "", offset: UInt64 = 0, length: UInt64 = 0, encoding = ""

        init(data: Data, want: String) {
            self.want = want
            super.init()
            let p = XMLParser(data: data)
            p.delegate = self
            p.parse()
        }

        func parser(_ parser: XMLParser, didStartElement el: String, namespaceURI: String?, qualifiedName: String?,
                    attributes: [String: String] = [:]) {
            path.append(el)
            text = ""
            if el == "file" { name = ""; offset = 0; length = 0; encoding = "" }
            if el == "encoding", path.dropLast().last == "data" { encoding = attributes["style"] ?? "" }
        }

        func parser(_ parser: XMLParser, foundCharacters s: String) { text += s }

        func parser(_ parser: XMLParser, didEndElement el: String, namespaceURI: String?, qualifiedName: String?) {
            let parent = path.dropLast().last
            switch (el, parent) {
            case ("name", "file"): name = text
            case ("offset", "data"): offset = UInt64(text) ?? 0
            case ("length", "data"): length = UInt64(text) ?? 0
            case ("file", _): if name == want, payload == nil { payload = Entry(offset: offset, length: length, encoding: encoding) }
            default: break
            }
            path.removeLast()
        }
    }

    // MARK: - gzip

    enum Gunzip {
        static func stream(from fh: FileHandle, count: UInt64, into sink: (UnsafeRawBufferPointer) throws -> Void,
                           progress: ((Double) -> Void)?) throws {
            var left = count
            func read(_ n: Int) throws -> Data {
                let d = try fh.read(upToCount: Int(min(UInt64(n), left))) ?? Data()
                left -= UInt64(d.count)
                return d
            }
            // header
            var first = try read(1 << 16)
            guard first.count > 10, first[first.startIndex] == 0x1F, first[first.startIndex + 1] == 0x8B,
                  first[first.startIndex + 2] == 8 else { throw PackageError.corrupt("payload is not gzip") }
            let flags = first[first.startIndex + 3]
            var p = 10
            if flags & 4 != 0 { p += 2 + Int(first[first.startIndex + p]) | Int(first[first.startIndex + p + 1]) << 8 }
            if flags & 8 != 0 { while first[first.startIndex + p] != 0 { p += 1 }; p += 1 }
            if flags & 16 != 0 { while first[first.startIndex + p] != 0 { p += 1 }; p += 1 }
            if flags & 2 != 0 { p += 2 }
            first = first.dropFirst(p)

            let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
            defer { stream.deallocate() }
            guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
                throw PackageError.corrupt("inflate init")
            }
            defer { compression_stream_destroy(stream) }
            let outSize = 1 << 20
            let out = UnsafeMutablePointer<UInt8>.allocate(capacity: outSize)
            defer { out.deallocate() }
            var chunk = first
            var finished = false
            var lastReport = 0.0
            while !finished {
                let flags = left == 0 ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
                try chunk.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
                    stream.pointee.src_ptr = src.bindMemory(to: UInt8.self).baseAddress ?? UnsafePointer(out)
                    stream.pointee.src_size = src.count
                    repeat {
                        stream.pointee.dst_ptr = out
                        stream.pointee.dst_size = outSize
                        let status = compression_stream_process(stream, flags)
                        if status == COMPRESSION_STATUS_ERROR { throw PackageError.corrupt("inflate") }
                        let produced = outSize - stream.pointee.dst_size
                        if produced > 0 { try sink(UnsafeRawBufferPointer(start: out, count: produced)) }
                        if status == COMPRESSION_STATUS_END { finished = true; return }
                    } while stream.pointee.src_size > 0 || stream.pointee.dst_size == 0
                }
                if finished { break }
                if left == 0 && chunk.isEmpty { throw PackageError.corrupt("payload truncated") }
                chunk = try read(1 << 20)
                let done = 1 - Double(left) / Double(max(count, 1))
                if done - lastReport > 0.01 { lastReport = done; progress?(done) }
            }
        }
    }

    // MARK: - cpio (odc) → PCMWave tar → files

    private final class CpioSink {
        let dest: URL
        let keep: (String) -> Bool
        var written: [String] = []
        var finished = false

        private var header = Data()
        private var name = Data()
        private var nameLeft = 0
        private var dataLeft: UInt64 = 0
        private var tar: TarSink?
        private enum State { case header, name, data }
        private var state = State.header

        init(dest: URL, keep: @escaping (String) -> Bool) { self.dest = dest; self.keep = keep }

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
                              let ns = Int(String(decoding: header[59..<65], as: UTF8.self), radix: 8),
                              let fs = UInt64(String(decoding: header[65..<76], as: UTF8.self), radix: 8)
                        else { throw PackageError.corrupt("cpio header") }
                        nameLeft = ns
                        dataLeft = fs
                        name = Data()
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
                        tar = nil
                        if path.hasSuffix(".SpeechVoice/Contents/Resources/PCMWave") {
                            let parts = path.split(separator: "/")
                            if let bundle = parts.first(where: { $0.hasSuffix(".SpeechVoice") }) {
                                let voice = String(bundle.dropLast(".SpeechVoice".count))
                                if !voice.isEmpty, !voice.contains(".."), keep(voice) {
                                    let dir = dest.appendingPathComponent(voice)
                                    try? FileManager.default.removeItem(at: dir)
                                    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                                    tar = TarSink(dir: dir)
                                    written.append(voice)
                                }
                            }
                        }
                        header = Data()
                        state = dataLeft == 0 ? .header : .data
                    }
                case .data:
                    let take = Int(min(dataLeft, UInt64(buf.count - i)))
                    if let tar { try tar.feed(UnsafeRawBufferPointer(rebasing: buf[i..<(i + take)])) }
                    i += take
                    dataLeft -= UInt64(take)
                    if dataLeft == 0 { try tar?.close(); tar = nil; state = .header }
                }
            }
        }
    }

    /// ustar member names are ROT13-encoded; only regular files are kept,
    /// flattened into `dir`.
    private final class TarSink {
        let dir: URL
        private var block = Data()
        private var fileLeft: UInt64 = 0
        private var pad = 0
        private var out: FileHandle?
        private var ended = false

        init(dir: URL) { self.dir = dir }

        func feed(_ buf: UnsafeRawBufferPointer) throws {
            var i = 0
            while i < buf.count {
                if ended { return }
                if fileLeft > 0 {
                    let take = Int(min(fileLeft, UInt64(buf.count - i)))
                    if let out { try out.write(contentsOf: Data(UnsafeRawBufferPointer(rebasing: buf[i..<(i + take)]))) }
                    i += take
                    fileLeft -= UInt64(take)
                    if fileLeft == 0 { try out?.close(); out = nil }
                    continue
                }
                if pad > 0 {
                    let take = min(pad, buf.count - i)
                    i += take
                    pad -= take
                    continue
                }
                let take = min(512 - block.count, buf.count - i)
                block.append(buf.baseAddress!.advanced(by: i).assumingMemoryBound(to: UInt8.self), count: take)
                i += take
                guard block.count == 512 else { continue }
                defer { block = Data() }
                if block.allSatisfy({ $0 == 0 }) { ended = true; return }
                let rawName = String(decoding: block[0..<100].prefix { $0 != 0 }, as: UTF8.self)
                let sizeField = String(decoding: block[124..<136].prefix { $0 != 0 }, as: UTF8.self)
                    .trimmingCharacters(in: .whitespaces)
                guard let size = UInt64(sizeField, radix: 8) else { throw PackageError.corrupt("voice data header") }
                let type = block[156]
                fileLeft = size
                pad = Int((512 - size % 512) % 512)
                if (type == 0x30 || type == 0) && size > 0 {
                    let base = rot13((rawName as NSString).lastPathComponent)
                    guard !base.isEmpty, base != ".", base != "..", !base.contains("/") else { throw PackageError.corrupt("voice file name") }
                    let url = dir.appendingPathComponent(base)
                    FileManager.default.createFile(atPath: url.path, contents: nil)
                    out = try FileHandle(forWritingTo: url)
                } else if size == 0 {
                    pad = 0
                }
            }
        }

        func close() throws {
            try out?.close()
            out = nil
            if fileLeft > 0 { throw PackageError.corrupt("voice data truncated") }
        }

        private func rot13(_ s: String) -> String {
            String(s.unicodeScalars.map { c -> Character in
                switch c.value {
                case 65...90: return Character(UnicodeScalar((c.value - 65 + 13) % 26 + 65)!)
                case 97...122: return Character(UnicodeScalar((c.value - 97 + 13) % 26 + 97)!)
                default: return Character(c)
                }
            })
        }
    }
}

extension Data {
    func be16(_ o: Int) -> UInt16 { UInt16(self[startIndex + o]) << 8 | UInt16(self[startIndex + o + 1]) }
    func be32(_ o: Int) -> UInt32 { UInt32(be16(o)) << 16 | UInt32(be16(o + 2)) }
    func be64(_ o: Int) -> UInt64 { UInt64(be32(o)) << 32 | UInt64(be32(o + 4)) }
}
