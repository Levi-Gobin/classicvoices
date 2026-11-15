#if os(iOS)
import SwiftUI

/// The iPhone and iPad Dictionaries tab (Nico's design, 2026-11-14): the desktop editors' conventions in iOS form.
/// A file picker and a searchable list of entries; double-tap edits an entry, and Preview, Remove and Restore the
/// community's are VoiceOver actions (swipe actions too). The entry sheet has the NVDA dialog's order: pattern,
/// replacement with Insert Phoneme, case, type, then a test text and Preview; Cancel and Save. While the tab is
/// open the voices use no dictionary (CommunityDictionary.editing), so plain reading is the "without" preview.
struct MobileDictionariesTab: View {
    @Environment(VoiceLibrary.self) private var library
    @Environment(\.scenePhase) private var phase
    @AppStorage("dictionaries.file") private var chosenFile = ""
    @State private var query = ""
    @State private var rows: [DictionaryStore.Row] = []
    @State private var editing: EditTarget?
    @State private var showSource = false
    @State private var fetching = false

    struct EditTarget: Identifiable {
        let id = UUID()
        let row: DictionaryStore.Row?
    }

    private var files: [String] { DictionariesTab.files(library) }
    private var file: String { files.contains(chosenFile) ? chosenFile : (files.first ?? "en.dic") }
    private var voices: [VoiceItem] { DictionariesTab.voices(for: file, in: library) }

    private var shown: [DictionaryStore.Row] {
        let q = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !q.isEmpty else { return rows }
        return rows.filter { $0.entry.pattern.lowercased().contains(q) || $0.entry.replacement.lowercased().contains(q) }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Dictionary", selection: Binding(get: { file }, set: { chosenFile = $0; reload() })) {
                        ForEach(files, id: \.self) { Text(DictionariesTab.fileTitle($0)).tag($0) }
                    }
                } footer: {
                    Text("While this tab is open the voices use no dictionary, so you hear words as they read them by themselves. Your own entries work even with the community dictionaries off.")
                }
                Section(shown.isEmpty ? (query.isEmpty ? "No entries yet" : "No matches") : "Entries") {
                    ForEach(shown) { row in
                        MobileEntryRow(row: row, canPreview: !voices.isEmpty && row.entry.type != 1,
                                       edit: { editing = EditTarget(row: row) },
                                       preview: { preview(row.entry) },
                                       remove: { change("Removed") { DictionaryStore.remove(file, key: row.entry.key) } },
                                       restore: { change("Restored") { DictionaryStore.restore(file, key: row.entry.key) } })
                    }
                }
            }
            .searchable(text: $query, prompt: "Patterns and replacements")
            .navigationTitle("Dictionaries")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Menu {
                        Button(fetching ? "Downloading…" : "Download the Latest Dictionaries") { fetch() }.disabled(fetching)
                        Button("Source…") { showSource = true }
                    } label: { Label("More", systemImage: "ellipsis.circle") }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button { editing = EditTarget(row: nil) } label: { Label("Add Entry", systemImage: "plus") }
                }
            }
            .sheet(item: $editing) { target in
                MobileEntrySheet(file: file, row: target.row, voices: voices) { reload() }
                    .environment(library)
            }
            .sheet(isPresented: $showSource, onDismiss: reload) {
                NavigationStack {
                    SourcePage(done: { showSource = false })
                        .navigationTitle("Source")
                        .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { showSource = false } } }
                }
            }
        }
        .onAppear(perform: reload)
        .task(id: phase) { if phase == .active { await DictionariesTab.holdEditingFlag() } }
    }

    private func reload() { rows = DictionaryStore.rows(file) }

    private func change(_ said: String, _ work: () -> Void) {
        work()
        reload()
        AccessibilityNotification.Announcement(said).post()
    }

    private func preview(_ e: DictionaryStore.Entry) {
        guard let v = voices.first, let key = v.entry?.key else { return }
        DirectPlayer.shared.tryOut(e.pattern, entries: e.line, voiceKey: key, language: v.language,
                                   rules: library.rules(for: v), rulesPack: library.rulesPack(for: v), settings: library.settings)
    }

    private func fetch() {
        fetching = true
        Task {
            let ok = await CommunityDictionary.fetchAll(force: true)
            fetching = false
            reload()
            AccessibilityNotification.Announcement(ok ? "The dictionaries are up to date"
                                                      : "The dictionaries couldn’t be downloaded").post()
        }
    }
}

/// One entry: double-tap edits; Preview, Remove and Restore the community's as actions.
private struct MobileEntryRow: View {
    let row: DictionaryStore.Row
    let canPreview: Bool
    let edit: () -> Void
    let preview: () -> Void
    let remove: () -> Void
    let restore: () -> Void

    private var replacement: String {
        let r = row.entry.replacement
        if r.isEmpty { return "nothing" }
        return r.contains("~~") ? "phonemes " + r.replacingOccurrences(of: "~~", with: "") : r
    }

    var body: some View {
        Button(action: edit) {
            VStack(alignment: .leading, spacing: 2) {
                Text(row.entry.pattern)
                Text("\(row.entry.replacement) · \(DictionariesTab.whose(row.origin))")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }
        .foregroundStyle(.primary)
        .swipeActions(edge: .trailing) {
            if row.origin != .removed { Button("Remove", role: .destructive, action: remove) }
            if row.origin == .changed || row.origin == .removed { Button("Restore", action: restore).tint(.green) }
        }
        .swipeActions(edge: .leading) {
            if canPreview { Button("Preview", action: preview).tint(.blue) }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(row.entry.pattern), replaced by \(replacement), \(DictionariesTab.whose(row.origin))")
        .accessibilityAddTraits(.isButton)
        .accessibilityActions {
            if canPreview { Button("Preview", action: preview) }
            if row.origin != .removed { Button("Remove", action: remove) }
            if row.origin == .changed || row.origin == .removed { Button("Restore the Community’s", action: restore) }
        }
    }
}

/// Adding or editing an entry.
private struct MobileEntrySheet: View {
    let file: String
    let row: DictionaryStore.Row?
    let voices: [VoiceItem]
    let saved: () -> Void
    @Environment(\.dismiss) private var dismiss
    @Environment(VoiceLibrary.self) private var library
    @State private var pattern = ""
    @State private var replacement = ""
    @State private var caseSensitive = false
    @State private var type = 2
    @State private var test = ""
    @State private var voiceKey = ""
    @State private var problem: String?
    @State private var confirmEmpty = false
    @State private var showPhonemes = false
    @State private var loaded = false

    init(file: String, row: DictionaryStore.Row?, voices: [VoiceItem], saved: @escaping () -> Void) {
        self.file = file; self.row = row; self.voices = voices; self.saved = saved
    }

    private var entry: DictionaryStore.Entry {
        DictionaryStore.Entry(pattern: pattern, replacement: replacement.replacingOccurrences(of: "\t", with: " "),
                              caseSensitive: caseSensitive, type: type)
    }

    private var voice: VoiceItem? { voices.first { $0.entry?.key == voiceKey } ?? voices.first }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Pattern", text: $pattern)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    TextField("Replacement", text: $replacement)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("Insert Phoneme…") { showPhonemes = true }.disabled(voices.isEmpty)
                } footer: {
                    Text("Phonemes go between ~~ and ~~, in X-SAMPA.")
                }
                Section {
                    Toggle("Case Sensitive", isOn: $caseSensitive)
                    Picker("Type", selection: $type) {
                        Text("Whole Word").tag(2)
                        Text("Anywhere").tag(0)
                        Text("Regular Expression").tag(1)
                    }
                }
                Section {
                    TextField("Test Text", text: $test)
                    if voices.count > 1 {
                        Picker("Voice", selection: $voiceKey) {
                            ForEach(voices) { v in Text("\(v.name), \(v.version.menuName)").tag(v.entry?.key ?? "") }
                        }
                    }
                    Button("Preview", action: preview).disabled(voices.isEmpty)
                } footer: {
                    Text(voices.isEmpty ? "No voice of this language is built into the app to preview with."
                                        : "Says the test text with this entry. Without it, any text is read as the voice says it, since the voices use no dictionary while this tab is open.")
                }
                if let problem {
                    Section { Text(problem).foregroundStyle(.red) }
                }
            }
            .navigationTitle(row == nil ? "Add Entry" : "Edit Entry")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) { Button("Save") { save(emptyOK: false) } }
            }
            .confirmationDialog("The replacement is empty", isPresented: $confirmEmpty, titleVisibility: .visible) {
                Button("Save Anyway") { save(emptyOK: true) }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("The voices will skip “\(pattern)” entirely. That suits things like rows of box-drawing characters, not words.")
            }
            .sheet(isPresented: $showPhonemes) {
                MobilePhonemeSheet(voiceKey: voice?.entry?.key ?? "", language: voice?.language ?? "") { x in
                    replacement = EntryDraft.inserting(x, into: replacement)
                }
                .environment(library)
            }
        }
        .onAppear {
            guard !loaded else { return }
            loaded = true
            if let e = row?.entry {
                pattern = e.pattern; replacement = e.replacement; caseSensitive = e.caseSensitive; type = e.type
                test = e.type == 1 ? "" : e.pattern
            }
            voiceKey = voices.first?.entry?.key ?? ""
        }
    }

    private func preview() {
        guard let v = voice, let key = v.entry?.key else { return }
        let text = test.isEmpty ? pattern : test
        guard !text.isEmpty else { return }
        let e = entry
        DirectPlayer.shared.tryOut(text, entries: e.pattern.isEmpty ? "" : e.line, voiceKey: key, language: v.language,
                                   rules: library.rules(for: v), rulesPack: library.rulesPack(for: v), settings: library.settings)
    }

    private func save(emptyOK: Bool) {
        let e = entry
        if let p = EntryDraft.problem(e, emptyOK: emptyOK) {
            if p == "empty" { confirmEmpty = true } else { problem = p }
            return
        }
        DictionaryStore.put(file, e, oldKey: row?.entry.key)
        saved()
        AccessibilityNotification.Announcement(row == nil ? "Added" : "Saved").post()
        dismiss()
    }
}

/// The voice's sounds with example words: double-tap inserts one; Preview (an action, or a swipe) says it.
private struct MobilePhonemeSheet: View {
    let voiceKey: String
    let language: String
    let insert: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @Environment(VoiceLibrary.self) private var library
    @State private var items: [(xsampa: String, example: String, heard: String)] = []

    var body: some View {
        NavigationStack {
            List {
                ForEach(items, id: \.xsampa) { item in
                    let label = item.example.isEmpty ? item.xsampa : "\(item.xsampa), as in \(item.example)"
                    Button(label) {
                        insert(item.xsampa)
                        AccessibilityNotification.Announcement("Inserted \(item.xsampa)").post()
                        dismiss()
                    }
                    .foregroundStyle(.primary)
                    .swipeActions(edge: .leading) {
                        Button("Preview") { PhonemePage.hear(item, voiceKey: voiceKey, library: library) }.tint(.blue)
                    }
                    .accessibilityAction(named: "Preview") { PhonemePage.hear(item, voiceKey: voiceKey, library: library) }
                }
            }
            .navigationTitle("Insert Phoneme")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
            .overlay { if items.isEmpty { Text("No phonemes for this voice.").foregroundStyle(.secondary) } }
        }
        .onAppear { items = PhonemePage.sounds(key: voiceKey, language: language) }
    }
}
#endif
