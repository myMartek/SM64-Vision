#include <PR/ultratypes.h>

#include "area.h"
#include "engine/math_util.h"
#include "geo_misc.h"
#include "gfx_dimensions.h"
#include "level_update.h"
#include "memory.h"
#include "save_file.h"
#include "segment2.h"
#include "sm64.h"
#include "hud.h"
#include "geo_commands.h"
#include "hardcoded.h"
#include "skybox.h"

/**
 * @file skybox.c
 *
 * Implements the skybox background.
 *
 * It's not exactly a sky"box": it's more of a sky tilemap. It renders a 3x3 grid of 32x32 pieces of the
 * whole skybox image, starting from the top left based on the camera's rotation. A skybox image has 64
 * unique 32x32 tiles, with the first two columns duplicated for a total of 80.
 *
 * The tiles are mapped to world space such that 2 full tiles fit on the screen, for a total of
 * 8 tiles around the full 360 degrees. Each tile takes up 45 degrees of the camera's field of view, and
 * the code draws 3 tiles or 135 degrees of the skybox in a frame. But only 2 tiles, or 90 degrees, can
 * fit on-screen at a time.
 *
 * @bug FOV is handled strangely by the code. It is used to scale and rotate the skybox, when really it
 * should probably only be used to calculate the distance drawn from the center of the looked-at tile.
 * But since the game always sets it to 90 degrees, the skybox always scales and rotates the same,
 * regardless of the camera's actual FOV. So even if the camera's FOV is 10 degrees the game draws a
 * full 90 degrees of the skybox, which makes the sky look really far away.
 *
 * @bug Skyboxes unnecessarily repeat the first 2 columns when they could just wrap the col index.
 * Although, the wasted space is only about 128 bytes for each image.
 */

/**
 * Describes the position, tiles, and orientation of the skybox image.
 *
 * Describes the scaled x and y offset into the tilemap, based on the yaw and pitch.  Computes the
 * upperLeftTile index into the skybox's tile list using scaledX and scaledY. See get_top_left_tile_idx.
 *
 * The skybox is always drawn behind everything, because in the level's geo script, the skybox is drawn
 * first, in a display list with the Z buffer disabled
 */
struct Skybox {
    /// The camera's yaw, from 0 to (M_PI*2), which maps to 0 to 360 degrees
    f32 yaw;
    /// The camera's pitch, which is bounded by +-(M_PI/2), which maps to -90 to 90 degrees
    f32 pitch;
    /// The skybox's X position in world space
    f32 scaledX;
    /// The skybox's Y position in world space
    f32 scaledY;
};

struct Skybox sSkyBoxInfo[2];

typedef const Texture *const SkyboxTexture[80];

extern u8 gRenderingInterpolated;

s8 gReadOnlyBackground;
s8 gOverrideBackground = -1;
Color gSkyboxColor = { 255, 255, 255 };

extern SkyboxTexture bbh_skybox_ptrlist;
extern SkyboxTexture bitdw_skybox_ptrlist;
extern SkyboxTexture bitfs_skybox_ptrlist;
extern SkyboxTexture bits_skybox_ptrlist;
extern SkyboxTexture ccm_skybox_ptrlist;
extern SkyboxTexture cloud_floor_skybox_ptrlist;
extern SkyboxTexture clouds_skybox_ptrlist;
extern SkyboxTexture ssl_skybox_ptrlist;
extern SkyboxTexture water_skybox_ptrlist;
extern SkyboxTexture wdw_skybox_ptrlist;
Texture* gCustomSkyboxPtrList[80] = { NULL };

SkyboxTexture *sSkyboxTextures[10] = {
    &water_skybox_ptrlist,
    &bitfs_skybox_ptrlist,
    &wdw_skybox_ptrlist,
    &cloud_floor_skybox_ptrlist,
    &ccm_skybox_ptrlist,
    &ssl_skybox_ptrlist,
    &bbh_skybox_ptrlist,
    &bitdw_skybox_ptrlist,
    &clouds_skybox_ptrlist,
    &bits_skybox_ptrlist,
};

/**
 * The skybox color mask.
 * The final color of each pixel is computed from the bitwise AND of the color and the texture.
 */
u8 sSkyboxColors[][3] = {
    { 0x50, 0x64, 0x5A },
    { 0xFF, 0xFF, 0xFF },
};

/**
 * Constant used to scale the skybox horizontally to a multiple of the screen's width
 */
#define SKYBOX_WIDTH (4 * SCREEN_WIDTH)
/**
 * Constant used to scale the skybox vertically to a multiple of the screen's height
 */
#define SKYBOX_HEIGHT (4 * SCREEN_HEIGHT)

/**
 * The tile's width in world space.
 * By default, two full tiles can fit in the screen.
 */
#define SKYBOX_TILE_WIDTH (SCREEN_WIDTH / 2)
/**
 * The tile's height in world space.
 * By default, two full tiles can fit in the screen.
 */
#define SKYBOX_TILE_HEIGHT (SCREEN_HEIGHT / 2)

/**
 * The horizontal length of the skybox tilemap in tiles.
 */
#define SKYBOX_COLS (10)
/**
 * The vertical length of the skybox tilemap in tiles.
 */
#define SKYBOX_ROWS (8)

static u16 sSkyboxTileNumX = 5;
// Three rows is exactly enough to cover a screen. A portal window is not a
// screen: step up to it and it subtends far more than the game's field of view,
// and a grid sized to that field of view ends in a hard edge with passthrough
// above it. So this one does have to change after all - see the portal block in
// draw_skybox().
static u16 sSkyboxTileNumY = 3;

struct GrowingArray *gBackgroundSkyboxVerts = NULL;

/**
 * Convert the camera's yaw into an x position into the scaled skybox image.
 *
 * fov is always 90 degrees, set in draw_skybox_facing_camera.
 *
 * The calculation performed is equivalent to (360 / fov) * (yaw / 65536) * SCREEN_WIDTH
 * in other words: (the number of fov-sized parts of the circle there are) *
 *                 (how far is the camera rotated from 0, scaled 0 to 1)   *
 *                 (the screen width)
 */
f32 calculate_skybox_scaled_x(s8 player, f32 fov) {
    f32 yaw = sSkyBoxInfo[player].yaw;

    //! double literals are used instead of floats
    f32 scaledX = SCREEN_WIDTH * 180.0 * yaw / (fov * M_PI);

    if (scaledX > SKYBOX_WIDTH) {
        scaledX -= (s32) scaledX / SKYBOX_WIDTH * SKYBOX_WIDTH;
    }
    return SKYBOX_WIDTH - scaledX;
}

/**
 * Convert the camera's pitch into a y position in the scaled skybox image.
 *
 * fov may have been used in an earlier version, but the developers changed the function to always use
 * 90 degrees.
 */
f32 calculate_skybox_scaled_y(s8 player, UNUSED f32 fov) {
    // Convert pitch to degrees. Pitch is bounded between -90 (looking down) and 90 (looking up).
    f32 pitchInDegrees = sSkyBoxInfo[player].pitch * 180.0 / M_PI;

    // Scale by 360 / fov
    f32 degreesToScale = 360.0f * pitchInDegrees / 90.0;

    // Since pitch can be negative, and the tile grid starts 1 octant above the camera's focus, add
    // 5 octants to the y position
    f32 scaledY = degreesToScale + 5 * SKYBOX_TILE_HEIGHT;

    if (scaledY > SKYBOX_HEIGHT) {
        scaledY = SKYBOX_HEIGHT;
    }
    if (scaledY < SCREEN_HEIGHT) {
        scaledY = SCREEN_HEIGHT;
    }
    return scaledY;
}

/**
 * Generates vertices for the skybox tile.
 *
 * @param tileIndex The index into the 32x32 sections of the whole skybox image. The index is converted
 *                  into an x and y by modulus and division by SKYBOX_COLS. x and y are then scaled by
 *                  SKYBOX_TILE_WIDTH to get a point in world space.
 */
#ifdef TARGET_VISIONOS
/// Portal mode places the skybox on a sphere around the game camera instead of
/// on the screen, because there is no screen any more. A flat quad only works
/// while it covers a narrow angle; widen it enough to fill a window you can
/// walk up to and it stops being a distant backdrop - parts of it end up metres
/// from the viewer while others are a hundred metres away.
///
/// The mapping is the one the game already assumes: one tile is 45 degrees, two
/// tiles fill the screen, the field of view is always 90 (see the file comment).
#define SKYBOX_PORTAL_RADIUS 15000.0f

static f32 sPortalScaledX, sPortalScaledY;
static bool sPortalSkybox;

static void portal_sky_point(f32 x, f32 y, s16 *ox, s16 *oy, s16 *oz) {
    const f32 ndcX = 2.0f * (x - sPortalScaledX) / (f32) SCREEN_WIDTH - 1.0f;
    const f32 ndcY = (y - sPortalScaledY) / ((f32) SCREEN_HEIGHT / 2.0f) + 1.0f;

    const f32 yaw = ndcX * (f32) (M_PI / 4.0);
    f32 pitch = ndcY * (f32) (M_PI / 4.0);
    // Keep well clear of the poles. A latitude/longitude grid degenerates
    // there: both upper corners of a tile land on the same point, several rows
    // stack onto it, and what is left are thin slivers with unreliable winding
    // - the spokes visible when looking straight up. Stopping at 78 degrees
    // keeps every quad a quad. The cap above it is not covered; filling it
    // properly means subdividing the tiles so their edges follow the arc
    // instead of cutting across it.
    // Just short of the pole, not well before it. Exactly at it both upper
    // corners of a tile collapse onto one point: one of the two triangles goes
    // to zero area and the other covers only half the wedge, which is what left
    // the spokes. Half a degree short keeps the top edge a real, if tiny, arc,
    // so both triangles stay valid and the cap closes to a disc under a fifth
    // of a degree across - the sky is 360 degrees for any practical purpose.
    const f32 limit = 1.5620f; // 89.5 degrees
    if (pitch >  limit) { pitch =  limit; }
    if (pitch < -limit) { pitch = -limit; }

    const f32 cp = cosf(pitch);
    *ox = (s16) ( SKYBOX_PORTAL_RADIUS * cp * sinf(yaw));
    *oy = (s16) ( SKYBOX_PORTAL_RADIUS * sinf(pitch));
    *oz = (s16) (-SKYBOX_PORTAL_RADIUS * cp * cosf(yaw));
}
#endif

Vtx *make_skybox_rect(s32 tileRow, s32 tileCol, s32 row, s32 col) {
    u16 index = row * sSkyboxTileNumX + col;
    Vtx *verts;
    if (gRenderingInterpolated) {
        verts = gBackgroundSkyboxVerts->buffer[index];
    } else {
        verts = alloc_display_list(4 * sizeof(*verts));
        gBackgroundSkyboxVerts->buffer[index] = verts;
    }

    f32 x = tileCol * SKYBOX_TILE_WIDTH;
    f32 y = SKYBOX_HEIGHT - tileRow / SKYBOX_COLS * SKYBOX_TILE_HEIGHT;

#ifdef TARGET_VISIONOS
    if (sPortalSkybox && verts != NULL) {
        const f32 xs[4] = { x, x, x + SKYBOX_TILE_WIDTH, x + SKYBOX_TILE_WIDTH };
        const f32 ys[4] = { y, y - SKYBOX_TILE_HEIGHT, y - SKYBOX_TILE_HEIGHT, y };
        const s16 us[4] = { 0, 0, 31 << 5, 31 << 5 };
        const s16 vs[4] = { 0, 31 << 5, 31 << 5, 0 };
        for (s32 i = 0; i < 4; i++) {
            s16 px, py, pz;
            portal_sky_point(xs[i], ys[i], &px, &py, &pz);
            make_vertex(verts, i, px, py, pz, us[i], vs[i], 255, 255, 255, 255);
        }
        return verts;
    }
#endif

    if (verts != NULL) {
        make_vertex(verts, 0, x, y, -1, 0, 0, 255, 255, 255, 255);
        make_vertex(verts, 1, x, y - SKYBOX_TILE_HEIGHT, -1, 0, 31 << 5, 255, 255, 255, 255);
        make_vertex(verts, 2, x + SKYBOX_TILE_WIDTH, y - SKYBOX_TILE_HEIGHT, -1, 31 << 5, 31 << 5, 255, 255, 255, 255);
        make_vertex(verts, 3, x + SKYBOX_TILE_WIDTH, y, -1, 31 << 5, 0, 255, 255, 255, 255);
    }
    return verts;
}

/**
 * Draws a 3x3 grid of 32x32 sections of the original skybox image.
 * The row and column are converted into an index into the skybox's tile list, which is then drawn in
 * world space so that the tiles will rotate with the camera.
 */
void draw_skybox_tile_grid(Gfx **dlist, s8 background, s8 player, s8 colorIndex) {
    s32 row;
    s32 col;

#ifdef TARGET_VISIONOS
    // Not in the 3D window: that mode renders the game's own view into a
    // texture, so its sky belongs on the screen where the game puts it.
    extern int mariovision_render_mode(void);
    const int mode = mariovision_render_mode();
    sPortalSkybox = (mode >= 2); // portal, VR, first person
    sPortalScaledX = sSkyBoxInfo[player].scaledX;
    sPortalScaledY = sSkyBoxInfo[player].scaledY;
#endif

    s32 colOffset = (sSkyboxTileNumX / 2) - 1;
    // Rows grow downwards from the first one, so extra rows would all pile up
    // below the view. This centres them the way colOffset centres the columns;
    // at the stock three rows it is zero and nothing changes.
    s32 rowOffset = (sSkyboxTileNumY / 2) - 1;
    if (rowOffset < 0) { rowOffset = 0; }

    for (row = 0; row < sSkyboxTileNumY; row++) {
        for (col = 0; col < sSkyboxTileNumX; col++) {
            s32 tileRow = (s32) (((SKYBOX_HEIGHT - sSkyBoxInfo[player].scaledY) / SKYBOX_TILE_HEIGHT) + row - rowOffset) * SKYBOX_COLS;
            s32 tileColTmp = ((floor(sSkyBoxInfo[player].scaledX / SKYBOX_TILE_WIDTH) + col) - colOffset);
            s32 tileCol = tileColTmp;
            if (tileCol >= SKYBOX_ROWS) { tileCol -= SKYBOX_ROWS; }
            if (tileCol < 0) { tileCol += SKYBOX_ROWS; }
            s32 tileIndex = tileRow + tileCol;

            // UGLY HACK: if the camera moves weird after a level transition this can go too high
            if (tileIndex < 0)  { tileIndex = 0;  }
            if (tileIndex > 79) { tileIndex = 79; }
            const Texture* texture = NULL;
            if (background < 0 || background >= 10) {
                texture = gCustomSkyboxPtrList[tileIndex];
            } else {
                texture = (*(SkyboxTexture *) segmented_to_virtual(sSkyboxTextures[background]))[tileIndex];
            }

            f32 r = gSkyboxColor[0] / 255.0f;
            f32 g = gSkyboxColor[1] / 255.0f;
            f32 b = gSkyboxColor[2] / 255.0f;
            u8 *color = sSkyboxColors[colorIndex];
            gDPSetEnvColor((*dlist)++, color[0] * r, color[1] * g, color[2] * b, 255);

            Vtx *vertices = make_skybox_rect(tileRow, tileColTmp, row, col);

            gLoadBlockTexture((*dlist)++, 32, 32, G_IM_FMT_RGBA, texture);
            gSPVertex((*dlist)++, VIRTUAL_TO_PHYSICAL(vertices), 4, 0);
            gSPDisplayList((*dlist)++, dl_draw_quad_verts_0123);
        }
    }
}

void *create_skybox_ortho_matrix(s8 player) {
    f32 left = sSkyBoxInfo[player].scaledX;
    f32 right = sSkyBoxInfo[player].scaledX + SCREEN_WIDTH;
    f32 bottom = sSkyBoxInfo[player].scaledY - SCREEN_HEIGHT;
    f32 top = sSkyBoxInfo[player].scaledY;

    extern Mtx* gBackgroundSkyboxMtx;
    Mtx *mtx;
    if (gRenderingInterpolated) {
        mtx = gBackgroundSkyboxMtx;
    } else {
        mtx = alloc_display_list(sizeof(*mtx));
        gBackgroundSkyboxMtx = mtx;
    }

    if (mtx != NULL) {
        guOrtho(mtx, left, right, bottom, top, 0.0f, 3.0f, 1.0f);
    } else {
    }

    return mtx;
}

/**
 * Creates the skybox's display list, then draws the 3x3 grid of tiles.
 */
Gfx *init_skybox_display_list(s8 player, s8 background, s8 colorIndex) {
    extern Gfx* gBackgroundSkyboxGfx;

    s32 dlCommandCount = 5 + (sSkyboxTileNumY * sSkyboxTileNumX) * 8; // 5 for the start and end, plus the amount of skybox tiles

    void *skybox;
    if (gRenderingInterpolated) {
        skybox = gBackgroundSkyboxGfx;
    } else {
        skybox = alloc_display_list(dlCommandCount * sizeof(Gfx));
        gBackgroundSkyboxGfx = (Gfx*)skybox;
    }

    Gfx *dlist = skybox;

    if (skybox == NULL) {
        return NULL;
    } else {
        Mtx *ortho = create_skybox_ortho_matrix(player);

        gSPDisplayList(dlist++, dl_skybox_begin);

#ifdef TARGET_VISIONOS
        // Portal and VR only. The 3D window renders the game's own view into a
        // texture, so its sky stays where the game puts it - on the screen.
        extern int mariovision_render_mode(void);
        if (mariovision_render_mode() >= 2) {
            // In portal mode the tiles are already points in the camera's space,
            // on a sphere, so what they need is the world's projection - not a
            // screen-space ortho, which would only undo that.
            //
            // Saying so outright instead of letting the portal guess: an
            // all-zero matrix has m[3][3] == 0, which the override reads as a
            // perspective and replaces wholesale with the per-eye matrix. The
            // contents never matter, only the classification. Guessing from the
            // draw order failed because the perspective count is not always one
            // per pass - and when the two sides disagreed, sphere coordinates
            // met the glass matrix, which is what crashed.
            Mtx *marker = alloc_display_list(sizeof(*marker));
            if (marker != NULL) {
                memset(marker, 0, sizeof(*marker));
                gSPMatrix(dlist++, VIRTUAL_TO_PHYSICAL(marker),
                          G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
            }
        } else
#endif
        gSPMatrix(dlist++, VIRTUAL_TO_PHYSICAL(ortho), G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
        gSPDisplayList(dlist++, dl_skybox_tile_tex_settings);
        draw_skybox_tile_grid(&dlist, background, player, colorIndex);
        gSPDisplayList(dlist++, dl_skybox_end);
        gSPEndDisplayList(dlist);
    }
    return skybox;
}

/**
 * Draw a skybox facing the direction from pos to foc.
 *
 * @param player Unused, determines which orientation info struct to update
 * @param background The skybox image to use
 * @param fov Unused. It SHOULD control how much the skybox is scaled, but the way it's coded it just
 *            controls how fast the skybox rotates. The given value is replaced with 90 right before the
 *            dl is created
 * @param posX,posY,posZ The camera's position
 * @param focX,focY,focZ The camera's focus.
 */
Gfx *create_skybox_facing_camera(s8 player, s8 background, f32 fov,
                                    f32 posX, f32 posY, f32 posZ,
                                    f32 focX, f32 focY, f32 focZ) {
    if (!gBackgroundSkyboxVerts) {
        gBackgroundSkyboxVerts = growing_array_init(NULL, sSkyboxTileNumY * sSkyboxTileNumX, malloc, free);
        gBackgroundSkyboxVerts->count = sSkyboxTileNumY * sSkyboxTileNumX;
        if (!gBackgroundSkyboxVerts) {
            sys_fatal("Cannot allocate skybox vertex buffer");
        }
    }

#ifdef TARGET_VISIONOS
    // Portal mode needs sky well past the game's field of view, because the
    // window can be walked up to. Nine rows reach roughly three times as far;
    // the tilemap clamps at its edges, so the extra ones repeat the topmost sky
    // instead of leaving a hole. Two triangles each - the cost is nothing.
    extern int mariovision_render_mode(void);
    // Five rows on the sphere: one tile is 45 degrees and rowOffset centres
    // them, so this reaches from the zenith down past the nadir. Nine were only
    // ever needed to paper over the flat quad's poor angular reach.
    // Six rows at 45 degrees each reach past both poles. The ones beyond are
    // clamped flat and rasterise to nothing, so they cost two degenerate
    // triangles and buy a sky that closes overhead and underfoot.
    const u16 wantTilesY = (mariovision_render_mode() >= 2) ? 6 : 3;
    if (sSkyboxTileNumY != wantTilesY && !gRenderingInterpolated) {
        sSkyboxTileNumY = wantTilesY;
        if (gBackgroundSkyboxVerts) {
            gBackgroundSkyboxVerts->count = sSkyboxTileNumY * sSkyboxTileNumX;
            growing_array_alloc(gBackgroundSkyboxVerts, 0);
        }
    }
#endif

    if (!gRenderingInterpolated) {
        f32 skyboxAspectRatio = ((f32)sSkyboxTileNumX * (f32)SKYBOX_TILE_WIDTH) / ((f32)sSkyboxTileNumY * (f32)SKYBOX_TILE_HEIGHT);
        f32 half_width = skyboxAspectRatio / GFX_DIMENSIONS_ASPECT_RATIO * SCREEN_WIDTH / 2;
        if (half_width < SCREEN_WIDTH / 2) {
            // how many horizontal tiles are needed to match the screen aspect ratio
            f32 minTilesX = sSkyboxTileNumY * ((f32)SKYBOX_TILE_HEIGHT / (f32)SKYBOX_TILE_WIDTH) * GFX_DIMENSIONS_ASPECT_RATIO;
            sSkyboxTileNumX = (u16) ceilf(minTilesX);

            // Update vertex buffer size
            gBackgroundSkyboxVerts->count = sSkyboxTileNumY * sSkyboxTileNumX;
            growing_array_alloc(gBackgroundSkyboxVerts, 0);
        }
    }

    gReadOnlyBackground = background;
    background = gOverrideBackground == -1 ? background : gOverrideBackground;

    f32 cameraFaceX = focX - posX;
    f32 cameraFaceY = focY - posY;
    f32 cameraFaceZ = focZ - posZ;
    s8 colorIndex = 1;

    // If the first star is collected in JRB, make the sky darker and slightly green
    if (background == BACKGROUND_ABOVE_CLOUDS && gLevelValues.jrbDarkenSkybox && !(save_file_get_star_flags(gCurrSaveFileNum - 1, COURSE_JRB - 1) & 1)) {
        colorIndex = 0;
    }

    //! fov is always set to 90.0f. If this line is removed, then the game crashes because fov is 0 on
    //! the first frame, which causes a floating point divide by 0
    fov = 90.0f;

    sSkyBoxInfo[player].yaw = (M_PI / 2.0) - atan2(cameraFaceZ, cameraFaceX);
    if (sSkyBoxInfo[player].yaw < 0) { sSkyBoxInfo[player].yaw += M_PI * 2.0; }
    sSkyBoxInfo[player].pitch = (M_PI / 2.0) - atan2(sqrtf(cameraFaceX * cameraFaceX + cameraFaceZ * cameraFaceZ), cameraFaceY);

    sSkyBoxInfo[player].scaledX = calculate_skybox_scaled_x(player, fov);
    sSkyBoxInfo[player].scaledY = calculate_skybox_scaled_y(player, fov);

    return init_skybox_display_list(player, background, colorIndex);
}
