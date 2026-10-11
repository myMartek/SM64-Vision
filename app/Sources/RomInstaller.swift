import Foundation
import CryptoKit

/// Gets a Super Mario 64 ROM into the place coopdx looks for it.
///
/// coopdx scans its write path for any `.z64`, checks the MD5 against the
/// vanilla US ROM and copies the match to `baserom.us.z64` (see
/// src/pc/rom_checker.cpp). All this has to do is put the file there - but it
/// checks the hash itself first, so a wrong file is rejected with a clear
/// message instead of silently landing the game on coopdx's own setup screen,
/// which expects a drag and drop that visionOS cannot deliver.
enum RomInstaller {
    /// MD5 of Super Mario 64 (USA), the only version coopdx supports.
    static let expectedMD5 = "20b854b239203baf6c961b850a4a51a2"

    enum State {
        case present
        case missing(expectedPath: String)
    }

    /// `Application Support/sm64coopdx`, matching SDL_GetPrefPath's layout in
    /// visionos_sdl_compat.m.
    static var writeDirectory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("sm64coopdx", isDirectory: true)
    }

    static var romURL: URL {
        writeDirectory.appendingPathComponent("baserom.us.z64")
    }

    /// The demo file's marker, and where its presence is remembered.
    ///
    /// Showing the app needs something to look at, and the one thing that
    /// cannot be handed over is the game data: a file that satisfies the checksum above is
    /// the original ROM, byte for byte, and the ROM carries not only textures
    /// but the geometry, the collision and the sound. So instead of pretending
    /// to be one, this file announces itself as what it is and switches the app
    /// to a scene of our own - which is enough to judge everything the app
    /// actually does: the portal, the stereo, the modes, the window.
    static let demoMarker = Data("SM64VISION-DEMO\u{0}".utf8)
    static var demoURL: URL {
        writeDirectory.appendingPathComponent("demo.marker")
    }

    static var demoActive: Bool {
        FileManager.default.fileExists(atPath: demoURL.path)
    }

    /// Writes the file that switches the demo scene on. Kept next to the app rather
    /// than shipped inside it, so nothing about the demo reaches ordinary users.
    static func writeDemoFile(to url: URL) throws {
        var data = demoMarker
        data.append(Data(count: 4096 - demoMarker.count))
        try data.write(to: url)
    }

    static func check() -> State {
        if demoActive { return .present }
        if FileManager.default.fileExists(atPath: romURL.path) {
            return .present
        }
        return .missing(expectedPath: romURL.path)
    }

    enum InstallError: LocalizedError {
        case unreadable
        case wrongVersion(String)

        var errorDescription: String? {
            switch self {
            case .unreadable:
                return String(localized: "The file could not be read.")
            case .wrongVersion(let md5):
                return String(format: String(localized: "That is not an unmodified US ROM (MD5 %@). Only the US release is supported."), md5)
            }
        }
    }

    static func install(from source: URL) throws {
        let needsRelease = source.startAccessingSecurityScopedResource()
        defer { if needsRelease { source.stopAccessingSecurityScopedResource() } }

        guard let data = try? Data(contentsOf: source) else {
            throw InstallError.unreadable
        }

        // The demo file first: it is recognised by what it says about itself,
        // not by a checksum, and it is never treated as game data.
        if data.count >= demoMarker.count, data.prefix(demoMarker.count) == demoMarker {
            try FileManager.default.createDirectory(at: writeDirectory,
                                                    withIntermediateDirectories: true)
            try Data(demoMarker).write(to: demoURL)
            return
        }

        let digest = Insecure.MD5.hash(data: data)
        let md5 = digest.map { String(format: "%02x", $0) }.joined()
        guard md5 == expectedMD5 else {
            throw InstallError.wrongVersion(md5)
        }

        try FileManager.default.createDirectory(at: writeDirectory,
                                                withIntermediateDirectories: true)
        if FileManager.default.fileExists(atPath: romURL.path) {
            try FileManager.default.removeItem(at: romURL)
        }
        try data.write(to: romURL)

        // And drop the review marker. check() asks for it first, so leaving it
        // behind kept the review scene running even once real game data was in
        // place - the ROM was accepted and then never looked at.
        try? FileManager.default.removeItem(at: demoURL)
    }
}
