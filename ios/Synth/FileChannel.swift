#if !os(tvOS)  // no AUMessageChannel on Apple TV
import AudioToolbox
import Foundation

/// The app's way into the extension's voice store. The app hosts this audio
/// unit itself (VoiceStore in the app) and asks over the message channel to
/// write, list and delete files in the store; the system-hosted copy of the
/// extension then reads them, because the container belongs to the extension
/// bundle, not to its host. This replaces the App Group, which sideloading
/// tools often drop, and keeps the sandboxed Mac app out of the group too.
final class FileChannel: NSObject, AUMessageChannel {
    static let name = "com.levigobin.ClassicVoices.files"
    var callHostBlock: CallHostBlock?

    private let ops = StoreOps(root: VoiceCatalog.extensionStore!)

    func callAudioUnit(_ message: [AnyHashable: Any]) -> [AnyHashable: Any] {
        let op = message["op"] as? String
        if op == "hello" || op == "catalog" || op == "tree" { VoiceCatalog.adoptLegacy() }
        var reply = ops.perform(message)
        if op == "hello" {
            reply["group"] = VoiceCatalog.legacyGroupRoot?.path ?? ""
            reply["groupExists"] = VoiceCatalog.legacyGroupRoot.map { FileManager.default.fileExists(atPath: $0.path) } ?? false
        }
        return reply
    }
}
#endif
