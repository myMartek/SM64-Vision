#ifndef MARIOVISION_HANDS_H
#define MARIOVISION_HANDS_H

//-----------------------------------------------------------------------------
// Tracked controllers in first-person VR.
//
// When PSVR2 Sense controllers are present, Mario's hands go where the physical
// controllers are and his arms are solved to follow. Everything here is inert
// without them: the graph hook returns false and the animation keeps its say,
// so a session with a gamepad runs the code paths it has always run.
//
// Poses live in the game camera's own space and units - the same space
// mariovision_set_eye_in_camera_space publishes into, and for the same reason:
// it is the one frame that is already proven to line up with what the compositor
// draws. x right, y up, and the camera looks along -z, which is also ARKit's
// convention, so nothing is flipped on the way in.
//
// Notably absent from the chain: the snap heading. Turning rotates the world
// underneath the viewer - it lives in the world-to-camera matrix - so camera
// space stays glued to the pose the mode was entered with. A hand published
// here is snap-correct already, and adding the heading would turn twice.
//-----------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MV_HAND_LEFT = 0, MV_HAND_RIGHT = 1, MV_HAND_COUNT = 2 };

enum {
    /// Position and orientation are both live.
    MV_HAND_TRACKED     = 1u << 0,
    /// Orientation only; the position is the last one that was trusted.
    MV_HAND_ORIENT_ONLY = 1u << 1,
    MV_HAND_TRIGGER     = 1u << 2,
    MV_HAND_GRIP        = 1u << 3,
};

/// One controller, in game camera space and N64 units.
typedef struct {
    float pos[3];
    float quat[4];   ///< x, y, z, w
    float vel[3];    ///< units per second
    float trigger;   ///< 0..1, the analogue value
    uint32_t flags;
} MarioVisionHandPose;

//-----------------------------------------------------------------------------
// Transport
//
// Written by the render thread at display rate, read by the game thread at 30
// Hz. A seqlock rather than a field of atomics: position and orientation have
// to agree with each other, and independent atomics would let a reader take the
// position from one sample and the orientation from the next. That tears for a
// single frame and shows as the hand snapping somewhere impossible.
//-----------------------------------------------------------------------------

void mariovision_set_hand_pose(int side, const MarioVisionHandPose *pose);
void mariovision_clear_hand(int side);

/// Once per game frame, before the graph is walked. Moves this frame's sample
/// into the previous slot and takes a fresh one.
///
/// The renderer blends each frame's matrices against the frame before, so the
/// arms need a previous pose that the previous frame was actually built with -
/// a remembered value that disagrees with it reads as shaking rather than as
/// motion. This is the same lesson the held-arm code learned the hard way.
void mariovision_hands_latch(void);

/// True when at least one controller is live. The graph hook checks this
/// itself; callers need it only to choose between control schemes.
bool mariovision_hands_active(void);

bool mariovision_hands_get(int side, bool prevFrame, MarioVisionHandPose *out);

/// The hand's frame as it is actually drawn: the controller's own orientation
/// with the correction between its grip frame and Mario's hand already applied.
///
/// One function rather than the same three lines in two places. They were two,
/// and they disagreed - the renderer drew a corrected hand while the punch
/// detector measured an uncorrected one, so the fist appeared to point exactly
/// backwards and no swing ever counted.
///
/// Row 0 is where the hand points, in game camera space.
bool mariovision_hand_frame(int side, bool prevFrame, float out[3][3]);

/// How far a hand's own reach is stretched before it becomes Mario's. Tunable
/// through MARIOVISION_HANDS_REACH.
float mariovision_hands_reach_scale(void);

//-----------------------------------------------------------------------------
// Fit
//
// Arm length and where the shoulders sit are the two numbers that decide
// whether the arms feel like one's own, and no amount of reasoning settles
// them - only wearing the headset does. They are therefore live settings
// rather than constants: turning a dial while wearing it beats a build, an
// install and a relaunch for every guess.
//-----------------------------------------------------------------------------

/// Game units, a hundred to the metre; factors where noted.
typedef struct {
    float armReach;      ///< shoulder to palm; about 62 for an adult
    /// Thickness of the arm as a factor of what its length implies.
    ///
    /// One keeps the limb evenly scaled, which is the only setting where the
    /// mesh meets itself exactly at the elbow. Above one the joint overlaps,
    /// which is invisible; below one it pulls apart, which is not. So the
    /// slider is honest about going down, but up is the safe direction.
    float armThickness;
    float shoulderHalf;  ///< half the distance between the shoulders
    float shoulderDrop;  ///< how far the shoulders sit below the eyes
    float shoulderBack;  ///< and how far behind them
    float elbowOut;      ///< how far outwards the elbows are pushed
    float elbowBack;     ///< and how far back
    float handScale;     ///< size of the hands, 1 keeps Mario's own
    /// Where the hand sits relative to the controller, in the hand's own
    /// frame - so it turns with the hand. This is what cancels the gap between
    /// the wrist bone and the middle of the glove: set right, twisting the
    /// controller on the spot turns the hand without moving it.
    float handOffset[3];
    /// And where it sits in the room, which does not turn with it. "The hands
    /// are too high" is a statement about the room, not about the hand, and
    /// correcting it with the offset above would bring the arc straight back.
    float handWorldOffset[3];
    float handRotate[3]; ///< pitch, yaw, roll of the hand, in degrees

    /// Show the hands alone, with no arm reaching to them.
    ///
    /// Every millimetre by which a hand misses its controller comes from the
    /// arm: from guessing where a shoulder sits, from a limb that is a little
    /// too short to reach, from an elbow that had to go somewhere. Cut the arm
    /// away and the hand is placed directly - it cannot miss, because nothing
    /// is left to compute. Floating hands are a long-standing solution in VR
    /// rather than a retreat, and they are exact.
    bool handsOnly;
} MarioVisionHandFit;

/// True while only the hands are drawn, so the renderer's visibility filter can
/// let the hand bones through and collapse the arm bones with everything else.
bool mariovision_hands_only(void);

/// What the hands do to the world, once per simulated frame.
///
/// Called from mario.c between the special floors and the interactions:
/// update_mario_inputs clears m->input just before, so anything injected
/// earlier is wiped, and mario_process_interactions has to still be ahead so it
/// sees what was set.
struct MarioState;
void mariovision_hands_tick(struct MarioState *m);

/// Whether doors are being opened by hand at the moment.
///
/// Asked by interact_door, which otherwise opens one by being walked into - and
/// then the same door has two ways of opening, of which the easier wins and the
/// arm might as well not be there.
bool mariovision_hand_doors_active(void);

/// Whether this graph node is a door being swung the opposite way round.
///
/// Both of the game's door animations turn the leaf the same way - measured in
/// the animation data, they differ only in how briskly. So a door that is to
/// open towards somebody standing on the other side cannot be had by picking
/// the other animation; the rotation has to be turned round as it is read.
bool mariovision_door_swings_back(const void *gfxNode);

//-----------------------------------------------------------------------------
// How each thing is done
//
// Three of these actions exist twice over now - once as a movement of the body
// and once as a button - and which one counts is a matter of taste rather than
// of correctness. Somebody who plays sitting down wants the buttons; somebody
// standing wants neither of them to fire by accident.
//-----------------------------------------------------------------------------

enum {
    MV_DO_SWIM = 0,
    MV_DO_PUNCH = 1,
    MV_DO_CARRY = 2,
    MV_DO_COUNT = 3,
};

enum {
    MV_SCHEME_PHYSICAL = 0,
    MV_SCHEME_BUTTON   = 1,
    MV_SCHEME_BOTH     = 2,
};

/// Say out loud what was just recognised, in the game's own message banner.
///
/// A gesture that does nothing and a gesture that was never seen look exactly
/// alike from inside a headset, and no amount of describing the difference
/// helps. This makes them distinguishable.
void mariovision_set_announce(bool on);
bool mariovision_announce(void);
void mariovision_announce_gesture(const char *what);

void mariovision_set_scheme(int what, int scheme);
int mariovision_scheme(int what);

/// Whether doors are opened by hand at all. Off, and they open by being walked
/// into, exactly as the game has always had it.
void mariovision_set_hand_doors(bool on);

/// Whether poles and trees are climbed by taking hold of them.
///
/// Off, and Mario grabs one by jumping into it and climbs it with the stick, as
/// he always has.
/// How hanging from a ledge is handled.
enum {
    MV_LEDGE_FIRST_PERSON = 0,  ///< Stay inside his head and let the game climb.
    MV_LEDGE_THIRD_PERSON = 1,  ///< Step outside for the duration, as before.
    MV_LEDGE_BY_HAND      = 2,  ///< Hold on yourself, shuffle along, pull up.
};
void mariovision_set_ledge_mode(int mode);
int mariovision_ledge_mode(void);

/// How hanging from a ceiling is handled. Same shape as the ledge, and the
/// third entry is deliberately left free for doing it by hand later.
enum {
    MV_HANG_FIRST_PERSON = 0,
    MV_HANG_THIRD_PERSON = 1,
};
void mariovision_set_hang_mode(int mode);
int mariovision_hang_mode(void);

/// Whether a ledge may only be caught with a hand on it, and whether one is.
bool mariovision_ledge_wants_hand(void);
bool mariovision_hand_gripping(void);

void mariovision_set_hand_climb(bool on);
bool mariovision_hand_climb(void);

/// Whether this hand has hold of a pole or a tree.
bool mariovision_pole_in_hand(int side);

/// How far the view has been shifted out of a tree trunk, in game units.
/// Zero when not hanging on one.
void mariovision_pole_eye_offset(float out[3]);

/// Whether flying stays inside Mario's head. Off, and a wing cap puts the view
/// behind him, which is what the game does on a screen.
void mariovision_set_fly_first_person(bool on);
bool mariovision_fly_first_person(void);

/// Whether the body may do this one.
bool mariovision_scheme_physical(int what);
/// And whether a button may.
bool mariovision_scheme_button(int what);

/// The trigger, pushed in from the controller backend each frame. Grabbing is
/// the one thing the hands do that needs a button, and it needs the analogue
/// value rather than a binding, so it does not go through the pad at all.
void mariovision_hands_set_trigger(int side, float value);
float mariovision_hands_trigger(int side);

//-----------------------------------------------------------------------------
// Haptics
//
// Per hand rather than per pair: feeling a hit in the hand that did not throw
// it is worse than feeling nothing at all.
//-----------------------------------------------------------------------------

void mariovision_hand_haptic(int side, float intensity, float sharpness);
bool mariovision_hand_haptic_available(int side);

void mariovision_hands_get_fit(MarioVisionHandFit *out);

//-----------------------------------------------------------------------------
// Tracing
//
// MARIOVISION_HANDLOG is a bitmask over the stages, so one variable covers the
// whole chain and a stage can be watched without the others drowning it. Output
// goes to stderr: os_log is invisible on a tethered headset, which is where
// this will actually be read.
//-----------------------------------------------------------------------------

enum {
    MV_HANDLOG_POSE    = 1,   ///< the latched pose, in game units
    MV_HANDLOG_SOLVE   = 2,   ///< the arm solve and how far the hand missed by
    MV_HANDLOG_GESTURE = 4,   ///< gesture verdicts, including near misses
    MV_HANDLOG_ACTION  = 8,   ///< what the game was actually asked to do
};

bool mariovision_hands_logging(int stage);
void mariovision_hands_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

//-----------------------------------------------------------------------------
// The graph hook
//-----------------------------------------------------------------------------

/// Rewrite one of Mario's bones so his hand lands on the tracked controller.
///
/// `part` is a MarioAnimPart, `parent` the accumulated parent-to-camera matrix
/// the renderer has on its stack, and `prevFrame` selects the pose the previous
/// frame was built with - the renderer blends the two, so both passes have to be
/// answered from poses that agree, or the arm shakes instead of moving.
///
/// Returns false when it has nothing to say, which is every part it does not
/// own and every moment without controllers. The animation then keeps its say,
/// exactly as before.
///
/// Spelled with plain arrays rather than Mat4/Vec3f/Vec3s so this header stays
/// free of engine types and can be reached from Swift.
bool mariovision_hands_override(int part, float parent[4][4],
                                float translation[3], short rotation[3],
                                float scale[3], bool prevFrame);

//-----------------------------------------------------------------------------
// Solver
//
// Kept free of engine types on purpose, so it builds for the host and can be
// checked against forward kinematics without a device or a simulator. Given
// this project's history with signs and half-turns, every one of them belongs
// in a test rather than in a comment.
//-----------------------------------------------------------------------------

/// Where the arm goes. Directions are unit vectors in camera space.
typedef struct {
    float upper[3];   ///< along the upper arm, shoulder to elbow
    float normal[3];  ///< normal of the plane the arm bends in
    float bend;       ///< elbow bend in radians; 0 is a straight arm
    float reach;      ///< distance from shoulder to target, before clamping
} MarioVisionArmSolution;

/// Two-bone analytic IK, entirely in camera space.
///
/// `pole` nudges the elbow: the arm bends towards whichever side of the
/// shoulder-to-target line the pole falls on. Out of reach needs no special
/// case - the cosines clamp, the arm straightens, and the hand stops short,
/// which is the right thing to look at.
///
/// Returns false only when the target sits on the shoulder, where there is no
/// direction to point at.
bool mv_arm_solve(const float shoulder[3], const float target[3],
                  float l1, float l2, const float pole[3],
                  MarioVisionArmSolution *out);

/// The rotations the renderer writes into the bones, in the engine's s16 XYZ
/// convention and in the frame the solve was done in.
///
/// The bend's sign lives here rather than at the call site on purpose. It is
/// exactly the sort of thing that is fifty-fifty by inspection, so it belongs
/// inside what the test walks - a renderer that merely writes what it is given
/// has nowhere left to get it wrong.
typedef struct {
    short shoulder[3];  ///< part 5 / 9, aims the upper arm and rolls the plane
    short forearm[3];   ///< part 7 / 11, bends the elbow
} MarioVisionArmBones;

void mv_arm_bones(const MarioVisionArmSolution *sol, MarioVisionArmBones *out);

/// The engine's Euler convention, both ways.
///
/// `m` is row-major with row i the image of local axis i - exactly the layout
/// mtxf_rotate_xyz_and_translate builds, so a triple that goes out through
/// mv_euler_from_matrix comes back through the engine unchanged.
void mv_euler_from_matrix(const float m[3][3], short out[3]);
void mv_matrix_from_euler(const short r[3], float out[3][3]);

/// Orthonormal frame whose local +X is `x` and whose local +Z is `z`, with +Y
/// completing it. Mario's bones run along their own +X, so this is how a
/// direction becomes a bone rotation.
void mv_frame_from_axes(const float x[3], const float z[3], float out[3][3]);

void mv_quat_to_matrix(const float q[4], float out[3][3]);

void mv_basis_mul(const float a[3][3], const float b[3][3], float out[3][3]);

/// The parent's rotation inverted with its scale divided out. Use this for
/// frames; a point going into the parent's units wants the scale kept.
bool mv_basis_rotation_inverse(const float m[3][3], float out[3][3]);

/// The rotation a bone must carry so that, composed onto `parent`, its own
/// frame comes out as `wanted`. Both frames row-major, row i the image of axis i.
bool mv_local_frame(const float wanted[3][3], const float parent[3][3],
                    short outRotation[3]);

/// A controller pose in the room turned into one in game camera space.
///
/// Both matrices are column-major, the way simd hands them over. `anchorInverse`
/// is the inverse of the frame the first-person mode was entered with - the
/// same one the eye already goes through - and `metresToUnits` is the reciprocal
/// of the first-person world scale, so a hundred.
void mv_room_to_camera(const float anchorInverse[16], const float roomPose[16],
                       float metresToUnits, float outPos[3], float outQuat[4]);

#ifdef __cplusplus
}
#endif

#endif
