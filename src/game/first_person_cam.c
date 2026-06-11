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
#include "ingame_menu.h" // get_dialog_id

#include "engine/math_util.h"

#include "pc/controller/controller_mouse.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_hud_utils.h"
#include "pc/vr/vr.h" // vr_is_active (flip cam pitches the eye in VR, the look here in flatscreen)
#include "pc/lua/utils/smlua_camera_utils.h"
#include "pc/lua/smlua_hooks.h"

struct FirstPersonCamera gFirstPersonCamera = {
    .enabled = false,
    .forcePitch = false,
    .forceYaw = false,
    .forceRoll = true,
    .centerL = true,
    .flipCam = false,
    .interactCam = true,
    .easeBack = 0.0f,
    .pitch = 0,
    .yaw = 0,
    .crouch = 0,
    .fov = FIRST_PERSON_DEFAULT_FOV,
    .offset = { 0, 0, 0 }
};

extern s16 gMenuMode;

bool first_person_check_cancels(struct MarioState *m) {
    // Note: ACT_FLYING is intentionally NOT cancelled, so first-person stays on while flying (wing cap).
    // The flying yaw/pitch is handled below in the per-action fix-up list.
    // ACT_READING_NPC_DIALOG is intentionally NOT cancelled either: cancelling it dropped first-person to
    // the NPC dialog camera (the view "got stuck" when you switched modes mid-conversation) AND it blocked
    // the interact ease-back, which triggers on get_dialog_id() and so never ran for characters. Staying in
    // first-person lets the ease-back pull back to show Mario talking, the way it already does for signs.
    if (m->action == ACT_FIRST_PERSON || m->action == ACT_IN_CANNON || m->action == ACT_DISAPPEARED) {
        return true;
    }
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
    // Switching view mode starts the interact ease-back fresh, so a half-eased camera left over from the
    // previous mode (e.g. switching modes mid-conversation) can't carry over and leave the view stuck.
    gFirstPersonCamera.easeBack = 0.0f;
}

// Accessors so the VR settings file (pc/vr/vr.c) can remember the first-person toggles without pulling in
// the whole first-person header. flipCam = roll/pitch the view with flips; interactCam = ease back on interact.
bool first_person_get_flip_cam(void)      { return gFirstPersonCamera.flipCam; }
void first_person_set_flip_cam(bool on)   { gFirstPersonCamera.flipCam = on; }
bool first_person_get_interact_cam(void)  { return gFirstPersonCamera.interactCam; }
void first_person_set_interact_cam(bool on){ gFirstPersonCamera.interactCam = on; }

// The death/fall cinematics (tip over and stay down, look back up the pit) need the death warp held
// back a beat to be seen at all - the warp's fade starts immediately and in VR also swaps the view to
// the flat panel. Death handlers gate their hold on this: local first person with FP Flip Cam on.
bool first_person_death_cinematic_active(void) {
    return get_first_person_enabled() && gFirstPersonCamera.flipCam;
}

// VR Tabletop: the C-up "look around" first-person state (right-stick up) freezes Mario and blocks movement,
// which is pointless in the tabletop diorama. While in tabletop, stop it engaging and back out if already in
// it. Called every frame from pc_main's VR loop (it no-ops outside tabletop).
void first_person_exit_lookaround_for_tabletop(void) {
    if (!vr_is_tabletop_mode()) { return; }
    gCameraMovementFlags &= ~CAM_MOVE_C_UP_MODE; // never enter C-up look-around in tabletop
    struct MarioState *m = &gMarioStates[0];
    if (m != NULL && m->action == ACT_FIRST_PERSON) {
        m->input &= ~INPUT_FIRST_PERSON;
        set_mario_action(m, ACT_IDLE, 0);
    }
}

// Prone body states: slides along the ground (butt/stomach/dive slides, slide kicks, the slide after a
// long jump) and being knocked flat by a hit or a fall. The eye drops toward the floor the same smooth
// way crouching/crawling already does (see the body-height block in first_person_camera_update).
static bool first_person_body_is_prone(struct MarioState *m) {
    switch (m->action) {
        case ACT_SLIDE_KICK_SLIDE:
        case ACT_DIVE_SLIDE:
        case ACT_LONG_JUMP_LAND:
        case ACT_BACKWARD_GROUND_KB:
        case ACT_FORWARD_GROUND_KB:
        case ACT_HARD_BACKWARD_GROUND_KB:
        case ACT_HARD_FORWARD_GROUND_KB:
        case ACT_GROUND_BONK:
            return true;
    }
    return (m->action & ACT_FLAG_BUTT_OR_STOMACH_SLIDE) != 0;
}

// Dying on the ground (or sinking in quicksand): the body ends up on the floor, so the eye sinks with
// it - slower than a slide so it tracks the collapse instead of beating it to the ground.
static bool first_person_body_is_dying(struct MarioState *m) {
    switch (m->action) {
        case ACT_STANDING_DEATH:
        case ACT_DEATH_ON_BACK:
        case ACT_DEATH_ON_STOMACH:
        case ACT_QUICKSAND_DEATH:
            return true;
    }
    return false;
}

static void first_person_camera_update(void) {
    struct MarioState *m = &gMarioStates[0];
    f32 sensX = 0.3f * camera_config_get_x_sensitivity();
    f32 sensY = 0.4f * camera_config_get_y_sensitivity();
    // Honor the camera Invert X/Y settings, the same way third-person (bettercamera) does via
    // newcam_ivrt(): invertX defaults false, invertY defaults true. Folding these into the yaw/pitch
    // below keeps the current default feel (invertY=true -> pitch stays "-=") while making the menu
    // toggle actually flip the axis in first-person, matching the third-person views.
    f32 invX = camera_config_is_x_inverted() ? -1.f : 1.f;
    f32 invY = camera_config_is_y_inverted() ? -1.f : 1.f;

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
            gFirstPersonCamera.pitch += invY * sensY * (extStickY - 1.5f * mouse_y);
            gFirstPersonCamera.pitch = clamp(gFirstPersonCamera.pitch, -0x3F00, 0x3F00);
        }

        // update yaw
        if (!gFirstPersonCamera.forceYaw) {
            if (m->controller->buttonDown & L_TRIG && gFirstPersonCamera.centerL) {
                gFirstPersonCamera.yaw = m->faceAngle[1] + 0x8000;
            } else {
                gFirstPersonCamera.yaw += invX * sensX * (extStickX - 1.5f * mouse_x);
            }
        }
    }

    // fix yaw for some specific actions
    // if the left stick is held, use Mario's yaw to set the camera's yaw
    // otherwise, set Mario's yaw to the camera's yaw
    u32 actions[] = { ACT_FLYING, ACT_HOLDING_BOWSER, ACT_TORNADO_TWIRLING, ACT_FLAG_ON_POLE, ACT_FLAG_SWIMMING, ACT_FLAG_SWIMMING_OR_FLYING };
    for (s32 i = 0; i < 6; i++) {
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
    // Ledge grab: snap to face the ledge the moment you grab it, then leave the yaw FREE so you can look
    // left/right while hanging instead of being locked staring at the wall.
    static u32 sFpLedgePrevAction = 0;
    if (m->action == ACT_LEDGE_GRAB && sFpLedgePrevAction != ACT_LEDGE_GRAB) {
        gFirstPersonCamera.yaw = m->faceAngle[1] + 0x8000;
    }
    sFpLedgePrevAction = m->action;

    gLakituState.yaw = gFirstPersonCamera.yaw;
    m->area->camera->yaw = gFirstPersonCamera.yaw;

    // update body height. Crouching/crawling drop the eye to the SHORT head height like before (Z held
    // keeps it down); PRONE bodies - ground slides like the one after a long jump, and being knocked
    // flat - drop further, so riding a slide or getting floored reads at body height, the way crawling
    // already does, instead of floating at standing height while the body scrapes along below.
    {
        bool prone = first_person_body_is_prone(m);
        f32 targetDrop = 0.0f;
        f32 rate = 10.0f;
        if (first_person_body_is_dying(m)) {
            targetDrop = FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_PRONE;
            rate = 6.0f; // sink with the collapse, don't beat it to the ground
        } else if (prone) {
            targetDrop = FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_PRONE;
            rate = 16.0f; // dives and slides hit the ground fast - catch up quicker than a crouch
        } else if (mario_is_crouching(m) || m->action == ACT_LEDGE_GRAB) {
            bool down = (m->controller->buttonDown & Z_TRIG) != 0 || m->action == ACT_CROUCH_SLIDE || m->action == ACT_LEDGE_GRAB;
            if (down) { targetDrop = FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_SHORT; }
        }
        f32 d = targetDrop - gFirstPersonCamera.crouch;
        gFirstPersonCamera.crouch += clamp(d, -rate, rate);
    }

    if (m->action == ACT_LEDGE_GRAB) {
        gFirstPersonCamera.crouch = FIRST_PERSON_MARIO_HEAD_POS - FIRST_PERSON_MARIO_HEAD_POS_SHORT;
    }

    // first-person eye pose at Mario's head
    Vec3f fpPos, fpFocus;
    fpPos[0] = (m->pos[0] + gFirstPersonCamera.offset[0]) + coss(gFirstPersonCamera.pitch) * sins(gFirstPersonCamera.yaw);
    fpPos[1] = (m->pos[1] + gFirstPersonCamera.offset[1]) + sins(gFirstPersonCamera.pitch) + (FIRST_PERSON_MARIO_HEAD_POS - gFirstPersonCamera.crouch);
    fpPos[2] = (m->pos[2] + gFirstPersonCamera.offset[2]) + coss(gFirstPersonCamera.pitch) * coss(gFirstPersonCamera.yaw);
    // Flatscreen flip cam: forward/back somersaults PITCH the look direction (triple jump / slide flip /
    // backflip / rollouts); the side flip ROLLs the screen instead (set below). The camera stays at the
    // head (fpPos keeps the plain pitch); only the view tilts. In VR the eye is rotated in vr.c instead,
    // so skip it here or it would double up.
    s16 flipAngle = vr_is_active() ? 0 : first_person_flip_roll(m);
    bool flipSide = first_person_flip_is_side(m);
    s16 lookPitch = gFirstPersonCamera.pitch + (flipSide ? 0 : flipAngle);
    fpFocus[0] = (m->pos[0] + gFirstPersonCamera.offset[0]) - 100 * coss(lookPitch) * sins(gFirstPersonCamera.yaw);
    fpFocus[1] = (m->pos[1] + gFirstPersonCamera.offset[1]) - 100 * sins(lookPitch) + (FIRST_PERSON_MARIO_HEAD_POS - gFirstPersonCamera.crouch);
    fpFocus[2] = (m->pos[2] + gFirstPersonCamera.offset[2]) - 100 * coss(lookPitch) * coss(gFirstPersonCamera.yaw);

    // Ease-back: when interacting (a dialog is open) or attacking, smoothly pull the camera back and up
    // so you see Mario do it, then ease back into first-person. Keeps you embodied but lets you watch
    // yourself act. Ease out is slower than ease in so rapid punches don't snap the camera around.
    // Sliding (butt/stomach/dive/crouch/slide-kick slides) is a MOVING attack: easing the camera back while
    // you slide along the ground reads as the world sliding out from under you in first-person, so suppress
    // the pull-back for slides and dives. Stationary attacks (punches/kicks) and dialog still ease back.
    bool isSlideOrDive = (m->action & ACT_FLAG_BUTT_OR_STOMACH_SLIDE) != 0
                      || (m->action & ACT_FLAG_DIVING) != 0
                      || ((m->action & ACT_FLAG_MOVING) != 0 && (m->action & ACT_FLAG_ATTACKING) != 0);
    bool wantPull = gFirstPersonCamera.interactCam
                 && (get_dialog_id() != DIALOG_NONE
                     || ((m->action & ACT_FLAG_ATTACKING) != 0 && !isSlideOrDive));
    f32 pTarget = wantPull ? 1.0f : 0.0f;
    // Ease-out: the old 0.06 rate took ~4 seconds to settle, so after closing a sign/dialog the view
    // visibly trailed behind ("stuck at the sign post"). Return faster in general, and the moment the
    // player is actually moving again snap back quickly - control regained means the show is over.
    f32 easeOutRate = 0.10f;
    if (!wantPull && ((m->action & ACT_FLAG_MOVING) || m->intendedMag > 0.5f)) { easeOutRate = 0.25f; }
    gFirstPersonCamera.easeBack += (pTarget - gFirstPersonCamera.easeBack)
                                 * ((pTarget > gFirstPersonCamera.easeBack) ? 0.12f : easeOutRate);

    if (gFirstPersonCamera.easeBack > 0.001f) {
        Vec3f look = { fpFocus[0] - fpPos[0], fpFocus[1] - fpPos[1], fpFocus[2] - fpPos[2] };
        vec3f_normalize(look);
        f32 t = gFirstPersonCamera.easeBack;
        f32 dist = 540.0f * t, up = 210.0f * t; // pull back far enough to frame Mario fully
        Vec3f marioHead = {
            m->pos[0] + gFirstPersonCamera.offset[0],
            m->pos[1] + gFirstPersonCamera.offset[1] + (FIRST_PERSON_MARIO_HEAD_POS - gFirstPersonCamera.crouch),
            m->pos[2] + gFirstPersonCamera.offset[2],
        };
        gLakituState.pos[0] = fpPos[0] - look[0] * dist;
        gLakituState.pos[1] = fpPos[1] - look[1] * dist + up;
        gLakituState.pos[2] = fpPos[2] - look[2] * dist;
        gLakituState.focus[0] = fpFocus[0] + (marioHead[0] - fpFocus[0]) * t;
        gLakituState.focus[1] = fpFocus[1] + (marioHead[1] - fpFocus[1]) * t;
        gLakituState.focus[2] = fpFocus[2] + (marioHead[2] - fpFocus[2]) * t;
    } else {
        vec3f_copy(gLakituState.pos, fpPos);
        vec3f_copy(gLakituState.focus, fpFocus);
    }
    vec3f_copy(m->area->camera->pos, gLakituState.pos);
    vec3f_copy(gLakituState.curPos, gLakituState.pos);
    vec3f_copy(gLakituState.goalPos, gLakituState.pos);
    vec3f_copy(m->area->camera->focus, gLakituState.focus);
    vec3f_copy(gLakituState.curFocus, gLakituState.focus);
    vec3f_copy(gLakituState.goalFocus, gLakituState.focus);

    // set other values. Forward/back flips pitch the view (the focus above); the side flip rolls the
    // screen here. Otherwise keep the roll level.
    gLakituState.roll = flipSide ? flipAngle : 0;
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

        // First-person: Mario is invisible, and snaps to SOLID (opaque, never half-transparent) only
        // once the camera has pulled CLEAR of his body. Below the threshold the camera is close to /
        // inside Mario, so he's hidden - otherwise easing back in would show the camera passing through
        // his solid body. Clearing only 0x100 keeps him opaque and preserves the cap-effect high byte.
        if (gFirstPersonCamera.easeBack > 0.30f) {
            m->marioBodyState->modelState &= ~0x100; // solid / opaque (camera is pulled clear of the body)
        } else {
            m->marioBodyState->modelState = 0x100;   // fully invisible (near the head; no body interior shown)
        }
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
    gFirstPersonCamera.flipCam = false;
    gFirstPersonCamera.interactCam = true;
    gFirstPersonCamera.easeBack = 0.0f;
    gFirstPersonCamera.pitch = 0;
    gFirstPersonCamera.yaw = 0;
    gFirstPersonCamera.crouch = 0;
    gFirstPersonCamera.fov = FIRST_PERSON_DEFAULT_FOV;
    gFirstPersonCamera.offset[0] = 0;
    gFirstPersonCamera.offset[1] = 0;
    gFirstPersonCamera.offset[2] = 0;
}

// Pole/tree actions. A wall-kick that LEAVES one of these is "jumping off a tree" (it flips); an ordinary
// wall kick off a wall is not (it doesn't), so the same ACT_WALL_KICK_AIR only flips when it came off a tree.
static bool fp_is_pole_action(u32 action) {
    return action == ACT_HOLDING_POLE || action == ACT_CLIMBING_POLE
        || action == ACT_GRAB_POLE_SLOW || action == ACT_GRAB_POLE_FAST
        || action == ACT_TOP_OF_POLE || action == ACT_TOP_OF_POLE_TRANSITION;
}

// Flip jumps (backflip / side flip / rollouts / tree jumps) rotate the model through baked animation, not
// the object angle, so there's nothing simple to read. Instead we SYNTHESIZE the camera move from the
// animation's progress. Forward/back somersaults PITCH a full turn; the side flip LEANS to the side it
// actually flips. Flatscreen applies this directly; VR injects it into the eye view (pc_main ->
// vr_set_flip_roll). Off unless the FP Flip Cam toggle is on, and it only moves the view - never control.
s16 first_person_flip_roll(struct MarioState *m) {
    if (!gFirstPersonCamera.flipCam || !gFirstPersonCamera.enabled || m == NULL || m->marioObj == NULL) { return 0; }

    // Per-frame bookkeeping (runs every frame the flip cam is on). The turn-around that triggers a side
    // flip spins Mario's facing; low-pass that yaw rate so that at the instant the side flip begins we can
    // read which way he turned - that's the side he flips toward. Also note when a wall-kick leaves a tree.
    static u32  sPrevAction = 0;
    static s16  sPrevFaceYaw = 0;
    static f32  sFaceYawVel = 0.0f;
    static s16  sSideSign = 1;            // +1 / -1: which way the current side flip leans
    static bool sWallKickFromPole = false;

    s16 dFace = (s16)(m->faceAngle[1] - sPrevFaceYaw);
    sPrevFaceYaw = m->faceAngle[1];
    sFaceYawVel = sFaceYawVel * 0.6f + (f32)dFace * 0.4f;

    if (m->action != sPrevAction) { // action just changed this frame
        if (m->action == ACT_SIDE_FLIP) {
            // Sample the turn direction; keep the previous sign if the turn was too small to read cleanly.
            if (sFaceYawVel >  64.0f) { sSideSign =  1; }
            else if (sFaceYawVel < -64.0f) { sSideSign = -1; }
        } else if (m->action == ACT_WALL_KICK_AIR) {
            sWallKickFromPole = fp_is_pole_action(sPrevAction); // jumped off a tree vs kicked off a wall
        }
    }
    sPrevAction = m->action;

    // Falling out of the level: once the only floor below Mario is the death plane, the fall is already
    // fatal. Ease the view up toward the level shrinking away above you and hold it there until the
    // death warp - a last look at where you fell from. Time-based (the freefall anims loop or hold, so
    // animation progress can't drive it) and advanced by GAME frames, so the VR render rate - which
    // calls this several times per game frame - doesn't change its speed.
    static f32 sFallOutRamp = 0.0f;
    static u32 sFallOutPrevTimer = 0;
    {
        extern u32 gGlobalTimer; // game_init.h
        u32 elapsed = gGlobalTimer - sFallOutPrevTimer;
        sFallOutPrevTimer = gGlobalTimer;
        if (elapsed > 4) { elapsed = 4; } // first call, or the toggle was off for a while
        bool fallingOut = (m->action & ACT_FLAG_AIR) && m->vel[1] < 0.0f
                       && m->floor != NULL && m->floor->type == SURFACE_DEATH_PLANE;
        sFallOutRamp += (fallingOut ? 1.0f : -2.0f) * (f32)elapsed / 45.0f; // ~1.5s up, faster back out
        if (sFallOutRamp < 0.0f) { sFallOutRamp = 0.0f; }
        if (sFallOutRamp > 1.0f) { sFallOutRamp = 1.0f; }
    }

    f32 dir = 0.0f;
    s16 tipPeak = 0;  // nonzero = PARTIAL tip that rises and settles (hurt / dive / melee / sweep)
    s16 holdPeak = 0; // nonzero = tip that eases over and STAYS down (deaths)
    bool strike = false; // melee: skew the tip so the lean LANDS with the hit, then eases out
    bool flips = true;
    switch (m->action) {
        case ACT_TRIPLE_JUMP:         dir =  1.0f; break; // forward (pitch)
        case ACT_SPECIAL_TRIPLE_JUMP: dir =  1.0f; break; // forward, star/cap triple jump (pitch)
        case ACT_FLYING_TRIPLE_JUMP:  dir =  1.0f; break; // forward, wing cap triple jump (pitch)
        case ACT_FORWARD_ROLLOUT:     dir =  1.0f; break; // forward roll (pitch)
        case ACT_BACKFLIP:            dir = -1.0f; break; // back flip (pitch)
        case ACT_BACKWARD_ROLLOUT:    dir = -1.0f; break; // backward roll (pitch)
        case ACT_TOP_OF_POLE_JUMP:    dir =  1.0f; break; // handstand jump off a tree top -> forward (pitch)
        case ACT_GROUND_POUND:    dir =  1.0f; break; // ground-pound spin -> forward (pitch); the straight-down slam anim doesn't advance, so it naturally settles level
        case ACT_SIDE_FLIP:           dir = (f32)sSideSign; break; // side flip -> directional LEAN (roll)
        case ACT_WALL_KICK_AIR:
            if (!sWallKickFromPole) { return 0; }         // ordinary wall kicks don't flip
            dir = -1.0f; break;                            // kicking off a tree -> back (pitch)
        // Falling and getting hurt: knockbacks TIP the view the way the hit threw Mario - backward hits
        // look up as you go down on your back, forward hits look down - then settle level. The ground
        // knockdowns pair with the prone eye drop (body height block) so being floored reads as lying
        // on the floor, and getting up brings both back together.
        case ACT_BACKWARD_AIR_KB:
        case ACT_HARD_BACKWARD_AIR_KB:
        case ACT_THROWN_BACKWARD:
        case ACT_BACKWARD_GROUND_KB:
        case ACT_HARD_BACKWARD_GROUND_KB:
            dir = -1.0f; tipPeak = FIRST_PERSON_HURT_PITCH; break;
        case ACT_FORWARD_AIR_KB:
        case ACT_HARD_FORWARD_AIR_KB:
        case ACT_THROWN_FORWARD:
        case ACT_FORWARD_GROUND_KB:
        case ACT_HARD_FORWARD_GROUND_KB:
        case ACT_GROUND_BONK:
            dir =  1.0f; tipPeak = FIRST_PERSON_HURT_PITCH; break;
        case ACT_SOFT_BONK:
            dir = -1.0f; tipPeak = FIRST_PERSON_HURT_PITCH / 2; break; // bumping a wall: a smaller jolt
        // Jump-slides: the dive (and, gentler, the long jump) lunges head-first. Lean the view into the
        // lunge and settle by the time the slide starts - the slide itself rides low through the prone
        // eye height, the same way a crawl does.
        case ACT_DIVE:                dir =  1.0f; tipPeak = FIRST_PERSON_DIVE_PITCH; break;
        case ACT_LONG_JUMP:           dir =  1.0f; tipPeak = FIRST_PERSON_DIVE_PITCH / 2; break;
        // Melee: lean WITH the strike - in or out, the way the animation moves the body - snapping in
        // fast and easing out through the follow-through (no symmetric rocking). Punches drive the body
        // forward, so the view leans in; the ground kick (combo finisher, arg 6+) and the jump kick arch
        // the body BACK behind the extended leg, so the view leans out. The punch combo swaps animations
        // per hit, so each hit gets its own lean. The crouch kick (the breakdance sweep, arg 9) instead
        // ROLLS the already-crouched view with the sweeping legs - first_person_flip_is_side routes it
        // to the roll axis like the side flip.
        case ACT_PUNCHING:
            if (m->actionArg == 9)      { dir = -1.0f; tipPeak = FIRST_PERSON_SWEEP_ROLL; }  // crouch sweep: roll
            else if (m->actionArg >= 6) { dir = -1.0f; tipPeak = FIRST_PERSON_MELEE_PITCH; } // ground kick: lean out
            else                        { dir =  1.0f; tipPeak = FIRST_PERSON_MELEE_PITCH; } // punches: lean in
            strike = true;
            break;
        case ACT_MOVE_PUNCHING:       dir =  1.0f; tipPeak = FIRST_PERSON_MELEE_PITCH; strike = true; break;
        case ACT_JUMP_KICK:           dir = -1.0f; tipPeak = FIRST_PERSON_MELEE_PITCH; strike = true; break;
        // Dying: ease the view over with the death animation and STAY down (the death anims hold their
        // last frame, so the tip holds until the death warp resets the run). Backward for the classic
        // standing death and dying on the back, forward on the stomach. Quicksand mostly SINKS (the
        // body-height block above) with a slight forward slump; drowning and the underwater death go
        // gently onto the back; electrocution is a jolt that recovers.
        case ACT_STANDING_DEATH:      dir = -1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH; break;
        case ACT_DEATH_ON_BACK:       dir = -1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH; break;
        case ACT_DEATH_ON_STOMACH:    dir =  1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH; break;
        case ACT_SUFFOCATION:         dir = -1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH / 2; break;
        case ACT_QUICKSAND_DEATH:     dir =  1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH / 4; break;
        case ACT_WATER_DEATH:         dir = -1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH / 2; break;
        case ACT_DROWNING:            dir = -1.0f; holdPeak = FIRST_PERSON_DEATH_PITCH / 2; break;
        case ACT_ELECTROCUTION:       dir = -1.0f; tipPeak = FIRST_PERSON_HURT_PITCH / 2; break;
        default:                      flips = false; break;
    }

    s16 actionAngle = 0;
    struct AnimInfo *a = &m->marioObj->header.gfx.animInfo;
    if (flips && a->curAnim != NULL && a->curAnim->loopEnd > 1) {
        f32 progress = (f32)a->animFrame / (f32)(a->curAnim->loopEnd - 1);
        if (progress < 0.0f) { progress = 0.0f; }
        if (progress > 1.0f) { progress = 1.0f; }

        if (tipPeak != 0) {
            // Partial tip: rises and settles back to level over the animation (half sine, 0 -> 1 -> 0),
            // so a hit or a lunge moves the view without ever flipping the world - and an animation that
            // holds its last frame (air knockbacks while falling) sits level until the next thing happens.
            // Strikes skew the curve (sqrt of progress): the lean lands WITH the hit a quarter of the way
            // into the animation, then eases out through the follow-through, instead of rocking evenly.
            f32 p = strike ? sqrtf(progress) : progress;
            f32 tip = sins((s16)(p * 32768.0f));
            actionAngle = (s16)(tip * dir * (f32)tipPeak);
        } else if (holdPeak != 0) {
            // Death tip: eases over with the animation and STAYS there (the death anims hold their last
            // frame), so you end the run lying the way Mario landed until the death warp resets things.
            f32 eased = progress * progress * (3.0f - 2.0f * progress);
            actionAngle = (s16)(eased * dir * (f32)holdPeak);
        } else if (m->action == ACT_SIDE_FLIP) {
            // A gentle lean toward the flip side that rises and settles back to level (half sine, 0 -> 1 -> 0),
            // so it reads as a side flip without a full barrel roll and never fights your control of Mario.
            f32 lean = sins((s16)(progress * 32768.0f)); // sin(pi * progress)
            actionAngle = (s16)(lean * dir * (f32)FIRST_PERSON_SIDE_FLIP_LEAN);
        } else {
            // Somersaults: a full eased turn over the animation (smoothstep 3t^2 - 2t^3 to ease in and out).
            f32 eased = progress * progress * (3.0f - 2.0f * progress);
            actionAngle = (s16)(eased * dir * 65536.0f);
        }
    }

    // Blend toward the fall-out-of-level look-up; it wins over whatever action carried you off the edge.
    if (sFallOutRamp > 0.001f) {
        f32 t = sFallOutRamp * sFallOutRamp * (3.0f - 2.0f * sFallOutRamp);
        return (s16)((f32)actionAngle * (1.0f - t) - (f32)FIRST_PERSON_FALL_DEATH_PITCH * t);
    }
    return actionAngle;
}

// Convenience for the VR bridge (pc_main): the local player's flip roll in radians. s16 angle units
// map 0x8000 -> pi, so radians = angle * pi / 32768. Returns 0 when not flipping or the toggle is off.
f32 first_person_flip_roll_rad(void) {
    return (f32)first_person_flip_roll(&gMarioStates[0]) * (3.14159265358979f / 32768.0f);
}

// A side flip rolls Mario sideways, so its camera should ROLL (tilt to the side) rather than pitch like
// the forward/back somersaults. The crouch kick (breakdance sweep) rolls too - the legs sweep around a
// low body. Callers route the flip angle to roll when this is true, pitch otherwise.
bool first_person_flip_is_side(struct MarioState *m) {
    if (!gFirstPersonCamera.flipCam || !gFirstPersonCamera.enabled || m == NULL) { return false; }
    // The fall-out-of-level look-up is a PITCH; while the fall is fatal (only the death plane below),
    // never route to the roll axis, even if the action that carried you off the edge was a side flip.
    if ((m->action & ACT_FLAG_AIR) && m->floor != NULL && m->floor->type == SURFACE_DEATH_PLANE) {
        return false;
    }
    if (m->action == ACT_SIDE_FLIP) { return true; }
    return m->action == ACT_PUNCHING && m->actionArg == 9; // crouch sweep kick: a low roll with the legs
}
