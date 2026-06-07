// OpenXR VR support for the sm64coopdx PC port.
// Public interface - platform-agnostic, no OpenXR/GL types leak out.
#ifndef SM64COOPDX_VR_H
#define SM64COOPDX_VR_H

#include <stdbool.h>

// Requested via the --vr CLI flag.
void  vr_request_enable(void);
bool  vr_is_requested(void);

// Lightweight startup probe: is a VR headset actually connected right now? Creates and
// tears down a throwaway OpenXR instance (no GL context needed) and asks for an HMD system.
// Lets the same exe auto-enable VR when a headset is present and stay flat otherwise.
bool  vr_headset_present(void);

// Is an OpenXR session live and actively rendering?
bool  vr_is_active(void);

// Is true first-person mode on? When true the caller enables the game's first-person camera so the
// viewpoint sits at Mario's head; VR then renders it life-size.
bool  vr_first_person_active(void);

// In-game VR options menu accessors (driven by the DJUI "VR" panel).
void  vr_set_first_person(bool on);     // toggle the first-person preset (life-size, eye at Mario's head)
float vr_get_menu_dist(void);    void vr_set_menu_dist(float v);    // flat menu panel distance (meters)
float vr_get_menu_size(void);    void vr_set_menu_size(float v);    // flat menu panel width (meters)
float vr_get_diorama_dist(void); void vr_set_diorama_dist(float v); // diorama anchor distance (meters)
float vr_get_diorama_scale(void);void vr_set_diorama_scale(float v);// diorama scale (game units per meter; bigger=smaller world)
float vr_get_stereo(void);       void vr_set_stereo(float v);       // stereo separation strength
float vr_get_diorama_height(void); void vr_set_diorama_height(float v); // diorama height offset (meters)
float vr_get_head_scale(void);   void vr_set_head_scale(float v);   // 6DoF head-motion damping (0=locked, 1=full)
void  vr_reset_defaults(void);   // reset every VR tunable to launch defaults

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
void  vr_set_panel_world_lock(bool on); // true=world-locked (in-game menus), false=head-locked (title/main menu)
bool  vr_begin_panel(void);       // acquire + bind the panel render target (the HUD swapchain)
void  vr_end_panel(void);         // release the panel image (submitted as the sole opaque quad)

// Headset orientation offsets (radians) from facing forward - used to world-lock the skybox.
float vr_head_yaw_rad(void);
float vr_head_pitch_rad(void);

void  vr_shutdown(void);

#endif // SM64COOPDX_VR_H
