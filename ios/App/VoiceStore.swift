import AVFoundation
import Foundation
#if os(iOS)
import UIKit
#endif

/// The app's handle on the speech extension's voice store.
///
/// Downloaded and added voices have to reach the extension, which runs in its
/// own sandbox. Earlier versions used an App Group, but sideloading tools
/// often strip that entitlement and the Mac warns about it, so the app now
/// hosts its own audio unit and sends files over the unit's message channel
/// into the extension's container (StoreOps / FileChannel). When the unit
/// can't be hosted but an App Group exists, the same operations run on the
/// group folder directly.
///
/// All methods block: call them off the main thread (Task.detached).
final class VoiceStore: @unchecked Sendable {
    static let shared = VoiceStore()
    static let channelName = "com.levigobin.ClassicVoices.files"
    static let chunk = 4 << 20

    struct Snapshot {
        var paths: Set<String> = []
        var markers: [String: String] = [:]
        var entries: [VoiceCatalog.Entry] = []
        var engines: Set<String> = []
        var hidden: Set<String> = []
        func has(_ rel: String) -> Bool { paths.contains(rel) }
    }

    struct StoreError: LocalizedError {
        let message: String
        init(_ m: String) { message = m }
        var errorDescription: String? { message }
    }

    private enum Backend {
        case channel(AVAudioUnit, AUMessageChannel)
        case group(StoreOps)
    }

    private let lock = NSLock()
    private var backend: Backend?
    private var lastFailure: String?
    /// "extension", "App Group" or "unavailable (reason)": for Diagnostics.
    private(set) var mode = "not connected"
    private(set) var snapshot = Snapshot()
    /// Called on the main actor after `refresh`.
    var onChange: @MainActor @Sendable () -> Void = {}

    // MARK: - connecting

    private func connect() throws -> Backend {
        lock.lock(); defer { lock.unlock() }
        if let backend { return backend }
        assert(!Thread.isMainThread, "VoiceStore blocks; use it off the main thread")
        var desc = AudioComponentDescription(componentType: kAudioUnitType_SpeechSynthesizer, componentSubType: 0x63766F78,
                                             componentManufacturer: 0x436C566F, componentFlags: 0, componentFlagsMask: 0)
        _ = desc
        let sem = DispatchSemaphore(value: 0)
        var unit: AVAudioUnit?
        var failure: String?
        AVAudioUnit.instantiate(with: desc, options: []) { u, e in
            unit = u
            failure = e?.localizedDescription
            sem.signal()
        }
        if sem.wait(timeout: .now() + 20) == .timedOut { failure = "the speech extension didn't answer in 20 seconds" }
        if let unit {
            let ch = unit.auAudioUnit.messageChannel(for: Self.channelName)
            if let reply = ch.callAudioUnit?(["op": "hello"]), reply["store"] != nil {
                log.notice("VoiceStore: extension store at \(reply["store"] as? String ?? "?", privacy: .public)")
                backend = .channel(unit, ch)
                mode = "extension"
                return backend!
            }
            failure = "the speech extension has no file channel (an older copy still running?)"
        }
        lastFailure = failure
        // Fallback: an App Group we can reach.
        if let group = VoiceCatalog.legacyGroupRoot {
            try? FileManager.default.createDirectory(at: group, withIntermediateDirectories: true)
            if FileManager.default.isWritableFile(atPath: group.path) {
                log.error("VoiceStore: no channel (\(failure ?? "?", privacy: .public)); using the App Group")
                backend = .group(StoreOps(root: group))
                mode = "App Group (\(failure ?? "no extension channel"))"
                return backend!
            }
        }
        mode = "unavailable (\(failure ?? "?"))"
        throw StoreError("Classic Voices can\u{2019}t reach its speech extension to store voices: \(failure ?? "unknown error"). Try opening the app again.")
    }

    /// Forget the connection, so the next call reconnects (after an error).
    private func disconnect() {
        lock.lock(); backend = nil; lock.unlock()
    }

    @discardableResult
    private func call(_ message: [AnyHashable: Any]) throws -> [AnyHashable: Any] {
        let reply: [AnyHashable: Any]?
        switch try connect() {
        case .channel(_, let ch): reply = ch.callAudioUnit?(message)
        case .group(let ops): reply = ops.perform(message)
        }
        guard let reply else { disconnect(); throw StoreError("The speech extension stopped answering.") }
        if let e = reply["error"] as? String, !e.isEmpty { throw StoreError(e) }
        return reply
    }

    // MARK: - what's there

    /// Re-reads the store's contents and voice list; the catalog the app shows
    /// comes from here (VoiceCatalog.remoteEnhanced).
    func refresh() throws {
        let tree = try call(["op": "tree"])
        let cat = try call(["op": "catalog"])
        var snap = Snapshot()
        snap.paths = Set(tree["paths"] as? [String] ?? [])
        snap.markers = tree["markers"] as? [String: String] ?? [:]
        if let data = cat["entries"] as? Data { snap.entries = (try? JSONDecoder().decode([VoiceCatalog.Entry].self, from: data)) ?? [] }
        snap.engines = Set(cat["engines"] as? [String] ?? [])
        snap.hidden = Set(cat["hidden"] as? [String] ?? [])
        lock.lock()
        snapshot = snap
        lock.unlock()
        VoiceCatalog.remoteEnhanced = snap.entries
        VoiceCatalog.remoteMacinTalkEngines = snap.engines
        VoiceCatalog.remoteHidden = snap.hidden
        let change = onChange
        Task { @MainActor in change() }
    }

    func read(_ rel: String) -> Data? { (try? call(["op": "read", "path": rel]))?["data"] as? Data }

    // MARK: - changing

    /// Copies a file or folder into the store at `rel`, replacing what was there.
    func install(_ local: URL, at rel: String, progress: ((Double) -> Void)? = nil) throws {
        let fm = FileManager.default
        var files: [(URL, String)] = []  // (file, path relative to rel)
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: local.path, isDirectory: &isDir) else { throw StoreError("\(local.lastPathComponent) is missing.") }
        if isDir.boolValue {
            if let it = fm.enumerator(atPath: local.path) {
                for case let sub as String in it {
                    let f = local.appendingPathComponent(sub)
                    var d: ObjCBool = false
                    if fm.fileExists(atPath: f.path, isDirectory: &d), !d.boolValue { files.append((f, rel + "/" + sub)) }
                }
            }
        } else {
            files = [(local, rel)]
        }
        let total = files.reduce(0) { $0 + ((try? fm.attributesOfItem(atPath: $1.0.path)[.size] as? Int) ?? 0) }
        var sent = 0
        try call(["op": "discard"])
        for (f, path) in files {
            let h = try FileHandle(forReadingFrom: f)
            defer { try? h.close() }
            var off = 0
            var any = false
            while let d = try h.read(upToCount: Self.chunk), !d.isEmpty || !any {
                try call(["op": "put", "path": path, "data": d, "offset": NSNumber(value: off)])
                off += d.count
                sent += d.count
                any = true
                progress?(total > 0 ? Double(sent) / Double(total) : 1)
                if d.isEmpty { break }
            }
        }
        try call(["op": "commit", "path": rel])
    }

    func write(_ data: Data, at rel: String) throws { try call(["op": "write", "path": rel, "data": data]) }

    func delete(_ rel: String) throws { try call(["op": "delete", "path": rel]) }

    /// Speaks a phrase with a voice key inside the extension; nil when it worked.
    func test(_ key: String) -> String? {
        do {
            let r = try call(["op": "test", "key": key])
            let rc = r["rc"] as? Int ?? -1, audio = r["audioMs"] as? Double ?? 0
            return rc == 0 && audio > 0 ? nil : (r["error"] as? String ?? "error \(rc)")
        } catch { return error.localizedDescription }
    }

    func setHidden(_ ids: Set<String>) {
        VoiceCatalog.hiddenVoices = ids  // app side: cache + legacy defaults
        if let data = try? JSONEncoder().encode(ids.sorted()) { try? write(data, at: "hidden.json") }
    }

    // MARK: - installs that must survive the app closing

    /// Work to do on the store: folders (and single files) to put in place
    /// and small files to write. Kept on disk until done, because a download
    /// can finish while the app is in the background, where the extension
    /// may not be reachable.
    struct Job: Codable {
        struct Item: Codable { let local: String; let dest: String }   // local: name inside the job folder
        struct Small: Codable { let dest: String; let data: Data }
        var items: [Item] = []
        var files: [Small] = []
        var deletes: [String] = []
    }

    private static var pendingRoot: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("PendingInstalls")
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    /// Takes the payloads (moved, so they must be ours), records the job and
    /// runs it now if it can. Throws if it couldn't run now; the job stays.
    func enqueue(_ job: Job, payloads: [URL], run: Bool = true) throws {
        let dir = Self.pendingRoot.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        for p in payloads { try FileManager.default.moveItem(at: p, to: dir.appendingPathComponent(p.lastPathComponent)) }
        try JSONEncoder().encode(job).write(to: dir.appendingPathComponent("job.json"))
        if run { try flush() }
    }

    /// Whether the app is in front: hosting the extension from the background
    /// is unreliable, so jobs wait for the next foreground.
    @MainActor static var isForeground: Bool {
        #if os(iOS)
        UIApplication.shared.applicationState == .active
        #else
        true
        #endif
    }

    /// Finishes every recorded job it can; stops at the first that fails.
    func flush(progress: ((Double) -> Void)? = nil) throws {
        let fm = FileManager.default
        let jobs = ((try? fm.contentsOfDirectory(atPath: Self.pendingRoot.path)) ?? []).sorted()
        var ran = false
        for name in jobs {
            let dir = Self.pendingRoot.appendingPathComponent(name)
            guard let data = try? Data(contentsOf: dir.appendingPathComponent("job.json")),
                  let job = try? JSONDecoder().decode(Job.self, from: data) else {
                try? fm.removeItem(at: dir); continue
            }
            for rel in job.deletes { try delete(rel) }
            for (i, item) in job.items.enumerated() {
                try install(dir.appendingPathComponent(item.local), at: item.dest) { p in
                    progress?((Double(i) + p) / Double(job.items.count))
                }
            }
            for f in job.files { try write(f.data, at: f.dest) }
            try? fm.removeItem(at: dir)
            ran = true
        }
        if ran || !jobs.isEmpty { try refresh() }
    }

    var hasPending: Bool { !((try? FileManager.default.contentsOfDirectory(atPath: Self.pendingRoot.path)) ?? []).isEmpty }
}
