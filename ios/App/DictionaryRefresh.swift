import Foundation
#if os(iOS)
import BackgroundTasks
#endif

/// Keeps the community pronunciation dictionaries current without the app being opened, when they're turned on:
/// new files swap in by themselves (CommunityDictionary reads a changed file in the background). iPhone and iPad:
/// a background app refresh task, about every 6 hours, as iOS allows. Mac: a background activity while the app runs.
/// The speech extension itself never goes on the network.
enum DictionaryRefresh {
    static let taskID = "com.levigobin.ClassicVoices.dictionaries"

    @MainActor static var enabled: Bool { VoiceLibrary.shared.settings.communityDictionaries }

    #if os(iOS)
    /// At launch, before it finishes (BGTaskScheduler's rule).
    static func register() {
        BGTaskScheduler.shared.register(forTaskWithIdentifier: taskID, using: nil) { task in
            guard let task = task as? BGAppRefreshTask else { return task.setTaskCompleted(success: false) }
            let work = Task {
                let on = await MainActor.run { enabled }
                let ok = on ? await CommunityDictionary.fetchAll() : true
                if on { await MainActor.run { schedule() } }
                task.setTaskCompleted(success: ok)
            }
            task.expirationHandler = { work.cancel() }
        }
    }

    /// When the app goes to the background: the next check, in about 6 hours.
    @MainActor static func schedule() {
        guard enabled else {
            BGTaskScheduler.shared.cancel(taskRequestWithIdentifier: taskID)
            return
        }
        let request = BGAppRefreshTaskRequest(identifier: taskID)
        request.earliestBeginDate = Date(timeIntervalSinceNow: CommunityDictionary.every)
        try? BGTaskScheduler.shared.submit(request)
    }
    #elseif os(macOS)
    nonisolated(unsafe) private static var activity: NSBackgroundActivityScheduler?

    /// While the app runs, about every 6 hours (the system picks the moment).
    static func start() {
        guard activity == nil else { return }
        let a = NSBackgroundActivityScheduler(identifier: taskID)
        a.repeats = true
        a.interval = CommunityDictionary.every
        a.tolerance = 3600
        a.qualityOfService = .utility
        a.schedule { done in
            Task {
                if await MainActor.run(body: { enabled }) { _ = await CommunityDictionary.fetchAll() }
                done(.finished)
            }
        }
        activity = a
    }
    #endif
}
