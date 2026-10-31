import AVFoundation
import Observation
import SwiftUI

/// A release the voices came from, as the version menus list them.
struct VoiceVersion: Identifiable, Hashable {
    enum Platform: String { case iOS, macOS }
    let id: String         // "ios6" / "lion"
    let platform: Platform
    let name: String       // "iOS 6" / "Lion"
    let number: String     // "6.1.6" / "10.7"
    let builds: [String]   // VoiceCatalog.Entry.build values it covers

    /// Menu title: "iOS 6" / "10.7 Lion".
    var menuName: String { platform == .iOS ? name : "\(number) \(name)" }
    /// How the test sentence names it: "iOS 6" / "Mac OS X Lion".
    var spokenName: String { platform == .iOS ? name : "Mac OS X \(name)" }

    static let all: [VoiceVersion] = [
        .init(id: "ios3", platform: .iOS, name: "iOS 3", number: "3.1.3", builds: ["ios3"]),
        .init(id: "ios4", platform: .iOS, name: "iOS 4", number: "4.3.5", builds: ["ios4"]),
        .init(id: "ios5", platform: .iOS, name: "iOS 5", number: "5.1.1", builds: ["ios5"]),
        .init(id: "ios6", platform: .iOS, name: "iOS 6", number: "6.1.6", builds: ["ios6"]),
        .init(id: "ios7", platform: .iOS, name: "iOS 7", number: "7.0.6", builds: ["ios70"]),
        .init(id: "tiger", platform: .macOS, name: "Tiger", number: "10.4", builds: ["mt104"]),
        .init(id: "leopard", platform: .macOS, name: "Leopard", number: "10.5", builds: ["mt105"]),
        .init(id: "snowleopard", platform: .macOS, name: "Snow Leopard", number: "10.6", builds: ["mt106"]),
        .init(id: "lion", platform: .macOS, name: "Lion", number: "10.7", builds: ["mt107", "mac107"]),
        .init(id: "mountainlion", platform: .macOS, name: "Mountain Lion", number: "10.8", builds: ["mac108"]),
        .init(id: "mavericks", platform: .macOS, name: "Mavericks", number: "10.9", builds: ["mac109"]),
    ]

    static func of(build: String) -> VoiceVersion? { all.first { $0.builds.contains(build) } }
}

/// One row of the browser: a voice the app has, or one it can download.
struct VoiceItem: Identifiable, Hashable {
    enum Kind { case compact, enhanced, premium, macinTalk }
    let id: String  // the identifier the system sees (VoiceCatalog.Entry.identifier)
    let version: VoiceVersion
    let language: String
    let name: String
    let kind: Kind
    var entry: VoiceCatalog.Entry?               // set once it's on the device
    var enhancedAsset: EnhancedDownloads.Asset?  // where it downloads from / what deleting removes
    var macPackage: MacDownloads.Voice?
    var discVoice: DiscDownloads.Voice?          // Alex, from an install disc

    var isInstalled: Bool { entry != nil }
    var isDownloadable: Bool { enhancedAsset != nil || macPackage != nil || discVoice != nil }
    var downloadSize: Int? { enhancedAsset?.size ?? macPackage?.size ?? discVoice?.size }
    var installedSize: Int? { discVoice?.installedSize ?? downloadSize }

    var kindName: String {
        switch kind {
        case .compact: "Compact"
        case .enhanced: "Enhanced"
        case .premium: "Premium"
        case .macinTalk: "MacinTalk"
        }
    }

    /// Default text on the voice's test screen.
    var greeting: String {
        let from = kind == .enhanced || kind == .premium ? "I am an enhanced voice from" : "I am from"
        return "Hello, my name is \(name). \(from) \(version.spokenName)."
    }

    static func languageName(_ code: String) -> String {
        Locale.current.localizedString(forIdentifier: code) ?? code
    }
}

/// Everything the browser shows. Rows keep their ids for the life of the app:
/// a download only changes its row, so nothing moves under VoiceOver's focus.
@MainActor @Observable
final class VoiceLibrary {
    /// One for the app's life: downloads report to it even when the system
    /// relaunches the app in the background to finish them.
    static let shared = VoiceLibrary()
    let enhanced = EnhancedDownloads()
    let mac = MacDownloads()
    let disc = DiscDownloads()
    let pronunciation = PronunciationDownloads()
    private(set) var items: [VoiceItem] = []
    private(set) var hidden = VoiceCatalog.hiddenVoices
    private(set) var settings = SpeechSettings()

    init() {
        rebuild()
        enhanced.onChange = { [weak self] in self?.rebuild() }
        mac.onChange = { [weak self] in self?.rebuild() }
        disc.onChange = { [weak self] in Task.detached { try? VoiceStore.shared.refresh() }; self?.rebuild() }
        // The store (the speech extension) says what's downloaded; it answers
        // off the main thread, and again after every change.
        VoiceStore.shared.onChange = { [weak self] in
            guard let self else { return }
            self.settings = VoiceStore.shared.snapshot.settings
            self.pronunciation.refresh()
            if self.settings.newerRulesWanted { self.pronunciation.ensure() }
            self.enhanced.refresh()
            self.mac.refresh()
            self.rebuild()
        }
        Task.detached {
            try? VoiceStore.shared.flush()   // installs left from a background download
            try? VoiceStore.shared.refresh()
        }
        Downloader.shared.restore()
    }

    func item(_ id: String) -> VoiceItem? { items.first { $0.id == id } }

    func rebuild() {
        var byID: [String: VoiceItem] = [:]
        var order: [String] = []
        func add(_ item: VoiceItem) {
            if byID[item.id] == nil { order.append(item.id) }
            byID[item.id] = item
        }
        for e in VoiceCatalog.entries {
            guard let version = VoiceVersion.of(build: e.build) else { continue }
            let kind: VoiceItem.Kind = e.macinTalk ? .macinTalk : e.premium ? .premium : e.enhanced ? .enhanced : .compact
            add(VoiceItem(id: e.identifier, version: version, language: e.language, name: e.name, kind: kind, entry: e))
        }
        for a in EnhancedDownloads.catalog {
            guard let version = VoiceVersion.of(build: a.build) else { continue }
            let id = VoiceCatalog.Entry(build: a.build, buildName: version.name, version: version.number,
                                        language: a.language, name: "", enhanced: true).identifier
            if var item = byID[id] {
                item.enhancedAsset = a
                byID[id] = item
            } else {
                add(VoiceItem(id: id, version: version, language: a.language, name: EnhancedDownloads.voiceName(a),
                              kind: .enhanced, enhancedAsset: a))
            }
        }
        for v in MacDownloads.catalog {
            guard let version = VoiceVersion.of(build: v.era) else { continue }
            // A package installs the premium voice and, often, a compact one.
            let folders = mac.installed.contains(v.id) ? MacDownloads.folders(of: v) : [v.name]
            for folder in folders {
                let id = "com.levigobin.ClassicVoices.\(v.era).\(folder)"
                if var item = byID[id] {
                    // A bundled compact stays undeletable.
                    if item.kind == .premium || mac.installed.contains(v.id) && !isBundled(item) { item.macPackage = v }
                    byID[id] = item
                } else if !mac.installed.contains(v.id) {
                    add(VoiceItem(id: id, version: version, language: v.locale, name: v.name, kind: .premium,
                                  macPackage: v))
                }
            }
        }
        for v in DiscDownloads.catalog {
            // Only where the rest of that release's MacinTalk is.
            guard let version = VoiceVersion.of(build: v.gen), VoiceCatalog.hasMacinTalkEngine(v.gen) else { continue }
            let id = VoiceCatalog.Entry(build: v.gen, buildName: version.name, version: version.number, language: "en-US",
                                        name: v.name, macDir: v.name, macinTalk: true).identifier
            if var item = byID[id] {
                item.discVoice = v
                byID[id] = item
            } else {
                add(VoiceItem(id: id, version: version, language: "en-US", name: v.name, kind: .macinTalk, discVoice: v))
            }
        }
        items = order.compactMap { byID[$0] }
    }

    private func isBundled(_ item: VoiceItem) -> Bool {
        VoiceCatalog.builtIn.contains { $0.identifier == item.id }
    }

    func isHidden(_ id: String) -> Bool { hidden.contains(id) }

    /// Changes the speech settings and saves them for the extension.
    func updateSettings(_ change: (inout SpeechSettings) -> Void) {
        var s = settings
        change(&s)
        guard s != settings else { return }
        settings = s
        Task.detached { VoiceStore.shared.saveSettings(s) }
        // The newer rules need their packs: fetched the first time any voice uses them.
        if s.newerRulesWanted { pronunciation.ensure() }
    }

    /// Apple's text rules for a voice: its own and the newer ones, as its switches say.
    func rules(for item: VoiceItem) -> PronunciationRules? {
        guard let entry = item.entry else { return nil }
        return PronunciationRules.rules(forVoiceKey: entry.key, identifier: item.id, language: entry.language,
                                        settings: settings)
    }

    /// Apple's later pronunciation pack for a voice, when it's on for it and downloaded.
    func rulesPack(for item: VoiceItem) -> String? {
        guard let entry = item.entry else { return nil }
        return PronunciationRules.newerPack(forVoiceKey: entry.key, identifier: item.id, language: entry.language,
                                            settings: settings)
    }

    /// The compact voice an enhanced or premium voice's pitch changes would use.
    func compactCounterpart(_ item: VoiceItem) -> VoiceItem? {
        guard let entry = item.entry, let c = VoiceCatalog.compactCounterpart(of: entry) else { return nil }
        return self.item(c.identifier)
    }

    /// Voices anywhere in the catalog, installed or not, matching a search.
    func search(_ query: String) -> [VoiceItem] {
        let words = query.lowercased().split(whereSeparator: \.isWhitespace).map(String.init)
        guard !words.isEmpty else { return [] }
        return items.filter { item in
            let hay = [item.name, item.kindName, item.language, VoiceItem.languageName(item.language),
                       item.version.menuName, item.version.spokenName, item.version.number,
                       item.version.platform.rawValue].joined(separator: " ").lowercased()
            return words.allSatisfy { hay.contains($0) }
        }
    }

    func setHidden(_ id: String, _ on: Bool) {
        if on { hidden.insert(id) } else { hidden.remove(id) }
        let list = hidden
        Task.detached {
            VoiceStore.shared.setHidden(list)
            await MainActor.run { AVSpeechSynthesisProviderVoice.updateSpeechVoices() }
        }
    }

    func download(_ item: VoiceItem) {
        if let a = item.enhancedAsset { enhanced.download(a) } else if let v = item.macPackage { mac.download(v) } else if let v = item.discVoice { disc.download(v) }
    }

    func remove(_ item: VoiceItem) {
        if let a = item.enhancedAsset { enhanced.remove(a) } else if let v = item.macPackage { mac.remove(v) } else if let v = item.discVoice { disc.remove(v) }
    }

    /// 0...1 while downloading.
    func progress(_ item: VoiceItem) -> Double? {
        if let a = item.enhancedAsset { return enhanced.progress[a.id] }
        if let v = item.macPackage { return mac.progress[v.id] }
        if let v = item.discVoice { return disc.progress[v.id] }
        return nil
    }

    func state(_ item: VoiceItem) -> VoiceInstallState {
        let total = item.downloadSize ?? 0
        if let p = progress(item) { return .downloading(bytesWritten: Int(Double(total) * p), totalBytes: total) }
        if !item.isInstalled { return .notInstalled(byteCount: total) }
        return item.isDownloadable ? .installed(byteCount: item.installedSize ?? total) : .builtIn
    }

    /// A row's action for a voice that isn't here: start or stop its download.
    func toggleDownload(_ item: VoiceItem) {
        if progress(item) != nil {
            if let a = item.enhancedAsset { enhanced.cancel(a.id) } else if let v = item.macPackage { mac.cancel(v.id) } else if let v = item.discVoice { disc.cancel(v.id) }
        } else {
            download(item)
        }
    }

    func dismissErrors() {
        enhanced.errors = [:]
        mac.errors = [:]
        disc.errors = [:]
    }

    func error(_ item: VoiceItem) -> String? {
        if let a = item.enhancedAsset { return enhanced.errors[a.id] }
        if let v = item.macPackage { return mac.errors[v.id] }
        if let v = item.discVoice { return disc.errors[v.id] }
        return nil
    }

    /// The other voices that go with this one when it's deleted.
    func packageMates(_ item: VoiceItem) -> [String] {
        guard let v = item.macPackage else { return [] }
        return MacDownloads.folders(of: v).filter { "com.levigobin.ClassicVoices.\(v.era).\($0)" != item.id }
    }
}

// MARK: - Views

struct RootView: View {
    @State private var library = VoiceLibrary.shared

    var body: some View {
        TabView {
            PlatformTab(platform: .iOS)
                .tabItem { Label("iOS", systemImage: "iphone") }
            PlatformTab(platform: .macOS)
                .tabItem { Label("macOS", systemImage: "desktopcomputer") }
            SettingsTab()
                .tabItem { Label("Settings", systemImage: "gearshape") }
        }
        .environment(library)
    }
}

enum BrowserRoute: Hashable {
    case language(String)
    case voice(String)  // VoiceItem.id
    case diagnostics
    case addFiles
}

/// One tab, as a drill-down: version menu and languages (or the voices, when
/// there's only one language), then a language's voices, then one voice.
/// Rows are buttons: moving focus onto one never opens it.
///
/// iOS pushes pages on a navigation stack. The Mac shows one page at a time
/// under a Back button (⌘[) and the path walked, and announces each new page,
/// as Speech Enhancer does.
struct PlatformTab: View {
    let platform: VoiceVersion.Platform
    @Environment(VoiceLibrary.self) private var library
    @AppStorage private var versionID: String
    @State private var path: [BrowserRoute] = []
    @State private var query = ""

    init(platform: VoiceVersion.Platform) {
        self.platform = platform
        _versionID = AppStorage(wrappedValue: "all", "version.\(platform.rawValue)")
    }

    private var versions: [VoiceVersion] { VoiceVersion.all.filter { $0.platform == platform } }
    private var tabName: String { platform == .iOS ? "iOS Voices" : "macOS Voices" }

    /// Voices of this tab in the chosen version.
    private func items(language: String? = nil) -> [VoiceItem] {
        library.items.filter {
            $0.version.platform == platform && (versionID == "all" || $0.version.id == versionID)
                && (language == nil || $0.language == language)
        }
    }

    private func open(_ route: BrowserRoute) { path.append(route) }

    var body: some View {
        #if os(macOS)
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 12) {
                Button("Back") { if !path.isEmpty { path.removeLast() } }
                    .keyboardShortcut("[", modifiers: .command)
                    .disabled(path.isEmpty)
                Text(crumbs.joined(separator: " › "))
                    .font(.headline)
                    .lineLimit(1).truncationMode(.head)
                    .accessibilityLabel("Current location: \(crumbs.joined(separator: ", "))")
                Spacer()
            }
            .padding(12)
            Divider()
            page(path.last)
        }
        .onChange(of: path) { _, new in
            let what = new.last.map(title) ?? tabName
            let detail: String
            switch new.last {
            case .voice?: detail = "voice"
            case .diagnostics?, .addFiles?: detail = ""
            case .language(let lang)?: detail = "\(items(language: lang).count) voices"
            case nil: detail = ""
            }
            AccessibilityNotification.Announcement(detail.isEmpty ? what : "\(what), \(detail)").post()
        }
        #else
        NavigationStack(path: $path) {
            page(nil)
                .navigationTitle(tabName)
                .navigationDestination(for: BrowserRoute.self) { route in
                    page(route).navigationTitle(title(route))
                }
        }
        #endif
    }

    private var crumbs: [String] {
        [platform == .iOS ? "iOS" : "macOS",
         versions.first { $0.id == versionID }?.menuName ?? "All versions"] + path.map(title)
    }

    private func title(_ route: BrowserRoute) -> String {
        switch route {
        case .language(let lang): VoiceItem.languageName(lang)
        case .voice(let id): library.item(id)?.name ?? "Voice"
        case .diagnostics: "Diagnostics"
        case .addFiles: "Add from Files"
        }
    }

    @ViewBuilder private func page(_ route: BrowserRoute?) -> some View {
        switch route {
        case nil: rootPage
        case .language(let lang)?:
            Page { VoiceButtons(items: items(language: lang), groupByVersion: versionID == "all", open: open) }
        case .voice(let id)?: VoiceDetail(id: id)
        case .diagnostics?: DiagnosticsView()
        case .addFiles?: AddFilesView()
        }
    }

    private var rootPage: some View {
        let items = items()
        let languages = Set(items.map(\.language)).sorted {
            VoiceItem.languageName($0).localizedStandardCompare(VoiceItem.languageName($1)) == .orderedAscending
        }
        let found = library.search(query)
        return Page {
            PageSection(nil) {
                SearchField(text: $query)
            }
            if !query.trimmingCharacters(in: .whitespaces).isEmpty {
                // Across everything, both platforms and all versions.
                if found.isEmpty {
                    Text("No voices match \u{201c}\(query)\u{201d}.").foregroundStyle(.secondary)
                } else {
                    VoiceButtons(items: found, groupByVersion: true, open: open)
                }
            } else {
            PageSection(nil) {
                Picker("Version", selection: $versionID) {
                    Text("All").tag("all")
                    ForEach(versions) { Text($0.menuName).tag($0.id) }
                }
                .pickerStyle(.menu)
                #if os(macOS)
                .frame(maxWidth: 300)
                #endif
            }
            if languages.count == 1 {
                VoiceButtons(items: items, groupByVersion: versionID == "all", open: open)
            } else {
                PageSection("Languages") {
                    ForEach(languages, id: \.self) { lang in
                        let n = items.filter { $0.language == lang }.count
                        OpenButton(title: VoiceItem.languageName(lang), detail: "\(n) voices") { open(.language(lang)) }
                    }
                }
            }
            if let note = alexNote {
                Text(note).font(.footnote).foregroundStyle(.secondary)
            }
            PageSection(nil) {
                if platform == .macOS {
                    OpenButton(title: "Add from Files", detail: "MacinTalk from your own Mac OS X") { open(.addFiles) }
                }
                OpenButton(title: "Diagnostics", detail: "a report to send if voices don\u{2019}t work") { open(.diagnostics) }
            }
            }
        }
    }

    /// MacinTalk releases whose files aren't here yet, or are missing Alex.
    private var alexNote: String? {
        guard let v = versions.first(where: { $0.id == versionID }),
              let gen = v.builds.first(where: { $0.hasPrefix("mt") }) else { return nil }
        if !VoiceCatalog.hasMacinTalkEngine(gen) {
            return "\(v.name)\u{2019}s MacinTalk voices aren\u{2019}t in this build of Classic Voices (a build on GitHub brings them). Add Apple\u{2019}s speech files from Mac OS X \(v.number) \(v.name) with Add from Files below."
        }
        guard let alex = DiscDownloads.catalog.first(where: { $0.gen == gen }),
              library.item(VoiceCatalog.Entry(build: gen, buildName: v.name, version: v.number, language: "en-US", name: alex.name,
                                              macDir: alex.name, macinTalk: true).identifier)?.isInstalled == false else { return nil }
        return "Alex doesn\u{2019}t come with the app: at several hundred megabytes, he downloads on his own from Mac OS X \(v.name)\u{2019}s install disc (\(alex.server))."
    }
}

/// A scrolling page: a list on iOS, a column of buttons on the Mac.
struct Page<Content: View>: View {
    @ViewBuilder let content: Content

    var body: some View {
        #if os(macOS)
        ScrollView {
            VStack(alignment: .leading, spacing: 6) { content }
                .padding(16)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        #else
        List { content }
        #endif
    }
}

struct PageSection<Content: View>: View {
    let header: String?
    @ViewBuilder let content: Content

    init(_ header: String?, @ViewBuilder content: () -> Content) {
        self.header = header
        self.content = content()
    }

    var body: some View {
        #if os(macOS)
        if let header {
            Text(header).font(.headline).padding(.top, 10).accessibilityAddTraits(.isHeader)
        }
        content
        #else
        if let header { Section(header) { content } } else { Section { content } }
        #endif
    }
}

/// A row that opens something. A button, so focusing it does nothing.
struct OpenButton: View {
    let title: String
    var detail: String? = nil
    var label: String? = nil  // what VoiceOver reads; title and detail by default
    var value: String? = nil  // progress: VoiceOver watches it and reads it at its own pace
    var opens = true          // shows the disclosure chevron
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                    if let detail { Text(detail).font(.footnote).foregroundStyle(.secondary) }
                }
                Spacer()
                #if !os(macOS)
                if opens {
                    Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundStyle(.tertiary)
                }
                #endif
            }
            .contentShape(Rectangle())
            #if os(macOS)
            .frame(width: 440, alignment: .leading)
            .padding(.vertical, 2)
            #endif
        }
        #if os(macOS)
        .buttonStyle(.bordered)
        #else
        .foregroundStyle(.primary)
        #endif
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(label ?? [title, detail].compactMap { $0 }.joined(separator: ", "))
        .accessibilityValue(value ?? "")
        .accessibilityAddTraits(value == nil ? .isButton : [.isButton, .updatesFrequently])
    }
}

/// Voices as rows: one section per version when showing all of them.
private struct VoiceButtons: View {
    let items: [VoiceItem]
    let groupByVersion: Bool
    let open: (BrowserRoute) -> Void
    @Environment(VoiceLibrary.self) private var library

    var body: some View {
        // A failed download puts its row straight back to "Download", which
        // on its own looks like the tap did nothing; say why.
        let failed = items.compactMap { item in library.error(item).map { "\(item.name): \($0)" } }
        if !failed.isEmpty {
            PageSection("Download problem") {
                ForEach(failed, id: \.self) { Text($0).foregroundStyle(.red) }
                Button("Dismiss") { library.dismissErrors() }
            }
        }
        if groupByVersion {
            ForEach(VoiceVersion.all.filter { v in items.contains { $0.version == v } }) { v in
                PageSection(v.spokenName) { rows(items.filter { $0.version == v }) }
            }
        } else {
            PageSection("Voices") { rows(items) }
        }
    }

    private func rows(_ items: [VoiceItem]) -> some View {
        ForEach(sorted(items)) { VoiceRow(item: $0, open: open) }
    }

    private func sorted(_ items: [VoiceItem]) -> [VoiceItem] {
        items.sorted {
            $0.name == $1.name ? $0.kindName < $1.kindName
                : $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
    }
}

/// One voice. Opens the voice when it's on the device; otherwise the row
/// downloads it, or stops the download, and its label says which before it
/// says the name (as in Classic Synths).
private struct VoiceRow: View {
    let item: VoiceItem
    let open: (BrowserRoute) -> Void
    @Environment(VoiceLibrary.self) private var library

    var body: some View {
        let state = library.state(item)
        let name = "\(item.name) (\(item.kindName))"
        OpenButton(title: name, detail: VoiceFormatting.detailText(state),
                   label: VoiceFormatting.accessibilityLabel(name: name, state: state),
                   value: VoiceFormatting.accessibilityValue(state),
                   opens: state.isAvailable) {
            if state.isAvailable { open(.voice(item.id)) } else { library.toggleDownload(item) }
        }
        .accessibilityActions {
            if state.isAvailable, let entry = item.entry {
                Button("Play sample") {
                    DirectPlayer.shared.speak(item.greeting, voiceKey: entry.key, rate: 100, rules: library.rules(for: item),
                                              rulesPack: library.rulesPack(for: item), settings: library.settings)
                }
            }
        }
    }
}

/// Where a voice stands, from the browser's point of view.
enum VoiceInstallState: Equatable {
    case builtIn                                  // part of the app; can't be removed
    case installed(byteCount: Int)                // downloaded
    case notInstalled(byteCount: Int)
    case downloading(bytesWritten: Int, totalBytes: Int)

    var isAvailable: Bool {
        switch self {
        case .builtIn, .installed: true
        case .notInstalled, .downloading: false
        }
    }
}

/// How a voice row reads, to the ear and to the eye (Classic Synths' wording).
enum VoiceFormatting {
    /// "12.3 KB", "45.6 MB": one decimal place, decimal units, so a list of
    /// sizes lines up.
    static func size(_ bytes: Int) -> String {
        let value = Double(max(bytes, 0))
        func number(_ v: Double) -> String {
            let f = NumberFormatter()
            f.numberStyle = .decimal
            f.minimumFractionDigits = 1
            f.maximumFractionDigits = 1
            return f.string(from: NSNumber(value: v)) ?? String(format: "%.1f", v)
        }
        if value >= 1_000_000_000 { return "\(number(value / 1_000_000_000)) GB" }
        if value >= 1_000_000 { return "\(number(value / 1_000_000)) MB" }
        if value >= 1_000 { return "\(number(value / 1_000)) KB" }
        return "\(Int(value)) bytes"
    }

    /// The state leads whenever the row does something other than open the
    /// voice: that's what you need before deciding to activate it.
    static func accessibilityLabel(name: String, state: VoiceInstallState) -> String {
        switch state {
        case .builtIn: name
        case .installed(let n): "\(name), using \(size(n))"
        case .notInstalled(let n): "Download \(name), \(size(n))"
        case .downloading: "Stop downloading \(name)"
        }
    }

    /// While downloading, how much has come, as the row's value: the label
    /// stays put, and VoiceOver reads the value as it changes (every 1%).
    static func accessibilityValue(_ state: VoiceInstallState) -> String? {
        if case .downloading(let w, let t) = state { return "\(size(w)) of \(size(t))" }
        return nil
    }

    static func detailText(_ state: VoiceInstallState) -> String? {
        switch state {
        case .builtIn: nil
        case .installed(let n): size(n)
        case .notInstalled(let n): "Download \u{2014} \(size(n))"
        case .downloading(let w, let t): "\(size(w)) of \(size(t))"
        }
    }
}

/// One voice: try it, whether the system sees it, and removing it.
struct VoiceDetail: View {
    let id: String
    @Environment(VoiceLibrary.self) private var library
    @ObservedObject private var player = DirectPlayer.shared
    @State private var text = ""
    @State private var rate = 100.0
    @State private var confirmingRemoval = false

    var body: some View {
        if let item = library.item(id), let entry = item.entry {
            Form {
                Section {
                    LabeledContent("Version", value: "\(item.version.spokenName) (\(item.version.number))")
                    LabeledContent("Language", value: VoiceItem.languageName(item.language))
                    LabeledContent("Type", value: item.kindName)
                } footer: {
                    if let note = Self.note(entry.key) { Text(note) }
                }
                Section {
                    TextField("Text to speak", text: $text, axis: .vertical)
                        .accessibilityLabel("Text to speak")
                    VStack(alignment: .leading) {
                        Text("Rate: \(Int(rate))%")
                        Slider(value: $rate, in: 50...400, step: 10) { Text("Rate") }
                    }
                    HStack {
                        Button { player.speak(text, voiceKey: entry.key, rate: Int(rate), rules: library.rules(for: item),
                                     rulesPack: library.rulesPack(for: item), settings: library.settings) } label: {
                            Label("Speak", systemImage: "speaker.wave.2.fill")
                        }
                        Button { player.stop() } label: {
                            Label("Stop", systemImage: "stop.fill")
                        }
                    }
                    if !player.stats.isEmpty {
                        Text(player.stats).font(.footnote).monospacedDigit().foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Try It")
                } footer: {
                    Text("Speaks straight from the engine, so it works before the system has picked the voice up.")
                }
                Section {
                    Toggle("Use as a system voice", isOn: Binding(
                        get: { !library.isHidden(item.id) },
                        set: { library.setHidden(item.id, !$0) }))
                } footer: {
                    Text("Off hides this voice from Settings, VoiceOver and Spoken Content. It stays downloaded.")
                }
                Section {
                    OverridePicker("Pronunciation rules", general: library.settings.rules,
                                   value: library.settings.rulesOverride[item.id]) { v in
                        library.updateSettings { $0.rulesOverride[item.id] = v }
                    }
                    if item.kind != .macinTalk {
                        OverridePicker("Newer pronunciation rules", general: library.settings.newerRules,
                                       value: library.settings.newerRulesOverride[item.id]) { v in
                            library.updateSettings { $0.newerRulesOverride[item.id] = v }
                        }
                        Text(PronunciationDownloadsView.explanation).font(.footnote).foregroundStyle(.secondary)
                        if library.settings.newerRulesEnabled(item.id) { PronunciationDownloadsView() }
                    }
                    if let compact = library.compactCounterpart(item) {
                        OverridePicker("Compact voice for pitch changes", general: library.settings.pitchFallback,
                                       value: library.settings.pitchFallbackOverride[item.id]) { v in
                            library.updateSettings { $0.pitchFallbackOverride[item.id] = v }
                        }
                        Text("Pitch changes speak with \(compact.name) (\(compact.kindName)).")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                    if item.kind == .macinTalk {
                        OverridePicker("Speech commands last one utterance", general: library.settings.resetCommands,
                                       value: library.settings.resetCommandsOverride[item.id]) { v in
                            library.updateSettings { $0.resetCommandsOverride[item.id] = v }
                        }
                    }
                } header: {
                    Text("For This Voice")
                } footer: {
                    Text("\u{201c}Default\u{201d} follows the Settings tab.")
                }
                if case .installed(let n) = library.state(item) {
                    Section {
                        Button(role: .destructive) { confirmingRemoval = true } label: {
                            Label("Remove Voice", systemImage: "trash")
                        }
                    } footer: {
                        let mates = library.packageMates(item)
                        Text("Frees \(VoiceFormatting.size(n)). You can download it again."
                             + (mates.isEmpty ? "" : " \(mates.joined(separator: ", ")) came in the same download and goes too."))
                    }
                    .confirmationDialog("Remove \(item.name)?", isPresented: $confirmingRemoval, titleVisibility: .visible) {
                        Button("Remove", role: .destructive) { library.remove(item) }
                        Button("Cancel", role: .cancel) {}
                    } message: {
                        Text("Anything set to this voice falls back to a system voice until you download it again.")
                    }
                }
            }
            .formStyle(.grouped)
            .navigationTitle(item.name)
            .onAppear {
                if text.isEmpty { text = item.greeting }
                player.stats = ""
            }
        } else {
            ContentUnavailableView("This voice isn\u{2019}t on this device", systemImage: "speaker.slash")
        }
    }

    /// Voices whose quirks are original, kept as they shipped.
    static func note(_ key: String) -> String? {
        let lion = "Lion\u{2019}s Australian dictionary is broken: it drops the vowel in words like car, park and hot, and leaves an American r on words like here and door. Lion spoke this way too, so it\u{2019}s kept."
        switch key {
        case "mac107/LeeCompact":
            return lion + " Mountain Lion Lee has byte-for-byte identical recordings with a working dictionary."
        case "mac107/KarenCompact":
            return lion + " iOS 5 Karen has byte-for-byte identical recordings with a working dictionary. Mountain Lion Karen is a newer recording, the same one as iOS 6."
        default:
            return nil
        }
    }
}


/// A search box that reads well to VoiceOver on both platforms.
struct SearchField: View {
    @Binding var text: String

    var body: some View {
        HStack {
            Image(systemName: "magnifyingglass").foregroundStyle(.secondary).accessibilityHidden(true)
            TextField("Search all voices", text: $text)
                .textFieldStyle(.plain)
                .autocorrectionDisabled()
                .accessibilityLabel("Search all voices")
                .accessibilityHint("Finds voices by name, language, version or type, in every version of iOS and macOS")
            if !text.isEmpty {
                Button { text = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary) }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Clear search")
            }
        }
        #if os(macOS)
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 6).fill(Color(nsColor: .textBackgroundColor)))
        .frame(width: 440)
        #endif
    }
}

/// Default / On / Off for one voice, where Default is the general setting.
struct OverridePicker: View {
    let title: String
    let general: Bool
    let value: Bool?
    let set: (Bool?) -> Void

    init(_ title: String, general: Bool, value: Bool?, set: @escaping (Bool?) -> Void) {
        (self.title, self.general, self.value, self.set) = (title, general, value, set)
    }

    var body: some View {
        Picker(title, selection: Binding(get: { value.map { $0 ? 1 : 0 } ?? -1 },
                                         set: { set($0 < 0 ? nil : $0 == 1) })) {
            Text("Default (\(general ? "on" : "off"))").tag(-1)
            Text("On").tag(1)
            Text("Off").tag(0)
        }
    }
}

/// Where the newer pronunciation packs stand: downloading (with its
/// progress as the value VoiceOver reads), failed (with Try Again), or
/// nothing once they're here.
struct PronunciationDownloadsView: View {
    static let explanation = "Apple\u{2019}s later pronunciation dictionary and rules, added to the voice\u{2019}s own."
    @Environment(VoiceLibrary.self) private var library

    var body: some View {
        let d = library.pronunciation
        if let p = d.progress {
            HStack {
                ProgressView(value: p) { Text("Downloading the pronunciation rules") }
                    .accessibilityValue("\(VoiceFormatting.size(d.bytes.written)) of \(VoiceFormatting.size(d.bytes.total))")
                Button("Stop") { d.cancel() }
            }
        } else if let e = d.error {
            Text("The pronunciation rules couldn\u{2019}t be downloaded: \(e)").foregroundStyle(.secondary)
            Button("Try Again") { d.download() }
        } else if !d.installed {
            Button("Download the Pronunciation Rules") { d.download() }
        }
    }
}

/// The Settings tab.
struct SettingsTab: View {
    var body: some View {
        #if os(macOS)
        SpeechSettingsView()
        #else
        NavigationStack { SpeechSettingsView() }
        #endif
    }
}

/// The speech settings, for every voice unless a voice says otherwise.
struct SpeechSettingsView: View {
    @Environment(VoiceLibrary.self) private var library

    var body: some View {
        Form {
            Section {
                Toggle("Apple\u{2019}s pronunciation rules", isOn: Binding(
                    get: { library.settings.rules },
                    set: { v in library.updateSettings { $0.rules = v } }))
            } footer: {
                Text("The text rules each voice\u{2019}s own iOS release or Mac OS X voice applied before speaking: \u{201c}iOS\u{201d} is spelled out, \u{201c}OS X\u{201d} is \u{201c}OS ten\u{201d}, St and Dr become Street and Drive, and so on. Each voice can override this on its own page.")
            }
            Section {
                Toggle("Newer pronunciation rules", isOn: Binding(
                    get: { library.settings.newerRules },
                    set: { v in library.updateSettings { $0.newerRules = v } }))
                if library.settings.newerRulesWanted { PronunciationDownloadsView() }
            } footer: {
                Text(PronunciationDownloadsView.explanation + " Off, each voice speaks as its own release did. Not for MacinTalk voices. Each voice can override this on its own page.")
            }
            Section {
                Toggle("Compact voice for pitch changes", isOn: Binding(
                    get: { library.settings.pitchFallback },
                    set: { v in library.updateSettings { $0.pitchFallback = v } }))
            } footer: {
                Text("Enhanced and premium voices can\u{2019}t change pitch. As VoiceOver did on iOS 5 and 6, anything spoken at a changed pitch (capital letters, the pitch rotor) uses the voice\u{2019}s compact version, which stays loaded. Voices without a compact version here are unaffected.")
            }
            Section {
                Picker("Speaking rate", selection: Binding(
                    get: { library.settings.modernRate },
                    set: { v in library.updateSettings { $0.modernRate = v } })) {
                    Text("Same as today\u{2019}s Alex").tag(true)
                    Text("Classic").tag(false)
                }
            } header: {
                Text("MacinTalk")
            } footer: {
                Text("Same as today\u{2019}s Alex: each speech rate setting speaks at the words per minute the current Alex uses for it, from about 70 to about 1000. Classic: a percent of each voice\u{2019}s own rate, from half to four times (Alex: 88 to 700 words per minute), as Classic Voices did before.")
            }
            Section {
                Toggle("Speech commands last one utterance", isOn: Binding(
                    get: { library.settings.resetCommands },
                    set: { v in library.updateSettings { $0.resetCommands = v } }))
            } footer: {
                Text("Embedded commands such as [[inpt PHON]], [[rate 300]] or [[char LTRL]] change a MacinTalk voice until something changes them back, so a command VoiceOver reads out once could leave the voice in that mode. With this on, each utterance starts from the voice\u{2019}s normal settings. Turn it off to keep a command in effect across utterances. Values past what the engine takes (rate above 32767, pbas above 127, pmod above 200, volm above 2) are set to the highest it takes.")
            }
            Section {
                Toggle("24-hour times in Leopard voices", isOn: Binding(
                    get: { library.settings.twentyFourHour },
                    set: { v in library.updateSettings { $0.twentyFourHour = v } }))
            } footer: {
                Text("Leopard reads a time such as 13:45 as \u{201c}one forty-five PM\u{201d}. With this on, its voices read 24-hour times the way Tiger, Snow Leopard and Lion do: \u{201c}thirteen forty-five\u{201d}, \u{201c}eighteen oh five\u{201d}.")
            }
        }
        .formStyle(.grouped)
        .navigationTitle("Settings")
    }
}
