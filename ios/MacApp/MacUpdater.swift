import AppKit
import Foundation
import Observation
import Security
import SwiftUI

/// The released Mac app updating itself, as the other platforms do
/// (nvda/.../update.py): the release's ClassicVoices-mac.zip is downloaded,
/// checked against the size and SHA-256 GitHub publishes for it and against
/// its signature (Developer ID, this team, this app), put in place of this
/// copy, and opened. The engines, fast code and voices live outside the app
/// (App Group, the extension's store), so they stay; fast code made by an
/// older version is rebuilt (FastCodeBuilder.stamp).
///
/// Updating automatically (the default; Settings, "Update automatically"), a
/// new version is downloaded and checked in the background (prepare) and put
/// in place when Classic Voices quits (installOnQuit), as Sparkle does; Restart
/// Now installs it at once. Off, the person is asked first (install).
@MainActor @Observable
final class MacUpdater {
    static let shared = MacUpdater()
    static let asset = "ClassicVoices-mac.zip"
    static let team = "7WV49AN2BX"

    private(set) var status: String?
    private(set) var fraction: Double?
    private(set) var error: String?
    private(set) var running = false
    /// Downloaded and checked, waiting to be put in place.
    private(set) var ready: (version: String, app: URL)?
    private var preparing = false

    static func canInstall(_ r: UpdateCheck.Release) -> Bool { r.assets[asset] != nil }

    static var automatic: Bool {
        UserDefaults.standard.object(forKey: "update.automatic") as? Bool ?? true
    }

    /// Whether this copy can update itself without asking: this account can
    /// replace it where it is.
    static var canUpdateQuietly: Bool {
        FileManager.default.isWritableFile(atPath: Bundle.main.bundleURL.deletingLastPathComponent().path)
    }

    /// Updating automatically: download and check the new version quietly. A
    /// failure leaves it to the next check (or, on its own, to the person:
    /// UpdateCheck shows the release when this gives up).
    func prepare(_ release: UpdateCheck.Release, failed: @escaping @MainActor () -> Void) {
        guard !running, !preparing, ready?.version != release.version, let a = release.assets[Self.asset] else { return }
        preparing = true
        Thread.detachNewThread { [weak self] in
            do {
                let new = try Self.fetch(a, version: release.version) { _ in }
                Task { @MainActor in
                    self?.preparing = false
                    self?.ready = (release.version, new)
                    if UserDefaults.standard.bool(forKey: "updateQuitWhenReady") { NSApp.terminate(nil) }
                }
            } catch {
                Task { @MainActor in
                    self?.preparing = false
                    failed()
                }
            }
        }
    }

    /// Restart Now: the downloaded version, put in place and opened.
    func installReady() {
        guard let r = ready, !running else { return }
        running = true
        status = "Installing Classic Voices \(r.version)"
        let app = Bundle.main.bundleURL
        do {
            try Self.replace(app, with: r.app)
            Self.relaunch(app)
        } catch {
            running = false
            status = nil
            self.error = error.localizedDescription
        }
    }

    /// Classic Voices is quitting: a downloaded version takes this one's place,
    /// and the system is told about its speech extension, as after an install.
    func installOnQuit() {
        guard let r = ready, !running else { return }
        ready = nil
        let app = Bundle.main.bundleURL
        guard (try? Self.replace(app, with: r.app)) != nil else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f \"$1\"; "
                       + "killall axassetsd 2>/dev/null", "sh", app.path]
        try? p.run()
        p.waitUntilExit()
    }

    func install(_ release: UpdateCheck.Release) {
        guard !running, let a = release.assets[Self.asset] else { return }
        running = true
        error = nil
        status = "Downloading Classic Voices \(release.version)"
        fraction = 0
        AccessibilityNotification.Announcement(status!).post()
        let app = Bundle.main.bundleURL
        Thread.detachNewThread { [weak self] in
            do {
                let new = try Self.fetch(a, version: release.version) { f in
                    Task { @MainActor in
                        let r = (f * 100).rounded(.down) / 100
                        if r != self?.fraction { self?.fraction = r }
                    }
                }
                Task { @MainActor in
                    self?.status = "Installing Classic Voices \(release.version)"
                    self?.fraction = nil
                    AccessibilityNotification.Announcement("Installing Classic Voices \(release.version)").post()
                }
                try Self.replace(app, with: new)
                Task { @MainActor in Self.relaunch(app) }
            } catch {
                Task { @MainActor in
                    self?.running = false
                    self?.status = nil
                    self?.fraction = nil
                    self?.error = error.localizedDescription
                    AccessibilityNotification.Announcement("The update didn\u{2019}t install: \(error.localizedDescription)").post()
                }
            }
        }
    }

    /// The new app, downloaded, checked and unpacked beside the old updates folder.
    /// What an update left behind (the folder it was unpacked in).
    nonisolated static func cleanUp() {
        try? FileManager.default.removeItem(at: FastCodeBuilder.cache.appendingPathComponent("Update"))
    }

    /// cleanUp, unless an update is being downloaded or waits there now.
    func cleanUpIfIdle() {
        if !preparing && ready == nil && !running { Self.cleanUp() }
    }

    nonisolated private static func fetch(_ a: UpdateCheck.Asset, version: String,
                                          progress: @escaping (Double) -> Void) throws -> URL {
        let fm = FileManager.default
        let dir = FastCodeBuilder.cache.appendingPathComponent("Update")
        try? fm.removeItem(at: dir)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let zip = dir.appendingPathComponent(asset)
        fm.createFile(atPath: zip.path, contents: nil)
        let out = try FileHandle(forWritingTo: zip)
        var hash = SHA256Stream()
        var got = 0
        do {
            try HTTP.stream(a.url, from: 0, to: UInt64(max(a.size, 1)) - 1) { chunk in
                try out.write(contentsOf: chunk)
                hash.update(chunk)
                got += chunk.count
                if a.size > 0 { progress(Double(got) / Double(a.size)) }
            }
            try out.close()
        } catch {
            try? out.close()
            throw error
        }
        if a.size > 0, got != a.size { throw UpdateError("The download was cut short.") }
        if let sha = a.sha256, hash.hex != sha { throw UpdateError("The download doesn\u{2019}t match what GitHub published.") }
        let unpacked = dir.appendingPathComponent("new")
        try run("/usr/bin/ditto", ["-x", "-k", zip.path, unpacked.path])
        try? fm.removeItem(at: zip)
        guard let name = try fm.contentsOfDirectory(atPath: unpacked.path).first(where: { $0.hasSuffix(".app") }) else {
            throw UpdateError("The download has no app in it.")
        }
        let app = unpacked.appendingPathComponent(name)
        try verify(app)
        return app
    }

    /// Signed with Developer ID by this team, as this app, and intact.
    nonisolated static func verify(_ app: URL) throws {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(app as CFURL, [], &code) == errSecSuccess, let code else {
            throw UpdateError("The new version can\u{2019}t be checked.")
        }
        let id = Bundle.main.bundleIdentifier ?? "com.levigobin.ClassicVoices.mac"
        let text = "identifier \"\(id)\" and anchor apple generic and certificate leaf[subject.OU] = \"\(team)\""
            + " and certificate 1[field.1.2.840.113635.100.6.2.6] exists"  // Developer ID
        var req: SecRequirement?
        guard SecRequirementCreateWithString(text as CFString, [], &req) == errSecSuccess else {
            throw UpdateError("The new version can\u{2019}t be checked.")
        }
        let flags = SecCSFlags(rawValue: kSecCSCheckAllArchitectures | kSecCSCheckNestedCode | kSecCSStrictValidate)
        guard SecStaticCodeCheckValidity(code, flags, req) == errSecSuccess else {
            throw UpdateError("The new version isn\u{2019}t signed by Classic Voices\u{2019} developer, so it wasn\u{2019}t installed.")
        }
    }

    /// Swaps the new app into this one's place (the running copy keeps its files open).
    nonisolated private static func replace(_ app: URL, with new: URL) throws {
        do {
            _ = try FileManager.default.replaceItemAt(app, withItemAt: new, backupItemName: nil, options: [])
        } catch {
            throw UpdateError("\(app.deletingLastPathComponent().path) can\u{2019}t be changed by this account. Download the new version from its release page instead.")
        }
    }

    /// Quits, and opens the new copy once this one has gone. The audio-unit
    /// cache is refreshed as tools/install_mac.sh does after an install.
    private static func relaunch(_ app: URL) {
        let pid = ProcessInfo.processInfo.processIdentifier
        let script = "while kill -0 \(pid) 2>/dev/null; do sleep 0.2; done; "
            + "/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f \"$1\"; "
            + "killall axassetsd 2>/dev/null; sleep 1; open \"$1\""
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", script, "sh", app.path]
        try? p.run()
        FastCodeBuilder.stopAll()
        NSApp.terminate(nil)
    }

    nonisolated private static func run(_ exe: String, _ args: [String]) throws {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        try p.run()
        p.waitUntilExit()
        if p.terminationStatus != 0 { throw UpdateError("Unpacking the update failed.") }
    }

    struct UpdateError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }
}

/// An update on its way, at the top of each voice page.
struct MacUpdateSection: View {
    @State private var updater = MacUpdater.shared
    @Environment(\.openURL) private var openURL

    var body: some View {
        if let ready = updater.ready, !updater.running, updater.error == nil {
            PageSection("Update ready") {
                Text("Classic Voices \(ready.version) has been downloaded. It\u{2019}s installed when you quit Classic Voices. Your voices stay.")
                Button("Restart Now") { updater.installReady() }
                Button("What\u{2019}s New") { openURL(UpdateCheck.page) }
            }
        }
        if updater.running || updater.error != nil {
            PageSection("Updating Classic Voices") {
                if let status = updater.status {
                    if let f = updater.fraction {
                        ProgressView(value: f) { Text(status) }
                            .accessibilityValue("\(Int(f * 100)) percent")
                    } else {
                        ProgressView { Text(status) }
                    }
                }
                if let error = updater.error {
                    Text(error).foregroundStyle(.red)
                    Button("Open the Release Page") { openURL(UpdateCheck.page) }
                }
            }
        }
    }
}
