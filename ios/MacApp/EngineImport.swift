import CommonCrypto
import Foundation

/// The released Mac app carries nothing of Apple's, so it fetches the engines
/// and voices itself, as the NVDA add-on and the Windows and Linux apps do
/// (nvda/.../remote.py, firmware.py): only the root filesystem of each iPhone
/// firmware is downloaded from Apple, inflated on the way, decrypted with the
/// key The Apple Wiki publishes, and the speech engine and its voices are
/// copied out of the HFS+ volume into the App Group (VoiceCatalog.importedRoot).
enum EngineImport {
    struct Firmware {
        let build: String     // "ios6"
        let number: String    // "10B500"
        let device: String    // "iPhone2,1"
        let name: String      // "iOS 6.1.6"
        let url: String
    }

    /// Apple's own links for the firmware Classic Voices runs (remote.py FIRMWARE).
    /// 7.1.2 before 7.0.6: the Mavericks voices' lid.dat is 7.1.2's when both are
    /// here (assembleMacCommon takes the first imported), as the bundled app has it.
    static let firmware: [Firmware] = [
        Firmware(build: "ios3", number: "7E18", device: "iPhone2,1", name: "iPhone OS 3.1.3",
                 url: "https://secure-appldnld.apple.com/iPhone/061-7472.20100202.8tugj/iPhone2,1_3.1.3_7E18_Restore.ipsw"),
        Firmware(build: "ios4", number: "8L1", device: "iPhone2,1", name: "iOS 4.3.5",
                 url: "https://secure-appldnld.apple.com/iPhone4/041-1965.20110721.gxUB5/iPhone2,1_4.3.5_8L1_Restore.ipsw"),
        Firmware(build: "ios5", number: "9B206", device: "iPhone2,1", name: "iOS 5.1.1",
                 url: "https://secure-appldnld.apple.com/iOS5.1.1/041-4347.20120427.o2yov/iPhone2,1_5.1.1_9B206_Restore.ipsw"),
        Firmware(build: "ios6", number: "10B500", device: "iPhone2,1", name: "iOS 6.1.6",
                 url: "https://secure-appldnld.apple.com/iOS6.1/091-3457.20140221.Btt3e/iPhone2,1_6.1.6_10B500_Restore.ipsw"),
        Firmware(build: "ios7", number: "11D257", device: "iPhone3,1", name: "iOS 7.1.2",
                 url: "https://secure-appldnld.apple.com/iOS7.1/031-4812.20140627.cq6y8/iPhone3,1_7.1.2_11D257_Restore.ipsw"),
        Firmware(build: "ios70", number: "11B651", device: "iPhone3,1", name: "iOS 7.0.6",
                 url: "https://secure-appldnld.apple.com/iOS7/031-3020.20140221.8j5GW/iPhone3,1_7.0.6_11B651_Restore.ipsw"),
        Firmware(build: "ios10", number: "14G61", device: "iPhone5,1", name: "iOS 10.3.4",
                 url: "https://updates.cdn-apple.com/2019/ios/091-25277-20190722-0C1B94DE-992C-11E9-A2EE-E2C9A77C2E40/iPhone_4.0_32bit_10.3.4_14G61_Restore.ipsw"),
    ]

    /// Mac OS X voice eras' common data, assembled from firmware files
    /// (firmware.py MAC_COMMON, tools/fetch_firmware.sh): each file from the
    /// first of its builds that's here.
    static let macCommon: [(era: String, files: [(builds: [String], file: String)])] = [
        ("mac107", [(["ios5"], "lid.dat"), (["ios6"], "synth_med_fxd_bet2f22.dat")]),
        ("mac108", [(["ios6"], "lid.dat"), (["ios6"], "synth_med_fxd_bet2f22.dat"), (["ios7", "ios70"], "synth_med_fxd_bet3f22.dat")]),
        ("mac109", [(["ios7", "ios70"], "lid.dat"), (["ios7", "ios70"], "synth_med_fxd_bet3f22.dat")]),
    ]

    struct ImportError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    typealias Progress = (_ message: String?, _ fraction: Double?) -> Void

    static func isImported(_ build: String, in root: URL) -> Bool {
        FileManager.default.fileExists(atPath: root.appendingPathComponent(build).appendingPathComponent("voiced").path)
    }

    /// Streams one firmware's root filesystem from Apple and imports its engine
    /// and voices into root/<build>. Blocks; call it off the main thread.
    static func importFromApple(_ fw: Firmware, into root: URL, progress: @escaping Progress) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        let tmp = root.appendingPathComponent(".rootfs-\(fw.build).dmg")
        defer { try? fm.removeItem(at: tmp) }
        let name = try streamRootFS(fw, to: tmp, progress: progress)
        try importRootFS(tmp, name: name, fw, into: root, progress: progress)
        assembleMacCommon(root)
        progress("\(fw.name) imported", 1)
    }

    // MARK: - the root filesystem, out of the IPSW on Apple's server

    private struct Member { let name: String; let method: UInt16; let compressed: UInt64; let headerAt: UInt64; let size: UInt64 }

    /// The largest disk image in a remote zip (the root filesystem), from its
    /// central directory read with range requests.
    private static func rootFSMember(_ url: URL) throws -> Member {
        let (_, total) = try HTTP.range(url, 0, 0)
        guard total > 0 else { throw ImportError("Apple's server didn't say how big the firmware is.") }
        let tailAt = total > 65536 ? total - 65536 : 0
        let (tail, _) = try HTTP.range(url, tailAt, total - 1)
        guard let i = tail.lastRange(of: Data([0x50, 0x4B, 0x05, 0x06])).map({ $0.lowerBound - tail.startIndex }) else {
            throw ImportError("The firmware on Apple's server isn't a zip.")
        }
        var cdSize = UInt64(tail.le32(i + 12)), cdOffset = UInt64(tail.le32(i + 16))
        if cdOffset == 0xFFFF_FFFF, let j = tail.lastRange(of: Data([0x50, 0x4B, 0x06, 0x07])).map({ $0.lowerBound - tail.startIndex }) {
            let off64 = tail.le64(j + 8)
            let (z, _) = try HTTP.range(url, off64, off64 + 55)
            cdSize = z.le64(40); cdOffset = z.le64(48)
        }
        let cd: Data
        if cdOffset >= tailAt {
            let a = Int(cdOffset - tailAt)
            cd = tail.subdata(in: tail.startIndex + a ..< tail.startIndex + a + Int(cdSize))
        } else {
            cd = try HTTP.range(url, cdOffset, cdOffset + cdSize - 1).0
        }
        var best: Member?
        var p = 0
        while p + 46 <= cd.count, cd.le32(p) == 0x0201_4B50 {
            let method = UInt16(cd.le32(p + 10) & 0xFFFF)
            var csz = UInt64(cd.le32(p + 20)), usz = UInt64(cd.le32(p + 24))
            let nl = Int(cd.le32(p + 28) & 0xFFFF), el = Int(cd.le32(p + 30) & 0xFFFF), cl = Int(cd.le32(p + 32) & 0xFFFF)
            var lho = UInt64(cd.le32(p + 42))
            let name = String(decoding: cd.subdata(in: cd.startIndex + p + 46 ..< cd.startIndex + p + 46 + nl), as: UTF8.self)
            if csz == 0xFFFF_FFFF || usz == 0xFFFF_FFFF || lho == 0xFFFF_FFFF {  // zip64 extra field
                var q = p + 46 + nl
                let end = q + el
                while q + 4 <= end {
                    let id = cd.le32(q) & 0xFFFF, len = Int(cd.le32(q) >> 16)
                    if id == 1 {
                        var v = q + 4
                        if usz == 0xFFFF_FFFF { usz = cd.le64(v); v += 8 }
                        if csz == 0xFFFF_FFFF { csz = cd.le64(v); v += 8 }
                        if lho == 0xFFFF_FFFF { lho = cd.le64(v) }
                    }
                    q += 4 + len
                }
            }
            if name.lowercased().hasSuffix(".dmg"), usz > (best?.size ?? 0) {
                best = Member(name: name, method: method, compressed: csz, headerAt: lho, size: usz)
            }
            p += 46 + nl + el + cl
        }
        guard let best else { throw ImportError("The firmware has no disk images.") }
        return best
    }

    /// Downloads only the root filesystem, inflating it on the way into `dest`.
    /// Returns its name inside the IPSW.
    private static func streamRootFS(_ fw: Firmware, to dest: URL, progress: @escaping Progress) throws -> String {
        guard let url = URL(string: fw.url) else { throw ImportError("Bad firmware link.") }
        progress("Finding \(fw.name) on Apple's server", nil)
        let m = try rootFSMember(url)
        let (h, _) = try HTTP.range(url, m.headerAt, m.headerAt + 29)
        let start = m.headerAt + 30 + UInt64(h.le32(26) & 0xFFFF) + UInt64(h.le32(26) >> 16)
        FileManager.default.createFile(atPath: dest.path, contents: nil)
        let out = try FileHandle(forWritingTo: dest)
        defer { try? out.close() }
        let inflater = m.method == 8 ? try Inflater() : nil
        progress("Downloading \(fw.name) (\(m.compressed >> 20) MB)", 0)
        // Apple's server gives each connection a few MB/s, so the image comes
        // in parts over several at once; they're inflated in order as they land.
        let got = try ParallelFetch.run(url, from: start, count: m.compressed) { part in
            try out.write(contentsOf: inflater.map { try $0.inflate(part) } ?? part)
        } progress: { done in
            progress(nil, Double(done) / Double(max(1, m.compressed)))
        }
        guard got == m.compressed else { throw ImportError("The download of \(fw.name) ended early.") }
        return (m.name as NSString).lastPathComponent
    }

    // MARK: - out of the root filesystem

    private static func importRootFS(_ image: URL, name: String, _ fw: Firmware, into root: URL, progress: Progress) throws {
        var src: RandomAccess = try FileSource(image)
        if try src.read(at: 0, count: 8) == Data("encrcdsa".utf8) {
            progress("Getting the \(fw.name) key from The Apple Wiki", nil)
            let key = try AppleWiki.rootFSKey(build: fw.number, device: fw.device, rootFS: name)
            src = try EncrcdsaSource(src, key: key)
        }
        let disk = try DiskImage(src)
        guard let at = try disk.hfsVolumes().first else { throw ImportError("\(fw.name)'s root filesystem has no HFS+ volume.") }
        let vol = try HFSVolume(disk, at: at)
        let fm = FileManager.default
        let dest = root.appendingPathComponent(fw.build)
        let part = root.appendingPathComponent(fw.build + ".part")
        try? fm.removeItem(at: part)
        progress("Copying the \(fw.name) engine and voices", nil)
        if fw.build == "ios10" {
            let bundle = "System/Library/TTSPlugins/TTSSpeechBundle.speechbundle/"
            let files = try vol.files { $0.hasPrefix(bundle) }
            guard let engine = files.first(where: { $0.path == bundle + "TTSSpeechBundle" }) else {
                throw ImportError("The iOS 10 speech engine isn't in the firmware.")
            }
            try copy(vol, engine, to: part.appendingPathComponent("voiced"))
            // Apple's IPA -> L&H+ maps, for phonemes (core/vxphonemes.c)
            for f in files {
                let n = String(f.path.dropFirst(bundle.count))
                if !n.contains("/"), n.hasPrefix("phonemeMap-"), n.hasSuffix(".json") {
                    try copy(vol, f, to: part.appendingPathComponent("phonemeMaps").appendingPathComponent(n))
                }
            }
        } else {
            let fwDir = "System/Library/PrivateFrameworks/VoiceServices.framework/"
            let res = fwDir + "TTSResources/"
            let files = try vol.files { $0 == fwDir + "Support/voiced" || $0.hasPrefix(res) }
            guard let voiced = files.first(where: { $0.path == fwDir + "Support/voiced" }) else {
                throw ImportError("\(fw.name)'s speech engine isn't in the firmware.")
            }
            try copy(vol, voiced, to: part.appendingPathComponent("voiced"))
            let voices = files.filter { $0.path.hasPrefix(res) }
            for (n, f) in voices.enumerated() {
                let rel = String(f.path.dropFirst(res.count))
                // iOS 7.1.2 is only the engine for the Siri and Mavericks voices
                if fw.build == "ios7", !rel.hasPrefix("common/") { continue }
                if rel.split(separator: "/").contains(where: { $0.hasPrefix(".") || $0 == ".." }) { continue }
                try copy(vol, f, to: part.appendingPathComponent("TTSResources").appendingPathComponent(rel))
                progress(nil, Double(n + 1) / Double(voices.count))
            }
        }
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: part, to: dest)
    }

    private static func copy(_ vol: HFSVolume, _ f: HFSVolume.File, to dest: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let packed = try vol.decompressed(f) {
            try packed.write(to: dest)
            return
        }
        fm.createFile(atPath: dest.path, contents: nil)
        let out = try FileHandle(forWritingTo: dest)
        defer { try? out.close() }
        var at: UInt64 = 0
        while at < f.fork.size {
            let d = try vol.read(f.fork, at: at, count: Int(min(f.fork.size - at, 8 << 20)))
            guard !d.isEmpty else { throw ImportError("\(f.path) is cut short in the firmware.") }
            try out.write(contentsOf: d)
            at += UInt64(d.count)
        }
    }

    /// <root>/mac10x/common from firmware files, where they're all here.
    static func assembleMacCommon(_ root: URL) {
        let fm = FileManager.default
        for (era, files) in macCommon {
            let sources = files.map { want in
                want.builds.lazy.map { root.appendingPathComponent("\($0)/TTSResources/common/\(want.file)") }
                    .first { fm.fileExists(atPath: $0.path) }
            }
            guard sources.allSatisfy({ $0 != nil }) else { continue }
            let out = root.appendingPathComponent(era).appendingPathComponent("common")
            try? fm.createDirectory(at: out, withIntermediateDirectories: true)
            for (src, want) in zip(sources, files) {
                let dest = out.appendingPathComponent(want.file)
                if !fm.fileExists(atPath: dest.path) { try? fm.copyItem(at: src!, to: dest) }
            }
        }
    }
}

// MARK: - decryption

/// The plaintext of an encrcdsa (v2) image, decrypting only the chunks read:
/// AES-128-CBC per chunk, the IV an HMAC-SHA1 of the chunk number.
final class EncrcdsaSource: RandomAccess {
    private let src: RandomAccess
    private let blockSize: UInt64
    private let dataOffset: UInt64
    let size: UInt64
    private let aesKey: Data
    private let hmacKey: Data
    private var cache: [UInt64: Data] = [:]

    init(_ src: RandomAccess, key hex: String) throws {
        self.src = src
        let hdr = try src.read(at: 0, count: 72)
        guard hdr.count == 72, hdr.prefix(8) == Data("encrcdsa".utf8) else { throw EngineImport.ImportError("Not an encrypted image.") }
        blockSize = UInt64(hdr.be32(52))
        size = hdr.be64(56)
        dataOffset = hdr.be64(64)
        var key = Data()
        var it = hex.makeIterator()
        while let a = it.next(), let b = it.next(), let v = UInt8(String([a, b]), radix: 16) { key.append(v) }
        guard key.count >= 36, blockSize > 0 else { throw EngineImport.ImportError("The Apple Wiki's key doesn't fit this firmware.") }
        aesKey = key.prefix(16)
        hmacKey = key.subdata(in: 16 ..< 36)
    }

    private func chunk(_ n: UInt64) throws -> Data {
        if let c = cache[n] { return c }
        let data = try src.read(at: dataOffset + n * blockSize, count: Int(blockSize))
        var number = UInt32(n).bigEndian
        var mac = [UInt8](repeating: 0, count: Int(CC_SHA1_DIGEST_LENGTH))
        hmacKey.withUnsafeBytes { k in CCHmac(CCHmacAlgorithm(kCCHmacAlgSHA1), k.baseAddress, k.count, &number, 4, &mac) }
        var out = Data(count: data.count)
        var moved = 0
        let status = out.withUnsafeMutableBytes { o in
            data.withUnsafeBytes { i in
                aesKey.withUnsafeBytes { k in
                    CCCrypt(CCOperation(kCCDecrypt), CCAlgorithm(kCCAlgorithmAES), 0, k.baseAddress, 16, mac,
                            i.baseAddress, data.count, o.baseAddress, data.count, &moved)
                }
            }
        }
        guard status == kCCSuccess else { throw EngineImport.ImportError("Decrypting the firmware failed.") }
        if cache.count > 256 { cache.removeAll() }
        cache[n] = out
        return out
    }

    func read(at offset: UInt64, count: Int) throws -> Data {
        var out = Data(capacity: count)
        var pos = offset
        let end = min(offset + UInt64(count), size)
        while pos < end {
            let (n, within) = pos.quotientAndRemainder(dividingBy: blockSize)
            let c = try chunk(n)
            let take = min(UInt64(c.count) - within, end - pos)
            guard take > 0 else { break }
            out.append(c.subdata(in: Int(within) ..< Int(within + take)))
            pos += take
        }
        return out
    }

    func sources(_ offset: UInt64, _ count: UInt64) -> [Range<UInt64>] { [offset ..< offset + count] }
    func prefetch(_ ranges: [Range<UInt64>]) {}
}

/// The root filesystem keys The Apple Wiki publishes (firmware.py fetch_rootfs_key).
enum AppleWiki {
    static let apis = ["https://theapplewiki.com/api.php", "https://theapplewiki.com/w/api.php"]

    private static func api(_ params: [String: String]) throws -> [String: Any] {
        var last: Error?
        for base in apis {
            var c = URLComponents(string: base)!
            c.queryItems = (params.merging(["format": "json"]) { a, _ in a }).map { URLQueryItem(name: $0.key, value: $0.value) }
            do {
                let (data, _) = try HTTP.get(c.url!)
                if let j = try JSONSerialization.jsonObject(with: data) as? [String: Any] { return j }
            } catch { last = error }
        }
        throw EngineImport.ImportError("The Apple Wiki can't be reached\(last.map { ": \($0.localizedDescription)" } ?? "").")
    }

    static func rootFSKey(build: String, device: String, rootFS: String) throws -> String {
        let si = try api(["action": "query", "meta": "siteinfo", "siprop": "namespaces"])
        let spaces = (si["query"] as? [String: Any])?["namespaces"] as? [String: [String: Any]] ?? [:]
        let ns = spaces.first { $0.value["canonical"] as? String == "Keys" || $0.value["*"] as? String == "Keys" }?.key ?? "0"
        let res = try api(["action": "query", "list": "search", "srsearch": "\(build) \(device)", "srnamespace": ns, "srlimit": "10"])
        let titles = (((res["query"] as? [String: Any])?["search"] as? [[String: Any]]) ?? []).compactMap { $0["title"] as? String }
        let flat = device.replacingOccurrences(of: " ", with: "")
        guard let title = titles.first(where: { $0.contains(build) && $0.replacingOccurrences(of: " ", with: "").contains(flat) }) ?? titles.first
        else { throw EngineImport.ImportError("The Apple Wiki has no key page for \(build).") }
        let parsed = try api(["action": "parse", "page": title, "prop": "text"])
        guard let html = ((parsed["parse"] as? [String: Any])?["text"] as? [String: Any])?["*"] as? String else {
            throw EngineImport.ImportError("The Apple Wiki's key page for \(build) can't be read.")
        }
        var text = html.replacingOccurrences(of: "<[^>]+>", with: " ", options: .regularExpression)
        text = text.replacingOccurrences(of: "&[a-zA-Z#0-9]+;", with: " ", options: .regularExpression)
        text = text.replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
        let segments = text.components(separatedBy: "Filename:").dropFirst()
        for seg in segments {
            guard let fn = seg.range(of: "\\S+\\.dmg", options: .regularExpression), String(seg[fn]) == rootFS,
                  let k = seg.range(of: "\\bKey\\b\\s*:?\\s*[0-9a-fA-F]{64,80}", options: .regularExpression),
                  let hex = String(seg[k]).range(of: "[0-9a-fA-F]{64,80}$", options: .regularExpression)
            else { continue }
            return String(String(seg[k])[hex]).lowercased()
        }
        // any 72-digit key on the page (the root filesystem's is the long one)
        if let k = text.range(of: "\\b[0-9a-fA-F]{72}\\b", options: .regularExpression) { return String(text[k]).lowercased() }
        throw EngineImport.ImportError("The Apple Wiki has no key for \(rootFS).")
    }
}

// MARK: - inflating

/// Raw deflate, as zip members are stored.
final class Inflater {
    private var z = z_stream()

    init() throws {
        guard inflateInit2_(&z, -15, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            throw EngineImport.ImportError("zlib couldn't start.")
        }
    }

    deinit { inflateEnd(&z) }

    func inflate(_ input: Data) throws -> Data {
        var out = Data()
        var buf = [UInt8](repeating: 0, count: 1 << 20)
        try input.withUnsafeBytes { (i: UnsafeRawBufferPointer) in
            z.next_in = UnsafeMutablePointer(mutating: i.bindMemory(to: Bytef.self).baseAddress)
            z.avail_in = uInt(input.count)
            repeat {
                let rc = buf.withUnsafeMutableBufferPointer { b -> Int32 in
                    z.next_out = b.baseAddress
                    z.avail_out = uInt(b.count)
                    return zlib.inflate(&z, Z_NO_FLUSH)
                }
                guard rc == Z_OK || rc == Z_STREAM_END || rc == Z_BUF_ERROR else {
                    throw EngineImport.ImportError("The firmware download is damaged.")
                }
                out.append(contentsOf: buf[0 ..< buf.count - Int(z.avail_out)])
                if rc == Z_STREAM_END { break }
            } while z.avail_in > 0 || z.avail_out == 0
        }
        return out
    }
}

/// One big range of a file, fetched in parts over several connections, handed
/// to `body` in order. A part whose connection drops is asked for again.
enum ParallelFetch {
    static let connections = 6
    static let partSize: UInt64 = 16 << 20

    static func run(_ url: URL, from start: UInt64, count: UInt64, _ body: (Data) throws -> Void,
                    progress: (UInt64) -> Void) throws -> UInt64 {
        let parts = Int((count + partSize - 1) / partSize)
        let lock = NSCondition()
        var ready: [Int: Data] = [:]
        var next = 0          // the next part a worker takes
        var consumed = 0      // parts handed to body
        var failure: Error?
        var cancelled = false
        let window = connections * 2  // parts ahead of the one being inflated, at most (~200 MB)
        let group = DispatchGroup()
        for _ in 0..<connections {
            group.enter()
            Thread.detachNewThread {
                defer { group.leave() }
                while true {
                    lock.lock()
                    while !cancelled && next < parts && next - consumed >= window { lock.wait() }
                    if cancelled || next >= parts { lock.unlock(); return }
                    let i = next
                    next += 1
                    lock.unlock()
                    let a = start + UInt64(i) * partSize
                    let b = min(a + partSize, start + count) - 1
                    var data: Data?
                    var lastError: Error?
                    for attempt in 0..<8 where data == nil {
                        do {
                            let (d, _) = try HTTP.range(url, a, b)
                            if UInt64(d.count) == b - a + 1 { data = d } else { lastError = EngineImport.ImportError("A part of the download was cut short.") }
                        } catch { lastError = error }
                        if data == nil { Thread.sleep(forTimeInterval: Double(attempt + 1) * 2) }
                        lock.lock(); let stop = cancelled; lock.unlock()
                        if stop { return }
                    }
                    lock.lock()
                    if let data { ready[i] = data } else { failure = failure ?? lastError; cancelled = true }
                    lock.broadcast()
                    lock.unlock()
                }
            }
        }
        var done: UInt64 = 0
        defer {
            lock.lock(); cancelled = true; lock.broadcast(); lock.unlock()
            group.wait()
        }
        for i in 0..<parts {
            lock.lock()
            while ready[i] == nil && failure == nil { lock.wait() }
            if let failure { lock.unlock(); throw failure }
            let d = ready.removeValue(forKey: i)!
            lock.unlock()
            try body(d)
            done += UInt64(d.count)
            progress(done)
            lock.lock(); consumed = i + 1; lock.broadcast(); lock.unlock()
        }
        return done
    }
}

// MARK: - HTTP

/// Blocking requests (the import runs on its own thread).
enum HTTP {
    static let session: URLSession = {
        let c = URLSessionConfiguration.default
        c.timeoutIntervalForRequest = 120
        return URLSession(configuration: c)
    }()

    static func get(_ url: URL, headers: [String: String] = [:]) throws -> (Data, HTTPURLResponse) {
        var req = URLRequest(url: url)
        req.setValue("ClassicVoices/\(UpdateCheck.current)", forHTTPHeaderField: "User-Agent")
        for (k, v) in headers { req.setValue(v, forHTTPHeaderField: k) }
        let done = DispatchSemaphore(value: 0)
        var result: Result<(Data, HTTPURLResponse), Error> = .failure(EngineImport.ImportError("No answer."))
        session.dataTask(with: req) { data, response, error in
            if let error { result = .failure(error) }
            else if let data, let r = response as? HTTPURLResponse, (200..<300).contains(r.statusCode) { result = .success((data, r)) }
            else { result = .failure(EngineImport.ImportError("The server answered \((response as? HTTPURLResponse)?.statusCode ?? 0) for \(url.lastPathComponent).")) }
            done.signal()
        }.resume()
        done.wait()
        return try result.get()
    }

    /// Bytes a...b of url, and the file's whole size.
    static func range(_ url: URL, _ a: UInt64, _ b: UInt64) throws -> (Data, UInt64) {
        let (data, r) = try get(url, headers: ["Range": "bytes=\(a)-\(b)"])
        let total = (r.value(forHTTPHeaderField: "Content-Range")?.split(separator: "/").last).flatMap { UInt64($0) } ?? 0
        return (data, total)
    }

    /// Bytes a...b of url, handed to `body` as they arrive.
    static func stream(_ url: URL, from a: UInt64, to b: UInt64, _ body: @escaping (Data) throws -> Void) throws {
        let d = StreamDelegate(body)
        let s = URLSession(configuration: .default, delegate: d, delegateQueue: nil)
        defer { s.finishTasksAndInvalidate() }
        var req = URLRequest(url: url, timeoutInterval: 120)
        req.setValue("ClassicVoices/\(UpdateCheck.current)", forHTTPHeaderField: "User-Agent")
        req.setValue("bytes=\(a)-\(b)", forHTTPHeaderField: "Range")
        s.dataTask(with: req).resume()
        d.done.wait()
        if let e = d.error { throw e }
    }

    private final class StreamDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
        let body: (Data) throws -> Void
        let done = DispatchSemaphore(value: 0)
        var error: Error?
        init(_ body: @escaping (Data) throws -> Void) { self.body = body }

        func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                        completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            if code != 206 && code != 200 {
                error = EngineImport.ImportError("Apple's server answered \(code).")
                completionHandler(.cancel)
            } else {
                completionHandler(.allow)
            }
        }

        func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
            guard error == nil else { return }
            do { try body(data) } catch { self.error = error; dataTask.cancel() }
        }

        func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError e: Error?) {
            if error == nil, let e { error = e }
            done.signal()
        }
    }
}
