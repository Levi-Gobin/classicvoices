import AVFoundation
import Foundation

/// `-latency`: time from queuing an utterance to the first audio buffer, through
/// the system speech path (the same one VoiceOver uses), for a voice served from
/// the extension's own store (ios6e) and bundled voices from the app bundle.
enum LatencyTest {
    static func run() {
        DispatchQueue.global().async {
            var all = AVSpeechSynthesisVoice.speechVoices()
            for _ in 0..<60 where !all.contains(where: { $0.identifier.contains("ClassicVoices") }) {
                Thread.sleep(forTimeInterval: 1); all = AVSpeechSynthesisVoice.speechVoices()
            }
            print("LATENCY voices: \(all.filter { $0.identifier.contains("ClassicVoices") }.count) from Classic Voices")
            // `-latency a.b,c.d` picks identifier suffixes; default compares the store's Samantha with bundled ones.
            var wanted = ["ios6e.en-US", "ios6.en-US", "ios3.en-US", "ios6e.en-US", "ios6.en-US", "ios6e.en-US"]
            if let i = CommandLine.arguments.firstIndex(of: "-latency"), i + 1 < CommandLine.arguments.count,
               !CommandLine.arguments[i + 1].hasPrefix("-") {
                wanted = CommandLine.arguments[i + 1].split(separator: ",").map(String.init)
            }
            for suffix in wanted {
                guard let v = all.first(where: { $0.identifier.hasSuffix(suffix) && $0.identifier.contains("ClassicVoices") }) else {
                    print("LATENCY \(suffix): voice not found"); continue
                }
                let synth = AVSpeechSynthesizer()
                let u = AVSpeechUtterance(string: "Settings. Button.")
                u.voice = v
                var frames = 0
                var first: Double = -1
                let done = DispatchSemaphore(value: 0)
                let t0 = Date()
                synth.write(u) { buf in
                    if let b = buf as? AVAudioPCMBuffer, b.frameLength > 0 {
                        if first < 0 { first = Date().timeIntervalSince(t0) * 1000 }
                        frames += Int(b.frameLength)
                    } else { done.signal() }
                }
                _ = done.wait(timeout: .now() + 20)
                print(String(format: "LATENCY %@ (%@): first audio %.0f ms, total %.0f ms, %d frames",
                             suffix, v.name, first, Date().timeIntervalSince(t0) * 1000, frames))
                fflush(stdout)
                Thread.sleep(forTimeInterval: 1)
            }
            exit(0)
        }
    }
}

/// `-speaktime a.b,c.d`: speaks out loud with AVSpeechSynthesizer and times
/// didStart to didFinish, which the offline `write` path can't be trusted for
/// with every engine. A sentence that takes over a second was really spoken.
enum SpeakTimeTest {
    final class Delegate: NSObject, AVSpeechSynthesizerDelegate {
        var started = Date(), finished = Date()
        let done = DispatchSemaphore(value: 0)
        var ranges = 0
        func speechSynthesizer(_ s: AVSpeechSynthesizer, didStart u: AVSpeechUtterance) { started = Date() }
        func speechSynthesizer(_ s: AVSpeechSynthesizer, willSpeakRangeOfSpeechString r: NSRange, utterance u: AVSpeechUtterance) { ranges += 1 }
        func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) { finished = Date(); done.signal() }
        func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel u: AVSpeechUtterance) { finished = Date(); done.signal() }
    }

    static func run(_ suffixes: [String]) {
        DispatchQueue.global().async {
            var all = AVSpeechSynthesisVoice.speechVoices()
            for _ in 0..<60 where !all.contains(where: { $0.identifier.contains("ClassicVoices") }) {
                Thread.sleep(forTimeInterval: 1); all = AVSpeechSynthesisVoice.speechVoices()
            }
            let synth = AVSpeechSynthesizer()
            let d = Delegate()
            synth.delegate = d
            for suffix in suffixes {
                guard let v = all.first(where: { $0.identifier.hasSuffix(suffix) && $0.identifier.contains("ClassicVoices") }) else {
                    print("SPEAKTIME \(suffix): voice not found"); continue
                }
                let u = AVSpeechUtterance(string: "Hello there. This is a sentence long enough to measure.")
                u.voice = v
                d.ranges = 0
                let t0 = Date()
                synth.speak(u)
                let r = d.done.wait(timeout: .now() + 25)
                print(String(format: "SPEAKTIME %@ (%@): %@ queued->finish %.0f ms, start->finish %.0f ms, %d word ranges",
                             suffix, v.name, r == .timedOut ? "TIMED OUT" : "ok",
                             d.finished.timeIntervalSince(t0) * 1000, d.finished.timeIntervalSince(d.started) * 1000, d.ranges))
                fflush(stdout)
            }
            exit(0)
        }
    }
}
