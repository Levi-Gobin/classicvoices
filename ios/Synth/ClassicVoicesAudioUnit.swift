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
        // Warm up the most recently used voice so the first utterance is fast.
        VoiceEngine.shared.preload(UserDefaults.standard.string(forKey: "lastVoiceKey") ?? "ios6/en-US")
        log.notice("CLASSICVOICES audio unit created; root=\(VoiceCatalog.root?.path ?? "nil", privacy: .public) voices=\(VoiceCatalog.entries.count)")
    }

    public override var outputBusses: AUAudioUnitBusArray { busses }

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
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

    private func dumpRenderLog() {
        let n = Int(renderCount.pointee)
        guard n > 0, requestStart.pointee > 0 else { return }
        var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
        let ms = { (t: UInt64) in Double(t - self.requestStart.pointee) * Double(tb.numer) / Double(tb.denom) / 1e6 }
        let parts = (0..<min(n, 8)).map { i -> String in
            let (t, asked, given) = renderLog[i]
            return String(format: "+%.0fms ask=%d got=%d", ms(t), asked, given)
        }
        log.notice("CLASSICVOICES renders: \(parts.joined(separator: " | "), privacy: .public)")
    }

    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        dumpRenderLog()
        renderCount.pointee = 0
        requestStart.pointee = mach_absolute_time()
        let job = SpeechJob.from(ssml: request.ssmlRepresentation)
        let key = VoiceCatalog.entry(forIdentifier: request.voice.identifier)?.key
            ?? "ios6/" + (request.voice.primaryLanguages.first ?? "en-US")
        log.notice("CLASSICVOICES request \(key, privacy: .public) rate=\(job.rate) pause=\(job.trailingPauseMs) text=[\(job.text.replacingOccurrences(of: "\u{1b}", with: "<ESC>"), privacy: .public)] ssml=\(request.ssmlRepresentation, privacy: .public)")
        VoiceEngine.shared.speak(job, voiceKey: key)
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
