import Foundation
import Observation
#if os(macOS)
import AppKit
#endif

/// Is there a newer Classic Voices? Asked as the other platforms ask
/// (nvda/.../update.py): GitHub at most once a day, in the background, and
/// each new version offered once. These apps carry Apple's code, so they're
/// built in each person's own GitHub account and never in a release: the
/// update is building it again. The release page says what's new. The
/// released Mac app (CV_PUBLIC) installs the update itself (MacUpdater).
@MainActor @Observable
final class UpdateCheck {
    static let shared = UpdateCheck()

    struct Asset: Equatable {
        let url: URL
        let size: Int
        let sha256: String?  // GitHub's digest of the file
    }

    struct Release: Identifiable, Equatable {
        let version: String
        let page: URL
        var assets: [String: Asset] = [:]
        var id: String { version }
    }

    /// A release to tell about, until it has been told.
    var available: Release?

    static let releases = URL(string: "https://api.github.com/repos/Levi-Gobin/classicvoices/releases?per_page=10")!
    static let page = URL(string: "https://github.com/Levi-Gobin/classicvoices/releases")!
    /// What builds it again on a Mac (README, "macOS").
    static let buildCommand = "bash <(curl -fsSL https://raw.githubusercontent.com/Levi-Gobin/classicvoices/main/classicvoices.sh)"
    nonisolated static var current: String { Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0" }

    /// The first three numbers of a version, compared as numbers.
    nonisolated static func number(_ v: String) -> [Int] {
        let parts = v.split { !$0.isNumber }.prefix(3).compactMap { Int($0) }
        return parts + Array(repeating: 0, count: 3 - parts.count)
    }

    nonisolated static func isNewer(_ a: String, than b: String) -> Bool {
        number(b).lexicographicallyPrecedes(number(a))
    }

    func check() async {
        let defaults = UserDefaults.standard
        // For testing an update: `-updateFeed http://127.0.0.1:8765/releases.json`
        // asks there instead, every launch.
        let feed = defaults.string(forKey: "updateFeed").flatMap(URL.init(string:))
        // not during the command-line test runs (-benchmark, -download, ...)
        guard feed != nil || !CommandLine.arguments.dropFirst().contains(where: { $0.hasPrefix("-") && !$0.hasPrefix("-NS") && !$0.hasPrefix("-Apple") })
        else { return }
        let now = Date().timeIntervalSince1970
        guard feed != nil || now - defaults.double(forKey: "update.checked") >= 24 * 3600 else { return }
        var request = URLRequest(url: feed ?? Self.releases, timeoutInterval: 10)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("ClassicVoices/\(Self.current)", forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200,
              let list = (try? JSONSerialization.jsonObject(with: data)) as? [[String: Any]] else { return }
        defaults.set(now, forKey: "update.checked")
        let announced = feed != nil ? "0" : defaults.string(forKey: "update.announced") ?? "0"
        var found: Release?
        for release in list where release["draft"] as? Bool != true {
            let v = ((release["tag_name"] as? String) ?? "").trimmingCharacters(in: CharacterSet(charactersIn: "v"))
            guard Self.isNewer(v, than: Self.current), Self.isNewer(v, than: announced),
                  found.map({ Self.isNewer(v, than: $0.version) }) ?? true else { continue }
            var assets: [String: Asset] = [:]
            for a in release["assets"] as? [[String: Any]] ?? [] {
                guard let name = a["name"] as? String, let url = (a["browser_download_url"] as? String).flatMap(URL.init(string:))
                else { continue }
                let digest = a["digest"] as? String ?? ""
                assets[name] = Asset(url: url, size: a["size"] as? Int ?? 0,
                                     sha256: digest.hasPrefix("sha256:") ? String(digest.dropFirst(7)).lowercased() : nil)
            }
            found = Release(version: v, page: (release["html_url"] as? String).flatMap(URL.init(string:)) ?? Self.page,
                            assets: assets)
        }
        guard let found else { return }
        defaults.set(found.version, forKey: "update.announced")
        #if CV_PUBLIC
        // testing with a feed: `-updateFeed … -updateNow YES` installs it without asking
        if feed != nil, defaults.bool(forKey: "updateNow"), MacUpdater.canInstall(found) {
            MacUpdater.shared.install(found)
            return
        }
        #endif
        available = found
    }

    var message: String {
        #if CV_PUBLIC
        if available.map(MacUpdater.canInstall) == true {
            return "You have \(Self.current). Update Now downloads it, checks it and opens the new version. Your voices stay."
        }
        return "You have \(Self.current). Download the new version from its release page and drag it into Applications. Your voices stay."
        #elseif os(macOS)
        "You have \(Self.current). The Mac app is built in your GitHub account, so to update it, run the build command in Terminal again and choose the Mac app. Copy Command puts it on the clipboard. Your voices stay."
        #else
        "You have \(Self.current). To update, build it again the way you first did and install the new app over this one. Your voices stay."
        #endif
    }

    #if os(macOS)
    func copyCommand() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(Self.buildCommand, forType: .string)
    }
    #endif
}
