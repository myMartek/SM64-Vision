import SwiftUI
import UniformTypeIdentifiers

/// The settings tab. The render mode sits on the first level because it is the
/// one thing you change often; everything else lives a level down.
struct SettingsView: View {
    @ObservedObject var game: GameBridge
    @State private var showGraphics = false
    @State private var picking = false
    @State private var romMessage: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Render mode", selection: $game.renderMode) {
                        // The flat window is the game's own render path and has
                        // nothing to show without it, so the review scene leaves
                        // it out rather than offering an empty window.
                        ForEach(RenderMode.allCases.filter { !game.demoMode || $0 != .window2D }) { mode in
                            Text(mode.pickerLabel).tag(mode)
                        }
                    }
                    Text(game.renderMode.explanation)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } header: {
                    Text("Presentation")
                } footer: {
                    Text("Can be switched while the game is running.")
                }

                Section {
                    NavigationLink("Controls") {
                        ControlsSettingsView(game: game)
                    }
                    NavigationLink("Graphics") {
                        GraphicsSettingsView(game: game)
                    }
                }

                Section {
                    Button("Replace game data…") { picking = true }
                    if let romMessage {
                        Text(romMessage)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Text(game.demoMode
                         ? "Currently running the review scene instead of a game. Choosing a ROM image switches back."
                         : "Choose a different ROM image, for instance after replacing the file. Takes effect on the next launch.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } header: {
                    Text("Game data")
                }
            }
            .navigationTitle("Settings")
            .fileImporter(isPresented: $picking,
                          allowedContentTypes: [UTType(filenameExtension: "z64") ?? .data, .data],
                          allowsMultipleSelection: false) { result in
                switch result {
                case .success(let urls):
                    guard let url = urls.first else { return }
                    do {
                        try RomInstaller.install(from: url)
                        romMessage = String(localized: "Accepted. Restart the app for it to take effect.")
                    } catch {
                        romMessage = error.localizedDescription
                    }
                case .failure(let error):
                    romMessage = error.localizedDescription
                }
            }
            .navigationDestination(isPresented: $showGraphics) {
                GraphicsSettingsView(game: game)
            }
            // The graphics capture wants this page open. Pushed from here rather
            // than from the launcher, because the stack has to exist first.
            .task {
                guard ScreenshotScene.active == .graphics else { return }
                try? await Task.sleep(for: .milliseconds(700))
                showGraphics = true
            }
        }
    }
}

/// How the three things one can do twice over are actually done.
struct ControlsSettingsView: View {
    @ObservedObject var game: GameBridge

    var body: some View {
        Form {
            Section {
                picker("Swimming", selection: $game.swimScheme)
                Text("Breaststroke with both hands at once, or the jump button, as the game has always had it. A long swim is tiring done properly, so both are allowed to begin with.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Swimming")
            }

            Section {
                picker("Punching", selection: $game.punchScheme)
                Text("Throw the punch, or press the button for it. Leaving the button on while the arm does the same job means the same act has two ways of happening — and the easier one wins, which rather defeats the arm.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Punching")
            }

            Section {
                picker("Picking things up", selection: $game.carryScheme)
                Text("Reach out and squeeze the trigger, or dive into things the way the game intends. The button covers punching too — in the original they are the same button, and it cannot be split.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Carrying")
            }

            Section {
                Toggle("Open doors by hand", isOn: $game.handDoors)
                Text("Take hold of the door and swing it yourself, then walk through when it is open. Off, and walking into a door opens it as it always did — which is the only way to play sitting down, or with an ordinary pad.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Doors")
            }

            Section {
                Toggle("Climb by holding on", isOn: $game.handClimb)
                Text("Reach for a pole or a tree, squeeze, and pull your hand down: you rise by as far as your hand came down, then reach higher with the other. At the top he goes into a handstand and the view turns over with him. Let go, or press the jump button, to push off. Off, Mario grabs a pole by jumping into it and climbs with the stick.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Climbing")
            }

            Section {
                Picker("Catching an edge", selection: $game.ledgeMode) {
                    ForEach(GameBridge.LedgeMode.offered) { mode in
                        Text(mode.title).tag(mode)
                    }
                }
                Text("Hanging from a ledge, the game pulls Mario up by itself — from his eyes, or from behind him, where you can see what you are hanging on.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Ledges")
            }

            Section {
                Picker("Hanging from a ceiling", selection: $game.hangMode) {
                    ForEach(GameBridge.HangMode.allCases) { mode in
                        Text(mode.title).tag(mode)
                    }
                }
                Text("Working along a ceiling by hand, from his eyes or from behind him. From behind you can see what you are hanging from and where the ceiling runs out; from his eyes both are above and behind the view.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Ceilings")
            }

            Section {
                Toggle("Fly in first person", isOn: $game.flyFirstPerson)
                Text("A wing cap banks and rolls with no say from you, which is the quickest way to feel unwell in a headset — so the view moves behind Mario for the duration. Turn this on to stay inside his head.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Flying")
            }

            Section {
                Toggle("Ground pound with stick down", isOn: $game.stickDownGroundPound)
                Text("Clicking the left stick ground-pounds, which is how a gamepad does it by default. Off, so pushing the stick hard into a wall does not pound and drop a wall lock. Z on a shoulder or trigger is unchanged.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Ground pound")
            }
        }
        .navigationTitle("Controls")
    }

    private func picker(_ title: LocalizedStringKey,
                        selection: Binding<GameBridge.Scheme>) -> some View {
        Picker(title, selection: selection) {
            ForEach(GameBridge.Scheme.allCases) { scheme in
                Text(scheme.title).tag(scheme)
            }
        }
    }
}

/// Everything that trades performance for image quality.
struct GraphicsSettingsView: View {
    @ObservedObject var game: GameBridge

    var body: some View {
        Form {
            Section {
                Toggle("Smooth distant textures", isOn: $game.mipmaps)
                Text("Keeps reduced copies of every texture for surfaces seen far away. Without them a distant floor is resampled afresh each frame and appears to crawl — which multisampling cannot help, since it happens inside a surface rather than at its edge. The console had none, and with its own small textures hardly needed them.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Textures")
            }

            Section {
                LabeledSlider(title: "Depth",
                              value: $game.stereoScale,
                              range: 0 ... 4,
                              format: { String(format: "%.2f x", $0) })
                Text("How far apart the two eyes sit. 1.00 would be physically correct in the portal, but the world behind it is greatly reduced and reads as flat — 1.50 gives it depth. The 3D window works at a different scale and is far more sensitive: there the near field decides, and a little is a lot. No effect in the 2D window.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                LabeledSlider(title: "Focal plane",
                              value: $game.screenFocus,
                              range: 200 ... 4000,
                              format: { String(format: "%.0f", $0) })
                Text("3D window only: the distance at which both images coincide. Nearer things step out of the screen, further ones lie behind it. Set too far forward the background becomes unpleasant; too far back, the foreground. About 1000 is the character\'s distance from the camera.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Stereo")
            }

            Section {
                Picker("Antialiasing", selection: $game.msaaSamples) {
                    Text("Off").tag(1)
                    Text("2x").tag(2)
                    Text("4x").tag(4)
                    Text("8x").tag(8)
                }
                Text("Smooths polygon edges. Takes effect at once. No function in the simulator — Metal rejects the depth resolve there.")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                LabeledSlider(title: "Supersampling",
                              value: $game.superSampling,
                              range: 1 ... 3,
                              format: { String(format: "%.2f x", $0) })
                Text("Renders into a correspondingly larger texture and averages it down. The most effective lever against ragged edges, because it also works inside textures and at the window\'s own edge. The cost rises with the square — at 2.00 therefore fourfold.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Image quality")
            } footer: {
                Text("The compositor\'s render resolution would be the strongest lever, but it cannot be raised here: any value above the platform\'s own stops the layer from starting.")
            }

            Section {
                LabeledSlider(title: "Size",
                              value: $game.hudScale,
                              range: 1 ... 6,
                              format: { String(format: "%.2f x", $0) })
                LabeledSlider(title: "Height",
                              value: $game.hudDrop,
                              range: -0.5 ... 1.0,
                              format: { String(format: "%+.2f m", -$0) })
                Text("Both VR modes. The readouts keep the arrangement the game gives them — the pane they are laid onto simply grows, which pushes the corners apart in both directions at once. Takes effect at once.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("HUD in VR")
            }

            Section {
                Toggle("Tilt on jumping", isOn: $game.vrTiltEnabled)
                Toggle("Snap instead of easing", isOn: $game.vrTiltInstant)
                LabeledSlider(title: "Angle",
                              value: $game.vrTiltAngle,
                              range: 0 ... 30,
                              format: { String(format: "%.0f°", $0) })
                LabeledSlider(title: "Speed",
                              value: $game.vrTiltSpeed,
                              range: 2 ... 90,
                              format: { String(format: "%.0f°/s", $0) })
                LabeledSlider(title: "Duration",
                              value: $game.vrTiltHold,
                              range: 0.2 ... 4,
                              format: { String(format: "%.1f s", $0) })
                Text("First-person VR only. Off the ground the view eases downwards, so the ground ahead comes into sight without you having to look for it mid-jump. Duration is how long a jump holds the tilt before it lets go again — a long fall should not leave the horizon leaning. Snapping arrives at once and ignores the speed.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Jump tilt")
            }
        }
        .navigationTitle("Graphics")
    }
}

/// Slider with its label and current value on one line - the Form's own
/// labelling puts them on top of each other, which reads badly at this width.
private struct LabeledSlider: View {
    let title: LocalizedStringKey
    @Binding var value: Float
    let range: ClosedRange<Float>
    let format: (Float) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Text(format(value))
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
            Slider(value: $value, in: range)
        }
    }
}
