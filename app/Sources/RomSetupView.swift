import SwiftUI
import UniformTypeIdentifiers

struct RomSetupView: View {
    let expectedPath: String
    let onInstalled: () -> Void

    @State private var picking = false
    @State private var errorMessage: String?

    private var romType: UTType {
        UTType(filenameExtension: "z64") ?? .data
    }

    var body: some View {
        VStack(spacing: 24) {
            Text("SM64 Vision")
                .font(.largeTitle.weight(.semibold))

            Text("SM64 Vision needs your own, unmodified ROM of the US release. It is copied into the app\'s folder once and never passed on.")
                .multilineTextAlignment(.center)
                .frame(maxWidth: 460)
                .foregroundStyle(.secondary)

            Button("Choose ROM…") { picking = true }
                .buttonStyle(.borderedProminent)

            if let errorMessage {
                Text(errorMessage)
                    .font(.callout)
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 460)
            }

            Text(expectedPath)
                .font(.caption.monospaced())
                .foregroundStyle(.tertiary)
                .textSelection(.enabled)
        }
        .padding(40)
        .fileImporter(isPresented: $picking,
                      allowedContentTypes: [romType, .data],
                      allowsMultipleSelection: false) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                do {
                    try RomInstaller.install(from: url)
                    errorMessage = nil
                    onInstalled()
                } catch {
                    errorMessage = error.localizedDescription
                }
            case .failure(let error):
                errorMessage = error.localizedDescription
            }
        }
    }
}
