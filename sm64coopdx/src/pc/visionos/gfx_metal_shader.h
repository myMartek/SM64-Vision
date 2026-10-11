/*
 * Metal Shading Language generator for the Fast3D colour combiner.
 *
 * Split out of gfx_metal.m because it is plain C with no Metal dependency,
 * which lets it be exercised on the host: tools/test_msl_shaders.sh generates
 * the shaders for a matrix of combiner configurations and pushes each through
 * the offline Metal compiler, so a syntax error surfaces at build time instead
 * of as a fatal error on the headset.
 */

#ifndef GFX_METAL_SHADER_H
#define GFX_METAL_SHADER_H

#include <stddef.h>
#include <stdbool.h>

#include "pc/gfx/gfx_cc.h"

/**
 * Emits a complete .metal source file for `cc` into `out`.
 *
 * @param numFloatsOut  receives the vertex stride in floats, which has to match
 *                      the layout gfx_pc packs its vertex buffer in.
 * @return false if the shader did not fit in `outSize`; `out` is then undefined.
 */
bool gfx_metal_generate_shader(struct ColorCombiner *cc,
                               const struct CCFeatures *ccf,
                               char *out,
                               size_t outSize,
                               size_t *numFloatsOut);

/**
 * Vertex attribute sizes in the order gfx_pc packs them, matching the VSIn
 * struct the generator emits. Returns the attribute count.
 */
int gfx_metal_attrib_sizes(struct ColorCombiner *cc,
                           const struct CCFeatures *ccf,
                           unsigned char *sizesOut);

#endif // GFX_METAL_SHADER_H
