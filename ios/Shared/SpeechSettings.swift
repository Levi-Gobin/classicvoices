import Foundation

/// How the voices speak, as set in the app: two switches, each with a default
/// for every voice and overrides per voice (by identifier). Kept as
/// `settings.json` in the extension's store, written by the app over the
/// store's channel and read by the extension on each request.
struct SpeechSettings: Codable, Equatable {
    /// Apple's text rules (PronunciationRules), as the original iOS applied them.
    var rules = true
    /// Pitch changes speak with the voice's compact version, as the original
    /// VoiceOver did: enhanced and premium voices can't change pitch themselves.
    var pitchFallback = true
    var rulesOverride: [String: Bool] = [:]
    var pitchFallbackOverride: [String: Bool] = [:]

    static let file = "settings.json"

    func rulesEnabled(_ voiceID: String) -> Bool { rulesOverride[voiceID] ?? rules }
    func pitchFallbackEnabled(_ voiceID: String) -> Bool { pitchFallbackOverride[voiceID] ?? pitchFallback }

    static func decode(_ data: Data) -> SpeechSettings? { try? JSONDecoder().decode(SpeechSettings.self, from: data) }
    func encoded() -> Data { (try? JSONEncoder().encode(self)) ?? Data() }

    // MARK: - the extension's copy

    nonisolated(unsafe) private static var cached = SpeechSettings()
    nonisolated(unsafe) private static var cachedDate: Date?
    private static let lock = NSLock()

    /// Settings as stored in this process's voice root (the extension's
    /// store, or the App Group for a process without one), re-read when the
    /// file changes.
    static var current: SpeechSettings {
        guard let url = VoiceCatalog.enhancedRoot?.appendingPathComponent(file) else { return cached }
        lock.lock(); defer { lock.unlock() }
        let date = (try? FileManager.default.attributesOfItem(atPath: url.path)[.modificationDate]) as? Date
        if date != cachedDate {
            cachedDate = date
            cached = (try? Data(contentsOf: url)).flatMap(decode) ?? SpeechSettings()
        }
        return cached
    }
}
