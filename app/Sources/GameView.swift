import SwiftUI
import UIKit
import Metal
import QuartzCore
import GameController

/// Hosts the CAMetalLayer the game renders into.
///
/// The engine boots at app launch (mariovision_boot) and drops frames until
/// this view hands it a layer; when the window closes, the layer is taken away
/// again and the game goes back to dropping frames. There is deliberately no
/// CADisplayLink - the game thread paces itself off nextDrawable.
final class GameLayerView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        configure()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        configure()
    }

    private func configure() {
        isMultipleTouchEnabled = false

        // The missing piece for gamepads in a *windowed* visionOS app: without
        // this interaction the system keeps a connected controller for its own
        // window navigation (which is why it could move the window) and only
        // forwards the menu button. Immersive-space apps get this routing for
        // free - KeynoteEnvViewer works because of that - windowed apps have to
        // claim it per view.
        let events = GCEventInteraction()
        events.handledEventTypes = .gamepad
        addInteraction(events)
    }

    // Attach/detach around the window lifecycle. Detach hangs on several hooks
    // because closing a visionOS window tears the whole scene down - the view
    // is deallocated without ever being *moved* out of its window, so no single
    // callback is guaranteed. mariovision_clear_metal_layer is idempotent and
    // only clears if this view's layer is still the active one, so the belt and
    // braces cannot strip a newly opened window's layer.

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            mariovision_set_metal_layer(metalLayer)
        } else {
            mariovision_clear_metal_layer(metalLayer)
        }
    }

    override func willMove(toWindow newWindow: UIWindow?) {
        super.willMove(toWindow: newWindow)
        if newWindow == nil {
            mariovision_clear_metal_layer(metalLayer)
        }
    }

    func detach() {
        mariovision_clear_metal_layer(metalLayer)
    }

    deinit {
        if let layer = layer as? CAMetalLayer {
            mariovision_clear_metal_layer(layer)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()

        let scale = displayScale
        let size = CGSize(width: bounds.width * scale, height: bounds.height * scale)
        guard size.width > 0, size.height > 0 else { return }

        metalLayer.drawableSize = size
        mariovision_set_drawable_size(UInt32(size.width), UInt32(size.height))
    }

    // MARK: - Pointer input
    //
    // Gaze plus pinch arrives as touches. djui's menus are written against a
    // mouse, so they are fed in as one; see controller_mouse_visionos.c.

    private var displayScale: CGFloat {
        let scale = traitCollection.displayScale
        return scale > 0 ? scale : contentScaleFactor
    }

    private func report(_ touch: UITouch, down: Bool?) {
        let point = touch.location(in: self)
        let scale = displayScale
        mariovision_pointer_moved(Int32(point.x * scale), Int32(point.y * scale))
        if let down { mariovision_pointer_button(down) }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else { return }
        report(touch, down: true)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else { return }
        report(touch, down: nil)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else { return }
        report(touch, down: false)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = touches.first else { return }
        report(touch, down: false)
    }
}

struct GameView: UIViewRepresentable {
    func makeUIView(context: Context) -> GameLayerView {
        GameLayerView(frame: .zero)
    }

    func updateUIView(_ uiView: GameLayerView, context: Context) {}

    // The one hook SwiftUI does guarantee when the window scene is torn down.
    static func dismantleUIView(_ uiView: GameLayerView, coordinator: ()) {
        uiView.detach()
    }
}
