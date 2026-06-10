// OpenXR VR support for the sm64coopdx PC port.
// Public interface - platform-agnostic, no OpenXR/GL types leak out.
#ifndef SM64COOPDX_VR_H
#define SM64COOPDX_VR_H

#include <stdbool.h>

// Theater mode (the flat game on a big world-locked screen) isn't finished, so its menu entry, sliders
// and backdrop options are hidden for now. The implementation stays compiled and the settings keep
// saving/loading; flip this to 1 to bring the UI back.
#define VR_THEATER_ENABLED 0

// Requested via the --vr CLI flag.
void  vr_request_enable(void);
bool  vr_is_requested(void);

// Lightweight startup probe: is a VR headset actually connected right now? Creates and
// tears down a throwaway OpenXR instance (no GL context needed) and asks for an HMD system.
// Lets the same exe auto-enable VR when a headset is present and stay flat otherwise.
bool  vr_headset_present(void);

// Is an OpenXR session live and actively rendering?
bool  vr_is_active(void);

// Is the headset actually being worn (session focused)? False the instant it's taken off, even while the
// session is still running - use this to fall back to flat input behaviour (mouse, hotkeys) when doffed.
bool  vr_is_focused(void);

// Is true first-person mode on? When true the caller enables the game's first-person camera so the
// viewpoint sits at Mario's head; VR then renders it life-size.
bool  vr_first_person_active(void);

// In-game VR options menu accessors (driven by the DJUI "VR" panel).
void  vr_set_first_person(bool on);     // toggle the first-person preset (life-size, eye at Mario's head)
int   vr_get_preset_index(void); void vr_set_preset_index(int idx); // VR mode: 0=Diorama, 1=Third-person, 2=First-person, 3=Theater (hidden)
void  vr_cycle_preset(void);     // cycle to the next VR mode (bound to F10 and d-pad up); skips Theater while hidden
int   vr_get_preset_count(void); const char* vr_get_preset_name(int i);
float vr_get_menu_dist(void);    void vr_set_menu_dist(float v);    // flat menu panel distance (meters)
float vr_get_menu_size(void);    void vr_set_menu_size(float v);    // flat menu panel width (meters)
float vr_get_theater_dist(void); void vr_set_theater_dist(float v); // Theater screen distance (meters)
float vr_get_theater_size(void); void vr_set_theater_size(float v); // Theater screen width (meters)
// Theater backdrop: what fills the surround behind the cinema screen.
enum VrTheaterBg { VR_BG_BLACK = 0, VR_BG_PANORAMIC = 1, VR_BG_MODEL = 2 };
int   vr_get_theater_bg(void);   void vr_set_theater_bg(int mode);  // Theater backdrop selection
bool  vr_render_backdrop_pano(void); // render the user panorama (theater_bg.png) into the backdrop swapchain
float vr_get_diorama_dist(void); void vr_set_diorama_dist(float v); // diorama anchor distance (meters)
float vr_get_diorama_scale(void);void vr_set_diorama_scale(float v);// diorama scale (game units per meter; bigger=smaller world)
float vr_get_stereo(void);       void vr_set_stereo(float v);       // stereo separation strength
float vr_get_diorama_height(void); void vr_set_diorama_height(float v); // diorama height offset (meters)
float vr_get_head_scale(void);   void vr_set_head_scale(float v);   // 6DoF head-motion damping (0=locked, 1=full)
int   vr_get_refresh_rate(void);                                    // headset refresh in Hz (0 until the runtime reports it)
float vr_get_hud_size(void);     void vr_set_hud_size(float v);     // VR gameplay HUD panel width (meters)
float vr_get_world_scale(void);  void vr_set_world_scale(float v);  // first-person world scale (1.0=default, bigger=world feels bigger)
int   vr_get_look_yaw(void);                                        // VR first-person head look yaw offset (SM64 s16 angle units)
bool  vr_get_head_move(void);    void vr_set_head_move(bool e);     // opt-in: in VR first-person, move/turn toward where the head looks
bool  vr_is_tabletop_mode(void);                                    // true in the Tabletop preset (free orbit camera)
bool  vr_is_theater_mode(void);                                     // true in the Theater preset (flat game on a big screen)
void  vr_reset_defaults(void);   // reset every VR tunable to launch defaults
void  vr_settings_mark_dirty(void); // note a VR menu change so it gets saved to vr_settings.txt (debounced)

// Geometry anti-clip (diorama/close-up). pc_main computes the world-space eye + collision and writes
// back the anchor offset; vr.c just stores the per-frame geometry and applies the offset.
bool  vr_anticlip_is_enabled(void);   void vr_anticlip_set_enabled(bool e);
bool  vr_anticlip_get_head_campos(float out[3]); // cyclopean eye in game-camera space; false = skip this frame
void  vr_anticlip_set_offset(const float m[3]);  // smoothed anchor offset (meters)

// First-person flip cam: roll the eye view by this angle (radians) so the headset follows Mario's
// flip jumps. Set each frame from pc_main; 0 = upright. Only applied in first-person mode.
void  vr_set_flip_roll(float radians);
void  vr_set_flip_side(bool side); // flip cam axis: true = side flip (roll), false = forward/back (pitch)

// Per frame: lazy-boot OpenXR, poll events, xrWaitFrame/xrBeginFrame, locate the
// per-eye views. Call once before the eye loop.
void  vr_begin_frame(void);

// Per-eye stereo rendering, between vr_begin_frame() and vr_submit():
//   for (int e = 0; e < vr_eye_count(); e++)
//       if (vr_begin_eye(e)) {
//           gfx_run_dl_vr_eye(dl, vr_eye_offset(e), vr_eye_width(e), vr_eye_height(e));
//           vr_end_eye(e);
//       }
//   vr_submit();
int   vr_eye_count(void);
int   vr_eye_width(int eye);
int   vr_eye_height(int eye);
int   vr_overlay_width(void);
int   vr_overlay_height(void);
bool  vr_begin_overlay(bool sky);   // acquire + bind the sky (true) or HUD (false) render target
void  vr_end_overlay(bool sky);     // release the image (submitted as its own layer)
const float *vr_eye_viewproj(int eye); // camera-space -> eye-clip matrix (16 floats, row-vector)
const float *vr_sky_viewproj(int eye); // rotation-only sky view-proj for the world-locked eye-sphere dome
bool  vr_begin_eye(int eye);    // acquire + bind the eye render target; false to skip
void  vr_end_eye(int eye);      // release the image + record its projection view
void  vr_submit(void);          // xrEndFrame with the stereo projection layer

// FLATSCREEN-ON-A-PANEL: for ALL non-gameplay screens, render the normal flat frame once into the
// panel swapchain and present it on one large opaque head-locked quad (no diorama/dome/HUD split).
void  vr_set_panel_mode(bool on); // mark this frame as a flat panel (vr_submit emits panel-only)
void  vr_set_panel_full_frame(bool full); // crop: true=full 16:9 (in-game menus), false=4:3 (title/main menu); always world-locked
bool  vr_begin_panel(void);       // acquire + bind the panel render target (the HUD swapchain)
void  vr_end_panel(void);         // release the panel image (submitted as the sole opaque quad)

// Headset orientation offsets (radians) from facing forward - used to world-lock the skybox.
float vr_head_yaw_rad(void);
float vr_head_pitch_rad(void);

void  vr_shutdown(void);

#endif // SM64COOPDX_VR_H
