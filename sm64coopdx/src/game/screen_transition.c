#include <PR/ultratypes.h>
#include <PR/gbi.h>

#include "area.h"
#include "camera.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "game/game_init.h"
#include "geo_misc.h"
#include "gfx_dimensions.h"
#include "memory.h"
#include "screen_transition.h"
#include "segment2.h"
#include "sm64.h"
#include "pc/utils/misc.h"
#include "hud.h"
#ifdef TARGET_VISIONOS
extern float mariovision_hud_overscan(void);
extern void mariovision_set_backdrop(unsigned char r, unsigned char g,
                                     unsigned char b, unsigned char a);
#endif

u8 sTransitionColorFadeCount[4] = { 0 };
u16 sTransitionTextureFadeCount[2] = { 0 };

static Gfx *sScreenTransitionVerticesPos[2];
static Vtx *sScreenTransitionVertices;
static Vtx *sScreenTransitionVerticesPrev;
static Vtx sScreenTransitionInterp[8] = { 0 };

void reset_screen_transition_timers(void) {
    for (s32 i = 0; i < 4; i++) { sTransitionColorFadeCount[i] = 0; }
    for (s32 i = 0; i < 2; i++) { sTransitionTextureFadeCount[i] = 0; }
}


extern void patch_screen_transition_before(void) {
    sScreenTransitionVerticesPos[0] = NULL;
    sScreenTransitionVerticesPos[1] = NULL;
    sScreenTransitionVertices = NULL;
    sScreenTransitionVerticesPrev = NULL;
}

void patch_screen_transition_interpolated(f32 delta) {
    if (sScreenTransitionVerticesPos[0] != NULL) {
        for (s32 i = 0; i < 8; i++) {
            sScreenTransitionInterp[i] = sScreenTransitionVerticesPrev[i];
            delta_interpolate_vec3f(sScreenTransitionInterp[i].n.ob,
                                    sScreenTransitionVerticesPrev[i].n.ob,
                                    sScreenTransitionVertices[i].n.ob, delta);
            delta_interpolate_rgba(sScreenTransitionInterp[i].v.cn,
                                   sScreenTransitionVerticesPrev[i].v.cn,
                                   sScreenTransitionVertices[i].v.cn, delta);
        }
        gSPVertexNonGlobal(sScreenTransitionVerticesPos[0], VIRTUAL_TO_PHYSICAL(sScreenTransitionInterp), 8, 0);
        gSPVertexNonGlobal(sScreenTransitionVerticesPos[1], VIRTUAL_TO_PHYSICAL(sScreenTransitionInterp), 4, 0);
    }
}

s32 set_and_reset_transition_fade_timer(s8 fadeTimer, u8 transTime) {
    s32 reset = FALSE;

    sTransitionColorFadeCount[fadeTimer]++;

    if (sTransitionColorFadeCount[fadeTimer] == transTime) {
        sTransitionColorFadeCount[fadeTimer] = 0;
        sTransitionTextureFadeCount[fadeTimer] = 0;
        reset = TRUE;
    }
    return reset;
}

u8 set_transition_color_fade_alpha(s8 fadeType, s8 fadeTimer, u8 transTime) {
    u8 time;

    switch (fadeType) {
        case 0:
            time = (f32) sTransitionColorFadeCount[fadeTimer] * 255.0 / (f32)(transTime - 1) + 0.5; // fade in
            break;
        case 1:
            time = (1.0 - sTransitionColorFadeCount[fadeTimer] / (f32)(transTime - 1)) * 255.0 + 0.5; // fade out
            break;
    }
    return time;
}

Vtx *vertex_transition_color(struct WarpTransitionData *transData, u8 alpha) {
    Vtx *verts = alloc_display_list(4 * sizeof(*verts));
    u8 r = transData->red;
    u8 g = transData->green;
    u8 b = transData->blue;

    if (verts != NULL) {
#ifdef TARGET_VISIONOS
        // And the same colour behind everything, in every direction - see
        // mariovision_backdrop. The quad below still does the work inside the
        // picture; this is what carries it past the edge of it.
        mariovision_set_backdrop(r, g, b, alpha);

        // Well past the pane - see mariovision_hud_overscan. A fade that stops
        // at the edge of the picture leaves the room visible around it, which
        // rather defeats a fade.
        {
            const f32 k = mariovision_hud_overscan();
            const f32 l = 160.0f - (160.0f - (f32) GFX_DIMENSIONS_FROM_LEFT_EDGE(0)) * k;
            const f32 rr = 160.0f + ((f32) GFX_DIMENSIONS_FROM_RIGHT_EDGE(0) - 160.0f) * k;
            const f32 t = 120.0f - 120.0f * k;
            const f32 bo = 120.0f + 120.0f * k;
            make_vertex(verts, 0, (s16) l,  (s16) t,  -1, 0, 0, r, g, b, alpha);
            make_vertex(verts, 1, (s16) rr, (s16) t,  -1, 0, 0, r, g, b, alpha);
            make_vertex(verts, 2, (s16) rr, (s16) bo, -1, 0, 0, r, g, b, alpha);
            make_vertex(verts, 3, (s16) l,  (s16) bo, -1, 0, 0, r, g, b, alpha);
        }
#else
        make_vertex(verts, 0, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 0, -1, 0, 0, r, g, b, alpha);
        make_vertex(verts, 1, GFX_DIMENSIONS_FROM_RIGHT_EDGE(0), 0, -1, 0, 0, r, g, b, alpha);
        make_vertex(verts, 2, GFX_DIMENSIONS_FROM_RIGHT_EDGE(0), SCREEN_HEIGHT, -1, 0, 0, r, g, b, alpha);
        make_vertex(verts, 3, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), SCREEN_HEIGHT, -1, 0, 0, r, g, b, alpha);
#endif
    } else {
    }
    return verts;
}

s32 dl_transition_color(s8 fadeTimer, u8 transTime, struct WarpTransitionData *transData, u8 alpha) {
    Vtx *verts = vertex_transition_color(transData, alpha);

    if (verts != NULL) {
        gSPDisplayList(gDisplayListHead++, dl_proj_mtx_fullscreen);
        gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        gSPVertexNonGlobal(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
        gSPDisplayList(gDisplayListHead++, dl_draw_quad_verts_0123);
        gSPDisplayList(gDisplayListHead++, dl_screen_transition_end);
    }
    return set_and_reset_transition_fade_timer(fadeTimer, transTime);
}

s32 render_fade_transition_from_color(s8 fadeTimer, u8 transTime, struct WarpTransitionData *transData) {
    u8 alpha = set_transition_color_fade_alpha(1, fadeTimer, transTime);

    return dl_transition_color(fadeTimer, transTime, transData, alpha);
}

s32 render_fade_transition_into_color(s8 fadeTimer, u8 transTime, struct WarpTransitionData *transData) {
    u8 alpha = set_transition_color_fade_alpha(0, fadeTimer, transTime);

    return dl_transition_color(fadeTimer, transTime, transData, alpha);
}

#if 0

s16 calc_tex_transition_radius(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData) {
    f32 texRadius = transData->endTexRadius - transData->startTexRadius;
    f32 radiusTime = sTransitionColorFadeCount[fadeTimer] * texRadius / (f32)(transTime - 1);
    f32 result = transData->startTexRadius + radiusTime;

    return (s16)(result + 0.5);
}

#else

s16 calc_tex_transition_radius(s8 fadeTimer, f32 interpolationFraction, s8 transTime, struct WarpTransitionData *transData) {
    f32 texRadius = transData->endTexRadius - transData->startTexRadius;
    f32 radiusTime = (sTransitionColorFadeCount[fadeTimer] == 0 ? 0 :
                      sTransitionColorFadeCount[fadeTimer] - 1 + interpolationFraction) * texRadius / (f32)(transTime - 1);
    f32 result = transData->startTexRadius + radiusTime;

    return (s16)(result + 0.5);
}

#endif

f32 calc_tex_transition_time(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData) {
    f32 startX = transData->startTexX;
    f32 startY = transData->startTexY;
    f32 endX = transData->endTexX;
    f32 endY = transData->endTexY;
    f32 sqrtfXY = sqrtf((startX - endX) * (startX - endX) + (startY - endY) * (startY - endY));
    f32 result = (f32) sTransitionColorFadeCount[fadeTimer] * sqrtfXY / (f32)(transTime - 1);

    return result;
}

u16 convert_tex_transition_angle_to_pos(struct WarpTransitionData *transData) {
    f32 x = transData->endTexX - transData->startTexX;
    f32 y = transData->endTexY - transData->startTexY;

    return atan2s(x, y);
}

s16 center_tex_transition_x(struct WarpTransitionData *transData, f32 texTransTime, u16 texTransPos) {
    f32 x = transData->startTexX + coss(texTransPos) * texTransTime;

    return (s16)(x + 0.5);
}

s16 center_tex_transition_y(struct WarpTransitionData *transData, f32 texTransTime, u16 texTransPos) {
    f32 y = transData->startTexY + sins(texTransPos) * texTransTime;

    return (s16)(y + 0.5);
}

void make_tex_transition_vertex(Vtx *verts, s32 n, s8 fadeTimer, struct WarpTransitionData *transData, s16 centerTransX, s16 centerTransY,
                   s16 texRadius1, s16 texRadius2, s16 tx, s16 ty) {
    u8 r = transData->red;
    u8 g = transData->green;
    u8 b = transData->blue;
    u16 zeroTimer = sTransitionTextureFadeCount[fadeTimer];
    f32 centerX = texRadius1 * coss(zeroTimer) - texRadius2 * sins(zeroTimer) + centerTransX;
    f32 centerY = texRadius1 * sins(zeroTimer) + texRadius2 * coss(zeroTimer) + centerTransY;
    s16 x = round_float(centerX);
    s16 y = round_float(centerY);

    make_vertex(verts, n, x, y, -1, tx * 32, ty * 32, r, g, b, 255);
}

void load_tex_transition_vertex(Vtx *verts, s8 fadeTimer, struct WarpTransitionData *transData, s16 centerTransX, s16 centerTransY,
                   s16 texTransRadius, s8 transTexType) {
    switch (transTexType) {
        case TRANS_TYPE_MIRROR:
            make_tex_transition_vertex(verts, 0, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, -texTransRadius, -31, 63);
            make_tex_transition_vertex(verts, 1, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, -texTransRadius, 31, 63);
            make_tex_transition_vertex(verts, 2, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, texTransRadius, 31, 0);
            make_tex_transition_vertex(verts, 3, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, texTransRadius, -31, 0);
            break;
        case TRANS_TYPE_CLAMP:
            make_tex_transition_vertex(verts, 0, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, -texTransRadius, 0, 63);
            make_tex_transition_vertex(verts, 1, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, -texTransRadius, 63, 63);
            make_tex_transition_vertex(verts, 2, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, texTransRadius, 63, 0);
            make_tex_transition_vertex(verts, 3, fadeTimer, transData, centerTransX, centerTransY, -texTransRadius, texTransRadius, 0, 0);
            break;
    }
    make_tex_transition_vertex(verts, 4, fadeTimer, transData, centerTransX, centerTransY, -2000, -2000, 0, 0);
    make_tex_transition_vertex(verts, 5, fadeTimer, transData, centerTransX, centerTransY, 2000, -2000, 0, 0);
    make_tex_transition_vertex(verts, 6, fadeTimer, transData, centerTransX, centerTransY, 2000, 2000, 0, 0);
    make_tex_transition_vertex(verts, 7, fadeTimer, transData, centerTransX, centerTransY, -2000, 2000, 0, 0);
}

void *sTextureTransitionID[] = {
    texture_transition_star_half,
    texture_transition_circle_half,
    texture_transition_mario,
    texture_transition_bowser_half,
};

#if 0

s32 render_textured_transition(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData, s8 texID, s8 transTexType) {
    f32 texTransTime = calc_tex_transition_time(fadeTimer, transTime, transData);
    u16 texTransPos = convert_tex_transition_angle_to_pos(transData);
    s16 centerTransX = center_tex_transition_x(transData, texTransTime, texTransPos);
    s16 centerTransY = center_tex_transition_y(transData, texTransTime, texTransPos);
    s16 texTransRadius = calc_tex_transition_radius(fadeTimer, transTime, transData);
    Vtx *verts = alloc_display_list(8 * sizeof(*verts));

    if (verts != NULL) {
        load_tex_transition_vertex(verts, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, transTexType);
        gSPDisplayList(gDisplayListHead++, dl_proj_mtx_fullscreen)
        gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
        gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 8, 0);
        gSPDisplayList(gDisplayListHead++, dl_transition_draw_filled_region);
        gDPPipeSync(gDisplayListHead++);
        gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        gDPSetTextureFilter(gDisplayListHead++, G_TF_BILERP);
        switch (transTexType) {
        case TRANS_TYPE_MIRROR:
            gDPLoadTextureBlock(gDisplayListHead++, sTextureTransitionID[texID], G_IM_FMT_IA, G_IM_SIZ_8b, 32, 64, 0,
                G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
            break;
        case TRANS_TYPE_CLAMP:
            gDPLoadTextureBlock(gDisplayListHead++, sTextureTransitionID[texID], G_IM_FMT_IA, G_IM_SIZ_8b, 64, 64, 0,
                G_TX_CLAMP, G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);
            break;
        }
        gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
        gSPDisplayList(gDisplayListHead++, dl_draw_quad_verts_0123);
        gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
        gSPDisplayList(gDisplayListHead++, dl_screen_transition_end);
        sTransitionTextureFadeCount[fadeTimer] += transData->texTimer;
    } else {
    }
    return set_and_reset_transition_fade_timer(fadeTimer, transTime);
}

#else

s32 render_textured_transition(s8 fadeTimer, s8 transTime, struct WarpTransitionData *transData, s8 texID, s8 transTexType) {
    f32 texTransTime = calc_tex_transition_time(fadeTimer, transTime, transData);
    u16 texTransPos = convert_tex_transition_angle_to_pos(transData);
    s16 centerTransX = center_tex_transition_x(transData, texTransTime, texTransPos);
    s16 centerTransY = center_tex_transition_y(transData, texTransTime, texTransPos);
    s16 texTransRadius = calc_tex_transition_radius(fadeTimer, 1.0f, transTime, transData);
    s16 texTransRadiusPrev = calc_tex_transition_radius(fadeTimer, 0.0f, transTime, transData);
    Vtx *verts = alloc_display_list(8 * sizeof(*verts));
    Vtx *vertsPrev = alloc_display_list(8 * sizeof(*vertsPrev));

    if (verts != NULL && vertsPrev != NULL) {
        load_tex_transition_vertex(verts, fadeTimer, transData, centerTransX, centerTransY, texTransRadius, transTexType);
        load_tex_transition_vertex(vertsPrev, fadeTimer, transData, centerTransX, centerTransY, texTransRadiusPrev, transTexType);
        sScreenTransitionVertices = verts;
        sScreenTransitionVerticesPrev = vertsPrev;
        gSPDisplayList(gDisplayListHead++, dl_proj_mtx_fullscreen)
        gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_OPA_SURF, G_RM_AA_OPA_SURF2);
        sScreenTransitionVerticesPos[0] = gDisplayListHead;
        gSPVertexNonGlobal(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(vertsPrev), 8, 0);
        gSPDisplayList(gDisplayListHead++, dl_transition_draw_filled_region);
        gDPPipeSync(gDisplayListHead++);
        gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
        gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);
        gDPSetTextureFilter(gDisplayListHead++, G_TF_BILERP);
        switch (transTexType) {
        case TRANS_TYPE_MIRROR:
            gDPLoadTextureBlock(gDisplayListHead++, sTextureTransitionID[texID], G_IM_FMT_IA, G_IM_SIZ_8b, 32, 64, 0,
                G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
            break;
        case TRANS_TYPE_CLAMP:
            gDPLoadTextureBlock(gDisplayListHead++, sTextureTransitionID[texID], G_IM_FMT_IA, G_IM_SIZ_8b, 64, 64, 0,
                G_TX_CLAMP, G_TX_CLAMP, 6, 6, G_TX_NOLOD, G_TX_NOLOD);
            break;
        }
        gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        sScreenTransitionVerticesPos[1] = gDisplayListHead;
        gSPVertexNonGlobal(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(vertsPrev), 4, 0);
        gSPDisplayList(gDisplayListHead++, dl_draw_quad_verts_0123);
        gSPTexture(gDisplayListHead++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
        gSPDisplayList(gDisplayListHead++, dl_screen_transition_end);
        sTransitionTextureFadeCount[fadeTimer] += transData->texTimer;
    } else {
    }
    return set_and_reset_transition_fade_timer(fadeTimer, transTime);
}

#endif

s32 render_screen_transition(s8 fadeTimer, s8 transType, u8 transTime, struct WarpTransitionData *transData) {
    switch (transType) {
        case WARP_TRANSITION_FADE_FROM_COLOR:
            return render_fade_transition_from_color(fadeTimer, transTime, transData);
            break;
        case WARP_TRANSITION_FADE_INTO_COLOR:
            return render_fade_transition_into_color(fadeTimer, transTime, transData);
            break;
        case WARP_TRANSITION_FADE_FROM_STAR:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_STAR, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_INTO_STAR:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_STAR, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_FROM_CIRCLE:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_CIRCLE, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_INTO_CIRCLE:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_CIRCLE, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_FROM_MARIO:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_MARIO, TRANS_TYPE_CLAMP);
            break;
        case WARP_TRANSITION_FADE_INTO_MARIO:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_MARIO, TRANS_TYPE_CLAMP);
            break;
        case WARP_TRANSITION_FADE_FROM_BOWSER:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_BOWSER, TRANS_TYPE_MIRROR);
            break;
        case WARP_TRANSITION_FADE_INTO_BOWSER:
            return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_BOWSER, TRANS_TYPE_MIRROR);
            break;
    }
    return render_textured_transition(fadeTimer, transTime, transData, TEX_TRANS_BOWSER, TRANS_TYPE_MIRROR);
}

Gfx *render_cannon_circle_base(void) {
    Vtx *verts = alloc_display_list(8 * sizeof(*verts));
    Gfx *dlist = alloc_display_list(24 * sizeof(*dlist));
    Gfx *g = dlist;

    if (verts != NULL && dlist != NULL) {
        // The opening, at the size it has always been. The black around it is
        // one quad carrying a circle that mirrors as it wraps - stretch that
        // quad to cover a headset's view and the mirroring obliges by tiling
        // the circle into a colander, which is what the first attempt at this
        // produced. So the textured quad stays exactly where it was, and the
        // reach is made up of plain black around it.
        make_vertex(verts, 0, 0, 0, -1, -1152, 1824, 0, 0, 0, 255);
        make_vertex(verts, 1, SCREEN_WIDTH, 0, -1, 1152, 1824, 0, 0, 0, 255);
        make_vertex(verts, 2, SCREEN_WIDTH, SCREEN_HEIGHT, -1, 1152, 192, 0, 0, 0, 255);
        make_vertex(verts, 3, 0, SCREEN_HEIGHT, -1, -1152, 192, 0, 0, 0, 255);

        // The frame around it. On a television this is the strip either side of
        // the 4:3 picture; in a headset it has to run out past the edge of
        // sight, which is what the overscan is.
        f32 k = 1.0f;
#ifdef TARGET_VISIONOS
        k = mariovision_hud_overscan();
#endif
        const f32 leftEdge  = (f32) GFX_DIMENSIONS_FROM_LEFT_EDGE(0);
        const f32 rightEdge = (f32) GFX_DIMENSIONS_FROM_RIGHT_EDGE(0);
        const s16 l = (s16) (160.0f - (160.0f - leftEdge) * k);
        const s16 r = (s16) (160.0f + (rightEdge - 160.0f) * k);
        const s16 t = (s16) (120.0f - 120.0f * k);
        const s16 b = (s16) (120.0f + 120.0f * k);

        make_vertex(verts, 4, l, t, -1, 0, 0, 0, 0, 0, 255);
        make_vertex(verts, 5, r, t, -1, 0, 0, 0, 0, 0, 255);
        make_vertex(verts, 6, r, b, -1, 0, 0, 0, 0, 0, 255);
        make_vertex(verts, 7, l, b, -1, 0, 0, 0, 0, 0, 255);

#ifdef TARGET_VISIONOS
        // Black all the way round, so that turning one's head finds the inside
        // of the barrel rather than the hillside.
        mariovision_set_backdrop(0, 0, 0, 255);
#endif

        gSPDisplayList(g++, dl_proj_mtx_fullscreen);
        gDPSetCombineMode(g++, G_CC_MODULATEIDECALA, G_CC_MODULATEIDECALA);
        gDPSetTextureFilter(g++, G_TF_BILERP);
        gDPLoadTextureBlock(g++, sTextureTransitionID[TEX_TRANS_CIRCLE], G_IM_FMT_IA, G_IM_SIZ_8b, 32, 64, 0,
            G_TX_WRAP | G_TX_MIRROR, G_TX_WRAP | G_TX_MIRROR, 5, 6, G_TX_NOLOD, G_TX_NOLOD);
        gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
        gSPVertexNonGlobal(g++, VIRTUAL_TO_PHYSICAL(verts), 8, 0);
        gSPDisplayList(g++, dl_draw_quad_verts_0123);
        gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);

        gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);
        // Four strips, each between an outer corner and the one inside it.
        gSP2Triangles(g++, 4, 0, 3, 0, 4, 3, 7, 0);   // left
        gSP2Triangles(g++, 1, 5, 6, 0, 1, 6, 2, 0);   // right
        gSP2Triangles(g++, 4, 5, 1, 0, 4, 1, 0, 0);   // top
        gSP2Triangles(g++, 7, 3, 2, 0, 7, 2, 6, 0);   // bottom

        gSPDisplayList(g++, dl_screen_transition_end);
        gSPEndDisplayList(g);
    } else {
        return NULL;
    }
    return dlist;
}


Gfx *geo_cannon_circle_base(s32 callContext, struct GraphNode *node, UNUSED Mat4 mtx) {
    struct GraphNodeGenerated *graphNode = (struct GraphNodeGenerated *) node;
    Gfx *dlist = NULL;
    if (callContext == GEO_CONTEXT_RENDER && gCurrentArea != NULL
        && gCurrentArea->camera->mode == CAMERA_MODE_INSIDE_CANNON) {
        graphNode->fnNode.node.flags = (graphNode->fnNode.node.flags & 0xFF) | 0x500;
        dlist = render_cannon_circle_base();
    }
    return dlist;
}
