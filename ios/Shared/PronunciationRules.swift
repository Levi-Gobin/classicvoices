import Foundation

/// Apple's own text rules for the Vocalizer voices. iOS 5 and 6 shipped
/// `TTSResources/<language>/user_rules.txt` next to each language: regular
/// expressions VoiceServices ran over the text before the engine saw it, so
/// "iOS" became "I O S", "OS X" became "OS ten", "St" became "Street" and so
/// on. The engine itself never asks for the file, so the rules are applied
/// here, the same way, before the text is handed over.
///
/// File format, one rule per line: `/pattern/flags --> "replacement"`, with
/// `$1`-style groups, `\xHH` and `\\` escapes in the replacement, and the
/// flags `i` and `m`.
struct PronunciationRules {
    struct Rule {
        let regex: NSRegularExpression
        let template: String
    }

    let rules: [Rule]

    func apply(_ text: String) -> String {
        var s = text
        for r in rules {
            s = r.regex.stringByReplacingMatches(in: s, range: NSRange(s.startIndex..., in: s), withTemplate: r.template)
        }
        return s
    }

    static func parse(_ text: String) -> [Rule] {
        var out: [Rule] = []
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
            guard let regex = try? NSRegularExpression(pattern: pattern, options: options) else { continue }
            let replacement = unescape(String(right.dropFirst().dropLast()))
            out.append(Rule(regex: regex, template: template(replacement)))
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

    /// The rules for a voice's language: iOS 6's file (the most complete set;
    /// iOS 3's doesn't know "iOS" yet), else the voice's own firmware's, else
    /// iOS 5's. nil when no firmware has rules for the language.
    static func rules(forVoiceKey key: String) -> PronunciationRules? {
        guard let root = VoiceCatalog.root, let entry = VoiceCatalog.entry(forKey: key) else { return nil }
        let builds = (["ios6", entry.build, "ios5"]).reduce(into: [String]()) {
            if !$0.contains($1) { $0.append($1) }
        }
        for build in builds {
            let file = root.appendingPathComponent("\(build)/TTSResources/\(entry.language)/user_rules.txt")
            lock.lock()
            if let hit = cache[file.path] { lock.unlock(); return hit }
            lock.unlock()
            guard let text = try? String(contentsOf: file, encoding: .utf8) else { continue }
            let parsed = PronunciationRules(rules: parse(text))
            lock.lock(); cache[file.path] = parsed; lock.unlock()
            return parsed
        }
        return nil
    }
}
