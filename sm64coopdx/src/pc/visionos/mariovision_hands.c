//-----------------------------------------------------------------------------
// Carrying controller poses from the renderer to the game, and standing in for
// them when there is no hardware.
//
// The arithmetic all lives in mariovision_hands_math.c, which builds for the
// host and is covered by scripts/test-hand-math.sh. What is left here is the
// part that cannot be tested that way: the handover between two threads running
// at different rates, and the synthetic source that lets the simulator exercise
// everything downstream.
//-----------------------------------------------------------------------------

#include "mariovision_hands.h"

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//-----------------------------------------------------------------------------
// Debug switches
//-----------------------------------------------------------------------------

/// Read once and remembered. getenv on every frame for every hand is a
/// surprising amount of work, and these never change while running.
static const char *env_once(const char *name) {
    return getenv(name);
}

static float env_float(const char *name, float fallback) {
    const char *v = env_once(name);
    if (!v || !*v) { return fallback; }
    return (float) atof(v);
}

/// Bitmask over the stages: 1 pose in, 2 converted, 4 the arm solve, 8 gesture
/// verdicts, 16 actions taken. Written to stderr rather than os_log, which is
/// invisible on a tethered headset.
static int hand_log_mask(void) {
    static int mask = -1;
    if (mask < 0) {
        const char *v = env_once("MARIOVISION_HANDLOG");
        mask = (v && *v) ? atoi(v) : 0;
    }
    return mask;
}

bool mariovision_hands_logging(int stage) {
    return (hand_log_mask() & stage) != 0;
}

void mariovision_hands_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

//-----------------------------------------------------------------------------
// Transport
//
// The renderer writes at display rate, the game reads at thirty. Independent
// atomics per field would let a reader take the position from one sample and
// the orientation from the next, which shows as the hand snapping somewhere
// impossible for a single frame. A seqlock keeps a sample whole: the writer
// bumps the counter to odd before touching anything and back to even after, and
// a reader that sees it move simply reads again.
//-----------------------------------------------------------------------------

typedef struct {
    _Atomic unsigned seq;
    MarioVisionHandPose pose;
} HandSlot;

static HandSlot sSlot[MV_HAND_COUNT];

/// The trigger, pushed in from the controller backend. Declared here because
/// clearing a hand has to clear it too.
static _Atomic float sTrigger[MV_HAND_COUNT];

/// What the game thread works from. Index 0 is this frame, 1 the frame before -
/// the renderer blends the two, and handing it a "previous" that the previous
/// frame was not built with is what reads as shaking rather than as motion.
static MarioVisionHandPose sLatched[MV_HAND_COUNT][2];
static bool sLatchedValid[MV_HAND_COUNT][2];
static unsigned sFrame = 0;

void mariovision_set_hand_pose(int side, const MarioVisionHandPose *pose) {
    if (side < 0 || side >= MV_HAND_COUNT || !pose) { return; }
    HandSlot *slot = &sSlot[side];

    atomic_fetch_add_explicit(&slot->seq, 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    slot->pose = *pose;
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&slot->seq, 1u, memory_order_relaxed);
}

void mariovision_clear_hand(int side) {
    if (side < 0 || side >= MV_HAND_COUNT) { return; }
    // The trigger goes with it. A controller that vanishes while its trigger
    // was held would otherwise leave that reading in place for ever, and Mario
    // would carry whatever he was carrying until something else intervened.
    atomic_store(&sTrigger[side], 0.0f);
    MarioVisionHandPose empty;
    memset(&empty, 0, sizeof empty);
    empty.quat[3] = 1.0f;
    mariovision_set_hand_pose(side, &empty);
}

/// One seqlock read. Returns false if the writer was mid-sample; the caller
/// retries rather than accepting a torn one.
static bool slot_read(const HandSlot *slot, MarioVisionHandPose *out) {
    const unsigned before = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    if (before & 1u) { return false; }
    atomic_thread_fence(memory_order_acquire);
    *out = slot->pose;
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&slot->seq, memory_order_relaxed) == before;
}

//-----------------------------------------------------------------------------
// Synthetic hands
//
// PSVR2 controllers do not exist in the simulator, so without this every part
// of the feature downstream of the pose - the arm solve, the grip state
// machine, the gesture detectors - would be unreachable except by wearing the
// headset. The motions are written directly in camera space, because the
// simulator never produces a device anchor and the room-space conversion
// therefore cannot run there at all. That conversion is checked on the host
// instead, which is the better place for it.
//
// Driven by the latch counter rather than a clock, so a screenshot taken at a
// given frame always catches the same pose.
//-----------------------------------------------------------------------------

typedef enum {
    CLIP_NONE = 0,
    CLIP_IDLE,
    CLIP_REACH,
    CLIP_PUNCH,
    CLIP_STROKE,
    CLIP_SWING,
    CLIP_LOST,
    CLIP_FIXED,
} HandClip;

static HandClip clip_selected(void) {
    static int resolved = -1;
    static HandClip clip = CLIP_NONE;
    if (resolved < 0) {
        resolved = 1;
        const char *v = env_once("MARIOVISION_HANDSCRIPT");
        if (v && *v) {
            if      (!strcmp(v, "idle"))   { clip = CLIP_IDLE; }
            else if (!strcmp(v, "reach"))  { clip = CLIP_REACH; }
            else if (!strcmp(v, "punch"))  { clip = CLIP_PUNCH; }
            else if (!strcmp(v, "stroke")) { clip = CLIP_STROKE; }
            else if (!strcmp(v, "swing"))  { clip = CLIP_SWING; }
            else if (!strcmp(v, "lost"))   { clip = CLIP_LOST; }
            else if (!strcmp(v, "fixed"))  { clip = CLIP_FIXED; }
            else {
                mariovision_hands_log("[hands] unknown MARIOVISION_HANDSCRIPT \"%s\"", v);
                clip = CLIP_NONE;
            }
        }
    }
    return clip;
}

/// Where a hand rests when nothing is happening: a little out to the side, well
/// below the eye, and slightly in front. The camera looks along -z.
static void clip_rest(int side, float out[3]) {
    const float lateral = (side == MV_HAND_RIGHT) ? 1.0f : -1.0f;
    out[0] = lateral * 24.0f;
    out[1] = -46.0f;
    out[2] = -22.0f;
}

static void clip_pose(HandClip clip, int side, unsigned frame, MarioVisionHandPose *out) {
    memset(out, 0, sizeof *out);
    out->quat[3] = 1.0f;
    out->flags = MV_HAND_TRACKED;

    // Thirty ticks a second, so a second of motion is thirty frames.
    const float t = (float) frame / 30.0f;
    float pos[3];
    clip_rest(side, pos);

    switch (clip) {
        case CLIP_IDLE:
            break;

        case CLIP_REACH: {
            // Out and back over 1.2 s, right hand only - the left stays put so
            // one screenshot shows both the moving and the resting case.
            if (side == MV_HAND_RIGHT) {
                const float phase = fmodf(t, 1.2f) / 1.2f;
                const float ease = 0.5f - 0.5f * cosf(phase * 2.0f * (float) M_PI);
                pos[1] += ease * 34.0f;
                pos[2] -= ease * 52.0f;
                out->trigger = ease > 0.7f ? 1.0f : 0.0f;
                if (out->trigger > 0.5f) { out->flags |= MV_HAND_TRIGGER; }
            }
            break;
        }

        case CLIP_PUNCH: {
            // A quarter second forward, then a slower recovery, so the detector
            // sees an extension fast enough to count and a retract that must
            // not count.
            if (side == MV_HAND_RIGHT) {
                const float phase = fmodf(t, 1.5f);
                if (phase < 0.25f) {
                    const float k = phase / 0.25f;
                    pos[1] += k * 30.0f;
                    pos[2] -= k * 70.0f;
                } else if (phase < 0.9f) {
                    const float k = 1.0f - (phase - 0.25f) / 0.65f;
                    pos[1] += k * 30.0f;
                    pos[2] -= k * 70.0f;
                }
            }
            break;
        }

        case CLIP_STROKE: {
            // Both hands together in front, then swept out and back: the shape
            // the swim detector is looking for.
            const float phase = fmodf(t, 1.6f) / 1.6f;
            const float lateral = (side == MV_HAND_RIGHT) ? 1.0f : -1.0f;
            const float reach = 0.5f - 0.5f * cosf(phase * 2.0f * (float) M_PI);
            pos[0] = lateral * (8.0f + reach * 40.0f);
            pos[1] = -30.0f + reach * 8.0f;
            pos[2] = -52.0f + reach * 34.0f;
            break;
        }

        case CLIP_SWING: {
            // A circle above the head, which is how Bowser gets wound up.
            if (side == MV_HAND_RIGHT) {
                const float a = t * 2.0f * (float) M_PI / 0.9f;
                pos[0] = cosf(a) * 38.0f;
                pos[1] = 34.0f;
                pos[2] = sinf(a) * 38.0f - 10.0f;
                out->trigger = 1.0f;
                out->flags |= MV_HAND_TRIGGER;
            }
            break;
        }

        case CLIP_LOST: {
            // Half a second tracked, half a second not. Every state machine
            // downstream has to survive this without leaving something held.
            if (fmodf(t, 1.0f) > 0.5f) {
                out->flags &= ~(unsigned) MV_HAND_TRACKED;
            }
            break;
        }

        case CLIP_FIXED: {
            // Deterministic, for a screenshot that is meant to be compared.
            const char *spec = env_once("MARIOVISION_HANDSCRIPT_FIXED");
            if (spec && *spec) {
                float v[6] = { 0, 0, 0, 0, 0, 0 };
                sscanf(spec, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
                const int base = (side == MV_HAND_RIGHT) ? 3 : 0;
                pos[0] = v[base + 0];
                pos[1] = v[base + 1];
                pos[2] = v[base + 2];
            }
            break;
        }

        case CLIP_NONE:
        default:
            out->flags = 0;
            break;
    }

    out->pos[0] = pos[0];
    out->pos[1] = pos[1];
    out->pos[2] = pos[2];
}

//-----------------------------------------------------------------------------
// Latch
//-----------------------------------------------------------------------------

void mariovision_hands_latch(void) {
    const HandClip clip = clip_selected();
    sFrame++;

    for (int side = 0; side < MV_HAND_COUNT; side++) {
        sLatched[side][1] = sLatched[side][0];
        sLatchedValid[side][1] = sLatchedValid[side][0];

        MarioVisionHandPose fresh;
        bool have = false;

        if (clip != CLIP_NONE) {
            clip_pose(clip, side, sFrame, &fresh);
            have = (fresh.flags & MV_HAND_TRACKED) != 0;
        } else {
            for (int attempt = 0; attempt < 4 && !have; attempt++) {
                have = slot_read(&sSlot[side], &fresh);
            }
            if (have) { have = (fresh.flags & MV_HAND_TRACKED) != 0; }
        }

        if (have) {
            // Units per second, from where the hand was a frame ago. The
            // platform reports its own velocity, but it is in metres and in the
            // room; this one is already in the space the game asks questions in.
            if (sLatchedValid[side][1]) {
                for (int i = 0; i < 3; i++) {
                    fresh.vel[i] = (fresh.pos[i] - sLatched[side][1].pos[i]) * 30.0f;
                }
            } else {
                fresh.vel[0] = fresh.vel[1] = fresh.vel[2] = 0.0f;
            }
            sLatched[side][0] = fresh;
            sLatchedValid[side][0] = true;
        } else {
            sLatchedValid[side][0] = false;
        }
    }

    if (mariovision_hands_logging(MV_HANDLOG_POSE) && (sFrame % 30) == 0) {
        for (int side = 0; side < MV_HAND_COUNT; side++) {
            if (!sLatchedValid[side][0]) {
                mariovision_hands_log("[hands] %s: not tracked",
                                      side == MV_HAND_LEFT ? "left" : "right");
                continue;
            }
            const MarioVisionHandPose *p = &sLatched[side][0];
            mariovision_hands_log(
                "[hands] %s: pos %.1f %.1f %.1f  vel %.1f %.1f %.1f  trigger %.2f",
                side == MV_HAND_LEFT ? "left" : "right",
                (double) p->pos[0], (double) p->pos[1], (double) p->pos[2],
                (double) p->vel[0], (double) p->vel[1], (double) p->vel[2],
                (double) p->trigger);
        }
    }
}

bool mariovision_hands_get(int side, bool prevFrame, MarioVisionHandPose *out) {
    if (side < 0 || side >= MV_HAND_COUNT) { return false; }
    const int slot = prevFrame ? 1 : 0;

    // Before the second frame there is no previous pose. Handing back this
    // frame's is right: the two agree, so the blend is a no-op and the arm
    // simply appears where it belongs instead of sweeping in from nowhere.
    const int use = (prevFrame && !sLatchedValid[side][1]) ? 0 : slot;
    if (!sLatchedValid[side][use]) { return false; }
    if (out) { *out = sLatched[side][use]; }
    return true;
}

/// Defaults: what is reachable with the body stays with the body, because
/// having a button do the same thing quietly bypasses everything the tracked
/// hands are for. Swimming is the exception - a stroke is tiring over a long
/// swim, and the button is a kindness.
static _Atomic int sScheme[MV_DO_COUNT] = {
    MV_SCHEME_BOTH,      // swimming
    MV_SCHEME_PHYSICAL,  // punching
    MV_SCHEME_PHYSICAL,  // carrying
};

static _Atomic bool sAnnounce = false;

void mariovision_set_announce(bool on) { atomic_store(&sAnnounce, on); }
bool mariovision_announce(void) { return atomic_load(&sAnnounce); }

void mariovision_set_scheme(int what, int scheme) {
    if (what < 0 || what >= MV_DO_COUNT) { return; }
    if (scheme < MV_SCHEME_PHYSICAL || scheme > MV_SCHEME_BOTH) { return; }
    atomic_store(&sScheme[what], scheme);
}

int mariovision_scheme(int what) {
    if (what < 0 || what >= MV_DO_COUNT) { return MV_SCHEME_BOTH; }
    return atomic_load(&sScheme[what]);
}

bool mariovision_scheme_physical(int what) {
    const int s = mariovision_scheme(what);
    return s == MV_SCHEME_PHYSICAL || s == MV_SCHEME_BOTH;
}

bool mariovision_scheme_button(int what) {
    const int s = mariovision_scheme(what);
    return s == MV_SCHEME_BUTTON || s == MV_SCHEME_BOTH;
}

void mariovision_hands_set_trigger(int side, float value) {
    if (side < 0 || side >= MV_HAND_COUNT) { return; }
    atomic_store(&sTrigger[side], value);
}

float mariovision_hands_trigger(int side) {
    if (side < 0 || side >= MV_HAND_COUNT) { return 0.0f; }
    return atomic_load(&sTrigger[side]);
}

bool mariovision_hand_frame(int side, bool prevFrame, float out[3][3]) {
    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, prevFrame, &pose)) { return false; }

    mv_quat_to_matrix(pose.quat, out);

    MarioVisionHandFit fit;
    mariovision_hands_get_fit(&fit);
    if (fit.handRotate[0] == 0.0f && fit.handRotate[1] == 0.0f
        && fit.handRotate[2] == 0.0f) {
        return true;
    }

    const short correction[3] = {
        (short) (int) (fit.handRotate[0] / 360.0f * 65536.0f),
        (short) (int) (fit.handRotate[1] / 360.0f * 65536.0f),
        (short) (int) (fit.handRotate[2] / 360.0f * 65536.0f),
    };
    float fix[3][3];
    mv_matrix_from_euler(correction, fix);
    mv_basis_mul((const float (*)[3]) fix, (const float (*)[3]) out, out);
    return true;
}

bool mariovision_hands_active(void) {
    return sLatchedValid[MV_HAND_LEFT][0] || sLatchedValid[MV_HAND_RIGHT][0];
}

//-----------------------------------------------------------------------------
// Fit
//
// Measured on the headset, then written down. These were sliders for a while,
// which is how the numbers were found - but a dial that is never touched again
// is just a way for a value to be wrong on someone else's device, so once the
// answers were known the dials came out.
//
// The hands are shown without arms. Every millimetre by which a hand missed its
// controller came from the arm - a guessed shoulder, a limb slightly too short
// to reach, an elbow that had to go somewhere - and with the arm gone the hand
// is placed directly and cannot miss.
//-----------------------------------------------------------------------------

/// Six tenths of the hand the game itself draws.
#define MV_HAND_SCALE 0.6f

/// Where the hand sits relative to the grip point, in the room. Eight
/// centimetres down and twelve back. This one does not turn with the hand, and
/// that is the point: "the hands sit too high" is a statement about the room.
#define MV_HAND_WORLD_X   0.0f
#define MV_HAND_WORLD_Y  -8.0f
#define MV_HAND_WORLD_Z -12.0f

/// Between the controller's grip frame and Mario's hand: half a turn of pitch
/// and a quarter of yaw. Hardware geometry, so found rather than derived.
#define MV_HAND_PITCH 180.0f
#define MV_HAND_YAW    90.0f
#define MV_HAND_ROLL    0.0f

void mariovision_hands_get_fit(MarioVisionHandFit *out) {
    if (!out) { return; }
    out->handScale = MV_HAND_SCALE;
    out->handOffset[0] = out->handOffset[1] = out->handOffset[2] = 0.0f;
    out->handWorldOffset[0] = MV_HAND_WORLD_X;
    out->handWorldOffset[1] = MV_HAND_WORLD_Y;
    out->handWorldOffset[2] = MV_HAND_WORLD_Z;
    out->handRotate[0] = MV_HAND_PITCH;
    out->handRotate[1] = MV_HAND_YAW;
    out->handRotate[2] = MV_HAND_ROLL;
    out->handsOnly = true;

    // Only consulted with the arms switched back on, which nothing does today.
    out->armReach = 62.0f;
    out->armThickness = 1.0f;
    out->shoulderHalf = 18.0f;
    out->shoulderDrop = 32.0f;
    out->shoulderBack = 16.0f;
    out->elbowOut = 0.35f;
    out->elbowBack = 0.15f;
}

/// Set while the door is being handed back to the game's own handler, which
/// would otherwise see the guard below and refuse the door it was just given.
bool gMarioVisionDoorHandBack = false;

static _Atomic bool sHandDoors = true;

void mariovision_set_hand_doors(bool on) {
    atomic_store(&sHandDoors, on);
}

static _Atomic int sLedgeMode = MV_LEDGE_FIRST_PERSON;

void mariovision_set_ledge_mode(int mode) {
    if (mode < MV_LEDGE_FIRST_PERSON || mode > MV_LEDGE_BY_HAND) { return; }
    atomic_store(&sLedgeMode, mode);
}

int mariovision_ledge_mode(void) { return atomic_load(&sLedgeMode); }

static _Atomic int sHangMode = MV_HANG_FIRST_PERSON;

void mariovision_set_hang_mode(int mode) {
    if (mode < MV_HANG_FIRST_PERSON || mode > MV_HANG_THIRD_PERSON) { return; }
    atomic_store(&sHangMode, mode);
}

int mariovision_hang_mode(void) { return atomic_load(&sHangMode); }

static _Atomic bool sHandClimb = false;
static _Atomic bool sFlyFirstPerson = false;

void mariovision_set_hand_climb(bool on) { atomic_store(&sHandClimb, on); }
bool mariovision_hand_climb(void) { return atomic_load(&sHandClimb); }

void mariovision_set_fly_first_person(bool on) { atomic_store(&sFlyFirstPerson, on); }
bool mariovision_fly_first_person(void) { return atomic_load(&sFlyFirstPerson); }

bool mariovision_hand_doors_active(void) {
    if (gMarioVisionDoorHandBack) { return false; }
    if (!atomic_load(&sHandDoors)) { return false; }
    extern bool mariovision_first_person_active(void);
    return mariovision_first_person_active()
        && mariovision_hands_active()
        && mariovision_scheme_physical(MV_DO_CARRY);
}

bool mariovision_hands_only(void) {
    return mariovision_hands_active();
}

float mariovision_hands_reach_scale(void) {
    static float scale = -1.0f;
    if (scale < 0.0f) {
        // One by default, and that is the point: the hands belong exactly where
        // the controllers are, which is what was asked for.
        //
        // An earlier plan had this amplified, on the reasoning that Mario's
        // arms outreach a person's and one should not have to lunge. Measuring
        // the graph killed that: it shrinks his model by a quarter on the way
        // out, so his arm is 31 centimetres, not 125. The arms are lengthened
        // to human ones instead - see mariovision_hands_arms.c - which leaves
        // this dial for taste rather than for correction.
        scale = env_float("MARIOVISION_HANDS_REACH", 1.0f);
        if (scale < 0.5f) { scale = 0.5f; }
        if (scale > 3.0f) { scale = 3.0f; }
    }
    return scale;
}
