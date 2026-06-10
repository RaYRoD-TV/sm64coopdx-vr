// VR motion controllers (Quest Touch, Index, WMR, Vive) as a game controller.
//
// vr.c polls the OpenXR action state once per frame; this backend feeds it into the game.
//
// The button layout is FIXED: VR buttons map straight to N64 buttons and are deliberately NOT
// routed through the gamepad bindings. Gamepad binds carry personal flat-screen muscle memory
// (a punch rebound onto another face button, for example) and inheriting that scrambles the VR
// controllers in ways that look like wrong-hand bugs. The pad is what gameplay and the DJUI
// menus read, so everything below works the same for every config:
//
//   left stick    move                 right stick   camera (C buttons; analog look is scaled
//                                                    down, see VR_CAM_STICK_SCALE)
//   A             jump (A)             B             punch (B)
//   left trigger  crouch (Z)           right trigger R (and next page in menus)
//   either grip   grab / throw (B)     left trigger  also previous page while a menu is up (L)
//   menu button   pause (Start)        left stick click   Z
//   right stick click  d-pad up (cycles the VR view mode)
//
// Key events still go to DJUI with each control's physical gamepad-button identity (A=0, B=1,
// X=2, ...), which keeps chat/console/player-list shortcuts and bind capture working; menu
// select/back themselves run off the pad's A/B, so they follow the fixed layout above.
//
// When VR is off (or the headset is doffed) this backend reports nothing and releases
// anything it was holding, so buttons can't stick down across focus changes.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

#include <ultra64.h>

#include "controller_api.h"
#include "controller_vr.h"
#include "controller_sdl.h" // VK_BASE_SDL_GAMEPAD: the keyspace used for DJUI key events
#include "pc/configfile.h"
#include "pc/vr/vr.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_pause.h"

#define MAX_VRBUTTONS 32 // virtual button indices, including the trigger virtual keys (0x1A/0x1B)

// How much of the camera stick's deflection reaches the analog look (free cam, first person).
// 1.0 was too fast in the headset. Only the analog path is scaled; the C-button thresholds
// still trip at the same physical deflection.
#define VR_CAM_STICK_SCALE 0.55f

// Virtual key indices in the SDL gamepad keyspace (SDL_GameControllerButton values), used for
// the DJUI key events and bind capture only - the gameplay mapping is the pad table below.
#define VBTN_A          0
#define VBTN_B          1
#define VBTN_X          2
#define VBTN_Y          3
#define VBTN_START      6
#define VBTN_LEFTSTICK  7
#define VBTN_RIGHTSTICK 8
#define VBTN_LSHOULDER  9
#define VBTN_RSHOULDER  10
#define VBTN_LTRIGGER   (VK_LTRIGGER - VK_BASE_SDL_GAMEPAD)
#define VBTN_RTRIGGER   (VK_RTRIGGER - VK_BASE_SDL_GAMEPAD)

// Physical control -> N64 pad buttons, fixed. Both grips act as B: in SM64 the grab IS the B
// interaction, so squeezing near a box or a bob-omb picks it up and squeezing again throws it.
// The right stick click lands on d-pad up so it cycles the VR view mode through the existing
// shortcut. Several controls mapping to the same button just OR together.
static const struct { unsigned vrMask; u32 n64Mask; } sVrPadMap[] = {
    { VR_BTN_A,        A_BUTTON },
    { VR_BTN_B,        B_BUTTON },
    { VR_BTN_X,        X_BUTTON },
    { VR_BTN_Y,        Y_BUTTON },
    { VR_BTN_MENU,     START_BUTTON },
    { VR_BTN_LSTICK,   Z_TRIG },
    { VR_BTN_RSTICK,   U_JPAD },
    { VR_BTN_LTRIGGER, Z_TRIG },
    { VR_BTN_RTRIGGER, R_TRIG },
    { VR_BTN_LGRIP,    B_BUTTON },
    { VR_BTN_RGRIP,    B_BUTTON },
};

// Physical control -> virtual key for DJUI events and bind capture (physical identity).
static const struct { unsigned vrMask; int vk; } sVrKeyMap[] = {
    { VR_BTN_A,        VBTN_A },
    { VR_BTN_B,        VBTN_B },
    { VR_BTN_X,        VBTN_X },
    { VR_BTN_Y,        VBTN_Y },
    { VR_BTN_MENU,     VBTN_START },
    { VR_BTN_LSTICK,   VBTN_LEFTSTICK },
    { VR_BTN_RSTICK,   VBTN_RIGHTSTICK },
    { VR_BTN_LTRIGGER, VBTN_LTRIGGER },
    { VR_BTN_RTRIGGER, VBTN_RTRIGGER },
    { VR_BTN_LGRIP,    VBTN_LSHOULDER },
    { VR_BTN_RGRIP,    VBTN_RSHOULDER },
};

static bool vr_buttons[MAX_VRBUTTONS] = { false };
static u32  last_vrbutton = VK_INVALID;

static void controller_vr_init(void) {
    // Nothing to set up: vr.c boots OpenXR lazily and the mapping is fixed.
}

// Same press/release plumbing as the SDL backend so DJUI sees VR buttons as gamepad keys.
static void vr_update_button(const int i, const bool held) {
    const bool pressed   = !vr_buttons[i] && held;
    const bool unpressed = vr_buttons[i] && !held;
    vr_buttons[i] = held;
    if (pressed) {
        last_vrbutton = i;
        djui_panel_pause_disconnect_key_update(VK_BASE_SDL_GAMEPAD + i);
        djui_interactable_on_key_down(VK_BASE_SDL_GAMEPAD + i);
    }
    if (unpressed) {
        djui_interactable_on_key_up(VK_BASE_SDL_GAMEPAD + i);
    }
}

// Identical deadzone/scaling math to the SDL backend so the two feel the same.
static void vr_update_analog_stick(s8 *stick_x, s8 *stick_y, int16_t input_x, int16_t input_y) {
    float magnitude_sq = (float)(input_x * input_x) + (float)(input_y * input_y);
    float deadzone = configStickDeadzone * DEADZONE_STEP;

    if (magnitude_sq > (deadzone * deadzone)) {
        float magnitude = sqrtf(magnitude_sq);
        float dir_x = (float)input_x / magnitude;
        float dir_y = (float)input_y / magnitude;
        float scale = 1.f / fmaxf(fabsf(dir_x), fabsf(dir_y));
        float max_magnitude = 0x8000 * scale;

        magnitude -= deadzone;
        magnitude *= max_magnitude / (max_magnitude - deadzone);
        magnitude /= 0x100;
        magnitude = fminf(magnitude, scale * 127.f);

        *stick_x = dir_x * magnitude;
        *stick_y = -dir_y * magnitude;
    }
}

static void controller_vr_read(OSContPad *pad) {
    const bool active = vr_controllers_active();
    const unsigned vr = active ? vr_controller_buttons() : 0;

    // Run the edge detection even when inactive so anything held when the headset comes off
    // (or the session drops) gets its key-up instead of sticking down.
    bool held[MAX_VRBUTTONS] = { false };
    for (size_t i = 0; i < sizeof(sVrKeyMap) / sizeof(sVrKeyMap[0]); i++) {
        if (vr & sVrKeyMap[i].vrMask) { held[sVrKeyMap[i].vk] = true; }
    }
    for (int i = 0; i < MAX_VRBUTTONS; i++) {
        if (vr_buttons[i] || held[i]) { vr_update_button(i, held[i]); }
    }

    if (!active) { return; }

    u32 buttons_down = 0;
    for (size_t i = 0; i < sizeof(sVrPadMap) / sizeof(sVrPadMap[0]); i++) {
        if (vr & sVrPadMap[i].vrMask) { buttons_down |= sVrPadMap[i].n64Mask; }
    }
    // The paginated menus flip pages with L/R, and R already comes from the right trigger.
    // While a panel is up, the left trigger also reads as L; in gameplay it stays Z only
    // (a constant L would make some camera modes recenter on every crouch).
    if ((vr & VR_BTN_LTRIGGER) && djui_panel_is_active()) { buttons_down |= L_TRIG; }
    pad->button |= buttons_down;

    // Thumbsticks arrive as -1..1 with +y up; convert to the SDL int16 convention (+y down)
    // so the shared deadzone math and C-button thresholds behave identically. The camera X is
    // negated: the gamepad-style direction reads inverted in VR (the world visibly rotates
    // around you instead of a camera panning on a screen), so pushing right should look right.
    float ls[2], rs[2];
    vr_controller_stick(0, ls);
    vr_controller_stick(1, rs);
    int16_t leftx  = (int16_t)(ls[0] *  32767.0f);
    int16_t lefty  = (int16_t)(ls[1] * -32767.0f);
    int16_t rightx = (int16_t)(rs[0] * -32767.0f);
    int16_t righty = (int16_t)(rs[1] * -32767.0f);

    // C buttons trip at the raw deflection, unaffected by the analog look scale.
    if (rightx < -0x4000) { pad->button |= L_CBUTTONS; }
    if (rightx >  0x4000) { pad->button |= R_CBUTTONS; }
    if (righty < -0x4000) { pad->button |= U_CBUTTONS; }
    if (righty >  0x4000) { pad->button |= D_CBUTTONS; }

    vr_update_analog_stick(&pad->stick_x, &pad->stick_y, leftx, lefty);

    // Analog look: run the shared deadzone math at full range (so the deadzone feel matches the
    // movement stick), then scale the result down to slow the camera.
    s8 camx = 0, camy = 0;
    vr_update_analog_stick(&camx, &camy, rightx, righty);
    if (camx || camy) {
        pad->ext_stick_x = (s8)(camx * VR_CAM_STICK_SCALE);
        pad->ext_stick_y = (s8)(camy * VR_CAM_STICK_SCALE);
    }
}

static u32 controller_vr_rawkey(void) {
    if (last_vrbutton != VK_INVALID) {
        const u32 ret = last_vrbutton;
        last_vrbutton = VK_INVALID;
        return ret;
    }
    return VK_INVALID;
}

static void controller_vr_rumble_play(f32 strength, f32 length) {
    vr_controller_rumble(strength, length);
}

static void controller_vr_rumble_stop(void) {
    vr_controller_rumble_stop();
}

static void controller_vr_shutdown(void) {
    vr_controller_rumble_stop();
}

struct ControllerAPI controller_vr = {
    VK_BASE_SDL_GAMEPAD, // DJUI key events use the gamepad keyspace; the pad mapping is fixed
    controller_vr_init,
    controller_vr_read,
    controller_vr_rawkey,
    controller_vr_rumble_play,
    controller_vr_rumble_stop,
    NULL, // reconfig: nothing to rebuild, the VR layout doesn't follow the gamepad binds
    controller_vr_shutdown
};
