import AVFoundation
import CryptoKit
import Foundation
import Observation

/// Downloads the original iOS 5/6 enhanced voices from Apple's MobileAsset
/// servers (as the phone's Settings did), verifies them, and unpacks them into
/// the App Group so the speech extension can use them.
@MainActor @Observable
final class EnhancedDownloads {
    struct Asset: Codable, Hashable, Identifiable {
        let build: String       // "ios5" / "ios6"
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
    @ObservationIgnored private var tasks: [String: Task<Void, Never>] = [:]

    /// Stops a download in progress; nothing is left behind.
    func cancel(_ id: String) {
        tasks[id]?.cancel()
        tasks[id] = nil
        progress[id] = nil
    }

    /// Progress is published in whole percents: every update redraws the rows.
    private func report(_ id: String, _ p: Double) {
        let v = (p * 100).rounded(.down) / 100
        if progress[id] != nil && progress[id] != v { progress[id] = v }
    }

    init() { refresh() }

    func refresh() {
        installed = Set(Self.catalog.filter { isInstalled($0) }.map(\.id))
    }

    func isInstalled(_ a: Asset) -> Bool {
        guard let dir = VoiceCatalog.enhancedDirectory(build: a.build, language: a.language) else { return false }
        return FileManager.default.fileExists(atPath: dir.appendingPathComponent("broker.hdr.asset").path)
    }

    /// Name of the built-in voice for the same build/language (enhanced voices share it).
    static func voiceName(_ a: Asset) -> String {
        VoiceCatalog.entries.first { $0.build == a.build && $0.language == a.language && !$0.enhanced }?.name ?? a.language
    }

    func download(_ a: Asset) {
        guard progress[a.id] == nil, let url = URL(string: a.url) else { return }
        progress[a.id] = 0
        errors[a.id] = nil
        tasks[a.id] = Task.detached(priority: .userInitiated) {
            do {
                let zip = try await Self.fetch(url) { p in Task { @MainActor in self.report(a.id, p * 0.9) } }
                defer { try? FileManager.default.removeItem(at: zip) }
                try Self.verify(zip, sha1: a.sha1)
                try Self.install(zip, asset: a)
                await MainActor.run {
                    self.tasks[a.id] = nil
                    self.progress[a.id] = nil
                    self.refresh()
                    self.onChange()
                    AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                }
            } catch {
                await MainActor.run {
                    self.tasks[a.id] = nil
                    self.progress[a.id] = nil
                    if !Task.isCancelled && (error as? URLError)?.code != .cancelled && !(error is CancellationError) {
                        self.errors[a.id] = error.localizedDescription
                    }
                }
            }
        }
    }

    func remove(_ a: Asset) {
        if let dir = VoiceCatalog.enhancedDirectory(build: a.build, language: a.language) {
            try? FileManager.default.removeItem(at: dir)
        }
        refresh()
        onChange()
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
    }

    // MARK: - steps

    private final class ProgressDelegate: NSObject, URLSessionDownloadDelegate {
        let report: @Sendable (Double) -> Void
        init(_ report: @escaping @Sendable (Double) -> Void) { self.report = report }
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                        totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
            if totalBytesExpectedToWrite > 0 { report(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)) }
        }
        func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {}
    }

    nonisolated private static func fetch(_ url: URL, progress: @escaping @Sendable (Double) -> Void) async throws -> URL {
        let (file, response) = try await URLSession.shared.download(from: url, delegate: ProgressDelegate(progress))
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw NSError(domain: "ClassicVoices", code: 3, userInfo: [NSLocalizedDescriptionKey: "HTTP \(http.statusCode)"])
        }
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".zip")
        try FileManager.default.moveItem(at: file, to: tmp)
        return tmp
    }

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

    nonisolated private static func install(_ zip: URL, asset a: Asset) throws {
        guard let dest = VoiceCatalog.enhancedDirectory(build: a.build, language: a.language) else {
            throw NSError(domain: "ClassicVoices", code: 2, userInfo: [NSLocalizedDescriptionKey: "App Group unavailable"])
        }
        let staging = dest.deletingLastPathComponent().appendingPathComponent(".staging-\(a.language)")
        try? FileManager.default.removeItem(at: staging)
        try ZipExtractor.extract(zip, to: staging)
        // Archives hold AssetData/<lang>.tmp/...
        let payload = staging.appendingPathComponent("AssetData/\(a.language).tmp")
        try? FileManager.default.removeItem(at: dest)
        try FileManager.default.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        try FileManager.default.moveItem(at: payload, to: dest)
        try? FileManager.default.removeItem(at: staging)
    }
}
