import AVFoundation

/// Extension principal class: the system asks it to create the audio unit.
public final class AudioUnitFactory: NSObject, AUAudioUnitFactory {
    public func beginRequest(with context: NSExtensionContext) {}

    @objc public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        log.notice("CLASSICVOICES createAudioUnit called")
        return try ClassicVoicesAudioUnit(componentDescription: componentDescription, options: [])
    }
}
