import SwiftUI
#if os(iOS)
import UserNotifications
#endif

/// "Dictionaries Updated" with what changed (CommunityDictionary.Changes.text): on iPhone and iPad a notification
/// when the dictionaries change by themselves (opt in), and everywhere an alert after Download the Latest
/// Dictionaries.
enum DictionaryNotifications {
    static let title = "Dictionaries Updated"

    #if os(iOS)
    static var enabled: Bool {
        get { UserDefaults.standard.bool(forKey: "dictionaries.notify") }
        set { UserDefaults.standard.set(newValue, forKey: "dictionaries.notify") }
    }

    /// At launch: fetches that change something (the background refresh, the app coming to the front) notify.
    static func install() {
        CommunityDictionary.changed = { changes in
            guard enabled, let body = changes.text else { return }
            let content = UNMutableNotificationContent()
            content.title = title
            content.body = body
            UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "dictionaries-updated",
                                                                        content: content, trigger: nil))
        }
    }

    static func requestPermission() async -> Bool {
        (try? await UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound])) ?? false
    }
    #endif
}

#if os(iOS)
/// The opt-in switch, next to the community dictionaries.
struct DictionaryNotificationsToggle: View {
    @State private var on = DictionaryNotifications.enabled
    @State private var denied = false

    var body: some View {
        Toggle("Notify When the Dictionaries Change", isOn: $on)
            .onChange(of: on) { _, v in
                DictionaryNotifications.enabled = v
                guard v else { return }
                Task {
                    if !(await DictionaryNotifications.requestPermission()) {
                        denied = true
                        on = false
                        DictionaryNotifications.enabled = false
                    }
                }
            }
            .alert("Notifications Are Off", isPresented: $denied) {
                Button("OK", role: .cancel) {}
            } message: {
                Text("Allow notifications for Classic Voices in the Settings app to be told when the dictionaries change.")
            }
    }
}
#endif

/// What a manual download changed, for an alert with OK: (title, message).
extension CommunityDictionary.Changes {
    func alert(ok: Bool) -> (String, String) {
        guard ok else { return ("Couldn’t Download the Dictionaries", "Check the connection and try again.") }
        if let text { return (DictionaryNotifications.title, text) }
        return ("Dictionaries Up to Date", "Nothing has changed since the last download.")
    }
}
