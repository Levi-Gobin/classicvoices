import Foundation
import Security

/// The person's dictionary changes and how they're described: shared by git (GitPublisher, Mac) and GitHub's API
/// (GitHub below, every platform).
enum DictionaryChanges {
    typealias Lists = (added: [DictionaryStore.Entry], changed: [DictionaryStore.Entry], removed: [DictionaryStore.Entry])
    static let maxNames = 5

    /// {file name: changes} for every file the person changed.
    static func pending() -> [String: Lists] {
        var out: [String: Lists] = [:]
        for name in DictionaryStore.fileNames() {
            let c = DictionaryStore.changes(name)
            if !c.added.isEmpty || !c.changed.isEmpty || !c.removed.isEmpty { out[name] = c }
        }
        return out
    }

    /// The commit message: up to `maxNames` patterns per kind on the first line, the full lists in the body.
    static func message(_ name: String, added: [DictionaryStore.Entry], changed: [DictionaryStore.Entry],
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

    /// What was published, for the alert: "3 entries added, 1 changed and 1 removed in en.dic" (several files:
    /// "... across 2 files").
    struct Summary: Sendable {
        var files: [String] = []
        var added = 0, changed = 0, removed = 0

        mutating func add(_ name: String, _ c: Lists) {
            files.append(name)
            added += c.added.count; changed += c.changed.count; removed += c.removed.count
        }

        var text: String {
            var parts: [String] = []
            for (n, verb) in [(added, "added"), (changed, "changed"), (removed, "removed")] where n > 0 {
                parts.append(parts.isEmpty ? "\(n) entr\(n == 1 ? "y" : "ies") \(verb)" : "\(n) \(verb)")
            }
            let what = parts.count > 1 ? parts.dropLast().joined(separator: ", ") + " and " + parts.last! : (parts.first ?? "")
            return what + (files.count == 1 ? " in \(files[0])" : " across \(files.count) files")
        }
    }
}

/// Publishing without git (iPhone and iPad always; the Mac when git isn't installed): the Classic Voices GitHub App
/// (Nico's), signed in to with GitHub's device flow (a code typed at github.com/login/device; no password and no
/// secret in the app), its keys in the Keychain. Each changed file: the repository's latest copy, the person's
/// changes made in it afresh, one commit per file with git's messages. Someone who can't write to the source can
/// have a copy (a fork) made and publish there; pull requests are theirs to open on GitHub.
enum GitHub {
    private static func env(_ k: String) -> String? {
        ProcessInfo.processInfo.environment[k].flatMap { $0.isEmpty ? nil : $0 }
    }
    /// The Classic Voices GitHub App (Nico's, github.com/apps/classic-voices): its client ID, which isn't secret
    /// (the device flow uses no client secret), and its name in its address.
    static let clientID = env("CV_GITHUB_CLIENT_ID") ?? "Iv23liprOaNWuXD30i5d"
    static let appSlug = env("CV_GITHUB_APP_SLUG") ?? "classic-voices"
    static let api = env("CV_GITHUB_API") ?? "https://api.github.com"
    static let web = env("CV_GITHUB_WEB") ?? "https://github.com"
    static var configured: Bool { !clientID.isEmpty }
    static var installURL: URL? { appSlug.isEmpty ? nil : URL(string: "\(web)/apps/\(appSlug)/installations/new") }

    struct Tokens: Codable, Sendable {
        var access: String
        var expires: Date
        var refresh: String
        var login: String
    }

    enum Failure: LocalizedError {
        case notSetUp, signedOut, expired, denied, http(Int, String), network(String)
        var errorDescription: String? {
            switch self {
            case .notSetUp: "Publishing through GitHub isn’t set up in this build yet."
            case .signedOut: "You’re not signed in to GitHub any more. Sign in again to publish."
            case .expired: "The code expired before it was entered. Try signing in again."
            case .denied: "Signing in was cancelled on GitHub."
            case .http(let code, let m): "GitHub answered \(code)\(m.isEmpty ? "" : ": " + m)"
            case .network(let m): "GitHub couldn’t be reached (\(m))."
            }
        }
    }

    // MARK: - keys (Keychain)

    private static let service = env("CV_GITHUB_KEYCHAIN_SERVICE") ?? "Classic Voices GitHub"  // tests: one of their own

    static func loadTokens() -> Tokens? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: "tokens", kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return try? JSONDecoder().decode(Tokens.self, from: d)
    }

    static func saveTokens(_ t: Tokens?) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: "tokens"]
        SecItemDelete(q as CFDictionary)
        guard let t, let d = try? JSONEncoder().encode(t) else { return }
        var add = q
        add[kSecValueData as String] = d
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
    }

    // MARK: - HTTP

    private static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 30
        return URLSession(configuration: c)
    }()

    /// (status, body). `form` goes as application/x-www-form-urlencoded, `json` as JSON.
    static func request(_ method: String, _ url: String, form: [String: String]? = nil, json: [String: Any]? = nil,
                        token: String? = nil, accept: String = "application/vnd.github+json") async throws -> (Int, Data) {
        guard let u = URL(string: url) else { throw Failure.network("bad address") }
        var r = URLRequest(url: u)
        r.httpMethod = method
        r.setValue(accept, forHTTPHeaderField: "Accept")
        r.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        r.setValue("ClassicVoices", forHTTPHeaderField: "User-Agent")
        if let token { r.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization") }
        if let form {
            var c = URLComponents()
            c.queryItems = form.map { URLQueryItem(name: $0.key, value: $0.value) }
            r.httpBody = Data((c.percentEncodedQuery ?? "").replacingOccurrences(of: "+", with: "%2B").utf8)
            r.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        } else if let json {
            r.httpBody = try JSONSerialization.data(withJSONObject: json)
            r.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        do {
            let (d, resp) = try await session.data(for: r)
            return ((resp as? HTTPURLResponse)?.statusCode ?? 0, d)
        } catch {
            throw Failure.network(error.localizedDescription)
        }
    }

    private static func object(_ d: Data) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] ?? [:]
    }

    private static func messageOf(_ d: Data) -> String { object(d)["message"] as? String ?? "" }

    // MARK: - signing in (device flow)

    struct DeviceCode: Sendable {
        let deviceCode: String
        let userCode: String
        let verification: URL
        let interval: Int
        let expires: Date
    }

    static func startSignIn() async throws -> DeviceCode {
        guard configured else { throw Failure.notSetUp }
        let (code, d) = try await request("POST", "\(web)/login/device/code", form: ["client_id": clientID], accept: "application/json")
        let o = object(d)
        guard code == 200, let dc = o["device_code"] as? String, let uc = o["user_code"] as? String,
              let v = (o["verification_uri"] as? String).flatMap(URL.init(string:)) else {
            throw Failure.http(code, o["error_description"] as? String ?? messageOf(d))
        }
        return DeviceCode(deviceCode: dc, userCode: uc, verification: v, interval: (o["interval"] as? Int) ?? 5,
                          expires: Date().addingTimeInterval(Double((o["expires_in"] as? Int) ?? 900)))
    }

    /// Waits for the code to be entered on GitHub (cancel the task to stop), then keeps the keys.
    static func finishSignIn(_ c: DeviceCode) async throws -> Tokens {
        var interval = max(1, c.interval)
        while Date() < c.expires {
            try await Task.sleep(for: .seconds(interval))
            let (_, d) = try await request("POST", "\(web)/login/oauth/access_token",
                                           form: ["client_id": clientID, "device_code": c.deviceCode,
                                                  "grant_type": "urn:ietf:params:oauth:grant-type:device_code"],
                                           accept: "application/json")
            let o = object(d)
            if o["access_token"] != nil { return try await keep(o) }
            switch o["error"] as? String {
            case "authorization_pending": continue
            case "slow_down": interval += 5
            case "expired_token": throw Failure.expired
            case "access_denied": throw Failure.denied
            default: throw Failure.http(0, o["error_description"] as? String ?? (o["error"] as? String ?? ""))
            }
        }
        throw Failure.expired
    }

    /// Stores a token answer (sign-in or refresh) with the account's name.
    private static func keep(_ o: [String: Any], login known: String? = nil) async throws -> Tokens {
        guard let access = o["access_token"] as? String else { throw Failure.signedOut }
        let expiresIn = (o["expires_in"] as? Int) ?? 8 * 3600
        var login = known ?? ""
        if login.isEmpty {
            let (code, d) = try await request("GET", "\(api)/user", token: access)
            guard code == 200, let l = object(d)["login"] as? String else { throw Failure.http(code, messageOf(d)) }
            login = l
        }
        let t = Tokens(access: access, expires: Date().addingTimeInterval(Double(expiresIn)),
                       refresh: o["refresh_token"] as? String ?? "", login: login)
        saveTokens(t)
        return t
    }

    /// Keys good for a while yet: renewed when within 5 minutes of running out (refresh keys change each time).
    static func tokens(forceRefresh: Bool = false) async throws -> Tokens {
        guard let t = loadTokens() else { throw Failure.signedOut }
        if !forceRefresh && t.expires.timeIntervalSinceNow > 300 { return t }
        guard !t.refresh.isEmpty else { saveTokens(nil); throw Failure.signedOut }
        let (_, d) = try await request("POST", "\(web)/login/oauth/access_token",
                                       form: ["client_id": clientID, "grant_type": "refresh_token", "refresh_token": t.refresh],
                                       accept: "application/json")
        let o = object(d)
        guard o["access_token"] != nil else { saveTokens(nil); throw Failure.signedOut }
        return try await keep(o, login: t.login)
    }

    /// Signs out; the copy (fork) belonged to that account, so it's forgotten too.
    static func signOut() {
        saveTokens(nil)
        fork = nil
    }

    // MARK: - publishing

    /// The person's copy of the source ("login/repo"), once made.
    static var fork: String? {
        get { UserDefaults.standard.string(forKey: "dictionaries.fork").flatMap { $0.isEmpty ? nil : $0 } }
        set { UserDefaults.standard.set(newValue, forKey: "dictionaries.fork") }
    }

    enum Result: Sendable {
        case published(DictionaryChanges.Summary, fork: String?)
        case nothing
        case needsFork  // the person can't write to the source: offer a copy
        case needsInstall  // a copy can't be made until the app is allowed on their account
        case failed(String)
    }

    /// Calls `body` with the keys; a refused key (401) is renewed once.
    private static func authorized<T>(_ body: (String) async throws -> T?) async throws -> T {
        var t = try await tokens()
        for attempt in 0..<2 {
            if let r = try await body(t.access) { return r }
            if attempt == 0 { t = try await tokens(forceRefresh: true) }
        }
        throw Failure.signedOut
    }

    /// The file's lines and version (sha) in `repo`; nil sha when it isn't there yet. Nil: the key was refused.
    private static func fetch(_ name: String, repo: String, branch: String, token: String) async throws -> ([String], String?)? {
        let path = name.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? name
        let b = branch.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? branch
        let (code, d) = try await request("GET", "\(api)/repos/\(repo)/contents/\(path)?ref=\(b)", token: token)
        if code == 401 { return nil }
        if code == 404 { return ([], nil) }
        guard code == 200 else { throw Failure.http(code, messageOf(d)) }
        let o = object(d)
        let sha = o["sha"] as? String
        var text = ""
        if let c = o["content"] as? String, !c.isEmpty,
           let raw = Data(base64Encoded: c.replacingOccurrences(of: "\n", with: ""), options: .ignoreUnknownCharacters) {
            text = String(decoding: raw, as: UTF8.self)
        } else if let u = o["download_url"] as? String {  // over 1 MB: the API leaves the content out
            let (rc, raw) = try await request("GET", u, token: token, accept: "application/vnd.github.raw")
            guard rc == 200 else { throw Failure.http(rc, "") }
            text = String(decoding: raw, as: UTF8.self)
        }
        var lines = text.components(separatedBy: "\n").map { $0.hasSuffix("\r") ? String($0.dropLast()) : $0 }
        if lines.last == "" { lines.removeLast() }
        return (lines, sha)
    }

    private enum Put { case done, conflict, forbidden, refused }

    private static func put(_ name: String, repo: String, branch: String, text: String, sha: String?, message: String,
                            token: String) async throws -> Put {
        let path = name.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? name
        var body: [String: Any] = ["message": message, "content": Data(text.utf8).base64EncodedString(), "branch": branch]
        if let sha { body["sha"] = sha }
        let (code, d) = try await request("PUT", "\(api)/repos/\(repo)/contents/\(path)", json: body, token: token)
        switch code {
        case 200, 201: return .done
        case 409, 422 where messageOf(d).lowercased().contains("sha") || code == 409: return .conflict
        case 401: return .refused
        case 403, 404: return .forbidden
        default: throw Failure.http(code, messageOf(d))
        }
    }

    /// Publishes every changed file to the source (or the person's copy of it).
    static func publish() async -> Result {
        let todo = DictionaryChanges.pending()
        if todo.isEmpty { return .nothing }
        let source = CommunityDictionary.source
        let target = fork ?? source.repo
        var summary = DictionaryChanges.Summary()
        var written: [String: String] = [:]
        do {
            for name in todo.keys.sorted() {
                let c = todo[name]!
                var finished = false
                for _ in 0..<3 where !finished {  // someone else committed meanwhile: on their new copy
                    let outcome: Put = try await authorized { token in
                        guard let (lines, sha) = try await fetch(name, repo: target, branch: source.branch, token: token) else { return nil }
                        let new = DictionaryStore.applyChanges(lines, added: c.added, changed: c.changed, removed: c.removed)
                        if new == lines { return .done }
                        let text = new.joined(separator: "\n") + "\n"
                        let r = try await put(name, repo: target, branch: source.branch, text: text, sha: sha,
                                              message: DictionaryChanges.message(name, added: c.added, changed: c.changed, removed: c.removed),
                                              token: token)
                        if r == .refused { return nil }
                        if r == .done { written[name] = text; summary.add(name, c) }
                        return r
                    }
                    switch outcome {
                    case .done: finished = true
                    case .conflict: continue
                    case .forbidden:
                        if fork == nil && summary.files.isEmpty { return .needsFork }
                        throw Failure.http(403, "not allowed to write to \(target)")
                    case .refused: throw Failure.signedOut
                    }
                }
                if !finished { throw Failure.http(409, "\(name) kept changing on GitHub; try again") }
            }
        } catch {
            if !summary.files.isEmpty { finishSource(written, toSource: target == source.repo) }
            return .failed(error.localizedDescription)
        }
        if summary.files.isEmpty { return .nothing }
        finishSource(written, toSource: target == source.repo)
        return .published(summary, fork: target == source.repo ? nil : target)
    }

    /// What went to the source is now the fetched copy; the person's published changes drop out of Mine.
    private static func finishSource(_ written: [String: String], toSource: Bool) {
        guard toSource, let dir = CommunityDictionary.folder else { return }
        for (name, text) in written { try? Data(text.utf8).write(to: dir.appendingPathComponent(name), options: .atomic) }
        DictionaryStore.tidy()
    }

    /// Makes the person's copy of the source and remembers it (waits until GitHub has it ready).
    static func makeFork() async -> Result {
        let source = CommunityDictionary.source
        do {
            let full: String = try await authorized { token in
                let (code, d) = try await request("POST", "\(api)/repos/\(source.repo)/forks",
                                                  json: ["default_branch_only": true], token: token)
                if code == 401 { return nil }
                if code == 403 || code == 404 { throw ForkRefused() }
                guard code == 202 || code == 200, let f = object(d)["full_name"] as? String else { throw Failure.http(code, messageOf(d)) }
                return f
            }
            for _ in 0..<30 {  // forking happens in the background
                let t = try await tokens()
                let (code, _) = try await request("GET", "\(api)/repos/\(full)/contents/?ref=\(source.branch)", token: t.access)
                if code == 200 { fork = full; return .nothing }
                try await Task.sleep(for: .seconds(2))
            }
            return .failed("GitHub is still making your copy. Try publishing again in a minute.")
        } catch is ForkRefused {
            return .needsInstall
        } catch {
            return .failed(error.localizedDescription)
        }
    }

    private struct ForkRefused: Error {}
}
