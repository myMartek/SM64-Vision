/*
 * Metal rendering backend - see gfx_metal.h.
 *
 * This is a reimplementation of gfx_opengl.c against Metal rather than a port of
 * libultraship's gfx_metal.cpp: that one targets libultraship's own C++ renderer
 * interface (framebuffer objects, depth readback, ImGui) and a different colour
 * combiner feature set, so almost nothing would have carried over. The shader
 * generation lives in gfx_metal_shader.c.
 *
 * Differences from the OpenGL backend that are deliberate:
 *
 *  - Clip space z is 0..1, so z_is_from_0_to_1 returns true and gfx_pc builds
 *    its projection accordingly (same path the D3D11 backend uses).
 *  - Metal's viewport and scissor origin is the top-left of the render target,
 *    OpenGL's is the bottom-left, so both are flipped on the way in.
 *  - Blending is baked into the pipeline state rather than toggled, so each
 *    shader has two pipelines (blend on/off), created lazily.
 *  - [[position]] in a fragment shader has its origin at the top-left, whereas
 *    gl_FragCoord's is at the bottom-left. That only feeds the noise, dither
 *    and scanline effects, which are symmetric patterns, so the flip is left
 *    uncorrected.
 */

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_metal.h"
#include "gfx_metal_shader.h"
#include "mariovision_portal.h"
#include "pc/gfx/gfx_cc.h"
#include "pc/gfx/gfx_pc.h"
#include "pc/platform.h"
#include "pc/configfile.h"

#define VERTEX_RING_SLOTS 3
#define INITIAL_VERTEX_BUFFER_BYTES (1 << 20) // 1 MiB per slot, grows on demand
#define TEX_CACHE_STEP 512

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

// Mirrors `struct Uniforms` in the generated MSL. Field order is chosen so that
// the C and MSL layouts agree without explicit padding; see the static assert
// after the declaration.
typedef struct {
    float uTex0Size[2];
    float uTex1Size[2];
    float uLightmapColor[4]; // only xyz used; float4 to dodge MSL's float3 alignment
    float uFrameCount;
    int   uTex0Filter;
    int   uTex1Filter;
    int   uFilter;
    int   uShaderFlags[SHADER_FLAG_MAX];
    float uShaderFlagValues[SHADER_FLAG_MAX];
    float uPortalEye[4];  // xyz eye in portal space, w = portal clip on/off
    float uPortalHalf[2];
    float uPortalPad[2];
} MVUniforms;

_Static_assert(sizeof(MVUniforms) == 144, "MVUniforms must match the generated MSL layout");
_Static_assert(SHADER_FLAG_MAX == 8, "shader flag count is baked into the generated MSL");

struct ShaderProgram {
    uint64_t hash;
    bool     valid;

    uint8_t num_inputs;
    bool    used_textures[2];
    uint8_t num_floats;
    uint8_t attrib_sizes[7];
    uint8_t num_attribs;

    bool used_noise;
    bool used_lightmap;
    bool world_geometry;

    // Index into sPipelines: [0] = blending off, [1] = blending on.
    // Created lazily, because a colour combiner is usually only ever used with
    // one of the two.
    __unsafe_unretained id<MTLRenderPipelineState> pipelines[2];
    __unsafe_unretained id<MTLFunction> vertexFunction;
    __unsafe_unretained id<MTLFunction> fragmentFunction;
    __unsafe_unretained MTLVertexDescriptor *vertexDescriptor;
};

@interface MVTexture : NSObject
@property (nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic, assign) float width;
@property (nonatomic, assign) float height;
@property (nonatomic, assign) BOOL linearFilter;
// Filtering and wrap modes belong to the texture, not to the tile it happens to
// be bound to - see gfx_metal_set_sampler_parameters.
@property (nonatomic, strong) id<MTLSamplerState> sampler;
@end

@implementation MVTexture
@end

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

static id<MTLDevice> sDevice;
static id<MTLRenderCommandEncoder> sEncoder;

// ARC cannot own Objective-C references inside a plain C struct, so the shader
// pool's Metal objects are retained here in parallel and referenced weakly-typed
// (__unsafe_unretained) from struct ShaderProgram.
static NSMutableArray *sShaderObjects;

static struct ShaderProgram sShaderPool[CC_MAX_SHADERS];
static uint8_t sShaderPoolSize = 0;
static uint8_t sShaderPoolIndex = 0;
static struct ShaderProgram *sCurrentShader = NULL;

static NSMutableArray<MVTexture *> *sTextures;
static MVTexture *sBoundTexture[2];
static int sCurrentTile = 0;

static id<MTLSamplerState> sSamplerCache[32];

static id<MTLDepthStencilState> sDepthStates[2][4]; // [reverse_z][depth_test*2 + depth_write]

// The compositor's drawables use reverse-Z (near = 1, far = 0), the window path
// classic 0..1. Flipped by the portal renderer around its passes.
static bool sReverseZ = false;

static id<MTLBuffer> sVertexRing[VERTEX_RING_SLOTS];
static NSUInteger sVertexRingCapacity = 0;
static NSUInteger sVertexRingOffset = 0;
static NSUInteger sVertexRingSlot = 0;

static MTLPixelFormat sColorFormat = MTLPixelFormatInvalid;
static MTLPixelFormat sDepthFormat = MTLPixelFormatInvalid;
static uint32_t sSampleCount = 1;
static uint32_t sFramebufferWidth = 0;
static uint32_t sFramebufferHeight = 0;
// Origin of the target rect inside the render target; see
// gfx_metal_set_target_rect.
static uint32_t sTargetOriginX = 0;
static uint32_t sTargetOriginY = 0;

static uint32_t sFrameCount = 0;

// Render state that Metal expresses through the pipeline or the encoder. It is
// applied lazily at draw time so that a run of redundant setters between two
// draws costs nothing.
static bool sDepthTest = false;
static bool sDepthMask = false;
static bool sZmodeDecal = false;
static bool sUseAlpha = false;
static bool sStateDirty = true;

void gfx_metal_set_reverse_z(bool reversed) {
    sReverseZ = reversed;
    sStateDirty = true;
}

//-----------------------------------------------------------------------------
// Metal object helpers
//-----------------------------------------------------------------------------

static MTLSamplerAddressMode gfx_cm_to_metal(uint32_t val) {
    if (val & G_TX_CLAMP) { return MTLSamplerAddressModeClampToEdge; }
    return (val & G_TX_MIRROR) ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
}

static id<MTLSamplerState> sampler_for(bool linear, uint32_t cms, uint32_t cmt) {
    const int key = (linear ? 16 : 0) | ((cms & 3) << 2) | (cmt & 3);
    if (sSamplerCache[key] != nil) { return sSamplerCache[key]; }

    MTLSamplerDescriptor *desc = [MTLSamplerDescriptor new];
    desc.minFilter = linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    desc.magFilter = desc.minFilter;
    desc.sAddressMode = gfx_cm_to_metal(cms);
    desc.tAddressMode = gfx_cm_to_metal(cmt);

    // Which level to read, once there is more than one.
    //
    // Blended between levels when the texture is filtered anyway, and snapped
    // to the nearest when it is not: a texture the game asked to be sampled
    // point-wise is meant to look like squares up close, and blending mip
    // levels would soften exactly that. Shimmer in the distance goes either
    // way, which is the point of having them at all.
    desc.mipFilter = linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;

    // A wall seen at a glancing angle is minified far more along one axis than
    // the other. Without this the level is chosen for the worse of the two and
    // the floor turns to mush a few metres out; with it, four samples are taken
    // along the direction that needs them.
    desc.maxAnisotropy = 4;

    sSamplerCache[key] = [sDevice newSamplerStateWithDescriptor:desc];
    return sSamplerCache[key];
}

static MTLVertexDescriptor *build_vertex_descriptor(const struct ShaderProgram *prg) {
    MTLVertexDescriptor *desc = [MTLVertexDescriptor new];
    NSUInteger offset = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        switch (prg->attrib_sizes[i]) {
            case 2: desc.attributes[i].format = MTLVertexFormatFloat2; break;
            case 3: desc.attributes[i].format = MTLVertexFormatFloat3; break;
            case 4: desc.attributes[i].format = MTLVertexFormatFloat4; break;
            default: sys_fatal("unexpected vertex attribute size %d", prg->attrib_sizes[i]);
        }
        desc.attributes[i].offset = offset;
        desc.attributes[i].bufferIndex = 0;
        offset += prg->attrib_sizes[i] * sizeof(float);
    }

    desc.layouts[0].stride = prg->num_floats * sizeof(float);
    desc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    return desc;
}

static id<MTLRenderPipelineState> pipeline_for(struct ShaderProgram *prg, bool blend) {
    const int slot = blend ? 1 : 0;
    if (prg->pipelines[slot] != nil) { return prg->pipelines[slot]; }

    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = prg->vertexFunction;
    desc.fragmentFunction = prg->fragmentFunction;
    desc.vertexDescriptor = prg->vertexDescriptor;
    desc.depthAttachmentPixelFormat = sDepthFormat;
    desc.rasterSampleCount = sSampleCount;

    MTLRenderPipelineColorAttachmentDescriptor *color = desc.colorAttachments[0];
    color.pixelFormat = sColorFormat;
    color.blendingEnabled = blend;
    if (blend) {
        // Colour: matches glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        // Alpha: deliberately NOT the same. In the portal the alpha channel is
        // the mask against the passthrough camera, so it has to accumulate
        // coverage (Porter-Duff "over"), not be multiplied by the source's own
        // translucency. GL's factor would give a half-transparent shadow over
        // opaque ground 0.5*0.5 + 1*0.5 = 0.75 - and the room shows through the
        // shadow. With ONE it stays at 0.5 + 0.5 = 1.
        //
        // Window mode renders into an opaque layer that ignores alpha, so this
        // is a portal fix that costs the window path nothing.
        color.sourceAlphaBlendFactor = MTLBlendFactorOne;
        color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }

    NSError *error = nil;
    id<MTLRenderPipelineState> state = [sDevice newRenderPipelineStateWithDescriptor:desc error:&error];
    if (state == nil) {
        sys_fatal("failed to create Metal pipeline state:\n%s",
                  error.localizedDescription.UTF8String ?: "unknown error");
    }

    [sShaderObjects addObject:state];
    prg->pipelines[slot] = state;
    return state;
}

/// Pipelines bake in the render target formats, so a format change invalidates
/// every one of them. In practice this happens once, when the first frame
/// arrives and the real formats become known.
static void invalidate_pipelines(void) {
    for (int i = 0; i < CC_MAX_SHADERS; i++) {
        sShaderPool[i].pipelines[0] = nil;
        sShaderPool[i].pipelines[1] = nil;
    }
}

static void ensure_vertex_capacity(NSUInteger bytes) {
    if (sVertexRingCapacity >= bytes) { return; }

    NSUInteger capacity = sVertexRingCapacity ? sVertexRingCapacity : INITIAL_VERTEX_BUFFER_BYTES;
    while (capacity < bytes) { capacity *= 2; }

    for (int i = 0; i < VERTEX_RING_SLOTS; i++) {
        sVertexRing[i] = [sDevice newBufferWithLength:capacity options:MTLResourceStorageModeShared];
        if (sVertexRing[i] == nil) { sys_fatal("out of memory allocating Metal vertex buffer"); }
    }
    sVertexRingCapacity = capacity;
    sVertexRingOffset = 0;
}

//-----------------------------------------------------------------------------
// App-facing entry points
//-----------------------------------------------------------------------------

void gfx_metal_set_device(id<MTLDevice> device) {
    sDevice = device;
}

void gfx_metal_begin_frame(id<MTLRenderCommandEncoder> encoder,
                           MTLPixelFormat colorFormat,
                           MTLPixelFormat depthFormat,
                           uint32_t widthPx,
                           uint32_t heightPx,
                           uint32_t sampleCount) {
    if (colorFormat != sColorFormat || depthFormat != sDepthFormat ||
        sampleCount != sSampleCount) {
        sColorFormat = colorFormat;
        sDepthFormat = depthFormat;
        sSampleCount = sampleCount ? sampleCount : 1;
        invalidate_pipelines();
    }

    sEncoder = encoder;
    sFramebufferWidth = widthPx;
    sFramebufferHeight = heightPx;
    sTargetOriginX = 0;
    sTargetOriginY = 0;

    sVertexRingSlot = (sVertexRingSlot + 1) % VERTEX_RING_SLOTS;
    sVertexRingOffset = 0;

    // Force every piece of encoder state to be re-applied: the encoder is new,
    // so nothing carries over from the previous frame. The samplers are not
    // reset here - they belong to the textures and survive the frame, exactly
    // as they do in OpenGL.
    sStateDirty = true;
    sCurrentShader = NULL;
}

void gfx_metal_end_frame(void) {
    sEncoder = nil;
}

void gfx_metal_set_target_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    sTargetOriginX = x;
    sTargetOriginY = y;
    if (width > 0)  { sFramebufferWidth = width; }
    if (height > 0) { sFramebufferHeight = height; }
}

void gfx_metal_get_dimensions(uint32_t *width, uint32_t *height) {
    if (width)  { *width = sFramebufferWidth; }
    if (height) { *height = sFramebufferHeight; }
}

//-----------------------------------------------------------------------------
// GfxRenderingAPI
//-----------------------------------------------------------------------------

static bool gfx_metal_z_is_from_0_to_1(void) {
    // Window path: gfx_pc builds its own guPerspective projections in OpenGL's
    // [-w, w] clip range, so it has to remap them to Metal's [0, w].
    //
    // Portal path: the injected projection is the compositor's own, which
    // already produces [0, w] reverse-Z depth. Remapping it there would squeeze
    // the range into [w/2, w] - halving depth precision, breaking the
    // compositor's depth reprojection, and above all changing aVtxPos.z so that
    // it is no longer the clip position the portal's clip-space inverse is
    // built from, which made the clip test discard nearly every fragment.
    return !mariovision_portal_pass_active() || mariovision_portal_screen_pass();
}

static void gfx_metal_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg == NULL || old_prg == sCurrentShader) {
        sCurrentShader = NULL;
    }
}

static void gfx_metal_load_shader(struct ShaderProgram *new_prg) {
    sCurrentShader = new_prg;
    sStateDirty = true;
}

static struct ShaderProgram *gfx_metal_create_and_load_new_shader(struct ColorCombiner *cc) {
    struct CCFeatures ccf = { 0 };
    gfx_cc_get_features(cc, &ccf);

    // Sized well above the largest shader coopdx can produce; overrunning it
    // would mean the generator changed, not that some level is unusual.
    static char msl[16384];
    size_t num_floats = 0;
    if (!gfx_metal_generate_shader(cc, &ccf, msl, sizeof(msl), &num_floats)) {
        sys_fatal("generated MSL shader exceeded the %zu byte buffer", sizeof(msl));
    }

    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    id<MTLLibrary> library = [sDevice newLibraryWithSource:@(msl) options:options error:&error];
    if (library == nil) {
        fprintf(stderr, "Generated MSL:\n%s\n", msl);
        sys_fatal("Metal shader compilation failed (see terminal):\n%s",
                  error.localizedDescription.UTF8String ?: "unknown error");
    }

    struct ShaderProgram *prg = &sShaderPool[sShaderPoolIndex];
    sShaderPoolIndex = (sShaderPoolIndex + 1) % CC_MAX_SHADERS;
    if (sShaderPoolSize < CC_MAX_SHADERS) { sShaderPoolSize++; }

    memset(prg, 0, sizeof(*prg));

    const int cnt = gfx_metal_attrib_sizes(cc, &ccf, prg->attrib_sizes);

    prg->hash = cc->hash;
    prg->valid = true;
    prg->num_inputs = ccf.num_inputs;
    prg->used_textures[0] = ccf.used_textures[0];
    prg->used_textures[1] = ccf.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;
    prg->used_noise = (cc->cm.use_alpha && cc->cm.use_dither) || ccf.do_noise;
    prg->used_lightmap = cc->cm.light_map;
    prg->world_geometry = cc->cm.world_geometry;

    id<MTLFunction> vertexFunction = [library newFunctionWithName:@"vertexMain"];
    id<MTLFunction> fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
    MTLVertexDescriptor *vertexDescriptor = build_vertex_descriptor(prg);

    [sShaderObjects addObject:library];
    [sShaderObjects addObject:vertexFunction];
    [sShaderObjects addObject:fragmentFunction];
    [sShaderObjects addObject:vertexDescriptor];

    prg->vertexFunction = vertexFunction;
    prg->fragmentFunction = fragmentFunction;
    prg->vertexDescriptor = vertexDescriptor;

    gfx_metal_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_metal_lookup_shader(struct ColorCombiner *cc) {
    for (size_t i = 0; i < sShaderPoolSize; i++) {
        if (sShaderPool[i].valid && sShaderPool[i].hash == cc->hash) {
            return &sShaderPool[i];
        }
    }
    return NULL;
}

static void gfx_metal_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

//-----------------------------------------------------------------------------
// Mip levels
//
// The N64 had none and did not miss them: its textures were 32 by 32 and the
// screen was 320 across, so a wall was rarely minified at all. A replacement
// pack changes both halves of that - a thousand pixels of texture on a wall
// twenty metres off, resampled afresh every frame from whatever texels happen
// to land under the sample points. That is the shimmer, and no amount of
// multisampling touches it: multisampling smooths the edges of triangles, and
// this is happening inside one.
//
// Built here rather than with a blit encoder, because the upload happens in the
// middle of a render pass that would have to be ended and begun again around
// it. A box filter over four texels, on a texture that is uploaded once and
// then read for as long as it stays in the cache, is not worth that.
//-----------------------------------------------------------------------------

static _Atomic bool sMipmaps = true;
static _Atomic bool sTexturesStale = false;

void mariovision_set_mipmaps(bool on) {
    if (atomic_exchange(&sMipmaps, on) != on) { atomic_store(&sTexturesStale, true); }
}

static uint8_t *sMipScratch[2];
static size_t sMipScratchSize;

static void mip_scratch_reserve(size_t bytes) {
    if (bytes <= sMipScratchSize) { return; }
    for (int i = 0; i < 2; i++) {
        free(sMipScratch[i]);
        sMipScratch[i] = malloc(bytes);
        if (sMipScratch[i] == NULL) { sys_fatal("out of memory building mip levels"); }
    }
    sMipScratchSize = bytes;
}

static NSUInteger mip_level_count(int w, int h) {
    NSUInteger n = 1;
    while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; }
    return n;
}

/// Halve an RGBA image, weighting the colours by how opaque they are.
///
/// A plain average would let a fully transparent texel drag its colour into the
/// result. Those texels are usually left black by whoever made the texture, so
/// the edge of a fence or a leaf picks up a dark fringe that grows with every
/// level. Weighting by alpha keeps the colour of what can actually be seen.
static void mip_halve(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        const int y0 = y * 2, y1 = (y * 2 + 1 < sh) ? y * 2 + 1 : sh - 1;
        for (int x = 0; x < dw; x++) {
            const int x0 = x * 2, x1 = (x * 2 + 1 < sw) ? x * 2 + 1 : sw - 1;
            const uint8_t *p[4] = {
                src + ((size_t) y0 * sw + x0) * 4, src + ((size_t) y0 * sw + x1) * 4,
                src + ((size_t) y1 * sw + x0) * 4, src + ((size_t) y1 * sw + x1) * 4,
            };
            uint32_t a = 0, r = 0, g = 0, b = 0;
            for (int i = 0; i < 4; i++) { a += p[i][3]; }
            if (a == 0) {
                for (int i = 0; i < 4; i++) { r += p[i][0]; g += p[i][1]; b += p[i][2]; }
                r /= 4; g /= 4; b /= 4;
            } else {
                for (int i = 0; i < 4; i++) {
                    r += (uint32_t) p[i][0] * p[i][3];
                    g += (uint32_t) p[i][1] * p[i][3];
                    b += (uint32_t) p[i][2] * p[i][3];
                }
                r /= a; g /= a; b /= a;
            }
            uint8_t *d = dst + ((size_t) y * dw + x) * 4;
            d[0] = (uint8_t) r; d[1] = (uint8_t) g; d[2] = (uint8_t) b; d[3] = (uint8_t) (a / 4);
        }
    }
}

static uint32_t gfx_metal_new_texture(void) {
    MVTexture *tex = [MVTexture new];
    [sTextures addObject:tex];
    return (uint32_t) (sTextures.count - 1);
}

static void gfx_metal_select_texture(int tile, uint32_t texture_id) {
    if (texture_id >= sTextures.count) { return; }
    sBoundTexture[tile] = sTextures[texture_id];
    sCurrentTile = tile;
    sStateDirty = true;
}

static void gfx_metal_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    MVTexture *tex = sBoundTexture[sCurrentTile];
    if (tex == nil || width <= 0 || height <= 0) { return; }

    const bool wantMips = atomic_load(&sMipmaps) && (width > 1 || height > 1);
    const NSUInteger levels = wantMips ? mip_level_count(width, height) : 1;

    // Always a new texture, never a write into the one that was there.
    //
    // gfx_pc's cache reuses its slots, and a slot that comes round again while
    // a command buffer from an earlier frame is still in flight would have had
    // its pixels replaced underneath the draw calls already reading them. On
    // shared storage that write is not synchronised against the GPU at all -
    // it is a memcpy - so what got drawn was a race, and it looked like a
    // texture flickering between two things. A fresh texture cannot be raced:
    // the encoder holds on to the old one until its command buffer is done,
    // and it is let go after that.
    MTLTextureDescriptor *desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    desc.mipmapLevelCount = levels;
    tex.texture = [sDevice newTextureWithDescriptor:desc];
    if (tex.texture == nil) { sys_fatal("out of memory allocating a %dx%d Metal texture", width, height); }

    [tex.texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0
                     withBytes:rgba32_buf
                   bytesPerRow:(NSUInteger) width * 4];

    if (levels > 1) {
        mip_scratch_reserve((size_t) width * height * 4);
        const uint8_t *src = rgba32_buf;
        int sw = width, sh = height;
        for (NSUInteger level = 1; level < levels; level++) {
            const int dw = sw > 1 ? sw / 2 : 1;
            const int dh = sh > 1 ? sh / 2 : 1;
            uint8_t *dst = sMipScratch[level & 1];
            mip_halve(src, sw, sh, dst, dw, dh);
            [tex.texture replaceRegion:MTLRegionMake2D(0, 0, dw, dh)
                           mipmapLevel:level
                             withBytes:dst
                           bytesPerRow:(NSUInteger) dw * 4];
            src = dst; sw = dw; sh = dh;
        }
    }

    tex.width = width;
    tex.height = height;
    sStateDirty = true;
}

/// Applies to the texture currently bound to `tile`, not to the tile itself.
///
/// gfx_pc tracks these per texture (TextureHashmapNode's cms/cmt/linear_filter)
/// and only calls this when they differ from what the bound texture already
/// has - which is OpenGL's model, where the parameters live on the texture
/// object. Storing them per tile instead would leave a cached texture being
/// sampled with whatever the previous texture in that tile used, and the
/// mismatched wrap mode shows up as bleeding along texture edges.
static void gfx_metal_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    sCurrentTile = tile;

    MVTexture *tex = sBoundTexture[tile];
    if (tex != nil) {
        tex.sampler = sampler_for(linear_filter, cms, cmt);
        tex.linearFilter = linear_filter;
    }
    sStateDirty = true;
}

static void gfx_metal_set_depth_test(bool depth_test) {
    if (sDepthTest != depth_test) { sDepthTest = depth_test; sStateDirty = true; }
}

static void gfx_metal_set_depth_mask(bool z_upd) {
    if (sDepthMask != z_upd) { sDepthMask = z_upd; sStateDirty = true; }
}

static void gfx_metal_set_zmode_decal(bool zmode_decal) {
    if (sZmodeDecal != zmode_decal) { sZmodeDecal = zmode_decal; sStateDirty = true; }
}

/// In the portal the game's screen is not a rectangle of pixels any more - it is
/// a quad projected into the eye texture, and where it lands moves with the
/// head. Viewport and scissor rectangles are computed by the game in screen
/// pixels, so applying them cuts straight across that quad in axis-aligned
/// blocks that shift as you look around. The portal's own clip test already
/// trims the image at the window edges, in the right space, so both simply
/// cover the whole target here.
static bool portal_full_target(int *x, int *y, int *width, int *height) {
    if (!mariovision_portal_pass_active()) { return false; }
    *x = (int) sTargetOriginX;
    *y = (int) sTargetOriginY;
    *width = (int) sFramebufferWidth;
    *height = (int) sFramebufferHeight;
    return true;
}

static void gfx_metal_set_viewport(int x, int y, int width, int height) {
    if (sEncoder == nil || width <= 0 || height <= 0) { return; }

    int fx, fy, fw, fh;
    if (portal_full_target(&fx, &fy, &fw, &fh)) {
        [sEncoder setViewport:(MTLViewport){ fx, fy, fw, fh, 0.0, 1.0 }];
        return;
    }

    // OpenGL measures y from the bottom of the render target, Metal from the top.
    MTLViewport viewport = {
        .originX = (double) (x + (int) sTargetOriginX),
        .originY = (double) ((int) sTargetOriginY + (int) sFramebufferHeight - (y + height)),
        .width   = (double) width,
        .height  = (double) height,
        .znear   = 0.0,
        .zfar    = 1.0,
    };
    [sEncoder setViewport:viewport];
}

static void gfx_metal_set_scissor(int x, int y, int width, int height) {
    if (sEncoder == nil) { return; }

    int fx, fy, fw, fh;
    if (portal_full_target(&fx, &fy, &fw, &fh)) {
        [sEncoder setScissorRect:(MTLScissorRect){ (NSUInteger) fx, (NSUInteger) fy,
                                                   (NSUInteger) fw, (NSUInteger) fh }];
        return;
    }

    int top = (int) sFramebufferHeight - (y + height);

    // Metal rejects a scissor rect that leaves the render target, which the
    // game's rects can do at odd aspect ratios, so clamp rather than trap.
    int x0 = x < 0 ? 0 : x;
    int y0 = top < 0 ? 0 : top;
    int x1 = x + width;
    int y1 = top + height;
    if (x1 > (int) sFramebufferWidth)  { x1 = (int) sFramebufferWidth; }
    if (y1 > (int) sFramebufferHeight) { y1 = (int) sFramebufferHeight; }
    if (x1 <= x0 || y1 <= y0) { return; }

    MTLScissorRect rect = {
        .x = (NSUInteger) (x0 + (int) sTargetOriginX),
        .y = (NSUInteger) (y0 + (int) sTargetOriginY),
        .width = (NSUInteger) (x1 - x0),
        .height = (NSUInteger) (y1 - y0),
    };
    [sEncoder setScissorRect:rect];
}

static void gfx_metal_set_use_alpha(bool use_alpha) {
    if (sUseAlpha != use_alpha) { sUseAlpha = use_alpha; sStateDirty = true; }
}

static void apply_state(void) {
    if (sCurrentShader == NULL) { return; }

    [sEncoder setRenderPipelineState:pipeline_for(sCurrentShader, sUseAlpha)];
    [sEncoder setDepthStencilState:sDepthStates[sReverseZ ? 1 : 0][(sDepthTest ? 2 : 0) + (sDepthMask ? 1 : 0)]];

    // Matches glPolygonOffset(-2, -2) with GL_POLYGON_OFFSET_FILL; with
    // reverse-Z, "toward the viewer" flips sign.
    //
    // The decal and what it sits on are exactly coplanar - Mario's shadow lies
    // in the floor's own plane - so there is no depth difference to resolve and
    // this offset is the only thing separating them. Two things make that
    // margin thin in the portal: reverse-Z with an infinite far plane squeezes
    // everything past a few metres into the first thousandths, where a float's
    // resolvable step is tiny and a bias counted in those steps is tiny with
    // it; and on flat ground the slope term contributes nothing at all, so the
    // constant carries the whole separation on its own.
    //
    // Raised for that path accordingly. MARIOVISION_DECAL_BIAS overrides it,
    // which is how the right magnitude gets found without a rebuild.
    if (sZmodeDecal) {
        static float sBiasScale = -1.0f;
        if (sBiasScale < 0.0f) {
            const char *raw = getenv("MARIOVISION_DECAL_BIAS");
            // 64 helped but did not settle it. The comparison is already
            // equal-inclusive, so nothing is being rejected for being level with
            // the floor - the two surfaces are triangulated differently and
            // their interpolated depths disagree from pixel to pixel. That is
            // ordinary z-fighting and wants a margin well clear of the
            // disagreement rather than one that merely matches it.
            //
            // A large value is close to free here: the bias moves the depth
            // only, never the position, so nothing shifts on screen. What it
            // does risk is the shadow showing through something genuinely in
            // front of it, which is the thing to watch for.
            sBiasScale = raw ? strtof(raw, NULL) : 512.0f;
        }
        const float bias = sReverseZ ? sBiasScale : -2.0f;
        const float slope = sReverseZ ? 4.0f : -2.0f;
        [sEncoder setDepthBias:bias slopeScale:slope clamp:0.0f];
    } else {
        [sEncoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    }

    MVUniforms uniforms = { 0 };
    uniforms.uFrameCount = (float) sFrameCount;
    uniforms.uFilter = configFiltering;

    // Portal clip parameters; identity/off outside a portal pass. The vertex
    // stage always has buffer(1) bound - the matrix is what varies.
    float clipToPortal[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float eyePortal[3] = { 0, 0, 0 };
    float half[2] = { 0, 0 };
    if (mariovision_portal_shader_params(clipToPortal, eyePortal, half)) {
        // The clip test is what makes the image stop at the window frame. In VR
        // mode there is no frame, so it stays off. MARIOVISION_NOCLIP=1 forces
        // the same thing for diagnostics.
        static int sNoClip = -1;
        if (sNoClip < 0) { sNoClip = getenv("MARIOVISION_NOCLIP") != NULL; }
        const bool unclipped = sNoClip || mariovision_portal_unclipped();
        uniforms.uPortalEye[0] = eyePortal[0];
        uniforms.uPortalEye[1] = eyePortal[1];
        uniforms.uPortalEye[2] = eyePortal[2];
        // 1 = portal with a frame, 2 = portal without one. Not 0: that also
        // switches off the sRGB linearisation further down in the shader, and
        // VR mode came out visibly paler because of it.
        uniforms.uPortalEye[3] = unclipped ? 2.0f : 1.0f;
        uniforms.uPortalHalf[0] = half[0];
        uniforms.uPortalHalf[1] = half[1];
    }
    [sEncoder setVertexBytes:clipToPortal length:sizeof(clipToPortal) atIndex:1];

    for (int t = 0; t < 2; t++) {
        if (!sCurrentShader->used_textures[t]) { continue; }

        MVTexture *tex = sBoundTexture[t];
        if (tex == nil || tex.texture == nil) { continue; }

        float *size = (t == 0) ? uniforms.uTex0Size : uniforms.uTex1Size;
        size[0] = tex.width;
        size[1] = tex.height;
        (t == 0 ? &uniforms.uTex0Filter : &uniforms.uTex1Filter)[0] = tex.linearFilter ? 1 : 0;

        [sEncoder setFragmentTexture:tex.texture atIndex:t];
        // The fallback matches the set_sampler_parameters(tile, false, 0, 0)
        // gfx_pc issues right after new_texture, so it should never be needed.
        [sEncoder setFragmentSamplerState:tex.sampler ?: sampler_for(false, 0, 0) atIndex:t];
    }

    if (sCurrentShader->used_lightmap) {
        uniforms.uLightmapColor[0] = gVertexColor[0] / 255.0f;
        uniforms.uLightmapColor[1] = gVertexColor[1] / 255.0f;
        uniforms.uLightmapColor[2] = gVertexColor[2] / 255.0f;
    }

    if (sCurrentShader->world_geometry) {
        memcpy(uniforms.uShaderFlags, gShaderFlags, sizeof(uniforms.uShaderFlags));
        memcpy(uniforms.uShaderFlagValues, gShaderFlagValues, sizeof(uniforms.uShaderFlagValues));
    }

    [sEncoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    sStateDirty = false;
}

_Atomic long gMVDebugTris = 0;
_Atomic long gMVDebugDraws = 0;

static void gfx_metal_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (sEncoder == nil || sCurrentShader == NULL || buf_vbo_num_tris == 0) { return; }
    atomic_fetch_add(&gMVDebugTris, (long) buf_vbo_num_tris);
    atomic_fetch_add(&gMVDebugDraws, 1);

    const NSUInteger bytes = buf_vbo_len * sizeof(float);
    ensure_vertex_capacity(bytes);

    // Vertex buffer offsets have to be 4-byte aligned, which a float array
    // always is; the round-up is here in case the vertex format ever changes.
    NSUInteger offset = (sVertexRingOffset + 3) & ~(NSUInteger) 3;
    if (offset + bytes > sVertexRingCapacity) {
        // A single frame overflowed the slot. Grow and restart the slot: the
        // draws already recorded reference the old buffer, which Metal keeps
        // alive through the command buffer.
        ensure_vertex_capacity(sVertexRingCapacity * 2);
        offset = 0;
    }

    id<MTLBuffer> buffer = sVertexRing[sVertexRingSlot];
    memcpy((uint8_t *) buffer.contents + offset, buf_vbo, bytes);
    sVertexRingOffset = offset + bytes;

    if (sStateDirty) { apply_state(); }

    [sEncoder setVertexBuffer:buffer offset:offset atIndex:0];
    [sEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0
                 vertexCount:3 * buf_vbo_num_tris];
}

static void gfx_metal_init(void) {
    if (sDevice == nil) {
        sys_fatal("gfx_metal_set_device must be called before gfx_init");
    }

    sShaderObjects = [NSMutableArray new];
    sTextures = [[NSMutableArray alloc] initWithCapacity:TEX_CACHE_STEP];

    // [reverse_z][depth_test*2 + depth_write]. Classic matches
    // glDepthFunc(GL_LEQUAL); reverse-Z mirrors it to GreaterEqual.
    for (int rev = 0; rev < 2; rev++) {
        for (int test = 0; test < 2; test++) {
            for (int write = 0; write < 2; write++) {
                MTLDepthStencilDescriptor *desc = [MTLDepthStencilDescriptor new];
                desc.depthCompareFunction = !test ? MTLCompareFunctionAlways
                    : (rev ? MTLCompareFunctionGreaterEqual : MTLCompareFunctionLessEqual);
                desc.depthWriteEnabled = write ? YES : NO;
                sDepthStates[rev][test * 2 + write] = [sDevice newDepthStencilStateWithDescriptor:desc];
            }
        }
    }

    ensure_vertex_capacity(INITIAL_VERTEX_BUFFER_BYTES);
}

static void gfx_metal_on_resize(void) {
}

static void gfx_metal_start_frame(void) {
    // Turning mip levels on or off changes every texture already uploaded, and
    // the cache would go on handing out the old ones. Emptied here, on the
    // thread that owns it, rather than from the switch in the settings.
    if (atomic_exchange(&sTexturesStale, false)) { gfx_texture_cache_clear(); }

    sFrameCount++;

    // The clear belongs to the render pass descriptor, which the app owns - see
    // the note in gfx_metal.h.
    sStateDirty = true;
}

// Distinct from the app-facing gfx_metal_end_frame: this is the Fast3D hook
// that runs while the encoder is still open, the other one closes the frame.
static void gfx_metal_api_end_frame(void) {
}

static void gfx_metal_finish_render(void) {
}

static const char *gfx_metal_get_name(void) {
    return "Metal";
}

static void gfx_metal_shutdown(void) {
    for (int i = 0; i < CC_MAX_SHADERS; i++) {
        memset(&sShaderPool[i], 0, sizeof(sShaderPool[i]));
    }
    sShaderPoolSize = 0;
    sShaderPoolIndex = 0;
    sCurrentShader = NULL;

    [sShaderObjects removeAllObjects];
    [sTextures removeAllObjects];
    sBoundTexture[0] = nil;
    sBoundTexture[1] = nil;

    for (int i = 0; i < VERTEX_RING_SLOTS; i++) { sVertexRing[i] = nil; }
    sVertexRingCapacity = 0;
}

struct GfxRenderingAPI gfx_metal_api = {
    gfx_metal_z_is_from_0_to_1,
    gfx_metal_unload_shader,
    gfx_metal_load_shader,
    gfx_metal_create_and_load_new_shader,
    gfx_metal_lookup_shader,
    gfx_metal_shader_get_info,
    gfx_metal_new_texture,
    gfx_metal_select_texture,
    gfx_metal_upload_texture,
    gfx_metal_set_sampler_parameters,
    gfx_metal_set_depth_test,
    gfx_metal_set_depth_mask,
    gfx_metal_set_zmode_decal,
    gfx_metal_set_viewport,
    gfx_metal_set_scissor,
    gfx_metal_set_use_alpha,
    gfx_metal_draw_triangles,
    gfx_metal_init,
    gfx_metal_on_resize,
    gfx_metal_start_frame,
    gfx_metal_api_end_frame,
    gfx_metal_finish_render,
    gfx_metal_get_name,
    gfx_metal_shutdown,
};
