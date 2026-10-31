import AVFoundation
import CryptoKit
import Foundation
import Observation

/// Downloads the original iOS 5/6/7 enhanced voices from Apple's MobileAsset
/// servers (as the phone's Settings did), verifies them, and hands them to the
/// speech extension's store (VoiceStore) so it can use them.
@MainActor @Observable
final class EnhancedDownloads {
    struct Asset: Codable, Hashable, Identifiable {
        let build: String       // "ios5" / "ios6" / "ios70"
        let language: String
        let url: String
        let size: Int
        let unarchivedSize: Int
        let sha1: String
        var id: String { "\(build)/\(language)" }
    }

    static let catalog: [Asset] = {
        guard let url = Bundle.main.url(forResource: "EnhancedCatalog", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return [] }
        return (try? JSONDecoder().decode([Asset].self, from: data)) ?? []
    }()

    var progress: [String: Double] = [:]   // asset id -> 0...1, in whole percents
    var errors: [String: String] = [:]
    private(set) var installed: Set<String> = []
    /// Called on the main actor when a voice was installed or removed.
    @ObservationIgnored var onChange: () -> Void = {}

    /// Stops a download in progress; nothing is left behind.
    func cancel(_ id: String) {
        Downloader.shared.cancel("enhanced:" + id)
        progress[id] = nil
    }

    /// Progress is published in whole percents: every update redraws the rows.
    private func report(_ id: String, _ p: Double) {
        let v = (p * 100).rounded(.down) / 100
        if progress[id] != nil && progress[id] != v { progress[id] = v }
    }

    init() {
        refresh()
        Downloader.shared.register(self, prefix: "enhanced")
    }

    func refresh() {
        installed = Set(Self.catalog.filter { isInstalled($0) }.map(\.id))
        for id in installed where progress[id] != nil { progress[id] = nil }  // finished while waiting
    }

    func isInstalled(_ a: Asset) -> Bool {
        VoiceStore.shared.snapshot.has("\(a.build)/\(a.language)/broker.hdr.asset")
    }

    /// Name of the built-in voice for the same build/language (enhanced voices share it).
    static func voiceName(_ a: Asset) -> String {
        VoiceCatalog.entries.first { $0.build == a.build && $0.language == a.language && !$0.enhanced }?.name ?? a.language
    }

    func download(_ a: Asset) {
        guard progress[a.id] == nil, let url = URL(string: a.url) else { return }
        progress[a.id] = 0
        errors[a.id] = nil
        Downloader.shared.start(url, id: "enhanced:" + a.id)
    }

    /// The download is done: check it, unpack it and tell the system.
    private func finished(_ a: Asset, _ result: Result<URL, Error>) {
        guard case .success(let zip) = result else {
            progress[a.id] = nil
            if case .failure(let error) = result, !(error is CancellationError), (error as? URLError)?.code != .cancelled {
                errors[a.id] = error.localizedDescription
            }
            return
        }
        if progress[a.id] == nil { progress[a.id] = 0.9 }
        report(a.id, 0.9)
        let now = VoiceStore.isForeground
        Task.detached(priority: .userInitiated) {
            defer { try? FileManager.default.removeItem(at: zip) }
            do {
                try Self.verify(zip, sha1: a.sha1)
                try Self.install(zip, asset: a, now: now)
                await MainActor.run {
                    if now { self.progress[a.id] = nil }  // else it clears when the store takes it
                    self.refresh()
                    self.onChange()
                    AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                }
            } catch {
                await MainActor.run {
                    self.progress[a.id] = nil
                    self.errors[a.id] = error.localizedDescription
                }
            }
        }
    }

    func remove(_ a: Asset) {
        Task.detached {
            do {
                try VoiceStore.shared.delete("\(a.build)/\(a.language)")
                try VoiceStore.shared.refresh()
            } catch {
                await MainActor.run { self.errors[a.id] = error.localizedDescription }
            }
            await MainActor.run {
                self.refresh()
                self.onChange()
                AVSpeechSynthesisProviderVoice.updateSpeechVoices()
            }
        }
    }

    // MARK: - steps

    nonisolated private static func verify(_ zip: URL, sha1: String) throws {
        let fh = try FileHandle(forReadingFrom: zip)
        defer { try? fh.close() }
        var h = Insecure.SHA1()
        while let chunk = try fh.read(upToCount: 1 << 20), !chunk.isEmpty { h.update(data: chunk) }
        let got = h.finalize().map { String(format: "%02x", $0) }.joined()
        if got != sha1 {
            throw NSError(domain: "ClassicVoices", code: 1, userInfo: [NSLocalizedDescriptionKey: "Checksum mismatch"])
        }
    }

    /// Unpacks the archive here, then hands the voice folder to the store.
    nonisolated private static func install(_ zip: URL, asset a: Asset, now: Bool) throws {
        let fm = FileManager.default
        let staging = fm.temporaryDirectory.appendingPathComponent("enhanced-\(a.build)-\(a.language)")
        try? fm.removeItem(at: staging)
        defer { try? fm.removeItem(at: staging) }
        try ZipExtractor.extract(zip, to: staging)
        // Archives hold AssetData/<lang>.tmp/...
        let payload = staging.appendingPathComponent("AssetData/\(a.language).tmp")
        let folder = staging.appendingPathComponent(a.language)
        try fm.moveItem(at: payload, to: folder)
        let job = VoiceStore.Job(items: [.init(local: a.language, dest: "\(a.build)/\(a.language)")])
        try VoiceStore.shared.enqueue(job, payloads: [folder], run: now)
    }
}

extension EnhancedDownloads: DownloadClient {
    nonisolated func downloadProgress(_ id: String, written: Int64, expected: Int64) {
        Task { @MainActor in
            guard let a = Self.catalog.first(where: { $0.id == id }) else { return }
            let total = expected > 0 ? expected : Int64(a.size)
            self.report(id, Double(written) / Double(max(total, 1)) * 0.9)
        }
    }

    nonisolated func downloadFinished(_ id: String, _ result: Result<URL, Error>) {
        Task { @MainActor in
            guard let a = Self.catalog.first(where: { $0.id == id }) else {
                if case .success(let f) = result { try? FileManager.default.removeItem(at: f) }
                return
            }
            self.finished(a, result)
        }
    }

    nonisolated func downloadRestored(_ id: String) {
        Task { @MainActor in if self.progress[id] == nil { self.progress[id] = 0 } }
    }
}
