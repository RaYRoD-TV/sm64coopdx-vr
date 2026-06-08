#ifndef FIRST_PERSON_CAM_H
#define FIRST_PERSON_CAM_H

#include "types.h"

#define FIRST_PERSON_DEFAULT_FOV 70

#define FIRST_PERSON_MARIO_HEAD_POS 120
#define FIRST_PERSON_MARIO_HEAD_POS_SHORT 60

// Peak side-flip lean (s16 angle units; 0x2000 ~ 45 deg). A side flip tips the view this far toward the
// side it flips and settles back to level, so it reads as a side flip without a disorienting barrel roll.
#define FIRST_PERSON_SIDE_FLIP_LEAN 0x2000

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

// VR Tabletop: back out of (and block) the C-up look-around state, which would otherwise freeze movement.
void first_person_exit_lookaround_for_tabletop(void);

/* |description|Synthetic flip-roll angle (s16) for the current action when FP Flip Cam is on, else 0|descriptionEnd| */
s16 first_person_flip_roll(struct MarioState *m);
/* |description|Local player's flip roll in radians for the VR eye view (0 when not flipping)|descriptionEnd| */
f32 first_person_flip_roll_rad(void);
/* |description|True while a side flip is in progress (camera rolls to the side instead of pitching)|descriptionEnd| */
bool first_person_flip_is_side(struct MarioState *m);

#endif // FIRST_PERSON_CAM_H