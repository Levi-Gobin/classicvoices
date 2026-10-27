import AVFoundation
import Foundation
import Observation
#if canImport(UIKit)
import UIKit
#endif

/// MacinTalk voices too big to come with the app (Alex), taken from the
/// install disc they shipped on: Leopard's and Snow Leopard's, kept on
/// archive.org, and the Lion installer on Apple's servers.
///
/// A disc is gigabytes and the voice is one package on it, so it goes in steps:
/// 1. Plan, in front: read the disc's tables over HTTP (10-40 MB) to find the
///    package and the bytes of the disc file it's made from (one or two runs).
///    Everything read is kept.
/// 2. Download those runs, in 64 MB parts side by side. Not in the background
///    session: macOS gives that a few KB/s from archive.org. Finished parts
///    are kept, so a download that stops (the app closed) goes on from them.
/// 3. Read the package out of the kept reads and the runs, as if they were the
///    disc, keep only the voice, and hand it to the store.
///
/// <Application Support>/DiscDownloads/<gen>-<name>/: plan.json, blocks/<n>
/// (the reads), parts/<i> (the runs).
@MainActor @Observable
final class DiscDownloads {
    enum Step: Hashable {
        case file(String)    // a file on the current disc
        case disc(String)    // a stored xar member of that file, a disc itself
    }

    struct Voice: Hashable, Identifiable {
        let gen: String
        let name: String
        let url: String
        let path: [Step]       // to the package holding the voice
        let size: Int          // downloaded, about
        let installedSize: Int
        var folder: String { name + ".SpeechVoice" }
        var id: String { "\(gen)/\(name)" }
        var server: String { url.contains("archive.org") ? "archive.org" : "Apple" }
    }

    private static let voices = "System/Installation/Packages/AdditionalSpeechVoices.pkg"
    static let catalog: [Voice] = [
        Voice(gen: "mt105", name: "Alex",
              url: "https://archive.org/download/Mac_OS_X_Leopard_Install_DVD_Version_10.5_2Z691-6037-A_Apple_Inc._2007/"
                + "Mac%20OS%20X%20Leopard%20Install%20DVD%20Version%2010.5%20%282Z691-6037-A%29%20%28Apple%2C%20Inc.%29%282007%29.iso",
              path: [.file(voices)], size: 748_000_000, installedSize: 701_000_000),
        Voice(gen: "mt106", name: "Alex",
              url: "https://archive.org/download/MacOSX10.6-Snow-Leopard-Retail-2Z691-6428-A/Mac%20OS%20X%20Install%20DVD.dmg",
              path: [.file(voices)], size: 429_000_000, installedSize: 400_000_000),
        Voice(gen: "mt107", name: "Alex",
              url: "https://updates.cdn-apple.com/2021/macos/041-7683-20210614-E610947E-C7CE-46EB-8860-D26D71F0D3EA/InstallMacOSX.dmg",
              path: [.file("InstallMacOSX.pkg"), .disc("InstallESD.dmg"), .file("Packages/AdditionalSpeechVoices.pkg")],
              size: 452_000_000, installedSize: 421_000_000),
    ]

    nonisolated static let part: UInt64 = 64 << 20

    struct Plan: Codable {
        let url: String         // where the disc is, after redirects
        let size: UInt64
        let parts: [[UInt64]]   // [start, end) of each run
        var total: UInt64 { parts.reduce(0) { $0 + $1[1] - $1[0] } }
    }

    /// Tests (`-download disc:mt106/Alex -disc-scratch <dir>`): the voice goes
    /// to <dir>/<gen>-<folder> instead of the store.
    nonisolated(unsafe) static var scratch: URL?

    var progress: [String: Double] = [:]   // 0...1, in whole percents
    var errors: [String: String] = [:]
    @ObservationIgnored var onChange: () -> Void = {}
    @ObservationIgnored private var written: [String: [Int: Int64]] = [:]
    @ObservationIgnored private var working: Set<String> = []   // planning or unpacking
    @ObservationIgnored private var fetching: [String: () -> Void] = [:]   // and their background time
    @ObservationIgnored private var retries: [String: Int] = [:]
    @ObservationIgnored private let parts = PartDownloader()

    init() { resume() }

    nonisolated static func dir(_ v: Voice) -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("DiscDownloads/\(v.gen)-\(v.name)", isDirectory: true)
    }

    private func report(_ id: String, _ p: Double) {
        let v = (p * 100).rounded(.down) / 100
        if progress[id] != nil && progress[id] != v { progress[id] = v }
    }

    func download(_ v: Voice) {
        guard progress[v.id] == nil, !working.contains(v.id) else { return }
        progress[v.id] = 0
        errors[v.id] = nil
        retries = retries.filter { !$0.key.hasPrefix(v.id + "#") }
        working.insert(v.id)
        let done = Self.keepRunning("Finding \(v.name) on the disc")
        Task.detached(priority: .userInitiated) {
            defer { Task { @MainActor in done() } }
            do {
                let plan = try Self.plan(v)
                await MainActor.run {
                    self.working.remove(v.id)
                    guard self.progress[v.id] != nil else {   // stopped meanwhile
                        try? FileManager.default.removeItem(at: Self.dir(v))
                        return
                    }
                    self.report(v.id, 0.03)
                    self.fetch(v, plan)
                }
            } catch {
                await MainActor.run {
                    self.working.remove(v.id)
                    self.progress[v.id] = nil
                    self.errors[v.id] = error.localizedDescription
                }
                try? FileManager.default.removeItem(at: Self.dir(v))
            }
        }
    }

    /// Downloads the parts not here yet.
    private func fetch(_ v: Voice, _ plan: Plan) {
        guard fetching[v.id] == nil, let url = URL(string: plan.url) else { return }
        fetching[v.id] = Self.keepRunning("Downloading \(v.name)")
        let dir = Self.dir(v)
        var have: [Int: Int64] = [:]
        for (i, p) in plan.parts.enumerated() where Self.partDone(v, plan, i) { have[i] = Int64(p[1] - p[0]) }
        written[v.id] = have
        if have.count == plan.parts.count { return finishedParts(v) }
        for (i, p) in plan.parts.enumerated() where have[i] == nil {
            parts.start(url, range: p[0] ..< p[1], to: dir.appendingPathComponent("parts/\(i)"), id: "\(v.id)#\(i)",
                        progress: { n in Task { @MainActor in self.partProgress(v, plan, i, n) } },
                        done: { error in Task { @MainActor in self.partFinished(v, plan, i, error) } })
        }
    }

    private func partProgress(_ v: Voice, _ plan: Plan, _ i: Int, _ n: Int64) {
        guard fetching[v.id] != nil else { return }
        written[v.id, default: [:]][i] = n
        let got = written[v.id]!.values.reduce(0, +)
        report(v.id, 0.03 + 0.87 * Double(got) / Double(max(plan.total, 1)))
    }

    private func partFinished(_ v: Voice, _ plan: Plan, _ i: Int, _ error: Error?) {
        guard fetching[v.id] != nil else { return }
        if let error {
            // a part that stalls or drops is tried again a few times
            let key = "\(v.id)#\(i)"
            retries[key, default: 0] += 1
            if retries[key]! <= 3, let url = URL(string: plan.url) {
                let p = plan.parts[i]
                parts.start(url, range: p[0] ..< p[1], to: Self.dir(v).appendingPathComponent("parts/\(i)"), id: key,
                            progress: { n in Task { @MainActor in self.partProgress(v, plan, i, n) } },
                            done: { error in Task { @MainActor in self.partFinished(v, plan, i, error) } })
                return
            }
            cancel(v.id)
            errors[v.id] = error.localizedDescription
            return
        }
        if Self.partsDone(v, plan) { finishedParts(v) }
    }

    private func finishedParts(_ v: Voice) {
        fetching.removeValue(forKey: v.id)?()
        unpack(v)   // or when the app is next in front
    }

    /// Stops a download in progress; nothing is left behind.
    func cancel(_ id: String) {
        guard let v = Self.catalog.first(where: { $0.id == id }) else { return }
        progress[id] = nil
        parts.cancel(prefix: id + "#")
        fetching.removeValue(forKey: id)?()
        if !working.contains(id) { try? FileManager.default.removeItem(at: Self.dir(v)) }
    }

    func remove(_ v: Voice) {
        Task.detached {
            do {
                try VoiceStore.shared.delete("\(v.gen)/Voices/\(v.folder)")
                try VoiceStore.shared.refresh()
            } catch {
                await MainActor.run { self.errors[v.id] = error.localizedDescription }
            }
            await MainActor.run {
                self.onChange()
                AVSpeechSynthesisProviderVoice.updateSpeechVoices()
            }
        }
    }

    /// Goes on with downloads that stopped when the app was closed or left
    /// the screen, and unpacks finished ones. Called at launch and whenever
    /// the app comes to the front.
    func resume() {
        for v in Self.catalog where !working.contains(v.id) && fetching[v.id] == nil {
            guard let plan = Self.loadPlan(v) else {
                try? FileManager.default.removeItem(at: Self.dir(v))   // planning that never finished
                continue
            }
            if progress[v.id] == nil { progress[v.id] = 0.03 }
            if Self.partsDone(v, plan) { unpack(v) } else { fetch(v, plan) }
        }
    }

    private func unpack(_ v: Voice) {
        guard VoiceStore.isForeground, !working.contains(v.id) else { return }
        working.insert(v.id)
        if progress[v.id] == nil { progress[v.id] = 0.9 }
        report(v.id, 0.9)
        let done = Self.keepRunning("Unpacking \(v.name)")
        Task.detached(priority: .userInitiated) {
            defer { Task { @MainActor in done() } }
            do {
                try Self.unpack(v) { p in Task { @MainActor in self.report(v.id, 0.9 + p * 0.1) } }
                await MainActor.run {
                    self.working.remove(v.id)
                    self.progress[v.id] = nil
                    self.onChange()
                    AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                }
            } catch {
                await MainActor.run {
                    self.working.remove(v.id)
                    self.progress[v.id] = nil
                    self.errors[v.id] = error.localizedDescription
                }
            }
            try? FileManager.default.removeItem(at: Self.dir(v))
        }
    }

    /// Asks iOS for time to finish when the app leaves the screen.
    private static func keepRunning(_ name: String) -> () -> Void {
        #if canImport(UIKit)
        let id = UIApplication.shared.beginBackgroundTask(withName: name)
        return { if id != .invalid { UIApplication.shared.endBackgroundTask(id) } }
        #else
        return {}
        #endif
    }

    // MARK: - steps

    struct DiscError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    /// The package, read through every disc on the way to it.
    nonisolated private static func package(_ v: Voice, on src: RandomAccess) throws -> RandomAccess {
        func volume(_ disk: DiskImage) throws -> HFSVolume {
            guard let base = try disk.hfsVolumes().first else { throw DiscError("The \(v.server) disc for \(v.name) has no Mac volume on it.") }
            return try HFSVolume(disk, at: base)
        }
        var vol = try volume(DiskImage(src))
        var current: RandomAccess?
        for step in v.path {
            switch step {
            case .file(let p):
                guard let f = try vol.files(where: { $0 == p }).first else {
                    throw DiscError("\(p) isn\u{2019}t on the \(v.server) disc for \(v.name) any more.")
                }
                current = vol.source(f)
            case .disc(let name):
                guard let c = current else { throw DiscError("No file to open \(name) in") }
                let (offset, length) = try MacVoicePackage.member(name, of: SourceReader(c))
                vol = try volume(DiskImage(SliceSource(c, at: offset, size: length)))
                current = nil
            }
        }
        guard let current else { throw DiscError("No package for \(v.name)") }
        return current
    }

    nonisolated private static func plan(_ v: Voice) throws -> Plan {
        let fm = FileManager.default
        let dir = dir(v)
        try? fm.removeItem(at: dir)
        try fm.createDirectory(at: dir.appendingPathComponent("parts"), withIntermediateDirectories: true)
        guard let url = URL(string: v.url) else { throw DiscError("Bad address for \(v.name)") }
        let http = try HTTPSource(url, keep: dir.appendingPathComponent("blocks"))
        let pkg = try package(v, on: http)
        let (offset, length) = try MacVoicePackage.member("Payload", of: SourceReader(pkg))
        // The runs of the disc file the package's payload is made from.
        var runs: [[UInt64]] = []
        for r in pkg.sources(offset, length).sorted(by: { $0.lowerBound < $1.lowerBound }) {
            if let last = runs.last, r.lowerBound <= last[1] {
                runs[runs.count - 1][1] = max(last[1], r.upperBound)
            } else {
                runs.append([r.lowerBound, r.upperBound])
            }
        }
        guard !runs.isEmpty, runs.count <= 8 else { throw DiscError("\(v.name)\u{2019}s package is in too many pieces on the disc (\(runs.count)).") }
        // In 64 MB parts, downloaded side by side: archive.org gives each
        // connection about 1 MB/s.
        var parts: [[UInt64]] = []
        for r in runs {
            var at = r[0]
            while at < r[1] {
                let end = min(r[1], at + Self.part)
                parts.append([at, end])
                at = end
            }
        }
        let plan = Plan(url: http.url.absoluteString, size: http.size, parts: parts)
        try JSONEncoder().encode(plan).write(to: dir.appendingPathComponent("plan.json"))
        return plan
    }

    nonisolated private static func loadPlan(_ v: Voice) -> Plan? {
        guard let d = try? Data(contentsOf: dir(v).appendingPathComponent("plan.json")) else { return nil }
        return try? JSONDecoder().decode(Plan.self, from: d)
    }

    nonisolated private static func partDone(_ v: Voice, _ plan: Plan, _ i: Int) -> Bool {
        let size = (try? FileManager.default.attributesOfItem(atPath: dir(v).appendingPathComponent("parts/\(i)").path)[.size]) as? UInt64
        return size == plan.parts[i][1] - plan.parts[i][0]
    }

    nonisolated private static func partsDone(_ v: Voice, _ plan: Plan) -> Bool {
        plan.parts.indices.allSatisfy { partDone(v, plan, $0) }
    }

    nonisolated private static func unpack(_ v: Voice, progress: @escaping (Double) -> Void) throws {
        let fm = FileManager.default
        let dir = dir(v)
        guard let plan = loadPlan(v) else { throw DiscError("The download of \(v.name) was lost; download him again.") }
        let src = try SparseSource(dir, plan)
        let pkg = try package(v, on: src)
        let tree = dir.appendingPathComponent("tree")
        try? fm.removeItem(at: tree)
        let prefix = "System/Library/Speech/Voices/\(v.folder)/"
        try InstallerPackage.extract(SourceReader(pkg), size: pkg.size, archive: false, name: "AdditionalSpeechVoices.pkg",
                                     into: tree, keep: { ($0.hasPrefix("./") ? String($0.dropFirst(2)) : $0).hasPrefix(prefix) },
                                     progress: progress)
        let voice = tree.appendingPathComponent(prefix)
        guard fm.fileExists(atPath: voice.appendingPathComponent("Contents/Resources/VoiceDescription").path) else {
            throw DiscError("\(v.name) wasn\u{2019}t in the package on the \(v.server) disc.")
        }
        if let scratch {
            let to = scratch.appendingPathComponent("\(v.gen)-\(v.folder)")
            try? fm.removeItem(at: to)
            try fm.createDirectory(at: scratch, withIntermediateDirectories: true)
            try fm.moveItem(at: voice, to: to)
            return
        }
        let job = VoiceStore.Job(items: [.init(local: v.folder, dest: "\(v.gen)/Voices/\(v.folder)")])
        try VoiceStore.shared.enqueue(job, payloads: [voice])
    }
}

/// Range requests in an ordinary session, each written to its own file.
final class PartDownloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private final class Job {
        let id: String, dest: URL, expected: Int64
        let progress: (Int64) -> Void, done: (Error?) -> Void
        var saved = false
        init(id: String, dest: URL, expected: Int64, progress: @escaping (Int64) -> Void, done: @escaping (Error?) -> Void) {
            self.id = id; self.dest = dest; self.expected = expected; self.progress = progress; self.done = done
        }
    }

    private let lock = NSLock()
    private var jobs: [Int: Job] = [:]
    private lazy var session: URLSession = {
        let c = URLSessionConfiguration.default
        c.httpMaximumConnectionsPerHost = 8
        c.timeoutIntervalForRequest = 120
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 1
        return URLSession(configuration: c, delegate: self, delegateQueue: q)
    }()

    func start(_ url: URL, range: Range<UInt64>, to dest: URL, id: String,
               progress: @escaping (Int64) -> Void, done: @escaping (Error?) -> Void) {
        var request = URLRequest(url: url)
        request.setValue("bytes=\(range.lowerBound)-\(range.upperBound - 1)", forHTTPHeaderField: "Range")
        let task = session.downloadTask(with: request)
        lock.withLock { jobs[task.taskIdentifier] = Job(id: id, dest: dest, expected: Int64(range.count), progress: progress, done: done) }
        task.resume()
    }

    func cancel(prefix: String) {
        let ids = lock.withLock { Set(jobs.filter { $0.value.id.hasPrefix(prefix) }.keys) }
        session.getAllTasks { for t in $0 where ids.contains(t.taskIdentifier) { t.cancel() } }
    }

    private func job(_ task: URLSessionTask) -> Job? { lock.withLock { jobs[task.taskIdentifier] } }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite _: Int64) {
        job(downloadTask)?.progress(totalBytesWritten)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let j = job(downloadTask), (downloadTask.response as? HTTPURLResponse)?.statusCode == 206,
              let size = (try? FileManager.default.attributesOfItem(atPath: location.path)[.size]) as? Int64, size == j.expected
        else { return }
        try? FileManager.default.removeItem(at: j.dest)
        j.saved = (try? FileManager.default.moveItem(at: location, to: j.dest)) != nil
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let j = lock.withLock({ jobs.removeValue(forKey: task.taskIdentifier) }) else { return }
        if (error as? URLError)?.code == .cancelled { return }
        let code = (task.response as? HTTPURLResponse)?.statusCode ?? 0
        let server = task.originalRequest?.url?.host?.hasSuffix("archive.org") == true ? "archive.org" : "The server"
        j.done(j.saved ? nil : error ?? DiscDownloads.DiscError("\(server) answered HTTP \(code)"))
    }
}

// MARK: - reading a disc on a web server

/// A file on a web server, read with range requests in 4 MB blocks. Every
/// block fetched is also written to `keep`, for SparseSource to read later.
final class HTTPSource: RandomAccess, @unchecked Sendable {
    static let block: UInt64 = 4 << 20
    let url: URL
    let size: UInt64
    private let keep: URL
    private var blocks: [UInt64: Data] = [:]
    private let lock = NSLock()

    init(_ url: URL, keep: URL) throws {
        self.keep = keep
        try FileManager.default.createDirectory(at: keep, withIntermediateDirectories: true)
        // archive.org redirects to the server holding the item; ask that one from then on.
        var request = URLRequest(url: url)
        request.setValue("bytes=0-0", forHTTPHeaderField: "Range")
        let (_, response) = try Self.fetch(request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 206,
              let range = http.value(forHTTPHeaderField: "Content-Range"), let total = range.split(separator: "/").last.flatMap({ UInt64($0) }),
              let resolved = http.url
        else { throw DiscDownloads.DiscError("\(url.host ?? "The server") didn\u{2019}t answer as expected.") }
        self.url = resolved
        size = total
    }

    private static func fetch(_ request: URLRequest) throws -> (Data, URLResponse) {
        var out: Result<(Data, URLResponse), Error> = .failure(URLError(.unknown))
        let done = DispatchSemaphore(value: 0)
        var r = request
        r.timeoutInterval = 120
        URLSession.shared.dataTask(with: r) { data, response, error in
            if let data, let response { out = .success((data, response)) } else { out = .failure(error ?? URLError(.badServerResponse)) }
            done.signal()
        }.resume()
        done.wait()
        return try out.get()
    }

    private func load(_ b: UInt64) throws -> Data {
        if let d = lock.withLock({ blocks[b] }) { return d }
        let start = b * Self.block, end = min(start + Self.block, size)
        var request = URLRequest(url: url)
        request.setValue("bytes=\(start)-\(end - 1)", forHTTPHeaderField: "Range")
        var last: Error = URLError(.unknown)
        for attempt in 0..<5 {
            do {
                let (data, response) = try Self.fetch(request)
                if (response as? HTTPURLResponse)?.statusCode == 206, UInt64(data.count) == end - start {
                    try data.write(to: keep.appendingPathComponent(String(b)))
                    lock.withLock { blocks[b] = data }
                    return data
                }
                last = URLError(.badServerResponse)
            } catch { last = error }
            Thread.sleep(forTimeInterval: Double(attempt + 1))
        }
        throw last
    }

    func read(at offset: UInt64, count: Int) throws -> Data {
        let end = min(offset + UInt64(count), size)
        guard offset < end else { return Data() }
        let first = offset / Self.block, lastBlock = (end - 1) / Self.block
        // a block it hasn't got, and the one after: reads are mostly in order
        if lock.withLock({ blocks[first] == nil }) { prefetchBlocks(Array(first ... min(first + 1, (size - 1) / Self.block))) }
        var out = Data(capacity: Int(end - offset))
        for b in first ... lastBlock {
            let d = try load(b)
            let from = b == first ? Int(offset - b * Self.block) : 0
            let to = b == lastBlock ? Int(end - b * Self.block) : d.count
            out.append(d[d.startIndex + from ..< d.startIndex + to])
        }
        return out
    }

    func sources(_ offset: UInt64, _ count: UInt64) -> [Range<UInt64>] { [offset ..< offset + count] }

    func prefetch(_ ranges: [Range<UInt64>]) {
        var want = Set<UInt64>()
        for r in ranges where !r.isEmpty && r.lowerBound < size {
            for b in r.lowerBound / Self.block ... (min(r.upperBound, size) - 1) / Self.block { want.insert(b) }
        }
        prefetchBlocks(want.sorted())
    }

    /// Several at once: one connection to archive.org gives about 1 MB/s.
    private func prefetchBlocks(_ list: [UInt64]) {
        let missing = lock.withLock { list.filter { blocks[$0] == nil } }
        guard missing.count > 1 else { return }
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 8
        for b in missing { q.addOperation { _ = try? self.load(b) } }
        q.waitUntilAllOperationsAreFinished()
    }
}

/// The disc again, from what planning read (blocks/) and the downloaded runs
/// (parts/). Reading anything else is an error: it would mean the disc on the
/// server changed under the plan.
final class SparseSource: RandomAccess {
    let size: UInt64
    private let dir: URL
    private let parts: [(Range<UInt64>, FileHandle)]
    private var cached: (UInt64, Data)?

    init(_ dir: URL, _ plan: DiscDownloads.Plan) throws {
        self.dir = dir
        size = plan.size
        parts = try plan.parts.enumerated().map { i, p in
            (p[0] ..< p[1], try FileHandle(forReadingFrom: dir.appendingPathComponent("parts/\(i)")))
        }
    }

    deinit { for (_, fh) in parts { try? fh.close() } }

    private func block(_ b: UInt64) throws -> Data {
        if let c = cached, c.0 == b { return c.1 }
        guard let d = try? Data(contentsOf: dir.appendingPathComponent("blocks/\(b)")) else {
            throw DiscDownloads.DiscError("The disc changed on the server while it downloaded; download again.")
        }
        cached = (b, d)
        return d
    }

    func read(at offset: UInt64, count: Int) throws -> Data {
        let end = min(offset + UInt64(count), size)
        var out = Data(capacity: Int(end - min(offset, end)))
        var pos = offset
        while pos < end {
            if let (r, fh) = parts.first(where: { $0.0.contains(pos) }) {
                let take = min(end, r.upperBound) - pos
                try fh.seek(toOffset: pos - r.lowerBound)
                out.append(try fh.read(upToCount: Int(take)) ?? Data())
                pos += take
            } else {
                let b = pos / HTTPSource.block
                let d = try block(b)
                let from = Int(pos - b * HTTPSource.block)
                var take = min(UInt64(d.count - from), end - pos)
                // stop where a part begins: it may be newer than the block
                if let next = parts.map(\.0.lowerBound).filter({ $0 > pos }).min() { take = min(take, next - pos) }
                out.append(d[d.startIndex + from ..< d.startIndex + from + Int(take)])
                pos += take
            }
        }
        return out
    }

    func sources(_ offset: UInt64, _ count: UInt64) -> [Range<UInt64>] { [offset ..< offset + count] }
    func prefetch(_ ranges: [Range<UInt64>]) {}
}
