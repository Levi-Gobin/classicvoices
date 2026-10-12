import AVFoundation
import SwiftUI

@main
struct ClassicVoicesApp: App {
    init() {
        let imported = MacinTalkImport.run()
        if !imported.isEmpty { print("IMPORTED \(imported)") }
        if CommandLine.arguments.contains("-benchmark") { Benchmark.run() }
        if CommandLine.arguments.contains("-diagnose") { Diagnose.run() }
        if CommandLine.arguments.contains("-report") { print(Diagnostics.report(downloadErrors: [:])); fflush(stdout) }
        if let i = CommandLine.arguments.firstIndex(of: "-voicetest"), i + 1 < CommandLine.arguments.count {
            VoiceTest.run(keys: CommandLine.arguments[i + 1].split(separator: ",").map(String.init))
        }
        // Tell the system our voices exist so they show up in Settings.
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
    }

    var body: some Scene {
        WindowGroup { RootView() }
        #if os(macOS)
            .defaultSize(width: 760, height: 720)
        #endif
    }
}

/// What the system sees of our extension; results go to stdout.
enum Diagnose {
    static let synth = AVSpeechSynthesizer()

    /// Speak through the engine stream and list the silent gaps in the output.
    static func gaps(_ key: String, _ text: String, pause: Int32 = 500) -> String {
        guard let stream = VoiceEngine.shared.stream else { return "no stream" }
        var buf = [Float](repeating: 0, count: 22050 * 10)
        vxs_start(stream, key, text, 190, 100, -1, pause)
        var n = 0; var done: Int32 = 0
        while done == 0 && n < buf.count - 512 {
            let k = buf.withUnsafeMutableBufferPointer { vxs_read(stream, $0.baseAddress! + n, 512, &done) }
            n += k
            if k == 0 && done == 0 { usleep(300) }
        }
        guard let first = buf[..<n].firstIndex(where: { abs($0) > 0.01 }) else { return "silent" }
        var out: [String] = []; var run = 0; var w = first
        while w + 220 < n {
            let e = buf[w..<w + 220].reduce(0) { $0 + $1 * $1 } / 220
            if e < 1e-5 { run += 1 } else { if run >= 5 { out.append("\(run * 10)ms") }; run = 0 }
            w += 220
        }
        return "\(n * 1000 / 22050)ms total, gaps \(out) tail \(run * 10)ms"
    }

    static func run() {
        let eos = "\u{1b}\\eos=1\\"
        for key in ["ios6/en-US", "ios3/en-US"] {
            print("DIAG gaps \(key): Settings|Active ->", gaps(key, "Settings \(eos) Active"))
            print("DIAG gaps \(key): Classic Voices|Button ->", gaps(key, "Classic Voices \(eos) Button"))
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) {
            let ausp = AudioComponentDescription(componentType: kAudioUnitType_SpeechSynthesizer, componentSubType: 0,
                                                 componentManufacturer: 0, componentFlags: 0, componentFlagsMask: 0)
            var probe = ausp
            print("DIAG AudioComponentCount(ausp) = \(AudioComponentCount(&probe))")
            var it: AudioComponent? = nil
            while let next = AudioComponentFindNext(it, &probe) {
                var d = AudioComponentDescription()
                AudioComponentGetDescription(next, &d)
                var name: Unmanaged<CFString>?
                AudioComponentCopyName(next, &name)
                print("DIAG   found \(name?.takeRetainedValue() as String? ?? "?") type=\(String(format: "%08x", d.componentType)) sub=\(String(format: "%08x", d.componentSubType)) flags=\(d.componentFlags)")
                it = next
            }
            let comps = AVAudioUnitComponentManager.shared().components(matching: ausp)
            print("DIAG speech synth components: \(comps.count)")
            for c in comps { print("DIAG   \(c.manufacturerName) / \(c.name) type=\(c.typeName) sandboxSafe=\(c.isSandboxSafe)") }
            let all = AVAudioUnitComponentManager.shared().components(passingTest: { c, _ in c.name.contains("Classic") })
            print("DIAG components named Classic*: \(all.map { "\($0.name) \($0.typeName)" })")
            let ours = AVSpeechSynthesisVoice.speechVoices().filter { $0.identifier.contains("com.levigobin.ClassicVoices.ios") }
            print("DIAG system voices from us: \(ours.count) \(ours.prefix(3).map { $0.name })")
            for v in AVSpeechSynthesisVoice.speechVoices() where v.name.contains("iOS 6") {
                print("DIAG   voice \(v.name) id=\(v.identifier) lang=\(v.language)")
            }
            print("DIAG total system voices: \(AVSpeechSynthesisVoice.speechVoices().count)")
            var desc = ausp
            desc.componentSubType = 0x63766F78  // 'cvox'
            desc.componentManufacturer = 0x436C566F  // 'ClVo'
            AVAudioUnit.instantiate(with: desc, options: .loadOutOfProcess) { unit, error in
                print("DIAG instantiate out-of-process: unit=\(unit != nil) error=\(String(describing: error))")
                if let au = unit?.auAudioUnit as? AVSpeechSynthesisProviderAudioUnit {
                    print("DIAG   provider voices: \(au.speechVoices.count)")
                } else if let au = unit?.auAudioUnit {
                    print("DIAG   auAudioUnit class: \(type(of: au))")
                }
                if let e = VoiceCatalog.entries.first(where: { $0.key == "ios3/en-US" }),
                   let voice = VoiceCatalog.systemVoice(for: e) {
                    let u = AVSpeechUtterance(string: "VoiceOver on. Settings. Button.")
                    u.voice = voice
                    Diagnose.synth.speak(u)
                    print("DIAG speaking through system with \(voice.name)")
                }
                print("DIAG done")
                fflush(stdout)
            }
        }
    }
}

/// Speaks through the system (i.e. the extension, as VoiceOver does) with each
/// voice key in turn; the extension's log says what happened.
enum VoiceTest {
    static let synth = AVSpeechSynthesizer()
    static func run(keys: [String]) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { next(keys, 0) }
    }
    static func next(_ keys: [String], _ i: Int) {
        guard i < keys.count else { print("VOICETEST done"); fflush(stdout); return }
        let key = keys[i]
        guard let e = VoiceCatalog.entries.first(where: { $0.key == key }), let v = VoiceCatalog.systemVoice(for: e) else {
            print("VOICETEST \(key): no system voice"); next(keys, i + 1); return
        }
        let u = AVSpeechUtterance(string: "Testing \(e.name). Settings, button.")
        u.voice = v
        synth.speak(u)
        print("VOICETEST \(key): speaking"); fflush(stdout)
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) { next(keys, i + 1) }
    }
}

/// Headless timing run, started with `-benchmark`; results go to stdout.
enum Benchmark {
    static func run() {
        Thread.detachNewThread {
            guard let root = VoiceCatalog.root else { print("BENCH no resources"); return }
            let long = "VoiceOver on. Settings. Button. Double tap to open. Messages, 3 new items. It is 4:15 PM on Saturday, October 10th, 2026."
            let cases: [(String, String, Int)] = [
                ("ios6/en-US", "Settings. Button.", 100),
                ("ios6/en-US", long, 100),
                ("ios6/en-US", long, 300),
                ("ios3/en-US", "Settings. Button.", 100),
                ("ios3/en-US", long, 100),
                ("ios3/en-US", long, 300),
                ("ios3/en-GB", "Hello, I'm Serena. Battery 85 percent.", 100),
                ("ios4/en-US", long, 100),
                ("ios5/en-US", long, 100),
                ("ios6e/en-US", "Settings. Button.", 100),
                ("ios6e/en-US", long, 100),
                ("ios6e/en-US", long, 300),
                ("mac107/SamanthaCompact", long, 100),
                ("mac108/SamanthaCompact", long, 100),
                ("mac109/SamanthaCompact", long, 100),
                ("mac109/SamanthaCompact", long, 300),
                ("mt105/Fred", "Settings. Button.", 100),
                ("mt105/Fred", long, 100),
                ("mt105/Vicki", long, 100),
                ("mt105/Alex", long, 100),
                ("mt105/Bells", "Hello there.", 100),
                ("mt106/Fred", long, 100),
                ("mt104/Fred", long, 100),
                ("mt104/Vicki", long, 100),
                ("mt106/Alex", long, 100),
                ("mt107/Fred", long, 100),
                ("mt107/Victoria", long, 100),
                ("mt107/Alex", long, 100),
            ].filter { c in
                // `-bench-only mt107` limits the run to keys with that prefix
                guard let i = CommandLine.arguments.firstIndex(of: "-bench-only"), i + 1 < CommandLine.arguments.count else { return true }
                return c.0.hasPrefix(CommandLine.arguments[i + 1])
            }
            // Fetch one enhanced voice exactly as the app's Download button does.
            if CommandLine.arguments.contains("-with-enhanced"),
               let a = EnhancedDownloads.catalog.first(where: { $0.build == "ios6" && $0.language == "en-US" }) {
                let sem = DispatchSemaphore(value: 0)
                Task { @MainActor in
                    let d = EnhancedDownloads()
                    if !d.isInstalled(a) {
                        print("BENCH downloading enhanced \(a.id) …"); fflush(stdout)
                        d.download(a)
                        while d.progress[a.id] != nil { try? await Task.sleep(nanoseconds: 200_000_000) }
                        print("BENCH download result: installed=\(d.isInstalled(a)) error=\(d.errors[a.id] ?? "none")")
                    }
                    sem.signal()
                }
                sem.wait()
            }
            let assets = VoiceCatalog.enhancedRoot?.path
            for (lang, text, rate) in cases {
                var open = 0.0, first = 0.0, total = 0.0, audio = 0.0
                let rc = vxs_benchmark(root.path, assets, lang, text, Int32(rate), &open, &first, &total, &audio)
                print(String(format: "BENCH %@ rate=%d rc=%d open=%.0fms first=%.0fms total=%.0fms audio=%.0fms speed=%.1fx",
                             lang, rate, rc, open, first, total, audio, audio / max(total, 1)))
            }
            print("BENCH done")
            fflush(stdout)
        }
    }
}

/// Plays the engine directly (bypassing the system) to measure on-device speed.
final class DirectPlayer: ObservableObject {
    static let shared = DirectPlayer()
    @Published var stats = ""
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?

    init() {
        let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 22050, channels: 1, interleaved: false)!
        let stream = VoiceEngine.shared.stream
        let node = AVAudioSourceNode(format: format) { _, _, frameCount, data in
            let buffers = UnsafeMutableAudioBufferListPointer(data)
            let out = buffers[0].mData!.assumingMemoryBound(to: Float.self)
            var done: Int32 = 0
            let n = stream.map { vxs_read($0, out, Int(frameCount), &done) } ?? 0
            if n < Int(frameCount) { (out + n).update(repeating: 0, count: Int(frameCount) - n) }
            return noErr
        }
        source = node
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: format)
    }

    func speak(_ text: String, voiceKey: String, rate: Int) {
        #if os(iOS)
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        try? AVAudioSession.sharedInstance().setActive(true)
        #endif
        if !engine.isRunning { try? engine.start() }
        var job = SpeechJob()
        job.text = text
        job.rate = rate
        VoiceEngine.shared.speak(job, voiceKey: voiceKey)
        stats = "Speaking…"
        DispatchQueue.global().async { [weak self] in
            guard let stream = VoiceEngine.shared.stream else { return }
            // wait for synthesis to finish, then report
            var waited = 0
            while waited < 600 {
                Thread.sleep(forTimeInterval: 0.05)
                waited += 1
                if vxs_last_audio_ms(stream) > 0 && vxs_last_synth_ms(stream) > 0 && vxs_last_first_audio_ms(stream) >= 0 { break }
            }
            let first = vxs_last_first_audio_ms(stream)
            let synth = vxs_last_synth_ms(stream)
            let audio = vxs_last_audio_ms(stream)
            let err = String(cString: vxs_last_error(stream))
            let text = String(format: "First audio %.0f ms · synthesized %.1f s of audio in %.0f ms (%.1f× real time)",
                              first, audio / 1000, synth, audio / max(synth, 1)) + (err.isEmpty ? "" : "\n\(err)")
            DispatchQueue.main.async { self?.stats = text }
        }
    }
}
