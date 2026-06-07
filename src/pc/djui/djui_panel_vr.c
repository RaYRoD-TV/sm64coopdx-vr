#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_vr.h"
#include "pc/vr/vr.h"
#include "game/first_person_cam.h"

// In-game VR settings. DJUI sliders are integer-valued, so each one uses a small proxy that scales
// to/from the VR module's float tunables. Values apply live as you drag, and you can see the current
// value on each slider. "Reset to Default" puts everything back to the launch defaults.

static bool sFp;                // first person on/off
static bool sAntiClip;          // geometry anti-clip (diorama/close-up): keep the eye out of walls/floors
static unsigned int sMenuDistI; // menu distance, tenths of a meter (10..80 = 1.0..8.0 m)
static unsigned int sMenuSizeI; // menu width, tenths of a meter (20..120 = 2.0..12.0 m)
static unsigned int sDioDistI;  // diorama distance, hundredths offset by 2 m (0..300 = -2.0..1.0 m)
static unsigned int sDioSizeI;  // diorama scale, game units per meter (bigger = smaller world)
static unsigned int sDioHeightI;// diorama height, hundredths offset by 1 m (0..200 = -1.0..1.0 m)
static unsigned int sStereoI;   // stereo depth, hundredths (0..200 = 0.0..2.0)
static unsigned int sHeadI;     // 6DoF head-motion amount, hundredths (0..150 = 0.0..1.5; lower = steadier)

static unsigned int clampu(float v, unsigned int lo, unsigned int hi) {
    if (v < (float)lo) { return lo; }
    if (v > (float)hi) { return hi; }
    return (unsigned int)(v + 0.5f);
}

// Pull the live VR values into the slider proxies so the widgets show the real state.
static void vr_panel_seed_proxies(void) {
    sFp         = vr_first_person_active() || get_first_person_enabled();
    sAntiClip   = vr_anticlip_is_enabled();
    sMenuDistI  = clampu(vr_get_menu_dist()    * 10.0f,            10, 80);
    sMenuSizeI  = clampu(vr_get_menu_size()    * 10.0f,            20, 120);
    sDioDistI   = clampu((vr_get_diorama_dist()   + 2.0f) * 100.0f, 0, 300);
    sDioSizeI   = clampu(vr_get_diorama_scale(),                   30, 3000);
    sDioHeightI = clampu((vr_get_diorama_height() + 1.0f) * 100.0f, 0, 200);
    sStereoI    = clampu(vr_get_stereo()       * 100.0f,            0, 200);
    sHeadI      = clampu(vr_get_head_scale()   * 100.0f,            0, 150);
}

static void vr_panel_fp_changed(UNUSED struct DjuiBase* caller) {
    set_first_person_enabled(sFp);                    // game first-person camera (flatscreen + VR base view)
    if (vr_is_active()) { vr_set_first_person(sFp); } // VR: render life-size at Mario's head
}
static void vr_panel_menu_dist_changed(UNUSED struct DjuiBase* caller) { vr_set_menu_dist((float)sMenuDistI / 10.0f); }
static void vr_panel_menu_size_changed(UNUSED struct DjuiBase* caller) { vr_set_menu_size((float)sMenuSizeI / 10.0f); }
static void vr_panel_dio_dist_changed(UNUSED struct DjuiBase* caller)  { vr_set_diorama_dist((float)sDioDistI / 100.0f - 2.0f); }
static void vr_panel_dio_size_changed(UNUSED struct DjuiBase* caller)  { vr_set_diorama_scale((float)sDioSizeI); }
static void vr_panel_dio_height_changed(UNUSED struct DjuiBase* caller){ vr_set_diorama_height((float)sDioHeightI / 100.0f - 1.0f); }
static void vr_panel_stereo_changed(UNUSED struct DjuiBase* caller)    { vr_set_stereo((float)sStereoI / 100.0f); }
static void vr_panel_head_changed(UNUSED struct DjuiBase* caller)      { vr_set_head_scale((float)sHeadI / 100.0f); }
static void vr_panel_anticlip_changed(UNUSED struct DjuiBase* caller)  { vr_anticlip_set_enabled(sAntiClip); }

static void vr_panel_reset(UNUSED struct DjuiBase* caller) {
    vr_reset_defaults();
    set_first_person_enabled(false);
    vr_panel_seed_proxies(); // refresh the widgets to the default values
}

void djui_panel_vr_create(struct DjuiBase* caller) {
    vr_panel_seed_proxies();

    struct DjuiThreePanel* panel = djui_panel_menu_create("VR", false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
        // First person works in both flatscreen and VR, so it's always shown.
        djui_checkbox_create(body, "First Person", &sFp, vr_panel_fp_changed);

        // The rest only matters with VR running, so hide it in plain flatscreen sessions.
        if (vr_is_requested()) {
            djui_slider_create(body, "Menu Distance",    &sMenuDistI,  10, 80,   vr_panel_menu_dist_changed);
            djui_slider_create(body, "Menu Size",        &sMenuSizeI,  20, 120,  vr_panel_menu_size_changed);
            djui_slider_create(body, "Diorama Distance", &sDioDistI,   0,  300,  vr_panel_dio_dist_changed);
            djui_slider_create(body, "Diorama Size",     &sDioSizeI,   30, 3000, vr_panel_dio_size_changed);
            djui_slider_create(body, "Diorama Height",   &sDioHeightI, 0,  200,  vr_panel_dio_height_changed);
            djui_slider_create(body, "Stereo Depth",     &sStereoI,    0,  200,  vr_panel_stereo_changed);
            djui_slider_create(body, "Head Motion",      &sHeadI,      0,  150,  vr_panel_head_changed);
            djui_checkbox_create(body, "Camera Anti-Clip", &sAntiClip, vr_panel_anticlip_changed);
        }

        djui_button_create(body, "Reset to Default", DJUI_BUTTON_STYLE_NORMAL, vr_panel_reset);
        djui_button_create(body, DLANG(MENU, BACK),  DJUI_BUTTON_STYLE_BACK,   djui_panel_menu_back);
    }

    djui_panel_add(caller, panel, NULL);
}
