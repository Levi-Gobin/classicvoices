import Foundation

/// Community pronunciation dictionaries (github.com/nicopn/classicvoices-dicts), as ports/host/cv_dict.c reads
/// them for NVDA: one file per language code (en.dic, en-us.dic ...) for the Vocalizer voices and macintalk.dic
/// for MacinTalk, in NVDA's speech dictionary format (pattern, replacement, case sensitive, type 0 anywhere /
/// 1 regular expression / 2 whole word). In a replacement, ~~...~~ is X-SAMPA, said as those sounds; it stands
/// in the text as a private-use character until the phonemes go in, so later entries and Apple's rules can't
/// match inside it. A span followed by letters (a regular expression's \1: "boost" + "ed") gets the original
/// word told to the engine stream, which says the rest as the voice would in the whole word (core/vxstream.c).
///
/// The app fetches the files (`refreshAll`) into the App Group's Dictionaries folder, where the speech extension
/// reads them: the extension, which sees everything VoiceOver says, never goes on the network (a Mac build whose
/// extension had the network entitlement made axassetsd crash-loop while listing its voices, 2026-11-14; see
/// NOTES.md). The person's own entries are in Dictionaries/Mine/<file> (same format; used before everything else,
/// and even with the community dictionaries off), and Mine/<file>.removed lists the fetched entries they took out
/// (pattern TAB type), as cv_dict.c reads them.
final class CommunityDictionary: @unchecked Sendable {
    static let defaultRepo = "nicopn/classicvoices-dicts", defaultBranch = "main"
    static let every: TimeInterval = 6 * 3600

    /// The GitHub repository and branch the community dictionaries come from (Dictionaries/source.json, as the
    /// NVDA add-on keeps it): Nico's unless the person chose a fork.
    static var source: (repo: String, branch: String) {
        guard let dir = folder, let d = try? Data(contentsOf: dir.appendingPathComponent("source.json")),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: String],
              let repo = j["repo"], validRepo(repo) else { return (defaultRepo, defaultBranch) }
        return (repo, j["branch"].flatMap { $0.isEmpty ? nil : $0 } ?? defaultBranch)
    }

    static func validRepo(_ r: String) -> Bool {
        let parts = r.split(separator: "/", omittingEmptySubsequences: false)
        return parts.count == 2 && parts.allSatisfy { !$0.isEmpty && $0.allSatisfy { $0.isLetter || $0.isNumber || "-_.".contains($0) } }
    }

    /// owner/repo from what was typed ("owner/repo", a github.com address, with .git), or nil.
    static func parseRepo(_ text: String) -> String? {
        var t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        for p in ["https://", "http://", "git@github.com:", "www.github.com/", "github.com/"] where t.lowercased().hasPrefix(p) {
            t = String(t.dropFirst(p.count))
        }
        if t.lowercased().hasPrefix("github.com/") { t = String(t.dropFirst(11)) }
        t = t.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        if t.hasSuffix(".git") { t = String(t.dropLast(4)) }
        t = t.split(separator: "/").prefix(2).joined(separator: "/")
        return validRepo(t) ? t : nil
    }

    /// Another repository: what was fetched from the old one stops counting, and the next fetch is now.
    static func setSource(repo: String, branch: String) {
        guard let dir = folder else { return }
        let j = ["repo": repo, "branch": branch.isEmpty ? defaultBranch : branch]
        if let d = try? JSONSerialization.data(withJSONObject: j) { try? d.write(to: dir.appendingPathComponent("source.json"), options: .atomic) }
        try? FileManager.default.removeItem(at: dir.appendingPathComponent("state.json"))
    }

    /// While the dictionary editor is open (the app touches Dictionaries/editing every 15 seconds), the voices use
    /// no dictionary at all, so the patterns are heard uncorrected; a flag older than 45 seconds (the app was
    /// stopped) is ignored.
    static var editing: Bool {
        guard let dir = folder,
              let date = (try? FileManager.default.attributesOfItem(atPath: dir.appendingPathComponent("editing").path))?[.modificationDate] as? Date
        else { return false }
        return Date().timeIntervalSince(date) < 45
    }

    private let patterns: [String]
    private let caseless: [Bool]
    private let templates: [String]
    private let ipa: [String]  // per span (with choices: its first options, a fallback)
    private let choices: [Int: SpanChoices]  // the spans that have choices, (a|b), by span number
    private let index: OpaquePointer?
    /// Compiled the first time a text could match (core/vxrules.c says which): a big dictionary would otherwise
    /// compile thousands of expressions before its first word.
    private var compiled: [NSRegularExpression?]
    private var tried: [Bool]
    private let compileLock = NSLock()

    private static let spanBase: UInt32 = 0xF0000

    /// A span's choices: its X-SAMPA with U+0001 where each goes, and each one's options and its capture group's
    /// alternatives (as cv_dict.c: the option in the place of the alternative the group matched; the one after
    /// them when it matched none, or took no part).
    struct Choice { var alts: [String]?; var opts: [String]; var caseless: Bool }
    struct SpanChoices { var xs: String; var list: [Choice] }

    /// Markers around what a choice's group matched, after its span's placeholder.
    static let choiceOpen = Unicode.Scalar(0x10FF00)!, choiceClose = Unicode.Scalar(0x10FF01)!

    private init(entries: [(pattern: String, caseless: Bool, template: String)], ipa: [String],
                 choices: [Int: SpanChoices] = [:]) {
        patterns = entries.map(\.pattern)
        caseless = entries.map(\.caseless)
        templates = entries.map(\.template)
        compiled = Array(repeating: nil, count: entries.count)
        tried = Array(repeating: false, count: entries.count)
        self.ipa = ipa
        self.choices = choices
        index = vx_rule_index_new()
        for (i, e) in entries.enumerated() { vx_rule_index_add(index, UInt32(i), e.pattern, e.caseless ? 1 : 0) }
    }

    private func regex(_ i: Int) -> NSRegularExpression? {
        compileLock.lock(); defer { compileLock.unlock() }
        if !tried[i] {
            tried[i] = true
            compiled[i] = try? NSRegularExpression(pattern: patterns[i], options: caseless[i] ? [.caseInsensitive] : [])
        }
        return compiled[i]
    }

    deinit { vx_rule_index_free(index) }

    // MARK: - files

    /// The App Group's Dictionaries folder: written by the app, read by the extension; readable before the first
    /// unlock, as the voices are.
    static var folder: URL? { folderOnce }

    /// Looked up and created once per process: each voice request reads it.
    private static let folderOnce: URL? = {
        #if CV_DICT_TEST  // tests only (build/dictbench): a folder of their own instead of the App Group
        if let d = ProcessInfo.processInfo.environment["CV_DICT_DIR"] { return URL(fileURLWithPath: d) }
        #endif
        guard let group = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroup) else { return nil }
        let dir = group.appendingPathComponent("Dictionaries")
        #if os(macOS)
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("Mine"), withIntermediateDirectories: true)
        #else
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("Mine"), withIntermediateDirectories: true,
                                                 attributes: [.protectionKey: FileProtectionType.none])
        #endif
        return dir
    }()

    /// The files a voice uses, narrowest first.
    static func names(voiceKey: String, language: String) -> [String] {
        if voiceKey.hasPrefix("mt") { return ["macintalk.dic"] }
        let lang = language.replacingOccurrences(of: "_", with: "-").lowercased()
        guard !lang.isEmpty else { return [] }
        var out = [lang + ".dic"]
        if let dash = lang.firstIndex(of: "-") { out.append(String(lang[..<dash]) + ".dic") }
        return out
    }

    nonisolated(unsafe) private static var cache: [String: CommunityDictionary] = [:]
    private static let lock = NSLock()

    /// The dictionary for a voice: the person's own entries, then the fetched files when `community` is on (less
    /// the entries the person removed); nil when there's nothing. Read again when a file changes.
    static func forVoice(key: String, language: String, community: Bool) -> CommunityDictionary? {
        guard let dir = folder, !editing else { return nil }
        let fm = FileManager.default
        var files: [DictFile] = []
        var id = ""
        func stamp(_ u: URL) -> String? {
            guard let attrs = try? fm.attributesOfItem(atPath: u.path) else { return nil }
            return "\(u.path):\(attrs[.size] ?? 0):\((attrs[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)|"
        }
        let names = names(voiceKey: key, language: language)
        for layer in 0..<(community ? 2 : 1) {
            for name in names {
                let mine = dir.appendingPathComponent("Mine").appendingPathComponent(name)
                let file = layer == 0 ? mine : dir.appendingPathComponent(name)
                guard let st = stamp(file) else { continue }
                id += st
                var removed: URL?
                if layer == 1 {
                    let r = mine.appendingPathExtension("removed")
                    if let rs = stamp(r) { removed = r; id += "-" + rs }
                }
                files.append(DictFile(url: file, removed: removed))
            }
        }
        guard !files.isEmpty else { return nil }
        let sig = files.map(\.url.path).joined(separator: "|")
        lock.lock()
        if let hit = cache[id] { lock.unlock(); return hit }
        let total = files.reduce(0) { $0 + ((try? fm.attributesOfItem(atPath: $1.url.path)[.size] as? Int) ?? 0) }
        if total > syncBytes || latest[sig] != nil {
            // Speech never waits for a big dictionary (50,000 entries: well over 100 ms to read), nor for a changed
            // one: it loads in the background, the voice speaking with the earlier copy, or without, until then.
            if !loading.contains(sig) {
                loading.insert(sig)
                DispatchQueue.global(qos: .utility).async {
                    let d = load(files)
                    lock.lock(); cache[id] = d; latest[sig] = d; loading.remove(sig); lock.unlock()
                }
            }
            let earlier = latest[sig]
            lock.unlock()
            return earlier
        }
        lock.unlock()
        let d = load(files)  // a small one now (Nico's en.dic: about 1 ms): the first words have it
        lock.lock(); cache[id] = d; latest[sig] = d; lock.unlock()
        return d
    }

    /// Files up to this size are read while speech waits; bigger ones in the background (ports/host/cv_dict.c).
    private static let syncBytes = 64 * 1024
    nonisolated(unsafe) private static var latest: [String: CommunityDictionary] = [:]
    nonisolated(unsafe) private static var loading = Set<String>()

    /// A file of a dictionary, and for a fetched one the list of its entries the person removed.
    private struct DictFile { let url: URL; let removed: URL? }

    /// A dictionary from text in the same format (trying one entry out in the editor).
    static func fromText(_ text: String) -> CommunityDictionary {
        var entries: [(pattern: String, caseless: Bool, template: String)] = []
        var ipa: [String] = []
        var narrower = Set<String>()
        var choices: [Int: SpanChoices] = [:]
        add(text, removed: [], entries: &entries, ipa: &ipa, choices: &choices, narrower: &narrower)
        return CommunityDictionary(entries: entries, ipa: ipa, choices: choices)
    }

    private static func load(_ files: [DictFile]) -> CommunityDictionary {
        var entries: [(pattern: String, caseless: Bool, template: String)] = []
        var ipa: [String] = []
        var narrower = Set<String>()  // pattern + type of the files read so far
        var choices: [Int: SpanChoices] = [:]
        for file in files {
            guard let text = try? String(contentsOf: file.url, encoding: .utf8) else { continue }
            var removed = Set<String>()  // type TAB pattern
            if let r = file.removed, let rt = try? String(contentsOf: r, encoding: .utf8) {
                for l in rt.split(whereSeparator: \.isNewline) {
                    let f = l.split(separator: "\t", omittingEmptySubsequences: false)
                    if let p = f.first, !p.isEmpty { removed.insert("\(f.count > 1 ? Int(f[1]) ?? 0 : 0)\t\(p)") }
                }
            }
            add(text, removed: removed, entries: &entries, ipa: &ipa, choices: &choices, narrower: &narrower)
        }
        return CommunityDictionary(entries: entries, ipa: ipa, choices: choices)
    }

    /// One file's entries into a dictionary being built: an entry an earlier file has (same pattern and type), or
    /// one the person removed, is left out.
    private static func add(_ text: String, removed: Set<String>, entries: inout [(pattern: String, caseless: Bool, template: String)],
                            ipa: inout [String], choices: inout [Int: SpanChoices], narrower: inout Set<String>) {
        var mine = Set<String>()
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.hasPrefix("\u{FEFF}") ? String(raw.dropFirst()) : String(raw)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            guard f.count >= 2, !f[0].isEmpty else { continue }
            let caseless = !(f.count > 2 && (f[2] == "1" || f[2] == "True"))
            let type = f.count > 3 ? Int(f[3]) ?? 0 : 0
            let key = "\(type)\t\(f[0])"
            mine.insert(key)
            if narrower.contains(key) || removed.contains(key) { continue }  // an earlier file has its own, or taken out
            var pattern: String
            switch type {
            case 1: pattern = pythonRegex(f[0])
            case 2: pattern = "\\b" + NSRegularExpression.escapedPattern(for: f[0]) + "\\b"
            default: pattern = NSRegularExpression.escapedPattern(for: f[0])
            }
            guard let template = template(f[1], regex: type == 1, pattern: f[0], caseless: caseless, ipa: &ipa,
                                          choices: &choices) else { continue }
            entries.append((pattern, caseless, template))
        }
        narrower.formUnion(mine)
    }

    /// Python's named groups in ICU's spelling.
    private static func pythonRegex(_ p: String) -> String {
        p.replacingOccurrences(of: "(?P<", with: "(?<")
            .replacingOccurrences(of: "\\(\\?P=(\\w+)\\)", with: "\\\\k<$1>", options: .regularExpression)
    }

    static func toIPA(_ x: String) -> String? {
        x.withCString { p in
            guard let r = vx_xsampa_to_ipa(p, strlen(p), nil) else { return nil }
            defer { free(r) }
            return String(cString: r)
        }
    }

    /// A span's X-SAMPA with each choice's option picked.
    static func pick(_ xs: String, _ list: [Choice], _ which: [Int]) -> String {
        var out = "", k = 0
        for u in xs.unicodeScalars {
            if u.value == 1 {
                if k < list.count, which[k] < list[k].opts.count { out += list[k].opts[which[k]] }
                k += 1
            } else { out.unicodeScalars.append(u) }
        }
        return out
    }

    /// The top-level alternatives of capturing group n (from 1) of a pattern, or nil when it has no such group.
    static func groupAlternatives(_ pattern: String, _ n: Int) -> [String]? {
        let p = Array(pattern.unicodeScalars)
        var count = 0, depth = 0, target = -1, cls = false, start = 0, out: [String] = []
        var i = 0
        while i < p.count {
            let c = p[i]
            if c == "\\", i + 1 < p.count { i += 2; continue }
            if cls { if c == "]" { cls = false }; i += 1; continue }
            if c == "[" {
                cls = true
                if i + 1 < p.count, p[i + 1] == "^" { i += 1 }
                if i + 1 < p.count, p[i + 1] == "]" { i += 1 }
                i += 1
                continue
            }
            if c == "(" {
                var capturing = true, body = i + 1
                if i + 1 < p.count, p[i + 1] == "?" {
                    capturing = false
                    if i + 3 < p.count, p[i + 2] == "P", p[i + 3] == "<" { capturing = true }
                    else if i + 3 < p.count, p[i + 2] == "<", p[i + 3] != "=", p[i + 3] != "!" { capturing = true }
                    if capturing, let gt = p[i...].firstIndex(of: ">") { body = gt + 1 }
                }
                depth += 1
                if capturing { count += 1; if count == n { target = depth; start = body } }
                i += 1
                continue
            }
            if target == depth, c == "|" || c == ")" {
                out.append(String(String.UnicodeScalarView(p[start..<i])))
                start = i + 1
                if c == ")" { return out }
            }
            if c == ")" { depth -= 1 }
            i += 1
        }
        return nil
    }

    /// Which option a choice takes for what its group captured.
    static func option(_ ch: Choice, captured: String) -> Int {
        guard let alts = ch.alts else { return 0 }
        for (k, a) in alts.enumerated() {
            if let re = try? NSRegularExpression(pattern: "^(?:" + pythonRegex(a) + ")$", options: ch.caseless ? [.caseInsensitive] : []),
               re.firstMatch(in: captured, range: NSRange(location: 0, length: (captured as NSString).length)) != nil { return k }
        }
        return alts.count
    }

    /// A replacement as an NSRegularExpression template: Python's \1 and \g<1> as $1, phonetic spans as
    /// placeholders. nil when a span isn't X-SAMPA.
    private static func template(_ rep: String, regex: Bool, pattern: String, caseless: Bool, ipa: inout [String],
                                 choices: inout [Int: SpanChoices]) -> String? {
        let c = Array(rep.unicodeScalars)
        var out = ""
        var i = 0
        var choicesSeen = 0  // the n-th choice of the replacement follows capture group n
        func lit(_ s: Unicode.Scalar) {
            if s == "$" || s == "\\" { out += "\\" }
            out.unicodeScalars.append(s)
        }
        while i < c.count {
            if c[i] == "~", i + 2 < c.count, c[i + 1] == "~", c[i + 2] != "~" {
                // the closing ~~: the last two of the first run of two or more tildes
                var j = i + 2
                var close: (content: Int, end: Int)?
                while j + 1 < c.count {
                    if c[j] == "~" && c[j + 1] == "~" {
                        var r = j
                        while r < c.count && c[r] == "~" { r += 1 }
                        close = (r - 2, r)
                        break
                    }
                    j += 1
                }
                if let close, close.content > i + 2 {
                    // choices, (a|b): their options split out, U+0001 where they go
                    var xs = ""
                    var list: [Choice] = []
                    var j = i + 2
                    while j < close.content {
                        guard c[j] == "(" else { xs.unicodeScalars.append(c[j]); j += 1; continue }
                        guard let end = c[j..<close.content].firstIndex(of: ")") else { return nil }
                        let opts = String(String.UnicodeScalarView(c[(j + 1)..<end])).split(separator: "|", omittingEmptySubsequences: false).map(String.init)
                        choicesSeen += 1
                        list.append(Choice(alts: regex ? groupAlternatives(pattern, choicesSeen) : nil, opts: opts, caseless: caseless))
                        xs.unicodeScalars.append(Unicode.Scalar(1))
                        j = end + 1
                    }
                    // checked as X-SAMPA: the span with its first options, and every option on its own
                    let first = Self.pick(xs, list, Array(repeating: 0, count: list.count))
                    guard let converted = first.isEmpty ? "" : Self.toIPA(first),
                          list.allSatisfy({ $0.opts.allSatisfy { $0.isEmpty || Self.toIPA($0) != nil } }),
                          let mark = Unicode.Scalar(spanBase + UInt32(ipa.count)) else { return nil }
                    if !list.isEmpty { choices[ipa.count] = SpanChoices(xs: xs, list: list) }
                    ipa.append(converted)
                    out.unicodeScalars.append(mark)
                    for (k, ch) in list.enumerated() {  // what each choice's group matched, between markers
                        out.unicodeScalars.append(choiceOpen)
                        if ch.alts != nil { out += "$\(choicesSeen - list.count + k + 1)" }
                        out.unicodeScalars.append(choiceClose)
                    }
                    i = close.end
                    continue
                }
            }
            if regex, c[i] == "\\", i + 1 < c.count {
                let e = c[i + 1]
                if e.properties.numericType != nil, let d = Int(String(e)) {
                    var n = d, j = i + 2
                    if j < c.count, let d2 = Int(String(c[j])), c[j].isASCII { n = n * 10 + d2; j += 1 }
                    out += "$\(n)"
                    i = j
                    continue
                }
                if e == "g", i + 2 < c.count, c[i + 2] == "<", let gt = c[(i + 3)...].firstIndex(of: ">"),
                   let n = Int(String(String.UnicodeScalarView(c[(i + 3)..<gt]))) {
                    out += "$\(n)"
                    i = gt + 1
                    continue
                }
                switch e {
                case "\\": lit("\\"); i += 2; continue
                case "n": out += "\n"; i += 2; continue
                case "t": out += "\t"; i += 2; continue
                default: break  // anything else stays as written, as Python leaves it
                }
            }
            lit(c[i])
            i += 1
        }
        return out
    }

    // MARK: - speaking

    private static func isWordScalar(_ s: Unicode.Scalar) -> Bool {
        s.properties.isAlphabetic || s == "'" || (s.value >= 0x80 && !(s.value >= spanBase && s.value < spanBase + 0xFFFE))
    }

    /// Engine text for `text`: the dictionary, then each phonetic span as the voice's phonemes (`phonemes`: an
    /// IPA string and whether it comes right after a word), then Apple's rules outside phoneme input.
    func speechText(_ text: String, rules: PronunciationRules?, phonemes: ((String, Bool) -> String?)?) -> String {
        // the dictionary, keeping where each UTF-16 unit came from
        var s = text as NSString
        var map = Array(0..<s.length)
        let count = patterns.count
        if count > 0 {
            var marks = [UInt8](repeating: 0, count: count)
            (s as String).withCString { c in marks.withUnsafeMutableBufferPointer { vx_rule_index_mark(index, c, $0.baseAddress, count) } }
            for i in 0..<count where marks[i] != 0 {
                guard let re = regex(i) else { continue }
                let found = re.matches(in: s as String, range: NSRange(location: 0, length: s.length))
                guard !found.isEmpty else { continue }
                let out = NSMutableString()
                var outMap: [Int] = []
                var copied = 0
                for m in found {
                    let r = m.range
                    out.append(s.substring(with: NSRange(location: copied, length: r.location - copied)))
                    outMap += map[copied..<r.location]
                    let rep = re.replacementString(for: m, in: s as String, offset: 0, template: templates[i])
                    out.append(rep)
                    let at = r.location < map.count ? map[r.location] : (map.last ?? 0)
                    outMap += Array(repeating: at, count: (rep as NSString).length)
                    copied = r.location + r.length
                }
                out.append(s.substring(from: copied))
                outMap += map[copied...]
                s = out
                map = outMap
                (s as String).withCString { c in marks.withUnsafeMutableBufferPointer { vx_rule_index_mark(index, c, $0.baseAddress, count) } }
            }
        }
        // the spans as phonemes
        let original = text as NSString
        var out = ""
        let scalars = Array((s as String).unicodeScalars)
        var utf16 = 0  // position of scalars[k] in s
        var k = 0
        while k < scalars.count {
            let sc = scalars[k]
            let len16 = sc.utf16.count
            guard sc.value >= Self.spanBase, Int(sc.value - Self.spanBase) < ipa.count else {
                out.unicodeScalars.append(sc)
                k += 1
                utf16 += len16
                continue
            }
            let joined = out.last.map { !$0.isWhitespace && $0 != "\\" } ?? false
            let src = utf16 < map.count ? map[utf16] : 0
            let span = Int(sc.value - Self.spanBase)
            k += 1
            utf16 += len16
            var spanIPA = ipa[span]
            if let ch = choices[span] {
                // each choice's option, by the alternative its group matched (the markers carry what it captured)
                var which: [Int] = []
                for c in ch.list {
                    guard k < scalars.count, scalars[k] == Self.choiceOpen else { break }
                    var captured = "", e = k + 1
                    while e < scalars.count, scalars[e] != Self.choiceClose { captured.unicodeScalars.append(scalars[e]); e += 1 }
                    for x in scalars[k...min(e, scalars.count - 1)] { utf16 += x.utf16.count }
                    k = e + 1
                    which.append(Self.option(c, captured: captured))
                }
                while which.count < ch.list.count { which.append(0) }
                let x = Self.pick(ch.xs, ch.list, which)
                if x.isEmpty { continue }  // nothing to say
                spanIPA = Self.toIPA(x) ?? spanIPA
            }
            guard let ph = phonemes?(spanIPA, joined) else { continue }
            // the original word the span stands in
            var ws = src, we = src
            func word(_ i: Int) -> Bool {  // a UTF-16 unit of a word: a letter, an apostrophe, anything not ASCII
                guard i >= 0, i < original.length else { return false }
                let u = original.character(at: i)
                guard let sc = Unicode.Scalar(u) else { return true }  // half of a surrogate pair
                return Self.isWordScalar(sc)
            }
            while ws > 0 && word(ws - 1) { ws -= 1 }
            while we < original.length && word(we) { we += 1 }
            let w = original.substring(with: NSRange(location: ws, length: we - ws))
            let usable = ph.hasPrefix("\u{1b}\\toi=lhp\\") && w.utf8.count < 64 && !w.contains("|") && !w.contains("\\")
            // letters right before the span: the word's start ("d" + "ecompile"); they go into a command telling
            // the engine the whole word, to say them as it would at its start (cv_dict.c, core/vxstream.c)
            var pre = ""
            for c in out.unicodeScalars.reversed() { if Self.isWordScalar(c) { pre.unicodeScalars.insert(c, at: pre.unicodeScalars.startIndex) } else { break } }
            if usable, !pre.isEmpty, pre.utf8.count < 48, w.utf8.count > pre.utf8.count, w.lowercased().hasPrefix(pre.lowercased()) {
                out.unicodeScalars.removeLast(pre.unicodeScalars.count)
                out += "\u{1b}\\cvpp=\(w)|\(pre)\\"
            }
            // letters right after the span: the word's rest; the engine is told the whole word
            var rest = ""
            var r = k
            while r < scalars.count, Self.isWordScalar(scalars[r]) { rest.unicodeScalars.append(scalars[r]); r += 1 }
            if usable, !rest.isEmpty, w.utf8.count > rest.utf8.count, w.lowercased().hasSuffix(rest.lowercased()) {
                out += "\u{1b}\\cvsp=\(w)|\(w.utf8.count - rest.utf8.count)\\"
            }
            out += ph
        }
        return Self.outsidePhonemes(out, rules: rules)
    }

    /// Apple's rules on the text between phoneme input (and the engine stream's own splice commands).
    static func outsidePhonemes(_ text: String, rules: PronunciationRules?) -> String {
        guard let rules else { return text }
        let pairs = [("\u{1b}\\toi=lhp\\", "\u{1b}\\toi=orth\\"), ("\u{1b}/+", "\u{1b}/+"), ("\u{1b}\\cvsp=", "\\"), ("\u{1b}\\cvpp=", "\\")]
        var out = ""
        var rest = Substring(text)
        while !rest.isEmpty {
            var next: (Range<Substring.Index>, Int)?
            for (i, p) in pairs.enumerated() {
                if let r = rest.range(of: p.0), next == nil || r.lowerBound < next!.0.lowerBound { next = (r, i) }
            }
            guard let (open, kind) = next else { out += rules.apply(String(rest)); break }
            out += rules.apply(String(rest[..<open.lowerBound]))
            let body = rest[open.upperBound...]
            let end = body.range(of: pairs[kind].1)?.upperBound ?? body.endIndex
            out += rest[open.lowerBound..<end]
            rest = rest[end...]
        }
        return out
    }

    // MARK: - fetching (the app)

    private static let fetchLock = NSLock()
    nonisolated(unsafe) private static var fetching = false

    /// Fetch every .dic file of the repository, at most every 6 hours (`force`: now), in the background; files it no
    /// longer has are removed. Mine/ is never touched. The app calls this when it comes to the front.
    static func refreshAll(force: Bool = false) {
        Task.detached { _ = await fetchAll(force: force) }
    }

    /// The same, finished when it returns (a background refresh task waits for it). false: nothing could be fetched
    /// (or another fetch was running).
    static func fetchAll(force: Bool = false) async -> Bool {
        guard let dir = folder else { return false }
        let stateURL = dir.appendingPathComponent("state.json")
        let saved = (try? JSONSerialization.jsonObject(with: Data(contentsOf: stateURL))) as? [String: Any] ?? [:]
        if !force, let checked = saved["checked"] as? Double, Date().timeIntervalSince1970 - checked < every { return true }
        guard startFetching() else { return false }
        defer { doneFetching() }
        let (repo, branch) = source
        // the repository's files (GitHub's contents API: 60 unauthenticated requests an hour is plenty): the
        // dictionaries, and phonemes/<language>.txt, the editor's example words
        func listing(_ path: String, _ ext: String) async -> [String]? {
            guard let url = URL(string: "https://api.github.com/repos/\(repo)/contents/\(path)?ref=\(branch)") else { return nil }
            var list = URLRequest(url: url, timeoutInterval: 30)
            list.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
            guard let (data, response) = try? await URLSession.shared.data(for: list),
                  let code = (response as? HTTPURLResponse)?.statusCode else { return nil }
            if code == 404 && !path.isEmpty { return [] }  // no such folder
            guard code == 200, let items = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return nil }
            return items.filter { ($0["type"] as? String ?? "file") == "file" }.compactMap { $0["name"] as? String }
                .filter { $0.hasSuffix(ext) && !$0.contains("/") && !$0.hasPrefix(".") }
        }
        guard let dics = await listing("", ".dic"), let phon = await listing("phonemes", ".txt") else { return false }
        let names = dics + phon.map { "phonemes/" + $0 }
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("phonemes"), withIntermediateDirectories: true)
        var etags = saved["etags"] as? [String: String] ?? [:]
        for name in names {
            guard let url = URL(string: "https://raw.githubusercontent.com/\(repo)/\(branch)/" + name) else { continue }
            var req = URLRequest(url: url, timeoutInterval: 30)
            let file = dir.appendingPathComponent(name)
            if let etag = etags[name], FileManager.default.fileExists(atPath: file.path) {
                req.setValue(etag, forHTTPHeaderField: "If-None-Match")
            }
            guard let (body, r) = try? await URLSession.shared.data(for: req), let http = r as? HTTPURLResponse else { return false }
            if http.statusCode == 200 {
                try? body.write(to: file, options: .atomic)
                etags[name] = http.value(forHTTPHeaderField: "ETag")
            } else if http.statusCode != 304 {
                return false  // try again later
            }
        }
        // files the repository no longer has
        for (sub, ext) in [("", ".dic"), ("phonemes", ".txt")] {
            let here = sub.isEmpty ? dir : dir.appendingPathComponent(sub)
            for f in (try? FileManager.default.contentsOfDirectory(atPath: here.path)) ?? [] {
                let name = sub.isEmpty ? f : sub + "/" + f
                guard f.hasSuffix(ext), !names.contains(name) else { continue }
                try? FileManager.default.removeItem(at: here.appendingPathComponent(f))
                etags[name] = nil
            }
        }
        let state: [String: Any] = ["etags": etags, "checked": Date().timeIntervalSince1970]
        if let d = try? JSONSerialization.data(withJSONObject: state) { try? d.write(to: stateURL, options: .atomic) }
        DictionaryStore.tidy()
        return true
    }

    private static func doneFetching() {
        fetchLock.lock(); fetching = false; fetchLock.unlock()
    }

    private static func startFetching() -> Bool {
        fetchLock.lock(); defer { fetchLock.unlock() }
        if fetching { return false }
        fetching = true
        return true
    }
}
