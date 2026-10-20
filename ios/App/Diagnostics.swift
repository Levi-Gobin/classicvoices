import AVFoundation
import SwiftUI

/// A plain-text report of what Classic Voices finds on this device, for
/// someone to paste into a bug report: where the voices are, whether the
/// shared App Group is reachable, whether each engine speaks, and the last
/// download errors.
enum Diagnostics {
    nonisolated static func report(downloadErrors errors: [String: String]) -> String {
        var out: [String] = []
        let info = Bundle.main.infoDictionary ?? [:]
        out.append("Classic Voices \(info["CFBundleShortVersionString"] ?? "?") (\(info["CFBundleVersion"] ?? "?"))")
        #if os(macOS)
        out.append("macOS \(ProcessInfo.processInfo.operatingSystemVersionString)")
        #else
        out.append("\(osName) \(ProcessInfo.processInfo.operatingSystemVersionString), \(modelIdentifier())")
        #endif
        out.append("Bundle ID: \(Bundle.main.bundleIdentifier ?? "?")")

        // The App Group: downloads live there, and the speech extension reads them from it.
        let container = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroup)
        out.append("App Group: \(appGroup) \(container == nil ? "(NOT available)" : "(available)")")
        #if !os(macOS)
        let profile = profileAppGroups(Bundle.main.bundleURL.appendingPathComponent("embedded.mobileprovision"))
        out.append("Profile App Groups: \(profile.map { $0.isEmpty ? "none" : $0.joined(separator: ", ") } ?? "no profile")")
        if let alt = info["ALTAppGroups"] as? [String] { out.append("ALTAppGroups: \(alt.joined(separator: ", "))") }
        #endif
        out.append("Voice store: \(VoiceStore.shared.mode)" + (VoiceStore.shared.hasPending ? ", installs waiting for the next launch" : ""))
        let ext = Bundle.main.builtInPlugInsURL?.appendingPathComponent("Synth.appex")
        out.append("Speech extension: \(ext.map { FileManager.default.fileExists(atPath: $0.path) ? "present" : "MISSING" } ?? "MISSING")")
        #if !os(macOS)
        if let ext, let groups = profileAppGroups(ext.appendingPathComponent("embedded.mobileprovision")) {
            out.append("Extension profile App Groups: \(groups.isEmpty ? "none" : groups.joined(separator: ", "))")
        }
        #endif

        // Each engine, run here in the app as Try It does.
        if let root = VoiceCatalog.root {
            for key in ["ios3/en-US", "ios4/en-US", "ios5/en-US", "ios6/en-US"] {
                var open = 0.0, first = 0.0, total = 0.0, audio = 0.0
                let rc = vxs_benchmark(root.path, VoiceCatalog.enhancedRoot?.path, key, "Settings. Button.", 100,
                                       &open, &first, &total, &audio)
                out.append(rc == 0 ? String(format: "Engine %@: speaks, %.0f ms of audio in %.0f ms", key, audio, total)
                                   : "Engine \(key): FAILED (\(rc))")
            }
        } else {
            out.append("Bundled voices: NOT FOUND")
        }

        // What the system lists, as VoiceOver sees it.
        let ours = AVSpeechSynthesisVoice.speechVoices().filter { $0.identifier.contains("com.levigobin.ClassicVoices") }
        out.append("System voices from Classic Voices: \(ours.count) of \(VoiceCatalog.providerVoices.count)")

        let downloaded = VoiceCatalog.enhanced.map(\.key).sorted()
        out.append("Downloaded voices: \(downloaded.count)" + (downloaded.isEmpty ? "" : " (\(downloaded.joined(separator: ", ")))"))
        out.append(errors.isEmpty ? "Download errors: none"
                                  : "Download errors:\n" + errors.sorted { $0.key < $1.key }.map { "  \($0.key): \($0.value)" }.joined(separator: "\n"))
        return out.joined(separator: "\n")
    }

    #if !os(macOS)
    private static var osName: String {
        #if os(tvOS)
        "tvOS"
        #elseif os(visionOS)
        "visionOS"
        #else
        "iOS"
        #endif
    }
    nonisolated private static func modelIdentifier() -> String {
        var u = utsname()
        uname(&u)
        return withUnsafeBytes(of: &u.machine) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
    #endif
}

/// Shows the report and copies it, so it can be pasted into a message.
struct DiagnosticsView: View {
    @Environment(VoiceLibrary.self) private var library
    @State private var text = "Checking…"
    @State private var copied = false

    var body: some View {
        ScrollView {
            Text(text)
                .font(.body.monospaced())
                #if !os(tvOS)
                .textSelection(.enabled)
                #endif
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
        }
        .toolbar {
            Button(copied ? "Copied" : "Copy Report") {
                #if os(macOS)
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(text, forType: .string)
                #elseif !os(tvOS)  // Apple TV has no pasteboard
                UIPasteboard.general.string = text
                #endif
                copied = true
                AccessibilityNotification.Announcement("Report copied").post()
            }
        }
        .task {
            let errors = library.enhanced.errors.merging(library.mac.errors) { a, _ in a }
            text = await Task.detached { Diagnostics.report(downloadErrors: errors) }.value
        }
    }
}
