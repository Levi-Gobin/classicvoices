import Compression
import Foundation

/// Minimal streaming ZIP reader (stored + deflate), enough for Apple's
/// MobileAsset voice archives. Large entries are inflated in chunks.
enum ZipExtractor {
    enum ZipError: LocalizedError {
        case notZip, unsupported(UInt16), corrupt(String)
        var errorDescription: String? {
            switch self {
            case .notZip: return "Not a zip archive"
            case .unsupported(let m): return "Unsupported zip compression method \(m)"
            case .corrupt(let what): return "Zip archive problem: \(what)"
            }
        }
    }

    static func extract(_ zip: URL, to dest: URL) throws {
        let fh = try FileHandle(forReadingFrom: zip)
        defer { try? fh.close() }
        let fileSize = try fh.seekToEnd()

        // End of central directory: last 22..65557 bytes
        let tailLen = min(fileSize, 65_557)
        try fh.seek(toOffset: fileSize - tailLen)
        let tail = try fh.read(upToCount: Int(tailLen)) ?? Data()
        guard let eocd = tail.lastRange(of: Data([0x50, 0x4B, 0x05, 0x06]))?.lowerBound else { throw ZipError.notZip }
        let count = Int(tail.u16(eocd + 10))
        let cdOffset = UInt64(tail.u32(eocd + 16))
        let cdSize = Int(tail.u32(eocd + 12))
        try fh.seek(toOffset: cdOffset)
        let cd = try fh.read(upToCount: cdSize) ?? Data()

        var p = 0
        for _ in 0..<count {
            guard cd.u32(p) == 0x0201_4B50 else { throw ZipError.corrupt("central directory") }
            let method = cd.u16(p + 10)
            let compSize = UInt64(cd.u32(p + 20))
            let nameLen = Int(cd.u16(p + 28)), extraLen = Int(cd.u16(p + 30)), commentLen = Int(cd.u16(p + 32))
            let localOffset = UInt64(cd.u32(p + 42))
            let name = String(decoding: cd[(p + 46)..<(p + 46 + nameLen)], as: UTF8.self)
            p += 46 + nameLen + extraLen + commentLen

            // Reject absolute names and ".." components (path-standardizing
            // can't be used: iOS drops "/private" only for paths that exist).
            let parts = name.split(separator: "/", omittingEmptySubsequences: true)
            guard !name.hasPrefix("/"), !parts.contains(".."), !parts.isEmpty else {
                throw ZipError.corrupt("entry escapes destination: \(name)")
            }
            let out = dest.appendingPathComponent(name)
            if name.hasSuffix("/") {
                try FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
                continue
            }
            try FileManager.default.createDirectory(at: out.deletingLastPathComponent(), withIntermediateDirectories: true)

            try fh.seek(toOffset: localOffset)
            let lh = try fh.read(upToCount: 30) ?? Data()
            guard lh.u32(0) == 0x0403_4B50 else { throw ZipError.corrupt("local header") }
            let dataStart = localOffset + 30 + UInt64(lh.u16(26)) + UInt64(lh.u16(28))
            try fh.seek(toOffset: dataStart)

            FileManager.default.createFile(atPath: out.path, contents: nil)
            let w = try FileHandle(forWritingTo: out)
            defer { try? w.close() }
            switch method {
            case 0:
                var left = compSize
                while left > 0 {
                    let chunk = try fh.read(upToCount: Int(min(left, 1 << 20))) ?? Data()
                    if chunk.isEmpty { throw ZipError.corrupt("truncated") }
                    try w.write(contentsOf: chunk)
                    left -= UInt64(chunk.count)
                }
            case 8:
                try inflate(from: fh, count: compSize, to: w)
            default:
                throw ZipError.unsupported(method)
            }
        }
    }

    /// Raw DEFLATE (Compression's COMPRESSION_ZLIB) streamed from `fh` to `w`.
    private static func inflate(from fh: FileHandle, count: UInt64, to w: FileHandle) throws {
        let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
        defer { stream.deallocate() }
        guard compression_stream_init(stream, COMPRESSION_STREAM_DECODE, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else {
            throw ZipError.corrupt("inflate init")
        }
        defer { compression_stream_destroy(stream) }
        let outSize = 1 << 20
        let outBuf = UnsafeMutablePointer<UInt8>.allocate(capacity: outSize)
        defer { outBuf.deallocate() }
        var left = count
        var finished = false
        while !finished {
            let chunk = left > 0 ? (try fh.read(upToCount: Int(min(left, 1 << 20))) ?? Data()) : Data()
            left -= UInt64(chunk.count)
            let flags = left == 0 ? Int32(COMPRESSION_STREAM_FINALIZE.rawValue) : 0
            try chunk.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
                stream.pointee.src_ptr = src.bindMemory(to: UInt8.self).baseAddress ?? UnsafePointer(outBuf)
                stream.pointee.src_size = chunk.count
                repeat {
                    stream.pointee.dst_ptr = outBuf
                    stream.pointee.dst_size = outSize
                    let status = compression_stream_process(stream, flags)
                    let produced = outSize - stream.pointee.dst_size
                    if produced > 0 { try w.write(contentsOf: Data(bytes: outBuf, count: produced)) }
                    if status == COMPRESSION_STATUS_END { finished = true; return }
                    if status == COMPRESSION_STATUS_ERROR { throw ZipError.corrupt("inflate") }
                } while stream.pointee.src_size > 0 || stream.pointee.dst_size == 0
            }
            if left == 0 && chunk.isEmpty && !finished { throw ZipError.corrupt("inflate: unexpected end") }
        }
    }
}

private extension Data {
    func u16(_ o: Int) -> UInt16 { UInt16(self[startIndex + o]) | UInt16(self[startIndex + o + 1]) << 8 }
    func u32(_ o: Int) -> UInt32 { UInt32(u16(o)) | UInt32(u16(o + 2)) << 16 }
}
