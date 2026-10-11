import SwiftUI
import CompositorServices

@main
struct MarioVisionApp: App {
    static let gameWindowID = "game"
    static let launcherWindowID = "main"
    static let launcherActivity = "de.martek.sm64.openLauncher"
    static let portalSpaceID = "portal"

    @StateObject private var game = GameBridge()

    var body: some Scene {
        // The launcher: ROM setup on first run, then the tabbed menu (Start,
        // Settings - later multiplayer and mods). Single-instance Window, not
        // WindowGroup, so it cannot be opened twice.
        Window("MarioVision", id: Self.launcherWindowID) {
            MainMenuView(game: game)
        }
        .defaultSize(width: 560, height: 640)
        // Without this the app refuses to launch once the launcher has closed
        // itself: the system keeps the scene session, tries to restore it, and
        // SwiftUI stops with "given a scene with id 'main' but the matching
        // Window is already connected". The game window has carried the same
        // guard from the start.
        .restorationBehavior(.disabled)

        // The game itself, opened by the Start button. Launch-suppressed so a
        // fresh start always goes through the launcher, and restoration is off
        // so a stale game window never reappears without a running game behind
        // it.
        Window("SM64 Vision", id: Self.gameWindowID) {
            GameView()
                .ignoresSafeArea()
                .modifier(KeepASceneAlive())
        }
        .defaultSize(width: 1024, height: 768)
        .restorationBehavior(.disabled)
        .defaultLaunchBehavior(.suppressed)

        // The portal: the game world behind a window fixed in the room, in
        // stereo through CompositorServices. Passthrough stays visible around
        // it (.mixed).
        ImmersiveSpace(id: Self.portalSpaceID) {
            CompositorLayer(configuration: PortalConfiguration()) { layerRenderer in
                let thread = Thread {
                    if let renderer = PortalRenderer(layerRenderer: layerRenderer) {
                        renderer.run()
                    }
                }
                thread.name = "PortalRenderer"
                thread.qualityOfService = .userInteractive
                thread.start()
            }
        }
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
        // The system's own controls get out of the way while playing.
        //
        // With tracked controllers in hand, reaching for something in the game
        // and reaching for the home indicator look the same to the system, and
        // being thrown back to the home screen mid-jump is the worst kind of
        // interruption. Hiding the persistent overlays takes the ones this app
        // can decide about; the deliberate press-and-hold on the crown stays,
        // which is right - there has to be a way out that always works.
        .persistentSystemOverlays(.hidden)
        // No system hands drawn on top of Mario's: his arms are the ones being
        // followed, and two sets of hands in the same place read as a fault.
        .upperLimbVisibility(.hidden)
    }
}

struct PortalConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // One texture per eye, two passes: gfx_pc transforms vertices on the
        // CPU with a different matrix per eye, so single-pass amplification
        // cannot apply. Foveation off for the same reason.
        configuration.layout = .dedicated
        configuration.isFoveationEnabled = false

        // Not raising the compositor's render quality, though it is the
        // strongest lever against jagged edges: the game covers only a small
        // patch of the eye texture, and multisampling cannot invent the pixels
        // in between. Every value above the platform's own stops the layer from
        // coming up on device - measured, repeatedly. Left alone until there is
        // a reason to believe otherwise; MARIOVISION_RENDERQUALITY still sets it
        // for a controlled experiment.
        if let raw = ProcessInfo.processInfo.environment["MARIOVISION_RENDERQUALITY"],
           let q = Float(raw), q > 0 {
            configuration.maxRenderQuality = LayerRenderer.RenderQuality(min(q, 1))
        }

        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
    }
}


/// Reopens the launcher when the game's window goes away.
///
/// visionOS ends an app once its last scene is gone, and no background mode
/// changes that - which is why closing the game window took the sound with it
/// and left no way back in. Keeping one window standing keeps the process, and
/// with it the running game, its audio, and the button that brings the game
/// back.
private struct KeepASceneAlive: ViewModifier {
    @Environment(\.openWindow) private var openWindow

    func body(content: Content) -> some View {
        content.onDisappear { openWindow(id: MarioVisionApp.launcherWindowID) }
    }
}
