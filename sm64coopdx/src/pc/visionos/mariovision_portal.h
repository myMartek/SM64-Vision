/*
 * Portal mode: the game world behind a virtual window, rendered in stereo
 * through CompositorServices.
 *
 * The trick (see the plan's phase 2): the game keeps its own camera, and only
 * the projection matrix is swapped. The game's modelview brings vertices into
 * camera space; instead of the game's perspective matrix, a per-eye matrix
 *
 *     Q_e = P_e · V_e · M_place · S
 *
 * takes them from there through the portal placement into the viewer's eye.
 * The window glass sits at the game camera's origin, so looking through the
 * window is looking through Lakitu's lens - with real parallax, because P_e
 * and V_e come from the headset each frame.
 *
 * Threads: the game thread keeps running its loop at 30 Hz and publishes its
 * display list here instead of rendering it. The compositor render thread
 * picks up the latest list and runs it through gfx_run once per eye at the
 * display rate, each time with fresh eye matrices - head motion is smooth at
 * 90 Hz even though the game simulates at 30.
 *
 * The publish/consume handoff is a pointer swap under a mutex. The buffer
 * behind it is coopdx's double-buffered gfx pool: the game overwrites a pool
 * roughly 66 ms after publishing it, by which time the renderer has long moved
 * to the newer list. A renderer stall beyond that window would read a list
 * mid-overwrite and show one glitched frame; accepted for now.
 */

#ifndef MARIOVISION_PORTAL_H
#define MARIOVISION_PORTAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//-----------------------------------------------------------------------------
// Mode switch (main thread or render thread)
//-----------------------------------------------------------------------------

/// How the game is presented. The first lives in a flat window; the other three
/// all run in the immersive space and differ only in where the game's camera is
/// put and whether the window frame trims the result.
typedef enum {
    MARIOVISION_RENDER_WINDOW_2D = 0, ///< flat window, no stereo
    MARIOVISION_RENDER_WINDOW_3D = 1, ///< stereo, camera at the viewer - a 3D screen
    MARIOVISION_RENDER_PORTAL    = 2, ///< stereo, camera at the glass - a window into the world
    MARIOVISION_RENDER_VR        = 3, ///< as the portal, but nothing trims it at the frame
    MARIOVISION_RENDER_FIRST_PERSON = 4, ///< standing in Mario's shoes, looking around freely
} MarioVisionRenderMode;

int  mariovision_render_mode(void);
void mariovision_set_render_mode(int mode);

/// True while the immersive space owns the rendering - every mode but 2D.
bool mariovision_portal_active(void);
void mariovision_portal_set_active(bool active);

/// True when the window frame must not trim the image (VR mode).
bool mariovision_portal_unclipped(void);

//-----------------------------------------------------------------------------
// First-person VR
//
// The viewer stands where Mario stands. Two rules keep it comfortable:
//
//  - The game camera holds still. Its yaw is the snap heading and only ever
//    moves in 30 degree steps, its pitch is nailed to zero. Everything else -
//    every degree of looking around - is applied by the renderer from the head
//    pose, at display rate. A view that waits for the 30 Hz simulation to turn
//    is the classic way to make someone ill.
//  - Mario still walks where you look, which means the direction his stick
//    input is measured against is the snap heading plus where the head is
//    turned. That is a different angle from the one the world is drawn with,
//    and it is the only place the head pose reaches the simulation.
//-----------------------------------------------------------------------------

/// Head yaw and pitch in the room, in radians, pushed by the renderer each
/// frame. Yaw is measured about the room's up axis from the heading the mode
/// was entered at.
void mariovision_set_head_pose(float yaw, float pitch);

/// The viewer's eyes expressed in the game camera's space and units, pushed by
/// the renderer each frame. Feeds mariovision_billboard_eye.
void mariovision_set_eye_in_camera_space(float x, float y, float z);

/// Called once per game frame, before the simulation runs: turns coopdx's own
/// first-person camera on and off with the mode, holds its pitch and yaw, and
/// reads the right stick for the snap turn.
void mariovision_first_person_update(void);

/// The heading Mario's stick input is measured against - the snap heading plus
/// the head. False when this mode is off, leaving the game's own chain alone.
bool mariovision_input_yaw(short *out);

/// Where the viewer is looking vertically, as a game angle, for swimming. The
/// game camera's own pitch is held level so the horizon does not tilt, which
/// leaves this as the only way up. False when this mode is off.
bool mariovision_input_pitch(short *out);

/// Where the viewer's eyes are, in the game camera's space and its units, so a
/// billboard can turn to point at them. False when this mode is off.
///
/// A flat game gets away with laying every billboard parallel to the image
/// plane: across 45 degrees of screen that is indistinguishable from facing the
/// viewer. Across a headset's 100-plus it is not - a sprite half way to the
/// edge is then plainly seen at an angle. Each quad has to be aimed at the eye
/// on its own, which needs the eye as a position rather than a direction.
///
/// It also removes the head's rotation from the problem entirely: turning your
/// head does not move your eyes, so nothing here has to keep up with it at
/// display rate. Only actually walking or leaning changes these numbers, and
/// slowly.
bool mariovision_billboard_eye(float out[3]);

/// How far Mario was carried by the viewer walking, since this was last asked,
/// in camera space and game units.
///
/// The renderer moves its anchor by the same amount, so the head measured
/// against that anchor is back where it started. Without that the movement is
/// applied twice - once to Mario and once to the view - and one leaves one's
/// own body at speed.
bool mariovision_take_walk(float out[2]);

/// How far the viewer's head moved in the room since the last frame, expressed
/// in the anchor's own directions and in game units.
///
/// Measured in the room on purpose. The obvious source - the eye's position
/// relative to the anchor - is poisoned: moving the anchor changes it even
/// though the head has not moved, so the next frame measures a step that never
/// happened and corrects it back. That is a loop, and it showed as the world
/// and the hands shivering.
void mariovision_set_walk_delta(float dx, float dz);

/// True while the viewer is actually standing in Mario's shoes - the mode is
/// selected and nothing has taken it away for the moment. Climbing and cannons
/// hand the view back to third person, where you can see what you are doing.
bool mariovision_first_person_active(void);

/// True while Mario is off the ground.
bool mariovision_first_person_airborne(void);

/// How far the world is turned about the line of sight, in radians. Half a turn
/// while Mario is standing on his hands at the top of a pole; nothing otherwise.
float mariovision_first_person_roll(void);

/// Bracket what render_hud draws. Between these two calls mariovision_hud_place
/// moves coordinates; outside them it does nothing, so dialogs, the pause screen
/// and the room-change animations keep the full pane.
void mariovision_hud_layout_begin(void);
void mariovision_hud_layout_end(void);

/// The contraction that brings the readouts in from the pane's far corners into
/// the field of view: how much of the distance from the centre is kept, and how
/// far down afterwards. False outside the VR modes and outside the bracket.
/// _ortho is in the game's 320x240 space, where y counts upwards; _rect takes a
/// texture rectangle's origin and size in screen space, where it counts down.
bool mariovision_hud_ortho(float *scale, float *drop);

/// The same, without requiring the HUD bracket to be open - for the flat things
/// drawn outside it, such as dialogue boxes.
bool mariovision_hud_inset(float *scale, float *drop);

/// How far past the screen a surface meant to cover it has to go. One outside
/// VR; several times that inside it, where the screen is a pane in a room.
float mariovision_hud_overscan(void);

/// The colour the whole view is to be wiped with, if any was named this frame.
void mariovision_set_backdrop(unsigned char r, unsigned char g,
                              unsigned char b, unsigned char a);
bool mariovision_backdrop(float out[4]);
bool mariovision_hud_rect(float *x, float *y, float *w, float *h);

/// True while the viewer is inside Mario's head: his model must go, his shadow
/// must stay. GRAPH_RENDER_INVISIBLE cannot express that - it drops the whole
/// subtree, shadow included.
bool mariovision_hide_mario_body(void);

/// The 3D-window mode renders the scene the way the flat window does - the
/// game's own projection, its own skybox, its own HUD placement - only into a
/// texture of ours, with the camera shifted sideways by half the eye
/// separation. That texture is then drawn onto the pane. Everything the portal
/// normally substitutes has to step aside for that pass, so each of those
/// places asks this.
bool mariovision_portal_screen_pass(void);

/// Shift of the game camera along its x axis, in game units, for the eye being
/// drawn, and the distance at which the two pictures should coincide.
///
/// Moving the cameras apart and leaving it at that puts the whole world in
/// front of the screen and makes the horizon diverge - the eyes are asked to
/// point outwards, which hurts. Shifting each frustum sideways as well places a
/// plane of zero parallax at `focusZ`: nearer things step out of the screen,
/// farther ones sit behind it, and infinity settles at a bounded offset instead
/// of running away.
void mariovision_portal_set_screen_eye(float shiftX, float focusZ);

/// Renders one eye of the 3D-window mode into the caller's encoder.
bool mariovision_portal_render_screen_eye(void);

/// True while the compositor thread is inside a frame - between latching the
/// display list and finishing the second eye. The window path must not start a
/// frame of its own then: both drive the same gfx state machine and the same
/// Metal encoder, and switching modes used to put them there at once.
bool mariovision_portal_renderer_busy(void);

//-----------------------------------------------------------------------------
// Game-thread side
//-----------------------------------------------------------------------------

/// Called from send_display_list. Returns true when portal mode consumed the
/// list (the game thread must then not render it itself).
bool mariovision_portal_submit(void *displayList);

/// Paces the game loop at 30 Hz while the portal renders. In window mode the
/// pacing comes from nextDrawable; in portal mode the game thread never touches
/// a drawable, so it sleeps here instead.
void mariovision_portal_pace(void);

/// Same, but for one of `steps` slices of a game frame - the pacing for the
/// interpolated frames between two simulation steps.
void mariovision_portal_pace_steps(int steps);

//-----------------------------------------------------------------------------
// Render-thread side
//-----------------------------------------------------------------------------

/// Per-eye matrices, all column-major float16 (simd layout):
///  q          - replaces the game's perspective projection (camera -> clip)
///  glass      - P·V·M_place·D: maps ortho NDC onto the window quad, used for
///               HUD/menu draws (their own ortho matrix is multiplied in front)
///  clipToPortal - inverse(P·V·M_place): reconstructs portal-space positions
///               from clip space for the fragment clip test
///  eyePortal  - eye position in portal space (x, y, z)
void mariovision_portal_set_eye(const float q[16], const float glass[16],
                                const float clipToPortal[16], const float eyePortal[3]);

/// Latches the published display list for the whole frame and takes the pool
/// lock. Both eyes must replay the same list: they are two views of one moment,
/// and letting each fetch its own means a publish landing between them shows
/// the left eye one game frame and the right eye the next. That reads as a
/// vague unease when moving rather than as an obvious glitch.
void mariovision_portal_begin_frame(void);
void mariovision_portal_end_frame(void);

/// Renders the most recently published display list with the matrices set
/// above. Runs on the compositor thread between gfx_metal_begin_frame and
/// gfx_metal_end_frame. Returns false when nothing has been published yet.
bool mariovision_portal_render_eye(void);

/// Window half extents in metres. The clip test and the glass mapping use
/// these; the Swift side keeps them in sync with the placed window.
void mariovision_portal_set_window(float halfWidth, float halfHeight);
void mariovision_portal_get_window(float *halfWidth, float *halfHeight);

//-----------------------------------------------------------------------------
// Live tuning
//
// The three knobs that together decide whether the stereo pair fuses. Written
// from the settings UI on the main thread, read by the render thread every
// frame - plain atomics, no locking, a frame of tearing between them would be
// invisible anyway.
//
//  stereoScale - multiplies the per-eye offset. 1.0 is physically correct.
//  distance    - eye-to-glass distance in metres.
//  worldScale  - game units to metres (1/400 puts Mario at about 40 cm).
//  convergence - horizontal NDC shift, applied to each eye with opposite sign.
//                Positive pulls the two images together. 0 is correct once the
//                projections are; it exists to null out a residual offset by
//                eye and to measure how large that offset is.
//  fovScale    - scales the projection's opening angle. 1.0 is the
//                compositor's own value, i.e. the geometrically correct one.
//-----------------------------------------------------------------------------

void mariovision_portal_set_tuning(float stereoScale, float distance, float worldScale,
                                   float convergence, float fovScale);
void mariovision_portal_get_tuning(float *stereoScale, float *distance, float *worldScale,
                                   float *convergence, float *fovScale);

//-----------------------------------------------------------------------------
// Hooks used by the render backend (game side)
//-----------------------------------------------------------------------------

/// Applied by gfx_sp_matrix after every G_MTX_PROJECTION|G_MTX_LOAD, in place,
/// in gfx_pc's row-vector layout. No-op when portal mode is off.
void mariovision_portal_override_projection(float m[4][4]);

/// Shader parameters for the current eye, fetched by the Metal backend at draw
/// time. `clipToPortal` is column-major for MSL; returns false when the clip
/// test should be off (portal inactive).
/// As above, but without requiring the game's render pass. For the review scene.
bool mariovision_portal_eye_params(float clipToPortal[16], float eyePortal[3],
                                   float half[2]);

bool mariovision_portal_shader_params(float clipToPortal[16], float eyePortal[3],
                                      float half[2]);

/// True while the compositor thread is inside gfx_run - the window backend's
/// start_frame/swap hooks step aside for the portal's own encoder then.
bool mariovision_portal_pass_active(void);

#ifdef __cplusplus
}
#endif

#endif // MARIOVISION_PORTAL_H
