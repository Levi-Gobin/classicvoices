import AVFoundation
import os

let log = Logger(subsystem: "com.levigobin.ClassicVoices", category: "synth")

private final class BundleMarker {}

#if os(macOS)
let appGroup = "7WV49AN2BX.com.levigobin.ClassicVoices"  // team-prefixed groups need no portal setup on macOS
#else
/// Sideloading tools rename App Groups when they re-sign the app with
/// someone's own Apple ID. AltStore and SideStore list the new names under
/// ALTAppGroups in Info.plist; others (Sideloadly) only put them in the
/// provisioning profile they embed, so read that too. The extension looks at
/// the containing app as well.
let appGroup: String = {
    let fallback = "group.com.levigobin.ClassicVoices"
    var bundles = [Bundle.main.bundleURL]
    if Bundle.main.bundleURL.pathExtension == "appex" {  // X.app/PlugIns/Synth.appex
        bundles.append(Bundle.main.bundleURL.deletingLastPathComponent().deletingLastPathComponent())
    }
    let pick = { (groups: [String]) in groups.first { $0.contains("ClassicVoices") } ?? groups[0] }
    for url in bundles {
        if let groups = Bundle(url: url)?.infoDictionary?["ALTAppGroups"] as? [String], !groups.isEmpty {
            return pick(groups)
        }
    }
    for url in bundles {
        if let groups = profileAppGroups(url.appendingPathComponent("embedded.mobileprovision")), !groups.isEmpty {
            return pick(groups)
        }
    }
    return fallback
}()

/// App Groups granted by a provisioning profile (a signed plist).
func profileAppGroups(_ url: URL) -> [String]? {
    guard let data = try? Data(contentsOf: url),
          let start = data.range(of: Data("<?xml".utf8)),
          let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound..<data.endIndex),
          let plist = try? PropertyListSerialization.propertyList(from: data[start.lowerBound..<end.upperBound], format: nil) as? [String: Any],
          let ents = plist["Entitlements"] as? [String: Any] else { return nil }
    return (ents["com.apple.security.application-groups"] as? [String])?.filter { !$0.contains("*") }
}
#endif

/// Voices from each bundled firmware (Voices/<build>/ inside the Synth
/// extension) plus enhanced voices downloaded into the App Group.
enum VoiceCatalog {
    static let builds = [("ios6", "iOS 6", "6.1.6"), ("ios5", "iOS 5", "5.1.1"),
                         ("ios4", "iOS 4", "4.3.5"), ("ios3", "iOS 3", "3.1.3")]

    /// Mac OS X releases whose Nuance voices we carry: compacts are bundled,
    /// premium voices (and the extra compacts) are downloaded.
    static let macEras = [("mac107", "Lion", "10.7"), ("mac108", "Mountain Lion", "10.8"),
                          ("mac109", "Mavericks", "10.9")]

    struct Entry: Hashable {
        let build: String      // "ios6" / "ios5" / "ios4" / "ios3", or a Mac era "mac107"
        let buildName: String  // "iOS 6" / "Lion"
        let version: String
        let language: String   // e.g. "en-US"
        let name: String       // e.g. "Samantha"
        var enhanced = false
        var macDir: String? = nil  // Mac voices: folder name, e.g. "KarenCompact" / "Karen"
        var premium = false        // Mac premium (full unit-selection) voice
        var macinTalk = false      // Apple MacinTalk voice (Tiger/Leopard/Snow Leopard)
        private var tag: String { enhanced ? build + "e" : build }
        var identifier: String {
            if let macDir { return "com.levigobin.ClassicVoices.\(build).\(macDir)" }
            return "com.levigobin.ClassicVoices.\(tag).\(language)"
        }
        var displayName: String {
            if macinTalk { return "\(name) (\(buildName))" }
            if macDir != nil { return "\(name) (\(buildName) \(premium ? "Premium" : "Compact"))" }
            return "\(name) (\(buildName)\(enhanced ? " Enhanced" : ""))"
        }
        var key: String { macDir.map { "\(build)/\($0)" } ?? "\(tag)/\(language)" }  // engine voice key
    }

    /// Mac OS X MacinTalk generations (Apple's own engine, run on the x86
    /// interpreter): <root>/<gen>/{MacinTalk, SpeechDictionary, lib, Voices}.
    static let macinTalkEras = [("mt104", "Tiger", "10.4"), ("mt105", "Leopard", "10.5"),
                                ("mt106", "Snow Leopard", "10.6"), ("mt107", "Lion", "10.7")]

    /// Voices in <base>/<gen>/Voices/<Name>.SpeechVoice; the display name is
    /// the Pascal string at offset 16 of the VoiceDescription.
    static func macinTalkVoices(in base: URL) -> [Entry] {
        var out: [Entry] = []
        for (gen, genName, version) in macinTalkEras where hasMacinTalkEngine(gen) {
            let dir = base.appendingPathComponent(gen).appendingPathComponent("Voices")
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { continue }
            for folder in names.sorted() where folder.hasSuffix(".SpeechVoice") {
                let vd = dir.appendingPathComponent(folder).appendingPathComponent("Contents/Resources/VoiceDescription")
                guard let d = try? Data(contentsOf: vd), d.count > 80 else { continue }
                let len = Int(d[d.startIndex + 16])
                let name = String(decoding: d[(d.startIndex + 17)..<(d.startIndex + 17 + min(len, 63))], as: UTF8.self)
                out.append(Entry(build: gen, buildName: genName, version: version, language: "en-US",
                                 name: name.isEmpty ? String(folder.dropLast(12)) : name,
                                 macDir: String(folder.dropLast(".SpeechVoice".count)), macinTalk: true))
            }
        }
        return out
    }

    /// MacinTalk engines come with the app when it was built with them, or
    /// were added from someone's own Mac OS X files.
    static func hasBundledMacinTalk(_ gen: String) -> Bool {
        root.map { FileManager.default.fileExists(atPath: $0.appendingPathComponent("\(gen)/MacinTalk").path) } ?? false
    }

    static func hasMacinTalkEngine(_ gen: String) -> Bool {
        hasBundledMacinTalk(gen)
            || enhancedRoot.map { FileManager.default.fileExists(atPath: $0.appendingPathComponent("\(gen)/MacinTalk").path) } ?? false
    }

    /// Nuance language codes (broker.hdr <langcode>) used by the Mac voices.
    static let nuanceLanguages = [
        "ARW": "ar-SA", "CAH": "zh-HK", "CZC": "cs-CZ", "DAD": "da-DK", "DUB": "nl-BE", "DUN": "nl-NL",
        "ENA": "en-AU", "ENE": "en-IE", "ENG": "en-GB", "ENS": "en-GB", "ENU": "en-US", "ENZ": "en-ZA",
        "ENI": "en-IN", "FIF": "fi-FI", "FRC": "fr-CA", "FRF": "fr-FR", "GED": "de-DE", "GRG": "el-GR",
        "HII": "hi-IN", "HUH": "hu-HU", "IDI": "id-ID", "ITI": "it-IT", "JPJ": "ja-JP", "KOK": "ko-KR",
        "MNC": "zh-CN", "MNT": "zh-TW", "NON": "no-NO", "PLP": "pl-PL", "PTB": "pt-BR", "PTP": "pt-PT",
        "ROR": "ro-RO", "RUR": "ru-RU", "SKS": "sk-SK", "SPE": "es-ES", "SPM": "es-MX", "SWS": "sv-SE",
        "THT": "th-TH", "TRT": "tr-TR",
    ]

    private static func tag(_ tag: String, in xml: String) -> String? {
        guard let a = xml.range(of: "<\(tag)>"), let b = xml.range(of: "</\(tag)>", range: a.upperBound..<xml.endIndex)
        else { return nil }
        return String(xml[a.upperBound..<b.lowerBound])
    }

    /// Mac voices under <base>/<era>/<VoiceDir>/broker.hdr. The downloader
    /// writes a `locale` file when the language code isn't in our table.
    static func macVoices(in base: URL) -> [Entry] {
        var out: [Entry] = []
        for (era, eraName, version) in macEras {
            let dir = base.appendingPathComponent(era)
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { continue }
            for name in names.sorted() where name != "common" && !name.hasPrefix(".") {
                let vdir = dir.appendingPathComponent(name)
                guard let xml = try? String(contentsOf: vdir.appendingPathComponent("broker.hdr"), encoding: .isoLatin1),
                      let voice = tag("voice", in: xml) else { continue }
                let code = tag("langcode", in: xml) ?? ""
                let locale = (try? String(contentsOf: vdir.appendingPathComponent("locale"), encoding: .utf8))?
                    .trimmingCharacters(in: .whitespacesAndNewlines)
                guard let language = nuanceLanguages[code] ?? locale else { continue }
                out.append(Entry(build: era, buildName: eraName, version: version, language: language, name: voice,
                                 macDir: name, premium: tag("voicemodel", in: xml)?.hasPrefix("full") ?? false))
            }
        }
        return out
    }

    /// Directory containing one folder per build. Works from both the
    /// extension and the containing app.
    static let root: URL? = {
        let appex = Bundle.main.builtInPlugInsURL?.appendingPathComponent("Synth.appex")
        let candidates = [
            Bundle(for: BundleMarker.self).resourceURL,
            appex.flatMap { Bundle(url: $0)?.resourceURL },  // macOS: Synth.appex/Contents/Resources
            appex,
        ]
        for base in candidates.compactMap({ $0 }) {
            let dir = base.appendingPathComponent("Voices")
            if FileManager.default.fileExists(atPath: dir.appendingPathComponent("ios6/voiced").path) {
                return dir
            }
        }
        return nil
    }()

    /// Downloaded enhanced voices: <group>/EnhancedVoices/<build>/<lang>/
    static var enhancedRoot: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroup)?
            .appendingPathComponent("EnhancedVoices")
    }

    static func enhancedDirectory(build: String, language: String) -> URL? {
        enhancedRoot?.appendingPathComponent(build).appendingPathComponent(language)
    }

    static let builtIn: [Entry] = {
        guard let root else { return [] }
        var out: [Entry] = []
        for (build, buildName, version) in builds {
            let res = root.appendingPathComponent(build).appendingPathComponent("TTSResources")
            guard let langs = try? FileManager.default.contentsOfDirectory(atPath: res.path) else { continue }
            for lang in langs.sorted() {
                let broker = res.appendingPathComponent(lang).appendingPathComponent("broker.hdr")
                guard let handle = try? FileHandle(forReadingFrom: broker),
                      let head = try? handle.read(upToCount: 64),
                      let line = String(data: head, encoding: .utf8)?.split(separator: "\n").first,
                      line.hasPrefix("defaultvoice:") else { continue }
                out.append(Entry(build: build, buildName: buildName, version: version, language: lang,
                                 name: String(line.dropFirst("defaultvoice:".count))))
            }
        }
        return out + macVoices(in: root) + macinTalkVoices(in: root)
    }()

    /// Re-scanned each time: voices can be downloaded while we run.
    static var enhanced: [Entry] {
        guard let base = enhancedRoot else { return [] }
        var out: [Entry] = []
        for (build, buildName, version) in builds {
            let dir = base.appendingPathComponent(build)
            guard let langs = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { continue }
            for lang in langs.sorted() where !lang.hasPrefix(".") {
                let broker = dir.appendingPathComponent(lang).appendingPathComponent("broker.hdr.asset")
                guard let xml = try? String(contentsOf: broker, encoding: .isoLatin1),
                      let a = xml.range(of: "<voice>"), let b = xml.range(of: "</voice>", range: a.upperBound..<xml.endIndex)
                else { continue }
                out.append(Entry(build: build, buildName: buildName, version: version, language: lang,
                                 name: String(xml[a.upperBound..<b.lowerBound]), enhanced: true))
            }
        }
        // Downloaded Mac voices that aren't also bundled.
        let bundled = Set(builtIn.map(\.key))
        return out + (macVoices(in: base) + macinTalkVoices(in: base)).filter { !bundled.contains($0.key) }
    }

    static var entries: [Entry] { builtIn + enhanced }

    /// The system prefixes our identifiers with the extension's bundle ID,
    /// so match on the suffix.
    static func entry(forIdentifier id: String) -> Entry? {
        entries.first { id == $0.identifier || id.hasSuffix("." + $0.identifier) }
    }

    static func systemVoice(for entry: Entry) -> AVSpeechSynthesisVoice? {
        AVSpeechSynthesisVoice.speechVoices().first { $0.identifier.hasSuffix(entry.identifier) }
    }

    /// Voices switched off in the app (by identifier): the system doesn't see
    /// them. Kept in the App Group so the extension reads the same list.
    static var hiddenVoices: Set<String> {
        get { Set(UserDefaults(suiteName: appGroup)?.stringArray(forKey: "hiddenVoices") ?? []) }
        set { UserDefaults(suiteName: appGroup)?.set(newValue.sorted(), forKey: "hiddenVoices") }
    }

    static var providerVoices: [AVSpeechSynthesisProviderVoice] {
        let hidden = hiddenVoices
        return entries.filter { !hidden.contains($0.identifier) }.map {
            let v = AVSpeechSynthesisProviderVoice(name: $0.displayName, identifier: $0.identifier,
                                                   primaryLanguages: [$0.language],
                                                   supportedLanguages: [$0.language])
            v.version = $0.version
            if $0.enhanced { v.voiceSize = 150_000_000 }
            if $0.premium { v.voiceSize = 400_000_000 }
            return v
        }
    }
}

/// Plain text plus prosody pulled out of the SSML the system hands us.
struct SpeechJob {
    var text = ""
    var rate = 100     // percent of normal, engine accepts 50...400
    var pitch = 100    // percent of normal, engine accepts 50...200
    var volume = -1    // 0...100, or -1 to leave the voice's default
    var trailingPauseMs = 0  // silence after the speech (an SSML <break> at the end)

    static func from(ssml: String) -> SpeechJob {
        let parser = SSMLParser()
        let xml = XMLParser(data: Data(ssml.utf8))
        xml.delegate = parser
        if xml.parse() {
            var job = parser.job
            parser.assemble(into: &job)
            return job
        }
        // Not well-formed: strip anything tag-like.
        var job = SpeechJob()
        job.text = ssml.replacingOccurrences(of: "<[^>]+>", with: " ", options: .regularExpression)
        return job
    }
}

private final class SSMLParser: NSObject, XMLParserDelegate {
    var job = SpeechJob()
    // prosody currently in effect while walking the tree
    private var current = (rate: 100, pitch: 100, volume: -1)
    private var stack: [(rate: Int, pitch: Int, volume: Int)] = []
    private var captured = false

    enum Token { case text(String), pause(Int), sentenceEnd, pitch(Int) }
    private var emittedPitch: Int?
    private var tokens: [Token] = []

    /// Build engine text with Vocalizer escapes: ESC\pause=N\ and ESC\eos=1\.
    func assemble(into job: inout SpeechJob) {
        guard let lastText = tokens.lastIndex(where: {
            if case .text(let t) = $0 { return !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            return false
        }) else { job.text = ""; return }
        let firstText = tokens.firstIndex(where: {
            if case .text(let t) = $0 { return !t.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            return false
        })!
        var out = ""
        for (i, tok) in tokens[...lastText].enumerated() {
            if i < firstText, case .pitch = tok {} else if i < firstText { continue }  // leading pauses add latency
            switch tok {
            case .text(let t): out += t.replacingOccurrences(of: "\u{1b}", with: " ")
            case .pause(let ms): out += " \u{1b}\\pause=\(ms)\\ "
            case .sentenceEnd: out += " \u{1b}\\eos=1\\ "
            case .pitch(let p): out += "\u{1b}\\pitch=\(vxs_engine_pitch(Int32(p)))\\"
            }
        }
        job.text = out.trimmingCharacters(in: .whitespacesAndNewlines)
        job.trailingPauseMs = tokens[(lastText + 1)...].reduce(0) {
            if case .pause(let ms) = $1 { return $0 + ms }
            return $0
        }
    }

    static func breakMs(_ attrs: [String: String]) -> Int {
        if let t = attrs["time"]?.trimmingCharacters(in: .whitespaces).lowercased() {
            if t.hasSuffix("ms"), let v = Double(t.dropLast(2)) { return Int(v) }
            if t.hasSuffix("s"), let v = Double(t.dropLast()) { return Int(v * 1000) }
        }
        switch attrs["strength"] ?? "medium" {
        case "none": return 0
        case "x-weak": return 100
        case "weak": return 200
        case "strong": return 700
        case "x-strong": return 1000
        default: return 400
        }
    }

    func parser(_ parser: XMLParser, didStartElement name: String, namespaceURI: String?,
                qualifiedName: String?, attributes: [String: String] = [:]) {
        switch name {
        case "prosody":
            stack.append(current)
            if let r = attributes["rate"] { current.rate = Self.scale(r, base: current.rate, keywords: [
                "x-slow": 50, "slow": 75, "medium": 100, "default": 100, "fast": 150, "x-fast": 200]) }
            if let p = attributes["pitch"] { current.pitch = Self.pitchScale(p, base: current.pitch) }
            if let v = attributes["volume"], !v.hasSuffix("dB") {
                current.volume = Self.scale(v, base: current.volume < 0 ? 80 : current.volume, keywords: [
                    "silent": 0, "x-soft": 20, "soft": 40, "medium": 60, "default": 80, "loud": 90, "x-loud": 100])
            }
        case "break":
            let ms = Self.breakMs(attributes)
            if ms > 0 { tokens.append(.pause(ms)) }
        default:
            break
        }
    }

    func parser(_ parser: XMLParser, didEndElement name: String, namespaceURI: String?, qualifiedName: String?) {
        if name == "prosody", let saved = stack.popLast() { current = saved }
        if name == "s" || name == "p" { tokens.append(.sentenceEnd) }
    }

    func parser(_ parser: XMLParser, foundCharacters string: String) {
        // One engine call per request, so the prosody around the first real
        // text wins.
        let hasText = !string.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        if !captured && hasText {
            // The utterance's baseline is the prosody around its first text,
            // except pitch, which follows each span inline (VoiceOver lowers
            // pitch for just a word or two).
            (job.rate, job.volume) = (current.rate, current.volume)
            captured = true
        }
        if hasText && emittedPitch != current.pitch {
            if emittedPitch != nil || current.pitch != 100 { tokens.append(.pitch(current.pitch)) }
            emittedPitch = current.pitch
        }
        if case .text(let prev)? = tokens.last {
            tokens[tokens.count - 1] = .text(prev + string)
        } else {
            tokens.append(.text(string))
        }
    }

    /// Pitch changes are taken as fractions of an octave, which is how they
    /// sound with Apple's own voices: VoiceOver on macOS sends -40% / -100% /
    /// +50% (iOS sends -15%), and a linear factor made -40% sound far too low.
    /// "+12%" / "-40%" -> base * 2^(n/100), clamped to the engine's range.
    static func pitchScale(_ raw: String, base: Int) -> Int {
        let s = raw.trimmingCharacters(in: .whitespaces).lowercased()
        let keywords = ["x-low": 60, "low": 80, "medium": 100, "default": 100, "high": 120, "x-high": 140]
        if let k = keywords[s] { return k }
        var factor = 1.0
        if s.hasSuffix("%"), let n = Double(s.dropLast()) {
            factor = (s.hasPrefix("+") || s.hasPrefix("-")) ? pow(2, n / 100) : n / 100
        } else if s.hasSuffix("st"), let n = Double(s.dropLast(2)) {
            factor = pow(2, n / 12)  // semitones
        } else if let n = Double(s) {
            factor = n
        }
        guard factor.isFinite else { return base }
        return Int((Double(base) * min(max(factor, 0.56), 1.58)).rounded())
    }

    /// "150%", "+20%", "-10%", "1.5", or a keyword -> percent.
    static func scale(_ raw: String, base: Int, keywords: [String: Int]) -> Int {
        let s = raw.trimmingCharacters(in: .whitespaces).lowercased()
        if let k = keywords[s] { return k }
        if s.hasSuffix("%"), let n = Double(s.dropLast()) {
            return Int(((s.hasPrefix("+") || s.hasPrefix("-")) ? Double(base) * (1 + n / 100) : n).rounded())
        }
        if let n = Double(s) { return Int((Double(base) * n).rounded()) }
        return base
    }
}

/// One engine stream shared by everything in the process.
final class VoiceEngine {
    static let shared = VoiceEngine()
    let stream: OpaquePointer?

    private init() {
        if let root = VoiceCatalog.root {
            stream = vxs_create(root.path)
            if let enhanced = VoiceCatalog.enhancedRoot { vxs_set_asset_root(stream, enhanced.path) }
        } else {
            log.error("voice resources not found in bundle")
            stream = nil
        }
    }

    func speak(_ job: SpeechJob, voiceKey: String) {
        guard let stream else { return }
        vxs_start(stream, voiceKey, job.text, Int32(job.rate), Int32(job.pitch), Int32(job.volume),
                  Int32(job.trailingPauseMs))
    }

    func cancel() {
        if let stream { vxs_cancel(stream) }
    }

    func preload(_ voiceKey: String) {
        if let stream { vxs_preload(stream, voiceKey) }
    }
}
