import SwiftUI

/// What this is, and where to say that it is broken.
///
/// Worth a page of its own rather than a line in the settings: somebody who has
/// just watched a door swing the wrong way wants to know whether that is
/// expected before they go looking for somewhere to report it. So the state of
/// the thing comes first and the address second.
struct FeedbackView: View {
    private static let project = URL(string: "https://github.com/myMartek/SM64-Vision")!

    @Environment(\.openURL) private var openURL

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text("This is a proof of concept.")
                        .font(.headline)
                    Text("It is far from completely tested. Whole parts of the game have never been played through in a headset, and the things that are new here — swinging doors by hand, climbing a tree, throwing what you are holding — were built against a handful of evenings' worth of trying them out. Expect to find something wrong.")
                }

                Section {
                    Text("Bugs, and anything that feels wrong, belong on the project page. What helps most: which level you were in, what you were doing, and whether it happens every time.")
                    Button {
                        openURL(Self.project)
                    } label: {
                        Label("Open the project on GitHub", systemImage: "arrow.up.right.square")
                    }
                    Text(verbatim: Self.project.absoluteString)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                } header: {
                    Text("Reporting something")
                }
            }
            .navigationTitle("Feedback")
        }
    }
}
