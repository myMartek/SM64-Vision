import Foundation

/// What a texture pack actually changes.
///
/// DynOS has no notion of a description: a pack is a folder, and its name is
/// the folder's name. That is enough to switch one on and nothing like enough
/// to decide whether one wants to - "Fixed Smoke" and "Coop Cafe Pipe" arrive
/// with the game itself and say nothing about themselves.
///
/// Two sources, in order. A pack may carry its own note in a plain text file,
/// which is how anything fetched later can explain itself without the app
/// having heard of it. Failing that, the handful that ship with the game are
/// described here, because somebody has to and they are not going to.
///
/// Read out of the archives rather than taken on trust. The HD pack's index -
/// 2723 entries - holds a texture for the burn smoke and not one .bin in the
/// whole of it, so it repaints the smoke and leaves its shape alone. Fixed
/// Smoke is a shape. They do different things and belong together.
@MainActor
enum PackNotes {
    private static let carried = ["description.txt", "README.txt", "readme.txt"]

    /// Where packs live: the writable folder beside the ROM, and the read-only
    /// copy inside the app that the game is shipped with.
    private static var folders: [URL] {
        var out = [TexturePack.packsDirectory]
        if let bundled = Bundle.main.resourceURL?
            .appendingPathComponent("dynos", isDirectory: true)
            .appendingPathComponent("packs", isDirectory: true) {
            out.append(bundled)
        }
        return out
    }

    static func note(for name: String) -> String? {
        for folder in folders {
            for file in carried {
                let url = folder.appendingPathComponent(name).appendingPathComponent(file)
                if let text = try? String(contentsOf: url, encoding: .utf8) {
                    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
                    if !trimmed.isEmpty { return trimmed }
                }
            }
        }
        return known[name]
    }

    /// Read off what is in them, since neither carries a word of its own.
    private static let known: [String: String] = [
        "Fixed Smoke": String(localized:
            "Replaces the shape of the smoke that rises when Mario is on fire, correcting a fault in the original. Comes with the game. Worth having alongside the HD textures, which repaint that smoke but leave its shape as it was."),
        "Render96 HD Texture Pack": String(localized:
            "Hand-made replacements for the game's textures — 2723 of them, at many times the original resolution. Textures only: no models are changed. Six hundred megabytes, fetched on request."),
    ]
}
