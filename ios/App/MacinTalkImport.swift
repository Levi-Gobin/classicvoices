import Foundation

/// MacinTalk voices too large to bundle (Alex) reach a device as plain
/// folders in the app's Documents/MacinTalk/<generation>/ (copied with
/// devicectl, or Finder/Files file sharing). On launch they are handed to
/// the speech extension's store, where it looks:
/// <store>/<generation>/Voices/<Name>.SpeechVoice
///
/// Blocks on the store: call off the main thread.
enum MacinTalkImport {
    @discardableResult
    static func run() -> [String] {
        let fm = FileManager.default
        guard let docs = fm.urls(for: .documentDirectory, in: .userDomainMask).first else { return [] }
        let inbox = docs.appendingPathComponent("MacinTalk")
        var moved: [String] = []
        for (gen, _, _) in VoiceCatalog.macinTalkEras {
            let src = inbox.appendingPathComponent(gen)
            guard let items = try? fm.contentsOfDirectory(atPath: src.path) else { continue }
            for item in items where item.hasSuffix(".SpeechVoice") {
                let from = src.appendingPathComponent(item)
                guard fm.fileExists(atPath: from.appendingPathComponent("Contents/Resources/VoiceDescription").path) else { continue }
                do {
                    let job = VoiceStore.Job(items: [.init(local: item, dest: "\(gen)/Voices/\(item)")])
                    try VoiceStore.shared.enqueue(job, payloads: [from])
                    moved.append("\(gen)/\(item)")
                } catch {
                    log.error("MacinTalk import \(item, privacy: .public): \(error.localizedDescription, privacy: .public)")
                }
            }
        }
        return moved
    }
}
