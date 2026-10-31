import Foundation

/// How the voices speak, as set in the app: switches, each with a default
/// for every voice and overrides per voice (by identifier). Kept as
/// `settings.json` in the extension's store, written by the app over the
/// store's channel and read by the extension on each request.
struct SpeechSettings: Codable, Equatable {
    /// Apple's text rules (PronunciationRules): the ones the voice's own iOS
    /// release or Mac OS X voice had.
    var rules = true
    /// Apple's later pronunciation dictionary and rules (a VoiceResources
    /// pack per language, see PronunciationPacks), loaded into the engine on
    /// top of the voice's own. Off: each voice speaks as its release did.
    var newerRules = false
    /// Pitch changes speak with the voice's compact version, as the original
    /// VoiceOver did: enhanced and premium voices can't change pitch themselves.
    var pitchFallback = true
    /// MacinTalk: embedded speech commands ([[inpt PHON]], [[rate 300]] ...)
    /// last only for the utterance they are in, instead of until changed back.
    var resetCommands = true
    /// MacinTalk: Leopard reads 24-hour times as Snow Leopard does (13:45 is
    /// "thirteen forty-five", not "one forty-five PM").
    var twentyFourHour = true
    /// MacinTalk: a rate speaks at the words per minute today's Alex speaks
    /// at, instead of a percent of the voice's own default rate.
    var modernRate = true
    var rulesOverride: [String: Bool] = [:]
    var newerRulesOverride: [String: Bool] = [:]
    var pitchFallbackOverride: [String: Bool] = [:]
    var resetCommandsOverride: [String: Bool] = [:]

    static let file = "settings.json"

    func rulesEnabled(_ voiceID: String) -> Bool { rulesOverride[voiceID] ?? rules }
    func newerRulesEnabled(_ voiceID: String) -> Bool { newerRulesOverride[voiceID] ?? newerRules }
    /// Whether any voice has the newer rules on, so their packs are needed.
    var newerRulesWanted: Bool { newerRules || newerRulesOverride.values.contains(true) }
    func pitchFallbackEnabled(_ voiceID: String) -> Bool { pitchFallbackOverride[voiceID] ?? pitchFallback }
    func resetCommandsEnabled(_ voiceID: String) -> Bool { resetCommandsOverride[voiceID] ?? resetCommands }

    init() {}
    // Settings saved before a switch existed keep the others, and the new one starts at its default.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        rules = try c.decodeIfPresent(Bool.self, forKey: .rules) ?? rules
        newerRules = try c.decodeIfPresent(Bool.self, forKey: .newerRules) ?? newerRules
        pitchFallback = try c.decodeIfPresent(Bool.self, forKey: .pitchFallback) ?? pitchFallback
        resetCommands = try c.decodeIfPresent(Bool.self, forKey: .resetCommands) ?? resetCommands
        twentyFourHour = try c.decodeIfPresent(Bool.self, forKey: .twentyFourHour) ?? twentyFourHour
        modernRate = try c.decodeIfPresent(Bool.self, forKey: .modernRate) ?? modernRate
        rulesOverride = try c.decodeIfPresent([String: Bool].self, forKey: .rulesOverride) ?? [:]
        newerRulesOverride = try c.decodeIfPresent([String: Bool].self, forKey: .newerRulesOverride) ?? [:]
        pitchFallbackOverride = try c.decodeIfPresent([String: Bool].self, forKey: .pitchFallbackOverride) ?? [:]
        resetCommandsOverride = try c.decodeIfPresent([String: Bool].self, forKey: .resetCommandsOverride) ?? [:]
    }

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
