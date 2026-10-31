import Foundation

/// Apple's own text rules for the Vocalizer voices. Each iOS release shipped
/// `TTSResources/<language>/user_rules.txt` next to each language, and each
/// Mac OS X voice a `user_rules.dat` of its own: regular expressions run over
/// the text before the engine saw it, so "iOS" became "I O S", "OS X" became
/// "OS ten", "St" became "Street" and so on. The engine itself never asks for
/// the file, so the rules are applied here, the same way, before the text is
/// handed over.
///
/// File format, one rule per line: `/pattern/flags --> "replacement"`, with
/// `$1`-style groups, `\xHH` and `\\` escapes in the replacement, and the
/// flags `i` and `m`.
final class PronunciationRules: @unchecked Sendable {
    private let patterns: [String]
    private let options: [NSRegularExpression.Options]
    private let templates: [String]
    /// Which rules could match a text (core/vxrules.c): a short phrase runs a
    /// handful of a set's thousands of rules instead of all of them.
    private let index: OpaquePointer?
    /// Compiled the first time a rule could match; nil after a failed compile.
    private var compiled: [NSRegularExpression?]
    private var tried: [Bool]
    private let compileLock = NSLock()

    var count: Int { patterns.count }

    private init(_ rules: [(pattern: String, options: NSRegularExpression.Options, template: String)]) {
        patterns = rules.map(\.pattern)
        options = rules.map(\.options)
        templates = rules.map(\.template)
        compiled = Array(repeating: nil, count: rules.count)
        tried = Array(repeating: false, count: rules.count)
        index = vx_rule_index_new()
        for (i, r) in rules.enumerated() {
            vx_rule_index_add(index, UInt32(i), r.pattern, r.options.contains(.caseInsensitive) ? 1 : 0)
        }
    }

    deinit { vx_rule_index_free(index) }

    /// A rules file's text (or several files' one after another).
    convenience init(text: String) { self.init(Self.parse(text)) }

    private func regex(_ i: Int) -> NSRegularExpression? {
        compileLock.lock(); defer { compileLock.unlock() }
        if !tried[i] {
            tried[i] = true
            compiled[i] = try? NSRegularExpression(pattern: patterns[i], options: options[i])
        }
        return compiled[i]
    }

    private func mark(_ text: String, _ marks: inout [UInt8]) {
        let n = marks.count
        text.withCString { c in
            marks.withUnsafeMutableBufferPointer { vx_rule_index_mark(index, c, $0.baseAddress, n) }
        }
    }

    /// The rules in order, each over the text the ones before it left; a
    /// rule that can't match is skipped without running it.
    func apply(_ text: String) -> String {
        let n = patterns.count
        guard n > 0 else { return text }
        var marks = [UInt8](repeating: 0, count: n)
        mark(text, &marks)
        var s = text
        for i in 0..<n where marks[i] != 0 {
            guard let re = regex(i) else { continue }
            let out = re.stringByReplacingMatches(in: s, range: NSRange(s.startIndex..., in: s), withTemplate: templates[i])
            if out != s {
                s = out
                mark(s, &marks)  // the new text may bring later rules into play
            }
        }
        return s
    }

    static func parse(_ text: String) -> [(pattern: String, options: NSRegularExpression.Options, template: String)] {
        var out: [(pattern: String, options: NSRegularExpression.Options, template: String)] = []
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            guard line.hasPrefix("/"), let arrow = line.range(of: "-->") else { continue }
            let left = line[..<arrow.lowerBound].trimmingCharacters(in: .whitespaces)
            var right = line[arrow.upperBound...].trimmingCharacters(in: .whitespaces)
            if let hash = right.range(of: "\" #") { right = String(right[..<hash.lowerBound]) + "\"" }  // trailing comment
            guard let close = left.lastIndex(of: "/"), close > left.startIndex,
                  right.hasPrefix("\""), right.hasSuffix("\""), right.count >= 2 else { continue }
            let pattern = String(left[left.index(after: left.startIndex)..<close])
            let flags = left[left.index(after: close)...]
            var options: NSRegularExpression.Options = []
            if flags.contains("i") { options.insert(.caseInsensitive) }
            if flags.contains("m") { options.insert(.anchorsMatchLines) }
            let replacement = unescape(String(right.dropFirst().dropLast()))
            out.append((pattern, options, template(replacement)))
        }
        return out
    }

    /// C-style escapes in the file's replacement strings.
    private static func unescape(_ s: String) -> String {
        var out = ""
        var it = s.makeIterator()
        while let c = it.next() {
            guard c == "\\" else { out.append(c); continue }
            guard let n = it.next() else { break }
            switch n {
            case "x":
                var hex = ""
                var pending: Character? = nil
                while hex.count < 2, let h = it.next() {
                    if h.isHexDigit { hex.append(h) } else { pending = h; break }
                }
                if let v = UInt32(hex, radix: 16), let u = Unicode.Scalar(v) { out.unicodeScalars.append(u) }
                if let pending { out.append(pending) }
            case "n": out.append("\n")
            case "t": out.append("\t")
            case "e": out.append("\u{1b}")
            default: out.append(n)  // \\ \" \/
            }
        }
        return out
    }

    /// A literal string as an NSRegularExpression template, keeping `$1`...`$9`.
    private static func template(_ s: String) -> String {
        var out = ""
        var prevDollar = false
        for c in s {
            if prevDollar {
                prevDollar = false
                if c.isNumber { out.append("$"); out.append(c); continue }
                out.append("\\$")
            }
            if c == "$" { prevDollar = true; continue }
            if c == "\\" { out.append("\\\\"); continue }
            out.append(c)
        }
        if prevDollar { out.append("\\$") }
        return out
    }

    // MARK: - per voice

    nonisolated(unsafe) private static var cache: [String: PronunciationRules] = [:]
    private static let lock = NSLock()

    /// The rules the voice itself had: an iOS voice its own release's
    /// `<build>/TTSResources/<language>/user_rules.txt` (an enhanced voice its
    /// release's), a Mac OS X voice the `user_rules.dat` in its own folder
    /// (same format), downloaded or bundled. nil when it had none: some
    /// languages and Mac voices shipped without, and MacinTalk never had any.
    static func rules(forVoiceKey key: String) -> PronunciationRules? {
        file(forVoiceKey: key).flatMap { rules(files: [$0]) }
    }

    /// What a voice speaks with, by its two switches: its own rules (when
    /// "Apple's pronunciation rules" is on for it), then the newer pack's
    /// override rules and rules for its language (when "Newer pronunciation
    /// rules" is on and the pack is here). The engine runs the rule set
    /// loaded last first, and iOS 12 loaded the overrides last, hence their
    /// place. nil when neither applies.
    static func rules(forVoiceKey key: String, identifier: String, language: String,
                      settings: SpeechSettings) -> PronunciationRules? {
        var files: [URL] = []
        if settings.rulesEnabled(identifier), let own = file(forVoiceKey: key) { files.append(own) }
        if let pack = newerPack(forVoiceKey: key, identifier: identifier, language: language, settings: settings) {
            let dir = URL(fileURLWithPath: pack)
            files += ["vocalizer-override-rules.txt", "vocalizer-user-rules.txt"].map { dir.appendingPathComponent($0) }
        }
        return files.isEmpty ? nil : rules(files: files)
    }

    /// One set from the files that exist, in order; cached by the files used.
    private static func rules(files: [URL]) -> PronunciationRules? {
        let id = files.map(\.path).joined(separator: "\n")
        lock.lock()
        if let hit = cache[id] { lock.unlock(); return hit }
        lock.unlock()
        let texts = files.compactMap { try? String(contentsOf: $0, encoding: .utf8) }
        guard !texts.isEmpty else { return nil }
        let parsed = PronunciationRules(text: texts.joined(separator: "\n"))
        lock.lock(); cache[id] = parsed; lock.unlock()
        return parsed
    }

    private static func file(forVoiceKey key: String) -> URL? {
        let fm = FileManager.default
        if let entry = VoiceCatalog.entry(forKey: key) {
            if entry.macinTalk { return nil }
            if let dir = entry.macDir {
                return [VoiceCatalog.enhancedRoot, VoiceCatalog.root].compactMap { $0 }
                    .map { $0.appendingPathComponent("\(entry.build)/\(dir)/user_rules.dat") }
                    .first { fm.fileExists(atPath: $0.path) }
            }
            return VoiceCatalog.root?.appendingPathComponent("\(entry.build)/TTSResources/\(entry.language)/user_rules.txt")
        }
        // A key the catalog doesn't list ("ios6/en-US" for an unknown request)
        guard key.hasPrefix("ios"), let slash = key.firstIndex(of: "/") else { return nil }
        var build = String(key[..<slash])
        if build.hasSuffix("e") { build.removeLast() }
        return VoiceCatalog.root?.appendingPathComponent("\(build)/TTSResources/\(key[key.index(after: slash)...])/user_rules.txt")
    }

    // MARK: - the newer pack

    /// Apple's later pronunciation pack for a language (PronunciationPacks
    /// downloads them): the folder holding its dictionary and rules. The
    /// dictionary goes into the engine (vxs_set_rules_pack); the rules are
    /// run here, with the voice's own (inside the old engines they cost up to
    /// a tenth of a second per utterance). nil when it isn't here. The
    /// extension reads it from its store; the app from its own copy.
    static func newerPack(language: String) -> String? {
        let fm = FileManager.default
        let appCopy = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent(PronunciationPacks.folder)
        for base in [VoiceCatalog.enhancedRoot?.appendingPathComponent(PronunciationPacks.folder), appCopy].compactMap({ $0 }) {
            let dir = base.appendingPathComponent(language)
            if fm.fileExists(atPath: dir.appendingPathComponent("vocalizer-user-rules.txt").path) { return dir.path }
        }
        return nil
    }

    /// The pack for a voice when the newer rules are on for it; nil for
    /// MacinTalk, which they don't apply to.
    static func newerPack(forVoiceKey key: String, identifier: String, language: String,
                          settings: SpeechSettings) -> String? {
        guard !key.hasPrefix("mt"), !language.isEmpty, settings.newerRulesEnabled(identifier) else { return nil }
        return newerPack(language: language)
    }
}

/// Where Apple's later pronunciation packs live and how they're fetched: the
/// VoiceResources MobileAssets iOS 12 used, one per language (39, 4.6 MB in
/// all), each a dictionary plus rule files loaded straight into the engine.
enum PronunciationPacks {
    /// <store>/Pronunciation/<language>/{vocalizer-user-dict.dat, ...}
    static let folder = "Pronunciation"
    static let files = ["vocalizer-user-dict.dat", "vocalizer-user-rules.txt", "vocalizer-override-rules.txt"]
    static let catalog = URL(string: "https://mesu.apple.com/assets/com_apple_MobileAsset_VoiceServices_VoiceResources/com_apple_MobileAsset_VoiceServices_VoiceResources.xml")!
    /// The asset format iOS 12 asked for.
    static let compatibilityVersion = 8
}
