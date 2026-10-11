import Foundation
import Metal
import simd

/// A scene of our own, for showing the app without the game.
///
/// The app cannot hand over the game's data - a file that satisfies its checksum
/// is the original ROM, and the ROM carries the geometry, the collision and the
/// sound, not merely textures. What can be shown instead is everything the app
/// itself does: the portal and its frame, the stereo pair, the render modes, the
/// window that is grabbed and moved. None of that needs the game.
///
/// So this draws a small world of its own through exactly the same path: the
/// same per-eye projection, the same placement, the same depth handling. What
/// behaves here is what behaves with the game.
struct DemoScene {
    private var pipeline: MTLRenderPipelineState?
    private var depthState: MTLDepthStencilState?
    private var vertexBuffer: MTLBuffer?
    private var vertexCount = 0

    /// Position, normal and colour per vertex - laid out for the shader below.
    private struct Vertex {
        var position: SIMD3<Float>
        var normal: SIMD3<Float>
        var color: SIMD3<Float>
    }

    /// What the fragment stage needs to trim the scene at the window's frame.
    /// Mirrors the game's own shader exactly - same test, so the portal behaves
    /// the same here as it does with the game behind it.
    struct PortalClip {
        var clipToPortal = matrix_identity_float4x4
        var eye = SIMD4<Float>(0, 0, 0, 0)   // xyz in portal space, w: 0 off, 1 clip
        var half = SIMD2<Float>(0.5, 0.375)
    }

    /// How far above the floor the viewer stands, in metres. Applied as a
    /// translation at draw time rather than baked into the vertices, because the
    /// world scale differs per mode - a fixed offset in game units would put the
    /// floor at ankle height in one mode and four metres down in another.
    static let eyeHeightMetres: Float = 1.15

    /// Built in the game's own units, so the world scale that the portal applies
    /// puts it at the same size the game would be.
    private static func build() -> [Vertex] {
        var out: [Vertex] = []

        func quad(_ a: SIMD3<Float>, _ b: SIMD3<Float>, _ c: SIMD3<Float>, _ d: SIMD3<Float>,
                  _ color: SIMD3<Float>) {
            let n = simd_normalize(simd_cross(b - a, c - a))
            for p in [a, b, c, a, c, d] {
                out.append(Vertex(position: p, normal: n, color: color))
            }
        }

        func box(center: SIMD3<Float>, size: SIMD3<Float>, color: SIMD3<Float>) {
            let h = size * 0.5
            let x0 = center.x - h.x, x1 = center.x + h.x
            let y0 = center.y - h.y, y1 = center.y + h.y
            let z0 = center.z - h.z, z1 = center.z + h.z
            let shade: (Float) -> SIMD3<Float> = { simd_clamp(color * $0, .zero, .one) }
            quad(SIMD3(x0, y1, z0), SIMD3(x0, y1, z1), SIMD3(x1, y1, z1), SIMD3(x1, y1, z0), shade(1.15))
            quad(SIMD3(x0, y0, z1), SIMD3(x0, y0, z0), SIMD3(x1, y0, z0), SIMD3(x1, y0, z1), shade(0.55))
            quad(SIMD3(x0, y0, z0), SIMD3(x0, y1, z0), SIMD3(x1, y1, z0), SIMD3(x1, y0, z0), shade(0.85))
            quad(SIMD3(x1, y0, z1), SIMD3(x1, y1, z1), SIMD3(x0, y1, z1), SIMD3(x0, y0, z1), shade(0.75))
            quad(SIMD3(x0, y0, z1), SIMD3(x0, y1, z1), SIMD3(x0, y1, z0), SIMD3(x0, y0, z0), shade(0.65))
            quad(SIMD3(x1, y0, z0), SIMD3(x1, y1, z0), SIMD3(x1, y1, z1), SIMD3(x1, y0, z1), shade(0.95))
        }

        // A chequered floor. Depth reads far better on a patterned surface than
        // on a flat one - the pattern is what makes the stereo legible.
        let tile: Float = 150
        let tiles = 14
        for ix in -tiles...tiles {
            for iz in -tiles...tiles {
                let x = Float(ix) * tile, z = Float(iz) * tile
                let light = (ix + iz) % 2 == 0
                let c: SIMD3<Float> = light ? SIMD3(0.82, 0.84, 0.88) : SIMD3(0.34, 0.38, 0.46)
                quad(SIMD3(x, 0, z), SIMD3(x, 0, z + tile),
                     SIMD3(x + tile, 0, z + tile), SIMD3(x + tile, 0, z), c)
            }
        }

        // Blocks at staggered distances. Near and far together are what show
        // whether the stereo pair fuses across the whole depth budget.
        box(center: SIMD3(0, 90, -420), size: SIMD3(180, 180, 180), color: SIMD3(0.90, 0.28, 0.24))
        box(center: SIMD3(-330, 60, -700), size: SIMD3(120, 120, 120), color: SIMD3(0.20, 0.62, 0.90))
        box(center: SIMD3(340, 130, -900), size: SIMD3(150, 260, 150), color: SIMD3(0.95, 0.76, 0.20))
        box(center: SIMD3(-120, 40, -180), size: SIMD3(80, 80, 80), color: SIMD3(0.30, 0.78, 0.42))
        box(center: SIMD3(700, 200, -1400), size: SIMD3(240, 400, 240), color: SIMD3(0.62, 0.38, 0.86))
        box(center: SIMD3(-760, 170, -1250), size: SIMD3(200, 340, 200), color: SIMD3(0.95, 0.52, 0.18))

        return out
    }

    mutating func prepare(device: MTLDevice, colorFormat: MTLPixelFormat,
                          depthFormat: MTLPixelFormat, sampleCount: Int, reverseZ: Bool) {
        if pipeline != nil { return }

        let source = """
        #include <metal_stdlib>
        using namespace metal;

        struct VIn  { float3 pos; float3 nrm; float3 col; };
        struct VOut { float4 pos [[position]]; float3 col; float shade; float4 portal; };

        struct Clip { float4x4 toPortal; float4 eye; float2 halfExtent; float2 pad; };

        vertex VOut mv_demo_vs(uint vid [[vertex_id]],
                               const device VIn *v [[buffer(0)]],
                               constant float4x4 &mvp [[buffer(1)]],
                               constant Clip &clip [[buffer(2)]]) {
            VOut o;
            o.pos = mvp * float4(v[vid].pos, 1);
            o.portal = clip.toPortal * o.pos;
            o.col = v[vid].col;
            // A fixed key light, so the blocks read as solid bodies rather than
            // flat silhouettes - shape is half of what makes depth readable.
            const float3 key = normalize(float3(0.35, 0.85, 0.4));
            o.shade = 0.45 + 0.55 * saturate(dot(normalize(v[vid].nrm), key));
            return o;
        }

        fragment float4 mv_demo_fs(VOut in [[stage_in]], constant Clip &clip [[buffer(0)]]) {
            // Straight from the game's shader: a fragment is visible when the
            // ray from the eye to it crosses the window rectangle in the z = 0
            // plane of portal space.
            if (clip.eye.w > 0.5 && clip.eye.w < 1.5) {
                float3 pp = in.portal.xyz / in.portal.w;
                if (pp.z >= 0.0) { discard_fragment(); }
                float t = clip.eye.z / (clip.eye.z - pp.z);
                float2 q = clip.eye.xy + t * (pp.xy - clip.eye.xy);
                if (abs(q.x) > clip.halfExtent.x || abs(q.y) > clip.halfExtent.y) { discard_fragment(); }
            }
            return float4(in.col * in.shade, 1.0);
        }
        """

        // Reported rather than swallowed: a failed library leaves the pipeline
        // nil and the scene simply never appears, with nothing to say why.
        let library: MTLLibrary
        do {
            library = try device.makeLibrary(source: source, options: nil)
        } catch {
            NSLog("[demo] shader failed: \(error)")
            return
        }
        guard let vs = library.makeFunction(name: "mv_demo_vs"),
              let fs = library.makeFunction(name: "mv_demo_fs") else {
            NSLog("[demo] shader functions missing")
            return
        }

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = vs
        desc.fragmentFunction = fs
        desc.colorAttachments[0].pixelFormat = colorFormat
        desc.depthAttachmentPixelFormat = depthFormat
        desc.rasterSampleCount = sampleCount
        pipeline = try? device.makeRenderPipelineState(descriptor: desc)

        let dd = MTLDepthStencilDescriptor()
        // The portal runs reverse-Z; nearer is the greater value there.
        dd.depthCompareFunction = reverseZ ? .greater : .less
        dd.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: dd)

        let verts = Self.build()
        vertexCount = verts.count
        vertexBuffer = verts.withUnsafeBytes {
            device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
        }
    }

    func draw(encoder: MTLRenderCommandEncoder, mvp: simd_float4x4, clip: PortalClip) {
        guard let pipeline, let depthState, let vertexBuffer, vertexCount > 0 else { return }
        var matrix = mvp
        var clipUniform = clip
        encoder.setRenderPipelineState(pipeline)
        encoder.setDepthStencilState(depthState)
        encoder.setCullMode(.none)
        encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
        encoder.setVertexBytes(&matrix, length: MemoryLayout<simd_float4x4>.size, index: 1)
        encoder.setVertexBytes(&clipUniform, length: MemoryLayout<PortalClip>.stride, index: 2)
        encoder.setFragmentBytes(&clipUniform, length: MemoryLayout<PortalClip>.stride, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertexCount)
    }
}
