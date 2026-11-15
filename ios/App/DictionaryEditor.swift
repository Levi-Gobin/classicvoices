import SwiftUI

/// The Dictionaries tab: the pronunciation dictionaries' entries, the community's and the person's own
/// (DictionaryStore), as the NVDA add-on's dictionary dialog shows them, as a drill-down of buttons like the voice
/// browser: files, a file's entries, one entry, the voice's sounds to insert. While the tab is open the voices use
/// no dictionary (CommunityDictionary.editing), so a word is heard as the voice reads it by itself; Preview says the
/// test text with just the entry being edited, in a voice played here in the app.
enum DictRoute: Hashable {
    case file(String)
    case entry(String, String?)  // file, entry key (nil: a new one)
    case phonemes
    case source
}

/// What the entry page edits; the sound list writes into it.
@MainActor @Observable
final class EntryDraft {
    var pattern = ""
    var replacement = ""
    var test = ""
    var caseSensitive = false
    var type = 2
    var voiceKey = ""
    var language = ""
    var loadedFor: DictRoute?
    var file = ""
    var savedKey: String?                   // the entry as it is on disk now (nil: not written yet)
    var lastSaved: DictionaryStore.Entry?
    var picking = false                     // on the sound list: the draft stays as it is

    var entry: DictionaryStore.Entry {
        DictionaryStore.Entry(pattern: pattern, replacement: replacement.replacingOccurrences(of: "\t", with: " "),
                              caseSensitive: caseSensitive, type: type)
    }

    func load(_ e: DictionaryStore.Entry?, for route: DictRoute) {
        guard loadedFor != route else { return }
        loadedFor = route
        if case .entry(let f, let k) = route { file = f; savedKey = k }
        lastSaved = e
        pattern = e?.pattern ?? ""; replacement = e?.replacement ?? ""; caseSensitive = e?.caseSensitive ?? false
        type = e?.type ?? 2; test = e.map { $0.type == 1 ? "" : $0.pattern } ?? ""
    }

    /// Why the entry can't be saved as it is, or nil.
    func problem(emptyOK: Bool) -> String? { Self.problem(entry, emptyOK: emptyOK) }

    /// Writes the entry as it is now, if it can be saved and changed (edits save themselves: a moment after the
    /// last change, when the page closes and when the app quits). An empty replacement waits for Save, which asks.
    @discardableResult
    func autosave(emptyOK: Bool = false) -> Bool {
        guard !file.isEmpty, loadedFor != nil, problem(emptyOK: emptyOK) == nil else { return false }
        let e = entry
        if e == lastSaved { return true }
        DictionaryStore.put(file, e, oldKey: savedKey)
        savedKey = e.key
        lastSaved = e
        return true
    }

    /// A sound into the replacement: into the phoneme span it ends with, else as a new span.
    func insert(_ xsampa: String) { replacement = Self.inserting(xsampa, into: replacement) }

    static func inserting(_ xsampa: String, into replacement: String) -> String {
        let spans = replacement.components(separatedBy: "~~").count - 1
        if replacement.hasSuffix("~~"), spans % 2 == 0, spans > 0 { return String(replacement.dropLast(2)) + xsampa + "~~" }
        if spans % 2 == 1 { return replacement + xsampa }
        return replacement + "~~" + xsampa + "~~"
    }

    /// A replacement with its words as phonemes (~~X-SAMPA~~, as `convert` gives them; nil: the word stays). Phonemes
    /// already there are kept, and so, in a regular expression's replacement, are its groups (\1, \g<name>).
    static func toPhonemes(_ s: String, regex: Bool, convert: (String) -> String?) -> (text: String, done: Int, left: Int) {
        let c = Array(s)
        var out = "", i = 0, done = 0, left = 0
        func isLetter(_ ch: Character) -> Bool { ch.isLetter }
        while i < c.count {
            if c[i] == "~", i + 1 < c.count, c[i + 1] == "~", !(i + 2 < c.count && c[i + 2] == "~") {
                var j = i + 2  // the span's close: the first run of two or more tildes
                while j + 1 < c.count, !(c[j] == "~" && c[j + 1] == "~") { j += 1 }
                var end = j + 1 < c.count ? j + 2 : c.count
                while end < c.count, c[end] == "~" { end += 1 }
                out += String(c[i..<end]); i = end
                continue
            }
            if regex, c[i] == "\\" {
                var j = i + 1
                if j < c.count, c[j].isNumber { while j < c.count, c[j].isNumber { j += 1 } }
                else if j < c.count, c[j] == "g", j + 1 < c.count, c[j + 1] == "<", let gt = c[j...].firstIndex(of: ">") { j = gt + 1 }
                else { j = min(i + 2, c.count) }
                out += String(c[i..<j]); i = j
                continue
            }
            if isLetter(c[i]) {
                var j = i
                while j < c.count, isLetter(c[j]) || (c[j] == "'" && j + 1 < c.count && isLetter(c[j + 1])) { j += 1 }
                let word = String(c[i..<j])
                if let x = convert(word) { out += "~~\(x)~~"; done += 1 } else { out += word; left += 1 }
                i = j
                continue
            }
            out.append(c[i]); i += 1
        }
        return (out, done, left)
    }

    /// Why an entry can't be saved, nil, or "empty" (an empty replacement: asked about first).
    static func problem(_ e: DictionaryStore.Entry, emptyOK: Bool) -> String? {
        if e.pattern.isEmpty { return "Type the pattern to replace." }
        if e.replacement.isEmpty && !emptyOK { return "empty" }
        let spans = e.replacement.components(separatedBy: "~~").count - 1
        if spans % 2 == 1 && !e.replacement.hasSuffix("~~~") { return "A phoneme span isn’t closed: end it with ~~." }
        if e.type == 1, (try? NSRegularExpression(pattern: e.pattern)) == nil { return "That regular expression doesn’t work." }
        return nil
    }
}

struct DictionariesTab: View {
    @Environment(VoiceLibrary.self) private var library
    @State private var path: [DictRoute] = []
    @State private var draft = EntryDraft()
    @State private var refresh = 0  // bumped after a change, so the pages read the files again
    @Environment(\.scenePhase) private var phase

    private func open(_ r: DictRoute) { path.append(r) }
    private func back() { if !path.isEmpty { path.removeLast() } }

    var body: some View {
        #if os(macOS)
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 12) {
                Button("Back") { back() }
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
            page(path.last).id("\(path.count)-\(refresh)")
        }
        .onChange(of: path) { _, new in
            AccessibilityNotification.Announcement(new.last.map(title) ?? "Dictionaries").post()
        }
        .task(id: phase) { if phase == .active { await editing() } }
        .onAppear { Self.isOpen = true }
        .onDisappear { Self.isOpen = false; DictionaryPublisher.shared.leftEditor() }
        #if os(macOS)
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in draft.autosave() }
        #endif
        #else
        NavigationStack(path: $path) {
            page(nil).navigationTitle("Dictionaries")
                .navigationDestination(for: DictRoute.self) { page($0).navigationTitle(title($0)) }
        }
        .task { await editing() }
        #endif
    }

    private func editing() async { await Self.holdEditingFlag() }

    /// Whether the tab is showing (the Mac asks about publishing when quitting from it).
    @MainActor static var isOpen = false

    @MainActor static var publishingDetail: String {
        let p = DictionaryPublisher.shared
        let how = DictionaryPublisher.usesGit ? "with git" : (p.login.map { "as \($0) on GitHub" } ?? "not signed in to GitHub")
        let n = DictionaryChanges.pending().count
        return (n == 0 ? "nothing to publish" : "changes in \(n) file\(n == 1 ? "" : "s") not published yet") + ", " + how
    }

    /// While the tab is open and the app is in front: the flag the speech extension checks, renewed every 15
    /// seconds; removed when the tab closes, the app goes to the back or its window closes, and when it quits.
    static func holdEditingFlag() async {
        guard let flag = Self.editingFlag else { return }
        defer { try? FileManager.default.removeItem(at: flag) }
        while !Task.isCancelled {
            try? Data().write(to: flag)
            try? await Task.sleep(for: .seconds(15))
        }
    }

    static var editingFlag: URL? { CommunityDictionary.folder?.appendingPathComponent("editing") }

    private var crumbs: [String] { ["Dictionaries"] + path.map(title) }

    private func title(_ r: DictRoute) -> String {
        switch r {
        case .file(let f): Self.fileTitle(f)
        case .entry(_, let key): key == nil ? "New entry" : (draft.pattern.isEmpty ? "Entry" : draft.pattern)
        case .phonemes: "Insert Phoneme"
        case .source: "Source"
        }
    }

    static func fileLanguage(_ file: String) -> String {
        let base = file.hasSuffix(".dic") ? String(file.dropLast(4)) : file
        let parts = base.split(separator: "-")
        return parts.count == 2 ? "\(parts[0])-\(parts[1].uppercased())" : base
    }

    static func fileTitle(_ file: String) -> String {
        if file == "macintalk.dic" { return "MacinTalk (macintalk.dic)" }
        return "\(VoiceItem.languageName(fileLanguage(file))) (\(file))"
    }

    @ViewBuilder private func page(_ r: DictRoute?) -> some View {
        switch r {
        case nil: rootPage
        case .file(let f)?: filePage(f)
        case .entry(let f, let key)?: EntryPage(file: f, key: key, draft: draft, voices: voices(for: f), open: open,
                                               done: { refresh += 1; back() })
        case .phonemes?: PhonemePage(draft: draft, done: back)
        case .source?: SourcePage(done: { refresh += 1; back() })
        }
    }

    /// Voices this app can play itself for a file's language (its own built-in ones; downloaded voices live in
    /// the speech extension's store).
    private func voices(for file: String) -> [VoiceItem] { Self.voices(for: file, in: library) }

    /// Voices the app can play itself for a file's language (its own built-in ones; downloaded voices live in the
    /// speech extension's store).
    static func voices(for file: String, in library: VoiceLibrary) -> [VoiceItem] {
        let lang = Self.fileLanguage(file).lowercased()
        return library.items.filter { item in
            guard let key = item.entry?.key, DirectPlayer.shared.canTryOut(key) else { return false }
            if file == "macintalk.dic" { return key.hasPrefix("mt") }
            let l = item.language.replacingOccurrences(of: "_", with: "-").lowercased()
            return !key.hasPrefix("mt") && (l == lang || l.hasPrefix(lang + "-"))
        }
    }

    private var rootPage: some View {
        let files = Self.files(library)
        let src = CommunityDictionary.source
        return Page {
            PageSection(nil) {
                Text("While this tab is open the voices use no dictionary, so you hear words as they read them by themselves. Your own entries work even with the community dictionaries turned off (Settings).")
                    .fixedSize(horizontal: false, vertical: true)
            }
            PageSection(nil) {
                OpenButton(title: "Publishing", detail: Self.publishingDetail) { DictionaryPublisher.shared.open() }
            }
            PageSection("Where they come from") {
                OpenButton(title: "Source", detail: "github.com/\(src.repo), branch \(src.branch)") { open(.source) }
                FetchDictionariesButton()
            }
            PageSection("Dictionaries") {
                ForEach(files, id: \.self) { f in
                    let rows = DictionaryStore.rows(f)
                    let yours = rows.filter { $0.origin != .community }.count
                    OpenButton(title: Self.fileTitle(f),
                               detail: "\(rows.count) entries" + (yours > 0 ? ", \(yours) changed by you" : "")) { open(.file(f)) }
                }
            }
        }
    }

    /// The voice's language files first, then every other file there is.
    static func files(_ library: VoiceLibrary) -> [String] {
        var out: [String] = []
        let langs = Set(library.items.filter { $0.isInstalled && !($0.entry?.key.hasPrefix("mt") ?? true) }.map {
            $0.language.replacingOccurrences(of: "_", with: "-").lowercased()
        })
        for l in langs.sorted() {
            let base = l.split(separator: "-").first.map(String.init) ?? l
            for f in [base + ".dic", l + ".dic"] where !out.contains(f) { out.append(f) }
        }
        if library.items.contains(where: { $0.isInstalled && ($0.entry?.key.hasPrefix("mt") ?? false) }) { out.append("macintalk.dic") }
        let existing = DictionaryStore.fileNames()
        // files that exist first, then the empty ones a voice could use
        return existing + out.filter { !existing.contains($0) }
    }

    private func filePage(_ file: String) -> some View {
        let rows = DictionaryStore.rows(file)
        return Page {
            PageSection(nil) {
                OpenButton(title: "Add an entry") { open(.entry(file, nil)) }
            }
            PageSection(rows.isEmpty ? "No entries yet" : "Entries") {
                ForEach(rows) { row in
                    OpenButton(title: row.entry.pattern,
                               detail: "\(row.entry.replacement) · \(Self.typeName(row.entry.type))\(row.entry.caseSensitive ? ", case sensitive" : "") · \(Self.whose(row.origin))") {
                        open(.entry(file, row.entry.key))
                    }
                }
            }
        }
    }

    static func typeName(_ t: Int) -> String { t == 1 ? "regular expression" : t == 2 ? "whole word" : "anywhere" }

    static func whose(_ o: DictionaryStore.Origin) -> String {
        switch o {
        case .community: "community"
        case .added: "yours"
        case .changed: "yours, changed"
        case .removed: "removed by you"
        }
    }
}

/// One entry: pattern, replacement (Insert Phoneme), case, kind, test text and Preview; Save, Remove, Restore.
private struct EntryPage: View {
    let file: String
    let key: String?
    @Bindable var draft: EntryDraft
    let voices: [VoiceItem]
    let open: (DictRoute) -> Void
    let done: () -> Void
    @Environment(VoiceLibrary.self) private var library
    @State private var problem: String?
    @State private var confirmEmpty = false

    @State private var pendingSave: Task<Void, Never>?

    /// The entry as it's on disk (after its edits saved themselves, under its new pattern).
    private var row: DictionaryStore.Row? {
        (draft.loadedFor == .entry(file, key) ? draft.savedKey : key).flatMap { k in DictionaryStore.rows(file).first { $0.entry.key == k } }
    }

    var body: some View {
        Page {
            PageSection(nil) {
                TextField("Pattern", text: $draft.pattern)
                TextField("Replacement (sounds go between ~~ and ~~)", text: $draft.replacement)
                OpenButton(title: "Insert Phoneme…", detail: voices.isEmpty ? "needs a voice of this language" : nil) {
                    draft.picking = true
                    open(.phonemes)
                }
                .disabled(voices.isEmpty)
                Button("Convert to Phonemes") { convert() }
                    .disabled(voices.isEmpty || draft.voiceKey.hasPrefix("mt"))
                Toggle("Case sensitive", isOn: $draft.caseSensitive)
                Picker("Type", selection: $draft.type) {
                    Text("Whole word").tag(2)
                    Text("Anywhere").tag(0)
                    Text("Regular expression").tag(1)
                }
            }
            PageSection("Try it") {
                TextField("Test text", text: $draft.test)
                if voices.count > 1 {
                    Picker("Voice", selection: $draft.voiceKey) {
                        ForEach(voices) { v in Text("\(v.name), \(v.version.menuName)").tag(v.entry?.key ?? "") }
                    }
                }
                // (without the entry: VoiceOver reads it so, the voices using no dictionary while this tab is open)
                Button("Preview") { hear(withEntry: true) }.disabled(voices.isEmpty)
                if voices.isEmpty {
                    Text("No voice of this language is built into the app to try it with.").foregroundStyle(.secondary)
                }
            }
            PageSection(nil) {
                if let problem { Text(problem).foregroundStyle(.red) }
                Button(key == nil ? "Add" : "Save") { save() }.keyboardShortcut(.defaultAction)
                    .confirmationDialog("The replacement is empty", isPresented: $confirmEmpty) {
                        Button("Save Anyway") { save(emptyOK: true) }
                        Button("Cancel", role: .cancel) {}
                    } message: {
                        Text("The voices will skip “\(draft.pattern)” entirely. That suits things like rows of box-drawing characters, not words.")
                    }
                if let row, row.origin != .removed {
                    Button("Remove", role: .destructive) {
                        pendingSave?.cancel()
                        draft.loadedFor = nil  // nothing more to save
                        DictionaryStore.remove(file, key: row.entry.key)
                        AccessibilityNotification.Announcement("Removed").post()
                        done()
                    }
                }
                if let row, row.origin == .changed || row.origin == .removed {
                    Button("Restore the Community’s") {
                        pendingSave?.cancel()
                        draft.loadedFor = nil
                        DictionaryStore.restore(file, key: row.entry.key)
                        AccessibilityNotification.Announcement("Restored").post()
                        done()
                    }
                }
            }
        }
        .onChange(of: draft.entry) { _, _ in
            pendingSave?.cancel()
            pendingSave = Task {
                try? await Task.sleep(for: .milliseconds(600))
                if !Task.isCancelled { draft.autosave() }
            }
        }
        .onDisappear {
            pendingSave?.cancel()
            draft.autosave()
            // closed (not off to the sound list): opened again, it's read from the file
            if !draft.picking { draft.loadedFor = nil }
        }
        .onAppear {
            draft.picking = false
            draft.load(key.flatMap { k in DictionaryStore.rows(file).first { $0.entry.key == k } }?.entry, for: .entry(file, key))
            if !voices.contains(where: { $0.entry?.key == draft.voiceKey }) { draft.voiceKey = voices.first?.entry?.key ?? "" }
            if let v = voices.first(where: { $0.entry?.key == draft.voiceKey }) { draft.language = v.language }
        }
        .onChange(of: draft.voiceKey) { _, k in
            if let v = voices.first(where: { $0.entry?.key == k }) { draft.language = v.language }
        }
    }

    /// The replacement's words as phonemes, as the voice says them (no stress marks: added by hand where needed).
    private func convert() {
        let key = draft.voiceKey, lang = draft.language
        let r = EntryDraft.toPhonemes(draft.replacement, regex: draft.type == 1) {
            VoiceEngine.xsampa(word: $0, voiceKey: key, language: lang)
        }
        draft.replacement = r.text
        AccessibilityNotification.Announcement(r.done == 0 ? "Nothing to convert"
            : r.left == 0 ? "Converted to phonemes. Add stress marks where needed."
            : "Converted \(r.done) words; \(r.left) stayed as they were").post()
    }

    private func hear(withEntry: Bool) {
        guard let v = voices.first(where: { $0.entry?.key == draft.voiceKey }), let key = v.entry?.key else { return }
        let text = draft.test.isEmpty ? draft.pattern : draft.test
        guard !text.isEmpty else { return }
        let e = draft.entry
        DirectPlayer.shared.tryOut(text, entries: withEntry && !e.pattern.isEmpty ? e.line : "", voiceKey: key,
                                   language: v.language, rules: library.rules(for: v), rulesPack: library.rulesPack(for: v),
                                   settings: library.settings)
    }

    private func save(emptyOK: Bool = false) {
        pendingSave?.cancel()
        if let p = draft.problem(emptyOK: emptyOK) {
            if p == "empty" { confirmEmpty = true } else { problem = p }
            return
        }
        draft.autosave(emptyOK: emptyOK)
        AccessibilityNotification.Announcement(key == nil ? "Added" : "Saved").post()
        draft.loadedFor = nil
        done()
    }
}

/// The voice's sounds, each with an example word from phonemes/<language>.txt: pressing one inserts it; Hear
/// (also a VoiceOver action on the row) says the sound and its word.
struct PhonemePage: View {
    @Bindable var draft: EntryDraft
    let done: () -> Void
    @Environment(VoiceLibrary.self) private var library
    @State private var items: [(xsampa: String, example: String, heard: String)] = []

    /// Sounds offered when the repository has no phonemes/<language>.txt yet (no example words then).
    static let generic = ("i y I Y e 2 E 9 { a A Q O o U u V @ 3 6 1 } M 7 8 & a~ E~ O~ 9~ o~ "
        + "p b t d k g q G\\ ? c J\\ m n N J F f v T D s z S Z x G h X R C j\\ s\\ z\\ tS dZ ts dz "
        + "l L 5 r r\\ 4 R j w H").split(separator: " ").map(String.init)

    var body: some View {
        Page {
            PageSection(items.isEmpty ? "No sounds" : "Sounds of this voice") {
                ForEach(items, id: \.xsampa) { item in
                    let label = item.example.isEmpty ? item.xsampa : "\(item.xsampa), as in \(item.example)"
                    HStack {
                        OpenButton(title: label, opens: false) {
                            draft.insert(item.xsampa)
                            AccessibilityNotification.Announcement("Inserted \(item.xsampa)").post()
                            done()
                        }
                        .accessibilityAction(named: "Hear") { hear(item) }
                        Button("Hear") { hear(item) }.accessibilityHidden(true)
                    }
                }
            }
        }
        .onAppear { items = sounds() }
    }

    private func sounds() -> [(xsampa: String, example: String, heard: String)] {
        Self.sounds(key: draft.voiceKey, language: draft.language)
    }

    /// The voice's sounds: the language's phonemes/<lang>.txt in order (or a general list), each the voice has
    /// once, with its example word and the engine text that says it.
    static func sounds(key: String, language lang: String) -> [(xsampa: String, example: String, heard: String)] {
        guard !key.isEmpty else { return [] }
        let listed = DictionaryStore.phonemeExamples(language: lang)
        var out: [(xsampa: String, example: String, heard: String)] = [], seen = Set<String>()
        for (x, ex) in listed.isEmpty ? Self.generic.map({ ($0, "") }) : listed {
            guard let ipa = VoiceEngine.ipa(xsampa: x),
                  let own = VoiceEngine.phonemeText(key: key, language: lang, ipa: ipa), !seen.contains(own) else { continue }
            seen.insert(own)  // the voice says it as one already in the list
            // a consonant is heard between two a's: alone, some voices say nothing
            let heard = VoiceEngine.hasVowel(ipa: ipa) ? own : (VoiceEngine.phonemeText(key: key, language: lang, ipa: "a" + ipa + "a") ?? own)
            out.append((x, ex, heard))
        }
        return out
    }

    private func hear(_ item: (xsampa: String, example: String, heard: String)) {
        Self.hear(item, voiceKey: draft.voiceKey, library: library)
    }

    /// Says a sound and its example word.
    static func hear(_ item: (xsampa: String, example: String, heard: String), voiceKey: String, library: VoiceLibrary) {
        guard let v = library.items.first(where: { $0.entry?.key == voiceKey }) else { return }
        let word = item.example.replacingOccurrences(of: "\u{1b}", with: " ")
        DirectPlayer.shared.tryOut("", engineText: item.heard + (word.isEmpty ? "" : "  " + word), voiceKey: voiceKey,
                                   language: v.language, rules: nil, rulesPack: nil, settings: library.settings)
    }
}

/// The GitHub repository the community dictionaries come from (a fork, say).
struct SourcePage: View {
    let done: () -> Void
    @State private var repo = CommunityDictionary.source.repo
    @State private var branch = CommunityDictionary.source.branch
    @State private var problem: String?

    var body: some View {
        Page {
            PageSection(nil) {
                TextField("GitHub repository (owner/name or its address)", text: $repo)
                TextField("Branch", text: $branch)
                if let problem { Text(problem).foregroundStyle(.red) }
                Button("Save") {
                    guard let r = CommunityDictionary.parseRepo(repo) else {
                        problem = "Type the repository as owner/name, for example nicopn/classicvoices-dicts."
                        return
                    }
                    let b = branch.trimmingCharacters(in: .whitespaces)
                    if (r, b.isEmpty ? CommunityDictionary.defaultBranch : b) != CommunityDictionary.source {
                        CommunityDictionary.setSource(repo: r, branch: b)
                        Task {
                            let ok = await CommunityDictionary.fetchAll(force: true)
                            AccessibilityNotification.Announcement(ok ? "Dictionaries fetched" : "The dictionaries couldn’t be fetched from there").post()
                        }
                    }
                    done()
                }
                .keyboardShortcut(.defaultAction)
                Button("Use the Default") {
                    repo = CommunityDictionary.defaultRepo
                    branch = CommunityDictionary.defaultBranch
                }
            }
        }
    }
}

