#include "first_person_cam.h"

#include "sm64.h"
#include "behavior_data.h"
#include "camera.h"
#include "level_update.h"
#include "object_list_processor.h"
#include "object_helpers.h"
#include "mario.h"
#include "hardcoded.h"
#include "save_file.h"

#include "engine/math_util.h"

#include "pc/controller/controller_mouse.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_hud_utils.h"
#include "pc/lua/utils/smlua_camera_utils.h"
#include "pc/lua/smlua_hooks.h"

struct FirstPersonCamera gFirstPersonCamera = {
    .enabled = false,
    .forcePitch = false,
    .forceYaw = false,
    .forceRoll = true,
    .centerL = true,
    .pitch = 0,
    .yaw = 0,
    .crouch = 0,
    .fov = FIRST_PERSON_DEFAULT_FOV,
    .offset = { 0, 0, 0 }
};

extern s16 gMenuMode;

bool first_person_check_cancels(struct MarioState *m) {
#ifdef TARGET_VISIONOS
    // Flying is the one of upstream's cancels one might want either way.
    //
    // On a flat screen a first-person flight is disorienting, which is why it
    // backs out. In a headset it can be the thing one most wants to be inside
    // of - and it can equally be the quickest way to feel ill, since the wing
    // cap banks and rolls with no say from the viewer. So it is a setting, and
    // it starts off: whoever wants it will go looking.
    extern bool mariovision_fly_first_person(void);
    extern bool mariovision_hand_climb(void);
    // A cannon backs out to third person, as upstream has it. Tried the other
    // way round to get the barrel around the viewer's head, and from inside
    // Mario far too much of the level is simply missing to aim by.
    if (m->action == ACT_FIRST_PERSON || m->action == ACT_IN_CANNON
        || m->action == ACT_READING_NPC_DIALOG || m->action == ACT_DISAPPEARED
        || (m->action == ACT_FLYING && !mariovision_fly_first_person())) {
        return true;
    }
#else
    if (m->action == ACT_FIRST_PERSON || m->action == ACT_IN_CANNON || m->action == ACT_READING_NPC_DIALOG || m->action == ACT_DISAPPEARED || m->action == ACT_FLYING) {
        return true;
    }
#endif
#ifdef TARGET_VISIONOS
    // Climbing is close to unreadable from inside Mario's own head: what you
    // are holding on to is behind you, and how far you have left to go is not
    // visible at all. Back out to third person for the duration - the mode
    // resumes by itself the moment he lets go.
    // Unless one is climbing it oneself, in which case being inside his head is
    // the whole point: the hand in the room is on the pole in front of one.
    extern int mariovision_hang_mode(void);
    if ((m->action & ACT_FLAG_ON_POLE) && mariovision_hand_climb()) {
        // stays in first person
    } else if (m->action & ACT_FLAG_ON_POLE) {
        return true;
    } else if ((m->action & ACT_FLAG_HANGING)
               && mariovision_hang_mode() == 1 /* MV_HANG_THIRD_PERSON */) {
        // Hanging from a ceiling is a choice too, and starts inside his head.
        // Backing out shows what one is hanging from and where the ceiling runs
        // out; staying keeps one in the headset. Neither is plainly right.
        return true;
    }
    // Ledges are a choice now. Backing out to third person was the first answer
    // - hanging off one, what you need to see is where your hands are and what
    // is above you, and both are behind the camera when it sits in his head.
    // But being thrown out of the headset view every time one catches an edge
    // is its own kind of wrong, so which of the two happens is a setting, and it
    // starts by staying put. The five ledge actions - grabbing, climbing up
    // slowly or fast, climbing down - are contiguous in the id.
    {
        extern int mariovision_ledge_mode(void);
        const u32 id = m->action & ACT_ID_MASK;
        if (id >= (ACT_LEDGE_GRAB & ACT_ID_MASK) && id <= (ACT_LEDGE_CLIMB_FAST & ACT_ID_MASK)
            && mariovision_ledge_mode() == 1 /* MV_LEDGE_THIRD_PERSON */) {
            return true;
        }
    }
#endif
    if (find_object_with_behavior(smlua_override_behavior(bhvActSelector)) != NULL) { return true; }

    if (gLuaLoadingMod != NULL) { return false; }

    struct Object *bowser = find_object_with_behavior(smlua_override_behavior(bhvBowser));
    if ((gCurrLevelNum == LEVEL_BOWSER_1 || gCurrLevelNum == LEVEL_BOWSER_2 || gCurrLevelNum == LEVEL_BOWSER_3) &&
        bowser != NULL &&
        (bowser->oAction == 5 || bowser->oAction == 6)) {
        return true;
    }

    return false;
}

bool get_first_person_enabled(void) {
    return gFirstPersonCamera.enabled && !first_person_check_cancels(&gMarioStates[0]);
}

void set_first_person_enabled(bool enable) {
    gFirstPersonCamera.enabled = enable;
}

static void first_person_camera_update(void) {
    struct MarioState *m = &gMarioStates[0];
    f32 sensX = 0.3f * camera_config_get_x_sensitivity();
    f32 sensY = 0.4f * camera_config_get_y_sensitivity();

    if (mouse_relative_enabled) {
        // hack: make c buttons work for moving the camera
        s16 extStickX = m->controller->extStickX;
        s16 extStickY = m->controller->extStickY;
        if (extStickX == 0) {
            extStickX = (clamp(m->controller->buttonDown & L_CBUTTONS, 0, 1) - clamp(m->controller->buttonDown & R_CBUTTONS, 0, 1)) * 32;
        }
        if (extStickY == 0) {
            extStickY = (clamp(m->controller->buttonDown & U_CBUTTONS, 0, 1) - clamp(m->controller->buttonDown & D_CBUTTONS, 0, 1)) * 24;
        }

        // update pitch
        if (!gFirstPersonCamera.forcePitch) {
            gFirstPersonCamera.pitch -= sensY * (extStickY - 1.5f * mouse_y);
            gFirstPersonCamera.pitch = clamp(gFirstPersonCamera.pitch, -0x3F00, 0x3F00);
        }

        // update yaw
        if (!gFirstPersonCamera.forceYaw) {
            if (m->controller->buttonDown & L_TRIG && gFirstPersonCamera.centerL) {
                gFirstPersonCamera.yaw = m->faceAngle[1] + 0x8000;
            } else {
                gFirstPersonCamera.yaw += sensX * (extStickX - 1.5f * mouse_x);
            }
        }
    }

    // fix yaw for some specific actions
    // if the left stick is held, use Mario's yaw to set the camera's yaw
    // otherwise, set Mario's yaw to the camera's yaw
    u32 actions[] = { ACT_FLYING, ACT_HOLDING_BOWSER, ACT_TORNADO_TWIRLING, ACT_FLAG_ON_POLE, ACT_FLAG_SWIMMING, ACT_FLAG_SWIMMING_OR_FLYING };
#ifdef TARGET_VISIONOS
    // Not in VR. This ties the camera's heading to Mario's while swimming, and
    // in a headset that means the world turns under the viewer whenever he
    // does - which is felt as the view snapping away by itself. Here the
    // heading belongs to the snap turn and to nothing else; Mario follows it
    // through intendedYaw, not the other way round.
    extern bool mariovision_first_person_active(void);
    const s32 mvActionCount = mariovision_first_person_active() ? 0 : 6;
#else
    const s32 mvActionCount = 6;
#endif
    for (s32 i = 0; i < mvActionCount; i++) {
        u32 flag = actions[i];
        if ((m->action & flag) == flag) {
            if (ABS(m->controller->stickX) > 4) {
                gFirstPersonCamera.yaw = m->faceAngle[1] + 0x8000;
            } else {
                m->faceAngle[1] = gFirstPersonCamera.yaw - 0x8000;
            }
            break;
        }
    }
    if (m->action == ACT_LEDGE_GRAB) {
        gFirstPersonCamera.yaw = m->faceAngle[1] + 0x8000;
    }

    gLakituState.yaw = gFirstPersonCamera.yaw;
    m->area->camera->yaw = gFirstPersonCamera.yaw;

    // update crouch
    if (mario_is_crouching(m) || m->action == ACT_LEDGE_GRAB) {
        bool up = (m->controller->buttonDown & Z_TRIG) != 0 || m->action == ACT_CROUCH_SLIDE || m->action == ACT_LEDGE_GRAB;
        f32 inc = 10 * (up ? 1 : -1);
        gFirstPersonCamera.crouch = clamp(gFirstPersonCamera.crouch + inc, 0, FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_SHORT);
    } else {
        gFirstPersonCamera.crouch = clamp(gFirstPersonCamera.crouch - 10, 0, FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_SHORT);
    }

    if (m->action == ACT_LEDGE_GRAB) {
        gFirstPersonCamera.crouch = FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_SHORT;
    }

#ifdef TARGET_VISIONOS
    // Out of the trunk while hanging on one - see mariovision_pole_eye_offset,
    // which also gives the hands the same shift back so that they stay where
    // the player's really are.
    {
        extern void mariovision_pole_eye_offset(float out[3]);
        float out[3];
        mariovision_pole_eye_offset(out);
        gFirstPersonCamera.offset[0] = out[0];
        gFirstPersonCamera.offset[1] = out[1];
        gFirstPersonCamera.offset[2] = out[2];
    }
#endif

    // update pos
    gLakituState.pos[0] = (m->pos[0] + gFirstPersonCamera.offset[0]) + coss(gFirstPersonCamera.pitch) * sins(gFirstPersonCamera.yaw);
    gLakituState.pos[1] = (m->pos[1] + gFirstPersonCamera.offset[1]) + sins(gFirstPersonCamera.pitch) + (FIRST_PERSON_MARIO_HEAD_POS - gFirstPersonCamera.crouch);
    gLakituState.pos[2] = (m->pos[2] + gFirstPersonCamera.offset[2]) + coss(gFirstPersonCamera.pitch) * coss(gFirstPersonCamera.yaw);
    vec3f_copy(m->area->camera->pos, gLakituState.pos);
    vec3f_copy(gLakituState.curPos, gLakituState.pos);
    vec3f_copy(gLakituState.goalPos, gLakituState.pos);

    // update focus
    gLakituState.focus[0] = (m->pos[0] + gFirstPersonCamera.offset[0]) - 100 * coss(gFirstPersonCamera.pitch) * sins(gFirstPersonCamera.yaw);
    gLakituState.focus[1] = (m->pos[1] + gFirstPersonCamera.offset[1]) - 100 * sins(gFirstPersonCamera.pitch) + (FIRST_PERSON_MARIO_HEAD_POS - gFirstPersonCamera.crouch);
    gLakituState.focus[2] = (m->pos[2] + gFirstPersonCamera.offset[2]) - 100 * coss(gFirstPersonCamera.pitch) * coss(gFirstPersonCamera.yaw);
    vec3f_copy(m->area->camera->focus, gLakituState.focus);
    vec3f_copy(gLakituState.curFocus, gLakituState.focus);
    vec3f_copy(gLakituState.goalFocus, gLakituState.focus);

    // set other values
    if (gFirstPersonCamera.forceRoll) {
        gLakituState.roll = 0;
    }
    gLakituState.posHSpeed = 0;
    gLakituState.posVSpeed = 0;
    gLakituState.focHSpeed = 0;
    gLakituState.focVSpeed = 0;
    vec3s_set(gLakituState.shakeMagnitude, 0, 0, 0);
}

void first_person_update(void) {
    if (gFirstPersonCamera.enabled && !gDjuiInMainMenu) {
        struct MarioState *m = &gMarioStates[0];

        // check cancels
        bool cancel = first_person_check_cancels(m);
        if (cancel) { return; }

        if (m->action == ACT_SHOT_FROM_CANNON && m->area->camera->mode == CAMERA_MODE_INSIDE_CANNON) {
            gFirstPersonCamera.yaw = m->faceAngle[1] + 0x8000;
            m->area->camera->mode = CAMERA_MODE_FREE_ROAM;
        }

        if (gFirstPersonCamera.pitch <= -0x3F00 &&
            m->floor && m->floor->type == SURFACE_LOOK_UP_WARP &&
            save_file_get_total_star_count(gCurrSaveFileNum - 1, COURSE_MIN - 1, COURSE_MAX - 1) >= gLevelValues.wingCapLookUpReq &&
            m->forwardVel == 0 &&
            sCurrPlayMode != PLAY_MODE_PAUSED) {
            level_trigger_warp(m, WARP_OP_LOOK_UP);
        }

#ifdef TARGET_VISIONOS
        // coopdx hides Mario in first person by turning him transparent - this
        // bit puts him on the transparent layer, where he fades out entirely.
        // In VR his arms are meant to stay, and this took them with them: the
        // parts were being let through and were simply invisible, on layer 5
        // where they had been on layer 1.
        //
        // Here the body is hidden by collapsing its geometry instead, which
        // leaves the arms opaque and visible. So the transparency is skipped.
        extern bool mariovision_hide_mario_body(void);
        if (!mariovision_hide_mario_body())
#endif
        m->marioBodyState->modelState = 0x100;
        if (m->heldObj) {
            Vec3f camDir = {
                m->area->camera->focus[0] - m->area->camera->pos[0],
                m->area->camera->focus[1] - m->area->camera->pos[1],
                m->area->camera->focus[2] - m->area->camera->pos[2]
            };
            vec3f_normalize(camDir);
            vec3f_mul(camDir, 100);
            vec3f_sum(m->marioObj->header.gfx.pos, m->pos, camDir);
        }

        first_person_camera_update();
    }
}

void first_person_reset(void) {
    gFirstPersonCamera.forceRoll = false;
    gFirstPersonCamera.centerL = true;
    gFirstPersonCamera.pitch = 0;
    gFirstPersonCamera.yaw = 0;
    gFirstPersonCamera.crouch = 0;
    gFirstPersonCamera.fov = FIRST_PERSON_DEFAULT_FOV;
    gFirstPersonCamera.offset[0] = 0;
    gFirstPersonCamera.offset[1] = 0;
    gFirstPersonCamera.offset[2] = 0;
}
