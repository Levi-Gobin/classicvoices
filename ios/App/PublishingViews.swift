import SwiftUI
#if canImport(UIKit)
import UIKit
#elseif canImport(AppKit)
import AppKit
#endif

/// Publishing the person's dictionary changes (Nico's design, 2026-11-15): git where it's installed (Mac), GitHub's
/// API otherwise (GitHub, signed in with the device flow). Publish with no way to publish opens the sign-in;
/// leaving the dictionaries with unpublished changes and no way to publish asks whether to sign in (Yes, No,
/// Don't Show Again). Everything is shown from one Publishing sheet that RootView presents.
@MainActor @Observable
final class DictionaryPublisher {
    static let shared = DictionaryPublisher()
    nonisolated static let every: TimeInterval = 24 * 3600

    private(set) var busy = false
    private(set) var lastError: String?
    private(set) var login: String? = GitHub.loadTokens()?.login
    var alert: PublishAlert?  // shown by the Publishing sheet
    var leaveAsk = false  // shown by RootView
    var showPublishing = false
    var signInFirst = false  // the sheet opens on signing in, then publishes

    enum PublishAlert: Identifiable {
        case result(String, String), forkOffer(String), install
        var id: String {
            switch self {
            case .result(let t, let m): t + m
            case .forkOffer(let r): "fork" + r
            case .install: "install"
            }
        }
    }

    /// git on this Mac (the API isn't needed then).
    static var usesGit: Bool {
        #if os(macOS)
        GitPublisher.available
        #else
        false
        #endif
    }

    var canPublish: Bool { Self.usesGit || login != nil }

    var lastPublished: Date? {
        let t = UserDefaults.standard.double(forKey: "dictionaries.lastPublished")
        return t > 0 ? Date(timeIntervalSince1970: t) : nil
    }

    /// Whether the person's changes go out by themselves once a day (off unless turned on).
    var automatic: Bool {
        get { access(keyPath: \.automatic); return UserDefaults.standard.bool(forKey: "dictionaries.publishAutomatically") }
        set { withMutation(keyPath: \.automatic) { UserDefaults.standard.set(newValue, forKey: "dictionaries.publishAutomatically") } }
    }

    static var dontAsk: Bool {
        get { UserDefaults.standard.bool(forKey: "dictionaries.dontAskSignIn") }
        set { UserDefaults.standard.set(newValue, forKey: "dictionaries.dontAskSignIn") }
    }

    func signedIn(_ t: GitHub.Tokens) { login = t.login }

    func signOut() {
        GitHub.signOut()
        login = nil
    }

    /// Opens the Publishing sheet (`signIn`: on signing in, then publishing).
    func open(signIn: Bool = false) {
        signInFirst = signIn && !canPublish
        showPublishing = true
    }

    /// Leaving the dictionary editor: asks to sign in when there's something to publish and no way to.
    func leftEditor() {
        guard !canPublish, GitHub.configured, !Self.dontAsk, !DictionaryChanges.pending().isEmpty else { return }
        leaveAsk = true
    }

    /// Publish My Changes Now.
    func publishNow() async {
        guard !busy else { return }
        busy = true
        let r = await run()
        busy = false
        handle(r, manual: true)
    }

    /// Once a day when turned on (the Mac's loop, the iPhone when the app comes to the front).
    func publishAutomatically() async {
        guard automatic, canPublish, !busy else { return }
        let key = "dictionaries.publishTried"
        guard Date().timeIntervalSince1970 - UserDefaults.standard.double(forKey: key) >= Self.every,
              !DictionaryChanges.pending().isEmpty else { return }
        UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: key)
        busy = true
        let r = await run()
        busy = false
        handle(r, manual: false)
    }

    /// Makes the person's copy, then publishes there.
    func makeFork() async {
        guard !busy else { return }
        busy = true
        let r = await GitHub.makeFork()
        if case .nothing = r {
            let p = await GitHub.publish()
            busy = false
            handle(p, manual: true)
        } else {
            busy = false
            handle(r, manual: true)
        }
    }

    private func run() async -> GitHub.Result {
        #if os(macOS)
        if Self.usesGit {
            let ((n, err), summary) = await Task.detached(priority: .utility) { GitPublisher.publishNowSummary() }.value
            if let err { return .failed("git couldn’t publish them (\(err))") }
            return n == 0 ? .nothing : .published(summary, fork: nil)
        }
        #endif
        return await GitHub.publish()
    }

    private func handle(_ r: GitHub.Result, manual: Bool) {
        login = GitHub.loadTokens()?.login  // a refused key signs out
        switch r {
        case .published(let s, let fork):
            lastError = nil
            UserDefaults.standard.set(Date().timeIntervalSince1970, forKey: "dictionaries.lastPublished")
            if manual {
                alert = .result("Dictionaries Published", s.text + "." + (fork.map { " Published to your copy, \($0)." } ?? ""))
            }
        case .nothing:
            if manual {
                if let fork = GitHub.fork, !Self.usesGit, !DictionaryChanges.pending().isEmpty {
                    alert = .result("Nothing to Publish", "Your copy, \(fork), has all your changes already.")
                } else {
                    alert = .result("Nothing to Publish", "You have no changes of your own that aren’t published yet.")
                }
            }
        case .needsFork:
            if manual { alert = .forkOffer(CommunityDictionary.source.repo) } else { lastError = "You can’t publish to \(CommunityDictionary.source.repo)." }
        case .needsInstall:
            if manual { alert = .install }
        case .failed(let m):
            lastError = m
            if manual { alert = .result("Couldn’t Publish", m + " Your changes stay on this device.") }
        }
    }

    /// "en.dic: 3 added, 1 changed" per file, or nil when there's nothing.
    static func pendingText() -> String? {
        let p = DictionaryChanges.pending()
        guard !p.isEmpty else { return nil }
        return p.keys.sorted().map { name -> String in
            let c = p[name]!
            let parts = [(c.added.count, "added"), (c.changed.count, "changed"), (c.removed.count, "removed")]
                .filter { $0.0 > 0 }.map { "\($0.0) \($0.1)" }
            return "\(name): " + parts.joined(separator: ", ")
        }.joined(separator: "; ")
    }
}

/// The Publishing sheet: how it publishes (git, or the GitHub account), what's waiting, Publish My Changes Now, the
/// once-a-day switch; signing in is a page of its own.
struct PublishingSheet: View {
    @State private var publisher = DictionaryPublisher.shared
    @State private var path: [Bool] = []  // a sign-in page; its value: publish after signing in
    @State private var pending = DictionaryPublisher.pendingText()
    @Environment(\.openURL) private var openURL

    var body: some View {
        NavigationStack(path: $path) {
            Form {
                Section("How it publishes") {
                    if DictionaryPublisher.usesGit {
                        Text("With git on this Mac, to github.com/\(CommunityDictionary.source.repo).")
                    } else if !GitHub.configured {
                        Text("Publishing through GitHub isn’t set up in this build yet.")
                    } else if let login = publisher.login {
                        Text("Signed in to GitHub as \(login)")
                        Button("Sign Out") {
                            publisher.signOut()
                            AccessibilityNotification.Announcement("Signed out").post()
                        }
                    } else {
                        Button("Sign In with GitHub") { path.append(false) }
                    }
                    if let fork = GitHub.fork, !DictionaryPublisher.usesGit {
                        Text("Publishing to your copy, \(fork). To offer your changes for everyone, open a pull request from it on GitHub.")
                    }
                }
                Section("Your changes") {
                    Text(pending.map { "Not published yet: \($0)" } ?? "Nothing of yours to publish.")
                    Button(publisher.busy ? "Publishing…" : "Publish My Changes Now") { publish() }
                        .disabled(publisher.busy)
                }
                Section {
                    Toggle("Publish automatically once a day", isOn: $publisher.automatic)
                    if let d = publisher.lastPublished {
                        Text("Last published: \(d.formatted(date: .abbreviated, time: .shortened))")
                    }
                    if let e = publisher.lastError {
                        Text("Last try: \(e)").foregroundStyle(.red)
                    }
                }
            }
            .formStyle(.grouped)
            .navigationTitle("Publishing")
            .navigationDestination(for: Bool.self) { then in
                SignInPage { tokens in
                    publisher.signedIn(tokens)
                    path.removeAll()
                    if then { publish() }
                }
            }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { publisher.showPublishing = false } }
            }
        }
        #if os(macOS)
        .frame(minWidth: 460, minHeight: 380)
        #endif
        .onAppear {
            if publisher.signInFirst { publisher.signInFirst = false; path = [true] }
        }
        .alert(item: $publisher.alert) { a in
            switch a {
            case .result(let title, let message):
                Alert(title: Text(title), message: Text(message), dismissButton: .default(Text("OK")))
            case .forkOffer(let repo):
                Alert(title: Text("Publish to Your Own Copy?"),
                      message: Text("You can’t publish to \(repo). Classic Voices can make your own copy of it (a fork) on GitHub and publish your changes there. To offer them for everyone, open a pull request from your copy on GitHub."),
                      primaryButton: .default(Text("Make My Copy")) { Task { await publisher.makeFork(); pending = DictionaryPublisher.pendingText() } },
                      secondaryButton: .cancel())
            case .install:
                Alert(title: Text("Allow Classic Voices on GitHub"),
                      message: Text("To make your copy, GitHub needs Classic Voices allowed on your account. Choose All repositories, then publish again."),
                      primaryButton: .default(Text("Open GitHub")) { if let u = GitHub.installURL { openURL(u) } },
                      secondaryButton: .cancel())
            }
        }
    }

    private func publish() {
        guard publisher.canPublish else { path.append(true); return }  // no way to publish: sign in first
        Task {
            await publisher.publishNow()
            pending = DictionaryPublisher.pendingText()
        }
    }
}

/// Signing in with GitHub's device flow: the code, a button that copies it and opens GitHub, and the wait.
private struct SignInPage: View {
    let done: (GitHub.Tokens) -> Void
    @State private var code: GitHub.DeviceCode?
    @State private var problem: String?
    @State private var attempt = 0
    @Environment(\.openURL) private var openURL

    var body: some View {
        Form {
            if let problem {
                Section {
                    Text(problem)
                    Button("Try Again") { self.problem = nil; attempt += 1 }
                }
            } else if let code {
                Section {
                    Text("Go to \(code.verification.host ?? "github.com")\(code.verification.path) and enter the code \(spelled(code.userCode)).")
                        .accessibilityLabel("Go to \(code.verification.absoluteString) and enter the code \(spelled(code.userCode)).")
                    Button("Copy Code and Open GitHub") {
                        copy(code.userCode)
                        openURL(code.verification)
                    }
                    Text("Waiting for GitHub…")
                }
            } else {
                Text("Asking GitHub for a code…")
            }
        }
        .formStyle(.grouped)
        .navigationTitle("Sign In with GitHub")
        .task(id: attempt) {
            do {
                let c = try await GitHub.startSignIn()
                code = c
                AccessibilityNotification.Announcement("Your code is \(spelled(c.userCode))").post()
                let t = try await GitHub.finishSignIn(c)
                AccessibilityNotification.Announcement("Signed in as \(t.login)").post()
                done(t)
            } catch is CancellationError {
            } catch {
                code = nil
                problem = error.localizedDescription
            }
        }
    }

    /// The code as separate characters, so VoiceOver reads it out letter by letter.
    private func spelled(_ s: String) -> String { s.map { $0 == "-" ? "dash" : String($0) }.joined(separator: " ") }

    private func copy(_ s: String) {
        #if canImport(UIKit)
        UIPasteboard.general.string = s
        #elseif canImport(AppKit)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(s, forType: .string)
        #endif
    }
}

/// RootView's part: the Publishing sheet and the question when leaving the dictionaries.
struct PublishingPrompts: ViewModifier {
    @State private var publisher = DictionaryPublisher.shared

    func body(content: Content) -> some View {
        content
            .sheet(isPresented: $publisher.showPublishing) { PublishingSheet() }
            .alert("Sign in to GitHub to publish your changes?", isPresented: $publisher.leaveAsk) {
                Button("Yes") { publisher.open(signIn: true) }
                Button("No", role: .cancel) {}
                Button("Don’t Show Again") { DictionaryPublisher.dontAsk = true }
            } message: {
                Text("Your dictionary changes are only on this device until they’re published.")
            }
    }
}
