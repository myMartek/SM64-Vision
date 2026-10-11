import SwiftUI

/// The launcher window. On visionOS the TabView renders as the floating tab bar
/// beside the window - it never overlays the content, unlike the ornament this
/// replaces. Structure follows Explore360's root view.
struct MainMenuView: View {
    @ObservedObject var game: GameBridge
    @State private var romState = RomInstaller.check()
    @State private var tab = 0
    @State private var startTaps = 0

    /// Counts taps on the Start tab, including taps on the one already showing
    /// - which is what makes a tab usable as a hidden switch. Ten opens the
    /// debug page.
    private var tabSelection: Binding<Int> {
        Binding(get: { tab },
                set: { picked in
                    if picked == 0 {
                        startTaps += 1
                        if startTaps >= 10 && !game.debugUnlocked {
                            game.debugUnlocked = true
                        }
                    } else {
                        startTaps = 0
                    }
                    tab = picked
                })
    }
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some View {
        Group {
            // The setup capture has to show this screen whether or not a ROM is
            // actually installed, so it is asked for by name.
            if ScreenshotScene.active == .setup {
                RomSetupView(expectedPath: RomInstaller.romURL.path) { }
            } else if case .missing(let expectedPath) = romState {
                RomSetupView(expectedPath: expectedPath) {
                    romState = RomInstaller.check()
                    game.bootIfPossible()
                }
            } else {
                TabView(selection: tabSelection) {
                    Tab("Start", systemImage: "play.circle.fill", value: 0) {
                        StartTabView(game: game, present: present)
                    }
                    Tab("Mods", systemImage: "square.stack.3d.up", value: 3) {
                        NavigationStack { ModsView(game: game) }
                    }
                    if game.debugUnlocked {
                        Tab("Debug", systemImage: "ladybug", value: 2) {
                            DebugView(game: game)
                        }
                    }
                    Tab("Feedback", systemImage: "exclamationmark.bubble", value: 4) {
                        FeedbackView()
                    }
                    if ControllerSetupView.wanted {
                        Tab("Controller", systemImage: "gamecontroller", value: 5) {
                            ControllerSetupView()
                        }
                    }
                    Tab("Settings", systemImage: "gearshape", value: 1) {
                        SettingsView(game: game)
                    }
                }
            }
        }
        .task { game.bootIfPossible() }
        .task { await runScreenshotScene() }
        .onAppear { game.openWindowAction = openWindow }
        // The game's own prompt still says to press R for the menu; what it
        // opens is this window. Just open - closing and reopening in one turn
        // is what asks the system to connect a second scene under the same id,
        // which SwiftUI treats as fatal.
        .onChange(of: game.menuRequested) { _, requested in
            guard requested else { return }
            game.clearMenuRequest()
            openWindow(id: MarioVisionApp.launcherWindowID)
        }
        // Switching mode while playing: tear down whichever presentation is up
        // and bring the other one in, without touching the running game.
        .onChange(of: game.renderMode) { _, _ in
            guard game.isRunning else { return }
            Task { await present() }
        }
        .task {
            // Debug: MARIOVISION_AUTOPORTAL=1 starts the game and opens the
            // presentation as soon as loading finishes - the simulator offers no
            // way to press buttons from automation.
            guard ProcessInfo.processInfo.environment["MARIOVISION_AUTOPORTAL"] != nil else { return }
            while !game.isReady { try? await Task.sleep(for: .milliseconds(500)) }
            game.startGame()
            await present()
        }
    }

    /// Puts the launcher into the state a capture wants and leaves it there.
    @MainActor
    private func runScreenshotScene() async {
        switch ScreenshotScene.active {
        case .graphics:
            // SettingsView pushes its graphics page itself once it is on
            // screen; a push aimed at a NavigationStack that has not appeared
            // yet is dropped.
            tab = 1
        case .game:
            while !game.isReady { try? await Task.sleep(for: .milliseconds(300)) }
            game.startGame()
            // Through present(), so the shot follows whatever render mode is
            // selected - MARIOVISION_MODE picks it - rather than always the
            // flat window.
            await present()
        case .setup, .none:
            break
        }
    }

    /// Opens whatever the current mode needs, closes the other, and then steps
    /// out of the way.
    ///
    /// The launcher has nothing left to say once the game is up, and in a
    /// headset a panel beside the world is more in the way than on a desktop.
    /// It comes back on the menu key - which lands here now rather than in
    /// coopdx's own panel - so closing it costs nothing.
    @MainActor
    private func present() async {
        if game.renderMode.usesImmersiveSpace {
            dismissWindow(id: MarioVisionApp.gameWindowID)
            _ = await openImmersiveSpace(id: MarioVisionApp.portalSpaceID)
        } else {
            await dismissImmersiveSpace()
            openWindow(id: MarioVisionApp.gameWindowID)
        }

        // A beat after the new presentation is up: this is the view's own
        // window, and closing it in the same breath as the scene change is
        // asking for the dismissal to be lost.
        try? await Task.sleep(for: .milliseconds(400))
        // The launcher is half of what the setup and settings shots show, but
        // it only gets in the way of the one showing the game.
        guard ScreenshotScene.active == nil || ScreenshotScene.active == .game else { return }
        dismissWindow(id: MarioVisionApp.launcherWindowID)
    }
}

/// The main page: the Start button and nothing else. Joining players, mods and
/// server settings will land here later.
struct StartTabView: View {
    @ObservedObject var game: GameBridge
    let present: @MainActor () async -> Void

    var body: some View {
        VStack(spacing: 28) {
            Spacer()

            Text("SM64 Vision")
                .font(.extraLargeTitle2)

            Group {
                if game.isRunning && game.windowOpen {
                    Label("Running", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.secondary)
                } else if game.isReady || game.isRunning {
                    Button {
                        if !game.isRunning { game.startGame() }
                        Task { await present() }
                    } label: {
                        Label(game.isRunning ? "Resume" : "Start", systemImage: "play.fill")
                            .font(.title2)
                            .padding(.horizontal, 24)
                            .padding(.vertical, 6)
                    }
                    .buttonStyle(.borderedProminent)
                } else {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("Loading…")
                            .foregroundStyle(.secondary)
                    }
                }
            }

            Text(hint)
                .font(.footnote)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 420)

            Spacer()
        }
        .padding(32)
    }

    private var hint: String {
        if game.isRunning && !game.windowOpen {
            // The game deliberately keeps running with nothing showing it, so
            // say so - otherwise a closed window looks like a lost session.
            return String(format: String(localized: "The game keeps running, you just can\'t see it. “Resume” brings it back as %@."), game.renderMode.title)
        }
        return String(localized: "Starts straight into the castle and skips the intro. This window closes as it does — the menu button on the controller brings it back.")
    }
}
