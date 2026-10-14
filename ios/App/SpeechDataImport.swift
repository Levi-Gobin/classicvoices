import Foundation

/// Adds Mac OS X speech files someone already has, picked in Files or Finder:
///
/// - a speech data folder, laid out as panthera-speech's are:
///   Speech/Synthesizers/MacinTalk.SpeechSynthesizer, Speech/Voices,
///   SpeechDictionary.framework and the C++ libraries beside them;
/// - a whole Mac OS X disk, or its System/Library/Speech folder;
/// - single MacinTalk voices (Alex.SpeechVoice).
///
/// Everything is copied (never moved) into the App Group, where the speech
/// extension looks: <group>/EnhancedVoices/<gen>/{MacinTalk, SpeechDictionary,
/// lib, Voices}. Which Mac OS X a MacinTalk came from is read from the
/// processors it was built for.
enum SpeechDataImport {
    /// What a pick turned up.
    struct Found {
        var engine: URL?            // .../MacinTalk.SpeechSynthesizer/Contents/MacOS/MacinTalk
        var dictionary: URL?        // .../SpeechDictionary.framework/Versions/A
        var libraries: [URL] = []
        var voices: [URL] = []      // MacinTalk .SpeechVoice folders
        var otherVoices: [String] = []  // Nuance .SpeechVoice folders, which come from Apple's servers instead
        var isEmpty: Bool { engine == nil && dictionary == nil && libraries.isEmpty && voices.isEmpty }
    }

    struct Outcome {
        var gen: String?
        var added: [String] = []    // what was copied, for the summary
        var missing: [String] = []  // what this release still needs, in plain words
        var test: String?           // result of speaking with it
        var needsRelease = false    // voices only, and it isn't clear for which release
    }

    static let libraryNames = ["libstdc++.6.0.4.dylib", "libstdc++.6.0.9.dylib", "libc++abi.dylib"]

    /// The C++ libraries each release's MacinTalk loads, from its /usr/lib.
    static func requiredLibraries(_ gen: String) -> [String] {
        switch gen {
        case "mt105": ["libstdc++.6.0.4.dylib"]
        case "mt106": ["libstdc++.6.0.9.dylib"]
        case "mt107": ["libstdc++.6.0.9.dylib", "libc++abi.dylib"]
        default: []
        }
    }

    static func releaseName(_ gen: String) -> String {
        VoiceCatalog.macinTalkEras.first { $0.0 == gen }.map { "Mac OS X \($0.2) \($0.1)" } ?? gen
    }

    // MARK: - looking

    static func scan(_ urls: [URL]) -> Found {
        var found = Found()
        let fm = FileManager.default
        for url in urls {
            let access = url.startAccessingSecurityScopedResource()
            defer { if access { url.stopAccessingSecurityScopedResource() } }
            look(at: url, &found)
            guard let it = fm.enumerator(at: url, includingPropertiesForKeys: [.isDirectoryKey],
                                         options: [.skipsHiddenFiles]) else { continue }
            for case let item as URL in it {
                if it.level > 9 { it.skipDescendants(); continue }
                // Only for folders: after a file it would skip the file's siblings.
                let isDir = (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
                if look(at: item, &found), isDir { it.skipDescendants() }
            }
        }
        return found
    }

    /// Notes one item; true when there's no need to look inside it.
    @discardableResult
    private static func look(at item: URL, _ found: inout Found) -> Bool {
        let fm = FileManager.default
        let name = item.lastPathComponent
        if name == "MacinTalk.SpeechSynthesizer" {
            let bin = item.appendingPathComponent("Contents/MacOS/MacinTalk")
            if fm.fileExists(atPath: bin.path) { found.engine = bin }
            return true
        }
        if name == "MacinTalk", found.engine == nil, era(ofEngine: item) != nil {
            found.engine = item  // a bare engine, as copied out by hand
            return true
        }
        if name == "SpeechDictionary.framework" {
            let a = item.appendingPathComponent("Versions/A")
            if fm.fileExists(atPath: a.appendingPathComponent("SpeechDictionary").path) { found.dictionary = a }
            return true
        }
        if name == "SpeechDictionary", found.dictionary == nil,
           fm.fileExists(atPath: item.appendingPathComponent("SpeechDictionary").path),
           fm.fileExists(atPath: item.appendingPathComponent("Resources").path) {
            found.dictionary = item
            return true
        }
        if libraryNames.contains(name), !found.libraries.contains(where: { $0.lastPathComponent == name }) {
            found.libraries.append(item)
            return true
        }
        if name.hasSuffix(".SpeechVoice") {
            if fm.fileExists(atPath: item.appendingPathComponent("Contents/Resources/VoiceDescription").path) {
                if !found.voices.contains(where: { $0.lastPathComponent == name }) { found.voices.append(item) }
            } else {
                found.otherVoices.append(String(name.dropLast(".SpeechVoice".count)))
            }
            return true
        }
        // Bundles and big folders that never hold speech files.
        let skip: Set<String> = ["Frameworks", "PrivateFrameworks", "Fonts", "Caches", "Extensions",
                                 "Documentation", "Developer", "Applications", "Users", "private", "Library.noindex"]
        return skip.contains(name) || name.hasSuffix(".app") || name.hasSuffix(".bundle") && name != "Speech"
    }

    /// Which release a MacinTalk binary is, from the processors in its fat header:
    /// Tiger i386+ppc, Leopard adds both 64-bit ones, Snow Leopard drops
    /// ppc64, Lion drops PowerPC.
    static func era(ofEngine url: URL) -> String? {
        guard let fh = try? FileHandle(forReadingFrom: url), let head = try? fh.read(upToCount: 8 + 20 * 8) else { return nil }
        try? fh.close()
        guard head.count >= 8, head.be32(0) == 0xCAFE_BABE else { return nil }
        let n = Int(head.be32(4))
        guard n > 0, n < 8, head.count >= 8 + 20 * n else { return nil }
        let cpus = Set((0..<n).map { head.be32(8 + 20 * $0) })
        let i386: UInt32 = 7, x64: UInt32 = 0x0100_0007, ppc: UInt32 = 18, ppc64: UInt32 = 0x0100_0012
        guard cpus.contains(i386) || cpus.contains(x64) else { return nil }
        if cpus.contains(ppc64) { return "mt105" }
        if cpus.contains(ppc) { return cpus.contains(x64) ? "mt106" : "mt104" }
        return "mt107"
    }

    /// Tiger 10.4.5 and later call Apple's "Don't Steal Mac OS X" check.
    private static func callsDSMOS(_ url: URL) -> Bool {
        guard let d = try? Data(contentsOf: url, options: .mappedIfSafe) else { return false }
        return d.range(of: Data("___commpage_dsmos".utf8)) != nil
    }

    /// Releases whose engine is here, bundled or added.
    static func installedReleases() -> [String] {
        VoiceCatalog.macinTalkEras.map(\.0).filter { VoiceCatalog.hasMacinTalkEngine($0) }
    }

    // MARK: - copying

    /// Copies what was found. `gen` decides the release when the files can't
    /// (voices picked on their own).
    static func install(_ found: Found, gen chosen: String? = nil, from urls: [URL]) throws -> Outcome {
        var out = Outcome()
        let engineGen = found.engine.flatMap(era(ofEngine:))
        if found.engine != nil && engineGen == nil {
            throw ImportError("That MacinTalk is for PowerPC Macs only. Use the files from an Intel Mac\u{2019}s install instead.")
        }
        if engineGen == "mt104", [found.engine, found.dictionary?.appendingPathComponent("SpeechDictionary")]
            .contains(where: { $0.map(callsDSMOS) ?? false }) {
            throw ImportError("This is MacinTalk 3.4, from Tiger 10.4.5 or later. It checks for genuine Apple hardware, so it can\u{2019}t run here. Use the first Intel Tiger disc (10.4.4, MacinTalk 3.3), or Leopard\u{2019}s files, which have Fred and the same voices.")
        }
        if found.engine == nil && found.voices.isEmpty { throw nothingFound(found) }
        let installed = installedReleases()
        guard let gen = engineGen ?? chosen ?? (installed.count == 1 ? installed[0] : nil) else {
            if found.voices.isEmpty { throw nothingFound(found) }
            if installed.isEmpty {
                throw ImportError("Voices alone can\u{2019}t speak: they need the MacinTalk engine from the same Mac OS X. Choose the folder that also holds Speech/Synthesizers, or add the engine first.")
            }
            out.needsRelease = true
            return out
        }
        out.gen = gen
        guard let base = VoiceCatalog.enhancedRoot?.appendingPathComponent(gen) else {
            throw ImportError("The app can\u{2019}t reach its shared storage (App Group), so it can\u{2019}t keep voices. The Diagnostics page says more.")
        }
        let fm = FileManager.default
        let access = urls.map { $0.startAccessingSecurityScopedResource() }
        defer { for (u, a) in zip(urls, access) where a { u.stopAccessingSecurityScopedResource() } }
        try fm.createDirectory(at: base, withIntermediateDirectories: true)

        func copy(_ src: URL, _ dst: URL) throws {
            try? fm.removeItem(at: dst)
            try fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.copyItem(at: src.resolvingSymlinksInPath(), to: dst)
        }
        if let e = found.engine {
            try copy(e, base.appendingPathComponent("MacinTalk"))
            out.added.append("the \(releaseName(gen)) MacinTalk engine")
        }
        if let d = found.dictionary, engineGen != nil || !VoiceCatalog.hasMacinTalkEngine(gen) {
            try copy(d, base.appendingPathComponent("SpeechDictionary"))
            out.added.append("its pronunciation dictionary")
        }
        for lib in found.libraries where requiredLibraries(gen).contains(lib.lastPathComponent) {
            try copy(lib, base.appendingPathComponent("lib").appendingPathComponent(lib.lastPathComponent))
            out.added.append(lib.lastPathComponent)
        }
        for v in found.voices {
            try copy(v, base.appendingPathComponent("Voices").appendingPathComponent(v.lastPathComponent))
        }
        if !found.voices.isEmpty {
            let names = found.voices.map { String($0.lastPathComponent.dropLast(".SpeechVoice".count)) }
            out.added.append(names.count == 1 ? "the voice \(names[0])" : "\(names.count) voices")
        }
        out.missing = missing(gen)
        if out.missing.isEmpty { out.test = test(gen, voices: found.voices) }
        return out
    }

    /// What a release still needs before it can speak, said so someone can find it.
    static func missing(_ gen: String) -> [String] {
        guard !VoiceCatalog.hasBundledMacinTalk(gen) else { return [] }
        let base = VoiceCatalog.enhancedRoot?.appendingPathComponent(gen)
        func has(_ p: String) -> Bool { base.map { FileManager.default.fileExists(atPath: $0.appendingPathComponent(p).path) } ?? false }
        var out: [String] = []
        if !has("MacinTalk") {
            out.append("the MacinTalk engine: System/Library/Speech/Synthesizers/MacinTalk.SpeechSynthesizer")
        }
        if !has("SpeechDictionary/SpeechDictionary") {
            out.append("the pronunciation dictionary: System/Library/PrivateFrameworks/SpeechDictionary.framework")
        }
        for lib in requiredLibraries(gen) where !has("lib/\(lib)") {
            out.append("\(lib), from usr/lib")
        }
        let voices = (try? FileManager.default.contentsOfDirectory(atPath: base?.appendingPathComponent("Voices").path ?? "")) ?? []
        if !voices.contains(where: { $0.hasSuffix(".SpeechVoice") }) {
            out.append("its voices: System/Library/Speech/Voices")
        }
        return out
    }

    /// Speaks a sentence with the new files, as the speech extension will.
    private static func test(_ gen: String, voices: [URL]) -> String {
        guard let root = VoiceCatalog.root else { return "Not tested: the app\u{2019}s own voices are missing." }
        let dir = VoiceCatalog.enhancedRoot?.appendingPathComponent(gen).appendingPathComponent("Voices")
        let names = (voices.map(\.lastPathComponent) +
                     ((try? FileManager.default.contentsOfDirectory(atPath: dir?.path ?? "")) ?? []).sorted())
            .filter { $0.hasSuffix(".SpeechVoice") }
        guard let first = names.first(where: { $0 != "Alex.SpeechVoice" }) ?? names.first else { return "" }
        let voice = String(first.dropLast(".SpeechVoice".count))
        var open = 0.0, firstAudio = 0.0, total = 0.0, audio = 0.0
        let rc = vxs_benchmark(root.path, VoiceCatalog.enhancedRoot?.path, "\(gen)/\(voice)", "Testing.", 100,
                               &open, &firstAudio, &total, &audio)
        return rc == 0 && audio > 0 ? "\(voice) speaks." : "\(voice) didn\u{2019}t speak (error \(rc)). Make sure every file came from the same Mac OS X release."
    }

    private static func nothingFound(_ found: Found) -> ImportError {
        if !found.otherVoices.isEmpty {
            return ImportError("\(found.otherVoices.joined(separator: ", ")) \(found.otherVoices.count == 1 ? "is a voice" : "are voices") Classic Voices downloads from Apple itself: find \(found.otherVoices.count == 1 ? "it" : "them") in the macOS tab.")
        }
        return ImportError("No Mac OS X speech files there. Choose a folder that holds System/Library/Speech, a speech data folder, or .SpeechVoice voices from Mac OS X 10.4 to 10.7.")
    }

    struct ImportError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }
}
