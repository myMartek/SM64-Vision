/*
 * Generates the Metal shaders for every colour combiner Super Mario 64 actually
 * uses and writes them out as .metal files, so that scripts/test-msl-shaders.sh
 * can push each one through the offline Metal compiler.
 *
 * Without this, a syntax error in the generated MSL would only show up as a
 * fatal error on the headset, at the moment the game first draws with that
 * particular combiner - which could be several levels in.
 *
 * The corpus is gfx_cc_precomp()'s list from src/pc/gfx/gfx_cc.c, which is the
 * set of combiners the vanilla game compiles up front, plus synthetic cases for
 * the coopdx-specific flags (light map, world geometry) that the vanilla list
 * never exercises.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/gfx/gfx_cc.h"
#include "pc/visionos/gfx_metal_shader.h"

// Combine mode flag bits, matching struct CombineMode's bitfield order.
#define CM_ALPHA        (1 << 0)
#define CM_FOG          (1 << 1)
#define CM_TEXTURE_EDGE (1 << 2)
#define CM_DITHER       (1 << 3)
#define CM_2CYCLE       (1 << 4)
#define CM_LIGHT_MAP    (1 << 5)
#define CM_WORLD_GEO    (1 << 6)

struct TestCase {
    const char *name;
    uint32_t rgb1, alpha1, rgb2, alpha2, flags;
};

/*
 * gfx_cc.c is linked in for gfx_cc_get_features, but it also carries
 * gfx_cc_precomp(), whose body calls into gfx_pc.c. Pulling in gfx_pc.c would
 * drag the whole renderer along, so the one symbol it needs is stubbed instead.
 * Nothing here ever calls it.
 */
void gfx_pc_precomp_shader(uint32_t rgb1, uint32_t alpha1, uint32_t rgb2, uint32_t alpha2, uint32_t flags);
void gfx_pc_precomp_shader(uint32_t rgb1, uint32_t alpha1, uint32_t rgb2, uint32_t alpha2, uint32_t flags) {
    (void) rgb1; (void) alpha1; (void) rgb2; (void) alpha2; (void) flags;
}

// From gfx_cc_precomp() - the combiners vanilla SM64 precompiles.
static const struct TestCase sVanilla[] = {
    { "vanilla00", 0x00030001, 0x02000000, 0x000a0004, 0x0a000b0b, 0x00000011 },
    { "vanilla01", 0x00040001, 0x00010005, 0x00040002, 0x0b020b05, 0x00000001 },
    { "vanilla02", 0x00040001, 0x00030001, 0x00040002, 0x0b030b02, 0x00000001 },
    { "vanilla03", 0x00040001, 0x00040001, 0x00040002, 0x0b040b02, 0x00000000 },
    { "vanilla04", 0x00040001, 0x00040001, 0x00040002, 0x0b040b02, 0x00000001 },
    { "vanilla05", 0x00040001, 0x00040001, 0x00040002, 0x0b040b02, 0x00000005 },
    { "vanilla06", 0x00040001, 0x00050001, 0x00040002, 0x0b050b02, 0x00000001 },
    { "vanilla07", 0x00040001, 0x00050001, 0x00040002, 0x0b050b02, 0x00000009 },
    { "vanilla08", 0x00040001, 0x01000000, 0x00040002, 0x02000b0b, 0x00000001 },
    { "vanilla09", 0x00040001, 0x04000000, 0x00040002, 0x04000b0b, 0x00000001 },
    { "vanilla10", 0x00040001, 0x04000000, 0x0a000000, 0x0a000b0b, 0x00000013 },
    { "vanilla11", 0x00040001, 0x05000000, 0x00040002, 0x05000b0b, 0x00000001 },
    { "vanilla12", 0x00040001, 0x05000000, 0x00040002, 0x05000b0b, 0x00000009 },
    { "vanilla13", 0x00040001, 0x05000000, 0x0a000000, 0x0a000b0b, 0x00000013 },
    { "vanilla14", 0x00050001, 0x00050001, 0x00050002, 0x0b050b02, 0x00000001 },
    { "vanilla15", 0x00050004, 0x00050004, 0x00050004, 0x0b050b04, 0x00000001 },
    { "vanilla16", 0x01000000, 0x00050001, 0x02000000, 0x0b050b02, 0x00000001 },
    { "vanilla17", 0x01000000, 0x01000000, 0x02000000, 0x02000000, 0x00000001 },
    { "vanilla18", 0x01000000, 0x01000000, 0x02000000, 0x02000b0b, 0x00000005 },
    { "vanilla19", 0x01000000, 0x01000000, 0x0a000000, 0x0a000b0b, 0x00000017 },
    { "vanilla20", 0x01000000, 0x04000000, 0x02000000, 0x04000b0b, 0x00000000 },
    { "vanilla21", 0x01000000, 0x04000000, 0x0a000000, 0x0a000b0b, 0x00000013 },
    { "vanilla22", 0x01000000, 0x05000000, 0x02000000, 0x05000b0b, 0x00000001 },
    { "vanilla23", 0x01000000, 0x05000000, 0x0a000000, 0x0a000b0b, 0x00000013 },
    { "vanilla24", 0x01070102, 0x01000102, 0x0a000000, 0x04000b0b, 0x00000011 },
    { "vanilla25", 0x04000000, 0x04000000, 0x04000000, 0x04000000, 0x00000001 },
    { "vanilla26", 0x04000000, 0x04000000, 0x04000000, 0x04000b0b, 0x00000000 },
    { "vanilla27", 0x04000000, 0x04000000, 0x04000000, 0x04000b0b, 0x00000005 },
    { "vanilla28", 0x04000000, 0x04000000, 0x0a000000, 0x0a000b0b, 0x00000013 },
    { "vanilla29", 0x04000000, 0x05000000, 0x04000000, 0x05000b0b, 0x00000001 },
    { "vanilla30", 0x04000000, 0x05000000, 0x04000000, 0x05000b0b, 0x00000005 },
    { "vanilla31", 0x04000000, 0x05000000, 0x04000000, 0x05000b0b, 0x00000009 },
    { "vanilla32", 0x04060401, 0x04000000, 0x04060402, 0x04000b0b, 0x00000000 },
    { "vanilla33", 0x04060401, 0x05000000, 0x04060402, 0x05000b0b, 0x00000001 },
    { "vanilla34", 0x01000000, 0x04000000, 0x02000000, 0x04000b0b, 0x00000001 },
    { "vanilla35", 0x04060401, 0x05000000, 0x04060402, 0x05000b0b, 0x00000009 },
    { "vanilla36", 0x00040001, 0x00040001, 0x00040002, 0x0b040b02, 0x00000009 },
};

// The vanilla corpus never sets the light map or world geometry bits, and never
// combines dither with two cycles, so those paths get their own cases. The
// combiner words are borrowed from vanilla entries that use two textures
// (light map samples TEXEL1) or one (world geometry).
static const struct TestCase sSynthetic[] = {
    { "lightmap",           0x00040001, 0x00040001, 0x00040002, 0x0b040b02, CM_ALPHA | CM_2CYCLE | CM_LIGHT_MAP },
    { "worldgeo",           0x00040001, 0x00040001, 0x00040002, 0x0b040b02, CM_ALPHA | CM_WORLD_GEO },
    { "worldgeo_noalpha",   0x04000000, 0x04000000, 0x04000000, 0x04000000, CM_WORLD_GEO },
    { "worldgeo_fog",       0x00040001, 0x00040001, 0x00040002, 0x0b040b02, CM_ALPHA | CM_FOG | CM_WORLD_GEO },
    { "dither_2cycle",      0x00030001, 0x02000000, 0x000a0004, 0x0a000b0b, CM_ALPHA | CM_DITHER | CM_2CYCLE },
    { "everything",         0x00030001, 0x02000000, 0x000a0004, 0x0a000b0b,
                            CM_ALPHA | CM_FOG | CM_TEXTURE_EDGE | CM_DITHER | CM_2CYCLE | CM_LIGHT_MAP | CM_WORLD_GEO },
    { "noise",              0x0f000000, 0x0f000000, 0x0f000000, 0x0f000000, CM_ALPHA },
    { "noalpha_notex",      0x01000000, 0x01000000, 0x02000000, 0x02000000, 0 },
};

/*
 * Mirrors gfx_generate_cc() in src/pc/gfx/gfx_pc.c, which is static there. Only
 * the combine-mode-to-shader-command mapping matters for shader generation; the
 * hashing and program lookup it also does are irrelevant here.
 */
static void generate_cc(struct ColorCombiner *cc) {
    uint8_t next_input_number = 0;
    uint8_t input_number[CC_ENUM_MAX] = { 0 };

    for (int i = 0; i < SHADER_CMD_LENGTH; i++) {
        uint8_t cm_cmd = cc->cm.all_values[i];
        uint8_t shader_cmd;
        switch (cm_cmd) {
            case CC_0:         shader_cmd = SHADER_0; break;
            case CC_1:         shader_cmd = SHADER_1; break;
            case CC_TEXEL0:    shader_cmd = SHADER_TEXEL0; break;
            case CC_TEXEL1:    shader_cmd = SHADER_TEXEL1; break;
            case CC_TEXEL0A:   shader_cmd = SHADER_TEXEL0A; break;
            case CC_TEXEL1A:   shader_cmd = SHADER_TEXEL1A; break;
            case CC_COMBINED:  shader_cmd = cc->cm.use_2cycle ? SHADER_COMBINED : SHADER_0; break;
            case CC_COMBINEDA: shader_cmd = cc->cm.use_2cycle ? SHADER_COMBINEDA : SHADER_0; break;
            case CC_NOISE:     shader_cmd = SHADER_NOISE; break;
            case CC_PRIM: case CC_PRIMA:
            case CC_SHADE: case CC_SHADEA:
            case CC_ENV: case CC_ENVA:
            case CC_LOD:
                if (input_number[cm_cmd] == 0) {
                    cc->shader_input_mapping[next_input_number] = cm_cmd;
                    input_number[cm_cmd] = SHADER_INPUT_1 + next_input_number;
                    next_input_number++;
                }
                shader_cmd = input_number[cm_cmd];
                break;
            default: shader_cmd = SHADER_0; break;
        }
        cc->shader_commands[i] = shader_cmd;
    }
}

static int emit(const struct TestCase *tc, const char *outDir) {
    struct ColorCombiner cc;
    memset(&cc, 0, sizeof(cc));

    cc.cm.rgb1 = tc->rgb1;
    cc.cm.alpha1 = tc->alpha1;
    cc.cm.rgb2 = tc->rgb2;
    cc.cm.alpha2 = tc->alpha2;
    cc.cm.flags = tc->flags;

    generate_cc(&cc);

    struct CCFeatures ccf;
    gfx_cc_get_features(&cc, &ccf);

    static char msl[16384];
    size_t numFloats = 0;
    if (!gfx_metal_generate_shader(&cc, &ccf, msl, sizeof(msl), &numFloats)) {
        fprintf(stderr, "%s: shader exceeded the output buffer\n", tc->name);
        return 1;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.metal", outDir, tc->name);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "%s: cannot write %s\n", tc->name, path);
        return 1;
    }
    fputs(msl, f);
    fclose(f);

    printf("%-18s inputs=%d tex=%d%d floats=%zu\n",
           tc->name, ccf.num_inputs, ccf.used_textures[0], ccf.used_textures[1], numFloats);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }

    int failures = 0;
    for (size_t i = 0; i < sizeof(sVanilla) / sizeof(sVanilla[0]); i++) {
        failures += emit(&sVanilla[i], argv[1]);
    }
    for (size_t i = 0; i < sizeof(sSynthetic) / sizeof(sSynthetic[0]); i++) {
        failures += emit(&sSynthetic[i], argv[1]);
    }

    if (failures) {
        fprintf(stderr, "%d shader(s) failed to generate\n", failures);
        return 1;
    }
    return 0;
}
