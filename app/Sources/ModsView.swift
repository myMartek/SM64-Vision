import SwiftUI

/// Texture packs: getting them, and choosing which are in use.
///
/// Its own page rather than a corner of the debug one, because unlike the
/// things hidden there this is meant to be found.
struct ModsView: View {
    @ObservedObject var game: GameBridge
    @StateObject private var pack = TexturePack()

    var body: some View {
        Form {
            Section {
                TexturePackRow(pack: pack)
            } header: {
                Text("High-definition textures")
            } footer: {
                Text("The Render96 pack: six hundred megabytes of hand-made replacements for the original textures. Not shipped with the app — it is somebody else's work under its own licence — so it is fetched on request.")
            }

            Section {
                let packs = game.texturePacks
                if packs.isEmpty {
                    Text("None yet.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(packs, id: \.index) { entry in
                        PackRow(game: game, index: entry.index, name: entry.name)
                    }
                }
            } header: {
                Text("Packs found")
            } footer: {
                Text("Downloading one does not switch it on — the game takes a new pack in disabled, by its own rule. Switching one on applies to everything drawn from then on, so the level you are standing in keeps its old look until you leave it.")
            }
        }
        .navigationTitle("Mods")
        // The pack list lives in C and cannot announce its own changes, so it
        // is re-read whenever this page comes back or the download finishes.
        .onChange(of: pack.state) { _, _ in game.packsChanged() }
    }
}

/// Fetching, showing progress, and getting rid of it again.
struct TexturePackRow: View {
    @ObservedObject var pack: TexturePack

    var body: some View {
        switch pack.state {
        case .absent:
            Button("Download the HD textures") { pack.install() }

        case .downloading(let done):
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("Downloading…")
                    Spacer()
                    Text(String(format: "%.0f %%", done * 100))
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                ProgressView(value: done, total: 1)
                HStack {
                    Spacer()
                    Button("Cancel", role: .cancel) { pack.cancel() }
                        .buttonStyle(.borderless)
                }
            }

        case .unpacking(let done):
            VStack(alignment: .leading, spacing: 6) {
                Text("Unpacking…")
                ProgressView(value: done, total: 1)
            }

        case .installed:
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Label("Installed", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("Remove", role: .destructive) { pack.remove() }
                        .buttonStyle(.borderless)
                }
                Text("Switch it on below. If it is not listed yet, the game is still reading the folders — that takes a moment after a download this size.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

        case .failed(let why):
            VStack(alignment: .leading, spacing: 6) {
                Text(why).font(.caption).foregroundStyle(.secondary)
                Button("Try again") { pack.install() }
            }
        }
    }
}


/// One pack in the list: the switch, and a way to ask what it does.
///
/// Behind a button rather than written underneath, because the list is read
/// mostly by somebody who already knows what they came for, and a paragraph
/// under every line turns four packs into a page of prose.
struct PackRow: View {
    @ObservedObject var game: GameBridge
    let index: Int
    let name: String

    @State private var showingNote = false

    var body: some View {
        HStack {
            Toggle(name, isOn: Binding(get: { game.isPackEnabled(index) },
                                       set: { game.setPack(index, $0) }))
            if let note = PackNotes.note(for: name) {
                Button {
                    showingNote = true
                } label: {
                    Image(systemName: "info.circle")
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(Text("What this pack changes"))
                .popover(isPresented: $showingNote) {
                    Text(note)
                        .font(.callout)
                        .padding()
                        .frame(maxWidth: 320)
                }
            }
        }
    }
}
