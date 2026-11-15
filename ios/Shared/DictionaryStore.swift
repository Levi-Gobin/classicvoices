import Foundation

/// The person's own dictionary entries, next to the fetched community files (as the NVDA add-on's dicts.py keeps
/// them, and ports/host/cv_dict.c and CommunityDictionary read them): Dictionaries/Mine/<file> holds the entries
/// they added or changed, Mine/<file>.removed the fetched entries they took out (pattern TAB type). The editor
/// shows a file's entries with whose they are; publishing (GitPublisher) writes the person's changes into the
/// repository, and `tidy` drops them once the fetched copy has them.
enum DictionaryStore {
    struct Entry: Hashable, Identifiable {
        var pattern: String
        var replacement: String
        var caseSensitive = false
        var type = 2  // 0 anywhere, 1 regular expression, 2 whole word

        var id: String { key }
        var key: String { "\(type)\t\(pattern)" }
        var line: String { "\(pattern)\t\(replacement)\t\(caseSensitive ? 1 : 0)\t\(type)" }

        init(pattern: String, replacement: String, caseSensitive: Bool = false, type: Int = 2) {
            self.pattern = pattern; self.replacement = replacement; self.caseSensitive = caseSensitive; self.type = type
        }

        /// From a .dic line; nil for comments, blank and bad lines.
        init?(line raw: String) {
            let line = raw.hasPrefix("\u{FEFF}") ? String(raw.dropFirst()) : raw
            if line.isEmpty || line.hasPrefix("#") { return nil }
            let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            guard f.count >= 2, !f[0].isEmpty else { return nil }
            pattern = f[0]; replacement = f[1]
            caseSensitive = f.count > 2 && (f[2] == "1" || f[2] == "True")
            type = f.count > 3 ? Int(f[3]) ?? 0 : 0
        }
    }

    enum Origin { case community, added, changed, removed }

    struct Row: Identifiable {
        let entry: Entry
        let origin: Origin
        var id: String { entry.key }
    }

    static var folder: URL? { CommunityDictionary.folder }
    static var mine: URL? { folder?.appendingPathComponent("Mine") }

    static func lines(_ url: URL?) -> [String] {
        guard let url, let t = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        var out = t.components(separatedBy: "\n").map { $0.hasSuffix("\r") ? String($0.dropLast()) : $0 }
        if out.last == "" { out.removeLast() }
        return out
    }

    static func entries(_ url: URL?) -> [Entry] { lines(url).compactMap(Entry.init(line:)) }

    private static func write(_ url: URL?, _ lines: [String]) {
        guard let url else { return }
        if lines.isEmpty { try? FileManager.default.removeItem(at: url); return }
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
    }

    private static func communityURL(_ name: String) -> URL? { folder?.appendingPathComponent(name) }
    private static func mineURL(_ name: String) -> URL? { mine?.appendingPathComponent(name) }
    private static func removedURL(_ name: String) -> URL? { mine?.appendingPathComponent(name + ".removed") }

    static func removed(_ name: String) -> Set<String> {
        Set(lines(removedURL(name)).compactMap { l -> String? in
            let f = l.split(separator: "\t", omittingEmptySubsequences: false)
            guard let p = f.first, !p.isEmpty else { return nil }
            return "\(f.count > 1 ? Int(f[1]) ?? 0 : 0)\t\(p)"
        })
    }

    private static func writeRemoved(_ name: String, _ keys: Set<String>) {
        write(removedURL(name), keys.sorted().map { k in
            let parts = k.split(separator: "\t", maxSplits: 1).map(String.init)
            return parts.count == 2 ? "\(parts[1])\t\(parts[0])" : k
        })
    }

    /// The file's entries for the editor: the fetched file's in order (with the person's changes and removals
    /// shown on them), then the person's additions.
    static func rows(_ name: String) -> [Row] {
        let community = entries(communityURL(name)), mineEntries = entries(mineURL(name)), gone = removed(name)
        let mineByKey = Dictionary(mineEntries.map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
        var out: [Row] = [], shown = Set<String>()
        for c in community where !shown.contains(c.key) {
            shown.insert(c.key)
            if let m = mineByKey[c.key] { out.append(Row(entry: m, origin: .changed)) }
            else if gone.contains(c.key) { out.append(Row(entry: c, origin: .removed)) }
            else { out.append(Row(entry: c, origin: .community)) }
        }
        out += mineEntries.filter { !shown.contains($0.key) }.map { Row(entry: $0, origin: .added) }
        return out
    }

    /// Adds or changes an entry (`oldKey`: the one it replaces, when its pattern or type changed).
    static func put(_ name: String, _ entry: Entry, oldKey: String? = nil) {
        var m = entries(mineURL(name)).filter { $0.key != entry.key && $0.key != oldKey }
        let community = Dictionary(entries(communityURL(name)).map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
        var gone = removed(name)
        if let oldKey, oldKey != entry.key, community[oldKey] != nil { gone.insert(oldKey) }
        gone.remove(entry.key)
        if community[entry.key] != entry { m.append(entry) }
        write(mineURL(name), m.map(\.line))
        writeRemoved(name, gone)
    }

    /// Takes an entry out: the person's goes; a fetched one is listed as removed.
    static func remove(_ name: String, key: String) {
        write(mineURL(name), entries(mineURL(name)).filter { $0.key != key }.map(\.line))
        if entries(communityURL(name)).contains(where: { $0.key == key }) {
            var gone = removed(name); gone.insert(key); writeRemoved(name, gone)
        }
    }

    /// Back to the fetched entry: the person's change or removal of it is dropped.
    static func restore(_ name: String, key: String) {
        write(mineURL(name), entries(mineURL(name)).filter { $0.key != key }.map(\.line))
        var gone = removed(name); gone.remove(key); writeRemoved(name, gone)
    }

    /// After a fetch: the person's entries the repository now has as they are, and removals of entries it no
    /// longer has, are dropped.
    static func tidy() {
        guard let mine, let names = try? FileManager.default.contentsOfDirectory(atPath: mine.path) else { return }
        for name in names where name.hasSuffix(".dic") {
            guard let c = communityURL(name), FileManager.default.fileExists(atPath: c.path) else { continue }
            let community = Dictionary(entries(c).map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
            let m = entries(mineURL(name))
            let keep = m.filter { community[$0.key] != $0 }
            if keep.count != m.count { write(mineURL(name), keep.map(\.line)) }
            let gone = removed(name)
            let still = gone.filter { community[$0] != nil }
            if still != gone { writeRemoved(name, still) }
        }
    }

    /// What the person changed in a file against the fetched copy.
    static func changes(_ name: String) -> (added: [Entry], changed: [Entry], removed: [Entry]) {
        let community = entries(communityURL(name))
        let byKey = Dictionary(community.map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
        var added: [Entry] = [], changed: [Entry] = []
        for e in entries(mineURL(name)) {
            if let c = byKey[e.key] { if c != e { changed.append(e) } } else { added.append(e) }
        }
        let rem = removed(name).sorted().compactMap { byKey[$0] }
        return (added, changed, rem)
    }

    /// A fetched file's lines with the person's changes made in them: changed lines in place, removed ones gone,
    /// added ones at the end; comments and order kept.
    static func applyChanges(_ lines: [String], added: [Entry], changed: [Entry], removed: [Entry]) -> [String] {
        var byKey = Dictionary(changed.map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
        let gone = Set(removed.map(\.key))
        var out: [String] = []
        for l in lines {
            guard let e = Entry(line: l) else { out.append(l); continue }
            if gone.contains(e.key) { continue }
            if let c = byKey.removeValue(forKey: e.key) { out.append(c.line) } else { out.append(l) }
        }
        out += added.map(\.line) + changed.filter { byKey[$0.key] != nil }.map(\.line)
        return out
    }

    /// Every dictionary file there is, fetched or the person's.
    static func fileNames() -> [String] {
        var out = Set<String>()
        for d in [folder, mine].compactMap({ $0 }) {
            for f in (try? FileManager.default.contentsOfDirectory(atPath: d.path)) ?? [] where f.hasSuffix(".dic") { out.insert(f) }
        }
        return out.sorted()
    }

    /// [(X-SAMPA, example word)] for a language: phonemes/<lang>.txt, its lines replaced and added to by the
    /// narrower phonemes/<lang-region>.txt.
    static func phonemeExamples(language: String) -> [(xsampa: String, example: String)] {
        let lang = language.replacingOccurrences(of: "_", with: "-").lowercased()
        var files: [String] = []
        if let dash = lang.firstIndex(of: "-") { files.append(String(lang[..<dash])) }
        files.append(lang)
        var order: [String] = [], by: [String: String] = [:]
        for f in files {
            for l in lines(folder?.appendingPathComponent("phonemes/\(f).txt")) {
                guard !l.hasPrefix("#"), let tab = l.firstIndex(of: "\t") else { continue }
                let ph = l[..<tab].trimmingCharacters(in: .whitespaces)
                let ex = l[l.index(after: tab)...].trimmingCharacters(in: .whitespaces)
                guard !ph.isEmpty else { continue }
                if by[ph] == nil { order.append(ph) }
                by[ph] = ex
            }
        }
        return order.map { ($0, by[$0] ?? "") }
    }
}
