import AppKit
import AVFoundation
import Foundation
import Observation
import SwiftUI

/// The released Mac app's first launch: it carries nothing of Apple's, so it
/// fetches the engines (EngineImport) and builds their fast code
/// (FastCodeBuilder) before the voices can speak, and says how far it is.
/// Each step that's done stays done; a later launch picks up the rest.
@MainActor @Observable
final class MacSetup {
    static let shared = MacSetup()

    /// What's happening now ("Downloading iOS 6.1.6 (580 MB)"), or nil.
    private(set) var status: String?
    /// How far that step is, 0...1, or nil while it can't say.
    private(set) var fraction: Double?
    private(set) var error: String?
    private(set) var running = false
    private(set) var finished = false
    @ObservationIgnored private var observing = false

    /// Steps still to do, for the page to say what's left.
    var remaining: [String] {
        guard let root = VoiceCatalog.importedRoot, let native = VoiceCatalog.nativeDir else { return [] }
        return EngineImport.firmware.filter { !EngineImport.isImported($0.build, in: root) }.map(\.name)
            + Self.macinTalkTodo(root).map { "MacinTalk \($0)" }
            + MacCompacts.todo(root).map { "the \(MacCompacts.recipe[$0]?.name ?? $0) compact voices" }
            + FastCodeBuilder.todo(root: root, native: native).map { "fast code for \($0)" }
    }

    /// MacinTalk releases not fetched yet (MacinTalkFetch).
    nonisolated static func macinTalkTodo(_ root: URL) -> [String] {
        FastCodeBuilder.macinTalkGens.filter { !FileManager.default.fileExists(atPath: root.appendingPathComponent("\($0)/MacinTalk").path) }
    }

    var needed: Bool { !remaining.isEmpty }

    func start() {
        #if CV_PUBLIC
        if !observing {
            observing = true
            MacUpdater.shared.cleanUpIfIdle()
            // compiles stop with the app
            NotificationCenter.default.addObserver(forName: NSApplication.willTerminateNotification, object: nil,
                                                   queue: .main) { _ in
                FastCodeBuilder.stopAll()
                // an update downloaded while it ran takes its place (MacUpdater)
                MainActor.assumeIsolated { MacUpdater.shared.installOnQuit() }
            }
        }
        guard !running, let root = VoiceCatalog.importedRoot, let native = VoiceCatalog.nativeDir else { return }
        guard needed else { finished = true; return }
        running = true
        error = nil
        let todo = EngineImport.firmware.filter { !EngineImport.isImported($0.build, in: root) }
        Thread.detachNewThread { [weak self] in
            var failure: String?
            for fw in todo {
                do {
                    try EngineImport.importFromApple(fw, into: root) { message, fraction in
                        Task { @MainActor in self?.report(message, fraction) }
                    }
                } catch {
                    failure = "\(fw.name): \(error.localizedDescription)"
                    break
                }
                Self.reloadVoices()
            }
            // the MacinTalk voices (Fred, Victoria ...): Tiger, Leopard and Snow
            // Leopard from install discs on archive.org, Lion from Apple
            if failure == nil {
                for gen in Self.macinTalkTodo(root) {
                    do {
                        try MacinTalkFetch.fetch(gen, into: root) { message, fraction in
                            Task { @MainActor in self?.report(message, fraction) }
                        }
                    } catch {
                        failure = "MacinTalk (\(gen)): \(error.localizedDescription)"
                        break
                    }
                    Self.reloadVoices()
                }
            }
            // the compact voices Lion, Mountain Lion and Mavericks had with the system
            if failure == nil {
                for era in MacCompacts.todo(root) {
                    do {
                        try MacCompacts.fetch(era, into: root) { message, fraction in
                            Task { @MainActor in self?.report(message, fraction) }
                        }
                    } catch {
                        failure = "\(MacCompacts.recipe[era]?.name ?? era) compact voices: \(error.localizedDescription)"
                        break
                    }
                    Self.reloadVoices()
                }
            }
            // then their fast code, which the voices use from then on
            let builds = failure == nil ? FastCodeBuilder.todo(root: root, native: native) : []
            if !builds.isEmpty {
                let report: EngineImport.Progress = { message, fraction in Task { @MainActor in self?.report(message, fraction) } }
                do {
                    let zig = try FastCodeBuilder.zig(progress: report)
                    // one engine that fails doesn't keep the others slow
                    var failed: [String] = []
                    for b in builds {
                        do {
                            try FastCodeBuilder.build(b, root: root, native: native, zig: zig, progress: report)
                            try? VoiceStore.shared.reloadEngines()
                        } catch {
                            failed.append("\(b): \(error.localizedDescription)")
                        }
                    }
                    if !failed.isEmpty { failure = "Fast code: " + failed.joined(separator: "; ") }
                } catch {
                    failure = "Fast code: \(error.localizedDescription)"
                }
            }
            Task { @MainActor in
                guard let self else { return }
                self.running = false
                self.status = nil
                self.fraction = nil
                self.error = failure
                self.finished = failure == nil
            }
        }
        #endif
    }

    private func report(_ message: String?, _ value: Double?) {
        if let message {
            status = message
            fraction = value
            AccessibilityNotification.Announcement(message).post()
        } else if let value {
            // whole percents: each change redraws the page and VoiceOver reads the value
            let rounded = (value * 100).rounded(.down) / 100
            if rounded != fraction { fraction = rounded }
        }
    }

    /// The extension and the app look at the new engines; the system hears of the voices.
    nonisolated static func reloadVoices() {
        VoiceCatalog.reloadBuiltIn()
        try? VoiceStore.shared.reloadEngines()
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
        Task { @MainActor in VoiceLibrary.shared.rebuild() }
    }
}

/// The first-launch steps on the released Mac app, at the top of each voice page.
struct MacSetupSection: View {
    @State private var setup = MacSetup.shared

    var body: some View {
        if setup.running || setup.error != nil || setup.needed {
            PageSection("Getting Classic Voices ready") {
                Text("Classic Voices gets its voices the first time it opens: the iPhone firmware they shipped in, from Apple\u{2019}s servers, and the MacinTalk voices from old Mac OS X install discs kept on archive.org and at Apple, about 12 GB in all. Then it makes them fast on this Mac. You can keep using the app meanwhile.")
                    .fixedSize(horizontal: false, vertical: true)
                if let status = setup.status {
                    if let f = setup.fraction {
                        ProgressView(value: f) { Text(status) }
                            .accessibilityValue("\(Int(f * 100)) percent")
                    } else {
                        ProgressView { Text(status) }
                    }
                }
                if let error = setup.error {
                    Text(error).foregroundStyle(.red)
                    Button("Try Again") { setup.start() }
                } else if !setup.running {
                    Button("Get the Voices") { setup.start() }
                }
            }
        }
    }
}
