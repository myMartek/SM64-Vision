import SwiftUI

/// Drives scripted captures. `-screenshot <scene>` on the command line puts
/// the launcher straight into one state and holds it there, so a script can
/// shoot without anyone touching a controller.
///
/// Only the launcher is scripted. The one scene that shows the game itself
/// starts it and waits - what is on screen then is the game's own doing, not a
/// mock-up, which is the whole point of shipping these as screenshots.
enum ScreenshotScene: String {
    /// The game running: the castle grounds, as they come up after loading.
    case game
    /// The first-run screen, before a ROM has been supplied.
    case setup
    /// Settings, with the graphics page pushed.
    case graphics

    static var active: ScreenshotScene? {
        let arguments = ProcessInfo.processInfo.arguments
        guard let index = arguments.firstIndex(of: "-screenshot"),
              index + 1 < arguments.count else { return nil }
        return ScreenshotScene(rawValue: arguments[index + 1])
    }

    /// True while capturing anything - used to hold back the auto-close that
    /// would otherwise take the launcher off screen mid-shot.
    static var capturing: Bool { active != nil }
}
