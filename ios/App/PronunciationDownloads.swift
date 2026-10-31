import CryptoKit
import Foundation
import Observation

/// Fetches Apple's later pronunciation packs (PronunciationPacks) the first
/// time the newer rules are turned on, for any voice: every language at once,
/// since all of them together are under 5 MB. Each archive is checked against
/// the SHA-1 Apple's catalog gives, as the enhanced voices are, and the three
/// files the engine loads go to the speech extension's store under
/// Pronunciation/<language>/, with a copy kept in the app for Try It.
@MainActor @Observable
final class PronunciationDownloads {
    /// 0...1 in whole percents while downloading.
    private(set) var progress: Double?
    private(set) var bytes = (written: 0, total: 0)
    var error: String?
    private(set) var installed = false
    @ObservationIgnored private var task: Task<Void, Never>?

    func refresh() {
        installed = VoiceStore.shared.snapshot.paths.contains {
            $0.hasPrefix(PronunciationPacks.folder + "/") && $0.hasSuffix("/vocalizer-user-rules.txt")
        }
        if installed { error = nil }
    }

    /// Starts the download unless the packs are here, coming, or just failed
    /// (then it waits for Try Again).
    func ensure() {
        refresh()
        guard !installed, progress == nil, error == nil else { return }
        download()
    }

    func download() {
        guard progress == nil else { return }
        progress = 0
        error = nil
        let now = VoiceStore.isForeground
        task = Task.detached(priority: .userInitiated) {
            do {
                try await Self.fetch { written, total in
                    Task { @MainActor in self.report(written, total) }
                }
                try Self.install(now: now)
                await MainActor.run {
                    self.progress = nil
                    self.refresh()
                }
            } catch {
                await MainActor.run {
                    self.progress = nil
                    if !(error is CancellationError) { self.error = error.localizedDescription }
                }
            }
        }
    }

    func cancel() {
        task?.cancel()
        task = nil
        progress = nil
    }

    private func report(_ written: Int, _ total: Int) {
        guard progress != nil else { return }
        bytes = (written, total)
        let v = (Double(written) / Double(max(total, 1)) * 100).rounded(.down) / 100
        if progress != v { progress = v }
    }

    // MARK: - steps

    private struct Asset { let language: String; let url: URL; let size: Int; let sha1: Data }

    nonisolated private static var staging: URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("pronunciation-packs")
    }

    /// Catalog, then each language's archive, into staging/Pronunciation/<language>/.
    nonisolated private static func fetch(progress: @escaping @Sendable (Int, Int) -> Void) async throws {
        let (data, response) = try await URLSession.shared.data(from: PronunciationPacks.catalog)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 { throw failure("Apple's server answered HTTP \(http.statusCode)") }
        guard let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let all = plist["Assets"] as? [[String: Any]] else { throw failure("Apple's pronunciation catalog couldn't be read") }
        let assets: [Asset] = all.compactMap { a in
            guard (a["_CompatibilityVersion"] as? Int) == PronunciationPacks.compatibilityVersion,
                  let language = (a["Languages"] as? [String])?.first,
                  let base = a["__BaseURL"] as? String, let rel = a["__RelativePath"] as? String,
                  let url = URL(string: base + rel), let sha1 = a["_Measurement"] as? Data else { return nil }
            return Asset(language: language, url: url, size: a["_DownloadSize"] as? Int ?? 0, sha1: sha1)
        }
        guard !assets.isEmpty else { throw failure("Apple's catalog no longer lists the pronunciation packs") }
        let fm = FileManager.default
        try? fm.removeItem(at: staging)
        let out = staging.appendingPathComponent(PronunciationPacks.folder)
        try fm.createDirectory(at: out, withIntermediateDirectories: true)
        let total = assets.reduce(0) { $0 + $1.size }
        var written = 0
        progress(0, total)
        for a in assets {
            try Task.checkCancellation()
            let (file, resp) = try await URLSession.shared.download(from: a.url)
            defer { try? fm.removeItem(at: file) }
            if let http = resp as? HTTPURLResponse, http.statusCode != 200 { throw failure("Apple's server answered HTTP \(http.statusCode)") }
            let zip = try Data(contentsOf: file)
            guard Data(Insecure.SHA1.hash(data: zip)) == a.sha1 else { throw failure("Checksum mismatch (\(a.language))") }
            let unpacked = staging.appendingPathComponent("unpack-\(a.language)")
            try? fm.removeItem(at: unpacked)
            try ZipExtractor.extract(file, to: unpacked)
            let dest = out.appendingPathComponent(a.language)
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            for name in PronunciationPacks.files {
                let src = unpacked.appendingPathComponent("AssetData/\(name)")
                if fm.fileExists(atPath: src.path) { try fm.moveItem(at: src, to: dest.appendingPathComponent(name)) }
            }
            try? fm.removeItem(at: unpacked)
            written += a.size
            progress(written, total)
        }
    }

    /// The app's own copy, then the store's (through a job, so it lands even
    /// if the extension can't be reached right now).
    nonisolated private static func install(now: Bool) throws {
        let fm = FileManager.default
        let packs = staging.appendingPathComponent(PronunciationPacks.folder)
        defer { try? fm.removeItem(at: staging) }
        if let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask).first {
            let mine = support.appendingPathComponent(PronunciationPacks.folder)
            try? fm.removeItem(at: mine)
            try fm.createDirectory(at: support, withIntermediateDirectories: true)
            try fm.copyItem(at: packs, to: mine)
        }
        let job = VoiceStore.Job(items: [.init(local: PronunciationPacks.folder, dest: PronunciationPacks.folder)])
        try VoiceStore.shared.enqueue(job, payloads: [packs], run: now)
    }

    nonisolated private static func failure(_ s: String) -> Error {
        NSError(domain: "ClassicVoices", code: 7, userInfo: [NSLocalizedDescriptionKey: s])
    }
}
