import Foundation
import os

/// How a request's speech went, for the Diagnostics report (Levi, 2026-11-27: iOS 7 Samantha choppy when a
/// notification came, music started, after a cold start): kept by the speech extension's render block (no locks,
/// no files) and written out by SpeechLog when the next request comes.
struct SpeechRenderStats {
    var firstAudio: UInt64 = 0   // when the first samples went out (mach time)
    var longestWait: UInt64 = 0  // the longest a render waited for samples after speech began (mach ticks)
    var waits = 0                // renders after speech began that waited over 20 ms
    var padded = 0               // frames of silence handed out after speech began, before the end
    var frames = 0               // frames of speech handed out
    var first = false            // the extension's first request since it started (a cold start)
    var memoryLeft = -1          // MB the system would still let the extension use, at the request (iOS)
}

/// The last utterances' timings, a line each, in the App Group (Logs/speech.txt), for the app's Diagnostics
/// report: time to first audio, the longest wait for samples once speech had begun, how many renders waited over
/// 20 ms, and the silence handed out mid-speech (heard as a break).
enum SpeechLog {
    static let keep = 40
    private nonisolated(unsafe) static var started = false

    /// For the request starting now: whether it's the extension's first, and the memory it has left.
    static func begin(_ stats: inout SpeechRenderStats) {
        stats.first = !started
        started = true
        #if os(iOS) || os(visionOS)
        stats.memoryLeft = Int(os_proc_available_memory() / 1_048_576)
        #endif
    }
    private static let queue = DispatchQueue(label: "speechlog", qos: .utility)

    static var url: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroup)?
            .appendingPathComponent("Logs/speech.txt")
    }

    static func add(key: String, start: UInt64, stats: SpeechRenderStats) {
        guard start > 0, stats.frames > 0 || stats.firstAudio == 0 else { return }
        var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
        let ms = { (t: UInt64) in Double(t) * Double(tb.numer) / Double(tb.denom) / 1e6 }
        let first = stats.firstAudio > start ? String(format: "%.0f ms", ms(stats.firstAudio - start)) : "none"
        let when = ISO8601DateFormatter.string(from: Date(), timeZone: .current, formatOptions: [.withTime, .withColonSeparatorInTime])
        var line = String(format: "%@ %@: first audio %@, %.1f s of speech, longest wait %.0f ms, %d waits over 20 ms, %.0f ms of silence filled in",
                          when, key, first, Double(stats.frames) / 22050, ms(stats.longestWait), stats.waits,
                          Double(stats.padded) * 1000 / 22050)
        if stats.memoryLeft >= 0 { line += ", \(stats.memoryLeft) MB of memory left" }
        if stats.first { line += " (first since the extension started)" }
        queue.async {
            guard let url else { return }
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            var lines = (try? String(contentsOf: url, encoding: .utf8))?.split(separator: "\n").map(String.init) ?? []
            lines.append(line)
            if lines.count > keep { lines.removeFirst(lines.count - keep) }
            try? (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
        }
    }

    static func recent() -> [String] {
        guard let url, let t = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return t.split(separator: "\n").map(String.init)
    }
}
