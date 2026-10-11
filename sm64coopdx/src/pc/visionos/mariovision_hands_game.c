//-----------------------------------------------------------------------------
// What the tracked hands do to the world.
//
// The hands arrive in the game camera's own space; the world is somewhere else
// entirely, so the first job here is the short walk between them. After that
// come the gestures - punching first, because it needs nothing the platform
// does not already report and no new mechanic in the game.
//
// This runs on the game thread, once per simulated frame, between the floors
// and the interactions. That position is not arbitrary: update_mario_inputs
// clears m->input and the upper byte of m->flags just before, so anything
// injected earlier would be wiped, and mario_process_interactions has to still
// be ahead so it sees what was set.
//-----------------------------------------------------------------------------

#include <stdio.h>

#include "mariovision_hands.h"
#include "mariovision_portal.h"

#include "sm64.h"
#include "types.h"
#include "audio/external.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game/interaction.h"
#include "game/first_person_cam.h"
#include "game/camera.h"
#include "game/object_list_processor.h"
#include "game/object_helpers.h"
#include "game/mario.h"
#include "game/mario_step.h"
#include "game/mario_actions_automatic.h"
#include "game/save_file.h"
#include "course_table.h"
#include "behavior_data.h"
#include "pc/lua/smlua_hooks.h"
#include "pc/djui/djui_popup.h"

#include <math.h>

/// Not in interaction.h, though process_interaction beside it is. Declared here
/// rather than added there: this is the only caller outside that file, and
/// widening a shared header for one visionOS path invites the two to drift.
u32 attack_object(struct MarioState *m, struct Object *o, s32 interaction);

/// Defined further down, with the rest of the door handling; used from the grab
/// code above it, which reaches for a door before it reaches for a crate.
static bool door_try_grab(struct MarioState *m, int side, const Vec3f hand);
static bool pole_try_grab(struct MarioState *m, int side, const Vec3f hand);
static bool ledge_try_grab(struct MarioState *m, int side, const Vec3f hand);
static bool ledge_hanging(struct MarioState *m);
static bool ledge_in_hand(int side);
/// Defined in mario_actions_automatic.c and not declared anywhere.
extern void add_tree_leaf_particles(struct MarioState *m);

//-----------------------------------------------------------------------------
// Camera space into the world
//
// The camera sits in Mario's head and, in this mode, has no pitch at all - the
// looking around happens in the view matrix on the render thread, not here. So
// the transform is a single rotation about the up axis, and the game's own
// sines are used for it rather than libm's, which keeps this consistent with
// every angle the rest of the engine computes.
//-----------------------------------------------------------------------------

static void camera_to_world(const float cam[3], Vec3f out) {
    const s16 yaw = gFirstPersonCamera.yaw;
    const f32 s = sins(yaw), c = coss(yaw);

    // Less whatever the view has been shifted by for comfort's sake. The player
    // did not move, so their hands did not either.
    float shift[3];
    mariovision_pole_eye_offset(shift);

    // The camera looks along its own -z, which in the world is
    // -(sin yaw, 0, cos yaw); its right is (cos yaw, 0, -sin yaw).
    out[0] = gLakituState.pos[0] - shift[0] + cam[0] * c + cam[2] * s;
    out[1] = gLakituState.pos[1] - shift[1] + cam[1];
    out[2] = gLakituState.pos[2] - shift[2] - cam[0] * s + cam[2] * c;
}

/// A direction only, so the camera's position stays out of it.
static void camera_dir_to_world(const float cam[3], Vec3f out) {
    const s16 yaw = gFirstPersonCamera.yaw;
    const f32 s = sins(yaw), c = coss(yaw);
    out[0] = cam[0] * c + cam[2] * s;
    out[1] = cam[1];
    out[2] = -cam[0] * s + cam[2] * c;
}

//-----------------------------------------------------------------------------
// Punching
//
// Deliberately not routed through ACT_PUNCHING. That action sets Mario's
// forward velocity from its own table and locks the action group for its whole
// length, so punching while walking - the entire point of doing it with the arm
// - would stop being possible.
//
// Three things have to agree before a swing counts, and each one rejects a
// different false positive:
//
//   speed      - measured in the room, where Mario's own movement simply is not
//                present: the poses are relative to the pose the mode was
//                entered with, so jumping moves him and not the controller
//   direction  - the fist has to lead the movement, so an arm swinging past
//                while walking does not count
//   extension  - the hand has to be getting further from the shoulder, which
//                rejects the pull back after a real punch
//-----------------------------------------------------------------------------

/// Units per frame at thirty a second. A brisk jab moves the hand about a metre
/// in a fifth of a second, which is 100 units over six frames.
/// Units per frame at thirty a second, with a hundred units to the metre - so
/// this is under two metres a second.
///
/// Set from a whole session of ordinary swings, which came in between 12 and 25
/// and never once above. The first guess was 16, the second 45, and both were
/// arrived at by picturing a punch rather than measuring one: 45 works out at
/// 7.5 m/s, which is a professional boxer, not somebody in a headset.
///
/// Ten worked, and then a gentle jab still went unanswered - so six, which is
/// about a jab and still far above an arm merely carried along. Mario's own
/// movement is subtracted before this is compared, so walking cannot reach it
/// however fast he runs.
#define PUNCH_SPEED       6.0f

/// How straight the movement has to be. Cosine, so this is within about 70
/// degrees - loose, deliberately. The hand's own axis is not quite the punching
/// direction and how far it is off depends on how the controller is held;
/// measured leads ran from 0.2 to 0.75 on swings that were all meant. What this
/// still rejects is the case it exists for: an arm travelling backwards, where
/// the lead goes negative.
#define PUNCH_LEAD       0.35f
/// Or, failing that, how much of the movement simply goes forwards. An arc
/// swung from beside the head satisfies this where it cannot satisfy the fist
/// leading, and an arm swinging past while walking satisfies neither.
#define PUNCH_FORWARD    0.55f
/// Frames a hand is ignored after it has thrown one.
#define PUNCH_LOCKOUT    10
/// And how slow it has to become before it may throw another. Below the
/// trigger, necessarily: leave it above and a hand that stays quick after a
/// punch re-arms while still moving fast enough to fire again, and one swing
/// lands as three.
#define PUNCH_REARM       3.0f
/// How far from the fist a target may be, on top of its own radius.
///
/// Sixty was arm's length thinking, and arm's length is the wrong measure: the
/// fist already sits about ninety units out from Mario, and walking any closer
/// to a goomba means being hurt by it before the punch can land. Ninety leaves
/// room to strike from where one can safely stand.
#define PUNCH_RANGE      90.0f
/// Within this of an enemy's own edge, the swing counts however it was aimed.
///
/// Measured: a goomba fifty-one units from the fist - a hand's breadth, close
/// enough to be touching - was thrown out for an aim of -0.18. From that close
/// the enemy lies beside the fist rather than in front of it, so the direction
/// of travel says nothing about whether contact was made. One cannot miss what
/// one is touching, and asking for aim at contact range was simply the wrong
/// question.
#define PUNCH_CONTACT    45.0f
/// How well the swing has to be aimed at it, once it is further off than that. The underwater punch uses 0.6 for
/// the same question - the only three-dimensional aim test the game already had
/// - but that one is measured from Mario's centre, where a target subtends far
/// less. From a fist already out at arm's length the same cone is much tighter
/// than it sounds, so this is looser.
#define PUNCH_AIM        0.35f

/// How long after a swing begins the fist may still connect.
///
/// A punch is a movement, not an instant. Looking for a target only at the
/// moment the swing is recognised means looking while the fist is still coming
/// off the body, a good arm's length short of where it will end up - which is
/// why a swing that plainly went at a goomba sometimes met nothing at all. Ten
/// frames is a third of a second, about as long as a real punch takes to
/// arrive.
#define PUNCH_WINDOW     10

typedef struct {
    Vec3f prevPos;
    bool hasPrev;
    int lockout;
    bool armed;
    /// Frames left in which this swing can still land.
    int striking;
    /// The direction it was thrown in, held so a strike that slows near the
    /// end is still judged by where it was going.
    Vec3f strikeDir;
} PunchState;

static PunchState sPunch[MV_HAND_COUNT];

/// Which hand is mid-punch, so the handler can knock the right one. The handler
/// is called synchronously from the tick below, so this cannot be stale.
static int sLastPunchHand = MV_HAND_RIGHT;

/// The one handler the injected punch goes through, so the Lua hooks and the
/// multiplayer mirroring both see it as the interaction it is.
static u32 mv_punch_handler(struct MarioState *m, UNUSED u32 interactType,
                            struct Object *o) {
    // A bully is not damaged, it is sent flying, and the flight is something
    // the game sets rather than something attack_object works out: a heading
    // and a speed inversely proportional to how big the thing is. Without them
    // a punch that landed did nothing visible at all.
    //
    // The heading comes from the punch and not from where Mario faces, which is
    // what interact_bully uses. Standing still and swinging sideways ought to
    // send it sideways; taking his facing would send it wherever the head
    // happened to be pointing.
    if (o->oInteractType & INTERACT_BULLY) {
        Vec3f dir;
        camera_dir_to_world(sPunch[sLastPunchHand].strikeDir, dir);
        if (dir[0] * dir[0] + dir[2] * dir[2] > 1e-4f) {
            o->oMoveAngleYaw = atan2s(dir[2], dir[0]);
        } else {
            o->oMoveAngleYaw = m->faceAngle[1];
        }
        o->oForwardVel = 3392.0f / (o->hitboxRadius > 1.0f ? o->hitboxRadius : 1.0f);
    }

    attack_object(m, o, INT_PUNCH);
    // The global source, not the object's camera-relative one. In first person
    // the camera sits inside Mario's head, so a sound positioned against him is
    // being placed at the listener - which is exactly where a panned sound has
    // nothing left to say, and was why none of this was audible.
    play_sound(SOUND_ACTION_HIT_2, gGlobalSoundSource);

    mariovision_hand_haptic(sLastPunchHand, 1.0f, 0.8f);

    // And emphatically not bounce_back_from_attack. That shoves Mario 48 units
    // backwards and shakes the camera, which on a screen reads as impact and in
    // a headset reads as nausea - the viewer's own head has not moved, and the
    // world lurching without them is exactly what one must not do.
    return TRUE;
}

/// Whatever is closest to the fist and worth hitting.
static struct Object *punch_target(const Vec3f fist, const Vec3f dir) {
    struct Object *best = NULL;
    f32 bestDist = 1e9f;
    int seen = 0, inRange = 0;
    f32 nearest = 1e9f, nearestAim = 0.0f;

    for (int list = 0; list < NUM_OBJ_LISTS; list++) {
        if (list != OBJ_LIST_GENACTOR && list != OBJ_LIST_PUSHABLE
            && list != OBJ_LIST_DESTRUCTIVE) {
            continue;
        }
        struct Object *head = (struct Object *) &gObjectLists[list];
        for (struct Object *o = (struct Object *) head->header.next;
             o != head; o = (struct Object *) o->header.next) {
            if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED) { continue; }
            // Bullies among them. They are the round things in the lava that
            // one shoves off the edge, and they carry an interact type of their
            // own - so with only the four here they were never a target at all,
            // however well one hit them.
            if (!(o->oInteractType & (INTERACT_BOUNCE_TOP | INTERACT_BOUNCE_TOP2
                                      | INTERACT_KOOPA | INTERACT_DAMAGE
                                      | INTERACT_BULLY))) {
                continue;
            }
            seen++;

            // To the nearest point on the enemy, not to its middle.
            //
            // A goomba is flat: its centre sits just above the ground while the
            // fist is at chest height, so standing right in front of one the
            // direction to its middle is almost straight down while the punch
            // travels forwards - and the aim test threw it out. Only from
            // several metres away did the angle flatten enough to pass, which
            // is exactly backwards from how punching should feel.
            //
            // An enemy is a cylinder, so it is measured as one: take the point
            // on its axis level with the fist, bounded by its own height.
            const f32 top = o->oPosY + o->hitboxHeight;
            const f32 level = (fist[1] < o->oPosY) ? o->oPosY
                            : (fist[1] > top ? top : fist[1]);
            Vec3f to = {
                o->oPosX - fist[0],
                level - fist[1],
                o->oPosZ - fist[2],
            };
            const f32 dist = sqrtf(to[0]*to[0] + to[1]*to[1] + to[2]*to[2]);
            if (dist < nearest) {
                nearest = dist;
                nearestAim = (dist > 1e-3f)
                    ? (to[0]*dir[0] + to[1]*dir[1] + to[2]*dir[2]) / dist : 1.0f;
            }
            if (dist > o->hitboxRadius + PUNCH_RANGE) { continue; }
            inRange++;

            // Close enough to be touching: the swing counts, aimed or not.
            if (dist <= o->hitboxRadius + PUNCH_CONTACT) {
                if (dist < bestDist) { best = o; bestDist = dist; }
                continue;
            }
            if (dist > bestDist) { continue; }
            if (dist < 1e-3f) { best = o; bestDist = dist; continue; }

            // Aimed at, in three dimensions rather than by heading alone: the
            // whole point of a tracked hand is that it can punch upwards.
            const f32 aim = (to[0]*dir[0] + to[1]*dir[1] + to[2]*dir[2]) / dist;
            if (aim < PUNCH_AIM) { continue; }

            best = o;
            bestDist = dist;
        }
    }
    if (mariovision_hands_logging(MV_HANDLOG_ACTION)) {
        mariovision_hands_log(
            "[hands] target search: %d attackable, %d within reach, nearest %.0f "
            "(aim %.2f, need %.2f), reach %.0f -> %s",
            seen, inRange, (double) nearest, (double) nearestAim,
            (double) PUNCH_AIM, (double) PUNCH_RANGE, best ? "hit" : "nothing");
    }
    return best;
}

static void punch_tick(struct MarioState *m, int side) {
    PunchState *st = &sPunch[side];

    // Not while swimming. A stroke is two arms pulling hard, which is exactly
    // what a punch looks like from here - so under water the punches came in
    // pairs with every stroke. Swimming wins: it is what the arms are for down
    // there, and the game has its own water punch on the button anyway.
    if (m->action & ACT_FLAG_SWIMMING) {
        st->hasPrev = false;
        st->striking = 0;
        return;
    }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, false, &pose)) {
        st->hasPrev = false;
        return;
    }

    // The swing is measured in camera space, not in the world.
    //
    // These poses are relative to the pose the mode was entered with - a frame
    // pinned to the room, not to Mario. So when he jumps, or is knocked back,
    // or rides a platform, the controller has not moved and this sees nothing,
    // which is right. Converting to the world first put all of his movement
    // into the hand and then took it out again by subtracting his velocity, and
    // that approximation broke exactly where the two disagree: jumping fired a
    // punch every time.
    Vec3f step = {
        pose.pos[0] - st->prevPos[0],
        pose.pos[1] - st->prevPos[1],
        pose.pos[2] - st->prevPos[2],
    };
    const bool hadPrev = st->hasPrev;
    st->prevPos[0] = pose.pos[0];
    st->prevPos[1] = pose.pos[1];
    st->prevPos[2] = pose.pos[2];
    if (!hadPrev) {
        st->hasPrev = true;
        st->armed = true;
        return;
    }

    const f32 speed = sqrtf(step[0]*step[0] + step[1]*step[1] + step[2]*step[2]);
    if (st->lockout > 0) { st->lockout--; }
    if (speed < PUNCH_REARM) { st->armed = true; }

    // A swing already under way keeps looking for something to meet, at
    // wherever the fist has got to by now.
    if (st->striking > 0) {
        st->striking--;

        Vec3f world, worldDir;
        camera_to_world(pose.pos, world);
        camera_dir_to_world(st->strikeDir, worldDir);

        struct Object *target = punch_target(world, worldDir);
        if (target != NULL) {
            st->striking = 0;
            if (mariovision_hands_logging(MV_HANDLOG_ACTION)) {
                mariovision_hands_log("[hands] punch connects with behaviour %p",
                                      (void *) target->behavior);
            }
            process_interaction(m, target->oInteractType, target, mv_punch_handler);
        }
    }

    if (speed < 1e-3f) { return; }
    Vec3f dir = { step[0] / speed, step[1] / speed, step[2] / speed };

    // The fist has to lead: the hand's own forward, turned into the world, must
    // point roughly where the hand is travelling. An arm swinging past while
    // walking fails this even when it is moving quickly.
    // The hand as it is drawn, correction and all. Taking the raw controller
    // frame here was the mistake: the renderer applies a half turn of pitch
    // between the grip frame and Mario's hand, so an uncorrected fist points
    // exactly backwards - measured as a lead of -0.69 where +0.69 was wanted,
    // which rejected every real punch.
    //
    // Both this and the movement are in camera space, so they can be compared
    // without either being converted anywhere.
    float grip[3][3];
    if (!mariovision_hand_frame(side, false, grip)) { return; }
    const f32 lead = dir[0]*grip[0][0] + dir[1]*grip[0][1] + dir[2]*grip[0][2];

    // Or simply travelling forwards, however the fist happens to be turned.
    //
    // A punch swung down from beside the head in a quarter circle never
    // satisfied the test above: through that arc the hand moves forwards while
    // its own axis is still pointing somewhere else entirely, so the fist never
    // "leads". It is plainly a punch all the same. What every punch does have
    // is that it goes away from the thrower, and the camera looks along -z, so
    // that is simply asked directly.
    const f32 forward = -dir[2];

    const bool fast = speed >= PUNCH_SPEED;
    const bool leads = lead >= PUNCH_LEAD || forward >= PUNCH_FORWARD;
    const bool ready = st->armed && st->lockout == 0;

    if (mariovision_hands_logging(MV_HANDLOG_GESTURE) && speed > PUNCH_SPEED * 0.5f) {
        mariovision_hands_log(
            "[hands] %s swing: speed %.1f/%.0f lead %.2f/%.2f forward %.2f/%.2f %s",
            side == MV_HAND_LEFT ? "left" : "right",
            (double) speed, (double) PUNCH_SPEED,
            (double) lead, (double) PUNCH_LEAD,
            (double) forward, (double) PUNCH_FORWARD,
            (fast && leads && ready) ? "-> punch" : "");
    }

    if (!fast || !leads || !ready) { return; }

    st->armed = false;
    st->lockout = PUNCH_LOCKOUT;
    sLastPunchHand = side;

    // The swing is heard whether or not it lands. A punch that connects gets
    // its own knock on top, from the handler.
    play_sound(SOUND_MARIO_PUNCH_YAH, gGlobalSoundSource);
    mariovision_announce_gesture("Punch");
    // And felt in the hand that threw it. A swing that meets nothing gets a
    // light knock; a hit gets a harder one from the handler.
    mariovision_hand_haptic(side, 0.45f, 0.4f);

    if (mariovision_hands_logging(MV_HANDLOG_ACTION)) {
        mariovision_hands_log("[hands] %s punch thrown, haptics %s",
                              side == MV_HAND_LEFT ? "left" : "right",
                              mariovision_hand_haptic_available(side) ? "yes" : "MISSING");
    }

    // The swing is open now; whether it meets anything is decided over the
    // frames that follow, as the fist actually travels.
    st->striking = PUNCH_WINDOW;
    st->strikeDir[0] = dir[0];
    st->strikeDir[1] = dir[1];
    st->strikeDir[2] = dir[2];
}

//-----------------------------------------------------------------------------
// Grabbing
//
// Reach out, squeeze the trigger, and what is there comes with the hand. Let go
// and it flies where the hand was going.
//
// The game's own route into carrying something is gated behind diving or the
// early frames of a punch - able_to_grab_object is deliberately narrow, because
// on a pad there is no other way to say "now". A tracked hand says it plainly,
// so that gate is bypassed and mario_grab_used_object is called directly, which
// is the same function the underwater grab uses for the same reason.
//-----------------------------------------------------------------------------

/// How far from the hand a thing may be and still be picked up.
#define GRAB_RANGE       70.0f
/// Squeeze past this to take hold, release below it to let go. Apart, so a
/// trembling finger does not drop what it is carrying.
#define GRAB_ON          0.6f
#define GRAB_OFF         0.35f
/// What a thrown thing does with the speed it was released at.
///
/// Not one to one, and the first attempt at one-to-one barely dropped things.
/// A hand travels 15 to 25 units a frame in a throw, while the game gives a
/// thrown box 40 of its own - so taking the hand's speed unscaled produced a
/// throw weaker than the one it replaced. Four times over puts a firm throw
/// around eighty, which is properly hard without being silly.
#define THROW_SCALE       4.0f
#define THROW_MIN        25.0f
#define THROW_MAX       120.0f

/// How many frames of the hand's recent movement are kept while carrying.
///
/// A throw is released by opening a finger, and fingers are late. By the time
/// they open the hand has often already slowed, or begun coming back - so the
/// last moment before release is the worst possible sample to throw from. Eight
/// frames is a quarter of a second, which comfortably contains the fastest part
/// of any throw a person makes.
#define THROW_HISTORY    8

typedef struct {
    bool holding;
    /// Recent hand velocities in camera space, newest at `at`.
    Vec3f recent[THROW_HISTORY];
    int at;
    int filled;
} GrabState;

static GrabState sGrab[MV_HAND_COUNT];

/// Set when something has just been thrown, so its speed can be written on the
/// following frame.
///
/// It cannot be written now: carryables set their own velocity from their
/// behaviour's thrown branch, and those run after the player in the same frame,
/// so anything put there immediately would be overwritten before it moved.
static struct Object *sThrown;
static Vec3f sThrowVel;

/// How wide the circle has to be before it counts, in game units.
///
/// The first attempt measured the hand's bearing about Mario and demanded a
/// quarter of a metre of clearance, which meant winding him up took a sweep of
/// the whole arm around the body - far more than anybody would do while holding
/// something heavy, and it simply never fired. What matters is that the hand
/// goes round, not how far out it goes, so the circle is now measured about the
/// hand's own recent middle. Eight units is eight centimetres: a small circle
/// of the wrist counts.
#define BOWSER_SWING_RADIUS   6.0f
/// How quickly that middle follows the hand. Slow enough that a circle keeps
/// its centre, quick enough that moving the whole arm elsewhere is not read as
/// half a turn.
#define BOWSER_CENTRE_FOLLOW  0.03f
/// How much a hand's turn is worth against a thumbstick's.
///
/// The action builds its speed from how far the heading moves each frame,
/// divided by 128, and needs 4096 of it for a full throw. A stick can be rolled
/// round its rim far faster than an arm can circle, so taken at face value a
/// hand needed some four seconds of unbroken circling - which is why it worked
/// but never quite reliably. Three to one asks for about a second and a half.
#define BOWSER_GAIN           3.0f
/// Frames the swing survives a gap in the circling.
///
/// Below the threshold the action decays what was built up at 64 a frame, so a
/// moment's hesitation - or the hand passing through the middle of its own
/// circle - undid a good deal of work. A short grace keeps that from happening
/// without letting a swing that has genuinely stopped carry on.
#define BOWSER_GRACE          10
/// What is written into intendedMag while swinging. The action wants more than
/// twenty before it accumulates anything at all.
#define BOWSER_SWING_MAG     32.0f

static bool is_bowser(struct Object *o) {
    return o->behavior == smlua_override_behavior(bhvBowser);
}

/// Whether it is carried the heavy way - the two-armed pose, and the slower
/// walk that comes with it.
///
/// It decides how the carrying looks, and nothing more. An earlier version made
/// this demand both hands, which is what the game means by heavy, but with
/// tracked controllers there is no reason a King Bob-omb cannot go under one
/// arm: the hand is where the strength is being pretended, not the fiction.
static bool carried_heavy(struct Object *o) {
    return (o->oInteractionSubtype & INT_SUBTYPE_GRABS_MARIO) != 0;
}

static struct Object *grabbable_near(const Vec3f hand) {
    struct Object *best = NULL;
    f32 bestDist = 1e9f;

    for (int list = 0; list < NUM_OBJ_LISTS; list++) {
        if (list != OBJ_LIST_GENACTOR && list != OBJ_LIST_PUSHABLE
            && list != OBJ_LIST_LEVEL && list != OBJ_LIST_DESTRUCTIVE) {
            continue;
        }
        struct Object *head = (struct Object *) &gObjectLists[list];
        for (struct Object *o = (struct Object *) head->header.next;
             o != head; o = (struct Object *) o->header.next) {
            if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED) { continue; }
            if (!(o->oInteractType & INTERACT_GRABBABLE)) { continue; }
            if (o->oInteractionSubtype & INT_SUBTYPE_NOT_GRABBABLE) { continue; }
            if (o->oHeldState != HELD_FREE) { continue; }

            const f32 top = o->oPosY + o->hitboxHeight;
            const f32 level = (hand[1] < o->oPosY) ? o->oPosY
                            : (hand[1] > top ? top : hand[1]);
            const f32 dx = o->oPosX - hand[0];
            const f32 dy = level - hand[1];
            const f32 dz = o->oPosZ - hand[2];
            const f32 dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist > o->hitboxRadius + GRAB_RANGE) { continue; }
            if (dist < bestDist) { best = o; bestDist = dist; }
        }
    }
    return best;
}

static void grab_take(struct MarioState *m, struct Object *o, bool heavy) {
    m->usedObj = o;

    // Bowser is his own case, and has to be, because the whole fight is in the
    // action rather than in the carrying. ACT_HOLDING_BOWSER is what
    // accumulates the angular velocity that decides how far he flies, and it
    // is also stationary - which is the other half of what was wrong here: held
    // through the ordinary carrying action he could be walked around, and no
    // amount of swinging did anything at all.
    if (is_bowser(o)) {
        m->angleVel[1] = 0;
        m->marioBodyState->grabPos = GRAB_POS_BOWSER;
        mario_grab_used_object(m);
        set_mario_action(m, ACT_HOLDING_BOWSER, 0);
        return;
    }

    m->marioBodyState->grabPos = heavy ? GRAB_POS_HEAVY_OBJ : GRAB_POS_LIGHT_OBJ;
    mario_grab_used_object(m);
    set_mario_action(m, heavy ? ACT_HOLD_HEAVY_IDLE : ACT_HOLD_IDLE, 0);
}

/// Winding Bowser up by swinging the arm around oneself.
///
/// Nothing new is computed here. act_holding_bowser already builds an angular
/// velocity out of how fast intendedYaw is turning - it was written for a
/// thumbstick being rolled around its rim, and a hand carried around the body
/// is the same quantity measured from a wider circle. So the bearing of the
/// hand about Mario is simply written where the stick's used to go.
static Vec3f sSwingCentre;
static bool sSwingHasCentre = false;
static s16 sSwingPrevBearing = 0;
static bool sSwingHasBearing = false;
/// The heading handed to the action - the hand's own turning, multiplied up.
static f32 sSwingFedYaw = 0.0f;
static int sSwingGrace = 0;

static void bowser_swing(struct MarioState *m, int side) {
    if (m->action != ACT_HOLDING_BOWSER) {
        sSwingHasCentre = false;
        sSwingHasBearing = false;
        sSwingGrace = 0;
        return;
    }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, false, &pose)) { return; }

    // In camera space, which is pinned to the room: circling the hand while
    // Mario is turned by that very circling would otherwise chase its own tail.
    if (!sSwingHasCentre) {
        sSwingCentre[0] = pose.pos[0];
        sSwingCentre[2] = pose.pos[2];
        sSwingHasCentre = true;
        sSwingHasBearing = false;
        sSwingFedYaw = 0.0f;
        sSwingGrace = 0;
        m->intendedMag = 0.0f;
        return;
    }

    const f32 dx = pose.pos[0] - sSwingCentre[0];
    const f32 dz = pose.pos[2] - sSwingCentre[2];

    // The middle drifts after the hand, so it is the middle of the circle being
    // drawn now rather than of wherever the arm started.
    sSwingCentre[0] += dx * BOWSER_CENTRE_FOLLOW;
    sSwingCentre[2] += dz * BOWSER_CENTRE_FOLLOW;

    const f32 out = sqrtf(dx * dx + dz * dz);
    if (out < BOWSER_SWING_RADIUS) {
        // Passing through the middle of its own circle, or simply stopped. A
        // few frames of grace tell those apart: a real swing comes back out,
        // and only a stopped one runs the grace down.
        sSwingHasBearing = false;
        if (sSwingGrace > 0) {
            sSwingGrace--;
            m->intendedYaw = (s16) sSwingFedYaw;
            m->intendedMag = BOWSER_SWING_MAG;
        } else {
            m->intendedMag = 0.0f;
        }
        return;
    }

    const s16 bearing = atan2s(dz, dx);
    sSwingGrace = BOWSER_GRACE;

    if (!sSwingHasBearing) {
        sSwingPrevBearing = bearing;
        sSwingHasBearing = true;
    }

    // How far round it went this frame, as a signed turn - the s16 subtraction
    // wraps of its own accord, so passing the half turn is not a special case.
    const s16 turned = (s16) (bearing - sSwingPrevBearing);
    sSwingPrevBearing = bearing;
    sSwingFedYaw += (f32) turned * BOWSER_GAIN;

    m->intendedYaw = (s16) sSwingFedYaw;
    m->intendedMag = BOWSER_SWING_MAG;

    if (mariovision_hands_logging(MV_HANDLOG_GESTURE)) {
        static unsigned tick = 0;
        if ((++tick % 15) == 0) {
            mariovision_hands_log(
                "[hands] bowser: circle %.0f wide, bearing %d, spin %d of 4096",
                (double) out, (int) m->intendedYaw, (int) m->angleVel[1]);
        }
    }
}

static void grab_release(struct MarioState *m, int side) {
    if (m->heldObj == NULL) { return; }
    struct Object *held = m->heldObj;

    // Bowser flies by his own rules: how far is decided by the angular velocity
    // wound up while holding him, which the release action reads for itself.
    // Nothing here has anything to add.
    if (m->action == ACT_HOLDING_BOWSER) {
        set_mario_action(m, ACT_RELEASING_BOWSER, 0);
        mariovision_hand_haptic(side, 1.0f, 0.6f);
        return;
    }

    // The fastest of the recent samples, not the last one.
    GrabState *st = &sGrab[side];
    Vec3f best = { 0, 0, 0 };
    f32 bestSpeed = 0.0f;
    for (int i = 0; i < st->filled; i++) {
        const f32 sp = sqrtf(st->recent[i][0]*st->recent[i][0]
                             + st->recent[i][1]*st->recent[i][1]
                             + st->recent[i][2]*st->recent[i][2]);
        if (sp > bestSpeed) { bestSpeed = sp; vec3f_copy(best, st->recent[i]); }
    }
    st->filled = 0;
    st->at = 0;

    Vec3f vel;
    camera_dir_to_world(best, vel);

    // The hand decides where it goes, whichever way that is.
    //
    // An earlier version handed the direction to the view whenever the swing
    // pointed even slightly backwards, which is far too eager: throwing across
    // oneself or out to the side is a perfectly ordinary throw, and having it
    // silently straightened to wherever one happened to be looking is worse
    // than the mistimed release it was meant to rescue.
    //
    // The view only stands in when there is no horizontal swing left at all -
    // released so late that the hand has already stopped - and then it is
    // rescuing a throw that would otherwise drop at one's feet.
    f32 flat = sqrtf(vel[0]*vel[0] + vel[2]*vel[2]);
    if (flat < 1.5f) {
        const s16 camYaw = gFirstPersonCamera.yaw;
        vel[0] = -sins(camYaw) * 2.0f;
        vel[2] = -coss(camYaw) * 2.0f;
        if (vel[1] < 0.0f) { vel[1] = 0.0f; }
        flat = 2.0f;
    }

    // Throw it along the hand, not along the look.
    //
    // mario_throw_held_object takes both the starting position and the initial
    // heading from Mario's own facing, and in first person that facing follows
    // the head - so however the arm had swung, the object set off wherever one
    // was looking. Lending it the hand's heading for the one call, and giving
    // it straight back, is enough: everything downstream reads what the throw
    // left behind rather than asking again.
    const s16 throwYaw = atan2s(vel[2], vel[0]);
    const s16 hadFacing = m->faceAngle[1];
    m->faceAngle[1] = throwYaw;
    mario_throw_held_object(m);
    m->faceAngle[1] = hadFacing;

    // And out of the carrying action.
    //
    // Detaching the object is only half of letting go: the action Mario is in
    // is what makes him walk slowly and refuse to climb, and it does not end by
    // itself. The game's own throw goes through ACT_THROWING, which resolves
    // afterwards - taking the throw out of that action meant nothing was left
    // to resolve it, and the carrying state outlived the thing being carried
    // until damage happened to replace it.
    //
    // Free fall rather than idle when he is off the ground, or he would land
    // standing in mid-air.
    set_mario_action(m, (m->action & ACT_FLAG_AIR) ? ACT_FREEFALL : ACT_IDLE, 0);

    const f32 horizontal = flat * THROW_SCALE;
    sThrown = held;
    sThrowVel[0] = (horizontal < THROW_MIN) ? THROW_MIN
                 : (horizontal > THROW_MAX ? THROW_MAX : horizontal);
    sThrowVel[1] = vel[1] * THROW_SCALE;
    sThrowVel[2] = (f32) throwYaw;

    mariovision_hand_haptic(side, 0.5f, 0.5f);
    if (mariovision_hands_logging(MV_HANDLOG_ACTION)) {
        mariovision_hands_log("[hands] thrown at %.0f forward, %.0f up",
                              (double) sThrowVel[0], (double) sThrowVel[1]);
    }
}

/// The deferred velocity, written the frame after the throw.
static void apply_thrown_velocity(void) {
    if (sThrown == NULL) { return; }
    if (sThrown->oHeldState == HELD_FREE && sThrown->activeFlags != ACTIVE_FLAG_DEACTIVATED) {
        sThrown->oForwardVel = sThrowVel[0];
        sThrown->oVelY = sThrowVel[1];
        sThrown->oMoveAngleYaw = (s16) sThrowVel[2];
    }
    sThrown = NULL;
}

static void grab_tick(struct MarioState *m, int side) {
    GrabState *st = &sGrab[side];
    const float trigger = mariovision_hands_trigger(side);
    const int other = (side == MV_HAND_LEFT) ? MV_HAND_RIGHT : MV_HAND_LEFT;

    if (st->holding) {
        // Keep the last handful of movements, so the throw can be taken from
        // the strongest of them rather than from whenever the finger happened
        // to open.
        MarioVisionHandPose carried;
        if (mariovision_hands_get(side, false, &carried)) {
            st->recent[st->at][0] = carried.vel[0] / 30.0f;
            st->recent[st->at][1] = carried.vel[1] / 30.0f;
            st->recent[st->at][2] = carried.vel[2] / 30.0f;
            st->at = (st->at + 1) % THROW_HISTORY;
            if (st->filled < THROW_HISTORY) { st->filled++; }
        }

        if (trigger < GRAB_OFF) {
            st->holding = false;
            // On a tree the two hands are independent - pole_tick has already
            // worked out who is still holding on. Clearing the other one here,
            // which is right for two hands on one crate, would say that the
            // hand still gripping had let go.
            if (mariovision_pole_in_hand(side) || mariovision_pole_in_hand(other)) { return; }
            if (ledge_in_hand(side) || ledge_in_hand(other)) { return; }
            grab_release(m, side);
            sGrab[other].holding = false;
        }
        return;
    }

    if (trigger < GRAB_ON) { return; }

    // Something already in hand that no hand has claimed - picked up by diving
    // into it, which the game still allows. Square and circle no longer throw,
    // so without this it could never be put down again. Whichever hand squeezes
    // takes charge of it, and releasing throws it as usual.
    if (m->heldObj != NULL) {
        if (!sGrab[other].holding) {
            st->holding = true;
            mariovision_hand_haptic(side, 0.5f, 0.5f);
        }
        return;
    }
    // Only from a state that can carry something: mid-jump or mid-slide the
    // carrying actions do not exist to move into. Hanging on a pole is let
    // through, because reaching higher up it is exactly what one does there.
    if (!(m->action & ACT_FLAG_STATIONARY) && !(m->action & ACT_FLAG_MOVING)
        && !((m->action & ACT_FLAG_ON_POLE) && mariovision_hand_climb())
        && !ledge_hanging(m)) { return; }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, false, &pose)) { return; }
    Vec3f hand;
    camera_to_world(pose.pos, hand);

    // A tree first, then a door: both are bigger than anything carryable and
    // one reaches for them the same way, so letting a crate nearby win would be
    // surprising.
    if (ledge_try_grab(m, side, hand)) { st->holding = true; return; }
    if (pole_try_grab(m, side, hand)) { st->holding = true; return; }
    if (door_try_grab(m, side, hand)) { return; }

    struct Object *o = grabbable_near(hand);
    if (o == NULL) { return; }

    const bool heavy = carried_heavy(o);
    grab_take(m, o, heavy);
    st->holding = true;
    mariovision_announce_gesture(is_bowser(o) ? "Bowser by the tail" : "Grabbed");    grab_take(m, o, false);
    st->holding = true;
    mariovision_hand_haptic(side, heavy ? 0.9f : 0.6f, 0.4f);
    if (mariovision_hands_logging(MV_HANDLOG_ACTION)) {
        mariovision_hands_log("[hands] %s grabbed behaviour %p",
                              side == MV_HAND_LEFT ? "left" : "right",
                              (void *) o->behavior);
    }
}


//-----------------------------------------------------------------------------
// Poles and trees
//
// Reach out, squeeze, and pull your hand down: you go up by exactly as far as
// your hand came down, and then reach higher with the other one. That is the
// whole of it, and it is why the height is measured in the room rather than
// against Mario - camera space is pinned to the room, so his climbing does not
// travel back into the measurement and cancel itself out.
//
// The game's own pole is left doing everything else. oMarioPolePos is how far
// up he is, act_holding_pole reads it every frame and puts him there, and the
// handstand at the top is a state it already has. Only what drives that number
// changes hands.
//-----------------------------------------------------------------------------

/// How far from the trunk a hand may be and still find it.
#define POLE_REACH   80.0f
/// How far one rises per unit the hand is drawn down. One for one - anything
/// else is a hand that does not stay where it was put.
#define POLE_RISE    1.0f

static struct Object *sPole;
/// Which hands have hold of it. Both are tracked, because hand over hand is the
/// whole point: one lets go only after the other has taken hold, and asking a
/// single "which hand" made the order matter - reach up, grab, then release,
/// and any other order dropped one off the tree.
static bool sPoleGrip[MV_HAND_COUNT];
/// Which one the climbing is measured from: the last to take hold.
static int sPoleSide = -1;
static f32 sPoleHandY;
/// Frames left to get the other hand on before one comes off the tree.
static int sPoleGrace;

//-----------------------------------------------------------------------------
// Getting one's face out of the bark
//
// set_pole_position puts Mario exactly on the pole's axis, and this view sits
// in his head - so the eye is inside the trunk. At a couple of centimetres from
// a surface, moving one's head by the same couple of centimetres swings what
// one sees by tens of degrees, and the tree appears to spin at the slightest
// movement. That is not the view being coupled to anything; it is simply how
// close it is.
//
// So the eye is moved out of the trunk, in the world and in a fixed direction -
// the side he took hold from. Fixed matters: pushing it along the line of sight
// would make the tree drift every time one looked round.
//
// And the hands are given the offset back. They are worked out from the
// camera's position, and camera space is pinned to the room, so a camera that
// has moved a metre without the player having moved says every hand is a metre
// from where it is. That was what put the tree out of reach of the hand holding
// it, the first time this was tried.
//-----------------------------------------------------------------------------

#define POLE_EYE_OUT 115.0f

static f32 sPoleEye[3];

void mariovision_pole_eye_offset(float out[3]) {
    out[0] = sPoleEye[0];
    out[1] = sPoleEye[1];
    out[2] = sPoleEye[2];
}

/// Long enough to reach across, short enough that a real letting-go still is
/// one. Climbing a rope, one hand leaves before the other arrives; asking for
/// an overlap works on a real branch, where the fingers know when they have it,
/// and not on a trigger, where they do not.
#define POLE_GRACE 20

static struct Object *pole_near(const Vec3f hand) {
    struct Object *best = NULL;
    f32 bestDist = 1e9f;

    struct Object *head = (struct Object *) &gObjectLists[OBJ_LIST_POLELIKE];
    for (struct Object *o = (struct Object *) head->header.next;
         o != head; o = (struct Object *) o->header.next) {
        if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED) { continue; }
        if (o->oInteractType != INTERACT_POLE) { continue; }

        // To the trunk, not to the object's origin: a tree is one point on the
        // ground and four metres of climbing, and one takes hold of it wherever
        // one's hand happens to be along it.
        const f32 dx = o->oPosX - hand[0];
        const f32 dz = o->oPosZ - hand[2];
        const f32 dist = sqrtf(dx * dx + dz * dz);
        if (dist > POLE_REACH) { continue; }
        if (hand[1] < o->oPosY - 60.0f) { continue; }
        if (hand[1] > o->oPosY + o->hitboxHeight + 60.0f) { continue; }

        if (dist < bestDist) { best = o; bestDist = dist; }
    }
    return best;
}

static void pole_release(void) {
    sPole = NULL;
    sPoleSide = -1;
    sPoleEye[0] = sPoleEye[1] = sPoleEye[2] = 0.0f;
    sPoleGrip[MV_HAND_LEFT] = false;
    sPoleGrip[MV_HAND_RIGHT] = false;
}

bool mariovision_pole_in_hand(int side) {
    if (side < 0 || side >= MV_HAND_COUNT) { return false; }
    return sPole != NULL && sPoleGrip[side];
}

static void pole_let_go(struct MarioState *m) {
    struct Object *pole = sPole;
    pole_release();
    if (pole == NULL || !(m->action & ACT_FLAG_ON_POLE)) { return; }

    add_tree_leaf_particles(m);
    // Away along the line of sight, and deliberately not the game's own turn of
    // half a revolution. That exists because on a screen Mario faces the pole
    // and one wants to leap off it backwards; here his facing already follows
    // where the viewer is looking, so turning him round would fling him behind
    // the head that was aiming the jump.
    set_mario_action(m, ACT_WALL_KICK_AIR, 0);
    mariovision_announce_gesture("Let go");
}

static bool pole_try_grab(struct MarioState *m, int side, const Vec3f hand) {
    if (!mariovision_hand_climb()) { return false; }
    if (sPole != NULL && sPoleGrip[side]) { return false; }

    struct Object *o = pole_near(hand);
    if (o == NULL) { return false; }
    // While one is already being climbed, only that one counts. In the moment
    // between hands nothing holds it, and a search from a hand halfway up a
    // tree could otherwise land on its neighbour.
    if (sPole != NULL && o != sPole) { return false; }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, false, &pose)) { return false; }

    const bool already = (m->action & ACT_FLAG_ON_POLE) != 0 && sPole == o;

    sPole = o;
    sPoleGrip[side] = true;
    sPoleSide = side;
    sPoleHandY = pose.pos[1];
    sPoleGrace = 0;
    mariovision_announce_gesture(side == MV_HAND_LEFT
        ? "Left hand on the pole" : "Right hand on the pole");

    if (!already) {
        // Which way out of the trunk: away from where he stood when he reached
        // for it. If he was standing on the axis - jumped into it, say - the
        // heading he is facing does instead.
        f32 ax = m->pos[0] - o->oPosX;
        f32 az = m->pos[2] - o->oPosZ;
        f32 len = sqrtf(ax * ax + az * az);
        if (len < 1.0f) {
            ax = -sins(m->faceAngle[1]);
            az = -coss(m->faceAngle[1]);
            len = 1.0f;
        }
        sPoleEye[0] = ax / len * POLE_EYE_OUT;
        sPoleEye[1] = 0.0f;
        sPoleEye[2] = az / len * POLE_EYE_OUT;

        mario_stop_riding_and_holding(m);
        m->interactObj = o;
        m->usedObj = o;
        m->vel[1] = 0.0f;
        m->forwardVel = 0.0f;
        m->marioObj->oMarioPoleYawVel = 0;
        m->marioObj->oMarioPolePos = m->pos[1] - o->oPosY;
        set_mario_action(m, ACT_HOLDING_POLE, 0);
        mariovision_announce_gesture("Hold on");
    }

    mariovision_hand_haptic(side, 0.6f, 0.25f);
    return true;
}

static void pole_tick(struct MarioState *m) {
    // Jumping off comes first, and is asked of the action rather than of our
    // own grip on it. Anywhere on the pole means anywhere: mid-climb, in the
    // handstand at the top, in the moment between hands, and equally on one
    // taken hold of the game's own way by jumping into it. Taken here, before
    // the pole action runs, so that its own half-turn never happens - see
    // pole_let_go for why that turn is wrong in a headset.
    if (mariovision_hand_climb()
        && (m->action & ACT_FLAG_ON_POLE)
        && (m->input & INPUT_A_PRESSED)) {
        if (sPole == NULL) {
            add_tree_leaf_particles(m);
            set_mario_action(m, ACT_WALL_KICK_AIR, 0);
            mariovision_announce_gesture("Let go");
        } else {
            pole_let_go(m);
        }
        return;
    }

    if (sPole == NULL) { return; }
    if (!(m->action & ACT_FLAG_ON_POLE) || sPole->activeFlags == ACTIVE_FLAG_DEACTIVATED) {
        pole_release();
        return;
    }

    // Who is still holding on. A hand that has opened simply stops counting;
    // only when neither is left does one come off the tree.
    for (int side = 0; side < MV_HAND_COUNT; side++) {
        if (sPoleGrip[side] && mariovision_hands_trigger(side) < GRAB_OFF) {
            sPoleGrip[side] = false;
            if (mariovision_hands_logging(MV_HANDLOG_ACTION) || mariovision_announce()) {
                mariovision_announce_gesture(side == MV_HAND_LEFT
                    ? "Left hand off the pole" : "Right hand off the pole");
            }
            // The other one carries on from where it is now, so that the hand
            // just let go of does not hand its last movement to its successor.
            if (sPoleSide == side) {
                const int other = (side == MV_HAND_LEFT) ? MV_HAND_RIGHT : MV_HAND_LEFT;
                MarioVisionHandPose held;
                if (sPoleGrip[other] && mariovision_hands_get(other, false, &held)) {
                    sPoleSide = other;
                    sPoleHandY = held.pos[1];
                } else {
                    sPoleSide = -1;
                    sPoleGrace = POLE_GRACE;
                }
            }
        }
    }

    if (sPoleSide < 0) {
        if (sPoleGrace > 0) {
            // Nobody holding on, but only just. Left hanging where he is for a
            // moment, so that letting go with one hand in order to reach higher
            // with the other does not put him on the ground.
            sPoleGrace--;
            if (m->controller != NULL) {
                m->controller->stickX = 0.0f;
                m->controller->stickY = 0.0f;
            }
            return;
        }
        pole_let_go(m);
        return;
    }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(sPoleSide, false, &pose)) { return; }

    // The stick has nothing to say up here. Left alone it would slide him down
    // and turn him at the same time as the hands are working, which is two
    // people climbing one tree.
    if (m->controller != NULL) {
        m->controller->stickX = 0.0f;
        m->controller->stickY = 0.0f;
    }

    const f32 pulled = sPoleHandY - pose.pos[1];
    sPoleHandY = pose.pos[1];

    struct Object *marioObj = m->marioObj;
    const f32 top = sPole->hitboxHeight - 100.0f;
    f32 at = marioObj->oMarioPolePos + pulled * POLE_RISE;
    if (at < 0.0f) { at = 0.0f; }
    if (at > top) { at = top; }
    marioObj->oMarioPolePos = at;

    if (at >= top - 0.5f && m->action == ACT_HOLDING_POLE) {
        set_mario_action(m, ACT_TOP_OF_POLE_TRANSITION, 0);
        mariovision_announce_gesture("Top of the pole");
    }
}


//-----------------------------------------------------------------------------
// Hanging from a ledge
//
// The same bargain as the tree, one storey up: hold on with the triggers, shove
// yourself sideways along the edge, and pull down to haul yourself over it.
//
// Sideways is the part the game has never had. Mario hangs where he caught the
// edge and can only go up or drop; there is no shuffling along. So the position
// is moved by hand and then asked whether the edge is still there - if it is
// not, the movement simply does not happen, which is what a hand finding no
// more wall feels like anyway.
//-----------------------------------------------------------------------------

/// How far a hand may be from the edge and still count as on it.
#define LEDGE_REACH   90.0f
/// How far the hand must come down to haul oneself up.
#define LEDGE_PULL    22.0f
/// One for one sideways, as up the tree.
#define LEDGE_SLIDE   1.0f
/// The most the ledge may step up or down and still be the same ledge.
#define LEDGE_STEP    30.0f
/// Frames to get the other hand on before one drops.
#define LEDGE_GRACE   20

static bool sLedgeGrip[MV_HAND_COUNT];
static int sLedgeSide = -1;
static f32 sLedgeHand[3];
static int sLedgeGrace;

static bool ledge_hanging(struct MarioState *m) {
    return m->action == ACT_LEDGE_GRAB
        && mariovision_ledge_mode() == MV_LEDGE_BY_HAND;
}

/// Whether a ledge is only to be caught when a hand is on it.
bool mariovision_ledge_wants_hand(void) {
    return mariovision_first_person_active()
        && mariovision_hands_active()
        && mariovision_ledge_mode() == MV_LEDGE_BY_HAND;
}

/// Whether either trigger is squeezed at this moment.
bool mariovision_hand_gripping(void) {
    for (int side = 0; side < MV_HAND_COUNT; side++) {
        if (mariovision_hands_trigger(side) >= GRAB_ON) { return true; }
    }
    return false;
}

static bool ledge_in_hand(int side) {
    if (side < 0 || side >= MV_HAND_COUNT) { return false; }
    return sLedgeGrip[side];
}

static void ledge_forget(void) {
    sLedgeGrip[MV_HAND_LEFT] = false;
    sLedgeGrip[MV_HAND_RIGHT] = false;
    sLedgeSide = -1;
    sLedgeGrace = 0;
}

/// Whether one could hang here, at this spot along the same edge.
static bool ledge_holds(struct MarioState *m, f32 x, f32 z) {
    struct Surface *floor = NULL;
    const f32 top = find_floor(x, m->pos[1] + 40.0f, z, &floor);
    if (floor == NULL) { return false; }
    if (floor->normal.y < 0.9063078f) { return false; }
    if (top < m->pos[1] - LEDGE_STEP || top > m->pos[1] + LEDGE_STEP) { return false; }
    return true;
}

static bool ledge_try_grab(struct MarioState *m, int side, const Vec3f hand) {
    if (!ledge_hanging(m)) { return false; }
    if (sLedgeGrip[side]) { return false; }

    // The edge is at Mario's own height, straight ahead of him - that is what
    // hanging from one means, so the hand only has to be near it.
    const f32 dx = hand[0] - m->pos[0];
    const f32 dy = hand[1] - m->pos[1];
    const f32 dz = hand[2] - m->pos[2];
    if (dx * dx + dy * dy + dz * dz > LEDGE_REACH * LEDGE_REACH) { return false; }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(side, false, &pose)) { return false; }

    sLedgeGrip[side] = true;
    sLedgeSide = side;
    sLedgeHand[0] = pose.pos[0];
    sLedgeHand[1] = pose.pos[1];
    sLedgeHand[2] = pose.pos[2];
    sLedgeGrace = 0;

    mariovision_hand_haptic(side, 0.6f, 0.25f);
    mariovision_announce_gesture(side == MV_HAND_LEFT
        ? "Left hand on the ledge" : "Right hand on the ledge");
    return true;
}

static void ledge_tick(struct MarioState *m) {
    static bool sWasHanging;
    if (!ledge_hanging(m)) {
        ledge_forget();
        sWasHanging = false;
        return;
    }
    if (!sWasHanging) {
        // Just caught it. A hand is on the trigger - that is what allowed the
        // catch - but it has yet to be registered as being on the ledge, so the
        // same allowance as between two hands is given for it to arrive.
        sWasHanging = true;
        sLedgeGrace = LEDGE_GRACE;
    }

    // Nothing the stick says up here. Pushed sideways it lets go of the ledge,
    // and pushed forward it climbs - both of which are the hands' business now.
    if (m->controller != NULL) {
        m->controller->stickX = 0.0f;
        m->controller->stickY = 0.0f;
    }

    for (int side = 0; side < MV_HAND_COUNT; side++) {
        if (sLedgeGrip[side] && mariovision_hands_trigger(side) < GRAB_OFF) {
            sLedgeGrip[side] = false;
            mariovision_announce_gesture(side == MV_HAND_LEFT
                ? "Left hand off the ledge" : "Right hand off the ledge");
            if (sLedgeSide == side) {
                const int other = (side == MV_HAND_LEFT) ? MV_HAND_RIGHT : MV_HAND_LEFT;
                MarioVisionHandPose held;
                if (sLedgeGrip[other] && mariovision_hands_get(other, false, &held)) {
                    sLedgeSide = other;
                    sLedgeHand[0] = held.pos[0];
                    sLedgeHand[1] = held.pos[1];
                    sLedgeHand[2] = held.pos[2];
                } else {
                    sLedgeSide = -1;
                    sLedgeGrace = LEDGE_GRACE;
                }
            }
        }
    }

    if (sLedgeSide < 0) {
        // Nobody on it. A moment's grace to get a hand across, then one drops -
        // the same allowance the tree gets, and for the same reason.
        if (sLedgeGrace > 0) { sLedgeGrace--; return; }
        extern s32 let_go_of_ledge(struct MarioState *m);
        ledge_forget();
        sWasHanging = false;
        let_go_of_ledge(m);
        mariovision_announce_gesture("Let go of the ledge");
        return;
    }

    MarioVisionHandPose pose;
    if (!mariovision_hands_get(sLedgeSide, false, &pose)) { return; }

    const f32 movedY = sLedgeHand[1] - pose.pos[1];
    const f32 movedX = pose.pos[0] - sLedgeHand[0];
    sLedgeHand[0] = pose.pos[0];
    sLedgeHand[1] = pose.pos[1];
    sLedgeHand[2] = pose.pos[2];

    // Hauling oneself up ends it. The game takes over from here and puts him
    // over the edge - which is exactly what one has just asked for, and it
    // knows how to do it.
    if (movedY >= LEDGE_PULL) {
        if (m->ceilHeight - m->floorHeight >= 160.0f) {
            ledge_forget();
            set_mario_action(m, ACT_LEDGE_CLIMB_FAST, 0);
            mariovision_announce_gesture("Up and over");
            return;
        }
    }

    // Sideways, along the edge. His facing is into the wall, so the edge runs
    // across it; the hand's own sideways movement is taken in the same frame
    // the eye sees, which is why it needs no rotating.
    if (movedX != 0.0f) {
        const s16 along = (s16) (m->faceAngle[1] + 0x4000);
        const f32 step = movedX * LEDGE_SLIDE;
        const f32 nx = m->pos[0] + sins(along) * step;
        const f32 nz = m->pos[2] + coss(along) * step;
        if (ledge_holds(m, nx, nz)) {
            m->pos[0] = nx;
            m->pos[2] = nz;
            if (m->marioObj != NULL) {
                m->marioObj->oPosX = nx;
                m->marioObj->oPosZ = nz;
            }
        }
    }
}

void mariovision_announce_gesture(const char *what) {
    if (!mariovision_announce()) { return; }
    djui_popup_create(what, 1);
}

//-----------------------------------------------------------------------------
// Doors
//
// Take hold of one and swing it yourself. Let go below a quarter turn and it
// falls shut; past that it swings the rest of the way open.
//
// The game has no such thing. A door there is a cutscene: walking into one puts
// Mario through a fixed animation, and whether he may pass is read off which
// frame that animation has reached. So the animation is driven by hand instead
// of by itself - oAction 100 is a state bhv_door_loop skips over entirely, with
// no case of its own, which makes it exactly the free hand-operated state this
// needs and costs nothing to borrow.
//
// Where a door leads out of the level the swing cannot finish the job: passing
// through is a warp, and the warp lives inside the action that was skipped. So
// there the swing is the gesture and the game's own sequence is the result -
// the same movement everywhere, a different consequence where the engine
// insists on one.
//-----------------------------------------------------------------------------

/// How far from the hand a door may be to take hold of it.
#define DOOR_REACH        90.0f
/// The quarter turn that decides it, as a fraction of fully open.
#define DOOR_TIPPING      0.5f
/// How far the door swings, in game angle, from shut to wide open.
#define DOOR_SWING        0x2800
/// How briskly the hand must still be turning it when let go for the door to
/// carry on by itself, in openness per frame.
#define DOOR_SHOVE        0.018f
/// What a door that has been committed to opening starts out with, so that one
/// which was merely carried far enough does not stall on the spot.
#define DOOR_PUSH_MIN     0.030f
/// How little a free swing loses each frame. A door coasts; it does not creep.
#define DOOR_COAST        0.985f
/// Below this the swing has run out of steam and the door falls back.
#define DOOR_STALL        0.004f
/// Frames the animation runs from shut to wide open. The whole animation is a
/// full open-and-close cycle over eighty; the first half is the opening.
#define DOOR_OPEN_FRAME   40.0f
/// How briskly it falls shut, and how much it is slowed on the way.
#define DOOR_SPRING       0.020f
#define DOOR_DAMPING      0.86f

extern bool gMarioVisionDoorHandBack;

/// How far the hand must turn before the door commits to a direction.
#define DOOR_START        0x0400
/// Beyond this, and out of its own rooms, an open door quietly shuts itself.
#define DOOR_FORGET       900.0f
/// How many doors may stand open at once.
#define DOOR_OPEN_SLOTS   4

/// A door left standing open, waiting to be walked through or forgotten.
typedef struct {
    struct Object *o;
    f32 open;
    f32 vel;
    bool flip;
    bool closing;
    /// Frames during which it is not to be forgotten, whatever the rooms say.
    int hold;
} OpenDoor;

static OpenDoor sOpen[DOOR_OPEN_SLOTS];

static struct Object *sDoor;
static f32 sDoorOpen;
static f32 sDoorVel;
static bool sDoorHeld;
/// Swinging open under its own momentum, with nobody holding it.
static bool sDoorFree;
static int sDoorSide;
static s16 sDoorGrabAngle;
/// +1, -1, or 0 while the hand has not yet said which way it is going.
static s16 sDoorSign;
static bool sDoorFlip;
/// Where Mario stood when he took hold of it - see door_finished_opening.
static Vec3f sDoorGrabPos;

static bool door_alive(struct Object *o) {
    // A pointer outlives its door across a level load, and the pool hands the
    // memory to something else. Both tests cost nothing and keep a stray
    // negated rotation from turning up on an unrelated object.
    return o != NULL
        && o->activeFlags != ACTIVE_FLAG_DEACTIVATED
        && (o->oInteractType & (INTERACT_DOOR | INTERACT_WARP_DOOR)) != 0;
}

bool mariovision_door_swings_back(const void *gfxNode) {
    if (sDoorFlip && door_alive(sDoor) && gfxNode == (const void *) &sDoor->header.gfx) {
        return true;
    }
    for (int i = 0; i < DOOR_OPEN_SLOTS; i++) {
        if (!sOpen[i].flip || !door_alive(sOpen[i].o)) { continue; }
        if (gfxNode == (const void *) &sOpen[i].o->header.gfx) { return true; }
    }
    return false;
}

/// Whether this door has something of the game's own to run when it opens.
///
/// A warp door leads somewhere, and a star door has its ceremony. Everything
/// else simply stands open and is walked through.
///
/// The save-file flag used to count too, and that was wrong. It belongs to the
/// castle doors that ask for a number of stars - the one to Cool, Cool Mountain
/// among them - and those go nowhere; the flag only records that the door has
/// been unlocked once. Treating them as passages meant the game was handed a
/// door it had nothing to do with, and answered with the walk-through it uses
/// for doors in general: Mario carried through a doorway on rails, which is
/// exactly what the whole business of opening them by hand is meant to replace.
///
/// So the flag only counts while it is still unset. That first time there is
/// something to see - the door being unlocked - and it is worth handing over
/// for. Afterwards the door is an ordinary one.
static bool door_hands_back(struct Object *o) {
    if (o->oInteractType == INTERACT_WARP_DOOR) { return true; }
    if (o->oInteractionSubtype & INT_SUBTYPE_STAR_DOOR) { return true; }
    const u32 flag = get_door_save_file_flag(o);
    return flag != 0 && !(save_file_get_flags() & flag);
}

static struct Object *door_near(const Vec3f hand) {
    struct Object *best = NULL;
    f32 bestDist = 1e9f;

    struct Object *head = (struct Object *) &gObjectLists[OBJ_LIST_SURFACE];
    for (struct Object *o = (struct Object *) head->header.next;
         o != head; o = (struct Object *) o->header.next) {
        if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED) { continue; }
        if (!(o->oInteractType & (INTERACT_DOOR | INTERACT_WARP_DOOR))) { continue; }
        // Only one that is at rest; one already swinging by itself is busy.
        if (o->oAction != 0 && o->oAction != 100) { continue; }

        const f32 dx = o->oPosX - hand[0];
        const f32 dy = (o->oPosY + 150.0f) - hand[1];
        const f32 dz = o->oPosZ - hand[2];
        const f32 dist = sqrtf(dx*dx + dy*dy + dz*dz);
        if (dist > DOOR_REACH + 100.0f) { continue; }
        if (dist < bestDist) { best = o; bestDist = dist; }
    }
    return best;
}

/// Where the hand is, as an angle about the door's hinge.
///
/// The hinge really is the object's own position: the geo layout hangs the leaf
/// off a bone at x = -300, which after the quarter scale and the offset of 78
/// that sits above it lands within three units of the origin.
static s16 door_hand_angle(struct Object *o, const Vec3f hand) {
    return atan2s(hand[2] - o->oPosZ, hand[0] - o->oPosX);
}

/// Show a door at a given openness.
static void door_write(struct Object *o, f32 open) {
    // The frame alone, never the animation: setting that again each frame would
    // sound the door's own opening noise thirty times a second. It is chosen
    // once, when the door is taken hold of.
    o->header.gfx.animInfo.animFrame = (s16) (open * DOOR_OPEN_FRAME);
    // And pinned, or the renderer advances it by itself and the door opens of
    // its own accord the moment it is touched.
    o->header.gfx.animInfo.animTimer = gAreaUpdateCounter;
    // Where door_allow_walk_through reads how far open it is.
    o->oForwardVel = open;
}

static void door_shut(struct Object *o) {
    o->oAction = 0;
    o->oForwardVel = 0.0f;
}

//-----------------------------------------------------------------------------
// Doors left standing open
//
// An ordinary door stays where it was swung, which is the point - one walks
// through it oneself. But a door that never shuts again leaves the castle
// propped open behind one, and the room beyond it visible from everywhere.
//
// So each one is watched, and closes itself once it is out of mind: when Mario
// is no longer in any room the door joins, which is the same test the game uses
// to decide whether to draw it at all, or simply once he is far enough away.
// The second test is there for outdoors, where there are no rooms and the first
// one never fires.
//-----------------------------------------------------------------------------

static bool door_forgotten(struct MarioState *m, struct Object *o) {
    if (o->oDoorUnk88 == 0) { return true; }
    const f32 dx = o->oPosX - m->pos[0];
    const f32 dz = o->oPosZ - m->pos[2];
    return (dx*dx + dz*dz) > (DOOR_FORGET * DOOR_FORGET);
}

static void door_keep_open(struct Object *o, f32 open, bool flip, int hold) {
    for (int i = 0; i < DOOR_OPEN_SLOTS; i++) {
        if (sOpen[i].o == o || sOpen[i].o == NULL) {
            sOpen[i] = (OpenDoor) { o, open, 0.0f, flip, false, hold };
            return;
        }
    }
    // All full: the oldest gives way and shuts on the spot.
    door_shut(sOpen[0].o);
    sOpen[0] = (OpenDoor) { o, open, 0.0f, flip, false, hold };
}

/// Take a door back out of the open list, and say how far open it was.
static bool door_reclaim(struct Object *o, f32 *open, bool *flip) {
    for (int i = 0; i < DOOR_OPEN_SLOTS; i++) {
        if (sOpen[i].o != o) { continue; }
        *open = sOpen[i].open;
        *flip = sOpen[i].flip;
        sOpen[i].o = NULL;
        return true;
    }
    return false;
}

static void door_open_list_tick(struct MarioState *m) {
    for (int i = 0; i < DOOR_OPEN_SLOTS; i++) {
        OpenDoor *d = &sOpen[i];
        if (d->o == NULL) { continue; }
        if (!door_alive(d->o)) { d->o = NULL; continue; }

        if (d->hold > 0) { d->hold--; }
        else if (!d->closing && door_forgotten(m, d->o)) { d->closing = true; }

        if (d->closing) {
            d->vel -= d->open * DOOR_SPRING;
            d->vel *= DOOR_DAMPING;
            d->open += d->vel;
            if (d->open <= 0.01f) {
                door_shut(d->o);
                d->o = NULL;
                continue;
            }
        }
        door_write(d->o, d->open);
    }
}

//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Letting go
//
// Two ways a door ends up open, because there are two ways a real one does.
// Either it was carried far enough round by hand, or it was given a shove and
// swings the rest of the way itself.
//
// The second is not a flourish, it is what makes pushing possible at all.
// Pulling a door has room to spare: the arm folds up and one can step back with
// it. Pushing has neither - the door is still solid, so Mario cannot follow his
// own hand, and the arm runs out at about forty centimetres. That is some
// fifteen degrees about the hinge where a full swing wants thirty-four. So a
// door that is being turned briskly at the moment it is released keeps going.
//-----------------------------------------------------------------------------

static void door_let_go(struct MarioState *m) {
    if (sDoor == NULL) { return; }
    sDoorHeld = false;

    if (sDoorOpen < DOOR_TIPPING && sDoorVel < DOOR_SHOVE) {
        // Neither far enough nor fast enough. It falls shut, which door_tick
        // sees to. The numbers are announced because they are exactly what one
        // needs to know when it did not go: which of the two was close.
        static char said[64];
        snprintf(said, sizeof said, "Door falls shut (%.2f open, %.3f/f)",
                 (double) sDoorOpen, (double) sDoorVel);
        mariovision_announce_gesture(said);
        return;
    }

    // It is going to open. Whether it was carried there or thrown, it finishes
    // the swing by itself from here.
    sDoorFree = true;
    if (sDoorVel < DOOR_PUSH_MIN) { sDoorVel = DOOR_PUSH_MIN; }
    mariovision_announce_gesture(sDoorOpen < DOOR_TIPPING ? "Door shoved" : "Door swung");
}

/// The swing has finished. Now it either stays open or leads somewhere.
static void door_finished_opening(struct MarioState *m) {
    if (sDoor == NULL) { return; }
    sDoorFree = false;

    if (door_hands_back(sDoor)) {
        // Through it, by the game's own route - the warp and what waits on the
        // other side both live in that action, and there is no reaching them
        // from out here.
        //
        // And by calling the game's own handler rather than copying what it
        // does. Written out a second time it was already wrong: the bit that
        // says "and warp when you get there" is added inside interact_warp_door
        // and nowhere else, so the door opened, Mario walked through, and
        // stopped in the stub of a room behind it that nobody is meant to
        // stand in. Everything else in there - the keys, the star doors, the
        // dialogue when one is refused - was a second copy waiting to drift
        // from the first in the same way.
        struct Object *door = sDoor;
        const bool warp = (door->oInteractType == INTERACT_WARP_DOOR);
        if (warp) {
            // Left standing open behind the fade. Shutting it here is what made
            // it slam the instant the circle began to close: the warp takes a
            // good second to arrive, and for all of it the door was back in its
            // frame. It is put in the list of open doors so that something keeps
            // holding it there, and the level load clears the list by itself.
            door_write(door, 1.0f);
            // Held open for a good while. The warp takes about a second to
            // arrive, and the moment it is asked for the rooms change under
            // one's feet - so the ordinary test, which closes a door once one
            // is no longer in any room it joins, fired during the fade and shut
            // it after all. The count runs out by itself afterwards, and the
            // door goes back to being an ordinary open one.
            door_keep_open(door, 1.0f, sDoorFlip, 150);
        } else {
            // The rest have a sequence of their own that moves the door, and
            // interact_door will not look at one that is still in a hand.
            door_shut(door);
        }
        sDoor = NULL;
        sDoorFlip = false;

        // Both handlers serve only somebody on their feet, which is what the
        // walking-in they were written for implies. Standing at a door having
        // just swung it is the same situation; the borrowed action is put back
        // if the door turns us away.
        const u32 was = m->action;
        if (m->action != ACT_WALKING && m->action != ACT_DECELERATING) {
            m->action = ACT_WALKING;
        }

        // And standing where he was when he took hold of it.
        //
        // Which side of the door one came from is worked out from where Mario
        // is, and it decides more than the animation: init_door_warp puts him
        // down three hundred units in front of the door at the other end, and
        // flips that round for the far side. By now he may well have walked
        // through the doorway - it lets one through at half open, the swing
        // finishes by itself over the best part of a second, and one naturally
        // steps forward. Then the answer comes out backwards and one arrives
        // behind the door one meant to come out of.
        //
        // The question is about where he came from, so it is answered with
        // where he came from. Put back at once; nothing reads it again until
        // the next frame, by which time the action has it.
        Vec3f stood;
        vec3f_copy(stood, m->pos);
        vec3f_copy(m->pos, sDoorGrabPos);

        gMarioVisionDoorHandBack = true;
        const u32 went = (door->oInteractType == INTERACT_WARP_DOOR)
            ? interact_warp_door(m, INTERACT_WARP_DOOR, door)
            : interact_door(m, INTERACT_DOOR, door);
        gMarioVisionDoorHandBack = false;
        vec3f_copy(m->pos, stood);
        if (!went) {
            m->action = was;
            // Turned away - a key one has not got. It does not stay hanging open.
            f32 back; bool flipBack;
            if (door_reclaim(door, &back, &flipBack)) { door_shut(door); }
        }

        mariovision_announce_gesture(went ? "Through the door" : "The door refuses");
        return;
    }

    // An ordinary one stays where it was swung to, and is handed to the list
    // that will eventually close it again.
    door_write(sDoor, 1.0f);
    door_keep_open(sDoor, 1.0f, sDoorFlip, 0);
    sDoor = NULL;
    sDoorFlip = false;
    mariovision_announce_gesture("Door stands open");
}

static void door_tick(struct MarioState *m) {
    if (sDoor == NULL) { return; }
    if (!door_alive(sDoor)) { sDoor = NULL; sDoorFlip = false; return; }

    if (sDoorHeld) {
        MarioVisionHandPose pose;
        if (!mariovision_hands_get(sDoorSide, false, &pose)
            || mariovision_hands_trigger(sDoorSide) < GRAB_OFF) {
            door_let_go(m);
            if (sDoor == NULL) { return; }
        } else {
            Vec3f hand;
            camera_to_world(pose.pos, hand);
            const s16 turned = (s16) (door_hand_angle(sDoor, hand) - sDoorGrabAngle);

            // The leaf simply follows the hand, either way round it goes. Which
            // way that is stays open until the hand has actually committed to
            // one, so a shiver at the moment of grabbing does not decide it -
            // and once decided it is kept, or turning back to close the door
            // would flip the leaf through the frame.
            if (sDoorSign == 0 && (turned >= DOOR_START || turned <= -DOOR_START)) {
                sDoorSign = (turned > 0) ? 1 : -1;
                // Negative is the direction the door's own animation does not
                // have. Both of the game's two turn the leaf the same way and
                // differ only in briskness, so the other way round is had by
                // negating the rotation as the renderer reads it.
                sDoorFlip = (sDoorSign < 0);
            }

            f32 open = 0.0f;
            if (sDoorSign != 0) {
                open = ((f32) turned * (f32) sDoorSign) / (f32) DOOR_SWING;
                if (open > 1.0f) { open = 1.0f; }
                if (open < 0.0f) { open = 0.0f; }
            }
            // Kept, so that letting go can tell a shove from a slow carry.
            // Smoothed a little, or one stuttering frame of tracking decides it.
            sDoorVel = 0.5f * sDoorVel + 0.5f * (open - sDoorOpen);
            sDoorOpen = open;
        }
    }

    if (!sDoorHeld && sDoorFree) {
        // Coasting the rest of the way round.
        sDoorOpen += sDoorVel;
        sDoorVel *= DOOR_COAST;
        if (sDoorOpen >= 1.0f) {
            sDoorOpen = 1.0f;
            door_write(sDoor, 1.0f);
            door_finished_opening(m);
            return;
        }
        // Out of steam short of the frame: it falls back after all.
        if (sDoorVel < DOOR_STALL) { sDoorFree = false; }
    } else if (!sDoorHeld) {
        // Falling shut, with enough damping that it does not bounce.
        sDoorVel -= sDoorOpen * DOOR_SPRING;
        sDoorVel *= DOOR_DAMPING;
        sDoorOpen += sDoorVel;
        if (sDoorOpen <= 0.01f) {
            door_shut(sDoor);
            sDoor = NULL;
            sDoorFlip = false;
            return;
        }
    }

    door_write(sDoor, sDoorOpen);
}

static bool door_try_grab(struct MarioState *m, int side, const Vec3f hand) {
    if (sDoor != NULL && sDoorHeld) { return false; }

    struct Object *o = door_near(hand);
    if (o == NULL) { return false; }
    // Not a star door: it slides apart instead of swinging, so there is no
    // hinge for a hand. Left to the game, which opens it by being walked into.
    if (o->oInteractionSubtype & INT_SUBTYPE_STAR_DOOR) { return false; }

    // The stars are asked for before it will budge, exactly as the game asks
    // before it will open - being refused by a door one may not pass is part
    // of the game, and silently swinging it would give away more than it should.
    //
    // Only on an ordinary door. The same byte on a warp door is which door it
    // is, not how many stars it wants, and reading it as a star count refuses
    // the basement door to anybody with fewer than two. What a warp door asks
    // for - the keys - is asked by its own handler when the door is let go,
    // and answered there with the dialogue that says which key is missing.
    const s16 needed = (o->oInteractType == INTERACT_WARP_DOOR)
        ? 0 : (s16) ((u32) o->oBehParams >> 24);
    const s16 have = save_file_get_total_star_count(gCurrSaveFileNum - 1,
                                                    COURSE_MIN - 1, COURSE_MAX - 1);
    if (have < needed) {
        mariovision_announce_gesture("It will not budge");
        mariovision_hand_haptic(side, 0.9f, 0.9f);
        return false;
    }

    // Taking hold of one that is already standing open: it keeps the angle and
    // the direction it has instead of slamming shut or turning inside out, so
    // an open door can be pushed shut again by hand.
    f32 open = 0.0f;
    bool flip = false;
    bool again = door_reclaim(o, &open, &flip);
    if (!again && o == sDoor && o->oAction == 100) {
        again = true;
        open = sDoorOpen;
        flip = sDoorFlip;
    }

    sDoor = o;
    sDoorSide = side;
    sDoorHeld = true;
    sDoorFree = false;
    sDoorOpen = open;
    sDoorVel = 0.0f;
    sDoorFlip = flip;
    sDoorSign = again ? (flip ? -1 : 1) : 0;
    sDoorGrabAngle = (s16) (door_hand_angle(o, hand)
                            - (s16) (sDoorSign * open * DOOR_SWING));
    if (!again) { vec3f_copy(sDoorGrabPos, m->pos); }

    if (o->oAction != 100) {
        // Always the same animation, now that the direction is decided by the
        // hand. The two the game has differ in their easing, and taking one on
        // each side would have made the same pull feel different depending on
        // which room one came from.
        o->oAction = 100;
        obj_init_animation_with_accel_and_sound(o, 1, 1.0f);
    }

    mariovision_hand_haptic(side, 0.7f, 0.3f);
    mariovision_announce_gesture("Door in hand");
    return true;
}

//-----------------------------------------------------------------------------
// Swimming
//
// A breaststroke pulls both hands outwards and back at once, and that is the
// whole of the detection: two hands doing the same thing together, which
// nothing else in the game asks of them.
//
// Nothing about how swimming works is touched. The game already turns a press
// of A into ACT_BREASTSTROKE and already rewards a stroke repeated in rhythm -
// sSwimStrength climbs with every well-timed one - so a stroke recognised here
// simply says the same thing the button would have said. That keeps the rhythm
// mechanic intact, which would be a shame to lose and an enormous nuisance to
// rebuild.
//-----------------------------------------------------------------------------

/// How hard both hands must pull before a stroke begins, in units per frame.
///
/// Six was a deliberate, sweeping stroke; three is the smaller pull one
/// actually makes. That both hands must pull together is what separates this
/// from any ordinary arm movement - the speed was never doing that work.
#define STROKE_PULL      3.0f
/// And how gently they may be moving and still count as still pulling. Lower
/// than the threshold to begin, so a stroke that eases off in the middle is not
/// chopped into two.
#define STROKE_HOLD      1.2f
/// How often the button is pressed while a stroke is under way.
///
/// Pressing once per stroke was the first attempt and it swims quite unlike a
/// player holding a pad, because in this game one press and a flurry of presses
/// are genuinely different things: a second press soon after the first restarts
/// the stroke and builds the swimmer's strength. So as long as the arms are
/// pulling, the button is pressed over and over, which is what a player does.
#define STROKE_REPEAT    3
/// Frames of stillness before the arms are considered to have stopped.
#define STROKE_SETTLE    6

static int sStrokeSettling = 0;
static int sStrokeNext = 0;
static bool sStrokeUnderway = false;

/// How much this hand is pulling: outwards from the middle, and backwards.
static f32 stroke_pull(int side, const MarioVisionHandPose *pose) {
    const f32 outward = (side == MV_HAND_RIGHT) ? pose->vel[0] : -pose->vel[0];
    // The camera looks along -z, so +z is behind: a pull comes back towards
    // the swimmer.
    return (outward + pose->vel[2]) / 30.0f;
}

static void swim_tick(struct MarioState *m) {
    if (!mariovision_scheme_physical(MV_DO_SWIM)) { return; }
    if (!(m->action & ACT_FLAG_SWIMMING)) {
        sStrokeUnderway = false;
        sStrokeSettling = 0;
        return;
    }

    MarioVisionHandPose left, right;
    if (!mariovision_hands_get(MV_HAND_LEFT, false, &left)) { return; }
    if (!mariovision_hands_get(MV_HAND_RIGHT, false, &right)) { return; }

    const f32 pullL = stroke_pull(MV_HAND_LEFT, &left);
    const f32 pullR = stroke_pull(MV_HAND_RIGHT, &right);
    // Both hands, or it is one arm doing something else entirely.
    const f32 weaker = (pullL < pullR) ? pullL : pullR;

    if (!sStrokeUnderway) {
        if (weaker < STROKE_PULL) { return; }
        sStrokeUnderway = true;
        sStrokeNext = 0;
        sStrokeSettling = 0;
        mariovision_announce_gesture("Stroke");
        mariovision_hand_haptic(MV_HAND_LEFT, 0.35f, 0.2f);
        mariovision_hand_haptic(MV_HAND_RIGHT, 0.35f, 0.2f);
    } else if (weaker < STROKE_HOLD) {
        if (++sStrokeSettling >= STROKE_SETTLE) { sStrokeUnderway = false; }
    } else {
        sStrokeSettling = 0;
    }

    if (mariovision_hands_logging(MV_HANDLOG_GESTURE) && weaker > STROKE_HOLD) {
        mariovision_hands_log("[hands] stroke: pull %.1f / %.1f, need %.1f, %s",
                              (double) pullL, (double) pullR, (double) STROKE_PULL,
                              sStrokeUnderway ? "under way" : "waiting");
    }

    if (!sStrokeUnderway) { return; }

    // The button, again and again, for as long as the arms keep pulling.
    if (sStrokeNext > 0) { sStrokeNext--; return; }
    sStrokeNext = STROKE_REPEAT;
    m->input |= INPUT_A_PRESSED;
}

//-----------------------------------------------------------------------------

void mariovision_hands_tick(struct MarioState *m) {
    if (m == NULL || m->playerIndex != 0) { return; }
    if (m->marioObj == NULL || m->area == NULL) { return; }

    // Doors left standing open are held there before anything else is asked.
    //
    // Their frame has to be written every single frame or the renderer carries
    // the animation on and closes them by itself - and the moment that matters
    // most is the one every guard below refuses: walking through a warp door
    // puts Mario into a cutscene action, and the door shut behind the fade
    // every time. It has nothing to do with hands, so it does not wait on them.
    door_open_list_tick(m);

    if (!mariovision_first_person_active() || !mariovision_hands_active()) { return; }
    // Not during a cutscene: the player is watching, not acting, and a hand
    // that happens to move should not reach into what is being shown.
    if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_CUTSCENE) { return; }

    apply_thrown_velocity();
    swim_tick(m);
    door_tick(m);
    pole_tick(m);
    ledge_tick(m);

    for (int side = 0; side < MV_HAND_COUNT; side++) {
        // Not while holding on to something. Drawing a hand sharply downwards
        // is how one climbs, and it is also exactly what the punch detector is
        // watching for - so every pull up the tree threw a punch as well.
        if (mariovision_scheme_physical(MV_DO_PUNCH)
            && !((m->action & ACT_FLAG_ON_POLE) && mariovision_hand_climb())) {
            punch_tick(m, side);
        }
        if (mariovision_scheme_physical(MV_DO_CARRY)) { grab_tick(m, side); }
        if (sGrab[side].holding) { bowser_swing(m, side); }
    }

    // Something dropped by the game itself - taking damage, entering water -
    // must not leave a hand believing it still holds it.
    if (m->heldObj == NULL) {
        sGrab[MV_HAND_LEFT].holding = false;
        sGrab[MV_HAND_RIGHT].holding = false;
    }
}
