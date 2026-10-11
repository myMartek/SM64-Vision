/*
 * Portal mode state and matrix plumbing - see mariovision_portal.h.
 *
 * Matrix conventions: gfx_pc uses row-vector matrices (v·M, mtxf_mul(out,A,B)
 * computes A·B in that order), while simd/Metal use column vectors. Everything
 * arriving from Swift is column-major and gets transposed once here.
 */

#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <os/log.h>

// Diagnostics go to stderr, not os_log: devicectl's --console bridges stdout
// and stderr, and that is the only channel that reaches a tethered headset.
// Every measurement that only existed in os_log was invisible on device, which
// is why the simulator kept being the thing under test.
#define MV_LOG(fmt, ...) do { fprintf(stderr, "[portal] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while (0)

#include "mariovision_portal.h"
#include "mariovision_platform.h"

#include "pc/gfx/gfx_pc.h"
#include "sm64.h"
#include "types.h"
#include "game/mario.h"
#include "game/level_update.h"
#include "game/area.h"
#include "game/first_person_cam.h"
#include "engine/surface_collision.h"
#include "engine/math_util.h"
#include "game/save_file.h"
#include "game/hardcoded.h"
#include "pc/lua/utils/smlua_level_utils.h"
#include "pc/visionos/mariovision_hands.h"
#include "pc/visionos/mariovision_levels.h"
#include "level_table.h"

extern void gfx_end_frame_render(void);

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

static _Atomic bool sActive = false;
static _Atomic int sRenderMode = MARIOVISION_RENDER_PORTAL;
static _Atomic bool sPassActive = false;
static _Atomic bool sScreenPass = false;
static _Atomic float sScreenShiftX = 0.0f;
static _Atomic float sScreenFocusZ = 1000.0f;
static _Atomic bool sOrthoNow = false;
static _Atomic bool sSkyboxNow = false;
static _Atomic int sDbgPersp = 0, sDbgOrtho = 0, sDbgSkipped = 0;
// Triangles seen and dropped by gfx_pc's clip test during one eye pass.
static int sAllTris, sAllRejected;

// Published display list (game thread writes, render thread reads).
static pthread_mutex_t sListMutex = PTHREAD_MUTEX_INITIALIZER;
static void *sDisplayList = NULL;

// Per-eye matrices, written by the render thread between eyes. Row layout for
// gfx_pc, column layout kept for the shader.
static float sQRow[4][4];
static float sGlassRow[4][4];
static float sClipToPortalCol[16];
static float sEyePortal[3];
static float sHalf[2] = { 0.5f, 0.375f }; // 1.0 m x 0.75 m window (4:3)

static _Atomic float sStereoScale = 1.5f;
static _Atomic float sDistance = 1.2f;
static _Atomic float sWorldScale = 1.0f / 400.0f;
static _Atomic float sConvergence = 0.0f;
static _Atomic float sFovScale = 1.0f;

static void to_row_layout(float out[4][4], const float colMajor[16]) {
    // gfx_pc's row-vector convention stores M^T row-major - and that is
    // byte-identical to M stored column-major: index i*4+j holds element (j, i)
    // in both. A straight copy is the correct "transposition"; doing an actual
    // element swap here is the bug that made the world vanish.
    memcpy(out, colMajor, 16 * sizeof(float));
}

//-----------------------------------------------------------------------------
// Projection trace
//
// Every projection the game sets during one eye's pass, in order. The skybox is
// supposed to show up as LOAD(id) followed by MUL - if it does not, it never
// takes the path the portal assumes, and that is what the device numbers hint
// at. One frame a second, so the console stays readable.
//-----------------------------------------------------------------------------

static char sSeq[1400];
static int sSeqLen;
static bool sSeqCapturing;

static bool trace_enabled(void) {
    static int sEnabled = -1;
    if (sEnabled < 0) { sEnabled = (getenv("MARIOVISION_VTXLOG") != NULL); }
    return sEnabled != 0;
}

static void seq_note(const char *what, const float m[4][4]) {
    if (!sSeqCapturing || sSeqLen >= (int) sizeof(sSeq) - 64) { return; }

    const char *kind = "persp";
    if (m[3][3] != 0.0f) {
        kind = "ortho";
        bool identity = true;
        for (int i = 0; i < 4 && identity; i++) {
            for (int j = 0; j < 4; j++) {
                const float d = m[i][j] - ((i == j) ? 1.0f : 0.0f);
                if (d > 1e-5f || d < -1e-5f) { identity = false; break; }
            }
        }
        if (identity) { kind = "IDENT"; }
    }
    sSeqLen += snprintf(sSeq + sSeqLen, sizeof(sSeq) - sSeqLen,
                        "%s:%s[%.2f %.2f %.2f] ", what, kind,
                        (double)m[0][0], (double)m[1][1], (double)m[3][3]);
}

/// Screen-space rectangles - the vanilla HUD, fades, dialogue boxes - write
/// their clip coordinates straight into the vertex buffer, past every
/// projection. In the portal that would smear them across the whole field of
/// view, which is why they used to be dropped entirely. Sending them through
/// the same glass matrix the ortho path uses puts them on the window pane
/// instead, a millimetre behind the glass, where the clip test trims them at
/// the frame rather than discarding them.
void mariovision_portal_transform_rect(float *x, float *y, float *z, float *w) {
    if (!atomic_load(&sPassActive) || atomic_load(&sScreenPass)) { return; }

    // What range do the rectangles actually occupy before the glass matrix?
    // The window maps NDC [-1, 1] onto the pane, so anything past that gets
    // trimmed at the frame - and that is what the HUD looks like it is doing.
    if (trace_enabled()) {
        static float lo = 1e9f, hi = -1e9f, loY = 1e9f, hiY = -1e9f;
        static int n = 0;
        if (*x < lo) { lo = *x; }
        if (*x > hi) { hi = *x; }
        if (*y < loY) { loY = *y; }
        if (*y > hiY) { hiY = *y; }
        if (++n >= 2000) {
            MV_LOG("rect ndc x=[%.3f %.3f] y=[%.3f %.3f] (window is -1..1)",
                   (double)lo, (double)hi, (double)loY, (double)hiY);
            lo = loY = 1e9f; hi = hiY = -1e9f; n = 0;
        }
    }

    const float in[4] = { *x, *y, *z, *w };
    float out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int j = 0; j < 4; j++) {
        for (int k = 0; k < 4; k++) {
            out[j] += in[k] * sGlassRow[k][j]; // row-vector: v * glass
        }
    }
    *x = out[0]; *y = out[1]; *z = out[2]; *w = out[3];
}

void mariovision_portal_note_projection_mul(const float m[4][4]) {
    seq_note("MUL", m);
}

//-----------------------------------------------------------------------------
// Mode
//-----------------------------------------------------------------------------

int mariovision_render_mode(void) {
    return atomic_load(&sRenderMode);
}

void mariovision_set_render_mode(int mode) {
    if (mode < MARIOVISION_RENDER_WINDOW_2D || mode > MARIOVISION_RENDER_FIRST_PERSON) { return; }
    atomic_store(&sRenderMode, mode);
}

static _Atomic bool sRendererBusy = false;

bool mariovision_portal_renderer_busy(void) {
    return atomic_load(&sRendererBusy);
}

bool mariovision_portal_unclipped(void) {
    // The 3D window's first pass renders in the game's own camera space, into a
    // texture of its own. Testing it against the pane's geometry discards
    // essentially all of it - which is why that mode came out black. It has no
    // frame to be trimmed at either; the pane is applied afterwards, when the
    // texture is drawn.
    if (atomic_load(&sScreenPass)) { return true; }
    const int mode = atomic_load(&sRenderMode);
    return mode == MARIOVISION_RENDER_VR || mode == MARIOVISION_RENDER_FIRST_PERSON;
}

//-----------------------------------------------------------------------------
// First-person VR
//-----------------------------------------------------------------------------

static _Atomic float sHeadYaw = 0.0f;    // radians, from the entry heading
static _Atomic float sHeadPitch = 0.0f;
static short sSnapYaw = 0;               // s16 game angle, 30 degree steps
static bool sFirstPersonOn = false;
static _Atomic bool sFirstPersonActive = false;
static unsigned int sPrevAction = 0;
static bool sPrevActive = false;
static _Atomic bool sAirborne = false;
static bool sSnapArmed = true;
/// Frames the head has been tilted up at the sun.
static int sLookUpHeld;
/// How far the world is turned about the line of sight, in radians.
static _Atomic float sFirstPersonRoll = 0.0f;
float mariovision_first_person_roll(void) {
    return atomic_load(&sFirstPersonRoll);
}

/// 30 degrees in the game's s16 angle: 65536 * 30 / 360.
#define MARIOVISION_SNAP_STEP 5461

void mariovision_set_head_pose(float yaw, float pitch) {
    atomic_store(&sHeadYaw, yaw);
    atomic_store(&sHeadPitch, pitch);
}

/// Radians about the room's up axis to the game's s16 angle. The game turns the
/// opposite way round from the room, hence the sign.
static short head_yaw_angle(void) {
    const float turns = -atomic_load(&sHeadYaw) / (2.0f * 3.14159265358979f);
    return (short) (int) (turns * 65536.0f);
}

//-----------------------------------------------------------------------------
// HUD layout in VR
//
// The readouts sit at the corners of the pane the ortho content is laid onto,
// and in VR that pane is deliberately large - big enough for the room-change
// animations, which fill it. Its corners then land some 47 degrees out to the
// side, which is past the edge of vision: the lives and coins are not merely
// awkward to read, they are off screen entirely.
//
// The pane has to keep its size, so the layout moves instead. Positions are
// contracted towards the centre of the 320x240 space the game draws in, and
// pushed a little further down. Only what render_hud draws is affected -
// dialogs, the pause screen and the transitions keep the full pane.
//-----------------------------------------------------------------------------

static bool sHudLayoutActive = false;

static float hud_env(const char *name, float fallback) {
    const char *raw = getenv(name);
    if (!raw) { return fallback; }
    const float v = strtof(raw, NULL);
    return v;
}

void mariovision_hud_layout_begin(void) { sHudLayoutActive = true; }
void mariovision_hud_layout_end(void) { sHudLayoutActive = false; }

/// The contraction, as a factor and a drop. `scale` is how much of the distance
/// from the centre is kept; `drop` is how far down afterwards, in the game's own
/// 240-unit height. False when it does not apply.
/// How far past the screen a surface meant to cover it has to go.
///
/// On a television "the whole screen" and "everything one can see" are the same
/// rectangle. In a headset they are not: the game's picture is a pane hung in
/// front of one, and the fade that ends a level, or the black around the inside
/// of a cannon, stops at its edge with the room carrying on around it - which
/// gives away the trick completely.
///
/// The readouts are drawn in to four tenths so they can be taken in without
/// turning one's head, so a pane-sized rectangle covers about that fraction of
/// the view. Reaching six times as far is comfortably past the edge of sight in
/// every direction, and costs nothing: it is two triangles either way.
float mariovision_hud_overscan(void) {
    const int mode = atomic_load(&sRenderMode);
    if (mode != MARIOVISION_RENDER_VR && mode != MARIOVISION_RENDER_FIRST_PERSON) {
        return 1.0f;
    }
    return hud_env("MARIOVISION_OVERSCAN", 6.0f);
}

//-----------------------------------------------------------------------------
// What lies beyond the picture
//
// A fade or the black inside a cannon is meant to be everything one can see.
// No flat surface can manage that, however far it is stretched - turn one's
// head far enough and its edge arrives, with the room carrying on past it.
//
// So it stops being a surface. The colour is handed to the renderer, which
// wipes the eye with it before anything is drawn: a wipe has no edges, and it
// is behind every direction at once. What the game draws afterwards - the view
// down the barrel, the card with the stars on it - lands on top of it as before.
//-----------------------------------------------------------------------------

static _Atomic unsigned int sBackdrop = 0;
/// The game frame it was last named on.
static _Atomic unsigned int sBackdropFrame = 0;

void mariovision_set_backdrop(unsigned char r, unsigned char g,
                              unsigned char b, unsigned char a) {
    atomic_store(&sBackdrop, ((unsigned) a << 24) | ((unsigned) r << 16)
                             | ((unsigned) g << 8) | (unsigned) b);
    atomic_store(&sBackdropFrame, (unsigned int) gGlobalTimer + 1u);
}

bool mariovision_backdrop(float out[4]) {
    // On a flat window the picture has a frame and is meant to: wiping the room
    // around it would be nonsense. This is only for the modes where the picture
    // is everything one can see.
    if (mariovision_hud_overscan() <= 1.0f) { return false; }

    // How long ago it was named, counted in the game's own frames.
    //
    // The first attempt counted calls instead, and that was wrong by a factor
    // of six: the renderer runs three times for every game frame and asks once
    // per eye, so the allowance ran out long before the next frame arrived and
    // the colour flickered on and off. A frame number cannot be miscounted that
    // way, however often it is read.
    const unsigned int named = atomic_load(&sBackdropFrame);
    if (named == 0) { return false; }
    const unsigned int now = (unsigned int) gGlobalTimer + 1u;
    if (now - named > 2u) { return false; }

    const unsigned int c = atomic_load(&sBackdrop);
    out[0] = (float) ((c >> 16) & 0xFF) / 255.0f;
    out[1] = (float) ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (float) (c & 0xFF) / 255.0f;
    out[3] = (float) ((c >> 24) & 0xFF) / 255.0f;
    return true;
}

/// The same drawing-in as mariovision_hud_ortho, without asking whether the HUD
/// is being built.
///
/// That bracket is set around the readouts and the text labels and nothing else,
/// so anything drawn outside it - a dialogue box, for one - asked the question
/// and was always told no. The answer it wanted depends only on the mode.
bool mariovision_hud_inset(float *scale, float *drop) {
    const int mode = atomic_load(&sRenderMode);
    if (mode != MARIOVISION_RENDER_VR && mode != MARIOVISION_RENDER_FIRST_PERSON) {
        return false;
    }
    if (scale) { *scale = hud_env("MARIOVISION_HUD_INSET", 0.40f); }
    if (drop)  { *drop  = hud_env("MARIOVISION_HUD_LOWER", 0.0f); }
    return true;
}

bool mariovision_hud_ortho(float *scale, float *drop) {
    if (!sHudLayoutActive) { return false; }
    const int mode = atomic_load(&sRenderMode);
    if (mode != MARIOVISION_RENDER_VR && mode != MARIOVISION_RENDER_FIRST_PERSON) { return false; }
    if (scale) { *scale = hud_env("MARIOVISION_HUD_INSET", 0.40f); }
    // Was 26, which sat the readouts a touch below the line they read best on.
    // Zero leaves them where the contraction alone puts them - the extra push
    // downwards is what the first pass over-did.
    if (drop)  { *drop  = hud_env("MARIOVISION_HUD_LOWER", 0.0f); }
    return true;
}

/// The same contraction for a texture rectangle, which is given as an origin
/// and a size in screen space - where y counts downwards, unlike the ortho
/// coordinates above. Position and size are scaled together, so the spacing
/// inside a readout survives; contracting each coordinate on its own does not,
/// and crushes the "x" into the digit beside it.
///
/// This has to happen while the display list is being built. Texture rectangles
/// bypass the matrix stack entirely, and by the time the renderer replays them
/// the bracket around render_hud is long since closed - it lives on the other
/// thread.
bool mariovision_hud_rect(float *x, float *y, float *w, float *h) {
    float k = 1.0f, drop = 0.0f;
    if (!mariovision_hud_ortho(&k, &drop)) { return false; }
    if (x) { *x = 160.0f + (*x - 160.0f) * k; }
    if (y) { *y = 120.0f + (*y - 120.0f) * k + drop; }  // screen space: down is +y
    if (w) { *w *= k; }
    if (h) { *h *= k; }
    return true;
}

/// True while Mario is off the ground. Read by the renderer, which eases the
/// view down over a jump so the ground ahead comes into sight without the
/// player having to look for it.
bool mariovision_first_person_airborne(void) {
    return atomic_load(&sAirborne);
}

bool mariovision_first_person_active(void) {
    return atomic_load(&sFirstPersonActive);
}

bool mariovision_hide_mario_body(void) {
    return atomic_load(&sFirstPersonActive);
}

bool mariovision_input_pitch(short *out) {
    if (!atomic_load(&sFirstPersonActive)) { return false; }
    // Negated. The derivation said otherwise - positive faceAngle[0] does swim
    // upwards - but on the headset up and down came out the wrong way round, so
    // the head's pitch runs opposite to the game's. Same lesson as the yaw,
    // where the two also disagree; the sign is a property of the device pose and
    // not something to be reasoned out from the game's side alone.
    const float turns = -atomic_load(&sHeadPitch) / (2.0f * 3.14159265358979f);
    if (out) { *out = (short) (int) (turns * 65536.0f); }
    return true;
}

bool mariovision_input_yaw(short *out) {
    if (!atomic_load(&sFirstPersonActive)) { return false; }
    // Where the world is drawn from is the snap heading; where the viewer is
    // actually looking is that plus the head. Mario walks towards the latter.
    if (out) { *out = (short) (sSnapYaw + head_yaw_angle()); }
    return true;
}

static _Atomic float sEyeCamX = 0.0f, sEyeCamY = 0.0f, sEyeCamZ = 0.0f;

void mariovision_set_eye_in_camera_space(float x, float y, float z) {
    atomic_store(&sEyeCamX, x);
    atomic_store(&sEyeCamY, y);
    atomic_store(&sEyeCamZ, z);
}

//-----------------------------------------------------------------------------
// Walking on one's own feet
//
// Stepping sideways in the room moved the view and nothing else: Mario stayed
// where the mode was entered, shadow and all, and one drifted out of one's own
// body. He should come along.
//
// Two things have to happen together, or the movement doubles. Mario moves by
// what the viewer walked - and the play space's own origin moves with him, so
// that the head, measured against that origin, is back where it started. A rig
// that only did the first would carry the viewer away at twice the speed.
//
// What Mario could not do - a wall in the way - is not passed back, so the
// anchor stays put and one simply leans past him. Walking off a ledge, on the
// other hand, is left entirely alone: he is placed and the game's own physics
// decide what happens next, which is the honest answer and the one that was
// asked for.
//-----------------------------------------------------------------------------

static _Atomic float sWalkPendingX = 0.0f, sWalkPendingZ = 0.0f;
static _Atomic float sWalkTakenX = 0.0f, sWalkTakenZ = 0.0f;

void mariovision_set_walk_delta(float dx, float dz) {
    atomic_store(&sWalkPendingX, atomic_load(&sWalkPendingX) + dx);
    atomic_store(&sWalkPendingZ, atomic_load(&sWalkPendingZ) + dz);
}

static void walk_with_the_viewer(struct MarioState *m) {
    const float dx = atomic_load(&sWalkPendingX);
    const float dz = atomic_load(&sWalkPendingZ);
    atomic_store(&sWalkPendingX, 0.0f);
    atomic_store(&sWalkPendingZ, 0.0f);

    // A few millimetres is tracking noise, not walking.
    if (dx * dx + dz * dz < 0.25f) { return; }

    // Anything the game is narrating is left alone; being nudged along during
    // a cutscene is worse than standing still through it.
    if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_CUTSCENE) { return; }
    if (m->action == ACT_IN_CANNON || (m->action & ACT_FLAG_ON_POLE)) { return; }

    // Camera space into the world: the same single rotation about the up axis
    // that the hands take, since this mode holds the camera's pitch at zero.
    const s16 yaw = gFirstPersonCamera.yaw;
    const f32 s = sins(yaw), c = coss(yaw);
    const f32 worldDX = dx * c + dz * s;
    const f32 worldDZ = -dx * s + dz * c;

    f32 x = m->pos[0] + worldDX;
    f32 y = m->pos[1];
    f32 z = m->pos[2] + worldDZ;

    // Walls stop him, as they would if he had walked there himself.
    f32_find_wall_collision(&x, &y, &z, 60.0f, 50.0f);

    const f32 tookX = x - m->pos[0];
    const f32 tookZ = z - m->pos[2];
    m->pos[0] = x;
    m->pos[2] = z;

    // Only what he actually managed is handed back, so a wall does not carry
    // the play space through it.
    atomic_store(&sWalkTakenX, atomic_load(&sWalkTakenX) + tookX);
    atomic_store(&sWalkTakenZ, atomic_load(&sWalkTakenZ) + tookZ);
}

bool mariovision_take_walk(float out[2]) {
    const float x = atomic_load(&sWalkTakenX);
    const float z = atomic_load(&sWalkTakenZ);
    if (x == 0.0f && z == 0.0f) { return false; }
    atomic_store(&sWalkTakenX, 0.0f);
    atomic_store(&sWalkTakenZ, 0.0f);
    out[0] = x;
    out[1] = z;
    return true;
}

bool mariovision_billboard_eye(float out[3]) {
    if (!atomic_load(&sFirstPersonActive)) { return false; }
    out[0] = atomic_load(&sEyeCamX);
    out[1] = atomic_load(&sEyeCamY);
    out[2] = atomic_load(&sEyeCamZ);
    return true;
}

/// Black all round while sitting in a cannon.
///
/// Said here rather than left to the display list that draws the black ring:
/// that is built inside the graph traversal, and whether it runs on a given
/// frame is the graph's business, not ours. This runs once per game frame,
/// full stop, which is what the colour needs.
static void cannon_backdrop(void) {
    // Asked of Mario as well as of the camera. The camera mode is what the
    // black ring is drawn on, but it is set by the camera code and read here a
    // step earlier in the frame, so on the way in and out there are frames
    // where one says yes and the other no. Either will do.
    const bool byCamera = gCurrentArea != NULL && gCurrentArea->camera != NULL
                          && gCurrentArea->camera->mode == CAMERA_MODE_INSIDE_CANNON;
    const bool byMario = gMarioStates[0].action == ACT_IN_CANNON
                         || gMarioStates[0].action == ACT_SHOT_FROM_CANNON;
    if (!byCamera && !byMario) { return; }
    mariovision_set_backdrop(0, 0, 0, 255);
}

void mariovision_first_person_update(void) {
    cannon_backdrop();

    struct MarioState *m = &gMarioStates[0];
    // Nothing here may run before a level is live. first_person_check_cancels
    // walks the object lists looking for the act selector and for Bowser, and
    // those lists do not exist yet while the game is still loading - which is
    // exactly when a mode left selected from the last session engages.
    const bool worldReady = m->marioObj != NULL && m->area != NULL;
    const bool want = atomic_load(&sRenderMode) == MARIOVISION_RENDER_FIRST_PERSON
                      && mariovision_game_running() && worldReady;

    if (want != sFirstPersonOn) {
        sFirstPersonOn = want;
        // Anything still pending belongs to the session that just ended.
        atomic_store(&sWalkPendingX, 0.0f);
        atomic_store(&sWalkPendingZ, 0.0f);
        atomic_store(&sWalkTakenX, 0.0f);
        atomic_store(&sWalkTakenZ, 0.0f);
        set_first_person_enabled(want);
        if (want) {
            // Hold both angles ourselves. coopdx would otherwise drive them
            // from the mouse, and the pitch especially has to stay at zero:
            // the head supplies it, and a game camera that also pitches would
            // tilt the horizon away from the room's.
            gFirstPersonCamera.forcePitch = true;
            gFirstPersonCamera.forceYaw = true;
            gFirstPersonCamera.pitch = 0;
            // Plus half a turn: the first-person camera sits at Mario's head
            // and looks back through its focus, so its yaw runs opposite to his
            // facing. coopdx says the same at each of its own centring points.
            sSnapYaw = (short) (m->faceAngle[1] + 0x8000);
            sSnapArmed = true;
            sPrevAction = m->action;
        } else {
            gFirstPersonCamera.forcePitch = false;
            gFirstPersonCamera.forceYaw = false;
        }
    }
    atomic_store(&sAirborne, worldReady && (m->action & ACT_FLAG_AIR) != 0);

    // Cancelled - climbing, a cannon, a dialog - means third person for as long
    // as it lasts. Everything downstream reads this one flag.
    atomic_store(&sFirstPersonActive, sFirstPersonOn && get_first_person_enabled());
    if (!sFirstPersonOn) { return; }

    // Snap turn. Continuous turning from a stick is the other classic way to
    // make someone ill - the world moves while the inner ear says it did not.
    // One step per flick, and the stick has to come back to centre before the
    // next, so a held stick does not spin.
    const short x = m->controller ? m->controller->extStickX : 0;
    if (sSnapArmed && x > 40) {
        sSnapYaw -= MARIOVISION_SNAP_STEP;
        sSnapArmed = false;
    } else if (sSnapArmed && x < -40) {
        sSnapYaw += MARIOVISION_SNAP_STEP;
        sSnapArmed = false;
    } else if (x > -20 && x < 20) {
        sSnapArmed = true;
    }

    // Out of a cannon you are looking wherever you happened to be looking, which
    // is almost never where you are about to fly. Turn the world under the
    // viewer so that their present line of sight becomes the launch direction -
    // solving for the snap heading rather than setting it, because the head is
    // not ours to move.
    if (m->action == ACT_SHOT_FROM_CANNON && sPrevAction != ACT_SHOT_FROM_CANNON) {
        sSnapYaw = (short) (m->faceAngle[1] + 0x8000 - head_yaw_angle());
    }

    // The view is brought back onto Mario at exactly two moments, and both are
    // edges rather than stretches of time.
    //
    // What was tried and taken out again: following him through every cutscene
    // frame by frame, and holding the view for a fixed count after a level or
    // area change. The first made the world spin with him while he collected a
    // star; the second caught doors, where his heading is mid-turn and was fine
    // left alone. Both moved the view while the player was watching.
    bool realign = false;

    // One: the frame a cutscene ends. Spawning into a level and flying out of a
    // painting are both cutscenes, and only when one finishes is his facing
    // settled - which is why following him during it did not help. Leaving a
    // painting he faces away from it, so this is also the "opposite of the
    // frame" the room asks for, without having to know where the frame is.
    const bool cutsceneNow = (m->action & ACT_GROUP_MASK) == ACT_GROUP_CUTSCENE;
    const bool cutsceneBefore = (sPrevAction & ACT_GROUP_MASK) == ACT_GROUP_CUTSCENE;
    if (cutsceneBefore && !cutsceneNow) { realign = true; }

    // Two: first person coming back after being taken away. Letting go of a
    // tree is the case that shows it - the climb ran in third person, and he
    // leaves it facing where the jump goes rather than where the heading was.
    const bool activeNow = atomic_load(&sFirstPersonActive);
    if (activeNow && !sPrevActive) { realign = true; }
    sPrevActive = activeNow;

    // Three: a wall kick. He leaves the wall facing away from it, and the view
    // does not - so one is thrown backwards down the corridor while still
    // looking at the bricks one just left. The next wall arrives behind the
    // head, and the kick that would carry on up between two walls cannot be
    // aimed at all. This is the one moment where the game turns him without the
    // viewer having asked, so it is the one moment the view has to follow.
    if (m->action == ACT_WALL_KICK_AIR && sPrevAction != ACT_WALL_KICK_AIR) {
        realign = true;
    }

    // Four: catching a ledge. One arrives at it from wherever one happened to
    // be looking - sideways, or over one's shoulder mid-jump - and then hangs
    // there facing a wall that is off to one side of the view. Everything that
    // matters next is at the ledge: what is above it, and whether to pull up.
    // He is already turned into it, so the view only has to follow him.
    if (m->action == ACT_LEDGE_GRAB && sPrevAction != ACT_LEDGE_GRAB) {
        realign = true;
    }

    if (realign) {
        sSnapYaw = (short) (m->faceAngle[1] + 0x8000 - head_yaw_angle());
    }

    // Turn his body to face where the viewer is looking.
    //
    // Anything the game measures against his facing rather than against the
    // stick goes wrong otherwise. The backflip shows it plainly: crouch, look
    // over your shoulder, jump - it drives him at forwardVel = -16 along his
    // facing, so with the body still pointing the old way he leaps forwards
    // past you.
    //
    // Minus half a turn, and that is the whole of the first attempt's failure:
    // the yaw here is a camera heading, and coopdx's own line reads
    // faceAngle = camera yaw - 0x8000. Set without it he ended up turned
    // exactly backwards, which left the backflip looking untouched.
    short lookYaw = 0;
    if (mariovision_input_yaw(&lookYaw)) {
        const short facing = (short) (lookYaw - 0x8000);

        // Standing still, and swimming at any time.
        //
        // Swimming needs it because update_swimming_yaw never looks at
        // intendedYaw at all - it turns him by an angular velocity taken from
        // the stick. The head therefore had no say in where he swam, and
        // coopdx's own coupling, which used to give a first-person player that
        // say, is the one this mode switched off to stop the view being
        // dragged around. Setting the facing here gives it back, and only in
        // the direction that is wanted: he follows the view, never the reverse.
        const bool stationary = (m->action & ACT_FLAG_STATIONARY) && !(m->action & ACT_FLAG_AIR);
        const bool swimming = (m->action & ACT_FLAG_SWIMMING) != 0;

        // Except with Bowser by the tail. That fight is won by winding him up:
        // the stick builds an angular velocity that is added to Mario's facing
        // each frame, and the speed he reaches decides how far he flies. Writing
        // the facing here overwrote that every frame, so he could not be swung
        // at all. The view stays put regardless - it follows the snap heading,
        // never Mario - which is exactly what is wanted while spinning.
        const bool holdingBowser = m->action == ACT_HOLDING_BOWSER
                                   || m->action == ACT_RELEASING_BOWSER;

        if ((stationary || swimming) && !holdingBowser
            && (m->action & ACT_GROUP_MASK) != ACT_GROUP_CUTSCENE) {
            m->faceAngle[1] = facing;
            if (m->marioObj) { m->marioObj->header.gfx.angle[1] = facing; }
        }
    }

    // Looking up at the sun, which is how the tower above the castle is
    // entered.
    //
    // coopdx checks its own camera's pitch for this, and in this mode that
    // pitch is deliberately held at zero - the looking around happens in the
    // head and in the view matrix, never in the game camera, or the world would
    // tilt with the viewer. So the game never saw anybody look up, and the
    // whole level was unreachable. The same test is made here instead, from the
    // head's own pitch.
    //
    // Not at the same angle, though, and that is why the first attempt at this
    // still did nothing. coopdx wants 88 degrees, which is what its camera
    // reaches the instant one holds the stick up - it is the stop, not a
    // measurement. A neck does not go there. The game's other route into this
    // level, the one for somebody standing still, asks for 34 degrees, so that
    // is the sort of number the sun actually needs; 45 is taken here, comfortably
    // above where one looks by accident and comfortably below where one cannot.
    //
    // And held for two thirds of a second. A threshold a head crosses in passing
    // would send one to the tower for glancing at the ceiling.
    //
    // Upwards is positive here, and the first cut had it the other way about,
    // so looking at one's feet opened the tower. The engine keeps two
    // conventions for this one axis: the camera's pitch counts upwards as
    // negative, which is where the number this test was copied from came from,
    // and Mario's own faceAngle counts it as positive, which is what
    // mariovision_input_pitch was made to speak so that swimming would work.
    // Reading one in the other's units gets the sign exactly backwards.
    {
        short headPitch = 0;
        const bool onTheSpot =
            mariovision_input_pitch(&headPitch)
            && m->floor && m->floor->type == SURFACE_LOOK_UP_WARP
            && m->forwardVel < 4.0f
            && save_file_get_total_star_count(gCurrSaveFileNum - 1,
                                              COURSE_MIN - 1, COURSE_MAX - 1)
                   >= gLevelValues.wingCapLookUpReq;

        if (onTheSpot && headPitch >= 0x2000) {
            if (++sLookUpHeld >= 20) {
                sLookUpHeld = 0;
                level_trigger_warp(m, WARP_OP_LOOK_UP);
            }
        } else {
            sLookUpHeld = 0;
        }

        // What the head is actually doing, said out loud on the one square of
        // floor where it matters. Twice now this has been built blind; if 45
        // degrees is still the wrong number, or the sign is, this says so in one
        // glance instead of another round of guessing.
        if (onTheSpot && mariovision_announce() && (gGlobalTimer % 30) == 0) {
            static char said[64];
            snprintf(said, sizeof said, "Looking up %d deg (want 45)",
                     (int) (headPitch * 360 / 65536));
            mariovision_announce_gesture(said);
        }
    }

    walk_with_the_viewer(m);

    sPrevAction = m->action;
    gFirstPersonCamera.pitch = 0;
    // Upside down at the top of a pole.
    //
    // At once, and deliberately not over a second and a half as it was first
    // built. Turning the whole world about the line of sight is the surest way
    // there is to make somebody ill, and the length of the turn is what does it:
    // for as long as it lasts, the eyes report a rotation the inner ear cannot
    // feel. A cut reports nothing at all. It is the same reason the turning here
    // is done in steps rather than by holding a stick over.
    {
        extern bool mariovision_hand_climb(void);
        atomic_store(&sFirstPersonRoll,
                     (mariovision_hand_climb()
                      && (m->action == ACT_TOP_OF_POLE
                          || m->action == ACT_TOP_OF_POLE_JUMP)) ? 3.14159265f : 0.0f);


    }

    gFirstPersonCamera.yaw = sSnapYaw;
}

bool mariovision_portal_active(void) {
    return atomic_load(&sActive);
}

void mariovision_portal_set_active(bool active) {
    atomic_store(&sActive, active);
    if (!active) {
        pthread_mutex_lock(&sListMutex);
        sDisplayList = NULL;
        pthread_mutex_unlock(&sListMutex);
    }
}

//-----------------------------------------------------------------------------
// Handoff
//-----------------------------------------------------------------------------

// The game thread builds the display list into a single pool while the
// compositor thread replays the last published one - out of the same memory,
// several times per game frame. Whoever loses that race reads a list that is
// half rewritten: geometry gone in whole quads, and now and then a wild pointer
// in gfx_sp_vertex.
//
// Both sides take this before touching the pool, and both take it before
// sListMutex, so the order is always the same and there is nothing to deadlock
// on.
static pthread_mutex_t sPoolMutex = PTHREAD_MUTEX_INITIALIZER;

void mariovision_portal_lock_pool(void) {
    pthread_mutex_lock(&sPoolMutex);
}

void mariovision_portal_unlock_pool(void) {
    pthread_mutex_unlock(&sPoolMutex);
}

bool mariovision_portal_submit(void *displayList) {
    if (!atomic_load(&sActive)) { return false; }

    pthread_mutex_lock(&sListMutex);
    sDisplayList = displayList;
    pthread_mutex_unlock(&sListMutex);
    return true;
}

//-----------------------------------------------------------------------------
// Render thread
//-----------------------------------------------------------------------------

void mariovision_portal_set_window(float halfWidth, float halfHeight) {
    sHalf[0] = halfWidth;
    sHalf[1] = halfHeight;
}

void mariovision_portal_get_window(float *halfWidth, float *halfHeight) {
    if (halfWidth)  { *halfWidth = sHalf[0]; }
    if (halfHeight) { *halfHeight = sHalf[1]; }
}

void mariovision_portal_set_tuning(float stereoScale, float distance, float worldScale,
                                   float convergence, float fovScale) {
    atomic_store(&sStereoScale, stereoScale);
    atomic_store(&sDistance, distance);
    atomic_store(&sWorldScale, worldScale);
    atomic_store(&sConvergence, convergence);
    atomic_store(&sFovScale, fovScale);
}

void mariovision_portal_get_tuning(float *stereoScale, float *distance, float *worldScale,
                                   float *convergence, float *fovScale) {
    if (stereoScale) { *stereoScale = atomic_load(&sStereoScale); }
    if (distance)    { *distance = atomic_load(&sDistance); }
    if (worldScale)  { *worldScale = atomic_load(&sWorldScale); }
    if (convergence) { *convergence = atomic_load(&sConvergence); }
    if (fovScale)    { *fovScale = atomic_load(&sFovScale); }
}

void mariovision_portal_set_eye(const float q[16], const float glass[16],
                                const float clipToPortal[16], const float eyePortal[3]) {
    to_row_layout(sQRow, q);
    to_row_layout(sGlassRow, glass);
    memcpy(sClipToPortalCol, clipToPortal, sizeof(sClipToPortalCol));
    memcpy(sEyePortal, eyePortal, sizeof(sEyePortal));
}

// The list both eyes of the current frame replay. Latched once so the pair is
// two views of the same moment, and so the game cannot rebuild the pool between
// them - with a single pool, a fetch per eye means differing content as well as
// a differing frame.
static void *sFrameList = NULL;

void mariovision_portal_begin_frame(void) {
    atomic_store(&sRendererBusy, true);
    mariovision_portal_lock_pool();
    pthread_mutex_lock(&sListMutex);
    sFrameList = sDisplayList;
    pthread_mutex_unlock(&sListMutex);
}

void mariovision_portal_end_frame(void) {
    sFrameList = NULL;
    mariovision_portal_unlock_pool();
    atomic_store(&sRendererBusy, false);
}

bool mariovision_portal_render_eye(void) {
    void *list = sFrameList;
    if (list == NULL) { return false; }

    static int sTraceFrames = 0;
    sSeqCapturing = trace_enabled() && ((sTraceFrames++ % 180) == 0);
    sSeqLen = 0;
    sSeq[0] = '\0';

    sAllTris = sAllRejected = 0;

    // gfx_run drives the whole gfx state machine; the pass flag makes the
    // window backend's start_frame/swap hooks yield to the portal's encoder.
    atomic_store(&sPassActive, true);
    gfx_run((Gfx *) list);
    gfx_end_frame_render();
    atomic_store(&sPassActive, false);

    if (sSeqCapturing) {
        sSeqCapturing = false;
        MV_LOG("projections: %s", sSeqLen ? sSeq : "(none)");
    }

    // TEMP diagnostics: once a second, report what the override saw.
    static int sDbgFrames = 0;
    if (++sDbgFrames % 180 == 0) {
        extern _Atomic long gMVDebugTris, gMVDebugDraws;
        MV_LOG("persp=%d ortho=%d tris=%ld draws=%ld clipped=%d/%d eye=(%.2f %.2f %.2f)",
                atomic_exchange(&sDbgPersp, 0), atomic_exchange(&sDbgOrtho, 0),
                atomic_exchange(&gMVDebugTris, 0), atomic_exchange(&gMVDebugDraws, 0),
                sAllRejected, sAllTris,
                (double)sEyePortal[0], (double)sEyePortal[1], (double)sEyePortal[2]);
    }
    return true;
}

bool mariovision_portal_pass_active(void) {
    return atomic_load(&sPassActive);
}

bool mariovision_portal_screen_pass(void) {
    return atomic_load(&sScreenPass);
}

void mariovision_portal_set_screen_eye(float shiftX, float focusZ) {
    atomic_store(&sScreenShiftX, shiftX);
    atomic_store(&sScreenFocusZ, focusZ);
}

bool mariovision_portal_render_screen_eye(void) {
    void *list = sFrameList;
    if (list == NULL) { return false; }

    atomic_store(&sScreenPass, true);
    atomic_store(&sPassActive, true);
    gfx_run((Gfx *) list);
    gfx_end_frame_render();
    atomic_store(&sPassActive, false);
    atomic_store(&sScreenPass, false);
    return true;
}


// TEMP diagnostics: MARIOVISION_VTXLOG=1 reports where ortho content - the
// skybox above all - actually lands, so the placement can be read off numbers
// instead of guessed from screenshots. NDC x and y should span the full
// [-1, 1] for a skybox that fills the window.
static float sSkyMinX, sSkyMaxX, sSkyMinY, sSkyMaxY, sSkyMinW, sSkyMaxW;
static int sSkyVerts, sSkyTris, sSkyRejected;

static void sky_reset(void) {
    sSkyMinX = sSkyMinY = sSkyMinW = 1e9f;
    sSkyMaxX = sSkyMaxY = sSkyMaxW = -1e9f;
    sSkyVerts = sSkyTris = sSkyRejected = 0;
}

/// Called when the next projection LOAD ends the skybox's matrix, so the
/// accumulated extent covers its tiles and nothing else.
static void sky_report(void) {
    static int sEnabled = -1;
    if (sEnabled < 0) { sEnabled = (getenv("MARIOVISION_VTXLOG") != NULL); }
    if (sEnabled == 0 || sSkyVerts == 0) { return; }

    static int sFrames = 0;
    if ((sFrames++ % 120) != 0) { return; }

    // On the glass, w is the 1.201 m eye-to-glass distance and x/y are metres
    // in the glass plane; the window is +/-0.500 by +/-0.375. A w far from
    // 1.201 means the skybox never reached the glass matrix at all.
    MV_LOG("skybox %d verts x=[%.3f %.3f] y=[%.3f %.3f] w=[%.3f %.3f] rejected=%d/%d tris",
                 sSkyVerts, (double)sSkyMinX, (double)sSkyMaxX,
                 (double)sSkyMinY, (double)sSkyMaxY,
                 (double)sSkyMinW, (double)sSkyMaxW,
                 sSkyRejected, sSkyTris);
}

/// gfx_sp_tri1 drops a triangle when all three vertices fail the same clip
/// plane. In the portal that boundary is |x| > w with w the 1.2 m glass
/// distance, while the window is only +/-0.5 m wide - so whole tiles can go.
void mariovision_portal_note_triangle(bool rejected) {
    if (!atomic_load(&sPassActive)) { return; }

    // Everything in the pass, not just the sky: the report from the device is
    // that distant geometry disappears in whole quads - water as much as sky -
    // while anything inside the level stays. That is the signature of triangles
    // failing a clip plane, so the interesting number is how many.
    sAllTris++;
    if (rejected) { sAllRejected++; }

    if (!atomic_load(&sSkyboxNow)) { return; }
    sSkyTris++;
    if (rejected) { sSkyRejected++; }
}

void mariovision_portal_debug_vertex(float x, float y, float z, float w) {
    // This sits in the per-vertex loop, so the env check comes first and the
    // whole thing costs one predictable branch when the logging is off.
    static int sEnabled = -1;
    if (sEnabled < 0) { sEnabled = (getenv("MARIOVISION_VTXLOG") != NULL); }
    if (sEnabled == 0) { return; }
    if (!atomic_load(&sPassActive) || !atomic_load(&sSkyboxNow)) { return; }

    if (x < sSkyMinX) { sSkyMinX = x; }
    if (x > sSkyMaxX) { sSkyMaxX = x; }
    if (y < sSkyMinY) { sSkyMinY = y; }
    if (y > sSkyMaxY) { sSkyMaxY = y; }
    if (w < sSkyMinW) { sSkyMinW = w; }
    if (w > sSkyMaxW) { sSkyMaxW = w; }
    sSkyVerts++;
}

void mariovision_portal_pace_steps(int steps) {
    static struct timespec sNext = { 0, 0 };
    if (steps < 1) { steps = 1; }
    const long period = 1000000000L / (30 * steps);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (sNext.tv_sec == 0 ||
        (sNext.tv_sec < now.tv_sec - 1)) { // stale after a pause - resync
        sNext = now;
    }

    sNext.tv_nsec += period;
    if (sNext.tv_nsec >= 1000000000L) { sNext.tv_nsec -= 1000000000L; sNext.tv_sec++; }

    if (sNext.tv_sec > now.tv_sec ||
        (sNext.tv_sec == now.tv_sec && sNext.tv_nsec > now.tv_nsec)) {
        struct timespec rest = { sNext.tv_sec - now.tv_sec, sNext.tv_nsec - now.tv_nsec };
        if (rest.tv_nsec < 0) { rest.tv_nsec += 1000000000L; rest.tv_sec--; }
        nanosleep(&rest, NULL);
    }
}

void mariovision_portal_pace(void) {
    mariovision_portal_pace_steps(1);
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------

void mariovision_portal_override_projection(float m[4][4]) {
    if (!atomic_load(&sActive) || !atomic_load(&sPassActive)) {
        if (atomic_load(&sActive)) { atomic_fetch_add(&sDbgSkipped, 1); }
        return;
    }
    seq_note("LOAD", (const float (*)[4]) m);

    if (atomic_load(&sScreenPass)) {
        // The 3D window keeps the game's own projection - that is the whole
        // point, the framing must stay the game's. Only the camera slides along
        // its x axis, and in row-vector layout that is the last row picking up
        // a multiple of the first. It happens before the projection, which is
        // why this works as a plain matrix where laying one over the finished
        // projection did not.
        if (m[3][3] == 0.0f) {
            const float d = atomic_load(&sScreenShiftX);
            const float p00 = m[0][0];

            // Move the camera sideways.
            for (int j = 0; j < 4; j++) { m[3][j] += d * m[0][j]; }

            // Then slide the frustum back by what that movement does at the
            // focus distance, so the two pictures coincide there. Without this
            // the whole world sits in front of the screen and the horizon
            // diverges. Adding a multiple of the w row to the x row is a
            // constant shift in normalised device coordinates.
            const float focus = atomic_load(&sScreenFocusZ);
            if (focus > 1.0f) {
                const float shift = p00 * d / focus;
                for (int i = 0; i < 4; i++) { m[i][0] -= shift * m[i][3]; }
            }
        }
        return;
    }

    const bool ortho = (m[3][3] != 0.0f);
    atomic_store(&sOrthoNow, ortho);

    // The skybox announces itself with an all-zero matrix (see skybox.c). Two
    // heuristics came before this and both were wrong: an identity projection
    // is also loaded by five HUD layers, and "whatever comes before the
    // perspective" fails because a pass does not always contain exactly one.
    const bool skybox = (m[0][0] == 0.0f && m[1][1] == 0.0f &&
                         m[2][2] == 0.0f && m[3][3] == 0.0f);

    if (atomic_exchange(&sSkyboxNow, skybox)) { sky_report(); }
    if (skybox) { sky_reset(); }
    if (ortho) { atomic_fetch_add(&sDbgOrtho, 1); }
    else       { atomic_fetch_add(&sDbgPersp, 1); }

    if (m[3][3] == 0.0f) {
        // Perspective - and the skybox marker, which is deliberately shaped to
        // land here. Replaced wholesale: the game camera's view enters through
        // the modelview, the eye takes over from there. The skybox's tiles are
        // already points in that same camera space, so they need exactly this
        // and nothing more.
        memcpy(m, sQRow, sizeof(sQRow));
    } else {
        // Ortho (HUD, menus, skybox tiles): keep the game's own 2D mapping and
        // append the glass transform, which lays the 320x240 box onto the
        // window quad, a millimetre behind the glass so the portal clip test
        // trims it at the window edges instead of discarding it.
        float original[4][4];
        memcpy(original, m, sizeof(original));
        // Row-vector composition: v·(original·glass).
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                float acc = 0.0f;
                for (int k = 0; k < 4; k++) {
                    acc += original[i][k] * sGlassRow[k][j];
                }
                m[i][j] = acc;
            }
        }
    }
}

/// The same three values, without requiring the game's render pass to be under
/// way. The review scene draws in place of the game and therefore never opens
/// that pass - but it wants the identical window trim, or the portal would be
/// indistinguishable from full VR.
bool mariovision_portal_eye_params(float clipToPortal[16], float eyePortal[3],
                                   float half[2]) {
    if (!atomic_load(&sActive)) { return false; }
    memcpy(clipToPortal, sClipToPortalCol, sizeof(sClipToPortalCol));
    memcpy(eyePortal, sEyePortal, sizeof(sEyePortal));
    memcpy(half, sHalf, sizeof(sHalf));
    return true;
}

bool mariovision_portal_shader_params(float clipToPortal[16], float eyePortal[3],
                                      float half[2]) {
    if (!atomic_load(&sPassActive)) { return false; }

    memcpy(clipToPortal, sClipToPortalCol, sizeof(sClipToPortalCol));
    memcpy(eyePortal, sEyePortal, sizeof(sEyePortal));
    memcpy(half, sHalf, sizeof(sHalf));
    return true;
}
