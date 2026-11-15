#if os(macOS)
import Foundation

/// Publishes the person's dictionary changes (DictionaryStore, Dictionaries/Mine) to the repository they come
/// from with git, as the NVDA add-on's gitsync.py does: only where git is installed (Homebrew's, or Apple's when
/// the Command Line Tools are there: /usr/bin/git alone would open Apple's install dialog) and its stored
/// credentials may push there. Never asks for anything; if it can't push, the changes stay on this Mac.
///
/// Once a day while the app runs (and from Publish Now): the repository's latest copy, the person's changes made
/// in it afresh (no merge conflicts), one commit per changed file ("en.dic: add 3 entries - a, b, c; modify 1
/// entry - d; remove 1 entry - e"), one push. The work copy is in ~/Library/Application Support/Classic Voices,
/// outside the App Group container, so git never reaches into it.
@MainActor @Observable
final class GitPublisher {
    static let shared = GitPublisher()
    nonisolated static let every: TimeInterval = 24 * 3600
    nonisolated static let maxNames = 5

    private(set) var busy = false
    private(set) var lastError: String?
    private(set) var lastPublished: Date?

    /// The git program, or nil (also in a sandboxed build, which can't run it).
    nonisolated static var git: String? {
        if ProcessInfo.processInfo.environment["APP_SANDBOX_CONTAINER_ID"] != nil { return nil }
        for g in ["/opt/homebrew/bin/git", "/usr/local/bin/git"] where FileManager.default.isExecutableFile(atPath: g) { return g }
        if let (rc, out) = try? run("/usr/bin/xcode-select", ["-p"]), rc == 0,
           FileManager.default.fileExists(atPath: out.trimmingCharacters(in: .whitespacesAndNewlines)) {
            return "/usr/bin/git"
        }
        return nil
    }

    /// Whether git is here, looked up once.
    nonisolated static let available: Bool = git != nil

    nonisolated static var workDir: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Classic Voices/dictionaries-git")
    }

    nonisolated private static func run(_ tool: String, _ args: [String], cwd: URL? = nil, timeout: TimeInterval = 120) throws -> (Int32, String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        if let cwd { p.currentDirectoryURL = cwd }
        var env = ProcessInfo.processInfo.environment
        for (k, v) in ["GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "never", "GIT_ASKPASS": "", "SSH_ASKPASS": "",
                       "GIT_SSH_COMMAND": "ssh -o BatchMode=yes"] { env[k] = v }
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        try p.run()
        let killer = DispatchWorkItem { if p.isRunning { p.terminate() } }
        DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: killer)
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        killer.cancel()
        return (p.terminationStatus, String(decoding: data, as: UTF8.self))
    }

    /// {file name: changes} for every file the person changed.
    nonisolated static func pending() -> [String: (added: [DictionaryStore.Entry], changed: [DictionaryStore.Entry], removed: [DictionaryStore.Entry])] {
        var out: [String: (added: [DictionaryStore.Entry], changed: [DictionaryStore.Entry], removed: [DictionaryStore.Entry])] = [:]
        for name in DictionaryStore.fileNames() {
            let c = DictionaryStore.changes(name)
            if !c.added.isEmpty || !c.changed.isEmpty || !c.removed.isEmpty { out[name] = c }
        }
        return out
    }

    /// The commit message: up to `maxNames` patterns per kind on the first line, the full lists in the body.
    nonisolated static func message(_ name: String, added: [DictionaryStore.Entry], changed: [DictionaryStore.Entry],
                                    removed: [DictionaryStore.Entry]) -> String {
        func part(_ verb: String, _ l: [DictionaryStore.Entry]) -> String {
            let names = l.map(\.pattern)
            let shown = names.prefix(maxNames).joined(separator: ", ") + (l.count > maxNames ? " and \(l.count - maxNames) more" : "")
            return "\(verb) \(l.count) entr\(l.count == 1 ? "y" : "ies") - \(shown)"
        }
        let kinds = [("add", added), ("modify", changed), ("remove", removed)].filter { !$0.1.isEmpty }
        let subject = "\(name): " + kinds.map { part($0.0, $0.1) }.joined(separator: "; ")
        let body = [("Added", added), ("Modified", changed), ("Removed", removed)].filter { $0.1.count > maxNames }
            .map { "\($0.0):\n" + $0.1.map { "- " + $0.pattern }.joined(separator: "\n") }
        return body.isEmpty ? subject : subject + "\n\n" + body.joined(separator: "\n\n")
    }

    /// The remote's address (tests may point it elsewhere with CV_DICT_REMOTE).
    nonisolated static var remoteURL: String {
        if let r = ProcessInfo.processInfo.environment["CV_DICT_REMOTE"] { return r }
        return "https://github.com/\(CommunityDictionary.source.repo).git"
    }

    /// The work copy, cloned or brought to the repository's latest; nil when that worked, else git's words.
    nonisolated private static func prepare(_ git: String) -> String? {
        let branch = CommunityDictionary.source.branch, url = remoteURL, w = workDir
        let fm = FileManager.default
        if fm.fileExists(atPath: w.appendingPathComponent(".git").path) {
            let r = try? run(git, ["remote", "get-url", "origin"], cwd: w)
            if r?.0 != 0 || r?.1.trimmingCharacters(in: .whitespacesAndNewlines) != url { try? fm.removeItem(at: w) }
        }
        if !fm.fileExists(atPath: w.appendingPathComponent(".git").path) {
            try? fm.removeItem(at: w)
            try? fm.createDirectory(at: w.deletingLastPathComponent(), withIntermediateDirectories: true)
            guard let (rc, out) = try? run(git, ["clone", "--quiet", "--branch", branch, url, w.path], timeout: 300) else { return "git didn't run" }
            if rc != 0 { return out }
        }
        for args in [["fetch", "--quiet", "origin", branch], ["checkout", "--quiet", "-B", branch, "origin/" + branch],
                     ["reset", "--quiet", "--hard", "origin/" + branch]] {
            guard let (rc, out) = try? run(git, args, cwd: w) else { return "git didn't run" }
            if rc != 0 { return out }
        }
        return nil
    }

    /// Whether git may push to the chosen repository (a push that sends nothing).
    nonisolated static func canPush() -> Bool {
        guard let git else { return false }
        if prepare(git) != nil { return false }
        return (try? run(git, ["push", "--dry-run", "--quiet", "origin", "HEAD"], cwd: workDir))?.0 == 0
    }

    /// Commits and pushes the changes: (files published, or why it couldn't).
    nonisolated static func publishNow() -> (Int, String?) {
        let todo = pending()
        guard !todo.isEmpty else { return (0, nil) }
        guard let git else { return (0, "git isn't installed") }
        for attempt in 0..<2 {  // pushed meanwhile by someone else: once more on the new copy
            if let err = prepare(git) { return (0, err.trimmingCharacters(in: .whitespacesAndNewlines)) }
            var made = 0
            for name in todo.keys.sorted() {
                let c = todo[name]!
                let file = workDir.appendingPathComponent(name)
                let lines = DictionaryStore.lines(file)
                let new = DictionaryStore.applyChanges(lines, added: c.added, changed: c.changed, removed: c.removed)
                if new == lines { continue }
                try? (new.joined(separator: "\n") + "\n").write(to: file, atomically: true, encoding: .utf8)
                var r = try? run(git, ["add", "--", name], cwd: workDir)
                if r?.0 == 0 { r = try? run(git, ["commit", "--quiet", "-m", message(name, added: c.added, changed: c.changed, removed: c.removed)], cwd: workDir) }
                if r?.0 != 0 { return (0, (r?.1 ?? "git didn't run").trimmingCharacters(in: .whitespacesAndNewlines)) }
                made += 1
            }
            if made == 0 { return (0, nil) }
            guard let (rc, out) = try? run(git, ["push", "--quiet", "origin", "HEAD"], cwd: workDir, timeout: 300) else { return (0, "git didn't run") }
            if rc == 0 {
                // what was pushed is now the fetched copy; the person's published changes drop out of Mine
                if let dir = CommunityDictionary.folder {
                    for name in todo.keys {
                        let src = workDir.appendingPathComponent(name)
                        if let d = try? Data(contentsOf: src) { try? d.write(to: dir.appendingPathComponent(name), options: .atomic) }
                    }
                }
                DictionaryStore.tidy()
                return (made, nil)
            }
            if attempt == 1 || !(out.contains("rejected") || out.contains("fetch first")) {
                return (0, out.trimmingCharacters(in: .whitespacesAndNewlines))
            }
        }
        return (0, nil)
    }

    /// Whether the person's changes go out by themselves once a day (off unless turned on: Publish Now otherwise).
    nonisolated static var automatic: Bool {
        get { UserDefaults.standard.bool(forKey: "dictionaries.publishAutomatically") }
        set { UserDefaults.standard.set(newValue, forKey: "dictionaries.publishAutomatically") }
    }

    /// Publishes in the background; `force`: now (Publish Now), else once a day when `automatic` is on.
    func publish(force: Bool) async -> (Int, String?) {
        if !force && !Self.automatic { return (0, nil) }
        let key = "dictionaries.publishTried"
        let tried = UserDefaults.standard.double(forKey: key)
        if busy || (!force && Date().timeIntervalSince1970 - tried < Self.every) { return (0, nil) }
        guard Self.git != nil, !Self.pending().isEmpty else { return (0, nil) }
        busy = true
        UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: key)
        let result = await Task.detached(priority: .utility) { Self.publishNow() }.value
        busy = false
        lastError = result.1
        if result.0 > 0 { lastPublished = Date() }
        return result
    }
}
#endif
