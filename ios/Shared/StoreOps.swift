import Foundation

/// Operations on a voice store, keyed by a message dictionary's "op". The
/// extension runs them on its own store for the app (FileChannel); the app
/// runs them itself on the App Group's folder when it can't reach the
/// extension but does have a group (VoiceStore's fallback).
///
/// Paths are relative to the root and may not climb out of it.
final class StoreOps {
    let root: URL
    init(root: URL) { self.root = root }

    private var incoming: URL { root.appendingPathComponent(".incoming") }

    private func url(_ message: [AnyHashable: Any], _ key: String = "path", under base: URL? = nil) -> URL? {
        guard let rel = message[key] as? String, !rel.hasPrefix("/"),
              !rel.split(separator: "/").contains("..") else { return nil }
        return (base ?? root).appendingPathComponent(rel)
    }

    private func fail(_ s: String) -> [AnyHashable: Any] { ["error": s] }

    func perform(_ message: [AnyHashable: Any]) -> [AnyHashable: Any] {
        let fm = FileManager.default
        switch message["op"] as? String {

        case "hello":
            return ["store": root.path]

        // A chunk of a file being transferred: goes to .incoming/<path> until "commit".
        case "put":
            guard let u = url(message, under: incoming), let data = message["data"] as? Data else { return fail("bad put") }
            let offset = (message["offset"] as? NSNumber)?.uint64Value ?? 0
            do {
                try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
                if offset == 0 {
                    try data.write(to: u)
                } else {
                    let h = try FileHandle(forWritingTo: u)
                    try h.seek(toOffset: offset)
                    try h.write(contentsOf: data)
                    try h.close()
                }
                return ["ok": true]
            } catch { return fail(error.localizedDescription) }

        // Moves .incoming/<path> into place, replacing what was there (a
        // link's target in the App Group included).
        case "commit":
            guard let u = url(message), let src = url(message, under: incoming) else { return fail("bad commit") }
            do {
                try removeItem(u)
                try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
                try fm.moveItem(at: src, to: u)
                return ["ok": true]
            } catch { return fail(error.localizedDescription) }

        case "discard":
            try? fm.removeItem(at: incoming)
            return ["ok": true]

        case "delete":
            guard let u = url(message) else { return fail("bad delete") }
            do { try removeItem(u); return ["ok": true] } catch { return fail(error.localizedDescription) }

        // Small whole files, both ways (markers, hidden.json).
        case "write":
            guard let u = url(message), let data = message["data"] as? Data else { return fail("bad write") }
            do {
                try fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
                try removeItem(u)
                try data.write(to: u)
                return ["ok": true]
            } catch { return fail(error.localizedDescription) }
        case "read":
            guard let u = url(message) else { return fail("bad read") }
            guard let data = try? Data(contentsOf: u) else { return ["missing": true] }
            return ["data": data]

        // Everything in the store, four levels deep, as relative paths, plus
        // the contents of the small ".installed" marker files.
        case "tree":
            var paths: [String] = []
            var markers: [String: String] = [:]
            func walk(_ dir: URL, _ rel: String, _ depth: Int) {
                guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return }
                for n in names.sorted() where n != ".incoming" && !n.hasPrefix(".staging") {
                    let p = rel.isEmpty ? n : rel + "/" + n
                    paths.append(p)
                    let u = dir.appendingPathComponent(n)
                    if rel.hasSuffix(".installed"), let s = try? String(contentsOf: u, encoding: .utf8) { markers[p] = s }
                    var isDir: ObjCBool = false
                    if depth < 4, fm.fileExists(atPath: u.path, isDirectory: &isDir), isDir.boolValue, !n.hasSuffix(".SpeechVoice") {
                        walk(u, p, depth + 1)
                    }
                }
            }
            walk(root, "", 0)
            return ["paths": paths, "markers": markers]

        // The voices the extension offers the system from this root.
        case "catalog":
            let engines = VoiceCatalog.macinTalkEras.map(\.0).filter {
                VoiceCatalog.hasBundledMacinTalk($0) || fm.fileExists(atPath: root.appendingPathComponent("\($0)/MacinTalk").path)
            }
            guard let data = try? JSONEncoder().encode(VoiceCatalog.scanEnhanced(root)) else { return fail("encode") }
            return ["entries": data, "engines": engines, "hidden": Array(VoiceCatalog.hiddenVoices)]

        // Speak a short phrase with a voice, as a check after adding files.
        // Through this process's engine stream, the way speech requests go: a
        // second MacinTalk engine wouldn't fit in the extension's address space.
        case "test":
            guard let key = message["key"] as? String, let stream = VoiceEngine.shared.stream else { return fail("no engine") }
            let t0 = Date()
            vxs_start(stream, key, message["text"] as? String ?? "Testing.", 100, 100, -1, 0)
            var buf = [Float](repeating: 0, count: 4096)
            var frames = 0
            var done: Int32 = 0
            while done == 0 && Date().timeIntervalSince(t0) < 20 {
                let k = buf.withUnsafeMutableBufferPointer { vxs_read(stream, $0.baseAddress!, 4096, &done) }
                frames += Int(k)
                if k == 0 && done == 0 { usleep(2000) }
            }
            return ["rc": frames > 0 ? 0 : -1, "audioMs": Double(frames) * 1000 / 22050,
                    "totalMs": Date().timeIntervalSince(t0) * 1000,
                    "error": frames > 0 ? "" : String(cString: vxs_last_error(stream))]

        default:
            return fail("unknown op")
        }
    }

    /// Removes an item; a link into the App Group takes its target with it.
    private func removeItem(_ u: URL) throws {
        let fm = FileManager.default
        if let dest = try? fm.destinationOfSymbolicLink(atPath: u.path) {
            try? fm.removeItem(atPath: dest)
            try fm.removeItem(at: u)
        } else if fm.fileExists(atPath: u.path) {
            try fm.removeItem(at: u)
        }
    }
}
