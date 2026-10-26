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
        defer { VoiceCatalog.invalidateEnhanced() }  // voices may have been added or removed
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

        // Developer probe: how much address space can this process still map?
        case "vmprobe":
            var lines: [String] = []
            var info = task_vm_info_data_t()
            var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
            let kr = withUnsafeMutablePointer(to: &info) {
                $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
            }
            if kr == KERN_SUCCESS {
                lines.append("virtual \(info.virtual_size >> 20) MB, resident \(info.resident_size >> 20) MB, footprint \(info.phys_footprint >> 20) MB")
            }
            for mb in [2048, 1536, 1280, 1024, 768, 512, 256] {
                let p = mmap(nil, mb << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
                if p == MAP_FAILED { lines.append("mmap \(mb) MB: FAILED errno \(errno)") }
                else { lines.append("mmap \(mb) MB: ok"); munmap(p, mb << 20) }
            }
            // Two at once, as the engine plus a Vocalizer voice would need.
            let a = mmap(nil, 1 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
            let b = mmap(nil, 1 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
            lines.append("two 1 GB maps: \(a != MAP_FAILED && b != MAP_FAILED ? "ok" : "FAILED errno \(errno)")")
            if a != MAP_FAILED { munmap(a, 1 << 30) }
            if b != MAP_FAILED { munmap(b, 1 << 30) }
            // Does an unbacked reservation escape the limit? 2 GB PROT_NONE, then
            // make the parts MacinTalk uses writable: 1 GB low, 64 MB of stacks high.
            let r = mmap(nil, 2 << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0)
            if r == MAP_FAILED { lines.append("reserve 2 GB PROT_NONE: FAILED errno \(errno)") }
            else {
                lines.append("reserve 2 GB PROT_NONE: ok")
                let lo = mprotect(r, 1 << 30, PROT_READ | PROT_WRITE)
                lines.append("mprotect low 1 GB RW: \(lo == 0 ? "ok" : "FAILED errno \(errno)")")
                let hi = mprotect(r! + (2 << 30) - (64 << 20), 64 << 20, PROT_READ | PROT_WRITE)
                lines.append("mprotect top 64 MB RW: \(hi == 0 ? "ok" : "FAILED errno \(errno)")")
                if lo == 0 { (r! + (512 << 20)).storeBytes(of: 1, as: UInt8.self); lines.append("touch low: ok") }
                if hi == 0 { (r! + (2 << 30) - 4096).storeBytes(of: 1, as: UInt8.self); lines.append("touch top: ok") }
                let more = mprotect(r! + (1 << 30), 512 << 20, PROT_READ | PROT_WRITE)
                lines.append("mprotect another 512 MB RW (1.5 GB total): \(more == 0 ? "ok" : "FAILED errno \(errno)")")
                let c = mmap(nil, 1 << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
                lines.append("plus a separate 1 GB RW map: \(c == MAP_FAILED ? "FAILED errno \(errno)" : "ok")")
                if c != MAP_FAILED { munmap(c, 1 << 30) }
                munmap(r, 2 << 30)
            }
            return ["lines": lines]

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
