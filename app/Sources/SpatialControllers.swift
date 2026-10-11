import ARKit
import GameController
import simd

/// Finds tracked controllers and hands their poses to the game.
///
/// The poses go through the very transform the eye already goes through - see
/// PortalRenderer's first-person block - so there is one coordinate chain in
/// this app rather than two that have to agree. The arithmetic itself lives in
/// C (mariovision_hands_math.c) where a host test can reach it; what is left
/// here is finding the hardware and asking ARKit for anchors.
///
/// Everything degrades to nothing. No controllers, no permission, or an
/// older visionOS, and the game never hears from this class at all - which is
/// what keeps a session with a gamepad running the code it has always run.
/// Not on the main actor: `publish` is called from the render thread, sixty or
/// ninety times a second, and hopping actors to fetch a hand pose would defeat
/// the point of predicting it. Setup and the connect notifications do arrive
/// elsewhere, though, so the one piece of state both touch - the provider - is
/// taken under a lock. ARKit's own anchor list is documented as unsafe to share
/// across threads, and only the render thread ever reads it.
final class SpatialControllers: @unchecked Sendable {

    /// Whether anything is being tracked right now. Read for the log, not for
    /// correctness: the game decides per hand, per frame, from what arrives.
    private(set) var isTracking = false

    private var session: ARKitSession?
    private var accessories: [ARKit.Accessory] = []
    private var authorized = false

    private let lock = NSLock()
    private var lockedProvider: AccessoryTrackingProvider?

    private var provider: AccessoryTrackingProvider? {
        get { lock.lock(); defer { lock.unlock() }; return lockedProvider }
        set { lock.lock(); lockedProvider = newValue; lock.unlock() }
    }

    /// The providers to run alongside world tracking. Empty when there is
    /// nothing to track, so the caller can splice it in blindly.
    var providers: [any DataProvider] {
        guard let provider else { return [] }
        return [provider]
    }

    // MARK: - Setup

    func prepare(session: ARKitSession) async {
        self.session = session

        guard AccessoryTrackingProvider.isSupported else {
            log("this system does not track accessories")
            return
        }

        let granted = await session.requestAuthorization(
            for: AccessoryTrackingProvider.requiredAuthorizations)
        guard granted[.accessoryTracking] == .allowed else {
            log("permission to track accessories was refused")
            return
        }
        authorized = true

        await rebuild()
        observeConnections()
    }

    /// Rebuilds the accessory list and the provider around it.
    private func rebuild() async {
        var found: [ARKit.Accessory] = []
        for device in spatialDevices() {
            do {
                let accessory = try await ARKit.Accessory(device: device)
                log("opened \"\(accessory.name)\", chirality \(accessory.inherentChirality), "
                    + "locations \(accessory.locations.map(\.rawValue))")
                found.append(accessory)
            } catch {
                log("could not open a controller: \(error)")
            }
        }
        accessories = found
        // Built even with an empty list, so the session can be started once
        // with it in place and the provider is there when a controller
        // arrives.
        provider = AccessoryTrackingProvider(accessories: found)
        log("\(found.count) tracked controller(s)")
    }

    private func observeConnections() {
        // Both the ordinary controller notifications and, where the system has
        // them, the spatial ones. The names are looked up as strings rather
        // than referenced directly: the spatial ones only exist in a newer SDK
        // than this project builds against, and pinning the whole build to a
        // beta toolchain for two constants is a poor trade.
        let names = ["GCControllerDidConnectNotification",
                     "GCControllerDidDisconnectNotification",
                     "GCSpatialAccessoryDidConnectNotification",
                     "GCSpatialAccessoryDidDisconnectNotification"]

        for name in names {
            NotificationCenter.default.addObserver(
                forName: Notification.Name(name), object: nil, queue: .main
            ) { [weak self] _ in
                Task { await self?.devicesChanged() }
            }
        }
    }

    private func devicesChanged() async {
        guard authorized, let session else { return }
        await rebuild()
        // Running again is the blunt way to change the tracked set, and it is
        // what works on every version this app supports. World tracking goes
        // back in the same call so it is never left out; the first-person
        // anchor survives because PortalRenderer only captures it while it is
        // still nil.
        try? await session.run([worldTrackingPassthrough].compactMap { $0 } + providers)
    }

    /// Set by the renderer so a re-run can put world tracking back.
    var worldTrackingPassthrough: (any DataProvider)?

    /// The devices worth asking ARKit about.
    ///
    /// Sense controllers are not ordinary `GCController`s - a single one cannot
    /// offer a full gamepad profile, and GameController only enumerates devices
    /// that can - so newer systems surface them as `GCSpatialAccessory`. That
    /// class is looked up by name because it postdates the SDK this builds
    /// against; where it is absent, the product category catches whatever the
    /// older path does expose.
    ///
    /// The pair also presents itself as one ordinary gamepad, and that handle
    /// is what locomotion already uses. It is deliberately left alone.
    private func spatialDevices() -> [any GCDevice] {
        var found: [any GCDevice] = []

        // The class object answers its own class property. perform, not
        // value(forKey:): key-value coding against a metaclass is not a
        // contract anybody promises, and the first attempt at it came back
        // empty on a headset that certainly had the class.
        let selector = Selector(("spatialAccessories"))
        if let cls = NSClassFromString("GCSpatialAccessory") as AnyObject? {
            if cls.responds(to: selector),
               let boxed = cls.perform(selector)?.takeUnretainedValue() as? [any GCDevice] {
                found = boxed
                log("GCSpatialAccessory reports \(boxed.count)")
            } else {
                log("GCSpatialAccessory is present but would not answer")
            }
        } else {
            log("no GCSpatialAccessory on this system")
        }

        // Whatever the ordinary controller list holds, named out loud. Which
        // object is which is only visible from the category: with the spatial
        // profile declared the pair arrives as two separate controllers, and
        // without it as one coalesced MFi gamepad that cannot be tracked.
        //
        // The decision to fall back is taken once, before the loop. Asking
        // "is the list still empty" inside it meant the first controller
        // filled it and the second was skipped - which showed up as a right
        // arm that followed and a left one that did not.
        let needFallback = found.isEmpty
        for controller in GCController.controllers() {
            log("gamepad \"\(controller.vendorName ?? "?")\" category=\(controller.productCategory)")
            if needFallback, controller.productCategory == GCProductCategorySpatialController {
                found.append(controller)
            }
        }

        return found
    }

    // MARK: - Per frame

    /// Samples both controllers and pushes them to the game.
    ///
    /// `anchored` is the room-space frame the first-person mode was entered
    /// with; `timestamp` is the instant the head was sampled for this frame.
    /// Predicting to the same instant rather than taking the latest anchor is
    /// what keeps a hand from lagging the head it is drawn beside.
    func publish(anchored: simd_float4x4, timestamp: TimeInterval) {
        guard let provider, provider.state == .running else {
            clearBoth()
            return
        }

        var seen = Set<Int32>()
        let inverse = anchored.inverse

        for latest in provider.latestAnchors {
            let anchor = provider.predictAnchor(for: latest, at: timestamp) ?? latest
            guard let side = Self.side(of: anchor) else { continue }
            // Orientation without a position, or nothing at all: either way the
            // position cannot be trusted, and a hand that jumps to a guessed
            // one reads far worse than one that holds still.
            guard anchor.trackingState == .positionOrientationTracked else { continue }

            let grip = Self.grip(of: anchor)
            var pos = [Float](repeating: 0, count: 3)
            var quat: [Float] = [0, 0, 0, 1]
            Self.toCameraSpace(inverse: inverse, room: grip, pos: &pos, quat: &quat)

            var pose = MarioVisionHandPose()
            pose.pos = (pos[0], pos[1], pos[2])
            pose.quat = (quat[0], quat[1], quat[2], quat[3])
            pose.flags = UInt32(MV_HAND_TRACKED)
            mariovision_set_hand_pose(side, &pose)
            seen.insert(side)
        }

        // A hand that stopped reporting has to be told so, or the game keeps
        // using its last pose for ever and the arm hangs in mid-air.
        for side in [MV_HAND_LEFT, MV_HAND_RIGHT] where !seen.contains(Int32(side)) {
            mariovision_clear_hand(Int32(side))
        }

        let now = !seen.isEmpty
        if now != isTracking {
            isTracking = now
            log(now ? "controllers tracked" : "controllers lost")
        }
    }

    private func clearBoth() {
        guard isTracking else { return }
        mariovision_clear_hand(Int32(MV_HAND_LEFT))
        mariovision_clear_hand(Int32(MV_HAND_RIGHT))
        isTracking = false
    }

    // MARK: - Details

    /// A hundred game units to the metre, matching the first-person world scale
    /// the eye already goes through.
    private static let metresToUnits: Float = 100

    private static func toCameraSpace(inverse: simd_float4x4, room: simd_float4x4,
                                      pos: inout [Float], quat: inout [Float]) {
        var inv = inverse
        var rm = room
        withUnsafeBytes(of: &inv) { invBytes in
            withUnsafeBytes(of: &rm) { roomBytes in
                mv_room_to_camera(invBytes.baseAddress!.assumingMemoryBound(to: Float.self),
                                  roomBytes.baseAddress!.assumingMemoryBound(to: Float.self),
                                  metresToUnits, &pos, &quat)
            }
        }
    }

    /// Which hand this is.
    ///
    /// What a controller is designed for is only a hint; which hand actually
    /// holds it is the truth, and on a pair picked up the wrong way round the
    /// two disagree.
    private static func side(of anchor: AccessoryAnchor) -> Int32? {
        switch anchor.heldChirality ?? anchor.accessory.inherentChirality {
        case .left:  return Int32(MV_HAND_LEFT)
        case .right: return Int32(MV_HAND_RIGHT)
        default:     return nil
        }
    }

    /// The grip: the point the hand closes around, which is where Mario's hand
    /// root belongs. The aim location would put it where a laser pointer starts.
    private static func grip(of anchor: AccessoryAnchor) -> simd_float4x4 {
        guard anchor.accessory.locations.contains(.grip) else {
            return anchor.originFromAnchorTransform
        }
        let space = anchor.coordinateSpace(for: .grip, correction: .rendered)
        return space.ancestorFromSpaceTransformFloat().matrix
    }

    private func log(_ message: String) {
        // stderr, like everything else here: os_log does not reach the console
        // of a tethered headset, which is where this gets read.
        FileHandle.standardError.write(Data("[hands] \(message)\n".utf8))
    }
}
