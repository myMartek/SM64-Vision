//-----------------------------------------------------------------------------
// Putting Mario's hands where the controllers are.
//
// The renderer walks his skeleton bone by bone, and at each one it asks the
// animation for a rotation. This intercepts that question for the arms and
// answers it from an inverse-kinematics solve instead, so the hands land on the
// tracked controllers and the elbows work out where to go.
//
// Everything is solved in camera space and only converted to a bone's own frame
// at the last moment. That is not an optimisation - it is what lets the solve
// avoid knowing anything about which way Mario's bones happen to point, a
// question the geo layout answers only indirectly and which would be easy to
// get subtly wrong.
//-----------------------------------------------------------------------------

#include "mariovision_hands.h"
#include "mariovision_portal.h"

#include "sm64.h"
#include "types.h"
#include "engine/math_util.h"
#include "game/first_person_cam.h"

#include <math.h>
#include <stdlib.h>

/// Mario's arm as the geo layout writes it: the forearm hangs 65 units along
/// the upper arm's own +X, the hand 60 further along the forearm's.
///
/// These are *model* units, and the graph shrinks them on the way out - the
/// matrix arriving at the shoulder was measured carrying a uniform quarter. So
/// his arm spans 125 model units but only about 31 of the units the world is
/// measured in, which at a hundred to the metre is 31 centimetres. Against a
/// person's 65 he reaches half as far, not twice as far as first assumed, and
/// pinning his hands to controllers held at arm's length would leave them
/// permanently outstretched and half a metre short.
///
/// So the arm is grown to a human one instead. Scaling at the shoulder takes
/// the whole limb with it - mesh included, so no joint pulls apart - and since
/// the rest of him is hidden anyway, nobody sees that these particular arms are
/// longer than Mario's.
#define ARM_UPPER 65.0f
#define ARM_FORE  60.0f
#define ARM_TOTAL (ARM_UPPER + ARM_FORE)

static float env_float(const char *name, float fallback) {
    const char *v = getenv(name);
    if (!v || !*v) { return fallback; }
    return (float) atof(v);
}

//-----------------------------------------------------------------------------
// Small matrix helpers over the engine's row-vector layout
//-----------------------------------------------------------------------------

/// General 3x3 inverse. The transpose would do for a pure rotation, but the
/// stack can carry an object scale, and a silently sheared arm is worse than a
/// few extra multiplies.
static bool basis_inverse(Mat4 m, float out[3][3]) {
    const float a = m[0][0], b = m[0][1], c = m[0][2];
    const float d = m[1][0], e = m[1][1], f = m[1][2];
    const float g = m[2][0], h = m[2][1], i = m[2][2];

    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabsf(det) < 1e-9f) { return false; }
    const float k = 1.0f / det;

    out[0][0] = (e * i - f * h) * k;
    out[0][1] = (c * h - b * i) * k;
    out[0][2] = (b * f - c * e) * k;
    out[1][0] = (f * g - d * i) * k;
    out[1][1] = (a * i - c * g) * k;
    out[1][2] = (c * d - a * f) * k;
    out[2][0] = (d * h - e * g) * k;
    out[2][1] = (b * g - a * h) * k;
    out[2][2] = (a * e - b * d) * k;
    return true;
}

/// The parent's 3x3 basis, copied out of the 4x4 the graph carries.
///
/// The copy exists so everything below can call the tested helpers in
/// mariovision_hands_math.c, which take float[3][3]. This file used to carry
/// its own transpose and its own multiply; they looked identical to the tested
/// ones and behaved differently on device, which cost an afternoon. One
/// implementation, and the tested one.
static void basis_of(Mat4 m, float out[3][3]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) { out[r][c] = m[r][c]; }
    }
}

/// A point in camera space expressed in the frame `parent` describes.
static bool point_into_parent(Mat4 parent, const float inv[3][3],
                              const float cam[3], float out[3]) {
    const float rel[3] = {
        cam[0] - parent[3][0],
        cam[1] - parent[3][1],
        cam[2] - parent[3][2],
    };
    for (int c = 0; c < 3; c++) {
        out[c] = rel[0] * inv[0][c] + rel[1] * inv[1][c] + rel[2] * inv[2][c];
    }
    return true;
}

//-----------------------------------------------------------------------------
// Where the shoulders sit
//
// Mario's own shoulders are no use here. The geo layout puts them 135 units
// above his origin while the first-person eye sits at 120, so they are fifteen
// units *above* the viewer's eyes, and 79 units - most of a metre - out to each
// side, against something like twenty for a person. Arms hung from there fold
// across the chest and the elbows splay outwards.
//
// So the shoulders are placed rather than inherited: a fixed offset from where
// the eye actually is, in camera space. Camera space is pinned to the pose the
// mode was entered with, not to the head, which gives exactly the right
// behaviour on both counts - a snap turn rotates the world and takes the
// shoulders with it, while merely looking around leaves them where a body's
// shoulders would stay.
//-----------------------------------------------------------------------------

static void shoulder_position(int side, float out[3]) {
    // Centimetres, at a hundred units to the metre. Set close to the eye at
    // first and the upper arms filled the lower half of the view - which is
    // correct for something twenty centimetres from your eye, and precisely
    // why real shoulders do not sit there. Further down and further back is
    // both truer and kinder: what stays in sight is the forearms and hands,
    // which is what one sees of one's own arms.
    MarioVisionHandFit fit;
    mariovision_hands_get_fit(&fit);
    const float half = fit.shoulderHalf;
    const float drop = fit.shoulderDrop;
    const float back = fit.shoulderBack;

    float eye[3] = { 0.0f, 0.0f, 0.0f };
    mariovision_billboard_eye(eye);

    out[0] = eye[0] + ((side == MV_HAND_RIGHT) ? half : -half);
    out[1] = eye[1] - drop;
    out[2] = eye[2] + back;   // the camera looks along -z, so +z is behind
}

/// Which way the elbow is pushed: down, a little outwards, a little back. This
/// is the whole of "arms near the body" - without it the solve is free to put
/// the elbows anywhere on a cone and they wander.
static void elbow_pole(int side, float out[3]) {
    MarioVisionHandFit fit;
    mariovision_hands_get_fit(&fit);
    const float outward = (side == MV_HAND_RIGHT) ? 1.0f : -1.0f;
    out[0] = outward * fit.elbowOut;
    out[1] = -1.0f;
    out[2] = fit.elbowBack;
}

//-----------------------------------------------------------------------------
// The solve, remembered between bones
//
// Parts arrive in order - shoulder, arm, forearm, hand - and each frame is
// walked twice, once for now and once for the frame before. Solving at the
// shoulder and keeping the answer for the forearm costs one small struct and
// saves repeating the work, and the traversal order that makes it safe is the
// renderer's own and does not vary.
//-----------------------------------------------------------------------------

typedef struct {
    bool valid;
    MarioVisionArmBones bones;
    float grow;
} ArmCache;

static ArmCache sArm[MV_HAND_COUNT][2];

/// The hand target in camera space, with the viewer's reach stretched out from
/// the shoulder.
///
/// Amplifying from the shoulder rather than from the camera matters: measured
/// from the camera, standing still with your arms down would already be a
/// displacement, and turning the dial up would push your hands into the floor.
static bool hand_target(int side, bool prevFrame, const float shoulder[3], float out[3]) {
    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, prevFrame, &pose)) { return false; }

    const float scale = mariovision_hands_reach_scale();
    for (int i = 0; i < 3; i++) {
        out[i] = shoulder[i] + (pose.pos[i] - shoulder[i]) * scale;
    }
    return true;
}

static int side_of_part(int part) {
    switch (part) {
        case MARIO_ANIM_PART_UPPER_LEFT:
        case MARIO_ANIM_PART_LEFT_ARM:
        case MARIO_ANIM_PART_LEFT_FOREARM:
        case MARIO_ANIM_PART_LEFT_HAND:
            return MV_HAND_LEFT;
        case MARIO_ANIM_PART_UPPER_RIGHT:
        case MARIO_ANIM_PART_RIGHT_ARM:
        case MARIO_ANIM_PART_RIGHT_FOREARM:
        case MARIO_ANIM_PART_RIGHT_HAND:
            return MV_HAND_RIGHT;
        default:
            return -1;
    }
}

//-----------------------------------------------------------------------------

/// How long the arm should be once it reaches the world, in game units. A
/// hundred to the metre, so the default is a 62 cm arm - about a person's.
static float arm_reach(void) {
    MarioVisionHandFit fit;
    mariovision_hands_get_fit(&fit);
    return fit.armReach;
}

/// What the graph is doing to lengths by the time it gets here. Measured rather
/// than assumed: it came out at a quarter, and a number that surprising is
/// exactly the sort that changes in a state nobody thought to check.
static float parent_scale(Mat4 parent) {
    const float len = sqrtf(parent[0][0] * parent[0][0]
                            + parent[0][1] * parent[0][1]
                            + parent[0][2] * parent[0][2]);
    return (len > 1e-6f) ? len : 1.0f;
}

bool mariovision_hands_override(int part, Mat4 parent, Vec3f translation,
                                Vec3s rotation, Vec3f scale, bool prevFrame) {
    if (!mariovision_first_person_active()) { return false; }
    if (!mariovision_hands_active()) { return false; }

    const int slot = prevFrame ? 1 : 0;

    // The torso carries everything the arms hang from, so it is turned to face
    // the way the viewer is looking. Left on the animation it faces whichever
    // way Mario is walking, which while running backwards is the opposite of
    // where you are looking - and the arms would be behind you.
    if (part == MARIO_ANIM_PART_BUTT) {
        rotation[0] = rotation[1] = rotation[2] = 0;
        return true;
    }

    if (part == MARIO_ANIM_PART_TORSO) {
        float parentBasis[3][3];
        basis_of(parent, parentBasis);

        // Bone +X runs up the spine and bone +Z is Mario's left, which the geo
        // layout gives away by hanging the left shoulder at +79 on z and the
        // head at +87 on x.
        const float up[3] = { 0.0f, 1.0f, 0.0f };
        const float left[3] = { -1.0f, 0.0f, 0.0f };
        float want[3][3];
        mv_frame_from_axes(up, left, want);

        if (!mv_local_frame((const float (*)[3]) want,
                            (const float (*)[3]) parentBasis, rotation)) {
            return false;
        }

        if (!prevFrame && mariovision_hands_logging(MV_HANDLOG_SOLVE)) {
            static unsigned tick = 0;
            if ((++tick % 30) == 0) {
                // A known matrix put through the same function, so the device
                // can be asked the question the host test already answers. The
                // two disagreed, and one of them had to be running code that
                // was not on disk.
                const float quarterAboutZ[3][3] = {
                    {  0.0f, 1.0f, 0.0f },
                    { -1.0f, 0.0f, 0.0f },
                    {  0.0f, 0.0f, 1.0f },
                };
                short probe[3];
                mv_euler_from_matrix(quarterAboutZ, probe);
                mariovision_hands_log(
                    "[hands] torso rot %d %d %d | probe %d %d %d (host says 0 0 16384)",
                    (int) rotation[0], (int) rotation[1], (int) rotation[2],
                    (int) probe[0], (int) probe[1], (int) probe[2]);
            }
        }
        return true;
    }

    const int side = side_of_part(part);
    if (side < 0) { return false; }

    //-------------------------------------------------------------------------
    // Hands alone
    //
    // The arm is where every millimetre of error comes from: a guessed
    // shoulder, a limb a little too short to reach, an elbow that had to go
    // somewhere. Take it away and the hand is simply placed - it cannot miss,
    // because nothing is left to compute.
    //
    // The bones still exist and are still walked; they are merely stacked on
    // top of each other at the controller, which puts the hand exactly there.
    // Their geometry is collapsed by the visibility filter, which already knows
    // how to make a part vanish without taking its render state with it.
    //-------------------------------------------------------------------------
    if (mariovision_hands_only()) {
        MarioVisionHandPose pose;
        if (!mariovision_hands_get(side, prevFrame, &pose)) { return false; }

        MarioVisionHandFit fit;
        mariovision_hands_get_fit(&fit);

        switch (part) {
            case MARIO_ANIM_PART_UPPER_LEFT:
            case MARIO_ANIM_PART_UPPER_RIGHT: {
                float inv[3][3];
                if (!basis_inverse(parent, inv)) { return false; }

                // In the room first, before anything turns. This is the one
                // that answers "the hands sit too high": it moves them in the
                // world, and stays put when the controller is twisted.
                const float placed[3] = {
                    pose.pos[0] + fit.handWorldOffset[0],
                    pose.pos[1] + fit.handWorldOffset[1],
                    pose.pos[2] + fit.handWorldOffset[2],
                };

                float local[3];
                point_into_parent(parent, (const float (*)[3]) inv, placed, local);
                translation[0] = local[0];
                translation[1] = local[1];
                translation[2] = local[2];

                // The controller's own orientation, plus the correction between
                // its grip frame and Mario's hand. Measured on the headset, not
                // reasoned about.
                float grip[3][3];
                if (!mariovision_hand_frame(side, prevFrame, grip)) { return false; }

                float parentBasis[3][3];
                basis_of(parent, parentBasis);
                mv_local_frame((const float (*)[3]) grip,
                               (const float (*)[3]) parentBasis, rotation);

                // Size, taken here so the whole stub inherits it.
                //
                // Straight through, and emphatically not divided by the
                // parent's scale. Dividing was the first attempt and it made
                // one mean four: the graph shrinks Mario by a quarter on the
                // way out, so cancelling that draws his hand at model size -
                // some thirty centimetres across. One here is his own hand as
                // the game draws it, which is the only meaning of "one" worth
                // having on a slider.
                scale[0] = scale[1] = scale[2] = fit.handScale;
                return true;
            }

            // Everything between the shoulder and the hand is folded onto the
            // same point: no offset, no turn. The hand therefore lands exactly
            // where the shoulder bone was put, which is the controller.
            case MARIO_ANIM_PART_LEFT_ARM:
            case MARIO_ANIM_PART_RIGHT_ARM:
            case MARIO_ANIM_PART_LEFT_FOREARM:
            case MARIO_ANIM_PART_RIGHT_FOREARM:
                translation[0] = translation[1] = translation[2] = 0.0f;
                rotation[0] = rotation[1] = rotation[2] = 0;
                return true;

            case MARIO_ANIM_PART_LEFT_HAND:
            case MARIO_ANIM_PART_RIGHT_HAND: {
                // The offsets are in game units - centimetres - so they have to
                // be divided by whatever the chain is scaling by, or a slider
                // marked "5 cm" would move the hand by five model units and
                // mean something different at every hand size.
                const float s = parent_scale(parent);
                const float k = (s > 1e-6f) ? (1.0f / s) : 1.0f;
                translation[0] = fit.handOffset[0] * k;
                translation[1] = fit.handOffset[1] * k;
                translation[2] = fit.handOffset[2] * k;
                rotation[0] = rotation[1] = rotation[2] = 0;
                return true;
            }

            default:
                return false;
        }
    }

    switch (part) {
        case MARIO_ANIM_PART_UPPER_LEFT:
        case MARIO_ANIM_PART_UPPER_RIGHT: {
            sArm[side][slot].valid = false;

            float shoulder[3];
            shoulder_position(side, shoulder);

            float target[3];
            if (!hand_target(side, prevFrame, shoulder, target)) { return false; }

            float pole[3];
            elbow_pole(side, pole);

            // The solve works in camera units, so the bones have to be measured
            // in camera units too - the model's 65 and 60 are not what arrives
            // out here.
            const float reach = arm_reach();
            const float l1 = reach * (ARM_UPPER / ARM_TOTAL);
            const float l2 = reach * (ARM_FORE / ARM_TOTAL);

            MarioVisionArmSolution sol;
            if (!mv_arm_solve(shoulder, target, l1, l2, pole, &sol)) {
                return false;
            }

            float inv[3][3];
            if (!basis_inverse(parent, inv)) { return false; }

            // Grow the limb to the length the solve just assumed. This scales
            // the basis and leaves the translation alone, so the whole arm
            // below grows while the shoulder stays where it was put.
            // Length and thickness, separately. The even scale is the one
            // where the mesh meets itself at the elbow; the factor moves the
            // thickness away from it, and overlapping (above one) hides far
            // better than pulling apart (below one).
            MarioVisionHandFit fit;
            mariovision_hands_get_fit(&fit);
            const float grow = reach / (ARM_TOTAL * parent_scale(parent));
            scale[0] = scale[1] = scale[2] = grow * fit.armThickness;

            // Put the shoulder where it belongs rather than where the skeleton
            // would have it.
            float local[3];
            point_into_parent(parent, (const float (*)[3]) inv, shoulder, local);
            translation[0] = local[0];
            translation[1] = local[1];
            translation[2] = local[2];

            MarioVisionArmBones bones;
            mv_arm_bones(&sol, &bones);

            // The solve produced a frame in camera space; the bone needs it
            // relative to its parent. mv_local_frame divides the parent's scale
            // out on the way, which the translation above deliberately keeps.
            float camFrame[3][3];
            mv_matrix_from_euler(bones.shoulder, camFrame);

            float parentBasis[3][3];
            basis_of(parent, parentBasis);
            if (!mv_local_frame((const float (*)[3]) camFrame,
                                (const float (*)[3]) parentBasis, rotation)) {
                return false;
            }

            // The forearm's bend is expressed in the shoulder's own frame, and
            // the arm bone in between neither moves nor turns, so it can be
            // handed straight on when that part comes round.
            sArm[side][slot].bones = bones;
            sArm[side][slot].grow = grow;
            sArm[side][slot].valid = true;

            if (!prevFrame && mariovision_hands_logging(MV_HANDLOG_SOLVE)) {
                static unsigned tick = 0;
                if (side == MV_HAND_RIGHT && (++tick % 30) == 0) {
                    float eye[3] = { 0, 0, 0 };
                    mariovision_billboard_eye(eye);
                    mariovision_hands_log(
                        "[hands] solve %s: reach %.1f of %.0f, bend %.1f deg, grow %.2f",
                        side == MV_HAND_LEFT ? "left" : "right",
                        (double) sol.reach, (double) reach,
                        (double) (sol.bend * 180.0f / (float) M_PI),
                        (double) grow);
                    mariovision_hands_log(
                        "[hands]   eye %.1f %.1f %.1f  shoulder %.1f %.1f %.1f  target %.1f %.1f %.1f",
                        (double) eye[0], (double) eye[1], (double) eye[2],
                        (double) shoulder[0], (double) shoulder[1], (double) shoulder[2],
                        (double) target[0], (double) target[1], (double) target[2]);
                    mariovision_hands_log(
                        "[hands]   parent origin %.1f %.1f %.1f  local shoulder %.1f %.1f %.1f",
                        (double) parent[3][0], (double) parent[3][1], (double) parent[3][2],
                        (double) translation[0], (double) translation[1], (double) translation[2]);
                    mariovision_hands_log(
                        "[hands]   torso axes: X %.2f %.2f %.2f  Y %.2f %.2f %.2f  Z %.2f %.2f %.2f",
                        (double) parent[0][0], (double) parent[0][1], (double) parent[0][2],
                        (double) parent[1][0], (double) parent[1][1], (double) parent[1][2],
                        (double) parent[2][0], (double) parent[2][1], (double) parent[2][2]);
                    mariovision_hands_log(
                        "[hands]   wanted upper %.2f %.2f %.2f",
                        (double) sol.upper[0], (double) sol.upper[1], (double) sol.upper[2]);
                    mariovision_hands_log(
                        "[hands]   parent row lengths %.3f %.3f %.3f",
                        (double) sqrtf(parent[0][0]*parent[0][0] + parent[0][1]*parent[0][1] + parent[0][2]*parent[0][2]),
                        (double) sqrtf(parent[1][0]*parent[1][0] + parent[1][1]*parent[1][1] + parent[1][2]*parent[1][2]),
                        (double) sqrtf(parent[2][0]*parent[2][0] + parent[2][1]*parent[2][1] + parent[2][2]*parent[2][2]));
                }
            }
            return true;
        }

        case MARIO_ANIM_PART_LEFT_ARM:
        case MARIO_ANIM_PART_RIGHT_ARM:
            // The whole shoulder rotation went into the part above, which sits
            // at the same point; this one only has to stop the animation
            // turning it again.
            if (!sArm[side][slot].valid) { return false; }
            rotation[0] = rotation[1] = rotation[2] = 0;
            if (!prevFrame && side == MV_HAND_RIGHT
                && mariovision_hands_logging(MV_HANDLOG_SOLVE)) {
                static unsigned tick = 0;
                if ((++tick % 30) == 0) {
                    mariovision_hands_log(
                        "[hands]   arm root landed at %.1f %.1f %.1f (scale %.3f)",
                        (double) parent[3][0], (double) parent[3][1], (double) parent[3][2],
                        (double) parent_scale(parent));
                }
            }
            return true;

        case MARIO_ANIM_PART_LEFT_FOREARM:
        case MARIO_ANIM_PART_RIGHT_FOREARM:
            if (!sArm[side][slot].valid) { return false; }
            rotation[0] = sArm[side][slot].bones.forearm[0];
            rotation[1] = sArm[side][slot].bones.forearm[1];
            rotation[2] = sArm[side][slot].bones.forearm[2];
            return true;

        case MARIO_ANIM_PART_LEFT_HAND:
        case MARIO_ANIM_PART_RIGHT_HAND: {
            if (!sArm[side][slot].valid) { return false; }
            rotation[0] = rotation[1] = rotation[2] = 0;

            // Turn the wrist with the controller.
            //
            // Which way the grip frame's axes point out of the device is a
            // hardware question, so the correction between it and Mario's hand
            // bone is a tunable rather than a guess: MARIOVISION_HANDS_GRIP_ROT
            // takes three degrees, pitch/yaw/roll, and one session on the
            // headset settles it. Left at zero the hand simply follows the
            // controller's own frame, which is already better than not turning
            // at all.
            MarioVisionHandPose pose;
            if (mariovision_hands_get(side, prevFrame, &pose)) {
                float grip[3][3];
                mv_quat_to_matrix(pose.quat, grip);

                MarioVisionHandFit turn;
                mariovision_hands_get_fit(&turn);
                const float px = turn.handRotate[0];
                const float py = turn.handRotate[1];
                const float pr = turn.handRotate[2];
                if (px != 0.0f || py != 0.0f || pr != 0.0f) {
                    const short correction[3] = {
                        (short) (int) (px / 360.0f * 65536.0f),
                        (short) (int) (py / 360.0f * 65536.0f),
                        (short) (int) (pr / 360.0f * 65536.0f),
                    };
                    float fix[3][3];
                    mv_matrix_from_euler(correction, fix);
                    mv_basis_mul((const float (*)[3]) fix,
                                 (const float (*)[3]) grip, grip);
                }

                float parentBasis[3][3];
                basis_of(parent, parentBasis);
                mv_local_frame((const float (*)[3]) grip,
                               (const float (*)[3]) parentBasis, rotation);
            }

            // Give the hand its own size back.
            //
            // Lengthening the arm scales it evenly, which is what keeps the
            // mesh from tearing at the joints - but it doubles the thickness
            // along with the length, and Mario's hands are already large
            // spheres. At the end of the chain they filled a third of the view.
            // The hand is a leaf, so undoing the scale here costs nothing and
            // touches nothing downstream.
            {
                const float k = sArm[side][slot].grow;
                const float undo = (k > 1e-3f) ? (1.0f / k) : 1.0f;
                MarioVisionHandFit fit;
                mariovision_hands_get_fit(&fit);
                scale[0] = scale[1] = scale[2] = undo * fit.handScale;

                // And where the glove sits relative to the controller. The
                // bone is the wrist, not the palm, so a hand that looks
                // correctly placed still needs a nudge along its own axes.
                translation[0] += fit.handOffset[0];
                translation[1] += fit.handOffset[1];
                translation[2] += fit.handOffset[2];
            }
            if (!prevFrame && side == MV_HAND_RIGHT
                && mariovision_hands_logging(MV_HANDLOG_SOLVE)) {
                static unsigned tick = 0;
                if ((++tick % 30) == 0) {
                    // Where the elbow ended up. Together with the target this
                    // is the one number that says whether the whole chain
                    // arrived: guessing from a screenshot has cost this project
                    // days before.
                    mariovision_hands_log(
                        "[hands]   elbow landed at %.1f %.1f %.1f",
                        (double) parent[3][0], (double) parent[3][1], (double) parent[3][2]);
                }
            }
            return true;
        }

        default:
            return false;
    }
}
