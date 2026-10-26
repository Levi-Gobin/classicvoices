import AVFoundation
import SwiftUI

@main
struct ClassicVoicesApp: App {
    #if canImport(UIKit)
    @UIApplicationDelegateAdaptor private var appDelegate: AppDelegate
    #endif

    init() {
        // `-import <folder>` adds Mac OS X speech files as Add from Files does, then quits.
        if let i = CommandLine.arguments.firstIndex(of: "-import"), i + 1 < CommandLine.arguments.count {
            // An absolute path, or one inside the app's Documents folder.
            let arg = CommandLine.arguments[i + 1]
            let url = arg.hasPrefix("/") ? URL(fileURLWithPath: arg)
                : FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent(arg)
            Task.detached {
                let found = SpeechDataImport.scan([url])
                print("IMPORT found engine=\(found.engine?.path ?? "none") dictionary=\(found.dictionary != nil) libraries=\(found.libraries.map(\.lastPathComponent)) voices=\(found.voices.count) other=\(found.otherVoices)")
                do {
                    let o = try SpeechDataImport.install(found, from: [url])
                    print("IMPORT \(o.gen ?? "?"): added \(o.added); missing \(o.missing); test \(o.test ?? "-"); needsRelease \(o.needsRelease)")
                } catch {
                    print("IMPORT error: \(error.localizedDescription)")
                }
                fflush(stdout)
                exit(0)
            }
        }
        _ = VoiceLibrary.shared
        Task.detached {
            let imported = MacinTalkImport.run()
            if !imported.isEmpty { print("IMPORTED \(imported)"); fflush(stdout) }
        }
        if CommandLine.arguments.contains("-benchmark") { Benchmark.run() }
        if let i = CommandLine.arguments.firstIndex(of: "-latbench"), i + 1 < CommandLine.arguments.count {
            // `-latbench mt105/Alex,ios6/en-US`: first-sound times through the stream (vxs_latency_bench)
            let keys = CommandLine.arguments[i + 1].split(separator: ",").map(String.init)
            Thread.detachNewThread {
                guard let root = VoiceCatalog.root else { print("LAT no resources"); exit(1) }
                var cs = keys.map { strdup($0) }
                cs.withUnsafeMutableBufferPointer { b in
                    b.baseAddress!.withMemoryRebound(to: UnsafePointer<CChar>?.self, capacity: b.count) {
                        vxs_latency_bench(root.path, VoiceCatalog.enhancedRoot?.path, $0, Int32(keys.count))
                    }
                }
                cs.forEach { free($0) }
                print("LAT done"); fflush(stdout); exit(0)
            }
        }
        if CommandLine.arguments.contains("-diagnose") { Diagnose.run() }
        if CommandLine.arguments.contains("-latency") { LatencyTest.run() }
        if let i = CommandLine.arguments.firstIndex(of: "-rules"), i + 2 < CommandLine.arguments.count {
            // `-rules ios6/en-US "iOS on OS X"`: the text after Apple's rules.
            let key = CommandLine.arguments[i + 1], text = CommandLine.arguments[i + 2]
            let r = PronunciationRules.rules(forVoiceKey: key)
            print("RULES \(key): \(r?.rules.count ?? 0) rules; [\(text)] -> [\(r?.apply(text) ?? text)]")
            fflush(stdout); exit(0)
        }
        if let i = CommandLine.arguments.firstIndex(of: "-speaktime"), i + 1 < CommandLine.arguments.count {
            SpeakTimeTest.run(CommandLine.arguments[i + 1].split(separator: ",").map(String.init))
        }
        if CommandLine.arguments.contains("-vmprobe") {
            Task.detached {
                for l in VoiceStore.shared.vmProbe() { print("VMPROBE extension: \(l)") }
                // The same in the app, for comparison.
                for l in (StoreOps(root: FileManager.default.temporaryDirectory).perform(["op": "vmprobe"])["lines"] as? [String] ?? []) { print("VMPROBE app: \(l)") }
                fflush(stdout); exit(0)
            }
        }
        if let i = CommandLine.arguments.firstIndex(of: "-storetest"), i + 1 < CommandLine.arguments.count {
            let keys = CommandLine.arguments[i + 1].split(separator: ",").map(String.init)
            Task.detached {
                for k in keys { print("STORETEST \(k): \(VoiceStore.shared.test(k) ?? "speaks")") }
                fflush(stdout); exit(0)
            }
        }
        if let i = CommandLine.arguments.firstIndex(of: "-download"), i + 1 < CommandLine.arguments.count {
            DownloadTest.run(CommandLine.arguments[i + 1].split(separator: ",").map(String.init))
        }
        if CommandLine.arguments.contains("-report") {
            Task.detached {  // after the store has answered, so the report shows it
                try? VoiceStore.shared.refresh()
                print(Diagnostics.report(downloadErrors: [:])); fflush(stdout)
            }
        }
        if let i = CommandLine.arguments.firstIndex(of: "-stress"), i + 1 < CommandLine.arguments.count {
            let a = CommandLine.arguments[i + 1].split(separator: ",").map(String.init)
            StressTest.shared.run(key: a[0], count: a.count > 1 ? Int(a[1]) ?? 300 : 300)
        }
        if let i = CommandLine.arguments.firstIndex(of: "-voicetest"), i + 1 < CommandLine.arguments.count {
            VoiceTest.run(keys: CommandLine.arguments[i + 1].split(separator: ",").map(String.init))
        }
        // Tell the system our voices exist so they show up in Settings.
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
    }

    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup { RootView() }
        #if os(macOS)
            .defaultSize(width: 760, height: 720)
        #endif
            .onChange(of: scenePhase) { _, phase in
                // Downloads that finished in the background are handed to the
                // speech extension once the app is in front again.
                if phase == .active, VoiceStore.shared.hasPending {
                    Task.detached { try? VoiceStore.shared.flush() }
                }
            }
    }
}

#if canImport(UIKit)
final class AppDelegate: NSObject, UIApplicationDelegate {
    /// Downloads finished while the app wasn't running.
    func application(_ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
                     completionHandler: @escaping () -> Void) {
        guard identifier == Downloader.sessionIdentifier else { return completionHandler() }
        Downloader.shared.backgroundCompletion = completionHandler
        _ = VoiceLibrary.shared
    }
}
#endif

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

/// Speaks short phrases through the system with one voice as VoiceOver does,
/// cutting most of them off, at volume 0, and reports any that never start or
/// finish: `-stress mt106/Alex,500`.
final class StressTest: NSObject, AVSpeechSynthesizerDelegate {
    static let shared = StressTest()
    private let synth = AVSpeechSynthesizer()
    private var voice: AVSpeechSynthesisVoice?
    private var count = 0, started = 0, finished = 0, stalls = 0
    private var current: AVSpeechUtterance?
    private var didStart = false, didEnd = false
    private let phrases = ["Settings", "Button", "Double tap to open.", "Messages, 3 new items", "Wi-Fi, on, button",
                           "Battery 85 percent", "12:42 PM", "Mail. Inbox, 1,204 unread. Heading.", "Back",
                           "Search field. Double tap to edit.", "Selected. Classic Voices, 2 of 14.", "Control Center. 4 items.",
                           "Hello, this is a longer sentence that keeps the voice talking for a little while longer than the rest."]

    func run(key: String, count: Int) {
        self.count = count
        synth.delegate = self
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [self] in
            guard let e = VoiceCatalog.entries.first(where: { $0.key == key }), let v = VoiceCatalog.systemVoice(for: e) else {
                print("STRESS \(key): no system voice"); fflush(stdout); return
            }
            voice = v
            step(0)
        }
    }

    private func step(_ i: Int) {
        guard i < count else {
            print("STRESS done: \(count) phrases, \(started) started, \(finished) finished, \(stalls) never started"); fflush(stdout)
            return
        }
        let u = AVSpeechUtterance(string: phrases.randomElement()!)
        u.voice = voice
        u.volume = 0
        u.rate = Float.random(in: 0.4...0.65)
        current = u
        didStart = false
        didEnd = false
        synth.speak(u)
        let cut = Int.random(in: 0..<3) > 0
        let t0 = Date()
        func check() {
            let waited = Date().timeIntervalSince(t0)
            if cut && didStart && waited > Double.random(in: 0.05...0.5) {
                synth.stopSpeaking(at: .immediate)
            } else if !(didEnd || (cut && didStart)) && waited < 10 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { check() }
                return
            }
            if !didStart {
                stalls += 1
                print("STRESS \(i): never started after \(Int(waited)) s"); fflush(stdout)
                synth.stopSpeaking(at: .immediate)
            }
            if i % 50 == 0 { print("STRESS \(i): \(started) started, \(finished) finished, \(stalls) never started"); fflush(stdout) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.03) { self.step(i + 1) }
        }
        check()
    }

    func speechSynthesizer(_ s: AVSpeechSynthesizer, didStart u: AVSpeechUtterance) {
        if u === current { didStart = true }
        started += 1
    }
    func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) {
        if u === current { didEnd = true }
        finished += 1
    }
    func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel u: AVSpeechUtterance) {
        if u === current { didEnd = true }
    }
}

/// Starts several downloads at once, as tapping Download on each would, and
/// prints their progress: `-download ios6/en-GB,mac107/041-85113`.
enum DownloadTest {
    @MainActor static func run(_ ids: [String]) {
        let lib = VoiceLibrary.shared
        let had = lib.enhanced.installed.union(lib.mac.installed)
        for id in ids {
            if let a = EnhancedDownloads.catalog.first(where: { $0.id == id }) { lib.enhanced.download(a) }
            else if let v = MacDownloads.catalog.first(where: { $0.id == id }) { lib.mac.download(v) }
            else { print("DOWNLOAD \(id): not in the catalogs") }
        }
        Task { @MainActor in
            let start = Date()
            while true {
                try? await Task.sleep(nanoseconds: 5_000_000_000)
                let p = lib.enhanced.progress.merging(lib.mac.progress) { a, _ in a }
                let line = p.sorted { $0.key < $1.key }.map { "\($0.key) \(Int($0.value * 100))%" }.joined(separator: ", ")
                print(String(format: "DOWNLOAD %.0fs: ", Date().timeIntervalSince(start)) + (line.isEmpty ? "all finished" : line))
                fflush(stdout)
                if p.isEmpty { break }
            }
            let errors = lib.enhanced.errors.merging(lib.mac.errors) { a, _ in a }
            print("DOWNLOAD errors: \(errors.isEmpty ? "none" : errors.description)")
            print("DOWNLOAD installed: \(ids.filter { lib.enhanced.installed.contains($0) || lib.mac.installed.contains($0) })")
            // `-download-cleanup` deletes what the test added.
            if CommandLine.arguments.contains("-download-cleanup") {
                for id in ids where !had.contains(id) {
                    if let a = EnhancedDownloads.catalog.first(where: { $0.id == id }) { lib.enhanced.remove(a) }
                    if let v = MacDownloads.catalog.first(where: { $0.id == id }) { lib.mac.remove(v) }
                }
                print("DOWNLOAD cleaned up")
            }
            print("DOWNLOAD done"); fflush(stdout)
        }
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
                ("mt105/Alex", "Settings. Button.", 100),
                ("mt105/Alex", "Back.", 100),
                ("mt105/Bells", "Hello there.", 100),
                ("mt106/Fred", long, 100),
                ("mt104/Fred", long, 100),
                ("mt104/Vicki", long, 100),
                ("mt106/Alex", long, 100),
                ("mt107/Fred", long, 100),
                ("mt107/Victoria", long, 100),
                ("mt107/Alex", long, 100),
                ("mt107/Alex", "Settings. Button.", 100),
                ("ios6/en-US", "Back.", 100),
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
                    let d = VoiceLibrary.shared.enhanced
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

    func speak(_ text: String, voiceKey: String, rate: Int, rules: PronunciationRules? = nil, settings: SpeechSettings = SpeechSettings()) {
        #if !os(macOS)
        try? AVAudioSession.sharedInstance().setCategory(.playback)
        try? AVAudioSession.sharedInstance().setActive(true)
        #endif
        if !engine.isRunning { try? engine.start() }
        var job = SpeechJob()
        job.text = rules?.apply(text) ?? text
        job.rate = rate
        VoiceEngine.shared.speak(job, voiceKey: voiceKey, resetCommands: settings.resetCommands,
                                 twentyFourHour: settings.twentyFourHour, modernRate: settings.modernRate)
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
