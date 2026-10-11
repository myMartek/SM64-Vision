/*
 * Metal Shading Language generator - see gfx_metal_shader.h.
 *
 * This is a direct translation of the GLSL generator in gfx_opengl.c: same
 * control flow, same combiner semantics, only the emitted syntax differs. Keep
 * the two in step when coopdx changes its shaders.
 *
 * The fragment function copies its stage_in varyings into locals named exactly
 * like the GLSL varyings (vInput1, vTexCoord0, vFog, ...), which is what lets
 * shader_item_to_str and append_formula stay line-for-line identical to the
 * OpenGL versions apart from vecN -> floatN.
 */

#include <stdio.h>
#include <string.h>

#include "gfx_metal_shader.h"
#include "pc/gfx/gfx_pc.h" // SHADER_FLAG_MAX

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha,
                                      bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_INPUT_5:
                return with_alpha || !inputs_have_alpha ? "vInput5" : "vInput5.rgb";
            case SHADER_INPUT_6:
                return with_alpha || !inputs_have_alpha ? "vInput6" : "vInput6.rgb";
            case SHADER_INPUT_7:
                return with_alpha || !inputs_have_alpha ? "vInput7" : "vInput7.rgb";
            case SHADER_INPUT_8:
                return with_alpha || !inputs_have_alpha ? "vInput8" : "vInput8.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a" :
                    (with_alpha ? "float4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                : "float3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a" :
                    (with_alpha ? "float4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                : "float3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_COMBINEDA:
                return hint_single_element ? "texel.a" :
                    (with_alpha ? "float4(texel.a, texel.a, texel.a, texel.a)"
                                : "float3(texel.a, texel.a, texel.a)");
            case SHADER_NOISE:
                return with_alpha ? "float4(noise)" : "float3(noise)";
        }
    } else {
        switch (item) {
            case SHADER_0:          return "0.0";
            case SHADER_1:          return "1.0";
            case SHADER_INPUT_1:    return "vInput1.a";
            case SHADER_INPUT_2:    return "vInput2.a";
            case SHADER_INPUT_3:    return "vInput3.a";
            case SHADER_INPUT_4:    return "vInput4.a";
            case SHADER_INPUT_5:    return "vInput5.a";
            case SHADER_INPUT_6:    return "vInput6.a";
            case SHADER_INPUT_7:    return "vInput7.a";
            case SHADER_INPUT_8:    return "vInput8.a";
            case SHADER_TEXEL0:     return "texVal0.a";
            case SHADER_TEXEL0A:    return "texVal0.a";
            case SHADER_TEXEL1:     return "texVal1.a";
            case SHADER_TEXEL1A:    return "texVal1.a";
            case SHADER_COMBINED:   return "texel.a";
            case SHADER_COMBINEDA:  return "texel.a";
            case SHADER_NOISE:      return "noise.a";
        }
    }
    return "unknown";
}

static void append_formula(char *buf, size_t *len, uint8_t *cmd, bool do_single, bool do_multiply,
                           bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(cmd[only_alpha * 4 + 3], with_alpha, only_alpha, opt_alpha, false));
    }
}

int gfx_metal_attrib_sizes(struct ColorCombiner *cc, const struct CCFeatures *ccf,
                           unsigned char *sizesOut) {
    int cnt = 0;
    sizesOut[cnt++] = 4; // aVtxPos
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) { sizesOut[cnt++] = 2; }
    }
    if (cc->cm.use_fog)   { sizesOut[cnt++] = 4; }
    if (cc->cm.light_map) { sizesOut[cnt++] = 2; }
    for (int i = 0; i < ccf->num_inputs; i++) {
        sizesOut[cnt++] = cc->cm.use_alpha ? 4 : 3;
    }
    return cnt;
}

bool gfx_metal_generate_shader(struct ColorCombiner *cc, const struct CCFeatures *ccf,
                               char *out, size_t outSize, size_t *numFloatsOut) {
    const bool opt_alpha        = cc->cm.use_alpha;
    const bool opt_fog          = cc->cm.use_fog;
    const bool opt_texture_edge = cc->cm.texture_edge;
    const bool opt_2cycle       = cc->cm.use_2cycle;
    const bool opt_light_map    = cc->cm.light_map;
    const bool opt_dither       = cc->cm.use_dither;
    const bool world_geometry   = cc->cm.world_geometry;

    size_t len = 0;
    size_t num_floats = 4;
    const int input_width = opt_alpha ? 4 : 3;

    append_line(out, &len, "#include <metal_stdlib>");
    append_line(out, &len, "using namespace metal;");
    // The combiner inputs are mirrored into locals uniformly, but whether a
    // given one ends up referenced depends on the formula - do_single, for
    // instance, drops three of its four operands. Warning about that would be
    // warning about the input data, not about the generator.
    append_line(out, &len, "#pragma clang diagnostic ignored \"-Wunused-variable\"");
    append_line(out, &len, "");

    // --- uniforms -----------------------------------------------------------
    // Must stay in sync with MVUniforms in gfx_metal.m, which static-asserts
    // its own size.
    append_line(out, &len, "struct Uniforms {");
    append_line(out, &len, "    float2 uTex0Size;");
    append_line(out, &len, "    float2 uTex1Size;");
    append_line(out, &len, "    float4 uLightmapColor;");
    append_line(out, &len, "    float uFrameCount;");
    append_line(out, &len, "    int uTex0Filter;");
    append_line(out, &len, "    int uTex1Filter;");
    append_line(out, &len, "    int uFilter;");
    len += sprintf(out + len, "    int uShaderFlags[%d];\n", SHADER_FLAG_MAX);
    len += sprintf(out + len, "    float uShaderFlagValues[%d];\n", SHADER_FLAG_MAX);
    append_line(out, &len, "    float4 uPortalEye;   // xyz eye in portal space, w: 0 off, 1 portal, 2 portal without frame");
    append_line(out, &len, "    float2 uPortalHalf;  // window half extents in metres");
    append_line(out, &len, "    float2 uPortalPad;");
    append_line(out, &len, "};");
    append_line(out, &len, "");
    // Vertex-stage uniforms, always bound at buffer(1). Identity outside the
    // portal, so window-mode shaders pay one matrix multiply and nothing else.
    append_line(out, &len, "struct VSUniforms {");
    append_line(out, &len, "    float4x4 uClipToPortal;");
    append_line(out, &len, "};");
    append_line(out, &len, "");

    // --- vertex stage I/O ---------------------------------------------------
    // Attribute indices follow the order gfx_pc packs the vertex buffer in,
    // which is the order gfx_metal_attrib_sizes reports.
    int attrib = 0;
    append_line(out, &len, "struct VSIn {");
    len += sprintf(out + len, "    float4 aVtxPos [[attribute(%d)]];\n", attrib++);
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            len += sprintf(out + len, "    float2 aTexCoord%d [[attribute(%d)]];\n", t, attrib++);
            num_floats += 2;
        }
    }
    if (opt_fog) {
        len += sprintf(out + len, "    float4 aFog [[attribute(%d)]];\n", attrib++);
        num_floats += 4;
    }
    if (opt_light_map) {
        len += sprintf(out + len, "    float2 aLightMap [[attribute(%d)]];\n", attrib++);
        num_floats += 2;
    }
    for (int i = 0; i < ccf->num_inputs; i++) {
        len += sprintf(out + len, "    float%d aInput%d [[attribute(%d)]];\n", input_width, i + 1, attrib++);
        num_floats += input_width;
    }
    append_line(out, &len, "};");
    append_line(out, &len, "");

    append_line(out, &len, "struct VSOut {");
    append_line(out, &len, "    float4 position [[position]];");
    append_line(out, &len, "    float4 vPortalPos;");
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            len += sprintf(out + len, "    float2 vTexCoord%d;\n", t);
        }
    }
    if (opt_fog)       { append_line(out, &len, "    float4 vFog;"); }
    if (opt_light_map) { append_line(out, &len, "    float2 vLightMap;"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        len += sprintf(out + len, "    float%d vInput%d;\n", input_width, i + 1);
    }
    append_line(out, &len, "};");
    append_line(out, &len, "");

    // --- vertex function ----------------------------------------------------
    append_line(out, &len, "vertex VSOut vertexMain(VSIn in [[stage_in]],");
    append_line(out, &len, "                        constant VSUniforms& vu [[buffer(1)]]) {");
    append_line(out, &len, "    VSOut out;");
    append_line(out, &len, "    out.position = in.aVtxPos;");
    append_line(out, &len, "    out.vPortalPos = vu.uClipToPortal * in.aVtxPos;");
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            len += sprintf(out + len, "    out.vTexCoord%d = in.aTexCoord%d;\n", t, t);
        }
    }
    if (opt_fog)       { append_line(out, &len, "    out.vFog = in.aFog;"); }
    if (opt_light_map) { append_line(out, &len, "    out.vLightMap = in.aLightMap;"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        len += sprintf(out + len, "    out.vInput%d = in.aInput%d;\n", i + 1, i + 1);
    }
    append_line(out, &len, "    return out;");
    append_line(out, &len, "}");
    append_line(out, &len, "");

    // --- fragment helpers ---------------------------------------------------
    if (ccf->used_textures[0] || ccf->used_textures[1]) {
        // 3 point texture filtering
        // Original author: ArthurCarvalho
        // GLSL implementation by twinaphex, mupen64plus-libretro project.
        append_line(out, &len, "#define TEX_OFFSET(off) tex.sample(smp, texCoord - (off)/texSize)");
        append_line(out, &len, "static float4 filter3point(texture2d<float> tex, sampler smp, float2 texCoord, float2 texSize) {");
        append_line(out, &len, "    float2 offset = fract(texCoord*texSize - float2(0.5));");
        append_line(out, &len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(out, &len, "    float4 c0 = TEX_OFFSET(offset);");
        append_line(out, &len, "    float4 c1 = TEX_OFFSET(float2(offset.x - sign(offset.x), offset.y));");
        append_line(out, &len, "    float4 c2 = TEX_OFFSET(float2(offset.x, offset.y - sign(offset.y)));");
        append_line(out, &len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(out, &len, "}");
        append_line(out, &len, "static float4 sampleTex(texture2d<float> tex, sampler smp, float2 uv, float2 texSize, bool dofilter, int filt) {");
        append_line(out, &len, "    if (dofilter && filt == 2) {");
        append_line(out, &len, "        return filter3point(tex, smp, uv, texSize);");
        append_line(out, &len, "    }");
        append_line(out, &len, "    return tex.sample(smp, uv);");
        append_line(out, &len, "}");
    }

    if (world_geometry) {
        // The GLSL version walks a chain of ifs to pick the threshold; a
        // constant array is the same table and reads better in MSL.
        append_line(out, &len, "static float dither4x4(float2 position, float brightness) {");
        append_line(out, &len, "    int x = int(fmod(position.x, 4.0));");
        append_line(out, &len, "    int y = int(fmod(position.y, 4.0));");
        append_line(out, &len, "    int index = x + y * 4;");
        append_line(out, &len, "    const float limits[16] = {");
        append_line(out, &len, "        0.0625, 0.5625, 0.1875, 0.6875,");
        append_line(out, &len, "        0.8125, 0.3125, 0.9375, 0.4375,");
        append_line(out, &len, "        0.25,   0.75,   0.125,  0.625,");
        append_line(out, &len, "        1.0,    0.5,    0.875,  0.375 };");
        append_line(out, &len, "    float limit = limits[clamp(index, 0, 15)];");
        append_line(out, &len, "    return brightness < limit ? 0.0 : 1.0;");
        append_line(out, &len, "}");

        append_line(out, &len, "static float3 rgb2hsv(float3 c) {");
        append_line(out, &len, "    float4 K = float4(0.0, -1.0/3.0, 2.0/3.0, -1.0);");
        append_line(out, &len, "    float4 p = mix(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));");
        append_line(out, &len, "    float4 q = mix(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));");
        append_line(out, &len, "    float d = q.x - min(q.w, q.y);");
        append_line(out, &len, "    float e = 1.0e-10;");
        append_line(out, &len, "    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);");
        append_line(out, &len, "}");
        append_line(out, &len, "static float3 hsv2rgb(float3 c) {");
        append_line(out, &len, "    float3 p = abs(fract(c.xxx + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);");
        append_line(out, &len, "    return c.z * mix(float3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);");
        append_line(out, &len, "}");
    }

    if ((opt_alpha && opt_dither) || ccf->do_noise) {
        append_line(out, &len, "static float randomValue(float3 value) {");
        append_line(out, &len, "    float r = dot(sin(value), float3(12.9898, 78.233, 37.719));");
        append_line(out, &len, "    return fract(sin(r) * 143758.5453);");
        append_line(out, &len, "}");
    }

    // --- fragment function --------------------------------------------------
    append_line(out, &len, "fragment float4 fragmentMain(VSOut in [[stage_in]],");
    append_line(out, &len, "                             constant Uniforms& u [[buffer(0)]]");
    for (int t = 0; t < 2; t++) {
        if (ccf->used_textures[t]) {
            len += sprintf(out + len, "                             , texture2d<float> uTex%d [[texture(%d)]]\n", t, t);
            len += sprintf(out + len, "                             , sampler uSmp%d [[sampler(%d)]]\n", t, t);
        }
    }
    append_line(out, &len, "                             ) {");

    for (int t = 0; t < 2; t++) {
        // With a light map, tile 1 is sampled at vLightMap and its own texture
        // coordinate is never read - though it stays in the vertex layout,
        // which gfx_pc packs regardless.
        if (t == 1 && opt_light_map) { continue; }
        if (ccf->used_textures[t]) {
            len += sprintf(out + len, "    float2 vTexCoord%d = in.vTexCoord%d;\n", t, t);
        }
    }
    if (opt_fog)       { append_line(out, &len, "    float4 vFog = in.vFog;"); }
    if (opt_light_map) { append_line(out, &len, "    float2 vLightMap = in.vLightMap;"); }
    for (int i = 0; i < ccf->num_inputs; i++) {
        len += sprintf(out + len, "    float%d vInput%d = in.vInput%d;\n", input_width, i + 1, i + 1);
    }
    // Only the noise, dither and scanline paths read the fragment position, and
    // declaring it unconditionally makes the Metal compiler warn on every other
    // shader.
    if ((opt_alpha && opt_dither) || ccf->do_noise || world_geometry) {
        append_line(out, &len, "    float2 fragCoord = in.position.xy;");
    }

    // Portal clip: a fragment is visible when the ray from the eye to it
    // crosses the window rectangle in the z=0 plane of portal space. HUD draws
    // sit a millimetre behind the glass, so the same test trims them at the
    // window edges without any per-draw flag.
    append_line(out, &len, "    if (u.uPortalEye.w > 0.5 && u.uPortalEye.w < 1.5) {");
    append_line(out, &len, "        float3 pp = in.vPortalPos.xyz / in.vPortalPos.w;");
    append_line(out, &len, "        if (pp.z >= 0.0) { discard_fragment(); }");
    append_line(out, &len, "        float t = u.uPortalEye.z / (u.uPortalEye.z - pp.z);");
    append_line(out, &len, "        float2 q = u.uPortalEye.xy + t * (pp.xy - u.uPortalEye.xy);");
    append_line(out, &len, "        if (abs(q.x) > u.uPortalHalf.x || abs(q.y) > u.uPortalHalf.y) { discard_fragment(); }");
    append_line(out, &len, "    }");

    if ((opt_alpha && opt_dither) || ccf->do_noise) {
        append_line(out, &len, "    float noise = floor(randomValue(floor(float3(fragCoord, u.uFrameCount))) + 0.5);");
    }

    if (ccf->used_textures[0]) {
        append_line(out, &len, "    float4 texVal0 = sampleTex(uTex0, uSmp0, vTexCoord0, u.uTex0Size, u.uTex0Filter != 0, u.uFilter);");
    }
    if (ccf->used_textures[1]) {
        if (opt_light_map) {
            append_line(out, &len, "    float4 texVal1 = sampleTex(uTex1, uSmp1, vLightMap, u.uTex1Size, u.uTex1Filter != 0, u.uFilter);");
            append_line(out, &len, "    texVal0.rgb *= u.uLightmapColor.rgb;");
            append_line(out, &len, "    texVal1.rgb = texVal1.rgb * texVal1.rgb + texVal1.rgb;");
        } else {
            append_line(out, &len, "    float4 texVal1 = sampleTex(uTex1, uSmp1, vTexCoord1, u.uTex1Size, u.uTex1Filter != 0, u.uFilter);");
        }
    }

    append_str(out, &len, opt_alpha ? "    float4 texel = " : "    float3 texel = ");
    for (int i = 0; i < (opt_2cycle + 1); i++) {
        uint8_t *cmd = &cc->shader_commands[i * 8];
        if (!ccf->color_alpha_same[i] && opt_alpha) {
            append_str(out, &len, "float4(");
            append_formula(out, &len, cmd, ccf->do_single[i*2+0], ccf->do_multiply[i*2+0], ccf->do_mix[i*2+0], false, false, true);
            append_str(out, &len, ", ");
            append_formula(out, &len, cmd, ccf->do_single[i*2+1], ccf->do_multiply[i*2+1], ccf->do_mix[i*2+1], true, true, true);
            append_str(out, &len, ")");
        } else {
            append_formula(out, &len, cmd, ccf->do_single[i*2+0], ccf->do_multiply[i*2+0], ccf->do_mix[i*2+0], opt_alpha, false, opt_alpha);
        }
        append_line(out, &len, ";");

        if (i == 0 && opt_2cycle) {
            append_str(out, &len, "    texel = ");
        }
    }

    if (opt_texture_edge && opt_alpha) {
        append_line(out, &len, "    if (texel.a > 0.3) { texel.a = 1.0; } else { discard_fragment(); }");
    }

    if (world_geometry) {
        append_line(out, &len, "    if (u.uShaderFlags[0] == 1) {"); // hue
        append_line(out, &len, "        float3 hsv = rgb2hsv(texel.rgb);");
        append_line(out, &len, "        hsv.x = fract(hsv.x + u.uShaderFlagValues[0]);");
        append_line(out, &len, "        texel.rgb = hsv2rgb(hsv);");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[1] == 1) {"); // saturation
        append_line(out, &len, "        const float3 w = float3(0.2125, 0.7154, 0.0721);");
        append_line(out, &len, "        float3 intensity = float3(dot(texel.rgb, w));");
        append_line(out, &len, "        texel.rgb = mix(intensity, texel.rgb, u.uShaderFlagValues[1]);");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[2] == 1) {"); // brightness
        append_line(out, &len, "        texel.rgb *= u.uShaderFlagValues[2];");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[3] == 1) {"); // contrast
        append_line(out, &len, "        texel.rgb = 0.5 + u.uShaderFlagValues[3] * (texel.rgb - 0.5);");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[4] == 1) {"); // exposure
        append_line(out, &len, "        texel.rgb = texel.rgb + (u.uShaderFlagValues[4] - 2) * texel.rgb + texel.rgb;");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[5] == 1) {"); // dithering
        append_line(out, &len, "        texel.rgb *= dither4x4(fragCoord, dot(texel.rgb, float3(0.299, 0.587, 0.114)));");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[6] == 1) {"); // posterization
        append_line(out, &len, "        int levels = int(max(1.0, u.uShaderFlagValues[6]));");
        append_line(out, &len, "        texel.rgb = floor(texel.rgb * levels) / levels;");
        append_line(out, &len, "    }");

        append_line(out, &len, "    if (u.uShaderFlags[7] == 1) {"); // scan lines
        append_line(out, &len, "        float scan = sin(fragCoord.y * 1.5) * 0.04;");
        append_line(out, &len, "        texel.rgb -= scan * u.uShaderFlagValues[7];");
        append_line(out, &len, "    }");
    }

    if (opt_fog) {
        if (opt_alpha) {
            append_line(out, &len, "    texel = float4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(out, &len, "    texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (opt_alpha && opt_dither) {
        append_line(out, &len, "    texel.a *= noise;");
    }

    // The compositor's render target is sRGB-encoded and treats shader output
    // as linear light; the game's colours are display-referred. Linearising
    // here makes the store/decode round-trip come out as the original colour.
    append_line(out, &len, "    if (u.uPortalEye.w > 0.5) {");
    if (opt_alpha) {
        append_line(out, &len, "        texel.rgb = pow(max(texel.rgb, 0.0), 2.2);");
    } else {
        append_line(out, &len, "        texel = pow(max(texel, 0.0), 2.2);");
    }
    append_line(out, &len, "    }");

    if (opt_alpha) {
        append_line(out, &len, "    return texel;");
    } else {
        append_line(out, &len, "    return float4(texel, 1.0);");
    }
    append_line(out, &len, "}");

    if (len >= outSize) { return false; }

    out[len] = '\0';
    *numFloatsOut = num_floats;
    return true;
}
