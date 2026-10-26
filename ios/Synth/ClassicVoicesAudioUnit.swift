import AVFoundation

/// System speech-synthesis provider backed by the original iOS 6 Vocalizer engine.
public final class ClassicVoicesAudioUnit: AVSpeechSynthesisProviderAudioUnit {
    private let format = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 22050,
                                       channels: 1, interleaved: false)!
    private var outputBus: AUAudioUnitBus!
    private var busses: AUAudioUnitBusArray!

    public override init(componentDescription: AudioComponentDescription,
                         options: AudioComponentInstantiationOptions = []) throws {
        try super.init(componentDescription: componentDescription, options: options)
        outputBus = try AUAudioUnitBus(format: format)
        busses = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
        VoiceCatalog.adoptLegacy()
        DispatchQueue.global(qos: .utility).async { VoiceCatalog.unprotectVoices() }
        // Warm up the most recently used voice so the first utterance is fast.
        VoiceEngine.shared.preload(UserDefaults.standard.string(forKey: "lastVoiceKey") ?? "ios6/en-US")
        log.notice("CLASSICVOICES audio unit created; root=\(VoiceCatalog.root?.path ?? "nil", privacy: .public) voices=\(VoiceCatalog.entries.count)")
    }

    public override var outputBusses: AUAudioUnitBusArray { busses }

    private let fileChannel = FileChannel()
    public override func messageChannel(for channelName: String) -> any AUMessageChannel {
        fileChannel
    }

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            VoiceCatalog.invalidateEnhanced()  // the app asked the system to update the list
            let v = VoiceCatalog.providerVoices
            log.notice("CLASSICVOICES speechVoices requested -> \(v.count) voices")
            return v
        }
        set {}
    }

    /// Diagnostics: time of the current request and what the first renders looked like.
    private let requestStart = UnsafeMutablePointer<UInt64>.allocate(capacity: 1)
    private let renderLog = UnsafeMutablePointer<(UInt64, Int32, Int32)>.allocate(capacity: 8)  // (time, frames asked, frames given)
    private let renderCount = UnsafeMutablePointer<Int32>.allocate(capacity: 1)

    /// Logs how the previous request's first renders went (given its start
    /// time and render count, taken before the new request reset them).
    private func dumpRenderLog(start: UInt64, count n: Int, renders: [(UInt64, Int32, Int32)]) {
        guard n > 0, start > 0 else { return }
        var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
        // A render can be logged between the count reset and the new start time, so never subtract past zero.
        let ms = { (t: UInt64) in Double(t > start ? t - start : 0) * Double(tb.numer) / Double(tb.denom) / 1e6 }
        let parts = renders.map { t, asked, given in String(format: "+%.0fms ask=%d got=%d", ms(t), asked, given) }
        log.notice("CLASSICVOICES renders: \(parts.joined(separator: " | "), privacy: .public)")
    }

    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        // Speech starts first; logging the previous request's renders can wait.
        let previous = (requestStart.pointee, Int(renderCount.pointee),
                        (0..<min(Int(renderCount.pointee), 8)).map { renderLog[$0] })
        renderCount.pointee = 0
        requestStart.pointee = mach_absolute_time()
        let entry = VoiceCatalog.entry(forIdentifier: request.voice.identifier)
        let key = entry?.key ?? "ios6/" + (request.voice.primaryLanguages.first ?? "en-US")
        let settings = SpeechSettings.current
        let rules = settings.rulesEnabled(entry?.identifier ?? "") ? PronunciationRules.rules(forVoiceKey: key) : nil
        let job = SpeechJob.from(ssml: request.ssmlRepresentation, rules: rules, exactPitch: key.hasPrefix("mt"))
        let fallback = entry.flatMap { settings.pitchFallbackEnabled($0.identifier) ? VoiceCatalog.compactCounterpart(of: $0)?.key : nil }
        VoiceEngine.shared.speak(job, voiceKey: key, fallbackKey: fallback,
                                 resetCommands: settings.resetCommandsEnabled(entry?.identifier ?? ""),
                                 twentyFourHour: settings.twentyFourHour, modernRate: settings.modernRate)
        dumpRenderLog(start: previous.0, count: previous.1, renders: previous.2)
        log.notice("CLASSICVOICES request \(key, privacy: .public) fallback=\(fallback ?? "-", privacy: .public) rate=\(job.rate) pitch=\(job.pitch) pause=\(job.trailingPauseMs) text=[\(job.text.replacingOccurrences(of: "\u{1b}", with: "<ESC>"), privacy: .public)] ssml=\(request.ssmlRepresentation, privacy: .public)")
        if UserDefaults.standard.string(forKey: "lastVoiceKey") != key {
            UserDefaults.standard.set(key, forKey: "lastVoiceKey")
        }
    }

    public override func cancelSpeechRequest() {
        VoiceEngine.shared.cancel()
    }

    public override var internalRenderBlock: AUInternalRenderBlock {
        let stream = VoiceEngine.shared.stream
        let renderLog = self.renderLog, renderCount = self.renderCount
        return { actionFlags, _, frameCount, _, outputData, _, _ in
            let buffers = UnsafeMutableAudioBufferListPointer(outputData)
            guard let raw = buffers[0].mData else { return noErr }
            let out = raw.assumingMemoryBound(to: Float.self)
            let frames = Int(frameCount)
            var done: Int32 = 1
            var n = 0
            if let stream {
                // The system pulls speech faster than real time, so silence we
                // hand back is queued ahead of the voice. Wait (briefly) for real
                // samples instead of padding: fill the whole request unless the
                // utterance has ended.
                let deadline = mach_absolute_time() + 500_000_000 / 41  // ~500 ms (24 MHz timebase ≈ 41.67 ns/tick)
                while true {
                    var d: Int32 = 0
                    n += vxs_read(stream, out + n, frames - n, &d)
                    done = d
                    if n == frames || d != 0 || mach_absolute_time() > deadline { break }
                    usleep(500)
                }
            }
            let i = Int(renderCount.pointee)
            if i < 8 { renderLog[i] = (mach_absolute_time(), Int32(frames), Int32(n)); renderCount.pointee += 1 }
            if n < frames { (out + n).update(repeating: 0, count: frames - n) }
            buffers[0].mDataByteSize = UInt32(frames * MemoryLayout<Float>.size)
            if done != 0 && n == 0 {
                actionFlags.pointee = .offlineUnitRenderAction_Complete
            }
            return noErr
        }
    }
}
