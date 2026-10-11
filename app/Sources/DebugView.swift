import SwiftUI

/// The hidden page. Ten taps on the Start tab reveal it, and it goes away again
/// when the app is closed.
///
/// Everything here exists to shorten a loop: reaching a particular fight, or
/// telling a gesture that was not recognised from one that was recognised and
/// did nothing. None of it is meant for anybody who has not gone looking.
struct DebugView: View {
    @ObservedObject var game: GameBridge

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Name each gesture", isOn: $game.announceGestures)
                    Text("Shows what was just recognised in the game's own message banner. A gesture that does nothing and a gesture that was never seen look identical from inside a headset — this tells them apart.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } header: {
                    Text("Gestures")
                }

                Section {
                    NavigationLink("Teleport to a level") {
                        LevelJumpView(game: game)
                    }
                    .disabled(!game.isRunning)
                    if !game.isRunning {
                        Text("Only once a game is running — before that there is nothing to warp out of.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Getting about")
                }

            }
            .navigationTitle("Debug")
        }
    }
}

/// Everywhere worth jumping to, favourites first.
struct LevelJumpView: View {
    @ObservedObject var game: GameBridge
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        let names = game.levelNames
        let favourites = names.indices.filter { game.favouriteLevels.contains($0) }
        let rest = names.indices.filter { !game.favouriteLevels.contains($0) }

        return Form {
            if !favourites.isEmpty {
                Section {
                    ForEach(favourites, id: \.self) { row(index: $0, names: names) }
                } header: {
                    Text("Favourites")
                }
            }

            Section {
                ForEach(rest, id: \.self) { row(index: $0, names: names) }
            } header: {
                Text(favourites.isEmpty ? "Levels" : "Everywhere else")
            } footer: {
                Text("The star keeps one to hand. Jumping takes effect at once, so the launcher closes with it.")
            }
        }
        .navigationTitle("Teleport")
    }

    private func row(index: Int, names: [String]) -> some View {
        HStack {
            Button(names[index]) {
                game.warpToLevel(index)
                dismiss()
            }
            Spacer()
            Button {
                if game.favouriteLevels.contains(index) {
                    game.favouriteLevels.remove(index)
                } else {
                    game.favouriteLevels.insert(index)
                }
            } label: {
                Image(systemName: game.favouriteLevels.contains(index) ? "star.fill" : "star")
            }
            .buttonStyle(.borderless)
        }
    }
}


