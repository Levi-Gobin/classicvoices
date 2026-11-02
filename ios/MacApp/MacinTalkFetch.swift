import CommonCrypto
import Foundation

/// The released Mac app carries nothing of Apple's, so it fetches the MacinTalk
/// engines and voices of Mac OS X 10.4 to 10.7 itself, as the build does with
/// tools/fetch_old_macintalk.py (Tiger, Leopard, Snow Leopard, from install
/// discs kept on archive.org) and tools/fetch_macintalk.sh (Lion, from the
/// installer Apple still offers). The result is laid out as extracted/<gen>/:
///
///     MacinTalk                      the engine
///     SpeechDictionary/...           SpeechDictionary.framework/Versions/A
///     lib/*.dylib                    the C++ libraries it loads (not Tiger)
///     Voices/<Name>.SpeechVoice/...  the MacinTalk voices, without Alex
///                                    (DiscDownloads gets him on request)
///
/// No disc is downloaded whole, and nothing is mounted: the discs are read in
/// place with HTTP range requests (DiskImage, HFSVolume): 1.3 to 2.5 GB of
/// discs of 4.7 to 8 GB. Tiger is an installed disk inside a .tar.bz2, which can't be read in
/// the middle, so it's streamed once and only the parts of the disk the
/// speech files are in are kept. Every file is checked against the hashes of
/// the files the fast engine code was made for (tools/profiles/oldmt.sha256,
/// and for Lion the same of extracted/mt107), which are copied in below.
enum MacinTalkFetch {
    typealias Progress = (_ message: String?, _ fraction: Double?) -> Void

    struct FetchError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    private struct Release {
        let gen: String
        let name: String       // "Snow Leopard"
        let url: String
        let lib: [String]      // usr/lib files the engine loads
        let download: UInt64   // about how much is downloaded, for the progress
        var packages: [String] = []   // on the disc, with the rest of the voices
        var server: String { url.contains("archive.org") ? "archive.org" : "Apple\u{2019}s server" }
    }

    private static let releases: [Release] = [
        Release(gen: "mt104", name: "Tiger", url: "https://archive.org/download/deadmoo-tiger-x86/tiger-x86.tar.bz2",
                lib: [], download: 1_383_030_913),
        Release(gen: "mt105", name: "Leopard",
                url: "https://archive.org/download/Mac_OS_X_Leopard_Install_DVD_Version_10.5_2Z691-6037-A_Apple_Inc._2007/"
                    + "Mac%20OS%20X%20Leopard%20Install%20DVD%20Version%2010.5%20%282Z691-6037-A%29%20%28Apple%2C%20Inc.%29%282007%29.iso",
                lib: ["libstdc++.6.0.4.dylib"], download: 1_700_000_000, packages: ["AdditionalSpeechVoices.pkg", "Essentials.pkg"]),
        Release(gen: "mt106", name: "Snow Leopard",
                url: "https://archive.org/download/MacOSX10.6-Snow-Leopard-Retail-2Z691-6428-A/Mac%20OS%20X%20Install%20DVD.dmg",
                lib: ["libstdc++.6.0.9.dylib"], download: 1_350_000_000, packages: ["AdditionalSpeechVoices.pkg", "Essentials.pkg"]),
        Release(gen: "mt107", name: "Lion",
                url: "https://updates.cdn-apple.com/2021/macos/041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg",
                lib: ["libc++abi.dylib", "libstdc++.6.0.9.dylib"], download: 2_460_000_000),
    ]

    static let generations = releases.map(\.gen)

    /// Bytes the last fetch downloaded (for tests and the diagnostics).
    private(set) static var downloaded: UInt64 = 0

    private static let engine = "System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/Contents/MacOS/MacinTalk"
    private static let dictionary = "System/Library/PrivateFrameworks/SpeechDictionary.framework/Versions/A"
    private static let voices = "System/Library/Speech/Voices"
    private static let big: Set<String> = ["Alex.SpeechVoice", "Alex-sequoia.SpeechVoice"]

    /// Fetches one generation ("mt104"..."mt107") into root/<gen>/, laid out as
    /// extracted/<gen>/ — without Alex (the app downloads him separately).
    /// Blocks; progress(message or nil, fraction 0...1 or nil).
    static func fetch(_ gen: String, into root: URL, progress: @escaping (String?, Double?) -> Void) throws {
        guard let r = releases.first(where: { $0.gen == gen }) else { throw FetchError("There\u{2019}s no MacinTalk release called \(gen).") }
        let fm = FileManager.default
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        let part = root.appendingPathComponent(gen + ".part", isDirectory: true)
        let work = root.appendingPathComponent(gen + ".work", isDirectory: true)
        for u in [part, work] { try? fm.removeItem(at: u) }
        defer { for u in [part, work] { try? fm.removeItem(at: u) } }
        try fm.createDirectory(at: part.appendingPathComponent("Voices"), withIntermediateDirectories: true)
        try fm.createDirectory(at: work, withIntermediateDirectories: true)
        downloaded = 0
        let meter = Meter(progress)
        progress("Finding \(r.name)\u{2019}s MacinTalk on \(r.server)", nil)
        switch gen {
        case "mt104": try tiger(r, part, work, meter)
        case "mt107": try lion(r, part, work, meter)
        default: try disc(r, part, meter)
        }
        // MacinTalk voices only: a VoiceDescription says it is one
        let voiceDir = part.appendingPathComponent("Voices")
        for name in try fm.contentsOfDirectory(atPath: voiceDir.path) {
            let v = voiceDir.appendingPathComponent(name)
            if !fm.fileExists(atPath: v.appendingPathComponent("Contents/Resources/VoiceDescription").path) { try fm.removeItem(at: v) }
        }
        progress("Checking \(r.name)\u{2019}s MacinTalk", nil)
        try check(r, part)
        let dest = root.appendingPathComponent(gen, isDirectory: true)
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: part, to: dest)
        progress("\(r.name)\u{2019}s MacinTalk is ready", 1)
    }

    /// Progress in whole percents, only ever forwards.
    private final class Meter {
        let report: Progress
        private var shown = -1
        init(_ report: @escaping Progress) { self.report = report }
        func set(_ f: Double) {
            let p = min(99, max(0, Int(f * 100)))
            if p > shown { shown = p; report(nil, Double(p) / 100) }
        }
        func say(_ m: String) { report(m, shown >= 0 ? Double(shown) / 100 : 0) }
    }

    // MARK: - Leopard and Snow Leopard: install DVDs on archive.org

    private static func disc(_ r: Release, _ out: URL, _ meter: Meter) throws {
        let remote = try RemoteFile(URL(string: r.url)!)
        defer { remote.close(); downloaded = remote.downloaded }
        remote.onRead = { [unowned remote] in meter.set(Double(remote.downloaded) / Double(r.download)) }
        meter.say("Downloading \(r.name)\u{2019}s MacinTalk from \(r.server)")
        let vol = try volume(DiskImage(remote), r)
        // The disc's own system has the engine, its dictionary and library and
        // Fred and Vicki; then AdditionalSpeechVoices.pkg is Alex (skipped, but
        // he has to stream past) and another Vicki, the one the fast code was
        // made for, and Essentials.pkg has the rest, most of the way into it.
        let packages = r.packages.map { "System/Installation/Packages/" + $0 }
        let files = try vol.files { onDisc($0, r) || packages.contains($0) }
        try copySystem(vol, files.filter { !packages.contains($0.path) }, r, out)
        for p in packages {
            guard let f = files.first(where: { $0.path == p }) else { continue }
            meter.say("Reading \((p as NSString).lastPathComponent) from \(r.name)\u{2019}s disc")
            try packageVoices(vol, f, into: out.appendingPathComponent("Voices"))
            remote.dropReadAhead()
        }
    }

    /// The files fetch_old_macintalk.py copies off the disc's own system.
    private static func onDisc(_ p: String, _ r: Release) -> Bool {
        if p == engine || p.hasPrefix(dictionary + "/") { return true }
        if r.lib.contains(where: { p == "usr/lib/" + $0 }) { return true }
        guard p.hasPrefix(voices + "/") else { return false }
        let parts = p.dropFirst(voices.count + 1).split(separator: "/")
        return parts.count >= 2 && parts[0].hasSuffix(".SpeechVoice") && !big.contains(String(parts[0]))
    }

    /// Copies the engine, dictionary, libraries and voices from a volume's
    /// system (paths as in `onDisc`) into `out`.
    private static func copySystem(_ vol: HFSVolume, _ files: [HFSVolume.File], _ r: Release, _ out: URL) throws {
        guard files.contains(where: { $0.path == engine }) else { throw missing(r, "the MacinTalk engine") }
        guard files.contains(where: { $0.path == dictionary + "/SpeechDictionary" }) else { throw missing(r, "the speech dictionary") }
        for lib in r.lib where !files.contains(where: { $0.path == "usr/lib/" + lib }) { throw missing(r, lib) }
        // in the order they're on the disc, so reading ahead helps
        for f in files.sorted(by: { ($0.fork.extents.first?.start ?? 0) < ($1.fork.extents.first?.start ?? 0) }) {
            let rel: String
            if f.path == engine { rel = "MacinTalk" }
            else if f.path.hasPrefix(dictionary + "/") { rel = "SpeechDictionary/" + f.path.dropFirst(dictionary.count + 1) }
            else if f.path.hasPrefix("usr/lib/") { rel = "lib/" + f.path.dropFirst(8) }
            else { rel = "Voices/" + f.path.dropFirst(voices.count + 1) }
            guard safe(rel) else { continue }
            try copy(vol, f, to: out.appendingPathComponent(rel))
        }
    }

    private static func missing(_ r: Release, _ what: String) -> FetchError {
        FetchError("\(what.prefix(1).uppercased() + what.dropFirst()) isn\u{2019}t on \(r.name)\u{2019}s disc on \(r.server) any more.")
    }

    /// No names Windows-style tools or the layout can't hold, nothing outside.
    private static func safe(_ rel: String) -> Bool {
        let bad = Set("<>:\"|?*")
        return !rel.contains(where: { bad.contains($0) })
            && !rel.split(separator: "/", omittingEmptySubsequences: false).contains(where: { $0.isEmpty || $0 == "." || $0 == ".." })
    }

    /// One file of a volume: symbolic links as links, HFS+ compressed files
    /// expanded.
    private static func copy(_ vol: HFSVolume, _ f: HFSVolume.File, to dest: URL) throws {
        let fm = FileManager.default
        try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? fm.removeItem(at: dest)
        if f.mode & 0o170000 == 0o120000 {
            let target = try vol.read(f.fork, at: 0, count: Int(f.fork.size))
            try fm.createSymbolicLink(atPath: dest.path, withDestinationPath: String(decoding: target, as: UTF8.self))
            return
        }
        if let packed = try vol.decompressed(f) {
            try packed.write(to: dest)
            return
        }
        if f.fork.size > 1 << 20 { vol.source(f).prefetch([0 ..< f.fork.size]) }
        fm.createFile(atPath: dest.path, contents: nil)
        let out = try FileHandle(forWritingTo: dest)
        defer { try? out.close() }
        var at: UInt64 = 0
        while at < f.fork.size {
            let d = try vol.read(f.fork, at: at, count: Int(min(f.fork.size - at, 8 << 20)))
            guard !d.isEmpty else { throw FetchError("\(f.path) is cut short on the disc.") }
            try out.write(contentsOf: d)
            at += UInt64(d.count)
        }
    }

    /// The voices in a package (fetch_old_macintalk.py take_voices): its files
    /// are in path order, so it stops once it has gone past the voices folder.
    private static func packageVoices(_ vol: HFSVolume, _ pkg: HFSVolume.File, into out: URL) throws {
        let prefix = "./" + voices + "/"
        var inside = false
        let cpio = Cpio { path, mode in
            guard path.hasPrefix(prefix) else {
                if inside { throw Cpio.Stop() }
                return .skip
            }
            inside = true
            let rel = String(path.dropFirst(prefix.count))
            guard !rel.isEmpty, safe(rel), let first = rel.split(separator: "/").first, !big.contains(String(first)) else { return .skip }
            switch mode & 0o170000 {
            case 0o040000: return .folder(out.appendingPathComponent(rel))
            case 0o100000: return .file(out.appendingPathComponent(rel))
            default: return .skip
            }
        }
        try unpack(SourceReader(vol.source(pkg)), name: pkg.path, into: cpio)
    }

    /// Streams a flat package's Payload (gzip or bzip2'd cpio) into `cpio`.
    private static func unpack(_ r: ByteReader, name: String, into cpio: Cpio) throws {
        let length = try MacVoicePackage.seekToPayload(r)
        let start = try r.offset()
        let magic = try r.read(upToCount: 3) ?? Data()
        try r.seek(toOffset: start)
        do {
            if magic.starts(with: [0x1F, 0x8B]) {
                try InstallerPackage.Inflate.stream(from: r, count: length, into: cpio.feed)
            } else if magic == Data("BZh".utf8) {
                try InstallerPackage.Bunzip2.stream(from: r, count: length, into: cpio.feed)
            } else {
                throw FetchError("\((name as NSString).lastPathComponent) is packed in a way Classic Voices can\u{2019}t read.")
            }
        } catch is Cpio.Stop {}
        try cpio.finish()
    }

    /// The HFS+ volume of a disc: GUID, Apple or no partition map, or a PC
    /// one (Tiger's VMware disk).
    private static func volume(_ disk: DiskImage, _ r: Release) throws -> HFSVolume {
        if let base = try disk.hfsVolumes().first { return try HFSVolume(disk, at: base) }
        let mbr = try disk.read(at: 0, count: 512)
        if mbr.count == 512, mbr[mbr.startIndex + 510] == 0x55, mbr[mbr.startIndex + 511] == 0xAA {
            for k in 0..<4 where mbr[mbr.startIndex + 446 + 16 * k + 4] == 0xAF {
                let base = UInt64(mbr.le32(446 + 16 * k + 8)) * 512
                let sig = try disk.read(at: base + 1024, count: 2)
                if sig == Data("H+".utf8) || sig == Data("HX".utf8) { return try HFSVolume(disk, at: base) }
            }
        }
        throw FetchError("\(r.name)\u{2019}s disc on \(r.server) has no Mac volume on it.")
    }

    // MARK: - Tiger: an installed disk in a .tar.bz2 on archive.org

    private static let tigerMember = "tiger-x86/tiger-x86-flat.img"

    private static func tiger(_ r: Release, _ out: URL, _ work: URL, _ meter: Meter) throws {
        let remote = try RemoteFile(URL(string: r.url)!)
        defer { remote.close(); downloaded = remote.downloaded }
        meter.say("Downloading \(r.name)\u{2019}s MacinTalk from \(r.server) (\(remote.size >> 20) MB)")
        let img = work.appendingPathComponent("disk.img")
        var image: SparseImage?
        let tar = Tar { name, size in
            guard name == tigerMember else { return nil }
            let s = try SparseImage(img, size: size) { onDisc($0, r) }
            image = s
            return s.write
        }
        try bunzip(remote, into: tar.feed) { meter.set(0.97 * Double($0) / Double(remote.size)) }
        guard let image, tar.finished else { throw FetchError("\(tigerMember) isn\u{2019}t whole in \(r.url).") }
        try image.finish()
        remote.close()
        meter.say("Copying \(r.name)\u{2019}s MacinTalk")
        let vol = try volume(DiskImage(FileSource(img)), r)
        try copySystem(vol, try vol.files { onDisc($0, r) }, r, out)
        try? FileManager.default.removeItem(at: img)
    }

    /// Reads a .bz2 (one or more streams) front to back.
    private static func bunzip(_ src: RandomAccess, into sink: (UnsafeRawBufferPointer) throws -> Void,
                               progress: (UInt64) -> Void) throws {
        var bz = bz_stream()
        guard BZ2_bzDecompressInit(&bz, 0, 0) == BZ_OK else { throw FetchError("bzip2 couldn\u{2019}t start.") }
        defer { BZ2_bzDecompressEnd(&bz) }
        let outSize = 1 << 20
        let out = UnsafeMutablePointer<CChar>.allocate(capacity: outSize)
        defer { out.deallocate() }
        var pos: UInt64 = 0
        var ended = false
        do {
            while pos < src.size {
                let chunk = try src.read(at: pos, count: 1 << 20)
                guard !chunk.isEmpty else { break }
                pos += UInt64(chunk.count)
                progress(pos)
                try chunk.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                    bz.next_in = UnsafeMutablePointer(mutating: raw.bindMemory(to: CChar.self).baseAddress)
                    bz.avail_in = UInt32(raw.count)
                    var more = true
                    while more {
                        if ended {  // another stream follows, or padding
                            if bz.avail_in == 0 { break }
                            guard bz.avail_in < 3 || memcmp(bz.next_in, "BZh", 3) == 0 else { throw Cpio.Stop() }
                            let (next, left) = (bz.next_in, bz.avail_in)
                            BZ2_bzDecompressEnd(&bz)
                            guard BZ2_bzDecompressInit(&bz, 0, 0) == BZ_OK else { throw FetchError("bzip2 couldn\u{2019}t start.") }
                            (bz.next_in, bz.avail_in) = (next, left)
                            ended = false
                        }
                        bz.next_out = out
                        bz.avail_out = UInt32(outSize)
                        let rc = BZ2_bzDecompress(&bz)
                        guard rc == BZ_OK || rc == BZ_STREAM_END else { throw FetchError("The download is damaged (bzip2 \(rc)).") }
                        let produced = outSize - Int(bz.avail_out)
                        if produced > 0 { try sink(UnsafeRawBufferPointer(start: out, count: produced)) }
                        if rc == BZ_STREAM_END { ended = true; more = bz.avail_in > 0 }
                        else { more = bz.avail_in > 0 || bz.avail_out == 0 }
                    }
                }
            }
        } catch is Cpio.Stop { return }
        guard ended else { throw FetchError("The download ended early.") }
    }

    // MARK: - Lion: InstallMacOSX.dmg on Apple's server

    private static func lion(_ r: Release, _ out: URL, _ work: URL, _ meter: Meter) throws {
        let fm = FileManager.default
        let remote = try RemoteFile(URL(string: r.url)!)
        defer { remote.close(); downloaded = remote.downloaded }
        remote.onRead = { [unowned remote] in meter.set(Double(remote.downloaded) / Double(r.download)) }
        meter.say("Downloading \(r.name)\u{2019}s MacinTalk from \(r.server)")
        // InstallMacOSX.dmg > InstallMacOSX.pkg (xar) > InstallESD.dmg > Packages/*.pkg, BaseSystem.dmg
        let outer = try volume(DiskImage(remote), r)
        guard let installer = try outer.files(where: { $0 == "InstallMacOSX.pkg" }).first else { throw missing(r, "InstallMacOSX.pkg") }
        let pkg = outer.source(installer)
        let (offset, length) = try MacVoicePackage.member("InstallESD.dmg", of: SourceReader(pkg))
        let esd = try volume(DiskImage(SliceSource(pkg, at: offset, size: length)), r)
        let packages = ["BaseSystemBinaries", "Essentials", "AdditionalSpeechVoices"].map { "Packages/\($0).pkg" }
        let files = try esd.files { packages.contains($0) || $0 == "BaseSystem.dmg" }

        // fetch_macintalk.sh: the programs come from the packages...
        let tree = work.appendingPathComponent("tree", isDirectory: true)
        for p in packages {
            guard let f = files.first(where: { $0.path == p }) else { throw missing(r, p) }
            meter.say("Reading \((p as NSString).lastPathComponent) from \(r.name)\u{2019}s installer")
            try lionPackage(esd, f, into: tree, expect: lionFiles(from: (p as NSString).lastPathComponent))
            remote.dropReadAhead()
        }
        let t = { (p: String) in tree.appendingPathComponent(p) }
        func have(_ p: String) -> Bool { fm.fileExists(atPath: t(p).path) }
        guard have(engine) else { throw missing(r, "the MacinTalk engine") }
        guard have(dictionary + "/SpeechDictionary") else { throw missing(r, "the speech dictionary") }
        try fm.copyItem(at: t(engine).resolvingSymlinksInPath(), to: out.appendingPathComponent("MacinTalk"))
        try fm.copyItem(at: t(dictionary), to: out.appendingPathComponent("SpeechDictionary"))
        try fm.createDirectory(at: out.appendingPathComponent("lib"), withIntermediateDirectories: true)
        for lib in r.lib {
            guard have("usr/lib/" + lib) else { throw missing(r, lib) }
            try fm.copyItem(at: t("usr/lib/" + lib).resolvingSymlinksInPath(), to: out.appendingPathComponent("lib/" + lib))
        }
        let voiceOut = out.appendingPathComponent("Voices")
        for name in (try? fm.contentsOfDirectory(atPath: t(voices).path))?.sorted() ?? [] where name.hasSuffix(".SpeechVoice") && !big.contains(name) {
            let v = t(voices).appendingPathComponent(name)
            guard isFile(v.appendingPathComponent("Contents/Resources/VoiceDescription")) else { continue }
            try fm.copyItem(at: v, to: voiceOut.appendingPathComponent(name))
        }

        // ...and Fred and the dictionary's data from the base system disk.
        meter.say("Reading BaseSystem.dmg from \(r.name)\u{2019}s installer")
        guard let bsFile = files.first(where: { $0.path == "BaseSystem.dmg" }) else { throw missing(r, "BaseSystem.dmg") }
        let bs = try volume(DiskImage(esd.source(bsFile)), r)
        let resources = dictionary + "/Resources/"
        let bsFiles = try bs.files { $0.hasPrefix(resources) || $0.hasPrefix(voices + "/") }
        for f in bsFiles where f.path.hasPrefix(resources) {
            let rel = String(f.path.dropFirst(resources.count))
            if safe(rel) { try copy(bs, f, to: out.appendingPathComponent("SpeechDictionary/Resources/" + rel)) }
        }
        let byVoice = Dictionary(grouping: bsFiles.filter { $0.path.hasPrefix(voices + "/") }) {
            String($0.path.dropFirst(voices.count + 1).split(separator: "/").first ?? "")
        }
        for (name, list) in byVoice.sorted(by: { $0.key < $1.key }) where name.hasSuffix(".SpeechVoice") && !big.contains(name) {
            let prefix = voices + "/" + name + "/"
            guard list.contains(where: { $0.path == prefix + "Contents/Resources/VoiceDescription" && $0.mode & 0o170000 != 0o120000 }),
                  !fm.fileExists(atPath: voiceOut.appendingPathComponent(name).path) else { continue }
            for f in list where f.path.hasPrefix(prefix) {
                let rel = name + "/" + f.path.dropFirst(prefix.count)
                if safe(rel) { try copy(bs, f, to: voiceOut.appendingPathComponent(rel)) }
            }
        }
    }

    private static func isFile(_ u: URL) -> Bool {
        (try? u.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true
    }

    /// What fetch_macintalk.sh's tar takes from a package, into `tree`
    /// (paths as in the package). Lion's packages aren't in path order, so
    /// it reads until the package has given every file in `expect`, or to its
    /// end when that's empty.
    private static func lionPackage(_ vol: HFSVolume, _ pkg: HFSVolume.File, into tree: URL, expect: Set<String>) throws {
        let wanted = ["System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer/", voices + "/",
                      "System/Library/PrivateFrameworks/SpeechDictionary.framework/",
                      "usr/lib/libc++abi.dylib", "usr/lib/libstdc++.6.0.9.dylib"]
        var left = expect
        let cpio = Cpio { path, mode in
            if !expect.isEmpty && left.isEmpty { throw Cpio.Stop() }
            let p = path.hasPrefix("./") ? String(path.dropFirst(2)) : path
            guard wanted.contains(where: { p.hasPrefix($0) || p == $0 }),
                  !p.contains("/Alex.SpeechVoice/"), safe(p) else { return .skip }
            let dest = tree.appendingPathComponent(p)
            switch mode & 0o170000 {
            case 0o040000: return .folder(dest)
            case 0o100000: left.remove(p); return .file(dest)
            case 0o120000: left.remove(p); return .link(dest)
            default: return .skip
            }
        }
        try unpack(SourceReader(vol.source(pkg)), name: pkg.path, into: cpio)
    }

    /// The files of Lion's result (as in `manifest`) one of its packages
    /// gives, by their paths in the package. The rest are BaseSystem.dmg's:
    /// Fred and the dictionary's data.
    private static func lionFiles(from package: String) -> Set<String> {
        func source(_ rel: String) -> String? {
            if rel.hasPrefix("SpeechDictionary/Resources/") || rel.hasPrefix("Voices/Fred.SpeechVoice/") { return nil }
            if rel == "MacinTalk" || rel.hasPrefix("lib/") || rel.hasPrefix("SpeechDictionary/") { return "BaseSystemBinaries.pkg" }
            if rel.hasPrefix("Voices/Vicki.SpeechVoice/") { return "AdditionalSpeechVoices.pkg" }
            return rel.hasPrefix("Voices/") ? "Essentials.pkg" : nil
        }
        var out = Set<String>()
        for rel in manifestEntries("mt107").keys where source(rel) == package {
            if rel == "MacinTalk" { out.insert(engine) }
            else if rel.hasPrefix("lib/") { out.insert("usr/lib/" + rel.dropFirst(4)) }
            else if rel.hasPrefix("SpeechDictionary/") { out.insert(dictionary + "/" + rel.dropFirst(17)) }
            else { out.insert(voices + "/" + rel.dropFirst(7)) }
        }
        return out
    }

    // MARK: - checking

    /// rel path in root/<gen> -> SHA-256 or "link:<target>", from `manifest`.
    private static func manifestEntries(_ gen: String) -> [String: String] {
        var want: [String: String] = [:]
        for line in manifest.split(separator: "\n") {
            let parts = line.split(separator: " ", maxSplits: 1)
            guard parts.count == 2, parts[1].hasPrefix(gen + "/") else { continue }
            want[String(parts[1].dropFirst(gen.count + 1))] = String(parts[0])
        }
        return want
    }

    private static func check(_ r: Release, _ dir: URL) throws {
        let want = manifestEntries(r.gen)
        guard !want.isEmpty else { return }
        let have = try digest(dir)
        let bad = Set(want.keys).union(have.keys).filter { want[$0] != have[$0] }.sorted()
        guard bad.isEmpty else {
            throw FetchError("\(bad.count) of \(r.name)\u{2019}s MacinTalk files aren\u{2019}t the ones Classic Voices was made for "
                + "(\(bad.prefix(3).joined(separator: ", "))). The disc on \(r.server) may have changed.")
        }
    }

    /// rel path -> SHA-256, or "link:<target>" for a symbolic link.
    private static func digest(_ dir: URL) throws -> [String: String] {
        let fm = FileManager.default
        var out: [String: String] = [:]
        let keys: [URLResourceKey] = [.isSymbolicLinkKey, .isRegularFileKey]
        guard let e = fm.enumerator(at: dir, includingPropertiesForKeys: keys) else { return out }
        let base = dir.standardizedFileURL.path + "/"
        for case let u as URL in e where u.lastPathComponent != ".DS_Store" {
            let v = try u.resourceValues(forKeys: Set(keys))
            let path = u.standardizedFileURL.path
            guard path.hasPrefix(base) else { continue }
            let rel = String(path.dropFirst(base.count))
            if v.isSymbolicLink == true {
                out[rel] = "link:" + (try fm.destinationOfSymbolicLink(atPath: u.path))
            } else if v.isRegularFile == true {
                let data = try Data(contentsOf: u, options: .alwaysMapped)
                var hash = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
                data.withUnsafeBytes { _ = CC_SHA256($0.baseAddress, CC_LONG($0.count), &hash) }
                out[rel] = hash.map { String(format: "%02x", $0) }.joined()
            }
        }
        return out
    }
}

// MARK: - a file on a web server

/// A file on a web server, read with range requests in 4 MB blocks. A run of
/// blocks read in order makes it fetch ahead on several connections (archive.org
/// gives each about 1 MB/s); only the last few dozen blocks are kept in memory.
/// A connection that drops is picked up from the byte it reached.
final class RemoteFile: RandomAccess, @unchecked Sendable {
    static let block: UInt64 = 4 << 20
    private static let ahead: UInt64 = 16, keep = 24, prefetchMost = 16
    private let original: URL
    private var url: URL
    let size: UInt64
    private let cond = NSCondition()
    private var cache: [UInt64: Data] = [:]
    private var order: [UInt64] = []   // least recently used first
    private var used: Set<UInt64> = []   // read at least once
    private var pending: Set<UInt64> = []
    private var ahead: [UInt64: Operation] = [:]   // read-ahead not started yet
    private var failed: [UInt64: Error] = [:]
    private var last = UInt64.max
    private var got: UInt64 = 0
    private var closed = false
    private let queue = OperationQueue()
    /// Called after each read, on the reading thread.
    var onRead: (() -> Void)?

    var downloaded: UInt64 { cond.withLock { got } }

    init(_ url: URL) throws {
        original = url
        queue.maxConcurrentOperationCount = 12
        let (resolved, total) = try Self.resolve(url)
        self.url = resolved
        size = total
    }

    /// archive.org redirects to the server holding the item; ask that one from then on.
    private static func resolve(_ url: URL) throws -> (URL, UInt64) {
        var last: Error = MacinTalkFetch.FetchError("\(url.host ?? "The server") didn\u{2019}t answer.")
        for attempt in 0..<6 {
            let r = RangeFetcher.shared.get(url, 0 ..< 1) { _ in }
            if let response = r.response, let total = response.value(forHTTPHeaderField: "Content-Range")?.split(separator: "/").last.flatMap({ UInt64($0) }),
               r.data.count == 1, let resolved = response.url {
                return (resolved, total)
            }
            last = r.error ?? last
            Thread.sleep(forTimeInterval: Double(2 * (attempt + 1)))
        }
        throw last
    }

    func close() {
        cond.withLock { closed = true; cond.broadcast() }
        queue.cancelAllOperations()
    }

    private func fetch(_ b: UInt64) throws -> Data {
        let start = b * Self.block, end = min(start + Self.block, size)
        var data = Data(capacity: Int(end - start))
        var failures = 0
        while UInt64(data.count) < end - start {
            let at = start + UInt64(data.count)
            let r = RangeFetcher.shared.get(cond.withLock { url }, at ..< end) { n in self.cond.withLock { self.got += UInt64(n) } }
            data.append(r.data.prefix(Int(end - at)))
            if UInt64(data.count) >= end - start { break }
            failures = r.data.isEmpty ? failures + 1 : 1   // went on a while: keep trying
            if cond.withLock({ closed }) { throw CancellationError() }
            guard failures <= 8 else {
                throw r.error ?? MacinTalkFetch.FetchError("\(url.host ?? "The server") stopped answering.")
            }
            if failures == 3 || failures == 6, let (u, s) = try? Self.resolve(original), s == size { cond.withLock { url = u } }
            Thread.sleep(forTimeInterval: Double(min(2 * failures, 20)))
        }
        return data
    }

    /// With `cond` locked.
    private func schedule(_ b: UInt64, urgent: Bool) {
        guard !closed, b * Self.block < size, cache[b] == nil, !pending.contains(b) else { return }
        pending.insert(b)
        let op = BlockOperation { [self] in
            guard cond.withLock({ ahead.removeValue(forKey: b) != nil || urgent }) else { return }
            let result = autoreleasepool { Result { try fetch(b) } }
            cond.withLock {
                pending.remove(b)
                switch result {
                case .success(let d):
                    cache[b] = d
                    order.append(b)
                    // blocks already read go first, so reading ahead isn't undone
                    while order.count > Self.keep {
                        let old = order.remove(at: order.firstIndex(where: { used.contains($0) }) ?? 0)
                        cache[old] = nil
                        used.remove(old)
                    }
                case .failure(let e):
                    failed[b] = e
                }
                cond.broadcast()
            }
        }
        op.queuePriority = urgent ? .veryHigh : .normal
        if !urgent { ahead[b] = op }
        queue.addOperation(op)
    }

    /// Forgets the read-ahead not started yet: the reading jumps elsewhere.
    func dropReadAhead() {
        cond.withLock {
            for (b, op) in ahead { op.cancel(); pending.remove(b) }
            ahead.removeAll()
        }
    }

    private func block(_ b: UInt64) throws -> Data {
        cond.lock()
        defer { cond.unlock() }
        if b != last {
            if last != .max && b == last + 1 {
                for k in b + 1 ... b + Self.ahead { schedule(k, urgent: false) }
            }
            last = b
        }
        while true {
            if let d = cache[b] {
                if let i = order.lastIndex(of: b) { order.remove(at: i); order.append(b) }
                used.insert(b)
                return d
            }
            if let e = failed.removeValue(forKey: b) { throw e }
            if closed { throw CancellationError() }
            schedule(b, urgent: true)
            ahead[b]?.queuePriority = .veryHigh
            cond.wait()
        }
    }

    func read(at offset: UInt64, count: Int) throws -> Data {
        let end = min(offset + UInt64(count), size)
        guard offset < end else { return Data() }
        var out = Data(capacity: Int(end - offset))
        for b in offset / Self.block ... (end - 1) / Self.block {
            let d = try block(b)
            let from = max(offset, b * Self.block) - b * Self.block, to = min(end, b * Self.block + UInt64(d.count)) - b * Self.block
            out.append(d[d.startIndex + Int(from) ..< d.startIndex + Int(to)])
        }
        onRead?()
        return out
    }

    func sources(_ offset: UInt64, _ count: UInt64) -> [Range<UInt64>] { [offset ..< offset + count] }

    func prefetch(_ ranges: [Range<UInt64>]) {
        var want = Set<UInt64>()
        for r in ranges where !r.isEmpty && r.lowerBound < size {
            for b in r.lowerBound / Self.block ... (min(r.upperBound, size) - 1) / Self.block { want.insert(b) }
        }
        cond.withLock {
            for b in want.sorted().filter({ cache[$0] == nil }).prefix(Self.prefetchMost) { schedule(b, urgent: false) }
        }
    }
}

/// Blocking range requests on one session (so connections are kept), which
/// insists on a 206 for the bytes asked for and hands back whatever arrived
/// before a connection dropped.
final class RangeFetcher: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    static let shared = RangeFetcher()

    struct Result { var data = Data(); var response: HTTPURLResponse?; var error: Error? }

    private final class Job {
        let start: UInt64, count: Int, progress: (Int) -> Void
        var result = Result()
        let done = DispatchSemaphore(value: 0)
        init(start: UInt64, count: Int, progress: @escaping (Int) -> Void) { self.start = start; self.count = count; self.progress = progress }
    }

    private let lock = NSLock()
    private var jobs: [Int: Job] = [:]
    private lazy var session: URLSession = {
        let c = URLSessionConfiguration.default
        c.httpMaximumConnectionsPerHost = 12
        c.timeoutIntervalForRequest = 60
        c.urlCache = nil
        c.requestCachePolicy = .reloadIgnoringLocalCacheData
        return URLSession(configuration: c, delegate: self, delegateQueue: nil)
    }()

    func get(_ url: URL, _ range: Range<UInt64>, progress: @escaping (Int) -> Void) -> Result {
        var req = URLRequest(url: url)
        req.setValue("ClassicVoices/\(UpdateCheck.current)", forHTTPHeaderField: "User-Agent")
        req.setValue("bytes=\(range.lowerBound)-\(range.upperBound - 1)", forHTTPHeaderField: "Range")
        let task = session.dataTask(with: req)
        let job = Job(start: range.lowerBound, count: range.count, progress: progress)
        lock.withLock { jobs[task.taskIdentifier] = job }
        task.resume()
        job.done.wait()
        return job.result
    }

    private func job(_ t: URLSessionTask) -> Job? { lock.withLock { jobs[t.taskIdentifier] } }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        guard let j = job(dataTask) else { return completionHandler(.cancel) }
        let http = response as? HTTPURLResponse
        j.result.response = http
        // Anything but the bytes asked for (a whole 8 GB disc, say) is refused.
        if http?.statusCode != 206 || http?.value(forHTTPHeaderField: "Content-Range")?.hasPrefix("bytes \(j.start)-") != true {
            let host = dataTask.originalRequest?.url?.host ?? "The server"
            j.result.error = MacinTalkFetch.FetchError("\(host) answered HTTP \(http?.statusCode ?? 0) instead of the part asked for.")
            completionHandler(.cancel)
        } else {
            completionHandler(.allow)
        }
    }

    func urlSession(_ s: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard let j = job(dataTask) else { return }
        j.result.data.append(data)
        j.progress(data.count)
        if j.result.data.count > j.count { dataTask.cancel() }
    }

    func urlSession(_ s: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let j = lock.withLock({ jobs.removeValue(forKey: task.taskIdentifier) }) else { return }
        if j.result.error == nil { j.result.error = error }
        if j.result.error != nil && j.result.response?.statusCode != 206 { j.result.data = Data() }
        j.done.signal()
    }
}

// MARK: - archives

/// An odc cpio archive (a package's Payload), fed as it's unpacked. `decide`
/// says what to do with each member, or throws Stop to end early.
private final class Cpio {
    struct Stop: Error {}
    enum Target { case skip, folder(URL), file(URL), link(URL) }

    private let decide: (String, UInt32) throws -> Target
    private var header = Data()
    private var name = Data()
    private var nameLeft = 0
    private var dataLeft: UInt64 = 0
    private var target = Target.skip
    private var out: FileHandle?
    private var linkData = Data()
    private var ended = false
    private enum State { case header, name, data }
    private var state = State.header

    init(_ decide: @escaping (String, UInt32) throws -> Target) { self.decide = decide }

    func feed(_ buf: UnsafeRawBufferPointer) throws { try autoreleasepool { try feedPart(buf) } }

    private func feedPart(_ buf: UnsafeRawBufferPointer) throws {
        guard let base = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
        var i = 0
        while i < buf.count && !ended {
            switch state {
            case .header:
                let take = min(76 - header.count, buf.count - i)
                header.append(base + i, count: take)
                i += take
                guard header.count == 76 else { continue }
                guard header.prefix(6) == Data("070707".utf8),
                      let mode = UInt32(String(decoding: header[18..<24], as: UTF8.self), radix: 8),
                      let ns = Int(String(decoding: header[59..<65], as: UTF8.self), radix: 8),
                      let fs = UInt64(String(decoding: header[65..<76], as: UTF8.self), radix: 8)
                else { throw MacinTalkFetch.FetchError("A package\u{2019}s file list is damaged.") }
                modeNow = mode
                nameLeft = ns
                dataLeft = fs
                name = Data()
                state = .name
            case .name:
                let take = min(nameLeft, buf.count - i)
                name.append(base + i, count: take)
                i += take
                nameLeft -= take
                guard nameLeft == 0 else { continue }
                header = Data()
                let path = String(decoding: name.prefix { $0 != 0 }, as: UTF8.self)
                if path == "TRAILER!!!" { ended = true; return }
                try begin(path)
                state = .data
                if dataLeft == 0 { try end() }
            case .data:
                let take = Int(min(dataLeft, UInt64(buf.count - i)))
                let piece = UnsafeRawBufferPointer(start: base + i, count: take)
                if let out { try out.write(contentsOf: Data(piece)) }
                if case .link = target { linkData.append(contentsOf: piece) }
                i += take
                dataLeft -= UInt64(take)
                if dataLeft == 0 { try end() }
            }
        }
    }

    private var modeNow: UInt32 = 0

    private func begin(_ path: String) throws {
        target = try decide(path, modeNow)
        let fm = FileManager.default
        switch target {
        case .skip: break
        case .folder(let u):
            try fm.createDirectory(at: u, withIntermediateDirectories: true)
        case .file(let u):
            try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.removeItem(at: u)
            fm.createFile(atPath: u.path, contents: nil)
            out = try FileHandle(forWritingTo: u)
        case .link:
            linkData = Data()
        }
    }

    private func end() throws {
        try out?.close()
        out = nil
        if case .link(let u) = target {
            let fm = FileManager.default
            try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.removeItem(at: u)
            try fm.createSymbolicLink(atPath: u.path, withDestinationPath: String(decoding: linkData, as: UTF8.self))
        }
        target = .skip
        state = .header
    }

    /// Closes what's open when the stream stopped early.
    func finish() throws {
        try out?.close()
        out = nil
    }
}

/// A tar archive, fed as it's unpacked: one member's data goes to the sink
/// `want` gives for it; once that member is done it stops (Cpio.Stop).
private final class Tar {
    typealias Sink = (UnsafeRawBufferPointer) throws -> Void
    private let want: (String, UInt64) throws -> Sink?
    private var header = Data()
    private var longName: Data?
    private var collecting: Data?   // a GNU long name being read
    private var left: UInt64 = 0
    private var pad = 0
    private var sink: Sink?
    private(set) var finished = false

    init(_ want: @escaping (String, UInt64) throws -> Sink?) { self.want = want }

    func feed(_ buf: UnsafeRawBufferPointer) throws {
        guard let base = buf.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
        var i = 0
        while i < buf.count {
            if left > 0 {
                let take = Int(min(left, UInt64(buf.count - i)))
                let piece = UnsafeRawBufferPointer(start: base + i, count: take)
                try sink?(piece)
                collecting?.append(contentsOf: piece)
                i += take
                left -= UInt64(take)
                if left == 0 {
                    if sink != nil { finished = true; throw Cpio.Stop() }
                    if let c = collecting { longName = c; collecting = nil }
                }
            } else if pad > 0 {
                let take = min(pad, buf.count - i)
                i += take
                pad -= take
            } else {
                let take = min(512 - header.count, buf.count - i)
                header.append(base + i, count: take)
                i += take
                guard header.count == 512 else { continue }
                let h = header
                header = Data()
                if h.allSatisfy({ $0 == 0 }) { throw Cpio.Stop() }   // the end of the archive
                func field(_ a: Int, _ n: Int) -> Data { h.subdata(in: a ..< a + n).prefix { $0 != 0 } }
                var size: UInt64 = 0
                if h[124] & 0x80 != 0 {  // GNU base-256
                    for k in 125..<136 { size = size << 8 | UInt64(h[k]) }
                } else {
                    size = UInt64(String(decoding: field(124, 12), as: UTF8.self).trimmingCharacters(in: .whitespaces), radix: 8) ?? 0
                }
                let type = h[156]
                var name = String(decoding: field(0, 100), as: UTF8.self)
                if field(257, 6) == Data("ustar".utf8), !field(345, 155).isEmpty {
                    name = String(decoding: field(345, 155), as: UTF8.self) + "/" + name
                }
                if let l = longName { name = String(decoding: l.prefix { $0 != 0 }, as: UTF8.self); longName = nil }
                left = size
                pad = Int((512 - size % 512) % 512)
                sink = nil
                if type == UInt8(ascii: "L") {
                    collecting = Data()
                } else if type == 0 || type == UInt8(ascii: "0") {
                    sink = try want(name, size)
                }
                if left == 0, sink != nil { finished = true; throw Cpio.Stop() }
            }
        }
    }
}

/// A disk image written as it streams past, as a sparse file. Once the
/// volume's catalog has gone by, only the parts of the disk holding the files
/// `wanted` accepts are kept; until then (or if that can't be worked out)
/// everything that isn't zeros is.
private final class SparseImage {
    private static let piece = 1 << 20
    private let url: URL
    private let size: UInt64
    private let out: FileHandle
    private let wanted: (String) -> Bool
    private let buffer = UnsafeMutableRawPointer.allocate(byteCount: piece, alignment: 16)
    private var filled = 0
    private var at: UInt64 = 0                 // where `buffer` starts
    private var keep: [Range<UInt64>]?         // nil: everything
    private var next = 0                       // into keep
    private var metadataEnd: UInt64?
    private var base: UInt64 = 0
    private var planned = false

    init(_ url: URL, size: UInt64, wanted: @escaping (String) -> Bool) throws {
        self.url = url
        self.size = size
        self.wanted = wanted
        FileManager.default.createFile(atPath: url.path, contents: nil)
        out = try FileHandle(forWritingTo: url)
        try out.truncate(atOffset: size)
    }

    deinit { buffer.deallocate() }

    func write(_ buf: UnsafeRawBufferPointer) throws {
        var i = 0
        while i < buf.count {
            let take = min(Self.piece - filled, buf.count - i)
            buffer.advanced(by: filled).copyMemory(from: buf.baseAddress!.advanced(by: i), byteCount: take)
            filled += take
            i += take
            if filled == Self.piece { try flush() }
        }
    }

    func finish() throws {
        if filled > 0 { try flush() }
        try out.synchronize()
        try out.close()
    }

    private func flush() throws {
        let d = UnsafeRawBufferPointer(start: buffer, count: filled)
        filled = 0
        let r = at ..< at + UInt64(d.count)
        if needed(r), !Self.isZero(d) {
            try out.seek(toOffset: at)
            try out.write(contentsOf: Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: d.baseAddress!), count: d.count, deallocator: .none))
        }
        at = r.upperBound
        if !planned && (metadataEnd == nil || at >= metadataEnd!) {
            do { try plan() } catch { planned = true }   // keeps everything
        }
    }

    private func needed(_ r: Range<UInt64>) -> Bool {
        guard let keep else { return true }
        while next < keep.count && keep[next].upperBound <= r.lowerBound { next += 1 }
        return next < keep.count && keep[next].lowerBound < r.upperBound
    }

    private static func isZero(_ raw: UnsafeRawBufferPointer) -> Bool {
        let words = raw.bindMemory(to: UInt64.self)   // the buffer is 16-byte aligned
        for w in words where w != 0 { return false }
        for k in words.count * 8 ..< raw.count where raw[k] != 0 { return false }
        return true
    }

    /// First where the volume's catalog is; once it's here, what to keep.
    private func plan() throws {
        if metadataEnd == nil {
            guard at >= 64 << 10 else { return }
            let disk = try DiskImage(FileSource(url))
            var found = try disk.hfsVolumes().first
            if found == nil {
                let mbr = Data(try disk.read(at: 0, count: 512))
                if mbr.count == 512, mbr[510] == 0x55, mbr[511] == 0xAA {
                    found = (0..<4).first { mbr[446 + 16 * $0 + 4] == 0xAF }.map { UInt64(mbr.le32(446 + 16 * $0 + 8)) * 512 }
                }
            }
            guard let b = found else { if at > 64 << 20 { planned = true }; return }
            guard at >= b + 1536 else { return }
            base = b
            let vh = try disk.read(at: base + 1024, count: 512)
            let blockSize = UInt64(vh.be32(40))
            var end: UInt64 = 0
            for o in [192, 272, 352] {   // extents overflow, catalog, attributes
                var blocks: UInt64 = 0
                for k in 0..<8 {
                    let start = UInt64(vh.be32(o + 16 + 8 * k)), count = UInt64(vh.be32(o + 20 + 8 * k))
                    blocks += count
                    if count > 0 { end = max(end, base + (start + count) * blockSize) }
                }
                if blocks * blockSize < vh.be64(o) { planned = true; return }   // more in the overflow file
            }
            metadataEnd = end
            if at < end { return }
        }
        planned = true
        let vol = try HFSVolume(DiskImage(FileSource(url)), at: base)
        let files = try vol.files(where: wanted)
        guard !files.isEmpty else { return }
        let vh = try FileSource(url).read(at: base + 1024, count: 512)
        let blockSize = UInt64(vh.be32(40))
        var ranges: [Range<UInt64>] = []
        for f in files {
            for e in f.fork.extents + f.resource.extents where e.count > 0 {
                ranges.append(base + UInt64(e.start) * blockSize ..< base + UInt64(e.start + e.count) * blockSize)
            }
        }
        ranges.sort { $0.lowerBound < $1.lowerBound }
        var merged: [Range<UInt64>] = []
        for r in ranges {
            if let l = merged.last, r.lowerBound <= l.upperBound { merged[merged.count - 1] = l.lowerBound ..< max(l.upperBound, r.upperBound) }
            else { merged.append(r) }
        }
        keep = merged
        next = 0
    }
}

// MARK: - the files the fast engine code was made for

extension MacinTalkFetch {
    /// tools/profiles/oldmt.sha256 without Alex, and the same of Lion's
    /// extracted/mt107 (what tools/fetch_macintalk.sh writes) without Alex.
    static let manifest = """
5cd3d9b2eb1380a41ebfa0d827e29cf271776320ec478d46470864dce6ab0e6f mt104/MacinTalk
ccc8d144309e7031aaf800f2585b526d10f3ee0862c13130d6dc7b0a734bfa4b mt104/SpeechDictionary/Resources/CartLite
8468117d5b082db0366fc0cc3acb8d50a091069e9308f5dd095f417ae0f7f00b mt104/SpeechDictionary/Resources/CartNames
66019145e20d8c5036422b04c95766a5553cd044ab9f55ea23c4ea678aafda94 mt104/SpeechDictionary/Resources/Info.plist
6db09ad16e2b85badb90ebea805cdbcfeb219f5684a751ade7d60c9b2d5c3a40 mt104/SpeechDictionary/Resources/PhonemeSymbols
01f4559968c47e91f518a39e59da105ca7f99e04c25d20e5d0ab40f44e125fc3 mt104/SpeechDictionary/Resources/StdDictionary
47a5dd37b05a6d3de8789b917805575a1289107ecfbaa9dbd8daf6d0c96144af mt104/SpeechDictionary/Resources/SymbolDictionary
f87f60ee35ec274937ed8b62f3ee272da38a7c981dae11daaf86d986e1029eca mt104/SpeechDictionary/Resources/version.plist
7756a8ababd7ba356db83c4800080a28256aa1d6e8ab9e247df7b44732718d9f mt104/SpeechDictionary/SpeechDictionary
0cdcc49921c7ddb5570e47b713d83c6649f1bde20f806b56eb8db5d3f8ac4f16 mt104/Voices/Agnes.SpeechVoice/Contents/Info.plist
544e48db4f2477cb9114174b6be70b0a36bc6598df07b82fb62d2b77d12b1bed mt104/Voices/Agnes.SpeechVoice/Contents/Resources/PCMWave
d9b57707e541eb5a8db8c837a1dacc25b45d3a01f078f67521b66d098b9daa5f mt104/Voices/Agnes.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Agnes.SpeechVoice/Contents/version.plist
378d11e347fcb176c8553c7a7c8fa97ab5d8042a6ee694fd9f173cbbb1d531f0 mt104/Voices/Albert.SpeechVoice/Contents/Info.plist
fe678bd0a5278a271489d37702c6683ab036a6f1049a60f157fda16b1ea5a8b6 mt104/Voices/Albert.SpeechVoice/Contents/Resources/PCMWave
78414d75dab5f4ce4fcf36e524cd1e7a58081ae7e810f89f1bbedef2507b1c06 mt104/Voices/Albert.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Albert.SpeechVoice/Contents/version.plist
949621f9c24d91c8f4ff1e169a3be5612e63c13e78f81e67ce6ec7e558ca113c mt104/Voices/BadNews.SpeechVoice/Contents/Info.plist
2eb42905b1e49b80690b90e1c36fc62cd3780e9fe08e377eef4873e7e7de387b mt104/Voices/BadNews.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/BadNews.SpeechVoice/Contents/version.plist
ef2f3d2a3bdbce574919f2d7c95cc06efbc4ac1f5428e4538497d6b2342fa146 mt104/Voices/Bahh.SpeechVoice/Contents/Info.plist
48413c8a38e725fb8ce5e2bec51638edc96b1254735a956e9ec9460f94a1991c mt104/Voices/Bahh.SpeechVoice/Contents/Resources/PCMWave
92aaf27f60c7d10f99ff96b54fcca8952ec61ed35fe1c4e39205f4adb541c315 mt104/Voices/Bahh.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Bahh.SpeechVoice/Contents/version.plist
f79cc8bcd4dac0aed7bdcf36b67282bb68c1097178cda54993acea182fccafdb mt104/Voices/Bells.SpeechVoice/Contents/Info.plist
de3308f18adf2638af13821ea190ee1a8e4e062632cc89f7752468924d7b7462 mt104/Voices/Bells.SpeechVoice/Contents/Resources/PCMWave
9fb62f7c46408c82a5ad00fd6608f8f2b03b867c242d79815ae0c9659f4f33f6 mt104/Voices/Bells.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Bells.SpeechVoice/Contents/version.plist
e9b0c12550c01f2281d0fc6b09d50a7c354855674b7db7288cf3c9b6b0e1a0e9 mt104/Voices/Boing.SpeechVoice/Contents/Info.plist
a19508ebc7c66fc4e5b000ef1cadf0040b0fdfe8e51674b931ddee9ad9644cdd mt104/Voices/Boing.SpeechVoice/Contents/Resources/PCMWave
fa6b4c3327ba799ec806372ac2c9cae780724b56fc494f27cb0432e09a3a379f mt104/Voices/Boing.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Boing.SpeechVoice/Contents/version.plist
d5b3e925c297815cb89478aa1d65a17d3209da56f313ab959af6f1412acab10c mt104/Voices/Bruce.SpeechVoice/Contents/Info.plist
ed19919c61476f228633d49239a0eee948f596a484dcab03f5ffff069a7fd5f9 mt104/Voices/Bruce.SpeechVoice/Contents/Resources/PCMWave
52fc6eaecb54eff627fb7b3f49a06a49c35a590284da9a6b40e02da340b1aea4 mt104/Voices/Bruce.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Bruce.SpeechVoice/Contents/version.plist
16eb8df606fae44a4679f3b55331c349d899c43b0b46cbbee253d47bcb44dc8a mt104/Voices/Bubbles.SpeechVoice/Contents/Info.plist
3c7f9bfa11e496af42006b9990b62eb1731a1942e68b2d24dd29a76f64f896bb mt104/Voices/Bubbles.SpeechVoice/Contents/Resources/PCMWave
92f44607dd55d67cd47bb8df1d68379f6e4118ad9db9e0b18f833d4385e04bb1 mt104/Voices/Bubbles.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Bubbles.SpeechVoice/Contents/version.plist
e21b96900aa405196b26c7af1ff8fe1dc4326fa92398c8516f0daa97e99b8503 mt104/Voices/Cellos.SpeechVoice/Contents/Info.plist
fd2ea62a5f8ebea6c17e1c47662d1889df3ae1ddea4eb5a7660d798d7100259c mt104/Voices/Cellos.SpeechVoice/Contents/Resources/PCMWave
722b8d501c4de746528585f58410065f7923fb41c5955cc40f9beaccb4152599 mt104/Voices/Cellos.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Cellos.SpeechVoice/Contents/version.plist
836aae02cdb4342ef0cb12826f3fb42b0c36a61f0ce2da7ae8021496e76e2375 mt104/Voices/Deranged.SpeechVoice/Contents/Info.plist
26f86dfbadb68a35fc76ace6ef380c81795b69b49aa4771b72e8f29576eaa1f9 mt104/Voices/Deranged.SpeechVoice/Contents/Resources/PCMWave
617037fb26fc4457bbbbfa36a810637e125e9f91d6332eeb5a50b54cab568ed3 mt104/Voices/Deranged.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Deranged.SpeechVoice/Contents/version.plist
27f01e146f403543c918cd71329cf483a070a7e20068accc22f630ed76088af8 mt104/Voices/Fred.SpeechVoice/Contents/Info.plist
992d7ab297c4ec9737cad4407492e4c616acdbfc9f7d5ce095dc0c1abb18a1fd mt104/Voices/Fred.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Fred.SpeechVoice/Contents/version.plist
bac7d00711d83e7853329c1b76516935b61c7b4d34025ec2f7d0923cedb56faa mt104/Voices/GoodNews.SpeechVoice/Contents/Info.plist
c2ab65e79b45fc967216f7ca400a60f3dd83ee6adb5cd8ed4a2760ccf0376c8e mt104/Voices/GoodNews.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/GoodNews.SpeechVoice/Contents/version.plist
003373327bec87c3dd38a07aac7b557e31eeb721c58985c4a7be959a4bc64fa0 mt104/Voices/Hysterical.SpeechVoice/Contents/Info.plist
111c90a4e42fb07d89de19e581da82112aae5a0904ad5b93ac91e9190fe81911 mt104/Voices/Hysterical.SpeechVoice/Contents/Resources/PCMWave
242c870d30b60388fb9f1721d6e7bfffba81aabe07248dfee6750678687a4041 mt104/Voices/Hysterical.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Hysterical.SpeechVoice/Contents/version.plist
b20c71ad8cd07674ecba9d23e4407f0f0171b0b1b2b47f734e98cbc78c21d782 mt104/Voices/Junior.SpeechVoice/Contents/Info.plist
5f17a1827109e881af05268e07aec06f59a8729f8ff635f439742c482c68f6cb mt104/Voices/Junior.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Junior.SpeechVoice/Contents/version.plist
ae967a8279155429fca6b1c457f9a010963ac142079fc1a45310c36d17418eb5 mt104/Voices/Kathy.SpeechVoice/Contents/Info.plist
ce8bee14e67808647976a30167ef532c377088be51a4cf12bdfc43ece7b7e1ef mt104/Voices/Kathy.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Kathy.SpeechVoice/Contents/version.plist
0b2cdc992a4f8e1e14ada566048cc5aa62221e0fe3d969366522b07970a4389d mt104/Voices/Organ.SpeechVoice/Contents/Info.plist
d801cb883112c14fe6aafb1d1db011d2f379eaa5d8b0d1db26eca50e9012a735 mt104/Voices/Organ.SpeechVoice/Contents/Resources/PCMWave
199cd5e760fe5e2ec7953e69296423a4d7357fe570b19b315686755ebb82af4c mt104/Voices/Organ.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Organ.SpeechVoice/Contents/version.plist
d6aea29ee5833b51c813fa7d2a6bfedd97b92906ba65a078cc8eb6d161c3ae24 mt104/Voices/Princess.SpeechVoice/Contents/Info.plist
9ab17fba52d962336cb376892c92e73d6b24456a5f0bea2bf02945bbd44216a4 mt104/Voices/Princess.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Princess.SpeechVoice/Contents/version.plist
320d240d14ae6f20e5e57e332d32dc04526a6783c5dea67be56fa79ed1fa5d07 mt104/Voices/Ralph.SpeechVoice/Contents/Info.plist
c6fc335b6b02c310f63ffbd312b82470aed9f42d405c009970570b39a4677022 mt104/Voices/Ralph.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Ralph.SpeechVoice/Contents/version.plist
b1c045ad6ecb7c995c23d19a5bc137382bfab0b020636dc9fad8d52ea20aea85 mt104/Voices/Trinoids.SpeechVoice/Contents/Info.plist
23ea3a76315bc1bc6a00e6e2c41865e1f69257871ca4203b866d7f40d3cc6f05 mt104/Voices/Trinoids.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Trinoids.SpeechVoice/Contents/version.plist
493d30d2d438198d0ce6d6c0e98ab9b914ea5d27b15955fb7e048d5a01f12688 mt104/Voices/Vicki.SpeechVoice/Contents/Info.plist
6c2a9ec188e5402588ec08cc26075a5ac74eeec27a54beef050fa112ecadde27 mt104/Voices/Vicki.SpeechVoice/Contents/Resources/PCMWave
c68af2545aa6c23fa2be6fcb21952a21835fed987775c031eef6b2f72df9d06c mt104/Voices/Vicki.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Vicki.SpeechVoice/Contents/version.plist
cb990655a8395e1a5456bf5367ab91fbf5ed9bf6635a66d30e987692f686cf46 mt104/Voices/Victoria.SpeechVoice/Contents/Info.plist
566398460b3732f3a4b1f2d100b26521e324e00fba3004f87854cfe3e9c10510 mt104/Voices/Victoria.SpeechVoice/Contents/Resources/PCMWave
62875168da28d447385111faee3ecf3cde85f6fb1872728737f544ede3f97939 mt104/Voices/Victoria.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Victoria.SpeechVoice/Contents/version.plist
00166f6ba223cee9779bcceec3fd4d718662eb65bc09bde21b372e1ef9bb12ba mt104/Voices/Whisper.SpeechVoice/Contents/Info.plist
5e1c63d3d24edd926f60faadecec55ac13619c8e21f9e3aed3da7eed2b37da26 mt104/Voices/Whisper.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Whisper.SpeechVoice/Contents/version.plist
d5fabe72b670ba300cfe768ca5105bfe86d3188a86576971e1b4fb0ebc26de2b mt104/Voices/Zarvox.SpeechVoice/Contents/Info.plist
46b59ccb1cd93bb6a3c3f77d5211f9a86f33057a038e1b36cbf5ecdd5b21ce7b mt104/Voices/Zarvox.SpeechVoice/Contents/Resources/VoiceDescription
e9b26d4c25aee1abf863de9ed044c6cb1076068326ae639e538947ffe5ec635c mt104/Voices/Zarvox.SpeechVoice/Contents/version.plist
a724bbc6f56f9a5ce5e397c10e3d1f5400f8b9bbd6822416823060f579b55e88 mt105/MacinTalk
927e505ca50797ac1844508ef529a6a3eb4d286dd91cacbaf78861a0f4d4a4f7 mt105/SpeechDictionary/CodeResources
a71de09261f0c0384237904d7fc84cd8b8f8eb6f7aad8c74777503921ab6b26a mt105/SpeechDictionary/Resources/CartLite
c31260305e7779c6037f89fce1e979c56659dd2ed657101f106dd1edc0e36907 mt105/SpeechDictionary/Resources/CartNames
e61572966b408097f26a536e3cabf051af55a17fb99c4e9c0b8e4928975f3efd mt105/SpeechDictionary/Resources/Homophones
5361a8d4420ffbfa5df18f27c317cd5ea8d9d5a0536311cf05b76b6a50c512bf mt105/SpeechDictionary/Resources/Info.plist
6db09ad16e2b85badb90ebea805cdbcfeb219f5684a751ade7d60c9b2d5c3a40 mt105/SpeechDictionary/Resources/PhonemeSymbols
1cc8c02d0ada7d2275a7f4f5880625ef24f5bc9ac1e630f590f1685e90fe1778 mt105/SpeechDictionary/Resources/PrefixDictionary
59419e2e0267542eab83a573e057fe2b89d972d8f203349c43c54b9d3e5ce040 mt105/SpeechDictionary/Resources/SymbolDictionary
dd5797ef493f5eb7b4447957e73884964902a1aaebd3e1765a54414cafe395c9 mt105/SpeechDictionary/Resources/Tuples
31cffe8632796a5261baa8824dd74816a71aad502b9800776897d6c128496f52 mt105/SpeechDictionary/Resources/version.plist
23b48675ca387ec72b1cae0875d7a7b3bd04ef93d3c99f7e575a1c08606c2da8 mt105/SpeechDictionary/SpeechDictionary
da53d8498d229b0391b7462167ffe3cf158cfb9718489475c716a99a20dc6f3d mt105/Voices/Agnes.SpeechVoice/Contents/Info.plist
544e48db4f2477cb9114174b6be70b0a36bc6598df07b82fb62d2b77d12b1bed mt105/Voices/Agnes.SpeechVoice/Contents/Resources/PCMWave
d9b57707e541eb5a8db8c837a1dacc25b45d3a01f078f67521b66d098b9daa5f mt105/Voices/Agnes.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Agnes.SpeechVoice/Contents/version.plist
fee6fa7f8e9621beee230f74bca93a73031487ebde74fbdee35be259d22d8657 mt105/Voices/Albert.SpeechVoice/Contents/Info.plist
fe678bd0a5278a271489d37702c6683ab036a6f1049a60f157fda16b1ea5a8b6 mt105/Voices/Albert.SpeechVoice/Contents/Resources/PCMWave
78414d75dab5f4ce4fcf36e524cd1e7a58081ae7e810f89f1bbedef2507b1c06 mt105/Voices/Albert.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Albert.SpeechVoice/Contents/version.plist
628a517d7e1413c0b233c62c5f75997ee22220286348b4283e050dad55aea552 mt105/Voices/BadNews.SpeechVoice/Contents/Info.plist
2eb42905b1e49b80690b90e1c36fc62cd3780e9fe08e377eef4873e7e7de387b mt105/Voices/BadNews.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/BadNews.SpeechVoice/Contents/version.plist
20618d5525a3fcbbf797b53387214d88d7e996f08955914f1d32570ccf19a1c9 mt105/Voices/Bahh.SpeechVoice/Contents/Info.plist
48413c8a38e725fb8ce5e2bec51638edc96b1254735a956e9ec9460f94a1991c mt105/Voices/Bahh.SpeechVoice/Contents/Resources/PCMWave
92aaf27f60c7d10f99ff96b54fcca8952ec61ed35fe1c4e39205f4adb541c315 mt105/Voices/Bahh.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Bahh.SpeechVoice/Contents/version.plist
6eb83d9f6c84b1b62f8d59e54a573378d676d1ccc11e1c7eaf5e76947bcb7abb mt105/Voices/Bells.SpeechVoice/Contents/Info.plist
de3308f18adf2638af13821ea190ee1a8e4e062632cc89f7752468924d7b7462 mt105/Voices/Bells.SpeechVoice/Contents/Resources/PCMWave
9fb62f7c46408c82a5ad00fd6608f8f2b03b867c242d79815ae0c9659f4f33f6 mt105/Voices/Bells.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Bells.SpeechVoice/Contents/version.plist
798040e9e048da44e44b7e4c4589ed7baeaff308faeac2398011cc7487d29483 mt105/Voices/Boing.SpeechVoice/Contents/Info.plist
a19508ebc7c66fc4e5b000ef1cadf0040b0fdfe8e51674b931ddee9ad9644cdd mt105/Voices/Boing.SpeechVoice/Contents/Resources/PCMWave
fa6b4c3327ba799ec806372ac2c9cae780724b56fc494f27cb0432e09a3a379f mt105/Voices/Boing.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Boing.SpeechVoice/Contents/version.plist
c8a0bd614059c3a6c281c31ddcb82e042eec1f30272a448401238decbbcd3e19 mt105/Voices/Bruce.SpeechVoice/Contents/Info.plist
ed19919c61476f228633d49239a0eee948f596a484dcab03f5ffff069a7fd5f9 mt105/Voices/Bruce.SpeechVoice/Contents/Resources/PCMWave
52fc6eaecb54eff627fb7b3f49a06a49c35a590284da9a6b40e02da340b1aea4 mt105/Voices/Bruce.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Bruce.SpeechVoice/Contents/version.plist
1615a24912d8a7e9948bed0f922822e4beb476112a8fd5817c22eeb8ef78bd0d mt105/Voices/Bubbles.SpeechVoice/Contents/Info.plist
3c7f9bfa11e496af42006b9990b62eb1731a1942e68b2d24dd29a76f64f896bb mt105/Voices/Bubbles.SpeechVoice/Contents/Resources/PCMWave
92f44607dd55d67cd47bb8df1d68379f6e4118ad9db9e0b18f833d4385e04bb1 mt105/Voices/Bubbles.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Bubbles.SpeechVoice/Contents/version.plist
83e4c6e3d48795b9a5ed74776e48e978945c0a14e755c8fcb16e0670ca78435c mt105/Voices/Cellos.SpeechVoice/Contents/Info.plist
fd2ea62a5f8ebea6c17e1c47662d1889df3ae1ddea4eb5a7660d798d7100259c mt105/Voices/Cellos.SpeechVoice/Contents/Resources/PCMWave
722b8d501c4de746528585f58410065f7923fb41c5955cc40f9beaccb4152599 mt105/Voices/Cellos.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Cellos.SpeechVoice/Contents/version.plist
8f25c3c375b38efbf31123a38fa987f2de25ede3701ad2b61b58989077046bbf mt105/Voices/Deranged.SpeechVoice/Contents/Info.plist
26f86dfbadb68a35fc76ace6ef380c81795b69b49aa4771b72e8f29576eaa1f9 mt105/Voices/Deranged.SpeechVoice/Contents/Resources/PCMWave
617037fb26fc4457bbbbfa36a810637e125e9f91d6332eeb5a50b54cab568ed3 mt105/Voices/Deranged.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Deranged.SpeechVoice/Contents/version.plist
08c859c518c48ca78db680b42839727093dc77a1eeb669ab378efa80c6aa3173 mt105/Voices/Fred.SpeechVoice/Contents/Info.plist
992d7ab297c4ec9737cad4407492e4c616acdbfc9f7d5ce095dc0c1abb18a1fd mt105/Voices/Fred.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Fred.SpeechVoice/Contents/version.plist
3d28c9a8187b37667960995ffbf75ec1e36477854f4006c3930386c114282804 mt105/Voices/GoodNews.SpeechVoice/Contents/Info.plist
c2ab65e79b45fc967216f7ca400a60f3dd83ee6adb5cd8ed4a2760ccf0376c8e mt105/Voices/GoodNews.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/GoodNews.SpeechVoice/Contents/version.plist
e84d70f073e81a889d32f558f24c0920ebd14e68b5217fe57ea67bdaa619b4b8 mt105/Voices/Hysterical.SpeechVoice/Contents/Info.plist
111c90a4e42fb07d89de19e581da82112aae5a0904ad5b93ac91e9190fe81911 mt105/Voices/Hysterical.SpeechVoice/Contents/Resources/PCMWave
242c870d30b60388fb9f1721d6e7bfffba81aabe07248dfee6750678687a4041 mt105/Voices/Hysterical.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Hysterical.SpeechVoice/Contents/version.plist
20ec549a582a3fcbcaa7c11cfbc259b08aea5d91b6542d0d9027684fd7d66d36 mt105/Voices/Junior.SpeechVoice/Contents/Info.plist
5f17a1827109e881af05268e07aec06f59a8729f8ff635f439742c482c68f6cb mt105/Voices/Junior.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Junior.SpeechVoice/Contents/version.plist
10ac39efb61648a38700096448b4aa9a65106905ae7c94a3d8bd16d406a70b3e mt105/Voices/Kathy.SpeechVoice/Contents/Info.plist
ce8bee14e67808647976a30167ef532c377088be51a4cf12bdfc43ece7b7e1ef mt105/Voices/Kathy.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Kathy.SpeechVoice/Contents/version.plist
c1608399675484ee029e3656c6c1e28621d958f2ee288bf4334c11325dada713 mt105/Voices/Organ.SpeechVoice/Contents/Info.plist
d801cb883112c14fe6aafb1d1db011d2f379eaa5d8b0d1db26eca50e9012a735 mt105/Voices/Organ.SpeechVoice/Contents/Resources/PCMWave
199cd5e760fe5e2ec7953e69296423a4d7357fe570b19b315686755ebb82af4c mt105/Voices/Organ.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Organ.SpeechVoice/Contents/version.plist
52215945fa673c3ed5e3ef3cba84db8fd6eaf080bfba4c7069e4a68cf911b8ce mt105/Voices/Princess.SpeechVoice/Contents/Info.plist
9ab17fba52d962336cb376892c92e73d6b24456a5f0bea2bf02945bbd44216a4 mt105/Voices/Princess.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Princess.SpeechVoice/Contents/version.plist
9154f532e31c933e9c2402f7c96ba1532b476402571211cb9053d24d21f68384 mt105/Voices/Ralph.SpeechVoice/Contents/Info.plist
c6fc335b6b02c310f63ffbd312b82470aed9f42d405c009970570b39a4677022 mt105/Voices/Ralph.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Ralph.SpeechVoice/Contents/version.plist
64951bfd4aad7adcae85e3ee63527c6a0cfbbbd4e42ff19bd81548b7ba2b36d9 mt105/Voices/Trinoids.SpeechVoice/Contents/Info.plist
23ea3a76315bc1bc6a00e6e2c41865e1f69257871ca4203b866d7f40d3cc6f05 mt105/Voices/Trinoids.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Trinoids.SpeechVoice/Contents/version.plist
29be566c9e59bc3b0b461dc224107f193cb9a3d3a5e343f702669a3c243f767d mt105/Voices/Vicki.SpeechVoice/Contents/Info.plist
6c2a9ec188e5402588ec08cc26075a5ac74eeec27a54beef050fa112ecadde27 mt105/Voices/Vicki.SpeechVoice/Contents/Resources/PCMWave
c68af2545aa6c23fa2be6fcb21952a21835fed987775c031eef6b2f72df9d06c mt105/Voices/Vicki.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Vicki.SpeechVoice/Contents/version.plist
3dd806f4794f31d207c5590b0e35cde61fab2dd8b74fe6a23541ddb150366f55 mt105/Voices/Victoria.SpeechVoice/Contents/Info.plist
566398460b3732f3a4b1f2d100b26521e324e00fba3004f87854cfe3e9c10510 mt105/Voices/Victoria.SpeechVoice/Contents/Resources/PCMWave
62875168da28d447385111faee3ecf3cde85f6fb1872728737f544ede3f97939 mt105/Voices/Victoria.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Victoria.SpeechVoice/Contents/version.plist
e3e64767480a286b75cb0689245b5e3a1fc404710328994805903d5a94b092e8 mt105/Voices/Whisper.SpeechVoice/Contents/Info.plist
5e1c63d3d24edd926f60faadecec55ac13619c8e21f9e3aed3da7eed2b37da26 mt105/Voices/Whisper.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Whisper.SpeechVoice/Contents/version.plist
3bfb6d54aee6375649e0b7904ea731e5bb4106092c2aa5b75f40286bef1528d7 mt105/Voices/Zarvox.SpeechVoice/Contents/Info.plist
46b59ccb1cd93bb6a3c3f77d5211f9a86f33057a038e1b36cbf5ecdd5b21ce7b mt105/Voices/Zarvox.SpeechVoice/Contents/Resources/VoiceDescription
0748ffa74ed0dd9c765b8649067b4dc9f29fde918e58345bcddbe9558e120ae0 mt105/Voices/Zarvox.SpeechVoice/Contents/version.plist
227032a31d558ee7675af925758f8ca4da94b8c4646d4c94f697aca7cb22a9a4 mt105/lib/libstdc++.6.0.4.dylib
8d46b1e6dbbd9d2f5982f0d40be28b52359f4e1580800ed9b23ac8f315e0c672 mt106/MacinTalk
link:_CodeSignature/CodeResources mt106/SpeechDictionary/CodeResources
beeb045cd7726f677f59294c91efdb867becb1d85030b48815b23462cb70489f mt106/SpeechDictionary/Resources/CartLite
a4c40ccc85f71818a9c4b61fdafc82d989f927fe637d3025dc6280c932bf163e mt106/SpeechDictionary/Resources/CartNames
96214793f5c7a01d24cc53d28ec69fa63fb264626ff7d5196cb95bbe5fee295e mt106/SpeechDictionary/Resources/Homophones
c1567c6b8ee74c24a652522d56b06d0bddc722d1d9336a3eb722a248eae7a366 mt106/SpeechDictionary/Resources/Info.plist
6db09ad16e2b85badb90ebea805cdbcfeb219f5684a751ade7d60c9b2d5c3a40 mt106/SpeechDictionary/Resources/PhonemeSymbols
df166f61139107b47549d0b83114a1738d2a1fb985f5dc404a1144126d206ba0 mt106/SpeechDictionary/Resources/PrefixDictionary
3d53b4c60df827a03151b75da69c253ce9f6d72e86da4a526e21fe5a7f03d9df mt106/SpeechDictionary/Resources/SymbolDictionary
aff73ad28f553444d37ac0b88d5e98090f4d4716b546b62468ed28538adff828 mt106/SpeechDictionary/Resources/Tuples
8c4d8d764b816a46d07480656a1096e93a8010e4eb57e6e9c6db9f0f01bb8c24 mt106/SpeechDictionary/Resources/version.plist
f3ee958b743e9c90659b2a07daff5dc3aeb844440608286c6c0cb99f81aedc5c mt106/SpeechDictionary/SpeechDictionary
1f9eaaa2fa000f4c87bb9d4bc46e9b05adc15495a00c37a102dee4a0ae27fef8 mt106/SpeechDictionary/_CodeSignature/CodeResources
2cee54d3390cab2cc6f6220c34164bc19a1f36c5970f207e4fa65ba65fce95f2 mt106/Voices/Agnes.SpeechVoice/Contents/Info.plist
544e48db4f2477cb9114174b6be70b0a36bc6598df07b82fb62d2b77d12b1bed mt106/Voices/Agnes.SpeechVoice/Contents/Resources/PCMWave
d9b57707e541eb5a8db8c837a1dacc25b45d3a01f078f67521b66d098b9daa5f mt106/Voices/Agnes.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Agnes.SpeechVoice/Contents/version.plist
a3865c179fd53b7d2ef5c31b2defe01c19e28a4c3acbdb6a7eda5746a328a691 mt106/Voices/Albert.SpeechVoice/Contents/Info.plist
fe678bd0a5278a271489d37702c6683ab036a6f1049a60f157fda16b1ea5a8b6 mt106/Voices/Albert.SpeechVoice/Contents/Resources/PCMWave
78414d75dab5f4ce4fcf36e524cd1e7a58081ae7e810f89f1bbedef2507b1c06 mt106/Voices/Albert.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Albert.SpeechVoice/Contents/version.plist
bc23e514cb9ea081dd47e1b563e213664dfc20dbbe1012962414a8b118791d7f mt106/Voices/BadNews.SpeechVoice/Contents/Info.plist
2eb42905b1e49b80690b90e1c36fc62cd3780e9fe08e377eef4873e7e7de387b mt106/Voices/BadNews.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/BadNews.SpeechVoice/Contents/version.plist
ceffcb53bbaafb86b33d39f66d6ffcfc91a74901e2d3876790d7e5e8dc887a10 mt106/Voices/Bahh.SpeechVoice/Contents/Info.plist
48413c8a38e725fb8ce5e2bec51638edc96b1254735a956e9ec9460f94a1991c mt106/Voices/Bahh.SpeechVoice/Contents/Resources/PCMWave
92aaf27f60c7d10f99ff96b54fcca8952ec61ed35fe1c4e39205f4adb541c315 mt106/Voices/Bahh.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Bahh.SpeechVoice/Contents/version.plist
4c117932d19a4a56d357dc5d7d68e0ba67fe696c14f9a6535b3c06a43a2efd46 mt106/Voices/Bells.SpeechVoice/Contents/Info.plist
de3308f18adf2638af13821ea190ee1a8e4e062632cc89f7752468924d7b7462 mt106/Voices/Bells.SpeechVoice/Contents/Resources/PCMWave
9fb62f7c46408c82a5ad00fd6608f8f2b03b867c242d79815ae0c9659f4f33f6 mt106/Voices/Bells.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Bells.SpeechVoice/Contents/version.plist
965a899226e49a18abef5d2bf5b9fa388548885dad6f6d3730190e80b8163153 mt106/Voices/Boing.SpeechVoice/Contents/Info.plist
a19508ebc7c66fc4e5b000ef1cadf0040b0fdfe8e51674b931ddee9ad9644cdd mt106/Voices/Boing.SpeechVoice/Contents/Resources/PCMWave
fa6b4c3327ba799ec806372ac2c9cae780724b56fc494f27cb0432e09a3a379f mt106/Voices/Boing.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Boing.SpeechVoice/Contents/version.plist
eee3270ee2b8120420f4c2aa3d20ec355bf838780572a9e3bc42238ff38e0275 mt106/Voices/Bruce.SpeechVoice/Contents/Info.plist
ed19919c61476f228633d49239a0eee948f596a484dcab03f5ffff069a7fd5f9 mt106/Voices/Bruce.SpeechVoice/Contents/Resources/PCMWave
52fc6eaecb54eff627fb7b3f49a06a49c35a590284da9a6b40e02da340b1aea4 mt106/Voices/Bruce.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Bruce.SpeechVoice/Contents/version.plist
26b3c6de1987fd05f6642ff33cf542d92af79ceec0523c2d3af6ef924542cf42 mt106/Voices/Bubbles.SpeechVoice/Contents/Info.plist
3c7f9bfa11e496af42006b9990b62eb1731a1942e68b2d24dd29a76f64f896bb mt106/Voices/Bubbles.SpeechVoice/Contents/Resources/PCMWave
92f44607dd55d67cd47bb8df1d68379f6e4118ad9db9e0b18f833d4385e04bb1 mt106/Voices/Bubbles.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Bubbles.SpeechVoice/Contents/version.plist
cb3995c389d490d559f948003892b5a164626cfc4c6bb4904f924718845b7505 mt106/Voices/Cellos.SpeechVoice/Contents/Info.plist
fd2ea62a5f8ebea6c17e1c47662d1889df3ae1ddea4eb5a7660d798d7100259c mt106/Voices/Cellos.SpeechVoice/Contents/Resources/PCMWave
722b8d501c4de746528585f58410065f7923fb41c5955cc40f9beaccb4152599 mt106/Voices/Cellos.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Cellos.SpeechVoice/Contents/version.plist
412a351f0f3eea8a4ca0732a8533889e03ebffa11a4e4ec141af6b8682a57b24 mt106/Voices/Deranged.SpeechVoice/Contents/Info.plist
26f86dfbadb68a35fc76ace6ef380c81795b69b49aa4771b72e8f29576eaa1f9 mt106/Voices/Deranged.SpeechVoice/Contents/Resources/PCMWave
617037fb26fc4457bbbbfa36a810637e125e9f91d6332eeb5a50b54cab568ed3 mt106/Voices/Deranged.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Deranged.SpeechVoice/Contents/version.plist
59b8b5c3c4a1eaa6dc7c28da00ed09c0f0c2e7c692a3ca176881b3683858df52 mt106/Voices/Fred.SpeechVoice/Contents/Info.plist
992d7ab297c4ec9737cad4407492e4c616acdbfc9f7d5ce095dc0c1abb18a1fd mt106/Voices/Fred.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Fred.SpeechVoice/Contents/version.plist
3b7e56883a0a4eafbc487ca71c5591c951a6ac4526889fea067921fa8376b7db mt106/Voices/GoodNews.SpeechVoice/Contents/Info.plist
c2ab65e79b45fc967216f7ca400a60f3dd83ee6adb5cd8ed4a2760ccf0376c8e mt106/Voices/GoodNews.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/GoodNews.SpeechVoice/Contents/version.plist
e8373b2e535e7fc91624aa49afb5880f3fd17d6817d7e33592c8c7e04b0163d4 mt106/Voices/Hysterical.SpeechVoice/Contents/Info.plist
111c90a4e42fb07d89de19e581da82112aae5a0904ad5b93ac91e9190fe81911 mt106/Voices/Hysterical.SpeechVoice/Contents/Resources/PCMWave
242c870d30b60388fb9f1721d6e7bfffba81aabe07248dfee6750678687a4041 mt106/Voices/Hysterical.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Hysterical.SpeechVoice/Contents/version.plist
8be881fa4f0e47864d9062de9e397fcb16cf53582d0c93d6b40ed730d9a73e09 mt106/Voices/Junior.SpeechVoice/Contents/Info.plist
5f17a1827109e881af05268e07aec06f59a8729f8ff635f439742c482c68f6cb mt106/Voices/Junior.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Junior.SpeechVoice/Contents/version.plist
5580e44dce7ecd933539daa723cd82b2d94384a179b3895ae9685faefdc5202e mt106/Voices/Kathy.SpeechVoice/Contents/Info.plist
ce8bee14e67808647976a30167ef532c377088be51a4cf12bdfc43ece7b7e1ef mt106/Voices/Kathy.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Kathy.SpeechVoice/Contents/version.plist
5f6c15b79696a3d819e992336e1222f5574e064a40a98df1c4db3e3d78245cbc mt106/Voices/Organ.SpeechVoice/Contents/Info.plist
d801cb883112c14fe6aafb1d1db011d2f379eaa5d8b0d1db26eca50e9012a735 mt106/Voices/Organ.SpeechVoice/Contents/Resources/PCMWave
199cd5e760fe5e2ec7953e69296423a4d7357fe570b19b315686755ebb82af4c mt106/Voices/Organ.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Organ.SpeechVoice/Contents/version.plist
7fa756ea36a7671ac6022b3190505a6d08ecece8db84f3dd14ba007c0af17a98 mt106/Voices/Princess.SpeechVoice/Contents/Info.plist
9ab17fba52d962336cb376892c92e73d6b24456a5f0bea2bf02945bbd44216a4 mt106/Voices/Princess.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Princess.SpeechVoice/Contents/version.plist
85d327f3877a94a51321070089db60e775d4ec6825fe9a6f131bc3db7c4b4291 mt106/Voices/Ralph.SpeechVoice/Contents/Info.plist
c6fc335b6b02c310f63ffbd312b82470aed9f42d405c009970570b39a4677022 mt106/Voices/Ralph.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Ralph.SpeechVoice/Contents/version.plist
ef6ab3a3cca7594f4a28ced627934a4a67619c44c06a07c05f2ed0e14fab2895 mt106/Voices/Trinoids.SpeechVoice/Contents/Info.plist
23ea3a76315bc1bc6a00e6e2c41865e1f69257871ca4203b866d7f40d3cc6f05 mt106/Voices/Trinoids.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Trinoids.SpeechVoice/Contents/version.plist
3254c2b1488953c980f7713b7a3fef23d67d0357744a8a4b86c48cefd1d2dd77 mt106/Voices/Vicki.SpeechVoice/Contents/Info.plist
6c2a9ec188e5402588ec08cc26075a5ac74eeec27a54beef050fa112ecadde27 mt106/Voices/Vicki.SpeechVoice/Contents/Resources/PCMWave
c68af2545aa6c23fa2be6fcb21952a21835fed987775c031eef6b2f72df9d06c mt106/Voices/Vicki.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Vicki.SpeechVoice/Contents/version.plist
1a14f35b9bead3af7f18093f0a13f48368ce182a4068494fe917de35e132a368 mt106/Voices/Victoria.SpeechVoice/Contents/Info.plist
566398460b3732f3a4b1f2d100b26521e324e00fba3004f87854cfe3e9c10510 mt106/Voices/Victoria.SpeechVoice/Contents/Resources/PCMWave
62875168da28d447385111faee3ecf3cde85f6fb1872728737f544ede3f97939 mt106/Voices/Victoria.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Victoria.SpeechVoice/Contents/version.plist
6c6baed062ba43a75a33f40186cf0345efc1d17f8cf0c7262d269b2380da9ae8 mt106/Voices/Whisper.SpeechVoice/Contents/Info.plist
5e1c63d3d24edd926f60faadecec55ac13619c8e21f9e3aed3da7eed2b37da26 mt106/Voices/Whisper.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Whisper.SpeechVoice/Contents/version.plist
b8c65a40cddb7350dd3fc236ab8839dae6651b296758be3c3b9e82dcbe4ded1c mt106/Voices/Zarvox.SpeechVoice/Contents/Info.plist
46b59ccb1cd93bb6a3c3f77d5211f9a86f33057a038e1b36cbf5ecdd5b21ce7b mt106/Voices/Zarvox.SpeechVoice/Contents/Resources/VoiceDescription
1997390c9684f5bcac31b78c0cd000a7b44da264cabc23ab3f75b1776f8e35a3 mt106/Voices/Zarvox.SpeechVoice/Contents/version.plist
0d1c4d94edf1b4275de9a924cce24f5897231f7fcc1ba39f251b9e281a956694 mt106/lib/libstdc++.6.0.9.dylib
ca3e43222a3fd53f1aad1668dfaac5a9a8bf24412ee53eed71742d4788ae4c5e mt107/MacinTalk
9e998e1f3947939eaa986a40d37277bfacb4d1d40942e3063686ebaf35c7e1cd mt107/SpeechDictionary/Resources/CartLiteEng
b4bc0274d5a8f3e756e86160a91143eb9f908e9bcf170d56ebd969cb00b2362e mt107/SpeechDictionary/Resources/CartNamesEng
a29f87370be2163e0f50208eefb6dd0655e4da84a87443c128c366d98fcfa836 mt107/SpeechDictionary/Resources/HomophonesEng
b75e64be66ef2ca825ca5069fecac2ba237360186a1d4bd0e9e900760eef5499 mt107/SpeechDictionary/Resources/Info.plist
6db09ad16e2b85badb90ebea805cdbcfeb219f5684a751ade7d60c9b2d5c3a40 mt107/SpeechDictionary/Resources/PhonemeSymbolsEng
209f11f918967c2b98afced749d9744286ac5499437ea8354d33fcfc4413c755 mt107/SpeechDictionary/Resources/PrefixDictionaryEng
ae2edcdc3c19f196ab2ac5195f64207223177532c2d51ffe207ff7ffe9037b53 mt107/SpeechDictionary/Resources/TuplesEng
8edb7d9e237570cbacdce90abb1ccf756bb0a9d31abd6989ff82ff795825fc81 mt107/SpeechDictionary/Resources/version.plist
7e5094053161abf99b13ebd7ecc5c4da2d48d4c5812f7615fdf5b9759032cb20 mt107/SpeechDictionary/SpeechDictionary
109b5f7ff166b657b381ced3f5c511bbdf76d7e615154c11f575c880c1c6e1d5 mt107/Voices/Agnes.SpeechVoice/Contents/Info.plist
544e48db4f2477cb9114174b6be70b0a36bc6598df07b82fb62d2b77d12b1bed mt107/Voices/Agnes.SpeechVoice/Contents/Resources/PCMWave
d9b57707e541eb5a8db8c837a1dacc25b45d3a01f078f67521b66d098b9daa5f mt107/Voices/Agnes.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Agnes.SpeechVoice/Contents/version.plist
289a6841a750caa8e3dd2f723d182c601ee6b2e9bdea6bdf6125ec57ab6fccbd mt107/Voices/Albert.SpeechVoice/Contents/Info.plist
fe678bd0a5278a271489d37702c6683ab036a6f1049a60f157fda16b1ea5a8b6 mt107/Voices/Albert.SpeechVoice/Contents/Resources/PCMWave
78414d75dab5f4ce4fcf36e524cd1e7a58081ae7e810f89f1bbedef2507b1c06 mt107/Voices/Albert.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Albert.SpeechVoice/Contents/version.plist
637b10b22f69b03232ef9fe138908dbaae8df29da8f44a56d42a925168e97fa0 mt107/Voices/BadNews.SpeechVoice/Contents/Info.plist
2eb42905b1e49b80690b90e1c36fc62cd3780e9fe08e377eef4873e7e7de387b mt107/Voices/BadNews.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/BadNews.SpeechVoice/Contents/version.plist
d6ad0df89fbb4336dea77632d1adfc3e0aa7ae10b5f6e9c56ec8c87d309c0e88 mt107/Voices/Bahh.SpeechVoice/Contents/Info.plist
48413c8a38e725fb8ce5e2bec51638edc96b1254735a956e9ec9460f94a1991c mt107/Voices/Bahh.SpeechVoice/Contents/Resources/PCMWave
92aaf27f60c7d10f99ff96b54fcca8952ec61ed35fe1c4e39205f4adb541c315 mt107/Voices/Bahh.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Bahh.SpeechVoice/Contents/version.plist
5f731409888bd95020075ed803945a0c0f2402c8b82eb4819845bc7d65a17d6a mt107/Voices/Bells.SpeechVoice/Contents/Info.plist
de3308f18adf2638af13821ea190ee1a8e4e062632cc89f7752468924d7b7462 mt107/Voices/Bells.SpeechVoice/Contents/Resources/PCMWave
9fb62f7c46408c82a5ad00fd6608f8f2b03b867c242d79815ae0c9659f4f33f6 mt107/Voices/Bells.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Bells.SpeechVoice/Contents/version.plist
8c61eee76305f188099bf75b529b470a680e184f549e5bb551dd65d251b5210b mt107/Voices/Boing.SpeechVoice/Contents/Info.plist
a19508ebc7c66fc4e5b000ef1cadf0040b0fdfe8e51674b931ddee9ad9644cdd mt107/Voices/Boing.SpeechVoice/Contents/Resources/PCMWave
fa6b4c3327ba799ec806372ac2c9cae780724b56fc494f27cb0432e09a3a379f mt107/Voices/Boing.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Boing.SpeechVoice/Contents/version.plist
2dc4cc35804fe1707761ef615d0569a0675c25cdd79390c247f2d67611a645da mt107/Voices/Bruce.SpeechVoice/Contents/Info.plist
ed19919c61476f228633d49239a0eee948f596a484dcab03f5ffff069a7fd5f9 mt107/Voices/Bruce.SpeechVoice/Contents/Resources/PCMWave
52fc6eaecb54eff627fb7b3f49a06a49c35a590284da9a6b40e02da340b1aea4 mt107/Voices/Bruce.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Bruce.SpeechVoice/Contents/version.plist
7b6e5728b573e7fb5e078ef0c033526f173e562b04ccc4a2e089ac889f322798 mt107/Voices/Bubbles.SpeechVoice/Contents/Info.plist
3c7f9bfa11e496af42006b9990b62eb1731a1942e68b2d24dd29a76f64f896bb mt107/Voices/Bubbles.SpeechVoice/Contents/Resources/PCMWave
92f44607dd55d67cd47bb8df1d68379f6e4118ad9db9e0b18f833d4385e04bb1 mt107/Voices/Bubbles.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Bubbles.SpeechVoice/Contents/version.plist
0afb611a58b13448012e61489baac6f0b6c1a1f2a3d48d4f295eede823d4339b mt107/Voices/Cellos.SpeechVoice/Contents/Info.plist
fd2ea62a5f8ebea6c17e1c47662d1889df3ae1ddea4eb5a7660d798d7100259c mt107/Voices/Cellos.SpeechVoice/Contents/Resources/PCMWave
722b8d501c4de746528585f58410065f7923fb41c5955cc40f9beaccb4152599 mt107/Voices/Cellos.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Cellos.SpeechVoice/Contents/version.plist
fb453bae1a4de30288857d76d94f374d1ffa5850390d9d69f1886d3bad2b05e3 mt107/Voices/Deranged.SpeechVoice/Contents/Info.plist
26f86dfbadb68a35fc76ace6ef380c81795b69b49aa4771b72e8f29576eaa1f9 mt107/Voices/Deranged.SpeechVoice/Contents/Resources/PCMWave
617037fb26fc4457bbbbfa36a810637e125e9f91d6332eeb5a50b54cab568ed3 mt107/Voices/Deranged.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Deranged.SpeechVoice/Contents/version.plist
b74bbfaa9999df681a7c0086e39318cebb3d600e89a9add4304a2608d638d063 mt107/Voices/Fred.SpeechVoice/Contents/Info.plist
992d7ab297c4ec9737cad4407492e4c616acdbfc9f7d5ce095dc0c1abb18a1fd mt107/Voices/Fred.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Fred.SpeechVoice/Contents/version.plist
bfdda8f7387bea4077ec13d892fb984f45f81c75009799760947b62dcb03ce33 mt107/Voices/GoodNews.SpeechVoice/Contents/Info.plist
c2ab65e79b45fc967216f7ca400a60f3dd83ee6adb5cd8ed4a2760ccf0376c8e mt107/Voices/GoodNews.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/GoodNews.SpeechVoice/Contents/version.plist
9ca53f1ce6934525e0aa27624cae7a9bd83f98775aa9569936e00b299656fe58 mt107/Voices/Hysterical.SpeechVoice/Contents/Info.plist
111c90a4e42fb07d89de19e581da82112aae5a0904ad5b93ac91e9190fe81911 mt107/Voices/Hysterical.SpeechVoice/Contents/Resources/PCMWave
242c870d30b60388fb9f1721d6e7bfffba81aabe07248dfee6750678687a4041 mt107/Voices/Hysterical.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Hysterical.SpeechVoice/Contents/version.plist
9be2b9095402db5273378d4b99daa401b83e8bd068024328474e28fd4afa1785 mt107/Voices/Junior.SpeechVoice/Contents/Info.plist
5f17a1827109e881af05268e07aec06f59a8729f8ff635f439742c482c68f6cb mt107/Voices/Junior.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Junior.SpeechVoice/Contents/version.plist
03c82438a040af952800f5138e4a7c07d8db29750e205816a0a48d86fb129ea6 mt107/Voices/Kathy.SpeechVoice/Contents/Info.plist
ce8bee14e67808647976a30167ef532c377088be51a4cf12bdfc43ece7b7e1ef mt107/Voices/Kathy.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Kathy.SpeechVoice/Contents/version.plist
633ec8179acb0250b1d890e0d124c80fea0ec257431f49c644d61957677425c6 mt107/Voices/Organ.SpeechVoice/Contents/Info.plist
d801cb883112c14fe6aafb1d1db011d2f379eaa5d8b0d1db26eca50e9012a735 mt107/Voices/Organ.SpeechVoice/Contents/Resources/PCMWave
199cd5e760fe5e2ec7953e69296423a4d7357fe570b19b315686755ebb82af4c mt107/Voices/Organ.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Organ.SpeechVoice/Contents/version.plist
d08596f930754e8449fc527a0cc11f1b5ede3061ca5d951727c28fcef1b9e07d mt107/Voices/Princess.SpeechVoice/Contents/Info.plist
9ab17fba52d962336cb376892c92e73d6b24456a5f0bea2bf02945bbd44216a4 mt107/Voices/Princess.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Princess.SpeechVoice/Contents/version.plist
9c538e61e65f98466086ea5ea67c3927ed92572eefd599a9cfacc0b0c694b4b1 mt107/Voices/Ralph.SpeechVoice/Contents/Info.plist
c6fc335b6b02c310f63ffbd312b82470aed9f42d405c009970570b39a4677022 mt107/Voices/Ralph.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Ralph.SpeechVoice/Contents/version.plist
99062a9680ab703dd27f8d7f4f84abde834f367192c69e4fcc4c083e87a268b4 mt107/Voices/Trinoids.SpeechVoice/Contents/Info.plist
23ea3a76315bc1bc6a00e6e2c41865e1f69257871ca4203b866d7f40d3cc6f05 mt107/Voices/Trinoids.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Trinoids.SpeechVoice/Contents/version.plist
51fa101347efbc7920f172166a66a0a7fe9820cf3bfc486e1157fd49973a4955 mt107/Voices/Vicki.SpeechVoice/Contents/Info.plist
6c2a9ec188e5402588ec08cc26075a5ac74eeec27a54beef050fa112ecadde27 mt107/Voices/Vicki.SpeechVoice/Contents/Resources/PCMWave
c68af2545aa6c23fa2be6fcb21952a21835fed987775c031eef6b2f72df9d06c mt107/Voices/Vicki.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Vicki.SpeechVoice/Contents/version.plist
28b18bdb53b1b3edb440ce17646eef59105b1cbc4efd53328b94c7c431532586 mt107/Voices/Victoria.SpeechVoice/Contents/Info.plist
566398460b3732f3a4b1f2d100b26521e324e00fba3004f87854cfe3e9c10510 mt107/Voices/Victoria.SpeechVoice/Contents/Resources/PCMWave
62875168da28d447385111faee3ecf3cde85f6fb1872728737f544ede3f97939 mt107/Voices/Victoria.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Victoria.SpeechVoice/Contents/version.plist
ed8f244ce5170d8016bcd0175e984672c1ce8a9013370d7d68ee0635b5937b0d mt107/Voices/Whisper.SpeechVoice/Contents/Info.plist
5e1c63d3d24edd926f60faadecec55ac13619c8e21f9e3aed3da7eed2b37da26 mt107/Voices/Whisper.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Whisper.SpeechVoice/Contents/version.plist
a23572e0f451a6c65e55f645b3afe4371794c1444914df43589b830f01ff1dbf mt107/Voices/Zarvox.SpeechVoice/Contents/Info.plist
46b59ccb1cd93bb6a3c3f77d5211f9a86f33057a038e1b36cbf5ecdd5b21ce7b mt107/Voices/Zarvox.SpeechVoice/Contents/Resources/VoiceDescription
a7c8f11e3d854b1d366323f9182147ce65066090fbb3b59fc49aed66f3a47805 mt107/Voices/Zarvox.SpeechVoice/Contents/version.plist
a63807a5a399f281db34feb15f137614ecb59af006ab046e4caeb2d72fdb9126 mt107/lib/libc++abi.dylib
4e057cc164608b47cc442eb88a93233b09a5ed0f749713ff62003b526a03c40f mt107/lib/libstdc++.6.0.9.dylib
"""
}
