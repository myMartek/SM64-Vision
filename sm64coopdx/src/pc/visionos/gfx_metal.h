/*
 * Metal rendering backend for the Fast3D display list interpreter.
 *
 * Implements src/pc/gfx/gfx_rendering_api.h, the same interface gfx_opengl.c
 * implements. visionOS has no OpenGL at all, so this replaces it wholesale.
 *
 * Unlike the desktop backends, this one does not own a swap chain or a render
 * pass: on visionOS the frames come from the app (a CAMetalLayer in phase 1, a
 * CompositorServices drawable in phase 2). The app therefore has to hand the
 * backend a render command encoder for every frame, between gfx_metal_begin_frame
 * and gfx_metal_end_frame, and is responsible for the clear (set the render pass
 * descriptor's loadAction to MTLLoadActionClear - the backend never clears).
 */

#ifndef GFX_METAL_H
#define GFX_METAL_H

#include "pc/gfx/gfx_rendering_api.h"
#include "pc/gfx/gfx_window_manager_api.h"

#ifdef __OBJC__
#import <Metal/Metal.h>

/// Must be called once before gfx_init.
void gfx_metal_set_device(id<MTLDevice> device);

/// Called by the app once per frame, before gfx_run.
/// `encoder` stays owned by the app; the backend only records into it.
/// `sampleCount` must match the attachments the encoder was made with - the
/// pipelines bake it in, so changing it rebuilds them.
void gfx_metal_begin_frame(id<MTLRenderCommandEncoder> encoder,
                           MTLPixelFormat colorFormat,
                           MTLPixelFormat depthFormat,
                           uint32_t widthPx,
                           uint32_t heightPx,
                           uint32_t sampleCount);

/// Called by the app after gfx_run, before ending the encoder.
void gfx_metal_end_frame(void);
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern struct GfxRenderingAPI gfx_metal_api;

/// Reports the drawable size the backend was last given, so the window manager
/// backend can answer get_dimensions consistently.
void gfx_metal_get_dimensions(uint32_t *width, uint32_t *height);

/// Depth direction: false = classic 0..1 with less-equal (the game window),
/// true = reverse-Z with greater-equal (CompositorServices drawables).
void gfx_metal_set_reverse_z(bool reversed);

/// The sub-rectangle of the render target to draw into.
///
/// gfx_pc lays its viewport out over the whole reported drawable size, so this
/// is what maps that onto a CompositorServices view's textureMap.viewport,
/// which need not cover the full texture. Reset by gfx_metal_begin_frame to the
/// full target; call it afterwards to narrow it.
void gfx_metal_set_target_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/// Whether textures carry reduced copies of themselves for use at a distance.
///
/// On by default. Off is the console's own behaviour, which is worth being able
/// to see, and with the original thirty-two-pixel textures it costs little.
void mariovision_set_mipmaps(bool on);

#ifdef __cplusplus
}
#endif

#endif // GFX_METAL_H
