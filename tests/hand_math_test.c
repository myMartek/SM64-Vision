//-----------------------------------------------------------------------------
// Checks the arm solver by walking Mario's bone chain forwards.
//
// The solver's whole job is to answer "what rotations put his hand there", and
// the only honest check is to take those rotations, walk the skeleton the way
// the renderer walks it, and see where the hand actually lands. Anything less -
// eyeballing the angles, trusting the algebra - is how a sign error survives to
// the headset, which in this project it has done more than once.
//
// The chain is walked through the engine's own Euler convention on purpose:
// the solve produces a matrix, the matrix is squeezed into an s16 triple, and
// the triple is expanded back into a matrix, exactly as the engine will. A
// convention error therefore shows up as a residual rather than hiding.
//
// Build and run: scripts/test-hand-math.sh
//-----------------------------------------------------------------------------

#include "mariovision_hands.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/// Mario's arm, from actors/mario/geo.inc.c: the forearm hangs 65 units along
/// the upper arm's own +X, and the hand 60 further along the forearm's.
#define ARM_UPPER 65.0f
#define ARM_FORE  60.0f

/// coopdx's own atan2f, reproduced here on purpose.
///
/// The engine defines a global function of exactly this name, and a plain
/// global symbol displaces the C library's for the whole binary - so on device
/// every file links against this one whether it asked for it or not. Its SM64
/// angle convention effectively swaps the arguments, which put every extracted
/// angle in the wrong slot while this test, linking only libm, kept insisting
/// the code was right.
///
/// Including it here makes the test environment the honest one. If anything in
/// the solver ever reaches for atan2f again, the checks below fail on the host
/// instead of on the headset.
float atan2f(float y, float x) {
    // The game's atan2s measures from +z rather than +x. The swap is the whole
    // of the difference, and the whole of the trap.
    return (float) atan2((double) x, (double) y);
}

static int sFailures = 0;
static int sChecks = 0;

static void fail(const char *what, const char *detail) {
    printf("  FAIL  %s: %s\n", what, detail);
    sFailures++;
}

static void check(bool ok, const char *what, const char *detail) {
    sChecks++;
    if (!ok) { fail(what, detail); }
}

//-----------------------------------------------------------------------------
// Row-vector matrix helpers, matching the engine: row i is the image of local
// axis i, and a point goes p' = p * M, so "local then parent" is A * B.
//-----------------------------------------------------------------------------

static void mat_mul(const float a[3][3], const float b[3][3], float out[3][3]) {
    float tmp[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
        }
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) { out[i][j] = tmp[i][j]; }
    }
}

static float dist3(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/// Walk shoulder -> elbow -> hand exactly as geo_process_animated_part would,
/// starting from an identity parent so local space and camera space coincide.
/// Nothing here interprets the solution: it takes the very s16 triples the
/// renderer will write and expands them through the engine's own convention.
/// That is what makes the check worth having - a sign error anywhere in
/// mv_arm_bones lands the hand somewhere else, and there is no second copy of
/// the convention in the test to agree with the mistake.
static void forward_kinematics(const float shoulder[3],
                               const MarioVisionArmSolution *sol,
                               float outElbow[3], float outHand[3]) {
    MarioVisionArmBones bones;
    mv_arm_bones(sol, &bones);

    float m5[3][3];
    mv_matrix_from_euler(bones.shoulder, m5);

    for (int i = 0; i < 3; i++) {
        outElbow[i] = shoulder[i] + ARM_UPPER * m5[0][i];
    }

    float m7[3][3];
    mv_matrix_from_euler(bones.forearm, m7);

    // Local then parent, in the engine's row-vector order.
    float m7cam[3][3];
    mat_mul(m7, m5, m7cam);

    for (int i = 0; i < 3; i++) {
        outHand[i] = outElbow[i] + ARM_FORE * m7cam[0][i];
    }
}

//-----------------------------------------------------------------------------

static void test_reachable_targets(void) {
    printf("==> Reachable targets: does the hand land on the target\n");

    // A shoulder somewhere unhelpful, so nothing can pass by accident through a
    // zero. Camera space: x right, y up, camera looking along -z.
    const float shoulder[3] = { 32.0f, -18.0f, -4.0f };
    const float pole[3] = { 0.35f, -1.0f, 0.15f };

    float worst = 0.0f;
    float worstAt[3] = { 0, 0, 0 };

    for (int ix = -3; ix <= 3; ix++) {
        for (int iy = -3; iy <= 3; iy++) {
            for (int iz = -3; iz <= 3; iz++) {
                const float target[3] = {
                    shoulder[0] + (float) ix * 18.0f,
                    shoulder[1] + (float) iy * 18.0f,
                    shoulder[2] + (float) iz * 18.0f
                };

                MarioVisionArmSolution sol;
                if (!mv_arm_solve(shoulder, target, ARM_UPPER, ARM_FORE, pole, &sol)) {
                    continue;   // target sitting on the shoulder
                }
                // Only the ones the arm can actually reach; the rest are the
                // next test's business.
                if (sol.reach > ARM_UPPER + ARM_FORE) { continue; }
                if (sol.reach < ARM_UPPER - ARM_FORE) { continue; }

                float elbow[3], hand[3];
                forward_kinematics(shoulder, &sol, elbow, hand);

                const float residual = dist3(hand, target);
                if (residual > worst) {
                    worst = residual;
                    worstAt[0] = target[0]; worstAt[1] = target[1]; worstAt[2] = target[2];
                }

                // The bones must also keep their lengths - a solve that lands
                // the hand correctly with a stretched forearm is still wrong.
                check(fabsf(dist3(shoulder, elbow) - ARM_UPPER) < 0.05f,
                      "upper arm length", "the upper arm changed length");
                check(fabsf(dist3(elbow, hand) - ARM_FORE) < 0.05f,
                      "forearm length", "the forearm changed length");
            }
        }
    }

    char detail[160];
    snprintf(detail, sizeof detail,
             "worst residual %.4f units at (%.0f, %.0f, %.0f)",
             (double) worst, (double) worstAt[0], (double) worstAt[1], (double) worstAt[2]);
    printf("      %s\n", detail);
    check(worst < 0.5f, "reachable residual", detail);
}

static void test_out_of_reach(void) {
    printf("==> Out of reach: arm straight, pointing at the target\n");

    const float shoulder[3] = { 0.0f, 0.0f, 0.0f };
    const float pole[3] = { 0.0f, -1.0f, 0.0f };
    const float target[3] = { 0.0f, 0.0f, -400.0f };   // far in front

    MarioVisionArmSolution sol;
    check(mv_arm_solve(shoulder, target, ARM_UPPER, ARM_FORE, pole, &sol),
          "far target solves", "the solver gave up on a distant target");

    check(fabsf(sol.bend) < 1e-3f, "far target bend",
          "the elbow is still bent although the arm cannot reach");

    float elbow[3], hand[3];
    forward_kinematics(shoulder, &sol, elbow, hand);

    // The hand should sit on the line to the target, at full extension.
    const float extension = sqrtf(hand[0]*hand[0] + hand[1]*hand[1] + hand[2]*hand[2]);
    char detail[128];
    snprintf(detail, sizeof detail, "extension %.3f, expected %.3f",
             (double) extension, (double) (ARM_UPPER + ARM_FORE));
    check(fabsf(extension - (ARM_UPPER + ARM_FORE)) < 0.5f, "full extension", detail);

    check(fabsf(hand[0]) < 0.5f && fabsf(hand[1]) < 0.5f,
          "points at the target", "the straight arm does not point at the target");
}

static void test_euler_roundtrip(void) {
    printf("==> Euler convention: matrix -> s16 triple -> matrix\n");

    // Includes straight up and straight down, where the y rotation swallows one
    // of the other two and only their sum survives. The frame still has to come
    // back, even though the triple that describes it will not.
    const float axes[][6] = {
        {  1,  0,  0,    0,  0,  1 },
        {  0,  1,  0,    1,  0,  0 },
        {  0,  0, -1,    0,  1,  0 },
        {  0,  1,  0,    0,  0,  1 },   // local +X straight up: gimbal lock
        {  0, -1,  0,    0,  0,  1 },   // straight down
        { 0.577f, 0.577f, 0.577f,  -0.707f, 0.707f, 0.0f },
    };

    for (unsigned i = 0; i < sizeof axes / sizeof axes[0]; i++) {
        float m[3][3];
        mv_frame_from_axes(&axes[i][0], &axes[i][3], m);

        short euler[3];
        mv_euler_from_matrix(m, euler);
        float back[3][3];
        mv_matrix_from_euler(euler, back);

        float worst = 0.0f;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                const float e = fabsf(m[r][c] - back[r][c]);
                if (e > worst) { worst = e; }
            }
        }

        char detail[128];
        snprintf(detail, sizeof detail, "case %u, worst element error %.5f",
                 i, (double) worst);
        check(worst < 1e-3f, "euler round-trip", detail);
    }
}

static void test_elbow_follows_pole(void) {
    printf("==> Elbow swivel: the elbow goes where the pole points\n");

    const float shoulder[3] = { 0.0f, 0.0f, 0.0f };
    const float target[3] = { 0.0f, 0.0f, -100.0f };   // in front, within reach

    // Elbow down.
    {
        const float pole[3] = { 0.0f, -1.0f, 0.0f };
        MarioVisionArmSolution sol;
        check(mv_arm_solve(shoulder, target, ARM_UPPER, ARM_FORE, pole, &sol),
              "solve with a downward pole", "the solver gave up");
        float elbow[3], hand[3];
        forward_kinematics(shoulder, &sol, elbow, hand);
        char detail[128];
        snprintf(detail, sizeof detail, "elbow y = %.2f, expected below zero",
                 (double) elbow[1]);
        check(elbow[1] < -1.0f, "elbow below the line", detail);
    }

    // Elbow up: the same target with the pole flipped has to move it the other
    // way, or the pole is being ignored.
    {
        const float pole[3] = { 0.0f, 1.0f, 0.0f };
        MarioVisionArmSolution sol;
        check(mv_arm_solve(shoulder, target, ARM_UPPER, ARM_FORE, pole, &sol),
              "solve with an upward pole", "the solver gave up");
        float elbow[3], hand[3];
        forward_kinematics(shoulder, &sol, elbow, hand);
        char detail[128];
        snprintf(detail, sizeof detail, "elbow y = %.2f, expected above zero",
                 (double) elbow[1]);
        check(elbow[1] > 1.0f, "elbow above the line", detail);
    }
}

static void test_degenerate(void) {
    printf("==> Degenerate inputs\n");

    const float shoulder[3] = { 5.0f, -3.0f, 2.0f };
    const float pole[3] = { 0.0f, -1.0f, 0.0f };

    // Target on the shoulder: there is no direction to point at.
    {
        MarioVisionArmSolution sol;
        check(!mv_arm_solve(shoulder, shoulder, ARM_UPPER, ARM_FORE, pole, &sol),
              "target on the shoulder", "the solver claimed a solution");
    }

    // Pole parallel to the arm says nothing about which way to bend, but the
    // solve still has to produce a usable arm rather than a NaN.
    {
        const float target[3] = { shoulder[0], shoulder[1] - 100.0f, shoulder[2] };
        MarioVisionArmSolution sol;
        check(mv_arm_solve(shoulder, target, ARM_UPPER, ARM_FORE, pole, &sol),
              "pole parallel to the arm", "the solver gave up");

        float elbow[3], hand[3];
        forward_kinematics(shoulder, &sol, elbow, hand);
        const float residual = dist3(hand, target);
        char detail[128];
        snprintf(detail, sizeof detail, "residual %.4f", (double) residual);
        check(residual == residual && residual < 0.5f,
              "parallel pole still lands", detail);
    }
}

//-----------------------------------------------------------------------------
// Room space to camera space
//
// This is the conversion the plan meant to exercise by writing the synthetic
// motions in room space. That turned out to be impossible in the simulator,
// which never produces a device anchor - so it is checked here instead, which
// is a better place for it anyway.
//-----------------------------------------------------------------------------

/// Column-major, the way simd hands matrices over: a yaw about +Y and an offset.
static void make_transform(float yaw, float tx, float ty, float tz, float out[16]) {
    const float c = cosf(yaw), s = sinf(yaw);
    // Columns are the images of the basis vectors.
    out[0] = c;    out[1] = 0.0f; out[2] = -s;   out[3] = 0.0f;
    out[4] = 0.0f; out[5] = 1.0f; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = s;    out[9] = 0.0f; out[10] = c;   out[11] = 0.0f;
    out[12] = tx;  out[13] = ty;  out[14] = tz;  out[15] = 1.0f;
}

/// Inverse of a rigid transform: transpose the basis, and undo the offset.
static void invert_rigid(const float m[16], float out[16]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[c * 4 + r] = m[r * 4 + c];
        }
        out[r * 4 + 3] = 0.0f;
    }
    for (int r = 0; r < 3; r++) {
        out[12 + r] = -(out[0 * 4 + r] * m[12] + out[1 * 4 + r] * m[13] + out[2 * 4 + r] * m[14]);
    }
    out[15] = 1.0f;
}

static void test_room_to_camera(void) {
    printf("==> Room space to camera space\n");

    // A hand a metre in front of where the viewer was standing has to land at
    // one hundred units in front of the camera, whichever way the room happens
    // to be turned. Undoing the anchor's rotation is exactly where a sign error
    // would sit, and only a turned anchor can catch it - an identity one agrees
    // with every convention.
    const float yaws[] = { 0.0f, 0.7f, 2.5f, -1.9f, 3.14159f };

    for (unsigned i = 0; i < sizeof yaws / sizeof yaws[0]; i++) {
        const float yaw = yaws[i];
        float anchor[16], anchorInv[16];
        make_transform(yaw, 1.5f, 1.2f, -0.4f, anchor);
        invert_rigid(anchor, anchorInv);

        // One metre along the anchor's own forward, which is its -Z column.
        float hand[16];
        make_transform(yaw,
                       anchor[12] - anchor[8],
                       anchor[13] - anchor[9],
                       anchor[14] - anchor[10],
                       hand);

        float pos[3], quat[4];
        mv_room_to_camera(anchorInv, hand, 100.0f, pos, quat);

        char detail[160];
        snprintf(detail, sizeof detail,
                 "yaw %.2f gave (%.2f, %.2f, %.2f), expected (0, 0, -100)",
                 (double) yaw, (double) pos[0], (double) pos[1], (double) pos[2]);
        check(fabsf(pos[0]) < 0.05f && fabsf(pos[1]) < 0.05f
                  && fabsf(pos[2] + 100.0f) < 0.05f,
              "a metre forward is a hundred units forward", detail);

        // Same rotation as the anchor means no rotation relative to it.
        float basis[3][3];
        mv_quat_to_matrix(quat, basis);
        float worst = 0.0f;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                const float want = (r == c) ? 1.0f : 0.0f;
                const float e = fabsf(basis[r][c] - want);
                if (e > worst) { worst = e; }
            }
        }
        snprintf(detail, sizeof detail, "yaw %.2f, worst element error %.5f",
                 (double) yaw, (double) worst);
        check(worst < 1e-3f, "matching orientation cancels", detail);
    }

    // A hand turned relative to the anchor, checked against the engine's own
    // row convention. The identity case above cannot catch a transposed basis -
    // the identity is symmetric and agrees with the mistake - so this one uses
    // an angle that does not.
    {
        const float anchorYaw = 0.6f;
        const float extra = 0.9f;

        float anchor[16], anchorInv[16], hand[16];
        make_transform(anchorYaw, 0.3f, 1.1f, -0.7f, anchor);
        invert_rigid(anchor, anchorInv);
        make_transform(anchorYaw + extra, anchor[12], anchor[13], anchor[14], hand);

        float quat[4];
        mv_room_to_camera(anchorInv, hand, 100.0f, NULL, quat);
        float basis[3][3];
        mv_quat_to_matrix(quat, basis);

        // Row i is the image of local axis i, and make_transform sends local +X
        // to (cos, 0, -sin).
        const float c = cosf(extra), s = sinf(extra);
        const float want[3][3] = {
            {  c, 0.0f, -s   },
            { 0.0f, 1.0f, 0.0f },
            {  s, 0.0f,  c   },
        };

        float worst = 0.0f;
        for (int r = 0; r < 3; r++) {
            for (int cc = 0; cc < 3; cc++) {
                const float e = fabsf(basis[r][cc] - want[r][cc]);
                if (e > worst) { worst = e; }
            }
        }
        char detail[160];
        snprintf(detail, sizeof detail,
                 "worst element error %.5f (row0 was %.3f, %.3f, %.3f, wanted %.3f, %.3f, %.3f)",
                 (double) worst,
                 (double) basis[0][0], (double) basis[0][1], (double) basis[0][2],
                 (double) want[0][0], (double) want[0][1], (double) want[0][2]);
        check(worst < 1e-3f, "relative turn keeps the engine's row order", detail);
    }

    // Standing at the anchor itself is the camera origin, whatever the room.
    {
        float anchor[16], anchorInv[16];
        make_transform(1.1f, -2.0f, 1.4f, 3.3f, anchor);
        invert_rigid(anchor, anchorInv);

        float pos[3];
        mv_room_to_camera(anchorInv, anchor, 100.0f, pos, NULL);
        char detail[128];
        snprintf(detail, sizeof detail, "gave (%.3f, %.3f, %.3f)",
                 (double) pos[0], (double) pos[1], (double) pos[2]);
        check(fabsf(pos[0]) < 0.01f && fabsf(pos[1]) < 0.01f && fabsf(pos[2]) < 0.01f,
              "the anchor is the origin", detail);
    }
}

//-----------------------------------------------------------------------------
// Composing a bone into a scaled parent
//
// This is where the first device run actually went wrong, so it gets a test
// with the very numbers that were measured there: a parent carrying a uniform
// quarter, and a wanted frame that is a clean permutation of it. The failure
// looked like an arm pointing up and to the right while the hand was forward
// and down, which reads as a broken solver and is not one.
//-----------------------------------------------------------------------------

static void test_local_frame(void) {
    printf("==> Composing a bone into its parent\n");

    // The exact matrix the device produced, kept as a fixture. It is a quarter
    // turn about Z - row 0 is the image of local +X, and +X has gone to +Y - so
    // the only non-zero angle must be the third.
    {
        const float quarterAboutZ[3][3] = {
            {  0.0f, 1.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            {  0.0f, 0.0f, 1.0f },
        };
        short r[3];
        mv_euler_from_matrix(quarterAboutZ, r);
        char detail[140];
        snprintf(detail, sizeof detail, "gave (%d, %d, %d), wanted (0, 0, 16384)",
                 (int) r[0], (int) r[1], (int) r[2]);
        check(r[0] == 0 && r[1] == 0 && abs((int) r[2] - 16384) <= 1,
              "quarter turn about Z", detail);
    }

    // Measured on device: what the torso's own parent looks like, scaled by a
    // quarter and permuted against the camera's axes.
    const float parent[3][3] = {
        { -0.00f,  0.00f,  0.25f },
        {  0.00f,  0.25f,  0.00f },
        { -0.25f,  0.00f, -0.00f },
    };

    // The torso case verbatim, printed rather than only asserted: the device
    // reported a rotation about the bone's own axis, which cannot move where
    // that axis points, so the number itself is worth seeing.
    {
        const float up[3] = { 0.0f, 1.0f, 0.0f };
        const float left[3] = { -1.0f, 0.0f, 0.0f };
        float want[3][3];
        mv_frame_from_axes(up, left, want);

        short r[3];
        check(mv_local_frame(want, parent, r), "torso frame solves", "gave up");
        printf("      torso rotation comes out as (%d, %d, %d)\n",
               (int) r[0], (int) r[1], (int) r[2]);

        float local[3][3], composed[3][3], parentUnit[3][3];
        mv_matrix_from_euler(r, local);
        for (int rr = 0; rr < 3; rr++) {
            const float len = sqrtf(parent[rr][0]*parent[rr][0]
                                    + parent[rr][1]*parent[rr][1]
                                    + parent[rr][2]*parent[rr][2]);
            for (int c = 0; c < 3; c++) { parentUnit[rr][c] = parent[rr][c] / len; }
        }
        mv_basis_mul(local, parentUnit, composed);

        char detail[160];
        snprintf(detail, sizeof detail,
                 "torso +X came out (%.2f, %.2f, %.2f), wanted (0, 1, 0)",
                 (double) composed[0][0], (double) composed[0][1], (double) composed[0][2]);
        check(fabsf(composed[0][1] - 1.0f) < 1e-3f, "torso points up", detail);
    }

    // A handful of frames a real solve produces.
    const float wantedAxes[][6] = {
        {  0.15f, -0.15f, -0.98f,   0.0f, 1.0f, 0.0f },
        {  0.0f,   1.0f,   0.0f,   -1.0f, 0.0f, 0.0f },
        { -0.71f, -0.71f,  0.0f,    0.0f, 0.0f, 1.0f },
        {  0.0f,   0.0f,  -1.0f,    1.0f, 0.0f, 0.0f },
    };

    for (unsigned i = 0; i < sizeof wantedAxes / sizeof wantedAxes[0]; i++) {
        float wanted[3][3];
        mv_frame_from_axes(&wantedAxes[i][0], &wantedAxes[i][3], wanted);

        short rotation[3];
        check(mv_local_frame(wanted, parent, rotation), "local frame solves",
              "mv_local_frame gave up on a well-formed parent");

        // Now do exactly what the renderer does: build the bone's matrix from
        // that triple and compose it onto the parent.
        float local[3][3];
        mv_matrix_from_euler(rotation, local);

        float parentUnit[3][3];
        for (int r = 0; r < 3; r++) {
            const float len = sqrtf(parent[r][0]*parent[r][0]
                                    + parent[r][1]*parent[r][1]
                                    + parent[r][2]*parent[r][2]);
            for (int c = 0; c < 3; c++) { parentUnit[r][c] = parent[r][c] / len; }
        }

        float composed[3][3];
        mv_basis_mul(local, parentUnit, composed);

        float worst = 0.0f;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                const float e = fabsf(composed[r][c] - wanted[r][c]);
                if (e > worst) { worst = e; }
            }
        }

        char detail[200];
        snprintf(detail, sizeof detail,
                 "case %u: bone +X came out (%.2f, %.2f, %.2f), wanted (%.2f, %.2f, %.2f)",
                 i,
                 (double) composed[0][0], (double) composed[0][1], (double) composed[0][2],
                 (double) wanted[0][0], (double) wanted[0][1], (double) wanted[0][2]);
        check(worst < 1e-3f, "bone lands in the wanted frame", detail);
    }
}

int main(void) {
    printf("Mario's arms: solver checks\n\n");
    test_local_frame();

    test_reachable_targets();
    test_out_of_reach();
    test_euler_roundtrip();
    test_elbow_follows_pole();
    test_degenerate();
    test_room_to_camera();

    printf("\n%d checks, %d failures\n", sChecks, sFailures);
    return sFailures == 0 ? 0 : 1;
}
