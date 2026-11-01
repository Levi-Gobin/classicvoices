import AVFoundation
import CryptoKit
import Foundation
import Observation

/// iOS 7's Siri voices (Aaron, Nicky, ... from iOS 7.0; Arthur, Martha, ...
/// from 7.1), from Apple's MobileAsset servers, as iOS 7 fetched them for
/// Siri. Each voice's compact and enhanced version is its own download, into
/// the speech extension's store: <store>/siri7/<Name>[Compact]/. They run on
/// 7.0.6's engine.
@MainActor @Observable
final class SiriDownloads {
    struct Voice: Codable, Hashable, Identifiable {
        let name: String
        let locale: String
        let gender: String
        let footprint: String   // "compact" / "premium"
        let url: String
        let size: Int
        let unarchivedSize: Int
        let sha1: String
        var compact: Bool { footprint == "compact" }
        var folder: String { compact ? name + "Compact" : name }
        var id: String { "\(VoiceCatalog.siri.build)/\(folder)" }
        /// The identifier the system sees once it's here (VoiceCatalog.Entry.identifier).
        var identifier: String { "com.levigobin.ClassicVoices.\(VoiceCatalog.siri.build).\(folder)" }
    }

    private struct File: Codable { let voices: [Voice] }

    static let catalog: [Voice] = {
        guard let url = Bundle.main.url(forResource: "SiriVoiceCatalog", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return [] }
        return (try? JSONDecoder().decode(File.self, from: data))?.voices ?? []
    }()

    var progress: [String: Double] = [:]   // voice id -> 0...1, in whole percents
    var errors: [String: String] = [:]
    private(set) var installed: Set<String> = []
    @ObservationIgnored var onChange: () -> Void = {}

    /// Stops a download in progress; nothing is left behind.
    func cancel(_ id: String) {
        Downloader.shared.cancel("siri:" + id)
        progress[id] = nil
    }

    private func report(_ id: String, _ p: Double) {
        let v = (p * 100).rounded(.down) / 100
        if progress[id] != nil && progress[id] != v { progress[id] = v }
    }

    init() {
        refresh()
        Downloader.shared.register(self, prefix: "siri")
    }

    func refresh() {
        installed = Set(Self.catalog.filter { VoiceStore.shared.snapshot.has("\($0.id)/broker.hdr") }.map(\.id))
        for id in installed where progress[id] != nil { progress[id] = nil }
    }

    func download(_ v: Voice) {
        guard progress[v.id] == nil, let url = URL(string: v.url) else { return }
        progress[v.id] = 0
        errors[v.id] = nil
        Downloader.shared.start(url, id: "siri:" + v.id)
    }

    private func finished(_ v: Voice, _ result: Result<URL, Error>) {
        guard case .success(let zip) = result else {
            progress[v.id] = nil
            if case .failure(let error) = result, !(error is CancellationError), (error as? URLError)?.code != .cancelled {
                errors[v.id] = error.localizedDescription
            }
            return
        }
        if progress[v.id] == nil { progress[v.id] = 0.9 }
        report(v.id, 0.9)
        let now = VoiceStore.isForeground
        Task.detached(priority: .userInitiated) {
            defer { try? FileManager.default.removeItem(at: zip) }
            do {
                try Self.verify(zip, sha1: v.sha1)
                try Self.install(zip, voice: v, now: now)
                await MainActor.run {
                    if now { self.progress[v.id] = nil }  // else it clears when the store takes it
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
        Task.detached {
            do {
                try VoiceStore.shared.delete(v.id)
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

    /// Unpacks the archive here (AssetData/ holds the voice's files), writes
    /// its language beside them, then hands the folder to the store.
    nonisolated private static func install(_ zip: URL, voice v: Voice, now: Bool) throws {
        let fm = FileManager.default
        let staging = fm.temporaryDirectory.appendingPathComponent("siri-\(v.folder)")
        try? fm.removeItem(at: staging)
        defer { try? fm.removeItem(at: staging) }
        try ZipExtractor.extract(zip, to: staging)
        let payload = staging.appendingPathComponent("AssetData")
        guard fm.fileExists(atPath: payload.appendingPathComponent("broker.hdr").path) else {
            throw NSError(domain: "ClassicVoices", code: 2, userInfo: [NSLocalizedDescriptionKey: "The archive had no voice in it"])
        }
        try Data((v.locale + "\n").utf8).write(to: payload.appendingPathComponent("locale"))
        let folder = staging.appendingPathComponent(v.folder)
        try fm.moveItem(at: payload, to: folder)
        let job = VoiceStore.Job(items: [.init(local: v.folder, dest: v.id)])
        try VoiceStore.shared.enqueue(job, payloads: [folder], run: now)
    }
}

extension SiriDownloads: DownloadClient {
    nonisolated func downloadProgress(_ id: String, written: Int64, expected: Int64) {
        Task { @MainActor in
            guard let v = Self.catalog.first(where: { $0.id == id }) else { return }
            let total = expected > 0 ? expected : Int64(v.size)
            self.report(id, Double(written) / Double(max(total, 1)) * 0.9)
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
