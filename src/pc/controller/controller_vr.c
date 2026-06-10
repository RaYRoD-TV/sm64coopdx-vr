// VR motion controllers (Quest Touch, Index) as a game controller.
//
// vr.c polls the OpenXR action state once per frame; this backend presents that state in the
// same virtual keyspace as the SDL gamepad. That one decision buys everything: the default
// binds give a sensible layout out of the box, the in-game rebind menu captures Quest buttons
// like any gamepad button, and the DJUI menus navigate exactly as they do with a gamepad.
//
// Default feel (with the stock binds):
//   left stick    move            right stick   camera (C buttons)
//   A             jump (A)        B             punch (B)
//   left trigger  Z (crouch)      right trigger R
//   left grip     L               right grip    R
//   menu button   Start (pause)   left stick click   Z
//   right stick click  d-pad up (cycles the VR view mode)
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
#include "controller_sdl.h" // VK_BASE_SDL_GAMEPAD / VK_BASE_SDL_MOUSE: we share the SDL gamepad keyspace
#include "pc/configfile.h"
#include "pc/vr/vr.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel_pause.h"

#define MAX_VRBINDS   32
#define MAX_VRBUTTONS 32 // virtual button indices, including the trigger virtual keys (0x1A/0x1B)

// Virtual button indices in the SDL gamepad keyspace (SDL_GameControllerButton values).
#define VBTN_A          0
#define VBTN_B          1
#define VBTN_X          2
#define VBTN_Y          3
#define VBTN_START      6
#define VBTN_LEFTSTICK  7
#define VBTN_RIGHTSTICK 8
#define VBTN_LSHOULDER  9
#define VBTN_RSHOULDER  10
#define VBTN_DPAD_UP    11
#define VBTN_LTRIGGER   (VK_LTRIGGER - VK_BASE_SDL_GAMEPAD)
#define VBTN_RTRIGGER   (VK_RTRIGGER - VK_BASE_SDL_GAMEPAD)

// Physical control -> virtual gamepad button. The right stick click lands on d-pad up so it
// cycles the VR view mode through the existing shortcut; everything else mirrors a gamepad.
static const struct { unsigned vrMask; int vbtn; } sVrButtonMap[] = {
    { VR_BTN_A,        VBTN_A },
    { VR_BTN_B,        VBTN_B },
    { VR_BTN_X,        VBTN_X },
    { VR_BTN_Y,        VBTN_Y },
    { VR_BTN_MENU,     VBTN_START },
    { VR_BTN_LSTICK,   VBTN_LEFTSTICK },
    { VR_BTN_RSTICK,   VBTN_DPAD_UP },
    { VR_BTN_LTRIGGER, VBTN_LTRIGGER },
    { VR_BTN_RTRIGGER, VBTN_RTRIGGER },
    { VR_BTN_LGRIP,    VBTN_LSHOULDER },
    { VR_BTN_RGRIP,    VBTN_RSHOULDER },
};

static u32  num_vr_binds = 0;
static u32  vr_binds[MAX_VRBINDS][2] = { 0 };
static bool vr_buttons[MAX_VRBUTTONS] = { false };
static u32  last_vrbutton = VK_INVALID;

// Gamepad-range binds only (the mouse subrange belongs to the mouse device).
static inline void vr_add_binds(const u32 mask, const u32 *btns) {
    for (u32 i = 0; i < MAX_BINDS; ++i) {
        if (btns[i] >= VK_BASE_SDL_GAMEPAD && btns[i] < VK_BASE_SDL_MOUSE && num_vr_binds < MAX_VRBINDS) {
            u32 idx = btns[i] - VK_BASE_SDL_GAMEPAD;
            if (idx < MAX_VRBUTTONS) {
                vr_binds[num_vr_binds][0] = idx;
                vr_binds[num_vr_binds][1] = mask;
                ++num_vr_binds;
            }
        }
    }
}

static void controller_vr_bind(void) {
    memset(vr_binds, 0, sizeof(vr_binds));
    num_vr_binds = 0;

    vr_add_binds(A_BUTTON,     configKeyA);
    vr_add_binds(B_BUTTON,     configKeyB);
    vr_add_binds(X_BUTTON,     configKeyX);
    vr_add_binds(Y_BUTTON,     configKeyY);
    vr_add_binds(Z_TRIG,       configKeyZ);
    vr_add_binds(STICK_UP,     configKeyStickUp);
    vr_add_binds(STICK_LEFT,   configKeyStickLeft);
    vr_add_binds(STICK_DOWN,   configKeyStickDown);
    vr_add_binds(STICK_RIGHT,  configKeyStickRight);
    vr_add_binds(U_CBUTTONS,   configKeyCUp);
    vr_add_binds(L_CBUTTONS,   configKeyCLeft);
    vr_add_binds(D_CBUTTONS,   configKeyCDown);
    vr_add_binds(R_CBUTTONS,   configKeyCRight);
    vr_add_binds(L_TRIG,       configKeyL);
    vr_add_binds(R_TRIG,       configKeyR);
    vr_add_binds(START_BUTTON, configKeyStart);
    vr_add_binds(U_JPAD,       configKeyDUp);
    vr_add_binds(D_JPAD,       configKeyDDown);
    vr_add_binds(L_JPAD,       configKeyDLeft);
    vr_add_binds(R_JPAD,       configKeyDRight);
}

static void controller_vr_init(void) {
    controller_vr_bind();
}

// Same press/release plumbing as the SDL backend so the DJUI menus see VR buttons as gamepad keys.
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
    for (size_t i = 0; i < sizeof(sVrButtonMap) / sizeof(sVrButtonMap[0]); i++) {
        if (vr & sVrButtonMap[i].vrMask) { held[sVrButtonMap[i].vbtn] = true; }
    }
    for (int i = 0; i < MAX_VRBUTTONS; i++) {
        if (vr_buttons[i] || held[i]) { vr_update_button(i, held[i]); }
    }

    if (!active) { return; }

    u32 buttons_down = 0;
    for (u32 i = 0; i < num_vr_binds; ++i) {
        if (vr_buttons[vr_binds[i][0]]) {
            buttons_down |= vr_binds[i][1];
        }
    }
    pad->button |= buttons_down;

    // Stick directions bound as buttons, same as the SDL backend.
    const u32 xstick = buttons_down & STICK_XMASK;
    const u32 ystick = buttons_down & STICK_YMASK;
    if (xstick == STICK_LEFT)       { pad->stick_x = -128; }
    else if (xstick == STICK_RIGHT) { pad->stick_x =  127; }
    if (ystick == STICK_DOWN)       { pad->stick_y = -128; }
    else if (ystick == STICK_UP)    { pad->stick_y =  127; }

    // Thumbsticks arrive as -1..1 with +y up; convert to the SDL int16 convention (+y down)
    // so the shared deadzone math and C-button thresholds behave identically.
    float ls[2], rs[2];
    vr_controller_stick(0, ls);
    vr_controller_stick(1, rs);
    int16_t leftx  = (int16_t)(ls[0] *  32767.0f);
    int16_t lefty  = (int16_t)(ls[1] * -32767.0f);
    int16_t rightx = (int16_t)(rs[0] *  32767.0f);
    int16_t righty = (int16_t)(rs[1] * -32767.0f);

    if (rightx < -0x4000) { pad->button |= L_CBUTTONS; }
    if (rightx >  0x4000) { pad->button |= R_CBUTTONS; }
    if (righty < -0x4000) { pad->button |= U_CBUTTONS; }
    if (righty >  0x4000) { pad->button |= D_CBUTTONS; }

    vr_update_analog_stick(&pad->stick_x, &pad->stick_y, leftx, lefty);
    vr_update_analog_stick(&pad->ext_stick_x, &pad->ext_stick_y, rightx, righty);
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
    VK_BASE_SDL_GAMEPAD, // shares the SDL gamepad keyspace: same binds, same rebind UI
    controller_vr_init,
    controller_vr_read,
    controller_vr_rawkey,
    controller_vr_rumble_play,
    controller_vr_rumble_stop,
    controller_vr_bind,
    controller_vr_shutdown
};
