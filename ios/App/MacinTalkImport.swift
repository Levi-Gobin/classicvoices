import Foundation

/// MacinTalk voices too large to bundle (Alex) reach a device as plain
/// folders in the app's Documents/MacinTalk/<generation>/ (copied with
/// devicectl, or Finder/Files file sharing). On launch they are moved into
/// the App Group, where the speech extension looks:
/// <group>/EnhancedVoices/<generation>/Voices/<Name>.SpeechVoice
enum MacinTalkImport {
    @discardableResult
    static func run() -> [String] {
        let fm = FileManager.default
        guard let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first,
              let dest = VoiceCatalog.enhancedRoot else { return [] }
        let inbox = docs.appendingPathComponent("MacinTalk")
        var moved: [String] = []
        for (gen, _, _) in VoiceCatalog.macinTalkEras {
            let src = inbox.appendingPathComponent(gen)
            guard let items = try? fm.contentsOfDirectory(atPath: src.path) else { continue }
            for item in items where item.hasSuffix(".SpeechVoice") {
                let from = src.appendingPathComponent(item)
                guard fm.fileExists(atPath: from.appendingPathComponent("Contents/Resources/VoiceDescription").path) else { continue }
                let to = dest.appendingPathComponent(gen).appendingPathComponent("Voices").appendingPathComponent(item)
                try? fm.createDirectory(at: to.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? fm.removeItem(at: to)
                do {
                    try fm.moveItem(at: from, to: to)
                    moved.append("\(gen)/\(item)")
                } catch {
                    log.error("MacinTalk import \(item, privacy: .public): \(error.localizedDescription, privacy: .public)")
                }
            }
        }
        return moved
    }
}
