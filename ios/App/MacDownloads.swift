import AVFoundation
import Foundation
import Observation

/// The voices Mac OS X Lion, Mountain Lion and Mavericks offered for download
/// (System Preferences › Speech), fetched from Apple's software-update CDN and
/// unpacked into the speech extension's store: <store>/<era>/<VoiceFolder>/.
///
/// A package holds the premium voice and its compact sibling; both are kept.
/// Lion and Mountain Lion voices run on the iOS 6 engine; Mavericks compacts
/// on iOS 7's, and Mavericks premium voices on iOS 10's (the only one we have
/// with both their newer pipeline and the vssq5 decoder).
@MainActor @Observable
final class MacDownloads {
    struct Voice: Codable, Hashable, Identifiable {
        let product: String
        let era: String      // "mac107" / "mac108" / "mac109"
        let name: String
        let locale: String
        let voiceID: String
        let version: String
        let gender: String
        let url: String
        let size: Int
        var id: String { "\(era)/\(product)" }
    }

    private struct File: Codable { let voices: [Voice] }

    static let catalog: [Voice] = {
        guard let url = Bundle.main.url(forResource: "MacVoiceCatalog", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return [] }
        return (try? JSONDecoder().decode(File.self, from: data))?.voices ?? []
    }()

    static func eraName(_ era: String) -> String {
        VoiceCatalog.macEras.first { $0.0 == era }.map { "Mac OS X \($0.2) \($0.1)" } ?? era
    }

    var progress: [String: Double] = [:]   // in whole percents
    var errors: [String: String] = [:]
    private(set) var installed: Set<String> = []
    @ObservationIgnored var onChange: () -> Void = {}

    /// Stops a download in progress; nothing is left behind.
    func cancel(_ id: String) {
        Downloader.shared.cancel("mac:" + id)
        progress[id] = nil
    }

    private func report(_ id: String, _ p: Double) {
        let v = (p * 100).rounded(.down) / 100
        if progress[id] != nil && progress[id] != v { progress[id] = v }
    }

    /// Voice folders a downloaded package installed ("Karen", "KarenCompact").
    nonisolated static func folders(of v: Voice) -> [String] {
        guard let list = VoiceStore.shared.snapshot.markers[marker(v)] else { return [] }
        return list.split(separator: "\n").map(String.init).filter { !$0.isEmpty }
    }

    /// The small file listing a package's folders: <era>/.installed/<product>
    nonisolated static func marker(_ v: Voice) -> String { "\(v.era)/.installed/\(v.product)" }

    init() {
        refresh()
        Downloader.shared.register(self, prefix: "mac")
    }

    func refresh() {
        let snap = VoiceStore.shared.snapshot
        installed = Set(Self.catalog.filter { snap.has(Self.marker($0)) }.map(\.id))
        for id in installed where progress[id] != nil { progress[id] = nil }  // finished while waiting
    }

    func download(_ v: Voice) {
        guard progress[v.id] == nil, let url = URL(string: v.url) else { return }
        progress[v.id] = 0
        errors[v.id] = nil
        Downloader.shared.start(url, id: "mac:" + v.id)
    }

    /// The package is here: unpack its voices into the App Group.
    private func finished(_ v: Voice, _ result: Result<URL, Error>) {
        guard case .success(let pkg) = result else {
            progress[v.id] = nil
            if case .failure(let error) = result, !(error is CancellationError), (error as? URLError)?.code != .cancelled {
                errors[v.id] = error.localizedDescription
            }
            return
        }
        if progress[v.id] == nil { progress[v.id] = 0.85 }
        report(v.id, 0.85)
        let now = VoiceStore.isForeground
        Task.detached(priority: .userInitiated) {
            defer { try? FileManager.default.removeItem(at: pkg) }
            do {
                try Self.install(pkg, voice: v, now: now) { p in Task { @MainActor in self.report(v.id, 0.85 + p * 0.15) } }
                await MainActor.run {
                    if now { self.progress[v.id] = nil }
                    self.refresh()
                    self.onChange()
                    AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                }
            } catch {
                await MainActor.run {
                    self.progress[v.id] = nil
                    self.errors[v.id] = error.localizedDescription
                }
            }
        }
    }

    func remove(_ v: Voice) {
        let folders = Self.folders(of: v).filter { !$0.contains("/") && !$0.contains("..") }
        Task.detached {
            do {
                for folder in folders { try VoiceStore.shared.delete("\(v.era)/\(folder)") }
                try VoiceStore.shared.delete(Self.marker(v))
                try VoiceStore.shared.refresh()
            } catch {
                await MainActor.run { self.errors[v.id] = error.localizedDescription }
            }
            await MainActor.run {
                self.refresh()
                self.onChange()
                AVSpeechSynthesisProviderVoice.updateSpeechVoices()
            }
        }
    }

    // MARK: - steps

    /// Unpacks the package here, then hands its voice folders to the store.
    nonisolated private static func install(_ pkg: URL, voice v: Voice, now: Bool, progress: @escaping (Double) -> Void) throws {
        let fm = FileManager.default
        let staging = fm.temporaryDirectory.appendingPathComponent("mac-\(v.era)-\(v.product)")
        try? fm.removeItem(at: staging)
        try fm.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: staging) }
        let folders = try MacVoicePackage.extract(pkg, to: staging, keep: { _ in true },
                                                  progress: progress)
        guard !folders.isEmpty else {
            throw NSError(domain: "ClassicVoices", code: 4, userInfo: [NSLocalizedDescriptionKey: "No voice in package"])
        }
        var job = VoiceStore.Job()
        for f in folders {
            try v.locale.write(to: staging.appendingPathComponent(f).appendingPathComponent("locale"), atomically: true, encoding: .utf8)
            job.items.append(.init(local: f, dest: "\(v.era)/\(f)"))
        }
        job.files.append(.init(dest: marker(v), data: Data(folders.joined(separator: "\n").utf8)))
        try VoiceStore.shared.enqueue(job, payloads: folders.map { staging.appendingPathComponent($0) }, run: now)
    }
}

extension MacDownloads: DownloadClient {
    nonisolated func downloadProgress(_ id: String, written: Int64, expected: Int64) {
        Task { @MainActor in
            guard let v = Self.catalog.first(where: { $0.id == id }) else { return }
            let total = expected > 0 ? expected : Int64(v.size)
            self.report(id, Double(written) / Double(max(total, 1)) * 0.85)
        }
    }

    nonisolated func downloadFinished(_ id: String, _ result: Result<URL, Error>) {
        Task { @MainActor in
            guard let v = Self.catalog.first(where: { $0.id == id }) else {
                if case .success(let f) = result { try? FileManager.default.removeItem(at: f) }
                return
            }
            self.finished(v, result)
        }
    }

    nonisolated func downloadRestored(_ id: String) {
        Task { @MainActor in if self.progress[id] == nil { self.progress[id] = 0 } }
    }
}
