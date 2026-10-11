import SwiftUI
import UIKit
import GameController

/// Learn a controller by being shown one.
///
/// Guessing what a pad calls its buttons has cost this project more evenings
/// than anything else: element keys were inferred from display names, and the
/// sticks went dead twice because "Linker Stick" is not what the device answers
/// to. This asks the device instead. One prompt at a time, the press is caught
/// as it happens, and what comes out the far end is the controller's own words.
///
/// Hidden behind a launch flag. New pads do not arrive often enough to earn a
/// place in the menu, and a page that records every button one presses is a
/// strange thing to stumble into.
@MainActor
final class ControllerProbe: ObservableObject {

    /// One thing to press, and what the controller said when it was pressed.
    struct Step: Identifiable {
        let id = UUID()
        let prompt: String
        let hint: String
        var aliases: [String] = []
        var name: String = ""
        var symbol: String = ""
        var kind: String = ""
        var value: String = ""
        var seen: Bool { !aliases.isEmpty || !name.isEmpty }
    }

    @Published var steps: [Step]
    @Published var at = 0
    @Published var controllerName = ""
    @Published var identity: [String: String] = [:]
    @Published var everything: [String] = []
    @Published var exported: String?
    @Published var savedTo: String?

    /// What is actually there, and what has actually arrived. Two different
    /// failures look identical from the outside - a pad the system never vends,
    /// and a pad whose presses go somewhere else - and only one of them is
    /// about being in a window rather than in the immersive view.
    @Published var found: [String] = []
    @Published var events = 0
    @Published var lastSeen = "-"
    @Published var elementCount = 0
    /// Whatever is not resting, shown as it happens - so that a pad which
    /// reports something this does not count as a press is still visible.
    @Published var liveValues: [String] = []

    /// All of them, not the first of them.
    ///
    /// Watching only GCController.controllers().first is why a DualSense on the
    /// same desk answered and the pad being learned did not: the list is in
    /// whatever order the system likes, and a capture screen is exactly the
    /// place where two controllers are plugged in at once.
    private var pads: [GCController] = []
    private var watching: [String: Float] = [:]
    /// Frames to ignore after something was taken down, so that a release or a
    /// stick springing back cannot write the next step by itself.
    private var settle = 0

    init() {
        // The order somebody holding an N64 pad would go through it. Anything a
        // given controller does not have is skipped rather than invented.
        func s(_ p: String, _ h: String) -> Step { Step(prompt: p, hint: h) }
        steps = [
            s("A", "The big blue one"),
            s("B", "The green one beside it"),
            s("Z", "The trigger underneath"),
            s("L", "Left shoulder"),
            s("R", "Right shoulder"),
            s("Start", "The red one in the middle"),
            s("D-pad up", "The cross, upwards"),
            s("D-pad down", "The cross, downwards"),
            s("D-pad left", "The cross, to the left"),
            s("D-pad right", "The cross, to the right"),
            s("C up", "The yellow group, upwards"),
            s("C down", "The yellow group, downwards"),
            s("C left", "The yellow group, to the left"),
            s("C right", "The yellow group, to the right"),
            s("Stick up", "Analogue stick, all the way up"),
            s("Stick down", "Analogue stick, all the way down"),
            s("Stick left", "Analogue stick, all the way left"),
            s("Stick right", "Analogue stick, all the way right"),
            s("Anything left over", "Any button not asked for yet - home, capture, whatever it has"),
        ]
    }

    var done: Bool { at >= steps.count }
    var current: Step? { done ? nil : steps[at] }

    // MARK: - Listening

    private var poll: Timer?
    private var sweeper: Timer?

    func begin() {
        // Keep hearing them when this window is not the thing in front. On
        // visionOS the menu is a window in the room, and a window is rarely
        // what the system considers foremost.
        GCController.shouldMonitorBackgroundEvents = true
        GCController.startWirelessControllerDiscovery(completionHandler: nil)

        for name in [Notification.Name.GCControllerDidConnect,
                     .GCControllerDidBecomeCurrent,
                     .GCControllerDidDisconnect] {
            NotificationCenter.default.addObserver(
                forName: name, object: nil, queue: .main) { [weak self] _ in
                    Task { @MainActor in self?.attach() }
                }
        }
        attach()

        // The list is re-read rather than only waited for. A controller already
        // paired when this opened raises no notification at all, and one that
        // arrives while the immersive view has focus may raise it somewhere
        // this object never hears.
        poll = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.attach() }
        }
        sweeper = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sweep() }
        }
    }

    private func attach() {
        let all = GCController.controllers()
        found = all.map { pad in
            let name = pad.vendorName ?? "?"
            let ext = pad.extendedGamepad != nil ? "extended" : "no extended"
            let profile = pad.physicalInputProfile
            let kind = String(describing: type(of: profile))
            return "\(name) - \(pad.productCategory) - \(ext) - \(kind) - \(profile.elements.count) elements"
        }

        guard !all.isEmpty else {
            controllerName = String(localized: "No controller connected")
            pads = []
            return
        }
        guard all.count != pads.count else { return }
        pads = all
        controllerName = all.compactMap(\.vendorName).joined(separator: ", ")
        // Every one of them. Naming only the first is how the export came out
        // headed "DualSense" while every press in it came from the N64 pad.
        identity = [:]
        for (i, pad) in all.enumerated() {
            let who = pad.vendorName ?? "controller \(i)"
            for (key, value) in Self.describe(pad) {
                identity["\(who) / \(key)"] = value
            }
        }
        everything = all.flatMap { Self.allElements($0) }

        watching = allValues()
    }

    private func allValues() -> [String: Float] {
        var out: [String: Float] = [:]
        for pad in pads {
            let who = pad.vendorName ?? "?"
            for (alias, value) in profileValues(pad) {
                out["\(who) / \(alias)"] = value
            }
        }
        return out
    }

    private func unusedAttachNote() {
        // Read, rather than waited for.
        //
        // Handlers were the first attempt and nothing arrived: this pad offers
        // no extended gamepad, and whatever it does offer either has no
        // elements to hang a handler on or does not call them. Asking the
        // profile what each of its elements reads, thirty times a second, needs
        // none of that to be true - if the device reports a value at all, this
        // sees it change.
    }

    /// Every element's value right now, by the alias it answers to.
    ///
    /// Two families, and a pad answers to whichever it was built for. The older
    /// one - button, axis, direction pad - is what most controllers still vend.
    /// The newer one describes an element by what it can do rather than what it
    /// is, and its values live one level in: a button has a pressed input, a
    /// direction pad has a pair of axes. Reading only the first found thirty
    /// elements and no values at all, which looks exactly like a dead pad.
    private func profileValues(_ pad: GCController) -> [String: Float] {
        var out: [String: Float] = [:]
        for (alias, element) in pad.physicalInputProfile.elements {
            if let b = element as? GCControllerButtonInput { out[alias] = b.value }
            else if let a = element as? GCControllerAxisInput { out[alias] = a.value }
            else if let d = element as? GCControllerDirectionPad {
                out[alias + " .x"] = d.xAxis.value
                out[alias + " .y"] = d.yAxis.value
            }
        }
        if !out.isEmpty { return out }

        for element in pad.input.elements {
            let key = element.aliases.sorted().first ?? String(describing: type(of: element))
            if let b = element as? GCButtonElement {
                out[key] = b.pressedInput.value
            } else if let a = element as? GCAxisElement {
                if let abs = a.absoluteInput { out[key] = abs.value }
            } else if let d = element as? GCDirectionPadElement {
                out[key + " .x"] = d.xyAxes.value.x
                out[key + " .y"] = d.xyAxes.value.y
            } else if let sw = element as? GCSwitchElement {
                out[key] = Float(sw.positionInput.position)
            }
        }
        return out
    }

    private func sweep() {
        guard !pads.isEmpty else { return }
        let now = allValues()
        if settle > 0 { settle -= 1 }
        elementCount = now.count
        liveValues = now
            .filter { abs($0.value) > 0.15 }
            .sorted { $0.key < $1.key }
            .prefix(8)
            .map { "\($0.key) = \(String(format: "%.2f", $0.value))" }
        defer { watching = now }
        guard !watching.isEmpty else { return }

        // The moment it goes down, and not the moment it comes back up.
        //
        // Counting movement counted both: one press wrote itself into the step
        // that was current when it went down and into the next one when it came
        // up, so a single A gave A and B. And it undid "back" a thirtieth of a
        // second after it was pressed, because the release was still to come.
        var bestAlias = ""
        var bestValue: Float = 0
        for (alias, value) in now {
            let before = watching[alias] ?? 0
            guard abs(value) >= Self.deflection, abs(before) < Self.deflection else { continue }
            if abs(value) > abs(bestValue) { bestAlias = alias; bestValue = value }
        }
        guard !bestAlias.isEmpty else { return }

        // And nothing at all for a moment afterwards. A stick let go of springs
        // past centre and back, and a button one holds a little too long is one
        // press however many frames it lasts.
        guard settle <= 0 else { return }
        settle = 12

        events += 1
        lastSeen = "\(bestAlias) = \(String(format: "%.2f", bestValue))"
        guard !done else { return }

        var step = steps[at]
        step.aliases = [bestAlias]
        step.value = String(format: "%.2f", bestValue)
        steps[at] = step
        at += 1
    }

    /// Big enough not to catch a resting stick or a fingernail.
    private static let deflection: Float = 0.6

    // MARK: - Moving about

    func back() {
        guard at > 0 else { return }
        // Long enough to have let go of whatever was pressed before reaching
        // for this button, or the release writes the step straight back in.
        settle = 20
        at -= 1
        steps[at].aliases = []
        steps[at].name = ""
        steps[at].symbol = ""
        steps[at].kind = ""
        steps[at].value = ""
    }

    func skip() {
        guard !done else { return }
        at += 1
    }

    func startOver() {
        for i in steps.indices {
            steps[i].aliases = []
            steps[i].name = ""
            steps[i].symbol = ""
            steps[i].kind = ""
            steps[i].value = ""
        }
        at = 0
        exported = nil
        savedTo = nil
    }

    // MARK: - What the controller is

    private static func describe(_ pad: GCController) -> [String: String] {
        var out: [String: String] = [:]
        out["vendorName"] = pad.vendorName ?? ""
        out["productCategory"] = pad.productCategory
        out["isAttachedToDevice"] = pad.isAttachedToDevice ? "true" : "false"
        out["playerIndex"] = String(pad.playerIndex.rawValue)
        out["hasExtendedGamepad"] = pad.extendedGamepad != nil ? "true" : "false"
        out["hasMicroGamepad"] = pad.microGamepad != nil ? "true" : "false"
        out["hasHaptics"] = pad.haptics != nil ? "true" : "false"
        out["hasLight"] = pad.light != nil ? "true" : "false"
        out["hasMotion"] = pad.motion != nil ? "true" : "false"
        out["hasBattery"] = pad.battery != nil ? "true" : "false"
        out["profileType"] = String(describing: type(of: pad.physicalInputProfile))
        return out
    }

    /// Every alias the profile answers to, with what it is bound to. This is the
    /// list that matters when writing the binding afterwards: the names it will
    /// actually accept, rather than the ones one might expect it to.
    private static func allElements(_ pad: GCController) -> [String] {
        pad.physicalInputProfile.elements
            .map { alias, element in
                let name = element.localizedName ?? ""
                let kind = String(describing: type(of: element))
                return "\(alias)  ->  \(kind)  \"\(name)\""
            }
            .sorted()
    }

    // MARK: - Handing it over

    func export() {
        var lines: [String] = []
        lines.append("# Controller mapping")
        lines.append("")
        lines.append("## Identity")
        for key in identity.keys.sorted() {
            lines.append("\(key): \(identity[key] ?? "")")
        }
        lines.append("")
        lines.append("## Mapping")
        for step in steps {
            if step.seen {
                let aliases = step.aliases.isEmpty ? "(no alias)" : step.aliases.joined(separator: ", ")
                lines.append("\(step.prompt): \(aliases) | \(step.kind) | \"\(step.name)\" | symbol \(step.symbol) | \(step.value)")
            } else {
                lines.append("\(step.prompt): skipped")
            }
        }
        lines.append("")
        lines.append("## Every element the profile answers to")
        lines.append(contentsOf: everything)

        let text = lines.joined(separator: "\n")
        exported = text

        // Written down as well as shown. A page of this read off a screen in a
        // headset and typed out again is how transcription mistakes get into a
        // binding, which is the one place they are hardest to find later.
        let url = RomInstaller.writeDirectory
            .appendingPathComponent("controller-mapping.txt")
        try? FileManager.default.createDirectory(at: RomInstaller.writeDirectory,
                                                 withIntermediateDirectories: true)
        if (try? text.write(to: url, atomically: true, encoding: .utf8)) != nil {
            savedTo = url.path
        }
    }
}

struct ControllerSetupView: View {
    @StateObject private var probe = ControllerProbe()
    @State private var showingExport = false
    @State private var copied = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Text(probe.controllerName).font(.headline)
                    if probe.identity["hasExtendedGamepad"] == "false" {
                        Text("This one offers no extended gamepad, so the ordinary binding path will not see it at all. Worth knowing before anything else.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    ForEach(probe.found, id: \.self) { line in
                        Text(line).font(.caption).monospaced()
                    }
                    Text("Elements: \(probe.elementCount) · events: \(probe.events)")
                        .font(.headline)
                    Text("Last: \(probe.lastSeen)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if probe.elementCount == 0 {
                        Text("The profile has nothing in it. Then there is nothing to read, and the question is what this pad is being vended as — not where its presses go.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    ForEach(probe.liveValues, id: \.self) { line in
                        Text(line).font(.caption2).monospaced()
                    }
                    if probe.events == 0 && !probe.found.isEmpty {
                        Text("The controller is there but nothing it does reaches this window. That is the case where the immersive view has to be open — start the game, then bring this window back with the menu button.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Controller")
                }

                if let step = probe.current {
                    Section {
                        Text(step.prompt)
                            .font(.system(size: 34, weight: .semibold))
                        Text(step.hint)
                            .foregroundStyle(.secondary)
                        Text("\(probe.at + 1) of \(probe.steps.count)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        HStack {
                            Button("Back") { probe.back() }
                                .disabled(probe.at == 0)
                            Spacer()
                            Button("It has none of these") { probe.skip() }
                        }
                    } header: {
                        Text("Press this")
                    }
                } else {
                    Section {
                        Text("Every prompt has been answered.")
                    } header: {
                        Text("Done")
                    }
                }

                Section {
                    Button {
                        probe.export()
                        showingExport = true
                    } label: {
                        Label("Export", systemImage: "square.and.arrow.up")
                    }
                    Button("Start over", role: .destructive) { probe.startOver() }
                }

                Section {
                    ForEach(probe.steps.filter(\.seen)) { step in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(step.prompt).font(.headline)
                            Text(step.aliases.isEmpty ? "(no alias)" : step.aliases.joined(separator: ", "))
                                .font(.caption)
                                .monospaced()
                            Text("\(step.kind) · \(step.value)")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                } header: {
                    Text("Recorded so far")
                }

            }
            .navigationTitle("Controller setup")
        }
        // Take the controller off the system while this page is up.
        //
        // Without it the pad drives the window - moving focus, pressing
        // buttons - and the app never sees a thing, which is precisely what
        // happened: the controller was found, and every press went to the
        // system's own navigation. This says the view will handle them itself.
        .handlesGameControllerEvents(matching: .gamepad)
        .onAppear { probe.begin() }
        // In front, not appended.
        //
        // It used to be one more section at the bottom of a long form, which in
        // a headset is a place nobody goes - the button looked as though it did
        // nothing at all. A sheet cannot be missed, and it has room for the one
        // thing that actually matters here: a copy button, so that a page of
        // element names never has to be read off a screen and typed again.
        .sheet(isPresented: $showingExport) {
            NavigationStack {
                ScrollView {
                    Text(probe.exported ?? "")
                        .font(.system(size: 12, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding()
                }
                .navigationTitle("Export")
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button(copied ? "Copied" : "Copy") {
                            UIPasteboard.general.string = probe.exported ?? ""
                            copied = true
                        }
                    }
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Close") { showingExport = false; copied = false }
                    }
                }
                .safeAreaInset(edge: .bottom) {
                    if let path = probe.savedTo {
                        Text("Also written to \(path)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                            .padding(8)
                    }
                }
            }
            .frame(minWidth: 700, minHeight: 500)
        }
    }
}

extension ControllerSetupView {
    /// Only when asked for at launch: MARIOVISION_CONTROLLERSETUP=1.
    static var wanted: Bool {
        ProcessInfo.processInfo.environment["MARIOVISION_CONTROLLERSETUP"] != nil
            || ProcessInfo.processInfo.arguments.contains("-controller-setup")
    }
}
