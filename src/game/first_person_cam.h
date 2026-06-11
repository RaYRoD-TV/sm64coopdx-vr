#ifndef FIRST_PERSON_CAM_H
#define FIRST_PERSON_CAM_H

#include "types.h"

#define FIRST_PERSON_DEFAULT_FOV 70

#define FIRST_PERSON_MARIO_HEAD_POS 120
#define FIRST_PERSON_MARIO_HEAD_POS_SHORT 60
// Eye height while the body is PRONE on the ground (belly/butt slides, long-jump slides, knocked flat).
// Lower than the crouch/crawl height: the head is basically at floor level when sliding on the stomach.
#define FIRST_PERSON_MARIO_HEAD_POS_PRONE 36

// Peak side-flip lean (s16 angle units; 0x2000 ~ 45 deg). A side flip tips the view this far toward the
// side it flips and settles back to level, so it reads as a side flip without a disorienting barrel roll.
#define FIRST_PERSON_SIDE_FLIP_LEAN 0x2000
// Peak hurt tip (s16 angle units; ~45 deg). Knockbacks tip the view the way the hit threw Mario - back
// hits look up as you land on your back, forward hits look down - and settle back to level.
#define FIRST_PERSON_HURT_PITCH 0x2000
// Peak dive lunge lean (~34 deg). The dive (and half of it for the long jump) leans the view into the
// head-first lunge and settles by the time the slide starts.
#define FIRST_PERSON_DIVE_PITCH 0x1800
// Peak melee lean (~7 deg). Punches lean the view in with the hit and the kicks lean back behind the
// extended leg; the lean is low-passed across the combo so mashing reads as one continuous subtle lean,
// not a rock. Kept SMALL on purpose - melee fires constantly, and anything bigger reads as jarring fast.
#define FIRST_PERSON_MELEE_PITCH 0x0500
// Peak death tip (~56 deg). Dying eases the view over with the death animation and HOLDS it there -
// backward for the classic standing death, forward for dying on the stomach - until the death warp.
#define FIRST_PERSON_DEATH_PITCH 0x2800
// Peak crouch-sweep roll (~31 deg). The crouch kick (breakdance sweep) rolls the already-low view with
// the sweeping legs and settles level, like a tighter side-flip lean.
#define FIRST_PERSON_SWEEP_ROLL 0x1600
// Fall-out-of-level look-up (~67 deg). Once the only floor below is the death plane the fall is already
// fatal, so the view eases up toward the level shrinking away above you and holds until the death warp.
#define FIRST_PERSON_FALL_DEATH_PITCH 0x3000
// Extra frames to lie where you died before the death warp fires (FP Flip Cam only). The warp's fade
// used to start the moment the death animation ended - and in VR the fade also swaps to the flat panel -
// so the tip-over never got its moment. Two seconds of lying there lets it land.
#define FIRST_PERSON_DEATH_HOLD_FRAMES 60

struct FirstPersonCamera {
    bool enabled;
    bool forcePitch;
    bool forceYaw;
    bool forceRoll;
    bool centerL;
    bool flipCam;    // first-person: roll the view with Mario's flip jumps (backflip / side flip / rollouts)
    bool interactCam;// first-person: ease the camera back to show Mario when interacting/attacking, then ease in
    f32 easeBack;    // 0..1 eased pullback amount (drives the camera pull AND Mario's fade-in visibility)
    s16 pitch;
    s16 yaw;
    f32 crouch;
    f32 fov;
    Vec3f offset;
};

extern struct FirstPersonCamera gFirstPersonCamera;

/* |description|Checks common cancels for first person|descriptionEnd| */
bool first_person_check_cancels(struct MarioState *m);

/* |description|Checks if first person is enabled|descriptionEnd| */
bool get_first_person_enabled(void);
/* |description|Sets if first person is enabled|descriptionEnd| */
void set_first_person_enabled(bool enable);

void first_person_update(void);
/* |description|Resets first person|descriptionEnd| */
void first_person_reset(void);

// First-person toggle accessors (used by the VR settings file so it can be remembered across launches).
bool first_person_get_flip_cam(void);       void first_person_set_flip_cam(bool on);
bool first_person_get_interact_cam(void);   void first_person_set_interact_cam(bool on);

// True when the first-person death/fall cinematic should run: local player in first person with the
// FP Flip Cam toggle on. Death handlers hold the death warp back a beat so the cinematic gets seen.
bool first_person_death_cinematic_active(void);

// VR Tabletop: back out of (and block) the C-up look-around state, which would otherwise freeze movement.
void first_person_exit_lookaround_for_tabletop(void);

/* |description|Synthetic flip-roll angle (s16) for the current action when FP Flip Cam is on, else 0|descriptionEnd| */
s16 first_person_flip_roll(struct MarioState *m);
/* |description|Local player's flip roll in radians for the VR eye view (0 when not flipping)|descriptionEnd| */
f32 first_person_flip_roll_rad(void);
/* |description|True while a side flip is in progress (camera rolls to the side instead of pitching)|descriptionEnd| */
bool first_person_flip_is_side(struct MarioState *m);

#endif // FIRST_PERSON_CAM_H