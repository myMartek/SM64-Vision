import CompositorServices
import SwiftUI
import ImageIO
import UniformTypeIdentifiers
import ARKit
import Metal
import QuartzCore
import simd

/// The portal render loop: stereo rendering of the game world behind a virtual
/// window, via CompositorServices.
///
/// Split of responsibilities with the C side (mariovision_portal.h): the game
/// thread simulates at 30 Hz and publishes display lists; this loop runs at
/// the display rate, computes fresh per-eye matrices from the headset pose and
/// replays the latest list once per eye. Head motion is therefore smooth at
/// 90 Hz even though the game itself ticks at 30.
final class PortalRenderer {
    /// 1.0 m x 0.75 m of glass (the game's 4:3), world scale 1/400 - Mario
    /// stands about 40 cm tall behind it.
    static let windowHalfWidth: Float = 0.5
    static let windowHalfHeight: Float = 0.375
    static let defaultWorldScale: Float = 1.0 / 400.0

    static let hudScaleKey = "mariovision.hudScale"
    static let hudDropKey = "mariovision.hudDrop"

    /// How much bigger the HUD's pane is in VR than the portal's, and how far
    /// below the centre it sits. Read fresh each frame so the sliders act live.
    private var hudScale: Float {
        let stored = UserDefaults.standard.float(forKey: Self.hudScaleKey)
        return stored > 0 ? stored : 3
    }
    private var hudDrop: Float {
        UserDefaults.standard.float(forKey: Self.hudDropKey)
    }

    /// Degrees of tilt at most, degrees per second getting there, and how long a
    /// jump keeps it before it eases back on its own.
    ///
    /// Read through object(forKey:) rather than float(forKey:) alone: an unset
    /// key and a stored zero both read as zero, so the earlier "zero means never
    /// touched" shortcut quietly ignored an angle the user had deliberately
    /// turned down to nothing. Presence is the question; the value answers it.
    private static func storedFloat(_ key: String, _ fallback: Float) -> Float {
        guard UserDefaults.standard.object(forKey: key) != nil else { return fallback }
        return UserDefaults.standard.float(forKey: key)
    }
    private static func storedBool(_ key: String, _ fallback: Bool) -> Bool {
        guard UserDefaults.standard.object(forKey: key) != nil else { return fallback }
        return UserDefaults.standard.bool(forKey: key)
    }

    // Off by default: tried on device and it did not help. Kept because the
    // idea is sound and the shape of it may be wrong rather than the idea.
    private var tiltEnabled: Bool { Self.storedBool(Self.tiltEnabledKey, false) }
    /// Snap straight to the angle instead of easing into it. Harsher, but it
    /// puts the ground in view the instant the jump starts.
    private var tiltInstant: Bool { Self.storedBool(Self.tiltInstantKey, false) }
    private var tiltAngle: Float { Self.storedFloat(Self.tiltAngleKey, 12) }
    private var tiltSpeed: Float { Self.storedFloat(Self.tiltSpeedKey, 30) }
    private var tiltHold: Float { Self.storedFloat(Self.tiltHoldKey, 1.2) }

    /// Where the tilt should be heading right now, in degrees.
    private func tiltTarget(now: Double) -> Float {
        guard isFirstPerson, tiltEnabled, tiltAngle > 0 else { return 0 }
        guard mariovision_first_person_airborne() else {
            airborneSince = nil
            return 0
        }
        let since = airborneSince ?? now
        if airborneSince == nil { airborneSince = now }
        // A long fall should not leave the horizon tilted indefinitely, so the
        // tilt lets go again after its own time even while still in the air.
        if tiltHold > 0, Float(now - since) > tiltHold { return 0 }
        return tiltAngle
    }

    /// Move towards the target at the configured rate and return the rotation
    /// to fold into the placement. Rx by a positive angle lifts what lies ahead,
    /// which reads as the view looking down.
    private func jumpTilt(now: Double) -> simd_float4x4? {
        let dt = Float(min(max(now - (lastFrameTime ?? now), 0), 0.1))
        lastFrameTime = now

        let target = tiltTarget(now: now)
        if tiltInstant {
            tiltCurrent = target
        } else {
            let step = max(tiltSpeed, 0.1) * dt
            if abs(target - tiltCurrent) <= step {
                tiltCurrent = target
            } else {
                tiltCurrent += (target > tiltCurrent) ? step : -step
            }
        }
        guard abs(tiltCurrent) > 0.01 else { return nil }

        let a = tiltCurrent * .pi / 180
        let c = cos(a), s = sin(a)
        return simd_float4x4(columns: (SIMD4<Float>(1, 0, 0, 0),
                                       SIMD4<Float>(0, c, s, 0),
                                       SIMD4<Float>(0, -s, c, 0),
                                       SIMD4<Float>(0, 0, 0, 1)))
    }

    /// Set once from the first valid head pose: 1.2 m in front of the eyes,
    /// centre a hand's width below eye height, facing the user. Fixed in the
    /// room from then on - that is the whole point of a portal. The space
    /// origin is useless for this: on device it sits on the floor somewhere in
    /// the room, in the simulator head tracking never delivers at all.
    private var placement: simd_float4x4?
    private var framesWithoutAnchor = 0
    private var loggedEyes = false

    private var placedDistance: Float = 0

    /// The pinches currently held, one per hand, keyed by the gesture they
    /// belong to. Written by the event handler on whatever thread it arrives
    /// on, read once a frame by the render loop, which does all the placement
    /// maths - so the window pose stays owned by a single thread.
    ///
    /// One entry per hand matters: a single shared slot got overwritten by the
    /// left and right hand in turn, so the window flipped between two poses
    /// from frame to frame and read as two images, one per eye.
    private struct PinchState {
        var order: UInt64                 // who grabbed first
        var origin = SIMD3<Float>(repeating: 0)
        var direction = SIMD3<Float>(0, 0, -1)
        var hand = SIMD3<Float>(repeating: 0)
        var hasHand = false
    }
    private let pinchLock = NSLock()
    private var pinches: [SpatialEventCollection.Event.ID: PinchState] = [:]
    private var pinchOrder: UInt64 = 0

    /// What holds the window, and what the pose looked like when it took hold.
    /// Re-anchored whenever the number of hands changes, so gaining or losing
    /// the second hand does not make the window jump.
    private var holdOrder: UInt64?
    private var rejectedOrder: UInt64?
    private var handCount = 0
    private var dragPivot = SIMD3<Float>(repeating: 0)
    private var dragReach: Float = 0
    private var dragHandDistance: Float = 0
    private var dragOffset = SIMD3<Float>(repeating: 0)
    private var spanAtGrab: Float = 0
    private var scaleAtGrab: Float = 1

    /// Where the game camera's origin - Mario's head - is pinned in the room,
    /// and the room heading the mode was entered facing. Taken once on entry so
    /// that leaning and stepping give real parallax: if it followed the head
    /// every frame the world would be glued to it and no head movement would
    /// ever be felt.
    private var firstPersonAnchor: simd_float4x4?
    private var firstPersonAnchorYaw: Float = 0
    private var firstPersonAnchorPitch: Float = 0
    /// The head's last position in the room, for measuring how far the wearer
    /// walked. In the room and not against the anchor, because the anchor moves
    /// in response to that very measurement - reading it there is a loop, and
    /// it showed as the world shivering after every return to the space.
    private var prevHeadRoom: SIMD3<Float>?

    /// The pane the HUD is laid onto in first person. The world placement is no
    /// use for it: that one sits on Mario's eyes, which is where the game camera
    /// is, so a pane built from it lands at zero distance from the viewer's own.
    /// This one is rebuilt in front of the head each frame, levelled, so the
    /// readouts stay in view however you turn - and only yaw follows, because a
    /// pane that pitches and rolls with the head is unpleasant to read.
    private var firstPersonHud: simd_float4x4?

    /// Jump tilt. Off the ground the world is eased upwards - which is the same
    /// as the view easing down - so the ground ahead comes into sight without
    /// the player having to look for it while in the air. Run here rather than
    /// in the game because it has to move at display rate: a tilt that stepped
    /// at 30 Hz would be felt as a stutter in the horizon, which is the one
    /// place a headset is least forgiving.
    static let tiltEnabledKey = "mariovision.vrTiltEnabled"
    static let tiltInstantKey = "mariovision.vrTiltInstant"
    static let tiltAngleKey = "mariovision.vrTiltAngle"
    static let tiltSpeedKey = "mariovision.vrTiltSpeed"
    static let tiltHoldKey = "mariovision.vrTiltHold"

    private var tiltCurrent: Float = 0        // degrees, positive tilts the view down
    private var airborneSince: Double?
    private var lastFrameTime: Double?

    /// Mario stands about 160 game units tall and his eyes sit at 120. At 1/100
    /// that puts the viewpoint at 1.2 m - about right for someone his size.
    private static let firstPersonWorldScale: Float = 1.0 / 100.0

    /// Latched once a frame: the game hands the view back to third person for
    /// climbing and cannons, and a value that changed halfway through a frame
    /// would draw one eye from Mario's head and the other from outside it.
    private var isFirstPerson = false

    /// Everything the mode owns has to be rebuilt when it comes and goes - the
    /// placement above all, which sits on Mario's eyes in one and out in front
    /// of them in the other.
    private func updateFirstPerson() {
        // In the review scene there is no Mario to stand in for, so the mode is
        // taken from the selection alone. Without this the first-person view was
        // indistinguishable from third-person VR - both fell back to the portal's
        // placement and scale.
        let now = mariovision_first_person_active()
            || (demoActive && mariovision_render_mode() == Int32(RenderMode.firstPerson.rawValue))
        guard now != isFirstPerson else { return }
        isFirstPerson = now
        placement = nil
        firstPersonAnchor = nil
        firstPersonHud = nil
    }

    /// Uniform scale of the window, carried in the placement matrix. Portal
    /// space keeps its own units - the glass stays 1.0 m x 0.75 m there and the
    /// world its 1/400 - so scaling the placement enlarges the pane, the world
    /// behind it and the HUD together, and the C side needs to know nothing.
    private var portalScale: Float = 1

    /// The demo scene. Drawn instead of the game when the demo file is in
    /// place, through the same matrices - so what is judged here is what the app
    /// does, not a mock-up of it.
    private var demoScene = DemoScene()
    private let demoActive = RomInstaller.demoActive

    /// Live from the settings sliders, read fresh each frame.
    private var stereoScale: Float = 1
    private var worldScale: Float = PortalRenderer.defaultWorldScale
    private var convergence: Float = 0
    private var fovScale: Float = 1

    /// Where a probe point lands on each eye's screen. The difference between
    /// the eyes is the disparity the viewer has to fuse, so this is the one
    /// number that says whether the stereo pair is right.
    private struct EyeProbe {
        var ndcGlass: Float = 0   // centre of the window, 1.2 m away
        var ndcDeep: Float = 0    // 2.5 m behind the window
        var p00: Float = 0        // projection x scale
        var p20: Float = 0        // projection off-axis term (differs per eye)
    }
    private var probes = [EyeProbe](repeating: EyeProbe(), count: 2)
    private var probeFrames = 0
    private var dumpFrames = 0

    /// Set when a frame comes back without a drawable: the layer is being torn
    /// down and there is no sound way to continue from there.
    private var stopped = false

    /// MARIOVISION_DUMP=<frame> writes one eye's finished texture to a PNG in
    /// the app container at that frame, so it can be pulled off the device and
    /// looked at directly.
    private static let dumpAtFrame: Int = {
        guard let raw = ProcessInfo.processInfo.environment["MARIOVISION_DUMP"],
              let n = Int(raw) else { return 0 }
        return n
    }()

    private let layerRenderer: LayerRenderer
    private let device: MTLDevice
    private let commandQueue: MTLCommandQueue
    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    /// Tracked controllers, when there are any. Shares the session above so
    /// both providers see the same world origin - which is the whole reason
    /// the hands can go through the eye's own transform.
    private let spatialHands = SpatialControllers()
    private var depthTextures: [MTLTexture] = []

    /// Lowest depth value distant geometry is allowed to reach. Off: spreading
    /// the range away from the clear value changed nothing on device, so the
    /// idea that distant geometry reads as "empty" to the compositor is
    /// refuted. MARIOVISION_DEPTHFLOOR=0.4 puts it back.
    private static let depthFloor: Float = {
        guard let raw = ProcessInfo.processInfo.environment["MARIOVISION_DEPTHFLOOR"],
              let v = Float(raw) else { return 0 }
        return min(max(v, 0), 0.9)
    }()

    /// Depth left outside the window, where the clip test discards everything
    /// and nothing writes depth of its own. MARIOVISION_CLEARDEPTH overrides
    /// it; 0 restores the far plane and the cliff at the window's edge.
    private static let clearDepth: Double = {
        guard let raw = ProcessInfo.processInfo.environment["MARIOVISION_CLEARDEPTH"],
              let v = Double(raw) else { return 0.0015 }
        return min(max(v, 0), 1)
    }()

    /// The clear doubles as the far plane, and it stands in metres of the room.
    /// Scaling the window pushes the whole level that many times further away
    /// there, so the threshold has to come down by the same factor - otherwise
    /// the far end of the level drops off the back of it as soon as the window
    /// is pulled larger. Reverse-Z: depth falls as 1/distance, so the factor is
    /// exactly the scale.
    private var farClearDepth: Double {
        // The threshold is tuned against the portal's 1/400. A larger world
        // scale puts the same level that many times further out in the room,
        // exactly as a larger window does, so both factors divide it.
        let worldFactor = Double(worldScale / Self.defaultWorldScale)
        return Self.clearDepth / (Double(max(portalScale, 0.05)) * max(worldFactor, 0.05))
    }

    /// MARIOVISION_OPAQUE=1 fills the eye texture opaque instead of clear. The
    /// room still showed through the gaps, which is how we know the pixels are
    /// removed after this renderer is done with them.
    private static let opaqueBackground =
        ProcessInfo.processInfo.environment["MARIOVISION_OPAQUE"] != nil

    /// Withholding depth is not a usable experiment: the compositor needs it to
    /// composite at all, and without it the portal goes black while the game
    /// keeps running. Worth knowing - it proves the depth is read and acted on.
    private static let submitDepth =
        ProcessInfo.processInfo.environment["MARIOVISION_NODEPTH"] == nil

    /// Without multisampling every polygon edge is a hard staircase, and the two
    /// eyes land on different steps - in stereo they cannot fuse it and it
    /// shimmers. The samples never leave tile memory, so this costs bandwidth,
    /// not memory. Not available in the simulator, which rejects depth32Float as
    /// a resolve target. MARIOVISION_MSAA=1 switches it off.
    static let sampleCountKey = "mariovision.msaa"
    static let renderQualityKey = "mariovision.renderQuality"

    /// Read fresh each frame so the settings slider acts without a restart. The
    /// environment variable still wins, for launches driven from a terminal.
    private var sampleCount: Int {
        if let raw = ProcessInfo.processInfo.environment["MARIOVISION_MSAA"],
           let n = Int(raw) { return max(1, n) }
        let stored = UserDefaults.standard.integer(forKey: Self.sampleCountKey)
        return stored > 0 ? stored : 4
    }
    private var msaaColor: [MTLTexture] = []
    private var msaaDepth: [MTLTexture] = []
    private var superColor: [MTLTexture] = []
    private var superDepth: [MTLTexture] = []
    private var resolveState: MTLRenderPipelineState?
    private var resolveDepthState: MTLDepthStencilState?
    private var screenColor: [MTLTexture] = []
    private var screenDepth: [MTLTexture] = []
    private var quadState: MTLRenderPipelineState?
    private var quadDepthState: MTLDepthStencilState?

    /// Scales the 3D window's eye separation down to a comfortable budget; see
    /// renderScreenEye. The slider then spans something usable instead of being
    /// unbearable above a twentieth of its travel.
    static let screenDepthFactor: Float = 0.15

    static let screenFocusKey = "mariovision.screenFocus"

    /// Distance in game units at which the two pictures of the 3D window
    /// coincide. SM64's camera trails Mario by roughly a thousand units, so
    /// that is where the eyes rest by default.
    private var screenFocus: Float {
        let stored = UserDefaults.standard.float(forKey: Self.screenFocusKey)
        return stored > 0 ? stored : 1000
    }

    static let superSamplingKey = "mariovision.supersampling"

    /// Read fresh each frame so the slider acts without a restart.
    private var superSampling: Float {
        if let raw = ProcessInfo.processInfo.environment["MARIOVISION_SUPERSAMPLE"],
           let v = Float(raw) { return min(max(v, 1), 3) }
        let stored = UserDefaults.standard.float(forKey: Self.superSamplingKey)
        return stored > 0 ? min(max(stored, 1), 3) : 2
    }

    init?(layerRenderer: LayerRenderer) {
        guard let device = MTLCreateSystemDefaultDevice(),
              let queue = device.makeCommandQueue() else { return nil }
        self.layerRenderer = layerRenderer
        self.device = device
        self.commandQueue = queue
    }

    /// Blocks until the layer is invalidated; run it on its own thread.
    func run() {
        // Controllers first, then one run with everything in it. Adding a
        // provider later would mean running the session again, and world
        // tracking restarting under a first-person anchor that was captured
        // against the old origin is not worth risking for a convenience.
        Task {
            spatialHands.worldTrackingPassthrough = worldTracking
            await spatialHands.prepare(session: arSession)
            try? await arSession.run([worldTracking] + spatialHands.providers)
        }

        // Once, not per frame. The game thread reads this to derive its aspect
        // ratio, and lays the HUD out against it with
        // GFX_DIMENSIONS_FROM_LEFT_EDGE while gfx_draw_rectangle compensates
        // with the ratio derived from the same value. Writing it every frame
        // from this thread let a frame be laid out with one aspect and
        // compensated with another, which is what left the coin and star
        // counters overhanging the pane. The portal's glass is 4:3 and does not
        // change, so it is set here and left alone.
        mariovision_set_drawable_size(1440, 1080)
        mariovision_portal_set_window(Self.windowHalfWidth, Self.windowHalfHeight)
        gfx_metal_set_reverse_z(true)
        mariovision_portal_set_active(true)

        installDragHandler()

        renderLoop()

        mariovision_portal_set_active(false)
        gfx_metal_set_reverse_z(false)
        arSession.stop()
    }

    private func renderLoop() {
        while !stopped {
            switch layerRenderer.state {
            case .invalidated:
                return
            case .paused:
                layerRenderer.waitUntilRunning()
            default:
                autoreleasepool { renderFrame() }
            }
        }
    }

    private func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { return }

        frame.startUpdate()
        frame.endUpdate()

        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)

        frame.startSubmission()

        // No defer around this. Switching modes tears the layer down, and from
        // then on queryDrawable returns nothing - but the deferred
        // endSubmission still ran, closing a submission that never received a
        // drawable. cp_frame_end_submission aborts on that, which is exactly
        // where the trace from the device pointed. So it is only ended on the
        // paths that actually presented something.
        guard let drawable = frame.queryDrawable() else {
            // Two very different situations look the same here.
            //
            // While the layer is still running this is a transient miss - the
            // space is coming up, or a frame was simply not ready. Treating it
            // as terminal stopped the loop on the first one and the portal never
            // appeared at all. The submission is closed and the next frame tried.
            //
            // Once the layer is no longer running the layer is being torn down:
            // closing the submission then aborts inside cp_frame_end_submission,
            // and leaving it open traps on the next start. So neither - the loop
            // ends and run() tears down in order.
            if layerRenderer.state == .running {
                frame.endSubmission()
            } else {
                stopped = true
            }
            return
        }

        let presentation = drawable.frameTiming.presentationTime
        let anchor = worldTracking.queryDeviceAnchor(
            atTimestamp: LayerRenderer.Clock.Instant.epoch.duration(to: presentation).timeInterval)
        drawable.deviceAnchor = anchor

        guard let commandBuffer = commandQueue.makeCommandBuffer() else { return }

        let deviceTransform = anchor?.originFromAnchorTransform ?? matrix_identity_float4x4

        var liveStereo: Float = 1, liveDistance: Float = 1.2, liveWorld = Self.defaultWorldScale
        var liveConvergence: Float = 0, liveFov: Float = 1
        mariovision_portal_get_tuning(&liveStereo, &liveDistance, &liveWorld,
                                      &liveConvergence, &liveFov)
        stereoScale = liveStereo
        updateFirstPerson()
        worldScale = isFirstPerson ? Self.firstPersonWorldScale : liveWorld
        convergence = liveConvergence
        fovScale = liveFov

        // First person: the game hands over a camera already sitting in Mario's
        // head and facing the snap heading, so the placement has only to pin
        // that origin somewhere in the room. Everything else - all the looking
        // around - is the head pose in the view matrix below, at display rate.
        if isFirstPerson {
            if firstPersonAnchor == nil, anchor != nil {
                firstPersonAnchor = Self.yawAligned(deviceTransform)
                let entry = Self.yawPitch(of: deviceTransform)
                firstPersonAnchorYaw = entry.yaw
                firstPersonAnchorPitch = entry.pitch
                // Start measuring from here. Without this the first frame after
                // coming back sees the whole gap since the mode was last left
                // as one enormous step.
                prevHeadRoom = nil
            }
            if let anchored = firstPersonAnchor {
                // Turned about the line of sight when the game says so, which
                // today means a handstand at the top of a pole. Applied to what
                // is drawn and not to the anchor itself: the anchor is also
                // what the hands and the room-scale walking are measured
                // against, and those must go on meaning what they meant.
                let roll = mariovision_first_person_roll()
                placement = roll == 0 ? anchored : anchored * Self.rollZ(roll)
                placedDistance = liveDistance
                portalScale = 1
            }
            // Relative to the heading the mode was entered facing: that is the
            // one the anchor was aligned to, and so the one the game's snap
            // heading corresponds to.
            // Where the eyes are, in the game camera's own space and units, so
            // billboards can aim at them individually. Only walking and leaning
            // change this - not looking around - so 30 Hz is plenty for it.
            if let anchored = firstPersonAnchor {
                let headRoom = SIMD4<Float>(deviceTransform.columns.3.x,
                                            deviceTransform.columns.3.y,
                                            deviceTransform.columns.3.z, 1)
                let inPortal = anchored.inverse * headRoom
                let toGameUnits = 1 / max(Self.firstPersonWorldScale, 1e-6)
                mariovision_set_eye_in_camera_space(inPortal.x * toGameUnits,
                                                    inPortal.y * toGameUnits,
                                                    inPortal.z * toGameUnits)
            }

            // The controllers, through the very same frame the eye just went
            // through. Sampled at the instant the head was, so a hand cannot
            // lag the head it is drawn beside.
            if let anchored = firstPersonAnchor {
                spatialHands.publish(
                    anchored: anchored,
                    timestamp: LayerRenderer.Clock.Instant.epoch
                        .duration(to: presentation).timeInterval)
            }

            // How far the head moved in the room, in the anchor's directions.
            if let anchored = firstPersonAnchor {
                let head = SIMD3<Float>(deviceTransform.columns.3.x,
                                        deviceTransform.columns.3.y,
                                        deviceTransform.columns.3.z)
                if let previous = prevHeadRoom {
                    let moved = head - previous
                    let right = SIMD3<Float>(anchored.columns.0.x, anchored.columns.0.y,
                                             anchored.columns.0.z)
                    let back = SIMD3<Float>(anchored.columns.2.x, anchored.columns.2.y,
                                            anchored.columns.2.z)
                    let toUnits = 1 / max(Self.firstPersonWorldScale, 1e-6)
                    mariovision_set_walk_delta(simd_dot(moved, right) * toUnits,
                                               simd_dot(moved, back) * toUnits)
                }
                prevHeadRoom = head
            }

            // Whatever of the walking the game actually granted moves the play
            // space with it, so the head measured against that space is back
            // where it started. Without this the step is applied twice - once
            // to Mario and once to the view - and one leaves one's own body at
            // twice the speed.
            //
            // What a wall refused is not sent back, so leaning past him is
            // still possible; only real movement re-anchors.
            var walked = (Float(0), Float(0))
            if withUnsafeMutablePointer(to: &walked, {
                $0.withMemoryRebound(to: Float.self, capacity: 2) {
                    mariovision_take_walk($0)
                }
            }), var anchored = firstPersonAnchor {
                let metres = SIMD3<Float>(walked.0, 0, walked.1) * Self.firstPersonWorldScale
                anchored.columns.3 += anchored.columns.0 * metres.x
                                    + anchored.columns.2 * metres.z
                firstPersonAnchor = anchored
                placement = anchored
            }

            firstPersonHud = Self.placementInFront(of: deviceTransform, distance: liveDistance)

            // The world tilts; the pane the readouts sit on does not, or they
            // would swim with it.
            if let anchored = firstPersonAnchor,
               let tilt = jumpTilt(now: CACurrentMediaTime()) {
                placement = anchored * tilt
            }

            // Both angles relative to the pose the mode was entered with.
            //
            // The yaw always was; the pitch was not, and that was the whole of
            // the swimming trouble. A constant offset in the device pose cancels
            // in a difference but survives in an absolute reading - measured, it
            // claimed a steady 88 to 116 degrees upwards, which is why Mario
            // rose and rose and could not be pointed anywhere else.
            let head = Self.yawPitch(of: deviceTransform)
            var turned = head.yaw - firstPersonAnchorYaw
            while turned > .pi { turned -= 2 * .pi }
            while turned < -.pi { turned += 2 * .pi }
            mariovision_set_head_pose(turned, head.pitch - firstPersonAnchorPitch)
        } else {
            firstPersonAnchor = nil
            firstPersonHud = nil
            prevHeadRoom = nil
        }

        // Distance slider: slide the window along its own forward axis. Its +z
        // faces the user, so away is -z. That is the same path a rebuild from
        // the original head pose would take, but unlike a rebuild it keeps
        // wherever the window has since been dragged to.
        if var current = placement, abs(liveDistance - placedDistance) > 0.001 {
            // Normalised: once the window has been resized its basis carries a
            // scale, and an unnormalised axis would move it that many times too far.
            var away = -SIMD3<Float>(current.columns.2.x, current.columns.2.y, current.columns.2.z)
            let axisLength = simd_length(away)
            if axisLength > 1e-6 { away /= axisLength }
            current.columns.3 += SIMD4<Float>(away * (liveDistance - placedDistance), 0)
            placement = current
            placedDistance = liveDistance
        }

        if placement == nil {
            if anchor != nil {
                placedDistance = liveDistance
                placement = Self.placementInFront(of: deviceTransform, distance: liveDistance)
            } else {
                // No pose yet. On device this happens for the first seconds
                // while world tracking spins up - waiting is mandatory, because
                // the identity pose is the space origin: on the FLOOR somewhere
                // in the room, which is exactly where an invisible portal ends
                // up. Only the simulator, which never delivers an anchor at
                // all, falls back to the identity pose.
                #if targetEnvironment(simulator)
                framesWithoutAnchor += 1
                if framesWithoutAnchor > 90 {
                    placedDistance = liveDistance
                    placement = Self.placementInFront(of: matrix_identity_float4x4,
                                                      distance: liveDistance)
                }
                #endif
            }
            if placement == nil {
                // Present cleared frames while waiting so the compositor gets
                // its drawable back - passthrough stays clean, no stale garbage.
                presentCleared(drawable: drawable, commandBuffer: commandBuffer)
                frame.endSubmission()
                return
            }
        }

        applyDrag(head: SIMD3<Float>(deviceTransform.columns.3.x,
                                    deviceTransform.columns.3.y,
                                    deviceTransform.columns.3.z))

        let scale = simd_float4x4(scale: worldScale)

        // One list for both eyes, and the game held off the pool until they are
        // both done. Fetching per eye let a publish land between them, so the
        // two eyes could show consecutive game frames - which is felt as unease
        // while moving rather than seen as a glitch.
        mariovision_portal_begin_frame()
        defer { mariovision_portal_end_frame() }

        let screenMode = mariovision_render_mode() == Int32(RenderMode.window3D.rawValue)

        for viewIndex in 0 ..< drawable.views.count {
            if screenMode {
                renderScreenEye(viewIndex: viewIndex,
                                drawable: drawable,
                                deviceTransform: deviceTransform,
                                commandBuffer: commandBuffer)
            } else {
                renderEye(viewIndex: viewIndex,
                          drawable: drawable,
                          deviceTransform: deviceTransform,
                          scale: scale,
                          commandBuffer: commandBuffer)
            }
        }

        dumpFrames += 1
        if Self.dumpAtFrame > 0 && dumpFrames == Self.dumpAtFrame {
            // Both eyes, so the stereo can be measured off the pair instead of
            // worn. Without a headset that is the only way to tell whether a
            // mode produces depth at all, and how much.
            for i in 0 ..< min(2, drawable.colorTextures.count) {
                dump(color: drawable.colorTextures[i], eye: i, commandBuffer: commandBuffer)
            }
        }

        drawable.encodePresent(commandBuffer: commandBuffer)
        commandBuffer.commit()
        frame.endSubmission()
    }

    /// Copies one eye's finished texture out to a PNG in the app container.
    ///
    /// This is the picture the compositor is handed, before it does anything
    /// with it - which settles the question the whole search has come down to:
    /// is the missing geometry absent here, or does it survive to this point and
    /// get removed afterwards? Blitting into a shared texture is the only way to
    /// read a private render target back.
    private func dump(color: MTLTexture, eye: Int, commandBuffer: MTLCommandBuffer) {
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: color.pixelFormat,
                                                            width: color.width,
                                                            height: color.height,
                                                            mipmapped: false)
        desc.storageMode = .shared
        desc.usage = [.shaderRead, .shaderWrite]
        guard let staging = device.makeTexture(descriptor: desc),
              let blit = commandBuffer.makeBlitCommandEncoder() else { return }

        blit.copy(from: color, sourceSlice: 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: color.width, height: color.height, depth: 1),
                  to: staging, destinationSlice: 0, destinationLevel: 0,
                  destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        blit.endEncoding()

        commandBuffer.addCompletedHandler { _ in
            let w = staging.width, h = staging.height
            var pixels = [UInt8](repeating: 0, count: w * h * 4)
            pixels.withUnsafeMutableBytes { raw in
                staging.getBytes(raw.baseAddress!, bytesPerRow: w * 4,
                                 from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
            }

            // Straight BGRA, alpha kept: a hole with alpha 0 and one with the
            // sky's colour but no alpha look identical on screen and mean very
            // different things.
            guard let provider = CGDataProvider(data: Data(pixels) as CFData),
                  let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32,
                                   bytesPerRow: w * 4,
                                   space: CGColorSpaceCreateDeviceRGB(),
                                   bitmapInfo: CGBitmapInfo(rawValue:
                                        CGImageAlphaInfo.premultipliedFirst.rawValue |
                                        CGBitmapInfo.byteOrder32Little.rawValue),
                                   provider: provider, decode: nil,
                                   shouldInterpolate: false, intent: .defaultIntent)
            else { return }

            let url = URL(fileURLWithPath: NSSearchPathForDirectoriesInDomains(
                .documentDirectory, .userDomainMask, true)[0])
                .appendingPathComponent("portal-eye\(eye).png")
            guard let dest = CGImageDestinationCreateWithURL(
                url as CFURL, "public.png" as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(dest, cg, nil)
            CGImageDestinationFinalize(dest)
            NSLog("[portal] dumped eye \(eye) to \(url.path)")
        }
    }

    private func presentCleared(drawable: LayerRenderer.Drawable,
                                commandBuffer: MTLCommandBuffer) {
        for viewIndex in 0 ..< drawable.views.count {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = drawable.colorTextures[viewIndex]
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            // Whatever the game says lies beyond the picture - a fade, or the
            // inside of a cannon barrel. Clear to nothing when it says nothing,
            // which is almost always, and the room shows through as before.
            pass.colorAttachments[0].clearColor = Self.backdropClear()
            if viewIndex < drawable.depthTextures.count {
                pass.depthAttachment.texture = drawable.depthTextures[viewIndex]
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.storeAction = .store
                pass.depthAttachment.clearDepth = farClearDepth
            }
            commandBuffer.makeRenderCommandEncoder(descriptor: pass)?.endEncoding()
        }
        drawable.encodePresent(commandBuffer: commandBuffer)
        commandBuffer.commit()
    }

    /// One eye of the 3D-window mode: a screen with depth rather than an
    /// opening.
    ///
    /// Two passes. The first renders the game exactly as the flat window would
    /// - its own projection, its own framing, its own sky and HUD - into a
    /// texture of ours, with the camera slid sideways by half the eye
    /// separation. The second draws that texture onto the pane from this eye.
    ///
    /// It has to be two. Laying a matrix over the game's finished projection
    /// cannot work, because the perspective divide belongs between the two and
    /// a 4x4 has nowhere to put it; squeezing depth in camera space instead
    /// pulls everything onto the pane at full size. Rendering to a texture puts
    /// the divide where it belongs.
    private func renderScreenEye(viewIndex: Int,
                                 drawable: LayerRenderer.Drawable,
                                 deviceTransform: simd_float4x4,
                                 commandBuffer: MTLCommandBuffer) {
        let view = drawable.views[viewIndex]
        guard let screen = ensureScreenTargets(index: viewIndex) else { return }

        // Half the eye separation, in game units - but a seventh of what the
        // straight conversion gives.
        //
        // Taken at face value, 3 cm of eye offset at 1/400 scale is 13 units,
        // and at the depth slider's default 19. Worked through, that is 3.5 cm
        // of separation on the pane for the horizon, which is fine, and 14 cm
        // for something two hundred units away - a wall, a nearby tree. Two
        // centimetres is about where comfort ends. The near field, not the
        // horizon, sets the budget here, and SM64's camera spends much of its
        // time close to something.
        var eyeLocal = view.transform
        let halfIPD = eyeLocal.columns.3.x * stereoScale * Self.screenDepthFactor
        mariovision_portal_set_screen_eye(halfIPD / max(worldScale * portalScale, 1e-6), screenFocus)

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = screen.color
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].storeAction = .store
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.depthAttachment.texture = screen.depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.storeAction = .dontCare
        // The game's own projection again, so the classic 0..1 depth and the
        // classic compare - not the compositor's reverse-Z.
        pass.depthAttachment.clearDepth = 1.0

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { return }
        gfx_metal_set_reverse_z(false)
        gfx_metal_begin_frame(encoder, screen.color.pixelFormat, screen.depth.pixelFormat,
                              UInt32(screen.color.width), UInt32(screen.color.height), 1)
        gfx_metal_set_target_rect(0, 0, UInt32(screen.color.width), UInt32(screen.color.height))
        mariovision_set_drawable_size(UInt32(screen.color.width), UInt32(screen.color.height))
        _ = mariovision_portal_render_screen_eye()
        gfx_metal_end_frame()
        gfx_metal_set_reverse_z(true)
        encoder.endEncoding()

        // Second pass: the pane itself, from this eye.
        if stereoScale != 1.0 {
            eyeLocal.columns.3 = SIMD4<Float>(eyeLocal.columns.3.x * stereoScale,
                                              eyeLocal.columns.3.y * stereoScale,
                                              eyeLocal.columns.3.z * stereoScale, 1)
        }
        let eyeTransform = deviceTransform * eyeLocal
        let mvp = drawable.computeProjection(viewIndex: viewIndex)
            * eyeTransform.inverse * placement!
            * simd_float4x4(diagonal: SIMD4<Float>(Self.windowHalfWidth,
                                                   Self.windowHalfHeight, 1, 1))

        drawScreenQuad(texture: screen.color, mvp: mvp,
                       drawable: drawable, viewIndex: viewIndex,
                       commandBuffer: commandBuffer)
    }

    private func renderEye(viewIndex: Int,
                           drawable: LayerRenderer.Drawable,
                           deviceTransform: simd_float4x4,
                           scale: simd_float4x4,
                           commandBuffer: MTLCommandBuffer) {
        let view = drawable.views[viewIndex]

        if !loggedEyes, drawable.views.count >= 2 {
            loggedEyes = true
            let a = drawable.views[0].transform.columns.3
            let b = drawable.views[1].transform.columns.3
            // Never read view.tangents here: in a mixed-reality layer that is
            // not merely deprecated, it aborts the process outright ("BUG IN
            // CLIENT: please use cp_drawable_compute_projection"). Which also
            // confirms computeProjection - what the renderer already uses - is
            // the sanctioned way to get the per-eye projection.
            let va = drawable.views[0].textureMap.viewport
            let vb = drawable.views[1].textureMap.viewport
            let tex = drawable.colorTextures[0]
            // The compositor's own near/far. Distant geometry vanishing in whole
            // quads while everything inside the level stays looks exactly like a
            // far plane, so this is the number that decides it.
            NSLog("[portal] depthRange=\(drawable.depthRange) quality=\(layerRenderer.configuration.maxRenderQuality)")
            // Foveation is switched off in the configuration, but nothing has
            // ever checked that it took. If the compositor hands out
            // rasterization rate maps anyway, it samples the texture at a
            // varying rate while this renderer draws at a uniform one - which
            // would look like blocky, low-resolution damage that is worst at
            // the edges of the view and shifts when the head turns.
            NSLog("[portal] foveation=\(layerRenderer.configuration.isFoveationEnabled) rateMaps=\(drawable.rasterizationRateMaps.count)")
            NSLog("[portal] eyeL=(%.4f %.4f %.4f) eyeR=(%.4f %.4f %.4f) dx=%.4f",
                  a.x, a.y, a.z, b.x, b.y, b.z, b.x - a.x)
            NSLog("[portal] tex=%dx%d vpL=(%.0f %.0f %.0fx%.0f) vpR=(%.0f %.0f %.0fx%.0f) views=%d texs=%d",
                  tex.width, tex.height,
                  va.originX, va.originY, va.width, va.height,
                  vb.originX, vb.originY, vb.width, vb.height,
                  drawable.views.count, drawable.colorTextures.count)
        }

        // Eye pose in the immersive space, and the matrices from the plan:
        //   Q  = P_e · V_e · M_place · S      (replaces the game projection)
        //   glass = P_e · V_e · M_place · D   (lays ortho NDC onto the window)
        //   U  = (P_e · V_e · M_place)^-1     (clip -> portal space, for clipping)
        var eyeLocal = view.transform
        if stereoScale != 1.0 {
            eyeLocal.columns.3 = SIMD4<Float>(eyeLocal.columns.3.x * stereoScale,
                                              eyeLocal.columns.3.y * stereoScale,
                                              eyeLocal.columns.3.z * stereoScale,
                                              1)
        }
        let eyeTransform = deviceTransform * eyeLocal
        let viewMatrix = eyeTransform.inverse
        var projection = drawable.computeProjection(viewIndex: viewIndex)

        // Opening angle. Scaling clip x and y together zooms the image without
        // touching the stereo geometry.
        if fovScale != 1.0 {
            for c in 0 ..< 4 {
                projection[c].x *= fovScale
                projection[c].y *= fovScale
            }
        }

        // Convergence: shift this eye's image sideways by a constant, opposite
        // per eye, by adding a multiple of clip w to clip x. Positive values
        // pull the two images together.
        if convergence != 0 {
            let shift = eyeLocal.columns.3.x < 0 ? convergence : -convergence
            for c in 0 ..< 4 {
                projection[c].x += shift * projection[c].w
            }
        }

        // Reverse-Z with an infinite far plane puts depth at near/distance, so
        // everything beyond a few metres crowds into the first thousandths -
        // right where the cleared "nothing here" value sits. The compositor
        // reads that buffer to decide what to composite, and distant geometry
        // reads to it as empty: sky at 37 m lands on 0.0027 against a clear of
        // 0. Proven by clearing the whole eye texture opaque and still seeing
        // the room through the gaps.
        //
        // So the range gets spread out: NDC z' = a*z + b maps [0, 1] onto
        // [b, a+b], and nothing sits near the clear value any more. Ordering is
        // untouched, so depth testing between game elements is unaffected.
        if Self.depthFloor > 0 {
            let a = 1 - Self.depthFloor
            for c in 0 ..< 4 {
                projection[c].z = a * projection[c].z + Self.depthFloor * projection[c].w
            }
        }

        let pvm = projection * viewMatrix * placement!

        if viewIndex < probes.count {
            let glass = pvm * SIMD4<Float>(0, 0, 0, 1)
            let deep = pvm * SIMD4<Float>(0, 0, -2.5, 1)
            probes[viewIndex] = EyeProbe(ndcGlass: glass.x / glass.w,
                                         ndcDeep: deep.x / deep.w,
                                         p00: projection.columns.0.x,
                                         p20: projection.columns.2.x)
        }
        if viewIndex == drawable.views.count - 1 {
            probeFrames += 1
            if probeFrames % 180 == 0 {
                // A point on the glass is 1.2 m away, so its disparity should be
                // a couple of per cent of screen width; the deep point roughly a
                // third of that. Much more than that and the eyes cannot fuse.
                NSLog("[portal] disp glass=%.4f (L%.4f R%.4f) deep=%.4f (L%.4f R%.4f) p00=%.3f/%.3f p20=%.4f/%.4f",
                      probes[0].ndcGlass - probes[1].ndcGlass, probes[0].ndcGlass, probes[1].ndcGlass,
                      probes[0].ndcDeep - probes[1].ndcDeep, probes[0].ndcDeep, probes[1].ndcDeep,
                      probes[0].p00, probes[1].p00, probes[0].p20, probes[1].p20)
            }
        }
        // 3D-window mode currently renders as the portal does. Two attempts at
        // making it a screen with depth both failed, and for the same reason:
        //
        //  - Appending a matrix to the game's own projection cannot work. The
        //    perspective divide has to happen between the two, and a 4x4 has no
        //    way to express that. The result was the distorted mess on device.
        //  - Squeezing depth in camera space avoids that, but compressing z
        //    while leaving x and y pulls everything onto the pane at its full
        //    angular size - Mario ends up enormous. Scaling x and y to match is
        //    just a smaller world, which is the portal again.
        //
        // A screen showing the game's own framing with real stereo needs the
        // scene rendered to a texture per eye and that texture drawn on the
        // pane. That is a separate pass, not a change of matrix.
        var q = pvm * scale
        // Maps ortho NDC onto the glass. The 1 mm push has to come AFTER the
        // z squash, not before it: applied first it gets multiplied by the
        // 0.0001 as well and shrinks to a tenth of a micrometre. Ortho content
        // then straddles the glass plane, and the shader's clip test discards
        // everything at or in front of it.
        //
        // VR mode has no pane for the HUD to sit on, and one sized to the
        // portal's leaves every readout clumped ahead of the viewer. The layout
        // stays exactly as the game arranges it - the pane it is mapped onto
        // just gets bigger, which pushes the corners apart in both directions
        // at once. Size and height are live from the settings, because how far
        // apart is far enough is a matter of taste and not of measurement.
        var glass: simd_float4x4
        if let hud = firstPersonHud {
            // Rebuilt from the head, so it needs the eye's own matrices rather
            // than the pvm the world is drawn with.
            glass = projection * viewMatrix * hud
                * simd_float4x4(translation: SIMD3<Float>(0, -hudDrop, -0.001))
                * simd_float4x4(diagonal: SIMD4<Float>(Self.windowHalfWidth * hudScale,
                                                       Self.windowHalfHeight * hudScale,
                                                       0.0001, 1))
        } else if mariovision_render_mode() == Int32(RenderMode.vr.rawValue) {
            glass = pvm
                * simd_float4x4(translation: SIMD3<Float>(0, -hudDrop, -0.001))
                * simd_float4x4(diagonal: SIMD4<Float>(Self.windowHalfWidth * hudScale,
                                                       Self.windowHalfHeight * hudScale,
                                                       0.0001, 1))
        } else {
            glass = pvm
                * simd_float4x4(translation: SIMD3<Float>(0, 0, -0.001))
                * simd_float4x4(diagonal: SIMD4<Float>(Self.windowHalfWidth,
                                                       Self.windowHalfHeight,
                                                       0.0001, 1))
        }

        var clipToPortal = pvm.inverse

        let eyeWorld = eyeTransform.columns.3
        var eyePortal = placement!.inverse * eyeWorld

        withUnsafePointer(to: &q) { qp in
            withUnsafePointer(to: &glass) { gp in
                withUnsafePointer(to: &clipToPortal) { up in
                    qp.withMemoryRebound(to: Float.self, capacity: 16) { qf in
                        gp.withMemoryRebound(to: Float.self, capacity: 16) { gf in
                            up.withMemoryRebound(to: Float.self, capacity: 16) { uf in
                                var eye: [Float] = [eyePortal.x, eyePortal.y, eyePortal.z]
                                mariovision_portal_set_eye(qf, gf, uf, &eye)
                            }
                        }
                    }
                }
            }
        }

        let outColor = drawable.colorTextures[viewIndex]
        let outDepth = Self.submitDepth && viewIndex < drawable.depthTextures.count
            ? drawable.depthTextures[viewIndex]
            : ensureDepthTexture(index: viewIndex, like: outColor)

        // Supersampling: draw into a bigger pair and shrink it on the way out.
        // The game covers only a small patch of the eye texture - measured,
        // 712x640 of 2048x1984 - and that patch is its real resolution.
        // Multisampling smooths polygon edges but cannot touch what happens
        // inside a texture, and it cannot invent the pixels that are simply not
        // there. Rendering at a multiple and averaging down does both, and
        // takes the window's own edge with it.
        let superSampled = ensureSuperTargets(index: viewIndex, color: outColor, depth: outDepth)
        let colorTexture = superSampled?.color ?? outColor

        // The compositor reads the depth back for reprojection, which is what
        // keeps the portal stable during head motion.
        let depthTexture = superSampled?.depth ?? outDepth

        let multisampled = ensureMultisampleTargets(index: viewIndex,
                                                    color: colorTexture,
                                                    depth: depthTexture)

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].loadAction = .clear
        // Alpha 0: passthrough fills everything outside the portal.
        //
        // MARIOVISION_OPAQUE=1 clears to opaque dark red instead, which decides
        // where the missing geometry is lost. The edges stay aligned to the
        // viewer's field of view when the head tilts, so whatever cuts them
        // lives in the eye texture, not in the world - and with every pixel of
        // that texture opaque, the room can only still show through if the
        // compositor is dropping our pixels rather than the renderer never
        // drawing them.
        //
        // Unless the game has named a colour for what lies beyond the picture -
        // a fade, or the black inside a cannon barrel. This is the clear that
        // happens every frame before the game draws; the one in presentCleared
        // is only the fallback for a frame with nothing in it, and putting the
        // backdrop there alone meant it never appeared at all.
        pass.colorAttachments[0].clearColor = Self.opaqueBackground
            ? MTLClearColor(red: 0.25, green: 0, blue: 0, alpha: 1)
            : Self.backdropClear()
        pass.depthAttachment.loadAction = .clear
        // Not the far plane. Outside the window the clip test discards every
        // fragment, so nothing writes depth there and it keeps whatever this
        // clear left - and the compositor reprojects our image using exactly
        // that buffer. Clearing to the far plane means the window's edge is a
        // step from "three to forty metres" straight to "infinitely far", and
        // that step runs precisely along the boundary where the blocky damage
        // appears.
        //
        // 0.0015 is about 67 m in this reverse-Z range: still behind everything
        // in the level, so all geometry passes the depth test as before, but
        // continuous with it instead of a cliff. Divided by the window's scale,
        // because a larger window puts the level that much further out.
        pass.depthAttachment.clearDepth = farClearDepth

        if let multisampled {
            pass.colorAttachments[0].texture = multisampled.color
            pass.colorAttachments[0].resolveTexture = colorTexture
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.depthAttachment.texture = multisampled.depth
            pass.depthAttachment.resolveTexture = depthTexture
            pass.depthAttachment.storeAction = .multisampleResolve
            // Reverse-Z: the nearest sample is the largest one, and that is the
            // depth the compositor's reprojection needs.
            pass.depthAttachment.depthResolveFilter = .max
        } else {
            pass.colorAttachments[0].texture = colorTexture
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depthTexture
            pass.depthAttachment.storeAction = .store
        }

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { return }

        // The compositor maps a specific sub-rectangle of the texture onto the
        // display for each eye. Rendering across the whole texture instead
        // stretches and offsets the image per eye - which reads as a stereo pair
        // far too wide to fuse, and is barely affected by the eye offset.
        // In supersampled units: the game must fill the bigger target.
        let scale = Double(colorTexture.width) / Double(outColor.width)
        let viewport = view.textureMap.viewport
        let width = UInt32(viewport.width * scale)
        let height = UInt32(viewport.height * scale)

        if demoActive {
            var vp = MTLViewport(originX: viewport.originX * scale, originY: viewport.originY * scale,
                                 width: Double(width), height: Double(height),
                                 znear: 0, zfar: 1)
            encoder.setViewport(vp)
            _ = vp
            demoScene.prepare(device: device,
                              colorFormat: colorTexture.pixelFormat,
                              depthFormat: depthTexture.pixelFormat,
                              sampleCount: multisampled == nil ? 1 : sampleCount,
                              reverseZ: true)
            var clip = DemoScene.PortalClip()
            var toPortal = [Float](repeating: 0, count: 16)
            var eye = [Float](repeating: 0, count: 3)
            var half = [Float](repeating: 0, count: 2)
            if mariovision_portal_eye_params(&toPortal, &eye, &half) {
                clip.clipToPortal = toPortal.withUnsafeBytes { $0.load(as: simd_float4x4.self) }
                let unclipped = mariovision_portal_unclipped()
                clip.eye = SIMD4<Float>(eye[0], eye[1], eye[2], unclipped ? 2 : 1)
                clip.half = SIMD2<Float>(half[0], half[1])
            }
            // Lower the scene so the viewer stands on the floor instead of in
            // it - the placement's origin sits at eye height.
            let drop = DemoScene.eyeHeightMetres / max(worldScale, 1e-6)
            let stand = simd_float4x4(translation: SIMD3<Float>(0, -drop, 0))
            demoScene.draw(encoder: encoder, mvp: q * stand, clip: clip)
        } else {
            gfx_metal_begin_frame(encoder, colorTexture.pixelFormat, depthTexture.pixelFormat,
                                  UInt32(colorTexture.width), UInt32(colorTexture.height),
                                  UInt32(multisampled == nil ? 1 : sampleCount))
            gfx_metal_set_target_rect(UInt32(viewport.originX * scale),
                                      UInt32(viewport.originY * scale), width, height)
            _ = mariovision_portal_render_eye()
            gfx_metal_end_frame()
        }
        encoder.endEncoding()

        if let superSampled {
            downsample(from: superSampled, toColor: outColor, toDepth: outDepth,
                       commandBuffer: commandBuffer)
        }
    }

    /// Where the 3D window's first pass renders. 4:3 like the pane, at a
    /// resolution the supersampling slider scales - this is the game's real
    /// picture, so it is worth giving it pixels.
    private func ensureScreenTargets(index: Int) -> (color: MTLTexture, depth: MTLTexture)? {
        guard index < 2 else { return nil }

        let base = 1440
        let w = Int(Float(base) * max(1, superSampling))
        let h = w * 3 / 4

        if index < screenColor.count, screenColor[index].width == w {
            return (screenColor[index], screenDepth[index])
        }

        func make(_ format: MTLPixelFormat, _ usage: MTLTextureUsage) -> MTLTexture? {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format,
                                                            width: w, height: h,
                                                            mipmapped: false)
            d.usage = usage
            d.storageMode = .private
            return device.makeTexture(descriptor: d)
        }
        guard let c = make(.bgra8Unorm_srgb, [.renderTarget, .shaderRead]),
              let z = make(.depth32Float, .renderTarget) else { return nil }

        while screenColor.count <= index { screenColor.append(c); screenDepth.append(z) }
        screenColor[index] = c
        screenDepth[index] = z
        return (c, z)
    }

    /// Draws the rendered picture onto the pane for one eye.
    ///
    /// The depth written is the pane's own, which is what a screen is: a flat
    /// surface at a fixed distance. The stereo comes from the two pictures
    /// differing, not from the surface moving.
    private func drawScreenQuad(texture: MTLTexture, mvp: simd_float4x4,
                                drawable: LayerRenderer.Drawable, viewIndex: Int,
                                commandBuffer: MTLCommandBuffer) {
        let outColor = drawable.colorTextures[viewIndex]
        let outDepth = viewIndex < drawable.depthTextures.count
            ? drawable.depthTextures[viewIndex]
            : ensureDepthTexture(index: viewIndex, like: outColor)

        guard let pipeline = quadPipeline(colorFormat: outColor.pixelFormat,
                                          depthFormat: outDepth.pixelFormat),
              let depthState = quadDepthState else { return }

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = outColor
        pass.colorAttachments[0].loadAction = .clear
        // This is the room around the pane, and so this is where a fade or the
        // black inside a cannon has to land.
        //
        // Two clears were changed before this one and neither could ever have
        // shown: with the game on a pane it is drawn into a texture of its own
        // and then hung in the room by this pass. Clearing that texture only
        // ever repainted the picture; what surrounds it is cleared here.
        pass.colorAttachments[0].clearColor = Self.backdropClear()
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = outDepth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = farClearDepth
        pass.depthAttachment.storeAction = .store

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { return }
        let viewport = drawable.views[viewIndex].textureMap.viewport
        encoder.setViewport(MTLViewport(originX: viewport.originX, originY: viewport.originY,
                                        width: viewport.width, height: viewport.height,
                                        znear: 0, zfar: 1))
        encoder.setRenderPipelineState(pipeline)
        encoder.setDepthStencilState(depthState)
        var m = mvp
        encoder.setVertexBytes(&m, length: MemoryLayout<simd_float4x4>.size, index: 0)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    private func quadPipeline(colorFormat: MTLPixelFormat,
                              depthFormat: MTLPixelFormat) -> MTLRenderPipelineState? {
        if let cached = quadState { return cached }

        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VOut { float4 pos [[position]]; float2 uv; };

        vertex VOut mv_quad_vs(uint vid [[vertex_id]],
                               constant float4x4 &mvp [[buffer(0)]]) {
            const float2 p[4] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1) };
            VOut o;
            o.pos = mvp * float4(p[vid], 0, 1);
            o.uv = float2(p[vid].x * 0.5 + 0.5, 0.5 - p[vid].y * 0.5);
            return o;
        }

        fragment float4 mv_quad_fs(VOut in [[stage_in]],
                                   texture2d<float> src [[texture(0)]]) {
            constexpr sampler smooth(filter::linear, address::clamp_to_edge);
            return float4(src.sample(smooth, in.uv).rgb, 1.0);
        }
        """

        guard let library = try? device.makeLibrary(source: source, options: nil),
              let vs = library.makeFunction(name: "mv_quad_vs"),
              let fs = library.makeFunction(name: "mv_quad_fs") else { return nil }

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vs
        desc.fragmentFunction = fs
        desc.colorAttachments[0].pixelFormat = colorFormat
        desc.depthAttachmentPixelFormat = depthFormat
        quadState = try? device.makeRenderPipelineState(descriptor: desc)

        let dd = MTLDepthStencilDescriptor()
        dd.depthCompareFunction = .always
        dd.isDepthWriteEnabled = true
        quadDepthState = device.makeDepthStencilState(descriptor: dd)

        return quadState
    }

    /// Colour and depth at a multiple of the compositor's resolution, or nil at
    /// 1x. Sized from the output textures so a change of factor rebuilds them.
    private func ensureSuperTargets(index: Int, color: MTLTexture, depth: MTLTexture)
        -> (color: MTLTexture, depth: MTLTexture)? {
        let factor = superSampling
        guard factor > 1.001, index < 2 else { return nil }

        let w = Int((Double(color.width) * Double(factor)).rounded())
        let h = Int((Double(color.height) * Double(factor)).rounded())

        if index < superColor.count, superColor[index].width == w,
           superColor[index].height == h,
           superColor[index].pixelFormat == color.pixelFormat {
            return (superColor[index], superDepth[index])
        }

        func make(_ format: MTLPixelFormat, _ usage: MTLTextureUsage) -> MTLTexture? {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format,
                                                            width: w, height: h,
                                                            mipmapped: false)
            d.usage = usage
            d.storageMode = .private
            return device.makeTexture(descriptor: d)
        }
        guard let c = make(color.pixelFormat, [.renderTarget, .shaderRead]),
              let z = make(depth.pixelFormat, [.renderTarget, .shaderRead]) else { return nil }

        while superColor.count <= index { superColor.append(c); superDepth.append(z) }
        superColor[index] = c
        superDepth[index] = z
        return (c, z)
    }

    /// Averages the supersampled pair down into what the compositor consumes.
    ///
    /// Colour goes through a linear sampler, which at an integer factor is
    /// exactly a box filter - and it carries the alpha with it, so the window's
    /// own edge comes out soft instead of stepped. Depth cannot be filtered
    /// that way and must not be: the compositor reprojects with it, and an
    /// averaged depth at a silhouette is a value that describes nothing. The
    /// nearest of the samples is taken instead, which under reverse-Z is the
    /// largest.
    private func downsample(from source: (color: MTLTexture, depth: MTLTexture),
                            toColor: MTLTexture, toDepth: MTLTexture,
                            commandBuffer: MTLCommandBuffer) {
        guard let pipeline = resolvePipeline(colorFormat: toColor.pixelFormat,
                                             depthFormat: toDepth.pixelFormat),
              let depthState = resolveDepthState else { return }

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = toColor
        pass.colorAttachments[0].loadAction = .dontCare
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = toDepth
        pass.depthAttachment.loadAction = .dontCare
        pass.depthAttachment.storeAction = .store

        guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { return }
        encoder.setRenderPipelineState(pipeline)
        encoder.setDepthStencilState(depthState)
        encoder.setFragmentTexture(source.color, index: 0)
        encoder.setFragmentTexture(source.depth, index: 1)
        var texel = SIMD2<Float>(1.0 / Float(source.color.width), 1.0 / Float(source.color.height))
        encoder.setFragmentBytes(&texel, length: MemoryLayout<SIMD2<Float>>.size, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
    }

    private func resolvePipeline(colorFormat: MTLPixelFormat,
                                 depthFormat: MTLPixelFormat) -> MTLRenderPipelineState? {
        if let cached = resolveState { return cached }

        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VOut { float4 pos [[position]]; float2 uv; };

        // One triangle covering the target - no vertex buffer needed.
        vertex VOut mv_resolve_vs(uint vid [[vertex_id]]) {
            const float2 p[3] = { float2(-1, -3), float2(-1, 1), float2(3, 1) };
            VOut o;
            o.pos = float4(p[vid], 0, 1);
            o.uv = float2(p[vid].x * 0.5 + 0.5, 0.5 - p[vid].y * 0.5);
            return o;
        }

        struct FOut { float4 color [[color(0)]]; float depth [[depth(any)]]; };

        fragment FOut mv_resolve_fs(VOut in [[stage_in]],
                                    texture2d<float> src [[texture(0)]],
                                    depth2d<float> srcDepth [[texture(1)]],
                                    constant float2 &texel [[buffer(0)]]) {
            constexpr sampler box(filter::linear, address::clamp_to_edge);
            constexpr sampler point(filter::nearest, address::clamp_to_edge);

            FOut o;
            o.color = src.sample(box, in.uv);

            // Reverse-Z: nearest is the largest. Averaging here would invent a
            // surface between foreground and background.
            float d = 0.0;
            for (int y = -1; y <= 1; y += 2) {
                for (int x = -1; x <= 1; x += 2) {
                    float2 uv = in.uv + float2(x, y) * texel * 0.5;
                    d = max(d, srcDepth.sample(point, uv));
                }
            }
            o.depth = d;
            return o;
        }
        """

        guard let library = try? device.makeLibrary(source: source, options: nil),
              let vs = library.makeFunction(name: "mv_resolve_vs"),
              let fs = library.makeFunction(name: "mv_resolve_fs") else { return nil }

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vs
        desc.fragmentFunction = fs
        desc.colorAttachments[0].pixelFormat = colorFormat
        desc.depthAttachmentPixelFormat = depthFormat
        resolveState = try? device.makeRenderPipelineState(descriptor: desc)

        let depthDesc = MTLDepthStencilDescriptor()
        depthDesc.depthCompareFunction = .always
        depthDesc.isDepthWriteEnabled = true
        resolveDepthState = device.makeDepthStencilState(descriptor: depthDesc)

        return resolveState
    }

    /// Multisample colour and depth for one eye, matching the compositor's
    /// textures in size and format. Returns nil when the device cannot resolve
    /// depth, in which case the caller renders single-sampled - jagged, but
    /// correct, and far better than a portal that swims during head motion.
    private func ensureMultisampleTargets(index: Int,
                                          color: MTLTexture,
                                          depth: MTLTexture) -> (color: MTLTexture, depth: MTLTexture)? {
        // Depth32Float is rejected outright as a resolve target in the simulator
        // ("cannot be a MSAA resolve target"), and a render pass cannot mix
        // sample counts across its attachments - so multisampled colour with
        // plain depth is not an option either. Single-sampled there.
        //
        // The device has no such validation, which is worth remembering: if it
        // is quietly dropping the same resolve, the compositor is handed a
        // depth buffer nobody ever wrote, and it needs that buffer to composite
        // at all. MARIOVISION_MSAA=1 takes multisampling out of the picture
        // without a rebuild, which is the way to test that.
        #if targetEnvironment(simulator)
        return nil
        #else
        guard sampleCount > 1,
              device.supportsTextureSampleCount(sampleCount),
              index < 2 else { return nil }
        #endif

        if msaaColor.first?.sampleCount != sampleCount { msaaColor = []; msaaDepth = [] }

        if index < msaaColor.count,
           msaaColor[index].width == color.width,
           msaaColor[index].height == color.height,
           msaaColor[index].pixelFormat == color.pixelFormat,
           msaaDepth[index].pixelFormat == depth.pixelFormat {
            return (msaaColor[index], msaaDepth[index])
        }

        func make(_ format: MTLPixelFormat) -> MTLTexture? {
            let desc = MTLTextureDescriptor()
            desc.textureType = .type2DMultisample
            desc.pixelFormat = format
            desc.width = color.width
            desc.height = color.height
            desc.sampleCount = sampleCount
            desc.usage = .renderTarget
            // The samples are resolved inside the pass and never read back, so
            // they can live and die in tile memory.
            desc.storageMode = .memoryless
            return device.makeTexture(descriptor: desc)
        }

        guard let newColor = make(color.pixelFormat),
              let newDepth = make(depth.pixelFormat) else { return nil }

        while msaaColor.count <= index { msaaColor.append(newColor) }
        while msaaDepth.count <= index { msaaDepth.append(newDepth) }
        msaaColor[index] = newColor
        msaaDepth[index] = newDepth
        return (newColor, newDepth)
    }

    /// CompositorServices can provide its own depth textures, but only in
    /// configurations we do not request yet - so the portal brings its own.
    private func ensureDepthTexture(index: Int, like color: MTLTexture) -> MTLTexture {
        if index < depthTextures.count,
           depthTextures[index].width == color.width,
           depthTextures[index].height == color.height {
            return depthTextures[index]
        }

        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float,
                                                            width: color.width,
                                                            height: color.height,
                                                            mipmapped: false)
        desc.usage = .renderTarget
        desc.storageMode = .private
        let texture = device.makeTexture(descriptor: desc)!

        while depthTextures.count <= index { depthTextures.append(texture) }
        depthTextures[index] = texture
        return texture
    }


    // MARK: - Moving the window

    /// An immersive space has no system window bar, so the pane itself is the
    /// handle: look at the window, pinch, and carry it. Events arrive off the
    /// render thread, so this only records the ray - the maths happens in
    /// applyDrag() on the thread that owns the placement.
    private func installDragHandler() {
        layerRenderer.onSpatialEvent = { [weak self] events in
            guard let self else { return }
            self.pinchLock.lock()
            for event in events where event.kind == .indirectPinch {
                guard event.phase == .active else {
                    self.pinches[event.id] = nil
                    continue
                }
                guard let ray = event.selectionRay else { continue }
                var direction = SIMD3<Float>(Float(ray.direction.x),
                                             Float(ray.direction.y),
                                             Float(ray.direction.z))
                let length = simd_length(direction)
                guard length > 1e-5 else { continue }
                direction /= length

                var state = self.pinches[event.id]
                if state == nil {
                    self.pinchOrder &+= 1
                    state = PinchState(order: self.pinchOrder)
                    // The selection ray says where you were looking at the
                    // moment of the pinch. It is frozen from then on - visionOS
                    // does not hand out gaze during a gesture - so it is only
                    // good for deciding what was grabbed, never for carrying it.
                    state!.origin = SIMD3<Float>(Float(ray.origin.x),
                                                 Float(ray.origin.y),
                                                 Float(ray.origin.z))
                    state!.direction = direction
                }
                if let pose = event.inputDevicePose {
                    state!.hand = SIMD3<Float>(Float(pose.pose3D.position.x),
                                               Float(pose.pose3D.position.y),
                                               Float(pose.pose3D.position.z))
                    state!.hasHand = true
                }
                self.pinches[event.id] = state
            }
            self.pinchLock.unlock()
        }
    }

    private struct PaneHit {
        var point: SIMD3<Float>
        var reach: Float
    }

    /// Where the pinch ray meets the glass, if it meets it at all.
    private func hitTest(origin: SIMD3<Float>, direction: SIMD3<Float>,
                         placement: simd_float4x4) -> PaneHit? {
        let normal = SIMD3<Float>(placement.columns.2.x,
                                  placement.columns.2.y,
                                  placement.columns.2.z)
        let centre = SIMD3<Float>(placement.columns.3.x,
                                  placement.columns.3.y,
                                  placement.columns.3.z)
        let denominator = simd_dot(direction, normal)
        guard abs(denominator) > 1e-4 else { return nil }          // ray runs along the glass
        let reach = simd_dot(centre - origin, normal) / denominator
        guard reach > 0.05 else { return nil }                     // behind the hand
        let point = origin + direction * reach

        let local = placement.inverse * SIMD4<Float>(point, 1)
        let margin: Float = 0.04                                   // the frame grabs too
        guard abs(local.x) <= Self.windowHalfWidth + margin,
              abs(local.y) <= Self.windowHalfHeight + margin else { return nil }
        return PaneHit(point: point, reach: reach)
    }

    /// Re-derive the grip from the current pose. Called on every change in the
    /// number of hands so nothing jumps at the transition.
    private func anchorDrag(hand: SIMD3<Float>, head: SIMD3<Float>, centre: SIMD3<Float>) {
        dragPivot = head
        let arm = hand - head
        dragHandDistance = max(simd_length(arm), 0.05)
        dragReach = max(simd_length(centre - head), 0.05)
        dragOffset = centre - (head + (arm / dragHandDistance) * dragReach)
    }

    /// Carry the window with one hand, resize it with two.
    private func applyDrag(head: SIMD3<Float>) {
        pinchLock.lock()
        let held = pinches.values.sorted { $0.order < $1.order }
        pinchLock.unlock()

        // Nothing to grab in VR - there is no window there, the world is
        // all around you.
        guard mariovision_render_mode() != Int32(RenderMode.vr.rawValue), !isFirstPerson else { return }

        let hands = held.filter(\.hasHand)
        guard var current = placement, let first = hands.first else {
            holdOrder = nil
            handCount = 0
            return
        }
        var centre = SIMD3<Float>(current.columns.3.x, current.columns.3.y, current.columns.3.z)

        // Take hold, if this pinch has not already been turned away. Checking
        // once and remembering the refusal keeps a hand that closed elsewhere
        // from snatching the window up as it sweeps across it.
        if holdOrder != first.order {
            guard rejectedOrder != first.order else { return }
            guard hitTest(origin: first.origin, direction: first.direction,
                          placement: current) != nil else {
                rejectedOrder = first.order
                return
            }
            holdOrder = first.order
            handCount = 0
        }

        let second = hands.count > 1 ? hands[1] : nil
        let reference = second.map { (first.hand + $0.hand) / 2 } ?? first.hand

        if hands.count != handCount {
            handCount = hands.count
            anchorDrag(hand: reference, head: head, centre: centre)
            if let second {
                spanAtGrab = max(simd_distance(first.hand, second.hand), 0.02)
                scaleAtGrab = portalScale
            }
        }

        // Two hands: the gap between them sets the size, their midpoint the
        // position - the same grip you would use on a physical pane.
        if let second {
            let span = max(simd_distance(first.hand, second.hand), 0.02)
            portalScale = min(max(scaleAtGrab * span / spanAtGrab, 0.35), 3)
        }

        // The pivot is the head at the moment of grabbing. Swinging the hand
        // around it moves the window along an arc at the reach it was grabbed
        // at, so a small hand movement carries a distant window a long way -
        // which is what makes one across the room draggable at all. Head
        // movement during the drag deliberately does not enter into it: the
        // window stays put in the room while you look around. Reaching out
        // pushes it away, pulling back draws it in.
        let arm = reference - dragPivot
        let armLength = simd_length(arm)
        guard armLength > 0.05 else { return }
        centre = dragPivot + (arm / armLength) * (dragReach * armLength / dragHandDistance) + dragOffset

        current = Self.rescaled(current, to: portalScale)
        current.columns.3 = SIMD4<Float>(centre, 1)
        placement = current
        // placedDistance deliberately left alone: the distance slider only acts
        // on change, so from here on it nudges the window along its axis from
        // wherever it was dragged to, which is what you want from it.
    }

    /// Put a uniform scale into the placement's basis, leaving its position and
    /// facing alone.
    private static func rescaled(_ m: simd_float4x4, to scale: Float) -> simd_float4x4 {
        var out = m
        func fit(_ column: SIMD4<Float>) -> SIMD4<Float> {
            let axis = SIMD3<Float>(column.x, column.y, column.z)
            let length = simd_length(axis)
            guard length > 1e-6 else { return column }
            return SIMD4<Float>(axis / length * scale, 0)
        }
        out.columns.0 = fit(m.columns.0)
        out.columns.1 = fit(m.columns.1)
        out.columns.2 = fit(m.columns.2)
        return out
    }

    /// The head pose with pitch and roll taken out: position kept, facing
    /// flattened onto the room's floor plane.
    /// A turn about the placement's own forward axis.
    /// The colour to wipe a view with before drawing.
    ///
    /// A surface, however large, has an edge one can turn one's head past. A
    /// wipe does not, and it is behind every direction at once - which is what
    /// a fade, or the black inside a barrel, is actually meant to be.
    private static func backdropClear() -> MTLClearColor {
        var c: (Float, Float, Float, Float) = (0, 0, 0, 0)
        var raw = [Float](repeating: 0, count: 4)
        guard mariovision_backdrop(&raw) else {
            return MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        }
        c = (raw[0], raw[1], raw[2], raw[3])
        return MTLClearColor(red: Double(c.0), green: Double(c.1),
                             blue: Double(c.2), alpha: Double(c.3))
    }

    private static func rollZ(_ radians: Float) -> simd_float4x4 {
        let c = cos(radians), s = sin(radians)
        return simd_float4x4(columns: (SIMD4<Float>( c, s, 0, 0),
                                       SIMD4<Float>(-s, c, 0, 0),
                                       SIMD4<Float>( 0, 0, 1, 0),
                                       SIMD4<Float>( 0, 0, 0, 1)))
    }

    private static func yawAligned(_ device: simd_float4x4) -> simd_float4x4 {
        var forward = -SIMD3<Float>(device.columns.2.x, 0, device.columns.2.z)
        let length = simd_length(forward)
        forward = length > 0.01 ? forward / length : SIMD3<Float>(0, 0, -1)
        let z = -forward
        let y = SIMD3<Float>(0, 1, 0)
        let x = simd_normalize(simd_cross(y, z))
        return simd_float4x4(columns: (SIMD4<Float>(x, 0),
                                       SIMD4<Float>(y, 0),
                                       SIMD4<Float>(z, 0),
                                       device.columns.3))
    }

    /// Head yaw and pitch in the room, in radians, measured from the -z the
    /// placement helpers both treat as forward.
    private static func yawPitch(of device: simd_float4x4) -> (yaw: Float, pitch: Float) {
        let forward = -SIMD3<Float>(device.columns.2.x, device.columns.2.y, device.columns.2.z)
        let yaw = atan2(-forward.x, -forward.z)
        let flat = simd_length(SIMD2<Float>(forward.x, forward.z))
        return (yaw, atan2(forward.y, flat))
    }

    /// Window pose 1.2 m ahead of the head, yaw-aligned, slightly below eye
    /// level; portal +z points back at the user.
    private static func placementInFront(of device: simd_float4x4,
                                        distance: Float) -> simd_float4x4 {
        let eye = SIMD3<Float>(device.columns.3.x, device.columns.3.y, device.columns.3.z)

        var forward = -SIMD3<Float>(device.columns.2.x, 0, device.columns.2.z)
        let length = simd_length(forward)
        forward = length > 0.01 ? forward / length : SIMD3<Float>(0, 0, -1)

        let z = -forward                       // portal +z faces the user
        let y = SIMD3<Float>(0, 1, 0)
        let x = simd_normalize(simd_cross(y, z))

        var centre = eye + forward * distance
        centre.y = eye.y - 0.05
        // Debug: vertical offset in metres, for simulator runs where the
        // launcher window sits exactly where the portal would be.
        if let raw = ProcessInfo.processInfo.environment["MARIOVISION_PORTAL_Y"],
           let offset = Float(raw) {
            centre.y += offset
        }

        return simd_float4x4(columns: (SIMD4<Float>(x, 0),
                                       SIMD4<Float>(y, 0),
                                       SIMD4<Float>(z, 0),
                                       SIMD4<Float>(centre, 1)))
    }
}

// MARK: - helpers

extension LayerRenderer.Clock.Instant.Duration {
    /// Seconds since the layer clock's epoch - the timestamp base
    /// queryDeviceAnchor expects.
    var timeInterval: TimeInterval {
        let nanoseconds = TimeInterval(components.attoseconds / 1_000_000_000)
        return TimeInterval(components.seconds) + nanoseconds / TimeInterval(NSEC_PER_SEC)
    }
}

// MARK: - simd helpers

extension simd_float4x4 {
    init(translation t: SIMD3<Float>) {
        self = matrix_identity_float4x4
        columns.3 = SIMD4<Float>(t.x, t.y, t.z, 1)
    }

    init(scale s: Float) {
        self.init(diagonal: SIMD4<Float>(s, s, s, 1))
    }
}
