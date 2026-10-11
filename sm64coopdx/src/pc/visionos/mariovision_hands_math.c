//-----------------------------------------------------------------------------
// The arm solver and the engine's angle conventions.
//
// This file deliberately knows nothing about the engine, ARKit or Objective-C:
// it is plain C over float arrays, so scripts/test-hand-math.sh can build it for
// the host and check the solver against forward kinematics. Everything here is
// the sort of code that is wrong by a sign or a half-turn and looks plausible
// either way, and this project has paid for that lesson more than once.
//-----------------------------------------------------------------------------

#include "mariovision_hands.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//-----------------------------------------------------------------------------
// Small vector helpers
//-----------------------------------------------------------------------------

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3]) {
    const float x = a[1] * b[2] - a[2] * b[1];
    const float y = a[2] * b[0] - a[0] * b[2];
    const float z = a[0] * b[1] - a[1] * b[0];
    out[0] = x; out[1] = y; out[2] = z;
}

/// Normalise in place. Returns the original length, so callers can tell a
/// degenerate vector from a short one without measuring twice.
static float normalize3(float v[3]) {
    const float len = sqrtf(dot3(v, v));
    if (len > 1e-6f) {
        const float k = 1.0f / len;
        v[0] *= k; v[1] *= k; v[2] *= k;
    }
    return len;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Arc tangent of y/x - and deliberately not the one called atan2f.
///
/// coopdx defines its own global function of that exact name (math_util.c),
/// built on the game's atan2s, whose SM64 angle convention effectively swaps
/// the arguments: atan2s(1, 0) is zero where the standard function gives a
/// quarter turn. Being a plain global symbol, it displaces the C library's for
/// the whole binary - including this file, which never asked for it and is not
/// even compiled against the engine's headers.
///
/// The cost of not knowing that was an afternoon: every angle here came out in
/// the wrong slot on device while the host test, linking only libm, insisted
/// the same code was correct. The double-precision atan2 has no such twin, so
/// going through it is what makes this file mean the same thing in both places.
static float mv_atan2(float y, float x) {
    return (float) atan2((double) y, (double) x);
}

//-----------------------------------------------------------------------------
// Angles
//
// The engine stores angles as s16 over a full turn and builds matrices with
// mtxf_rotate_xyz_and_translate: x first, then y, then z, in the row-vector
// layout where row i is the image of local axis i. Row 0 therefore reads
// (cy*cz, cy*sz, -sy) - which is worth stating plainly, because it means the
// x component is a roll about the bone's own axis and does not affect where the
// bone points. That is what makes the elbow swivel a free parameter rather than
// something to solve for.
//-----------------------------------------------------------------------------

static short angle_from_radians(float rad) {
    const float turns = rad / (2.0f * (float) M_PI);
    return (short) (int) lrintf(turns * 65536.0f);
}

static float radians_from_angle(short a) {
    return (float) a * (2.0f * (float) M_PI) / 65536.0f;
}

void mv_matrix_from_euler(const short r[3], float out[3][3]) {
    const float sx = sinf(radians_from_angle(r[0]));
    const float cx = cosf(radians_from_angle(r[0]));
    const float sy = sinf(radians_from_angle(r[1]));
    const float cy = cosf(radians_from_angle(r[1]));
    const float sz = sinf(radians_from_angle(r[2]));
    const float cz = cosf(radians_from_angle(r[2]));

    out[0][0] = cy * cz;
    out[0][1] = cy * sz;
    out[0][2] = -sy;

    out[1][0] = sx * sy * cz - cx * sz;
    out[1][1] = sx * sy * sz + cx * cz;
    out[1][2] = sx * cy;

    out[2][0] = cx * sy * cz + sx * sz;
    out[2][1] = cx * sy * sz - sx * cz;
    out[2][2] = cx * cy;
}

void mv_euler_from_matrix(const float m[3][3], short out[3]) {
    const float sy = clampf(-m[0][2], -1.0f, 1.0f);
    const float y = asinf(sy);

    float x, z;
    // Straight up or straight down, where the y rotation has swallowed one of
    // the other two and only their sum is recoverable. Give it all to x: the
    // bone points the same way either way, and z is the one that would jump.
    if (fabsf(m[0][2]) > 0.99999f) {
        x = mv_atan2(-m[2][1], m[1][1]);
        z = 0.0f;
    } else {
        x = mv_atan2(m[1][2], m[2][2]);
        z = mv_atan2(m[0][1], m[0][0]);
    }

    out[0] = angle_from_radians(x);
    out[1] = angle_from_radians(y);
    out[2] = angle_from_radians(z);
}

void mv_frame_from_axes(const float x[3], const float z[3], float out[3][3]) {
    float ax[3] = { x[0], x[1], x[2] };
    normalize3(ax);

    // Take z only as a hint and re-derive it, so a caller that hands over
    // something not quite perpendicular still gets an orthonormal frame back
    // rather than a subtly sheared one.
    float ay[3];
    cross3(z, ax, ay);
    if (normalize3(ay) < 1e-6f) {
        // z was parallel to x. Any perpendicular will do; pick the axis that
        // x leans on least.
        const float fallback[3] = {
            fabsf(ax[0]) < 0.9f ? 1.0f : 0.0f,
            fabsf(ax[0]) < 0.9f ? 0.0f : 1.0f,
            0.0f
        };
        cross3(fallback, ax, ay);
        normalize3(ay);
    }

    float az[3];
    cross3(ax, ay, az);
    normalize3(az);

    for (int i = 0; i < 3; i++) {
        out[0][i] = ax[i];
        out[1][i] = ay[i];
        out[2][i] = az[i];
    }
}

void mv_quat_to_matrix(const float q[4], float out[3][3]) {
    float n[4] = { q[0], q[1], q[2], q[3] };
    const float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2] + n[3]*n[3]);
    if (len > 1e-6f) {
        const float k = 1.0f / len;
        n[0] *= k; n[1] *= k; n[2] *= k; n[3] *= k;
    } else {
        n[0] = n[1] = n[2] = 0.0f; n[3] = 1.0f;
    }

    const float x = n[0], y = n[1], z = n[2], w = n[3];

    // Row i is the image of local axis i, matching the engine's layout.
    out[0][0] = 1.0f - 2.0f * (y*y + z*z);
    out[0][1] = 2.0f * (x*y + z*w);
    out[0][2] = 2.0f * (x*z - y*w);

    out[1][0] = 2.0f * (x*y - z*w);
    out[1][1] = 1.0f - 2.0f * (x*x + z*z);
    out[1][2] = 2.0f * (y*z + x*w);

    out[2][0] = 2.0f * (x*z + y*w);
    out[2][1] = 2.0f * (y*z - x*w);
    out[2][2] = 1.0f - 2.0f * (x*x + y*y);
}

//-----------------------------------------------------------------------------
// Room space to game camera space
//
// Kept here rather than on the Swift side, and that is a deliberate change of
// plan. The intention had been to write the synthetic test motions in room
// space so the conversion would be exercised along with everything else - but
// the simulator never produces a device anchor, so the chain cannot run there
// at all. Proving it in the simulator was never on the table. Proving it on the
// host is, as long as the arithmetic lives somewhere a test can reach.
//
// Matrices arrive column-major, the way simd hands them over: element (row r,
// column c) sits at m[c * 4 + r].
//-----------------------------------------------------------------------------

static float mat4_at(const float m[16], int row, int col) {
    return m[col * 4 + row];
}

void mv_room_to_camera(const float anchorInverse[16], const float roomPose[16],
                       float metresToUnits, float outPos[3], float outQuat[4]) {
    // anchorInverse * roomPose, but only the parts that are wanted: the
    // translation column and the rotation basis.
    float basis[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += mat4_at(anchorInverse, r, k) * mat4_at(roomPose, k, c);
            }
            basis[c][r] = sum;   // transposed: the engine wants row i = image of axis i
        }
    }

    if (outPos) {
        for (int r = 0; r < 3; r++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += mat4_at(anchorInverse, r, k) * mat4_at(roomPose, k, 3);
            }
            outPos[r] = sum * metresToUnits;
        }
    }

    if (outQuat) {
        // Shepperd's method: pick whichever diagonal term is largest, so the
        // square root is never taken of something near zero.
        const float m00 = basis[0][0], m11 = basis[1][1], m22 = basis[2][2];
        const float trace = m00 + m11 + m22;
        if (trace > 0.0f) {
            const float s = sqrtf(trace + 1.0f) * 2.0f;
            outQuat[3] = 0.25f * s;
            outQuat[0] = (basis[1][2] - basis[2][1]) / s;
            outQuat[1] = (basis[2][0] - basis[0][2]) / s;
            outQuat[2] = (basis[0][1] - basis[1][0]) / s;
        } else if (m00 > m11 && m00 > m22) {
            const float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
            outQuat[3] = (basis[1][2] - basis[2][1]) / s;
            outQuat[0] = 0.25f * s;
            outQuat[1] = (basis[1][0] + basis[0][1]) / s;
            outQuat[2] = (basis[2][0] + basis[0][2]) / s;
        } else if (m11 > m22) {
            const float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
            outQuat[3] = (basis[2][0] - basis[0][2]) / s;
            outQuat[0] = (basis[1][0] + basis[0][1]) / s;
            outQuat[1] = 0.25f * s;
            outQuat[2] = (basis[2][1] + basis[1][2]) / s;
        } else {
            const float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
            outQuat[3] = (basis[0][1] - basis[1][0]) / s;
            outQuat[0] = (basis[2][0] + basis[0][2]) / s;
            outQuat[1] = (basis[2][1] + basis[1][2]) / s;
            outQuat[2] = 0.25f * s;
        }
    }
}

//-----------------------------------------------------------------------------
// Composing a bone into its parent
//
// Two inverses are needed here and they are not interchangeable. Turning a
// point into the parent's frame wants the parent's scale kept, because the
// local units it produces are about to be scaled straight back. Turning a
// *frame* into it must not: the graph carries a uniform quarter, and an inverse
// that keeps it comes out at four times size - meaningless to pull Euler angles
// out of, since the extraction reads a sine directly off a matrix element and
// needs unit rows. Measured on device, that mistake left the arm pointing up
// and to the right while the hand was forward and down.
//-----------------------------------------------------------------------------

void mv_basis_mul(const float a[3][3], const float b[3][3], float out[3][3]) {
    float tmp[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            tmp[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        }
    }
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) { out[r][c] = tmp[r][c]; }
    }
}

bool mv_basis_rotation_inverse(const float m[3][3], float out[3][3]) {
    for (int r = 0; r < 3; r++) {
        const float len = sqrtf(m[r][0] * m[r][0] + m[r][1] * m[r][1] + m[r][2] * m[r][2]);
        if (len < 1e-6f) { return false; }
        const float k = 1.0f / len;
        // Orthonormal once the scale is divided out, so the inverse is the
        // transpose.
        for (int c = 0; c < 3; c++) { out[c][r] = m[r][c] * k; }
    }
    return true;
}

bool mv_local_frame(const float wanted[3][3], const float parent[3][3],
                    short outRotation[3]) {
    float inv[3][3];
    if (!mv_basis_rotation_inverse(parent, inv)) { return false; }

    // Row vectors: a bone's own frame times its parent's gives the frame in the
    // space the parent lives in. So to end up at `wanted`, the bone's own frame
    // has to be wanted times the parent's inverse.
    float local[3][3];
    mv_basis_mul(wanted, inv, local);
    mv_euler_from_matrix((const float (*)[3]) local, outRotation);
    return true;
}

//-----------------------------------------------------------------------------
// Two-bone IK
//
// Law of cosines on the triangle shoulder-elbow-target. The only choice the
// geometry leaves open is which way round the shoulder-to-target line the elbow
// falls, and that is what the pole vector settles.
//-----------------------------------------------------------------------------

bool mv_arm_solve(const float shoulder[3], const float target[3],
                  float l1, float l2, const float pole[3],
                  MarioVisionArmSolution *out) {
    if (!out) { return false; }

    float u[3] = {
        target[0] - shoulder[0],
        target[1] - shoulder[1],
        target[2] - shoulder[2]
    };
    const float d = normalize3(u);
    out->reach = d;
    if (d < 1e-3f) { return false; }

    // Clamping here is the whole out-of-reach story. Past l1+l2 the first
    // cosine saturates at 1, so alpha is zero and the arm points straight at
    // the target; the second saturates at -1, so the bend is zero and the arm
    // is straight. The hand stops short of the controller, which is the honest
    // thing to show - the alternative is a limb that dislocates.
    const float cosAlpha = clampf((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    const float cosElbow = clampf((l1 * l1 + l2 * l2 - d * d) / (2.0f * l1 * l2), -1.0f, 1.0f);
    const float alpha = acosf(cosAlpha);
    out->bend = (float) M_PI - acosf(cosElbow);

    // The component of the pole across the shoulder-to-target line: that is the
    // direction the elbow is pushed towards.
    float b[3] = { pole[0], pole[1], pole[2] };
    const float along = dot3(b, u);
    b[0] -= along * u[0];
    b[1] -= along * u[1];
    b[2] -= along * u[2];
    if (normalize3(b) < 1e-4f) {
        // Pole parallel to the arm - it says nothing about which way to bend.
        // Any perpendicular keeps the solve well defined.
        const float alt[3] = { 0.0f, 1.0f, 0.0f };
        float fallback[3];
        cross3(u, alt, fallback);
        if (normalize3(fallback) < 1e-4f) {
            const float alt2[3] = { 1.0f, 0.0f, 0.0f };
            cross3(u, alt2, fallback);
            normalize3(fallback);
        }
        cross3(fallback, u, b);
        normalize3(b);
    }

    const float sinAlpha = sinf(alpha);
    for (int i = 0; i < 3; i++) {
        out->upper[i] = cosAlpha * u[i] + sinAlpha * b[i];
    }
    normalize3(out->upper);

    // (u, b, n) right-handed. The forearm turns about this normal, which in the
    // bone's own frame is a rotation about local +Z.
    cross3(u, b, out->normal);
    normalize3(out->normal);

    return true;
}

void mv_arm_bones(const MarioVisionArmSolution *sol, MarioVisionArmBones *out) {
    if (!sol || !out) { return; }

    // The upper arm runs along the bone's own +X and the arm bends in the plane
    // whose normal the solve found, so putting that normal on local +Z leaves
    // the elbow turning about +Z alone.
    float m[3][3];
    mv_frame_from_axes(sol->upper, sol->normal, m);
    mv_euler_from_matrix(m, out->shoulder);

    // Negative: a positive turn about +Z swings the forearm away from the
    // target rather than towards it. Checked, not reasoned - the isoceles case
    // where both bones are equal and the elbow makes a right angle is the one
    // that settles it, and the test walks it.
    out->forearm[0] = 0;
    out->forearm[1] = 0;
    out->forearm[2] = angle_from_radians(-sol->bend);
}
