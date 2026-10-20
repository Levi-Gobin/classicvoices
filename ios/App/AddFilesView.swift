#if !os(tvOS)  // Apple TV has no file picker
import AVFoundation
import SwiftUI
import UniformTypeIdentifiers

/// Adds speech files someone has from their own Mac OS X: what each release
/// still needs, a button to choose files or folders, and what happened.
struct AddFilesView: View {
    @Environment(VoiceLibrary.self) private var library
    @State private var picking = false
    @State private var working = false
    @State private var result = ""
    @State private var pending: (SpeechDataImport.Found, [URL])?
    @State private var askRelease = false
    @State private var refresh = 0

    var body: some View {
        Page {
            PageSection(nil) {
                Text("MacinTalk voices (Fred, Victoria, Alex and the rest) run on Apple\u{2019}s own MacinTalk engine from Mac OS X, which Classic Voices can\u{2019}t download for you. Add it from a Mac OS X 10.4 to 10.6 install disc you have: choose its disk image (.dmg or .iso) or the disc itself, a speech data folder, a copy of a Mac\u{2019}s System/Library/Speech folder with the files listed below, or single .SpeechVoice voices such as Alex.")
                    .font(.callout)
                OpenButton(title: working ? "Adding\u{2026}" : "Choose Files or Folders\u{2026}", opens: false) { picking = true }
                    .disabled(working)
            }
            if !result.isEmpty {
                PageSection("Result") { Text(result).textSelection(.enabled) }
            }
            PageSection("Mac OS X releases") {
                ForEach(VoiceCatalog.macinTalkEras, id: \.0) { era in
                    VStack(alignment: .leading, spacing: 2) {
                        Text("\(era.2) \(era.1)")
                        Text(state(era.0)).font(.footnote).foregroundStyle(.secondary)
                    }
                    .accessibilityElement(children: .combine)
                }
            }
            .id(refresh)
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [.folder, .item], allowsMultipleSelection: true) { r in
            guard case .success(let urls) = r, !urls.isEmpty else { return }
            start(urls)
        }
        .confirmationDialog("Which Mac OS X are these voices from?", isPresented: $askRelease, titleVisibility: .visible) {
            ForEach(SpeechDataImport.installedReleases(), id: \.self) { gen in
                Button(SpeechDataImport.releaseName(gen)) { if let p = pending { finish(p.0, p.1, gen: gen) } }
            }
            Button("Cancel", role: .cancel) { pending = nil }
        }
    }

    private func state(_ gen: String) -> String {
        if VoiceCatalog.hasBundledMacinTalk(gen) { return "Comes with this copy of the app." }
        let missing = SpeechDataImport.missing(gen)
        if missing.isEmpty { return "Added." }
        let none = !VoiceCatalog.hasMacinTalkEngine(gen) && missing.count >= 3
        return (none ? "Not added. Needs " : "Still needs ") + missing.joined(separator: "; ") + "."
            + (gen == "mt104" ? " Use an Intel copy of Tiger; the PowerPC discs\u{2019} engine can\u{2019}t run here." : "")
    }

    private func start(_ urls: [URL]) {
        working = true
        result = ""
        Task.detached {
            let found = SpeechDataImport.scan(urls)
            await MainActor.run { finish(found, urls, gen: nil) }
        }
    }

    private func finish(_ found: SpeechDataImport.Found, _ urls: [URL], gen: String?) {
        working = true
        pending = nil
        Task.detached {
            let text: String
            var ask = false
            do {
                let o = try SpeechDataImport.install(found, gen: gen, from: urls) { step in
                    Task { @MainActor in result = step }
                }
                if o.needsRelease {
                    ask = true
                    text = ""
                } else {
                    var lines = ["Added " + (o.added.isEmpty ? "nothing new" : o.added.joined(separator: ", ")) + " for \(SpeechDataImport.releaseName(o.gen ?? ""))."]
                    if !o.missing.isEmpty {
                        lines.append("It can\u{2019}t speak yet. Still needed, from the same Mac OS X disk:")
                        lines += o.missing.map { "\u{2022} \($0)" }
                    }
                    if let t = o.test, !t.isEmpty { lines.append(t) }
                    if !found.otherVoices.isEmpty {
                        lines.append("Skipped \(found.otherVoices.joined(separator: ", ")): Classic Voices downloads those from Apple in the macOS tab.")
                    }
                    text = lines.joined(separator: "\n")
                }
            } catch {
                text = error.localizedDescription
            }
            await MainActor.run {
                working = false
                if ask {
                    pending = (found, urls)
                    askRelease = true
                    return
                }
                result = text
                refresh += 1
                library.rebuild()
                AVSpeechSynthesisProviderVoice.updateSpeechVoices()
                AccessibilityNotification.Announcement(text).post()
            }
        }
    }
}
#endif
