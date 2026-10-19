import Foundation
import SwiftUI

/// How the game is presented. Mirrors MarioVisionRenderMode in
/// mariovision_portal.h - the renderer and the game thread read the same value.
enum RenderMode: Int, CaseIterable, Identifiable {
    case window2D = 0
    case window3D = 1
    case portal = 2
    case vr = 3
    case firstPerson = 4

    var id: Int { rawValue }

    /// Localised. The strings live in Localizable.strings; English is both the
    /// key language and the fallback, German applies only on a German device.
    var title: String {
        switch self {
        case .window2D: return String(localized: "2D Window")
        case .window3D: return String(localized: "3D Window")
        case .portal:   return String(localized: "3D Portal")
        case .vr:       return String(localized: "3rd-Person VR")
        case .firstPerson: return String(localized: "VR (First Person)")
        }
    }

    /// The 3D window's depth budget is still awkward to set, and third-person VR
    /// puts the camera somewhere the game never meant it to be. Both work; both
    /// can look wrong before they look right.
    var isBeta: Bool { self == .window3D || self == .vr }

    /// What the picker shows. The badge is part of the label rather than a view
    /// beside it, because a picker row collapses to its text on every style
    /// this app can end up using.
    var pickerLabel: String {
        isBeta ? String(format: String(localized: "%@ (Beta)"), title) : title
    }

    var explanation: String {
        switch self {
        case .window2D:
            return String(localized: "The game in a flat window, as on a screen.")
        case .window3D:
            return String(localized: "A screen with depth: the framing stays the game's own, but it sits in space in front of you instead of flat on the glass.")
        case .portal:
            return String(localized: "A window into the world. The camera sits on the glass — move your head and you see past it, sideways.")
        case .vr:
            return String(localized: "Like the portal, without the frame: the world fills your whole field of view.")
        case .firstPerson:
            return String(localized: "You stand where the character stands. He walks where you look; the right stick turns your heading in 30° steps. Of him, only the shadow remains.")
        }
    }

    /// Everything but the flat window runs in the immersive space.
    var usesImmersiveSpace: Bool { self != .window2D }
}

/// Publishes the game's state to SwiftUI and forwards the native UI's commands
/// back into it.
///
/// The game runs on its own thread and does not call back out, so the two flags
/// the interface needs are polled rather than pushed. Commands go the other way
/// through mariovision_*, which queues them for the game thread - see
/// mariovision_ui.c.
@MainActor
final class GameBridge: ObservableObject {
    /// Loaded and sitting in the main menu: Start can do something.
    @Published var isReady = false

    /// Loaded and playing.
    @Published var isRunning = false

    /// Something is showing the game - its window or the portal.
    @Published private(set) var windowOpen = false

    /// Set when the player pressed the menu key; the launcher watches it and
    /// clears it by bringing itself back.
    @Published private(set) var menuRequested = false

    func clearMenuRequest() { menuRequested = false }

    /// Handed over by the launcher while it is on screen, so the menu request
    /// can still open it once it has closed.
    var openWindowAction: OpenWindowAction?

    // MARK: - Portal tuning
    //
    // Pushed straight into the portal's atomics on every change, so the render
    // thread picks them up on its next frame - the sliders act live.

    @Published var stereoScale: Float = 1.5 { didSet { pushTuning() } }
    @Published var portalDistance: Float = 1.2 { didSet { pushTuning() } }
    /// Denominator of the world scale: game units per metre. Larger means a
    /// smaller, more distant-feeling world.
    @Published var worldScaleDivisor: Float = 400 { didSet { pushTuning() } }
    /// Horizontal shift per eye. Positive pulls the two images together.
    @Published var convergence: Float = 0 { didSet { pushTuning() } }
    /// Multiplies the compositor's own opening angle.
    @Published var fovScale: Float = 1.0 { didSet { pushTuning() } }

    // MARK: - Render quality
    //
    // Not part of the portal's live tuning: the sample count is read by the
    // renderer each frame, but the compositor fixes its render resolution when
    // it builds the layer, so that one only lands on the next open.

    @Published var msaaSamples: Int = {
        let stored = UserDefaults.standard.integer(forKey: PortalRenderer.sampleCountKey)
        return stored > 0 ? stored : 4
    }() {
        didSet { UserDefaults.standard.set(msaaSamples, forKey: PortalRenderer.sampleCountKey) }
    }

    /// Switchable while the game runs; the renderer and the game thread both
    /// read it from the shared atomic on their next frame.
    @Published var renderMode: RenderMode = {
        // object(forKey:), not integer(forKey:): an unset key reads as 0, and 0
        // is a valid mode - the flat window - so the fallback would never fire.
        guard let stored = UserDefaults.standard.object(forKey: "mariovision.renderMode") as? Int,
              let mode = RenderMode(rawValue: stored) else { return .portal }
        return mode
    }() {
        didSet {
            UserDefaults.standard.set(renderMode.rawValue, forKey: "mariovision.renderMode")
            mariovision_set_render_mode(Int32(renderMode.rawValue))
        pushSchemes()
        mariovision_set_announce(announceGestures)
        mariovision_set_mipmaps(mipmaps)
        }
    }

    /// Whether textures carry reduced copies of themselves for use at a
    /// distance. The console had none, and with its own thirty-two-pixel
    /// textures it hardly mattered; with a replacement pack it is the whole
    /// difference between a still wall and a shimmering one.
    @Published var mipmaps: Bool = GameBridge.storedFlagOr("mv.mipmaps", true) {
        didSet {
            UserDefaults.standard.set(mipmaps, forKey: "mv.mipmaps")
            mariovision_set_mipmaps(mipmaps)
        }
    }

    /// VR only: how far the HUD's pane is spread, and how far below the centre
    /// it sits.
    @Published var hudScale: Float = {
        let stored = UserDefaults.standard.float(forKey: PortalRenderer.hudScaleKey)
        return stored > 0 ? stored : 3
    }() {
        didSet { UserDefaults.standard.set(hudScale, forKey: PortalRenderer.hudScaleKey) }
    }

    // MARK: - Controls
    //
    // Three things exist twice over - as a movement and as a button - and which
    // one counts is taste, not correctness. Somebody playing seated wants the
    // buttons; somebody standing wants neither firing by accident.

    enum Scheme: Int, CaseIterable, Identifiable {
        case physical = 0
        case button = 1
        case both = 2

        var id: Int { rawValue }
        var title: String {
            switch self {
            case .physical: return String(localized: "With your body")
            case .button:   return String(localized: "With a button")
            case .both:     return String(localized: "Either")
            }
        }
    }

    @Published var swimScheme: Scheme = GameBridge.storedScheme("mv.swimScheme", .both) {
        didSet { storeScheme("mv.swimScheme", 0, swimScheme) }
    }
    @Published var punchScheme: Scheme = GameBridge.storedScheme("mv.punchScheme", .physical) {
        didSet { storeScheme("mv.punchScheme", 1, punchScheme) }
    }
    @Published var carryScheme: Scheme = GameBridge.storedScheme("mv.carryScheme", .physical) {
        didSet { storeScheme("mv.carryScheme", 2, carryScheme) }
    }

    /// Whether doors are swung by hand. Off, and they open by being walked
    /// into, which is how the game has always had it - and the only way to play
    /// seated, or with an ordinary pad in one's lap.
    @Published var handDoors: Bool = GameBridge.storedFlagOr("mv.handDoors", true) {
        didSet {
            UserDefaults.standard.set(handDoors, forKey: "mv.handDoors")
            mariovision_set_hand_doors(handDoors)
        mariovision_set_hand_climb(handClimb)
        mariovision_set_ledge_mode(Int32(ledgeMode.rawValue))
        mariovision_set_hang_mode(Int32(hangMode.rawValue))
        mariovision_set_fly_first_person(flyFirstPerson)
        }
    }

    /// What happens when Mario catches an edge.
    enum LedgeMode: Int, CaseIterable, Identifiable {
        case firstPerson = 0
        case thirdPerson = 1
        /// Built and working, but kept out of the list for now - see the picker
        /// in SettingsView, which offers `offered` rather than `allCases`.
        case byHand = 2

        static let offered: [LedgeMode] = [.firstPerson, .thirdPerson]

        var id: Int { rawValue }
        var title: String {
            switch self {
            case .firstPerson: return String(localized: "Automatic, from his eyes")
            case .thirdPerson: return String(localized: "Automatic, from behind")
            case .byHand:      return String(localized: "By hand")
            }
        }
    }

    @Published var ledgeMode: LedgeMode = {
        let stored = LedgeMode(rawValue:
            UserDefaults.standard.object(forKey: "mv.ledgeMode") as? Int ?? 0) ?? .firstPerson
        // A mode that is no longer offered must not stay switched on behind the
        // list's back.
        return LedgeMode.offered.contains(stored) ? stored : .firstPerson
    }() {
        didSet {
            UserDefaults.standard.set(ledgeMode.rawValue, forKey: "mv.ledgeMode")
            mariovision_set_ledge_mode(Int32(ledgeMode.rawValue))
        mariovision_set_hang_mode(Int32(hangMode.rawValue))
        }
    }

    /// What happens when Mario hangs from a ceiling.
    enum HangMode: Int, CaseIterable, Identifiable {
        case firstPerson = 0
        case thirdPerson = 1

        var id: Int { rawValue }
        var title: String {
            switch self {
            case .firstPerson: return String(localized: "Automatic, from his eyes")
            case .thirdPerson: return String(localized: "Automatic, from behind")
            }
        }
    }

    @Published var hangMode: HangMode =
        HangMode(rawValue: UserDefaults.standard.object(forKey: "mv.hangMode") as? Int ?? 0)
        ?? .firstPerson {
        didSet {
            UserDefaults.standard.set(hangMode.rawValue, forKey: "mv.hangMode")
            mariovision_set_hang_mode(Int32(hangMode.rawValue))
        }
    }

    /// Whether poles and trees are climbed by taking hold of them.
    @Published var handClimb: Bool = GameBridge.storedFlagOr("mv.handClimb", false) {
        didSet {
            UserDefaults.standard.set(handClimb, forKey: "mv.handClimb")
            mariovision_set_hand_climb(handClimb)
        mariovision_set_ledge_mode(Int32(ledgeMode.rawValue))
        mariovision_set_hang_mode(Int32(hangMode.rawValue))
        }
    }

    /// Whether a wing cap keeps the view inside Mario's head.
    @Published var flyFirstPerson: Bool = GameBridge.storedFlagOr("mv.flyFirstPerson", false) {
        didSet {
            UserDefaults.standard.set(flyFirstPerson, forKey: "mv.flyFirstPerson")
            mariovision_set_fly_first_person(flyFirstPerson)
        }
    }

    private func storeScheme(_ key: String, _ what: Int32, _ scheme: Scheme) {
        UserDefaults.standard.set(scheme.rawValue, forKey: key)
        mariovision_set_scheme(what, Int32(scheme.rawValue))
    }

    private static func storedScheme(_ key: String, _ fallback: Scheme) -> Scheme {
        guard let raw = UserDefaults.standard.object(forKey: key) as? Int,
              let scheme = Scheme(rawValue: raw) else { return fallback }
        return scheme
    }

    /// Says out loud what gesture was just recognised. A gesture that does
    /// nothing and a gesture that was never seen look identical from inside a
    /// headset; this tells them apart.
    @Published var announceGestures: Bool = GameBridge.storedFlagOr("mv.announce", false) {
        didSet {
            UserDefaults.standard.set(announceGestures, forKey: "mv.announce")
            mariovision_set_announce(announceGestures)
        }
    }

    /// Whether the hidden page is showing. Ten taps on the Start tab, and it
    /// stays until the app is closed - long enough for an evening's work,
    /// short enough that nobody finds it by accident twice.
    @Published var debugUnlocked = false

    /// Levels one can jump to, and which of them are worth keeping to hand.
    @Published var favouriteLevels: Set<Int> =
        Set(UserDefaults.standard.array(forKey: "mv.favLevels") as? [Int] ?? []) {
        didSet { UserDefaults.standard.set(Array(favouriteLevels), forKey: "mv.favLevels") }
    }

    var levelNames: [String] {
        (0 ..< Int(mariovision_level_count())).map {
            String(cString: mariovision_level_name(Int32($0)))
        }
    }

    func warpToLevel(_ index: Int) { mariovision_level_warp(Int32(index)) }

    /// The texture packs the game found as it came up. Empty before then, and
    /// empty for a pack copied in since - the folders are read once.
    /// Index and name of every pack whose folder is still there.
    ///
    /// The list DynOS keeps only ever grows: a pack it has seen once stays in
    /// it, and deleting the folder does not take it out again. So what is shown
    /// is filtered by whether the folder still exists, or a pack just removed
    /// would sit in the list for the rest of the session, switched on and
    /// pointing at nothing.
    var texturePacks: [(index: Int, name: String)] {
        (0 ..< Int(mariovision_pack_count())).compactMap { i in
            guard mariovision_pack_exists(Int32(i)) else { return nil }
            return (i, String(cString: mariovision_pack_name(Int32(i))))
        }
    }

    func isPackEnabled(_ index: Int) -> Bool { mariovision_pack_enabled(Int32(index)) }

    func setPack(_ index: Int, _ on: Bool) {
        mariovision_pack_set_enabled(Int32(index), on)
        objectWillChange.send()
    }

    /// Nudges the views that read the pack list, which is C state and so has
    /// nothing to publish changes of its own.
    func packsChanged() { objectWillChange.send() }

    private static func storedFlagOr(_ key: String, _ fallback: Bool) -> Bool {
        guard UserDefaults.standard.object(forKey: key) != nil else { return fallback }
        return UserDefaults.standard.bool(forKey: key)
    }

    /// Pushes all three, so a restart does not quietly fall back to the
    /// built-in defaults - a didSet only fires when something is changed.
    func pushSchemes() {
        mariovision_set_scheme(0, Int32(swimScheme.rawValue))
        mariovision_set_scheme(1, Int32(punchScheme.rawValue))
        mariovision_set_scheme(2, Int32(carryScheme.rawValue))
        mariovision_set_hand_doors(handDoors)
        mariovision_set_hand_climb(handClimb)
        mariovision_set_ledge_mode(Int32(ledgeMode.rawValue))
        mariovision_set_hang_mode(Int32(hangMode.rawValue))
        mariovision_set_fly_first_person(flyFirstPerson)
    }

    private static func storedFloat(_ key: String, _ fallback: Float) -> Float {
        guard UserDefaults.standard.object(forKey: key) != nil else { return fallback }
        return UserDefaults.standard.float(forKey: key)
    }

    @Published var hudDrop: Float =
        UserDefaults.standard.float(forKey: PortalRenderer.hudDropKey) {
        didSet { UserDefaults.standard.set(hudDrop, forKey: PortalRenderer.hudDropKey) }
    }

    /// Jump tilt in first-person VR: how far the view eases down, how fast it
    /// gets there, and how long a jump holds it before letting go again. Read
    /// back from the defaults at launch, so the sliders show what the renderer
    /// is actually using rather than a fresh constant.
    private static func stored(_ key: String, _ fallback: Float) -> Float {
        guard UserDefaults.standard.object(forKey: key) != nil else { return fallback }
        return UserDefaults.standard.float(forKey: key)
    }
    private static func storedFlag(_ key: String, _ fallback: Bool) -> Bool {
        guard UserDefaults.standard.object(forKey: key) != nil else { return fallback }
        return UserDefaults.standard.bool(forKey: key)
    }

    @Published var vrTiltEnabled: Bool = GameBridge.storedFlag(PortalRenderer.tiltEnabledKey, false) {
        didSet { UserDefaults.standard.set(vrTiltEnabled, forKey: PortalRenderer.tiltEnabledKey) }
    }
    @Published var vrTiltInstant: Bool = GameBridge.storedFlag(PortalRenderer.tiltInstantKey, false) {
        didSet { UserDefaults.standard.set(vrTiltInstant, forKey: PortalRenderer.tiltInstantKey) }
    }

    @Published var vrTiltAngle: Float = GameBridge.stored(PortalRenderer.tiltAngleKey, 12) {
        didSet { UserDefaults.standard.set(vrTiltAngle, forKey: PortalRenderer.tiltAngleKey) }
    }
    @Published var vrTiltSpeed: Float = GameBridge.stored(PortalRenderer.tiltSpeedKey, 30) {
        didSet { UserDefaults.standard.set(vrTiltSpeed, forKey: PortalRenderer.tiltSpeedKey) }
    }
    @Published var vrTiltHold: Float = GameBridge.stored(PortalRenderer.tiltHoldKey, 1.2) {
        didSet { UserDefaults.standard.set(vrTiltHold, forKey: PortalRenderer.tiltHoldKey) }
    }

    /// 3D window only: where the two pictures coincide, in game units.
    @Published var screenFocus: Float = {
        let stored = UserDefaults.standard.float(forKey: PortalRenderer.screenFocusKey)
        return stored > 0 ? stored : 1000
    }() {
        didSet { UserDefaults.standard.set(screenFocus, forKey: PortalRenderer.screenFocusKey) }
    }

    /// Multiple of the compositor's resolution the portal renders at.
    @Published var superSampling: Float = {
        let stored = UserDefaults.standard.float(forKey: PortalRenderer.superSamplingKey)
        return stored > 0 ? stored : 2
    }() {
        didSet { UserDefaults.standard.set(superSampling, forKey: PortalRenderer.superSamplingKey) }
    }

    private func pushTuning() {
        mariovision_portal_set_tuning(stereoScale, portalDistance,
                                      1.0 / max(worldScaleDivisor, 1),
                                      convergence, fovScale)
    }

    let nativeUIEnabled = mariovision_native_ui_enabled()

    private var timer: Timer?
    private var seenPackCount = -1
    private var seenPackScanning = false
    private var booted = false

    init() {
        // MARIOVISION_MODE=<0..3> forces a mode at launch, so each one can be
        // driven from automation - the simulator has no way to work a picker.
        if let raw = ProcessInfo.processInfo.environment["MARIOVISION_MODE"],
           let n = Int(raw), let mode = RenderMode(rawValue: n) {
            renderMode = mode
        }
        mariovision_set_render_mode(Int32(renderMode.rawValue))

        // Four times a second is plenty for two booleans that only change at
        // the transitions between loading, menu and playing.
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    /// Boots the game engine once the ROM is in place. The game loads with no
    /// window at all - the launcher shows its progress, and the game window
    /// only opens on Start.
    /// True while the review scene stands in for the game.
    let demoMode = RomInstaller.demoActive

    func bootIfPossible() {
        // The engine is not started in demo mode - it has no data to start on.
        // The renderer draws the review scene instead, and everything around it
        // - the modes, the window, the settings - behaves as it always does.
        guard !demoMode else {
            if !booted { booted = true; isReady = true }
            return
        }
        guard !booted, case .present = RomInstaller.check() else { return }
        booted = true
        // Apply before the engine starts, including when Settings was never opened.
        let silverMetal = (UserDefaults.standard.object(forKey: "mv.silverMetal") as? Bool) ?? true
        mariovision_set_silver_metal(silverMetal)
        mariovision_boot()
    }

    deinit {
        timer?.invalidate()
    }

    private func refresh() {
        if demoMode { return }
        let ready = mariovision_game_ready()
        let running = mariovision_game_running()
        let open = mariovision_is_visible()

        if mariovision_take_menu_request() {
            menuRequested = true

            // The launcher closes on start, so the SwiftUI observer that used
            // to react to this - it lives inside that very window - is gone
            // exactly when it is needed. The action itself outlives the view,
            // though: the launcher hands it over while it is up, and it still
            // works afterwards. Declaring an activity type on the Window scene
            // instead tripped a SwiftUI assertion at launch.
            openWindowAction?(id: MarioVisionApp.launcherWindowID)
        }

        // The pack list is C state and publishes nothing of its own. It is
        // filled on the game thread, some way after the download that asked for
        // it has finished - so the one nudge sent when the download completed
        // arrived before there was anything to see, and nothing came after it.
        // Watching it here, where something already ticks, is what makes it
        // appear on its own.
        let packs = Int(mariovision_pack_count())
        let scanning = mariovision_pack_rescanning()
        if packs != seenPackCount || scanning != seenPackScanning {
            seenPackCount = packs
            seenPackScanning = scanning
            objectWillChange.send()
        }

        if ready != isReady { isReady = ready }
        if running != isRunning {
            isRunning = running
            // Push the settings again as the game comes up.
            //
            // Setting them once in init was not enough - after a restart the
            // gesture messages stayed off until the switch was worked by hand,
            // which means something between there and here does not keep them.
            // Rather than chase which, they are simply said again at the moment
            // there is certainly a game to hear them.
            if running {
                pushSchemes()
                mariovision_set_announce(announceGestures)
                mariovision_set_mipmaps(mipmaps)
            }
        }
        if open != windowOpen { windowOpen = open }
    }

    // MARK: - Commands

    func startGame() {
        guard !demoMode else { isRunning = true; return }
        mariovision_start_game()
    }
}
