import Foundation

/// Something whose downloads go through `Downloader`, under its own id prefix.
/// Called on the downloader's queue.
protocol DownloadClient: AnyObject, Sendable {
    /// Bytes so far; `expected` is -1 when the server didn't say.
    func downloadProgress(_ id: String, written: Int64, expected: Int64)
    /// The file is the client's to move or delete.
    func downloadFinished(_ id: String, _ result: Result<URL, Error>)
    /// A download from before the app was relaunched is still going.
    func downloadRestored(_ id: String)
}

/// Fetches voice packages in a background URLSession. The system runs them
/// while the app is in the background or closed, queues many at once without
/// timing out the ones still waiting, and reports real progress. Interrupted
/// downloads resume where they stopped, a few times, before failing.
final class Downloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    static let shared = Downloader()
    static let sessionIdentifier = "com.levigobin.ClassicVoices.downloads"

    /// iOS hands this over when it wakes the app for finished downloads.
    var backgroundCompletion: (() -> Void)?

    private let lock = NSLock()
    private var clients: [String: DownloadClient] = [:]
    private var finished: [Int: URL] = [:]       // task id -> file, until the task completes
    private var attempts: [String: Int] = [:]
    private var cancelled: Set<String> = []
    private let queue: OperationQueue = {
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 1
        q.name = "Classic Voices downloads"
        return q
    }()

    private lazy var session: URLSession = {
        let c = URLSessionConfiguration.background(withIdentifier: Self.sessionIdentifier)
        c.isDiscretionary = false
        c.sessionSendsLaunchEvents = true
        c.allowsCellularAccess = true
        c.allowsExpensiveNetworkAccess = true
        c.allowsConstrainedNetworkAccess = true
        c.timeoutIntervalForResource = 24 * 60 * 60
        return URLSession(configuration: c, delegate: self, delegateQueue: queue)
    }()

    /// Ids look like "<prefix>:<client's own id>".
    func register(_ client: DownloadClient, prefix: String) {
        lock.withLock { clients[prefix] = client }
    }

    /// Reconnects to downloads still running from before a relaunch.
    func restore() {
        session.getAllTasks { tasks in
            for t in tasks where t.state == .running || t.state == .suspended {
                guard let id = t.taskDescription, let (client, sub) = self.client(for: id) else { continue }
                client.downloadRestored(sub)
            }
        }
    }

    func start(_ url: URL, id: String) {
        lock.withLock {
            cancelled.remove(id)
            attempts[id] = 0
        }
        let task = session.downloadTask(with: url)
        task.taskDescription = id
        task.resume()
    }

    func cancel(_ id: String) {
        lock.withLock { _ = cancelled.insert(id) }
        session.getAllTasks { tasks in
            for t in tasks where t.taskDescription == id { t.cancel() }
        }
    }

    private func client(for id: String) -> (DownloadClient, String)? {
        guard let colon = id.firstIndex(of: ":") else { return nil }
        let prefix = String(id[..<colon]), sub = String(id[id.index(after: colon)...])
        return lock.withLock { clients[prefix] }.map { ($0, sub) }
    }

    // MARK: - URLSessionDownloadDelegate

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard let id = downloadTask.taskDescription, let (client, sub) = client(for: id) else { return }
        client.downloadProgress(sub, written: totalBytesWritten, expected: totalBytesExpectedToWrite)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file is deleted when this returns, so it moves now.
        if let http = downloadTask.response as? HTTPURLResponse, http.statusCode != 200 { return }
        let ext = downloadTask.originalRequest?.url?.pathExtension ?? "download"
        let dest = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + "." + ext)
        do {
            try FileManager.default.moveItem(at: location, to: dest)
            lock.withLock { finished[downloadTask.taskIdentifier] = dest }
        } catch {
            log.error("download move: \(error.localizedDescription, privacy: .public)")
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let id = task.taskDescription else { return }
        let file = lock.withLock { finished.removeValue(forKey: task.taskIdentifier) }
        let target = client(for: id)
        if lock.withLock({ cancelled.remove(id) }) != nil {
            if let file { try? FileManager.default.removeItem(at: file) }
            target?.0.downloadFinished(target!.1, .failure(CancellationError()))
            return
        }
        if let error {
            // Pick up where it stopped when the network dropped.
            let ns = error as NSError
            let n = lock.withLock { () -> Int in
                attempts[id, default: 0] += 1
                return attempts[id]!
            }
            if let data = ns.userInfo[NSURLSessionDownloadTaskResumeData] as? Data, n <= 5 {
                log.info("download \(id, privacy: .public) resuming after: \(error.localizedDescription, privacy: .public)")
                let again = session.downloadTask(withResumeData: data)
                again.taskDescription = id
                again.resume()
                return
            }
            target?.0.downloadFinished(target!.1, .failure(error))
            return
        }
        if let http = task.response as? HTTPURLResponse, http.statusCode != 200 {
            target?.0.downloadFinished(target!.1, .failure(NSError(
                domain: "ClassicVoices", code: 3,
                userInfo: [NSLocalizedDescriptionKey: "Apple's server answered HTTP \(http.statusCode)"])))
            return
        }
        guard let file else {
            target?.0.downloadFinished(target!.1, .failure(NSError(
                domain: "ClassicVoices", code: 5, userInfo: [NSLocalizedDescriptionKey: "The download couldn't be saved"])))
            return
        }
        if let target { target.0.downloadFinished(target.1, .success(file)) } else { try? FileManager.default.removeItem(at: file) }
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        DispatchQueue.main.async {
            self.backgroundCompletion?()
            self.backgroundCompletion = nil
        }
    }
}
