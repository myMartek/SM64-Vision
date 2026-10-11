import Foundation

/// Fetches the high-definition texture pack and puts it where the game looks.
///
/// The pack is not shipped with the app and should not be: it is six hundred
/// megabytes of somebody else's work, under its own licence, derived from a
/// game nobody may distribute either. So it is fetched on request, by anybody
/// who wants it, and lands in the same writable place the ROM does.
///
/// The archive is deliberately stored rather than compressed. Its contents are
/// PNGs, which are compressed already - measured, deflate saved two and a half
/// per cent - and paying that for a decompressor in here would be a poor
/// bargain. What is left is a ZIP reader that only has to copy bytes out, which
/// is short enough to read in one sitting and has correspondingly little room
/// to be wrong.
@MainActor
final class TexturePack: ObservableObject {

    enum State: Equatable {
        case absent
        case downloading(Double)
        case unpacking(Double)
        case installed
        case failed(String)
    }

    @Published private(set) var state: State = .absent

    static let source = URL(string: "https://www.martek.de/sm64vision/mods/hdtextures.zip")!

    /// Where DynOS looks: the packs folder beside the ROM, which is the writable
    /// half of the two places it scans at startup.
    static var packsDirectory: URL {
        RomInstaller.writeDirectory
            .appendingPathComponent("dynos", isDirectory: true)
            .appendingPathComponent("packs", isDirectory: true)
    }

    private static let folderName = "Render96 HD Texture Pack"

    static var isInstalled: Bool {
        FileManager.default.fileExists(
            atPath: packsDirectory.appendingPathComponent(folderName).path)
    }

    init() {
        state = Self.isInstalled ? .installed : .absent
    }

    // MARK: - Fetching

    func install() {
        if case .downloading = state { return }
        if case .unpacking = state { return }
        Task { await run() }
    }

    /// Calls off a download in progress. Six hundred megabytes is long enough
    /// to change one's mind about, and long enough to have started by accident.
    func cancel() {
        guard case .downloading = state else { return }
        cancelled = true
        task?.cancel()
    }

    func remove() {
        let folder = Self.packsDirectory.appendingPathComponent(Self.folderName)
        try? FileManager.default.removeItem(at: folder)
        state = Self.isInstalled ? .installed : .absent
    }

    private var task: URLSessionDownloadTask?
    private var cancelled = false

    private func run() async {
        cancelled = false
        state = .downloading(0)
        do {
            let archive = try await download()
            defer { try? FileManager.default.removeItem(at: archive) }
            try unpack(archive)
            // The game reads the pack folders once as it comes up, so a pack
            // that has just arrived is invisible until it is told to look
            // again. Asking straight away spares anybody a restart after a
            // download of this size.
            mariovision_request_pack_rescan()
            state = .installed
        } catch {
            // Being called off is not a failure, and must not read like one.
            state = cancelled ? .absent : .failed(error.localizedDescription)
        }
        task = nil
    }

    private func download() async throws -> URL {
        // A session of our own, because the shared one ignores delegates.
        //
        // Two wrong turns led here and both are worth remembering. Streaming
        // the body a byte at a time reported progress perfectly and took an
        // age. Handing a delegate to URLSession.shared was fast and reported
        // nothing at all: the shared session does not support them, and says so
        // in its documentation rather than at the call site.
        //
        // A session that is ours takes both. The download runs in the
        // framework's own fast path, and the delegate is asked how far along it
        // is as it goes.
        let destination = FileManager.default.temporaryDirectory
            .appendingPathComponent("hdtextures.zip")
        try? FileManager.default.removeItem(at: destination)

        let watcher = Watcher(saveTo: destination) { [weak self] fraction in
            Task { @MainActor in self?.state = .downloading(fraction) }
        }
        let session = URLSession(configuration: .default,
                                 delegate: watcher, delegateQueue: nil)
        defer { session.finishTasksAndInvalidate() }

        return try await withCheckedThrowingContinuation { continuation in
            watcher.finished = { continuation.resume(with: $0) }
            let job = session.downloadTask(with: Self.source)
            task = job
            job.resume()
        }
    }

    /// Reports how far along the download is, and keeps the file.
    ///
    /// A class of its own because URLSession wants an object, and it must not
    /// be the pack itself: these callbacks arrive on the session's own queue
    /// while the pack belongs to the main actor.
    ///
    /// Moving the file here is not tidiness but necessity - the temporary one
    /// is deleted the moment this callback returns, so anything that wants it
    /// has to take it now.
    private final class Watcher: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
        private let destination: URL
        private let report: (Double) -> Void
        var finished: ((Result<URL, Error>) -> Void)?

        init(saveTo destination: URL, report: @escaping (Double) -> Void) {
            self.destination = destination
            self.report = report
        }

        func urlSession(_ session: URLSession,
                        downloadTask: URLSessionDownloadTask,
                        didWriteData bytesWritten: Int64,
                        totalBytesWritten: Int64,
                        totalBytesExpectedToWrite: Int64) {
            guard totalBytesExpectedToWrite > 0 else { return }
            report(Double(totalBytesWritten) / Double(totalBytesExpectedToWrite))
        }

        func urlSession(_ session: URLSession,
                        downloadTask: URLSessionDownloadTask,
                        didFinishDownloadingTo location: URL) {
            if let http = downloadTask.response as? HTTPURLResponse,
               http.statusCode != 200 {
                finished?(.failure(Failure("The server answered \(http.statusCode).")))
                finished = nil
                return
            }
            do {
                try? FileManager.default.removeItem(at: destination)
                try FileManager.default.moveItem(at: location, to: destination)
                finished?(.success(destination))
            } catch {
                finished?(.failure(error))
            }
            finished = nil
        }

        func urlSession(_ session: URLSession,
                        task: URLSessionTask,
                        didCompleteWithError error: Error?) {
            // Only reached with an error; success has already been answered
            // above and cleared the handler.
            if let error { finished?(.failure(error)) }
            finished = nil
        }
    }

    // MARK: - Unpacking

    private func unpack(_ archive: URL) throws {
        state = .unpacking(0)

        let handle = try FileHandle(forReadingFrom: archive)
        defer { try? handle.close() }

        let entries = try ZipReader.entries(in: archive)
        guard !entries.isEmpty else { throw Failure("The archive is empty.") }

        let root = Self.packsDirectory
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)

        for (index, entry) in entries.enumerated() {
            // A name that climbs out of the destination is the classic way an
            // archive escapes where it was told to go. Refused outright rather
            // than sanitised, since nothing legitimate looks like that.
            guard !entry.name.hasPrefix("/"), !entry.name.contains("..") else {
                throw Failure("The archive contains an unsafe path.")
            }

            let target = root.appendingPathComponent(entry.name)
            if entry.isDirectory {
                try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
                continue
            }

            try FileManager.default.createDirectory(
                at: target.deletingLastPathComponent(), withIntermediateDirectories: true)

            try handle.seek(toOffset: entry.dataOffset)
            let bytes = try handle.read(upToCount: Int(entry.size)) ?? Data()
            guard bytes.count == Int(entry.size) else {
                throw Failure("The archive ended sooner than it said it would.")
            }
            try bytes.write(to: target)

            if index % 64 == 0 {
                state = .unpacking(Double(index) / Double(entries.count))
            }
        }
    }

    struct Failure: LocalizedError {
        let text: String
        init(_ text: String) { self.text = text }
        var errorDescription: String? { text }
    }
}
