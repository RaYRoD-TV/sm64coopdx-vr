// OpenXR VR support for the sm64coopdx PC port.
//
// STEP 1 (true stereo): boot an OpenXR session bound to the GL context, create
// stereo swapchains, and render the game's display list ONCE PER EYE directly
// into each eye's swapchain image (with a depth buffer), injecting a per-eye
// horizontal view-space offset into fast3d (gfx_run_dl_vr_eye) so the two eyes
// differ -> real stereoscopic depth.
//
// GL-backend only (OpenXR is bound to WGL); --vr forces the GL backend.

#include "vr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- request flag (platform-agnostic, set by the --vr CLI handler) ----------
static bool sRequested = false;
void vr_request_enable(void) { sRequested = true; }
bool vr_is_requested(void) { return sRequested; }

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h> // defines IUnknown - referenced by openxr_platform.h's XR_USE_PLATFORM_WIN32 structs

#define GLEW_STATIC
#include <GL/glew.h>

#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> // SDL_GetKeyboardState - live diorama tuning keys

// ---- OpenXR state -----------------------------------------------------------
static XrInstance     sInstance   = XR_NULL_HANDLE;
static XrSystemId     sSystemId   = XR_NULL_SYSTEM_ID;
static XrSession      sSession    = XR_NULL_HANDLE;
static XrSpace        sLocalSpace = XR_NULL_HANDLE; // world-locked reference space
static XrSessionState sState      = XR_SESSION_STATE_UNKNOWN;

static bool sBootTried  = false; // boot attempted (success or fail) - don't retry every frame
static int  sBootRetryIn = 0;    // frames until the next boot retry (headset-not-ready is transient: keep trying)
static bool sBootRetrying = false; // a retry attempt (suppress the per-attempt boot chatter; success still prints)
static bool sRunning    = false; // session is between xrBeginSession/xrEndSession
static bool sFrameBegun = false; // xrBeginFrame issued, owes an xrEndFrame
static bool sViewsValid = false; // sViews holds per-eye poses for this frame (xrLocateViews succeeded)
static bool sPoseTracked = false; // the runtime reports the head pose as actually tracked (not just predicted/zero)

// --- persistent VR settings (vr_settings.txt next to the exe) -----------------
// Remembers the VR menu state across launches. Declared up here so vr_boot() can load before the preset
// table is defined further down. Saved debounced (sSettingsFlushIn frames after the last change) so a
// slider drag doesn't hammer the disk. The two first-person toggles + hide-HUD live on the game side, so
// they're reached through small accessors instead of pulling the game headers into this file.
static bool sSettingsDirty   = false;
static int  sSettingsFlushIn = 0;
static bool sSettingsLoaded  = false; // vr_settings.txt was read this session (gates the quit-time flush)
static void vr_settings_load(void);
static void vr_settings_save(void);
extern unsigned char gMenuHideHud;                                                      // game/hud.h
extern bool first_person_get_flip_cam(void);     extern void first_person_set_flip_cam(bool on);      // game/first_person_cam.h
extern bool first_person_get_interact_cam(void); extern void first_person_set_interact_cam(bool on);

static XrFrameState            sFrameState;
static uint32_t                sViewCount = 0;
static XrViewConfigurationView sViewConfigs[2];
static XrView                  sViews[2];

typedef struct {
    XrSwapchain handle;
    uint32_t w, h, imgCount;
    XrSwapchainImageOpenGLKHR *images;
} VrSwapchain;
static VrSwapchain sEye[2];

static GLuint   sEyeFbo     = 0; // render FBO: eye swapchain image (color) + depth
static GLuint   sEyeDepthRB = 0; // shared depth renderbuffer for eye rendering
static XrCompositionLayerProjectionView sProjViews[2];
static uint32_t sEyeImgIdx[2];

// 2D split across two layers: sky (world-anchored) + HUD/menus (head-locked).
static XrSpace     sViewSpace      = XR_NULL_HANDLE; // VIEW reference space (head-locked)
static VrSwapchain sOverlay;                         // sky swapchain (world-anchored)
static VrSwapchain sHud;                             // HUD/menu swapchain (head-locked)
static GLuint      sOverlayFbo     = 0;              // shared render FBO for both
static GLuint      sOverlayDepthRB = 0;
static uint32_t    sOverlayImgIdx  = 0;
static uint32_t    sHudImgIdx      = 0;
static bool        sOverlayReady   = false;          // sky rendered this frame
static bool        sHudReady       = false;          // HUD rendered this frame
static bool        sPanelMode      = false;          // this frame is a flat panel (non-gameplay screen)
static const int   sOverlayW = 1920;
static const int   sOverlayH = 1080;
static bool        sHasCylinder = false; // runtime supports the cylinder layer (surround sky)
static bool        sHasEquirect2 = false; // runtime supports the equirect2 layer (full-sphere sky, no black poles)

// --- Diorama transform tunables ----------------------------------------------
// The game world (camera space) is shrunk and anchored in front of the seated
// viewer; you see it stereoscopically and can lean around it. Tuned by feel.
static float sDioramaScale  = 1200.0f; // game units per meter (larger = smaller diorama)
static float sDioramaDist   = -0.17f;  // meters the anchor sits in front (-Z); default = Third-person preset
static float sDioramaHeight = 0.0f;    // meters above LOCAL origin (~eye height); default = Third-person preset
static float sDioramaPitchRad = 0.0f;  // constant nose-down view tilt for diorama/close-up (rad); per-preset, 0 = level
static float sClipMargin    = 0.30f;   // anti-clip: keep the diorama this far from the head (backs off on lean-in)
// (eye-space clip planes are computed per-frame from sDioramaScale so a bigger
//  world isn't far-clipped to black - see vr_build_eye_matrix.)

// Geometry anti-clip (diorama/close-up only). pc_main reads the cyclopean eye position in game-camera
// space, converts it to world via the game camera, runs level collision, and writes back an anchor
// offset (meters) that nudges the whole shrunk world off any wall/floor/ceiling the eye pokes into.
// First-person is excluded (moving the eye off the head causes sickness).
static bool  sAnticlipEnabled    = true;
static bool  sHeadMoveEnabled    = false; // opt-in: in VR first-person, move/turn toward where the head looks
static float sAnticlipOffsetM[3] = { 0.0f, 0.0f, 0.0f }; // applied anchor offset (smoothed, in vr_build_eye_matrix)
static float sHeadCamPos[3]      = { 0.0f, 0.0f, 0.0f }; // cyclopean eye in game-camera space (game units)
static bool  sHeadCamPosValid    = false;

// First-person flip cam: roll (radians) applied to the eye view so the headset follows Mario's flips.
static float sFlipRollRad        = 0.0f;
static bool  sFlipIsSide         = false; // flip cam: true = side flip (roll about forward), false = pitch

// Sky: a world-anchored cylinder that wraps around you (falls back to a flat quad
// if the cylinder layer isn't supported). HUD/menu panel: head-locked, near.
static float sSkyRadius   = 8.0f;       // cylinder radius (meters) - distance to the sky surface
static float sSkyAngle    = 6.2831853f;  // full 360 wrap - fed by the full-panorama capture (step 2)
static float sOverlayDist = 6.0f;       // flat-quad fallback distance
static float sOverlaySize = 16.0f;      // flat-quad fallback width
static float sHudDist     = 2.0f;       // HUD panel distance (head-locked, -Z in VIEW)
static float sHudSize     = 2.4f;       // HUD panel width in meters (height follows aspect)
// 2D-only screens (menus / title / course-select / file-select / dialog): present the WHOLE
// framebuffer as a large OPAQUE head-locked "theater" panel filling a comfortable FOV, so the
// menu's real background fills the view instead of a small transparent quad in a black void.
static float sMenuDist    = 3.0f;       // menu panel distance (head-locked, -Z in VIEW); pushed back from 2.0
static float sMenuSize    = 4.8f;       // menu panel width in meters (~2x HUD; raise to overfill the FOV)
static float sTheaterDist = 4.0f;       // Theater screen distance (meters) - sit back from a big screen
static float sTheaterSize = 9.0f;       // Theater screen width (meters) - cinema-sized; the flat game plays on it
static int   sTheaterBg = 0;            // Theater backdrop: 0=Black, 1=Panoramic image, 2=3D Model (see VrTheaterBg)
static GLuint sBgTex = 0;               // user panorama (theater_bg.png) uploaded once
static int   sBgW = 0, sBgH = 0;
static bool  sBgLoadTried = false;      // attempted the PNG load (so a miss isn't retried every frame)
static bool  sBackdropReady = false;    // sOverlay was acquired+blitted THIS frame (gate the backdrop layer on this, NOT the shared sOverlayReady)

// True first-person: enables coopdx's first-person camera (game camera at Mario's head) and renders the
// world life-size at 1:1 instead of the shrunk diorama. Free-look: the headset looks around freely, the
// stick still moves and turns Mario like the flat game. Selected via the "First-person" preset (F10) or
// the in-game VR menu.
static bool sFirstPerson = false;
static float sWorldScale = 1.0f; // first-person World Scale (>1 = world feels bigger); diorama/close-up ignore it

// Menu panel: ALWAYS world-locked so you can turn your head to look around any menu (title + in-game).
// sPanelFullFrame only chooses the crop/shape: in-game menus (Player/DynOS lists run to the edges) show
// the full 16:9 frame; the title/main menu shows the centered 4:3 region on a taller quad (so it has no
// empty void top/bottom). Set per-frame by pc_main via vr_set_panel_full_frame().
static bool  sPanelFullFrame   = true;
static bool  sPanelAnchorValid = false;
static float sPanelAnchorPos[3] = {0.0f, 0.0f, 0.0f};
static float sPanelAnchorQy = 0.0f, sPanelAnchorQw = 1.0f; // yaw-only orientation captured at menu open
static float sPanelEyeY      = 0.0f;   // locked eye-level Y for the panel (captured once when tracked)
static bool  sPanelEyeYValid = false;  // so the panel sits at eye level and doesn't drift low on re-anchor
static float sMenuVOffset    = 0.0f;   // vertical nudge for the menu panel (meters; + raises it)

// Per-eye camera-space -> eye-clip matrices (row-vector; clip = p_cam * sEyeVP).
static float sEyeVP[2][4][4];
static float sSkyVP[2][4][4]; // rotation-only sky view-proj for the eye-sphere dome (at infinity)

// Stereo comfort: scales each eye's offset from the head center (1.0 = true IPD,
// lower = gentler stereo / less cross-eye). sRenderPose is the (separation-adjusted)
// pose actually used to render AND submitted to the compositor, so they stay consistent.
static float   sStereoScale = 0.21f; // dialed-in comfortable default (symmetric frustum keeps it fusing near + far)
static float   sHeadScale   = 0.4f;  // 6DoF damping: scales head translation from rest (1=full parallax, 0=locked)
static float   sHeadRest[3] = {0,0,0};
static bool    sHeadRestSet = false;
static int     sHeadWarmup  = 0;     // frames before capturing rest (lets tracking settle so we don't grab a bad pose)
static float   sHeadRestYawQy = 0.0f, sHeadRestYawQw = 1.0f; // INVERSE yaw captured at rest, to recenter the view on the user's initial gaze
static float   sHeadRestYawRad = 0.0f; // HMD yaw (radians) at rest, so head-direction movement is relative to your starting gaze
static bool    sYawRecenterSet = false;
static XrPosef sRenderPose[2];
static XrFovf  sRenderFov[2]; // symmetrized fov actually rendered + submitted

static PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetGLReq = NULL;

bool vr_is_active(void)      { return sRunning; }
// Headset actually being worn (session focused). Drops to false the moment you take the headset off (the
// runtime moves FOCUSED -> VISIBLE), so input that only makes sense in VR (the mode hotkey, mouse capture)
// can fall back to flat behaviour while the headset is off, even though the session is still running.
bool vr_is_focused(void)     { return sRunning && sState == XR_SESSION_STATE_FOCUSED; }
bool vr_first_person_active(void) { return sFirstPerson; }
int  vr_eye_count(void)      { return (int)sViewCount; }
int  vr_eye_width(int eye)   { return (eye >= 0 && eye < 2) ? (int)sEye[eye].w : 0; }
int  vr_eye_height(int eye)  { return (eye >= 0 && eye < 2) ? (int)sEye[eye].h : 0; }
int  vr_overlay_width(void)  { return sOverlayW; }
int  vr_overlay_height(void) { return sOverlayH; }
const float *vr_eye_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sEyeVP[eye][0][0] : NULL;
}
const float *vr_sky_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sSkyVP[eye][0][0] : NULL;
}

// --- matrix helpers (row-vector convention: clip = v * M, M[row][col]) -------
static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j] + a[i][3]*b[3][j];
    memcpy(out, t, sizeof(t));
}

// OpenXR fov -> projection (row-vector, OpenGL clip z in [-1, 1]).
static void mat_proj_fov(float m[4][4], XrFovf fov, float zn, float zf) {
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float dn = tanf(fov.angleDown), up = tanf(fov.angleUp);
    float w = r - l, h = up - dn;
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = 2.0f / w;
    m[1][1] = 2.0f / h;
    m[2][0] = (r + l) / w;
    m[2][1] = (up + dn) / h;
    m[2][2] = -(zf + zn) / (zf - zn);
    m[2][3] = -1.0f;
    m[3][2] = -(2.0f * zf * zn) / (zf - zn);
}

// XrPosef -> world->eye view matrix (row-vector): rigid inverse of the eye's
// world transform (rotate orientation, translate position).
static void mat_view_from_pose(float m[4][4], XrPosef pose) {
    float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    // Row-vector rotation Rrv (v_row * Rrv rotates a point in the eye's frame).
    float Rrv[3][3] = {
        { 1.0f-2.0f*(y*y+z*z), 2.0f*(x*y+z*w),       2.0f*(x*z-y*w)       },
        { 2.0f*(x*y-z*w),      1.0f-2.0f*(x*x+z*z),  2.0f*(y*z+x*w)       },
        { 2.0f*(x*z+y*w),      2.0f*(y*z-x*w),       1.0f-2.0f*(x*x+y*y)  },
    };
    // View 3x3 = transpose(Rrv); translation row = -position rotated by it.
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = Rrv[j][i];
    m[0][3] = m[1][3] = m[2][3] = 0.0f;
    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    m[3][0] = -(px*m[0][0] + py*m[1][0] + pz*m[2][0]);
    m[3][1] = -(px*m[0][1] + py*m[1][1] + pz*m[2][1]);
    m[3][2] = -(px*m[0][2] + py*m[1][2] + pz*m[2][2]);
    m[3][3] = 1.0f;
}

// World-Y (yaw-only) rotation from a yaw quaternion (0,qy,0,qw), row-vector convention (matches Rrv
// in mat_view_from_pose). Used to recenter the view on the user's initial gaze yaw at startup.
static void mat_yaw_from_quat(float m[4][4], float qy, float qw) {
    float c = 1.0f - 2.0f * qy * qy;
    float s = 2.0f * qw * qy;
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = c; m[0][2] = -s;
    m[1][1] = 1.0f;
    m[2][0] = s; m[2][2] = c;
    m[3][3] = 1.0f;
}

// Build sEyeVP[eye] = A (camera->world diorama) * V (world->eye) * P (eye proj).
static void vr_build_eye_matrix(int eye) {
    // Head center (real, in meters).
    XrPosef pose = sViews[eye].pose;
    float cx = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
    float cy = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
    float cz = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);

    // 6DoF damping: the diorama is a shrunken world, so full head translation makes
    // parallax feel twitchy. Scale the head's movement (relative to a captured rest
    // pose) by sHeadScale. Rotation is untouched, so you still look around freely.
    // Persistent 6DoF damping: scale the head's offset from a captured rest pose so
    // ALL leans/translations are reduced (not just fast jerks). Capture rest after a
    // short warmup so tracking has settled (a frame-1 capture put the eye in the floor).
    if (eye == 0 && !sHeadRestSet && sPoseTracked && ++sHeadWarmup >= 15) {
        sHeadRest[0] = cx; sHeadRest[1] = cy; sHeadRest[2] = cz; sHeadRestSet = true;
        sHeadRestYawRad = vr_head_yaw_rad(); // baseline gaze yaw for head-direction movement
        printf("[VR] 6DoF rest captured at (%.2f, %.2f, %.2f)\n", cx, cy, cz);
        // Snapshot the INVERSE of the initial head yaw so the view recenters on the user's starting gaze:
        // the diorama ends up dead ahead no matter which way they were physically facing at launch.
        {
            float qy = sViews[0].pose.orientation.y, qw = sViews[0].pose.orientation.w;
            float n = sqrtf(qy*qy + qw*qw);
            if (n < 1e-6f) { qy = 0.0f; qw = 1.0f; n = 1.0f; }
            sHeadRestYawQy = -qy / n; // conjugate (negate qy) = inverse yaw
            sHeadRestYawQw =  qw / n;
            sYawRecenterSet = true;
        }
    }
    float dcx = cx, dcy = cy, dcz = cz; // full pose until rest is captured (no damping yet)
    if (sHeadRestSet) {
        dcx = sHeadRest[0] + (cx - sHeadRest[0]) * sHeadScale;
        dcy = sHeadRest[1] + (cy - sHeadRest[1]) * sHeadScale;
        dcz = sHeadRest[2] + (cz - sHeadRest[2]) * sHeadScale;
    }

    // Stereo comfort: keep the IPD offset (scaled by sStereoScale) around the damped center.
    pose.position.x = dcx + (pose.position.x - cx) * sStereoScale;
    pose.position.y = dcy + (pose.position.y - cy) * sStereoScale;
    pose.position.z = dcz + (pose.position.z - cz) * sStereoScale;
    sRenderPose[eye] = pose;

    // Anti-clip: if the (damped) head leans toward the diorama anchor, push the
    // anchor farther so you can't poke your face through the near geometry.
    float adx = dcx, ady = dcy - sDioramaHeight, adz = dcz + sDioramaDist;
    float headToAnchor = sqrtf(adx*adx + ady*ady + adz*adz);
    float effDist = sDioramaDist;
    // First-person puts the eye AT the camera (anchor distance ~0), so the diorama anti-clip would
    // shove the world away from your face. Skip it in first-person.
    if (!sFirstPerson && headToAnchor < sClipMargin) effDist += (sClipMargin - headToAnchor);

    float A[4][4] = {{0}};
    float invS = 1.0f / sDioramaScale;
    // First-person World Scale. A uniform geometry scale (multiplying invS) is PERSPECTIVE-INVARIANT: a
    // proportionally-scaled world viewed from a proportionally-scaled eye projects to the identical image, so
    // it looked like nothing happened (only a faint stereo shift). Instead we change how big the world FEELS
    // by shifting your eye height as if you were a different SIZE: worldScale > 1 lowers the eye so the world
    // towers over you (feels bigger / you feel small); < 1 raises it so the world shrinks below you. This is a
    // render-only shift (the game camera and gameplay are untouched) and it is actually visible.
    float worldScaleEyeOffsetM = 0.0f;
    if (sFirstPerson && sWorldScale > 0.0f) {
        const float baseEyeM = 1.1f; // ~life-size eye height in the render
        worldScaleEyeOffsetM = baseEyeM * (1.0f - 1.0f / sWorldScale);
        if (eye == 0) { static int sWsDbg = 0; if ((sWsDbg++ % 90) == 0) printf("[WSCALE] worldScale=%.2f eyeOffset=%.2fm\n", sWorldScale, worldScaleEyeOffsetM); }
    }
    A[0][0] = invS; A[1][1] = invS; A[2][2] = invS; A[3][3] = 1.0f;
    A[3][0] = sAnticlipOffsetM[0];
    A[3][1] = sDioramaHeight + sAnticlipOffsetM[1] + worldScaleEyeOffsetM;
    A[3][2] = -effDist + sAnticlipOffsetM[2];

    // Cyclopean eye position in game-camera space (the space A consumes), computed against the BASE
    // anchor (NO anti-clip offset) - the offset is the OUTPUT pc_main derives from this raw position,
    // so it must converge to zero when the raw eye is clear of geometry. pc_main converts this to world
    // units and runs collision (diorama/close-up only; first-person is excluded).
    if (eye == 0) {
        sHeadCamPos[0] = (dcx - 0.0f)            * sDioramaScale;
        sHeadCamPos[1] = (dcy - sDioramaHeight)  * sDioramaScale;
        sHeadCamPos[2] = (dcz - (-effDist))      * sDioramaScale; // dcz + effDist
        sHeadCamPosValid = sViewsValid && !sFirstPerson;
    }

    // Clip planes adapt to the world scale so a big/life-size world isn't
    // far-clipped to black. SM64 geometry spans ~±8000 units.
    float worldHalfM = 8000.0f * invS; // tracks invS (incl. first-person World Scale) so a bigger world isn't far-clipped
    float zn = 0.02f;
    float zf = (sDioramaDist > 0.0f ? sDioramaDist : 0.0f) + worldHalfM * 3.0f + 5.0f;

    // Symmetrize the fov (drop the off-axis term) so reducing the eye separation
    // (sStereoScale) scales ALL disparities proportionally and keeps infinity at
    // zero disparity - otherwise near and far can't both fuse at once.
    XrFovf fov = sViews[eye].fov;
    float aH = fmaxf(fabsf(fov.angleLeft), fabsf(fov.angleRight));
    float aV = fmaxf(fabsf(fov.angleUp),   fabsf(fov.angleDown));
    fov.angleLeft = -aH; fov.angleRight = aH;
    fov.angleDown = -aV; fov.angleUp = aV;
    sRenderFov[eye] = fov;

    // Rotate the eye view for the flip cam / diorama tilt. First-person: forward/back somersaults PITCH
    // about eye-space X (sFlipRollRad), the side flip ROLLs about eye-space Z, the forward axis
    // (sFlipIsSide picks which). Diorama/close-up: a constant nose-DOWN pitch (sDioramaPitchRad) so the
    // shrunk world sits below your gaze. Built once; reused for the eye and sky dome so they stay locked.
    float pitchRad = 0.0f, rollRad = 0.0f;
    if (sFirstPerson) {
        if (sFlipIsSide) { rollRad = sFlipRollRad; } else { pitchRad = sFlipRollRad; }
    } else {
        pitchRad = -sDioramaPitchRad;
    }
    bool  doFlipRoll = (pitchRad != 0.0f || rollRad != 0.0f);
    float flipRx[4][4] = {{0}};
    flipRx[0][0] = flipRx[1][1] = flipRx[2][2] = flipRx[3][3] = 1.0f; // identity base
    if (pitchRad != 0.0f) {                 // pitch about X (forward/back flip, diorama tilt)
        float cp = cosf(pitchRad), sp = sinf(pitchRad);
        flipRx[1][1] = cp;  flipRx[1][2] = sp;
        flipRx[2][1] = -sp; flipRx[2][2] = cp;
    } else if (rollRad != 0.0f) {           // roll about Z = forward (side flip)
        float cr = cosf(rollRad), sr = sinf(rollRad);
        flipRx[0][0] = cr;  flipRx[0][1] = sr;
        flipRx[1][0] = -sr; flipRx[1][1] = cr;
    }

    float V[4][4], P[4][4], AV[4][4];
    mat_view_from_pose(V, pose);
    // Startup yaw recenter: rotate the world by the inverse of the initial gaze yaw so the diorama is
    // dead ahead regardless of which way the user was physically facing at launch. World-side rotation
    // (Yaw * V), orthogonal to the eye-side diorama pitch (V * flipRx), so they don't interact.
    float Yaw[4][4]; bool haveYaw = sYawRecenterSet;
    if (haveYaw) {
        mat_yaw_from_quat(Yaw, sHeadRestYawQy, sHeadRestYawQw);
        float Vy[4][4]; mat_mul(Vy, Yaw, V); memcpy(V, Vy, sizeof(V));
    }
    mat_proj_fov(P, fov, zn, zf);
    if (doFlipRoll) {
        float Vr[4][4]; mat_mul(Vr, V, flipRx); // V' = V * Rx  (pitch the eye view)
        mat_mul(AV, A, Vr);
    } else {
        mat_mul(AV, A, V);
    }
    mat_mul(sEyeVP[eye], AV, P);

    // Eye-sphere dome view-proj: rotation-only (zero translation -> sky at infinity, no
    // parallax) with a far projection so the radius-1000 sphere fits. World-locked because
    // it tracks the head's ROTATION only (the eye/projection layer is world-locked).
    XrPosef skyPose = pose;
    skyPose.position.x = skyPose.position.y = skyPose.position.z = 0.0f;
    float Vsky[4][4], Psky[4][4];
    mat_view_from_pose(Vsky, skyPose);
    if (haveYaw) { float Vy[4][4]; mat_mul(Vy, Yaw, Vsky); memcpy(Vsky, Vy, sizeof(Vsky)); } // recenter sky with the eye
    mat_proj_fov(Psky, fov, 1.0f, 5000.0f);
    if (doFlipRoll) {
        float Vskyr[4][4]; mat_mul(Vskyr, Vsky, flipRx); // pitch the sky dome with the eye view
        mat_mul(sSkyVP[eye], Vskyr, Psky);
    } else {
        mat_mul(sSkyVP[eye], Vsky, Psky);
    }

    { static int d = 0; if (d < 6) { d++;
        printf("[VRskyVP] eye=%d sky.diag=%.4f,%.4f,%.4f sky.m23=%.4f | eye.diag=%.4f,%.4f,%.4f | orient=%.3f,%.3f,%.3f,%.3f\n",
            eye, sSkyVP[eye][0][0], sSkyVP[eye][1][1], sSkyVP[eye][2][2], sSkyVP[eye][2][3],
            sEyeVP[eye][0][0], sEyeVP[eye][1][1], sEyeVP[eye][2][2],
            skyPose.orientation.x, skyPose.orientation.y, skyPose.orientation.z, skyPose.orientation.w); } }
}

// ---- helpers ----------------------------------------------------------------
static bool xrok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
    if (sInstance != XR_NULL_HANDLE) xrResultToString(sInstance, r, buf);
    else snprintf(buf, sizeof buf, "%d", (int)r);
    printf("[VR] %s failed: %s\n", what, buf);
    return false;
}

// Head orientation offsets from looking straight forward (-Z). Used to world-lock
// the 2D skybox: the game draws the panorama window for (camera + head) direction,
// so turning your head reveals the matching part of the real sky instead of swimming.
float vr_head_yaw_rad(void) {
    if (!sRunning || !sViewsValid) { return 0.0f; }
    XrQuaternionf q = sViews[0].pose.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y);          // forward.x of R*(0,0,-1)
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y)); // forward.z
    return atan2f(fx, -fz); // 0 when facing -Z, + when turning right
}

float vr_head_pitch_rad(void) {
    if (!sRunning || !sViewsValid) { return 0.0f; }
    XrQuaternionf q = sViews[0].pose.orientation;
    float fy = -2.0f * (q.y * q.z - q.w * q.x); // forward.y
    if (fy >  1.0f) { fy =  1.0f; }
    if (fy < -1.0f) { fy = -1.0f; }
    return asinf(fy); // + when looking up
}

static int64_t vr_choose_swapchain_format(void) {
    uint32_t n = 0;
    if (!XR_SUCCEEDED(xrEnumerateSwapchainFormats(sSession, 0, &n, NULL)) || n == 0)
        return GL_SRGB8_ALPHA8;
    int64_t *fmts = (int64_t *)calloc(n, sizeof(int64_t));
    xrEnumerateSwapchainFormats(sSession, n, &n, fmts);
    const int64_t prefs[] = { GL_SRGB8_ALPHA8, GL_RGBA8 }; // sRGB first - game writes final gamma values; linear made the compositor re-encode (too bright)
    int64_t chosen = fmts[0];
    bool found = false;
    for (uint32_t p = 0; p < 2 && !found; p++)
        for (uint32_t i = 0; i < n; i++)
            if (fmts[i] == prefs[p]) { chosen = prefs[p]; found = true; break; }
    free(fmts);
    printf("[VR] swapchain format: 0x%llx (%s)\n", (unsigned long long)chosen,
           chosen == GL_RGBA8 ? "RGBA8 linear" : chosen == GL_SRGB8_ALPHA8 ? "SRGB8 (may dim)" : "other");
    return chosen;
}

// ---- Motion-controller input (OpenXR actions) --------------------------------
// One action set covering gameplay: both thumbsticks, the face buttons, the menu button, stick
// clicks, triggers, grips, and a haptic output per hand. Suggested bindings cover the Quest Touch
// profile (Quest 2/3/Pro over Link / Air Link / Virtual Desktop), the Index profile, and the khr
// simple profile as a last resort. controller_vr.c turns this state into the game's pad.
static XrActionSet sActionSet  = XR_NULL_HANDLE;
static XrAction sActMove       = XR_NULL_HANDLE; // left thumbstick (vector2)
static XrAction sActCam        = XR_NULL_HANDLE; // right thumbstick (vector2)
static XrAction sActBtnA       = XR_NULL_HANDLE;
static XrAction sActBtnB       = XR_NULL_HANDLE;
static XrAction sActBtnX       = XR_NULL_HANDLE;
static XrAction sActBtnY       = XR_NULL_HANDLE;
static XrAction sActMenu       = XR_NULL_HANDLE;
static XrAction sActLStick     = XR_NULL_HANDLE; // thumbstick clicks
static XrAction sActRStick     = XR_NULL_HANDLE;
static XrAction sActLTrigger   = XR_NULL_HANDLE; // analog 0..1, turned digital here with hysteresis
static XrAction sActRTrigger   = XR_NULL_HANDLE;
static XrAction sActLGrip      = XR_NULL_HANDLE;
static XrAction sActRGrip      = XR_NULL_HANDLE;
static XrAction sActHaptic     = XR_NULL_HANDLE; // vibration output, per-hand subactions
static XrPath   sHandPath[2]   = { XR_NULL_PATH, XR_NULL_PATH }; // /user/hand/left, /user/hand/right
static bool     sInputAttached = false;          // action set attached to the session
static unsigned sCtrlButtons   = 0;              // VR_BTN_* mask, refreshed each xrSyncActions
static float    sCtrlStick[2][2] = {{ 0 }};      // [hand][x,y], +x right +y up

static XrAction vr_make_action(XrActionType type, const char *name, const char *localized, bool perHand) {
    XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
    aci.actionType = type;
    strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (perHand) { aci.countSubactionPaths = 2; aci.subactionPaths = sHandPath; }
    XrAction a = XR_NULL_HANDLE;
    if (!xrok(xrCreateAction(sActionSet, &aci, &a), name)) { return XR_NULL_HANDLE; }
    return a;
}

typedef struct { XrAction action; const char *path; } VrBind;
static void vr_suggest_profile(const char *profilePath, const VrBind *binds, int count) {
    XrPath profile = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(sInstance, profilePath, &profile))) { return; }
    XrActionSuggestedBinding sb[32];
    uint32_t n = 0;
    for (int i = 0; i < count && n < 32; i++) {
        if (binds[i].action == XR_NULL_HANDLE) { continue; }
        XrPath p = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(sInstance, binds[i].path, &p))) { continue; }
        sb[n].action  = binds[i].action;
        sb[n].binding = p;
        n++;
    }
    if (n == 0) { return; }
    XrInteractionProfileSuggestedBinding spb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    spb.interactionProfile = profile;
    spb.suggestedBindings = sb;
    spb.countSuggestedBindings = n;
    // Not fatal if the runtime rejects a profile it doesn't know; the others still apply.
    xrok(xrSuggestInteractionProfileBindings(sInstance, &spb), profilePath);
}

// Create the action set, suggest the per-device bindings and attach to the session. Runs once in
// vr_boot; any failure leaves VR fully functional, just without motion controllers (gamepad still works).
static void vr_input_create(void) {
    sInputAttached = false;
    sCtrlButtons = 0;
    memset(sCtrlStick, 0, sizeof(sCtrlStick));
    xrStringToPath(sInstance, "/user/hand/left",  &sHandPath[0]);
    xrStringToPath(sInstance, "/user/hand/right", &sHandPath[1]);

    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(asci.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(asci.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!xrok(xrCreateActionSet(sInstance, &asci, &sActionSet), "xrCreateActionSet")) {
        sActionSet = XR_NULL_HANDLE;
        return;
    }

    sActMove     = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "move",          "Move",                false);
    sActCam      = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "camera",        "Camera",              false);
    sActBtnA     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_a",      "A Button",            false);
    sActBtnB     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_b",      "B Button",            false);
    sActBtnX     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_x",      "X Button",            false);
    sActBtnY     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_y",      "Y Button",            false);
    sActMenu     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "menu",          "Pause",               false);
    sActLStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "left_stick",    "Left Stick Click",    false);
    sActRStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "right_stick",   "Right Stick Click",   false);
    sActLTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_trigger",  "Left Trigger",        false);
    sActRTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_trigger", "Right Trigger",       false);
    sActLGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_grip",     "Left Grip",           false);
    sActRGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_grip",    "Right Grip",          false);
    sActHaptic   = vr_make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT,"rumble",        "Rumble",              true);

    // Quest Touch (the right controller's Oculus button is reserved by the system, so it isn't bound).
    const VrBind touch[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/a/click" },
        { sActBtnB,     "/user/hand/right/input/b/click" },
        { sActBtnX,     "/user/hand/left/input/x/click" },
        { sActBtnY,     "/user/hand/left/input/y/click" },
        { sActMenu,     "/user/hand/left/input/menu/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/value" },
        { sActRGrip,    "/user/hand/right/input/squeeze/value" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));

    // Valve Index: same layout, but A/B exist on both hands and there's no menu button.
    const VrBind index[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/a/click" },
        { sActBtnB,     "/user/hand/right/input/b/click" },
        { sActBtnX,     "/user/hand/left/input/a/click" },
        { sActBtnY,     "/user/hand/left/input/b/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/value" },
        { sActRGrip,    "/user/hand/right/input/squeeze/value" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index) / sizeof(index[0])));

    // Bare-minimum fallback profile every runtime understands (select + menu only).
    const VrBind simple[] = {
        { sActBtnA,   "/user/hand/right/input/select/click" },
        { sActBtnB,   "/user/hand/left/input/select/click" },
        { sActMenu,   "/user/hand/left/input/menu/click" },
        { sActHaptic, "/user/hand/left/output/haptic" },
        { sActHaptic, "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple) / sizeof(simple[0])));

    XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    sai.countActionSets = 1;
    sai.actionSets = &sActionSet;
    if (!xrok(xrAttachSessionActionSets(sSession, &sai), "xrAttachSessionActionSets")) { return; }
    sInputAttached = true;
    printf("[VR] motion controllers ready (Quest Touch / Index profiles suggested).\n");
}

static bool vr_action_bool(XrAction a) {
    if (a == XR_NULL_HANDLE) { return false; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(sSession, &gi, &st)) && st.isActive && st.currentState;
}

static float vr_action_float(XrAction a) {
    if (a == XR_NULL_HANDLE) { return 0.0f; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(sSession, &gi, &st)) || !st.isActive) { return 0.0f; }
    return st.currentState;
}

static void vr_action_vec2(XrAction a, float out[2]) {
    out[0] = out[1] = 0.0f;
    if (a == XR_NULL_HANDLE) { return; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(sSession, &gi, &st)) || !st.isActive) { return; }
    out[0] = st.currentState.x;
    out[1] = st.currentState.y;
}

// Analog trigger/grip to a digital button with hysteresis: press past 60%, release under 40%,
// so a finger resting lightly on the trigger can't flicker the bound action.
static bool vr_analog_latch(float v, bool held) {
    return held ? (v > 0.4f) : (v > 0.6f);
}

// Pull fresh controller state from the runtime. Once per frame, from vr_begin_frame.
static void vr_input_sync(void) {
    if (!sInputAttached) { return; }
    XrActiveActionSet active = { sActionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a success code that means "no input for you" (headset off, runtime
    // menu up). Release everything so a button held at that moment can't stay stuck down.
    if (xrSyncActions(sSession, &si) != XR_SUCCESS) {
        sCtrlButtons = 0;
        memset(sCtrlStick, 0, sizeof(sCtrlStick));
        return;
    }
    unsigned b = 0;
    if (vr_action_bool(sActBtnA))   { b |= VR_BTN_A; }
    if (vr_action_bool(sActBtnB))   { b |= VR_BTN_B; }
    if (vr_action_bool(sActBtnX))   { b |= VR_BTN_X; }
    if (vr_action_bool(sActBtnY))   { b |= VR_BTN_Y; }
    if (vr_action_bool(sActMenu))   { b |= VR_BTN_MENU; }
    if (vr_action_bool(sActLStick)) { b |= VR_BTN_LSTICK; }
    if (vr_action_bool(sActRStick)) { b |= VR_BTN_RSTICK; }
    if (vr_analog_latch(vr_action_float(sActLTrigger), sCtrlButtons & VR_BTN_LTRIGGER)) { b |= VR_BTN_LTRIGGER; }
    if (vr_analog_latch(vr_action_float(sActRTrigger), sCtrlButtons & VR_BTN_RTRIGGER)) { b |= VR_BTN_RTRIGGER; }
    if (vr_analog_latch(vr_action_float(sActLGrip),    sCtrlButtons & VR_BTN_LGRIP))    { b |= VR_BTN_LGRIP; }
    if (vr_analog_latch(vr_action_float(sActRGrip),    sCtrlButtons & VR_BTN_RGRIP))    { b |= VR_BTN_RGRIP; }
    sCtrlButtons = b;
    vr_action_vec2(sActMove, sCtrlStick[0]);
    vr_action_vec2(sActCam,  sCtrlStick[1]);
}

bool vr_controllers_active(void) {
    return sInputAttached && sRunning && sState == XR_SESSION_STATE_FOCUSED;
}

unsigned vr_controller_buttons(void) {
    return vr_controllers_active() ? sCtrlButtons : 0;
}

void vr_controller_stick(int hand, float out[2]) {
    if (!vr_controllers_active() || hand < 0 || hand > 1) { out[0] = out[1] = 0.0f; return; }
    out[0] = sCtrlStick[hand][0];
    out[1] = sCtrlStick[hand][1];
}

void vr_controller_rumble(float strength, float seconds) {
    if (!vr_controllers_active() || sActHaptic == XR_NULL_HANDLE) { return; }
    if (strength < 0.0f) { strength = 0.0f; }
    if (strength > 1.0f) { strength = 1.0f; }
    XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
    vib.duration  = (XrDuration)(seconds * 1e9); // nanoseconds
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    vib.amplitude = strength;
    for (int h = 0; h < 2; h++) {
        XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
        hai.action = sActHaptic;
        hai.subactionPath = sHandPath[h];
        xrApplyHapticFeedback(sSession, &hai, (const XrHapticBaseHeader *)&vib);
    }
}

void vr_controller_rumble_stop(void) {
    if (!sInputAttached || sSession == XR_NULL_HANDLE || sActHaptic == XR_NULL_HANDLE) { return; }
    for (int h = 0; h < 2; h++) {
        XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
        hai.action = sActHaptic;
        hai.subactionPath = sHandPath[h];
        xrStopHapticFeedback(sSession, &hai);
    }
}

// Lightweight startup probe: is a VR headset actually connected right now? Creates a
// throwaway OpenXR instance (no graphics binding, so no GL context required) and asks for
// an HMD system. Enabling XR_KHR_opengl means a runtime that can't do GL fails here and we
// stay flat (correct - our VR path is GL-only). Everything is torn down before vr_boot()
// creates the real session. Returns true only if a usable HMD is available.
bool vr_headset_present(void) {
    const char *exts[1] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    strncpy(ici.applicationInfo.applicationName, "sm64coopdx", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0; // VirtualDesktopXR etc. are OpenXR 1.0

    XrInstance inst = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateInstance(&ici, &inst)) || inst == XR_NULL_HANDLE) {
        return false; // no OpenXR runtime installed, or not GL-capable -> stay flat
    }
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrResult r = xrGetSystem(inst, &sgi, &sys); // XR_ERROR_FORM_FACTOR_UNAVAILABLE if no HMD
    xrDestroyInstance(inst);
    return XR_SUCCEEDED(r) && sys != XR_NULL_SYSTEM_ID;
}

// Full OpenXR boot. Runs lazily on the first vr_begin_frame() so the WGL context
// is guaranteed current. Any failure leaves VR inactive (game keeps rendering flat).
static void vr_boot(void) {
    if (!sBootRetrying) { printf("[VR] booting OpenXR...\n"); }
    if (!sSettingsLoaded) { sSettingsLoaded = true; vr_settings_load(); } // only the FIRST attempt loads the file; a retry must not clobber live menu edits

    // Opt into the cylinder layer (surround sky) if the runtime offers it.
    sHasCylinder = false;
    sHasEquirect2 = false;
    {
        uint32_t ec = 0;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &ec, NULL)) && ec > 0) {
            XrExtensionProperties *ep = (XrExtensionProperties *)calloc(ec, sizeof(XrExtensionProperties));
            for (uint32_t i = 0; i < ec; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
            xrEnumerateInstanceExtensionProperties(NULL, ec, &ec, ep);
            for (uint32_t i = 0; i < ec; i++) {
                if (strcmp(ep[i].extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)  == 0) sHasCylinder = true;
                if (strcmp(ep[i].extensionName, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME) == 0) sHasEquirect2 = true;
            }
            free(ep);
        }
    }
    const char *exts[3];
    uint32_t nexts = 0;
    exts[nexts++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    if (sHasCylinder)  exts[nexts++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
    if (sHasEquirect2) exts[nexts++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = nexts;
    ici.enabledExtensionNames = exts;
    strncpy(ici.applicationInfo.applicationName, "sm64coopdx", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    strncpy(ici.applicationInfo.engineName, "fast3d", XR_MAX_ENGINE_NAME_SIZE - 1);
    // Request OpenXR 1.0 (universal baseline); XR_CURRENT_API_VERSION is 1.1 and
    // 1.0-only runtimes reject it with XR_ERROR_API_VERSION_UNSUPPORTED.
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (!xrok(xrCreateInstance(&ici, &sInstance), "xrCreateInstance")) { vr_shutdown(); return; }

    XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(sInstance, &props)) && !sBootRetrying)
        printf("[VR] runtime: %s\n", props.runtimeName);
    if (!sBootRetrying) {
        printf("[VR] surround sky (cylinder layer): %s\n", sHasCylinder ? "yes" : "no - flat quad fallback");
        printf("[VR] full-sphere sky (equirect2 layer): %s\n", sHasEquirect2 ? "yes - no black poles" : "no - cylinder fallback");
    }

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    {
        XrResult r = xrGetSystem(sInstance, &sgi, &sSystemId);
        if (XR_FAILED(r)) {
            vr_shutdown();
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
                // The runtime is installed but the headset isn't ready yet (e.g. Virtual Desktop still
                // connecting, headset asleep). The spec marks this transient: keep retrying quietly in the
                // background and VR comes up the moment the headset does. The game stays flat meanwhile.
                if (!sBootRetrying) {
                    printf("[VR] headset not connected yet - retrying in the background. The game runs flat\n");
                    printf("[VR] until the headset is ready (connect Virtual Desktop / wake the headset).\n");
                }
                sBootRetrying = true;
                sBootTried = false;     // allow another attempt
                sBootRetryIn = 180;     // ~2-6 s of frames between attempts (cheap: instance create + probe)
            } else {
                xrok(r, "xrGetSystem"); // hard failure: print it and stay flat for this session
            }
            return;
        }
    }
    if (sBootRetrying) { printf("[VR] headset connected - booting OpenXR now.\n"); sBootRetrying = false; }

    if (!xrok(xrGetInstanceProcAddr(sInstance, "xrGetOpenGLGraphicsRequirementsKHR",
            (PFN_xrVoidFunction *)&pfnGetGLReq), "get xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }
    XrGraphicsRequirementsOpenGLKHR glReq = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!xrok(pfnGetGLReq(sInstance, sSystemId, &glReq), "xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }

    HDC   hdc  = wglGetCurrentDC();
    HGLRC glrc = wglGetCurrentContext();
    if (!hdc || !glrc) { printf("[VR] no current WGL context - cannot bind session\n"); vr_shutdown(); return; }

    XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    gb.hDC = hdc;
    gb.hGLRC = glrc;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = sSystemId;
    if (!xrok(xrCreateSession(sInstance, &sci, &sSession), "xrCreateSession")) { vr_shutdown(); return; }

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (!xrok(xrCreateReferenceSpace(sSession, &rsci, &sLocalSpace), "xrCreateReferenceSpace")) { vr_shutdown(); return; }

    XrViewConfigurationType vct = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    uint32_t n = 0;
    if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, 0, &n, NULL), "enum view count")) { vr_shutdown(); return; }
    if (n > 2) n = 2;
    for (uint32_t i = 0; i < n; i++) sViewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, n, &n, sViewConfigs), "enum views")) { vr_shutdown(); return; }
    sViewCount = n;
    printf("[VR] %u eyes, %ux%u per eye recommended\n", n,
        sViewConfigs[0].recommendedImageRectWidth, sViewConfigs[0].recommendedImageRectHeight);

    int64_t fmt = vr_choose_swapchain_format();
    for (uint32_t e = 0; e < sViewCount; e++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = fmt;
        scci.sampleCount = 1;
        scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
        // The runtime's recommended per-eye size can be very large (e.g. 3072x3264). With lots of
        // GPU memory already consumed (big mod lists), xrCreateSwapchain can return RUNTIME_FAILURE.
        // Try the full recommended size, then fall back to progressively smaller sizes so VR still
        // boots (slightly lower eye resolution) instead of failing to flatscreen.
        static const float scEyeScales[] = { 1.0f, 0.85f, 0.7f, 0.55f, 0.4f };
        XrResult scRes = XR_ERROR_RUNTIME_FAILURE;
        for (int s = 0; s < (int)(sizeof(scEyeScales) / sizeof(scEyeScales[0])); s++) {
            scci.width  = (uint32_t)(sViewConfigs[e].recommendedImageRectWidth  * scEyeScales[s]);
            scci.height = (uint32_t)(sViewConfigs[e].recommendedImageRectHeight * scEyeScales[s]);
            scRes = xrCreateSwapchain(sSession, &scci, &sEye[e].handle);
            if (XR_SUCCEEDED(scRes)) {
                if (scEyeScales[s] < 1.0f) {
                    printf("[VR] eye %u swapchain fell back to %ux%u (%.0f%% of recommended) - GPU memory pressure\n",
                           e, scci.width, scci.height, scEyeScales[s] * 100.0f);
                }
                break;
            }
            printf("[VR] eye %u swapchain %ux%u rejected (XrResult %d); retrying smaller\n",
                   e, scci.width, scci.height, (int)scRes);
        }
        if (!XR_SUCCEEDED(scRes)) {
            printf("[VR] xrCreateSwapchain failed at every size - VR disabled. Try: close other VR apps, reduce loaded mods, or restart Virtual Desktop.\n");
            vr_shutdown(); return;
        }
        sEye[e].w = scci.width;
        sEye[e].h = scci.height;

        uint32_t imgN = 0;
        xrEnumerateSwapchainImages(sEye[e].handle, 0, &imgN, NULL);
        sEye[e].images = (XrSwapchainImageOpenGLKHR *)calloc(imgN, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < imgN; i++) sEye[e].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sEye[e].handle, imgN, &imgN, (XrSwapchainImageBaseHeader *)sEye[e].images);
        sEye[e].imgCount = imgN;
    }

    // Render FBO + a shared depth renderbuffer (both eyes are the same size and
    // are rendered sequentially, so one depth buffer is enough).
    glGenFramebuffers(1, &sEyeFbo);
    glGenRenderbuffers(1, &sEyeDepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, sEyeDepthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sEye[0].w, (GLsizei)sEye[0].h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    // VIEW reference space (head-locked) for the 2D overlay quad.
    XrReferenceSpaceCreateInfo vrci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vrci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vrci.poseInReferenceSpace.orientation.w = 1.0f;
    xrok(xrCreateReferenceSpace(sSession, &vrci, &sViewSpace), "xrCreateReferenceSpace(VIEW)");

    // 2D overlay swapchain (mono) + its FBO and depth.
    XrSwapchainCreateInfo oci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    oci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    oci.format = fmt;
    oci.sampleCount = 1;
    oci.width  = sOverlay.w = (uint32_t)sOverlayW;
    oci.height = sOverlay.h = (uint32_t)sOverlayH;
    oci.faceCount = 1; oci.arraySize = 1; oci.mipCount = 1;
    if (xrok(xrCreateSwapchain(sSession, &oci, &sOverlay.handle), "xrCreateSwapchain(overlay)")) {
        uint32_t on = 0;
        xrEnumerateSwapchainImages(sOverlay.handle, 0, &on, NULL);
        sOverlay.images = (XrSwapchainImageOpenGLKHR *)calloc(on, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < on; i++) sOverlay.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sOverlay.handle, on, &on, (XrSwapchainImageBaseHeader *)sOverlay.images);
        sOverlay.imgCount = on;
        glGenFramebuffers(1, &sOverlayFbo);
        glGenRenderbuffers(1, &sOverlayDepthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, sOverlayDepthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sOverlayW, (GLsizei)sOverlayH);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    // Second swapchain for the HUD/menus (head-locked layer); shares the FBO + depth.
    oci.width  = sHud.w = (uint32_t)sOverlayW;
    oci.height = sHud.h = (uint32_t)sOverlayH;
    if (xrok(xrCreateSwapchain(sSession, &oci, &sHud.handle), "xrCreateSwapchain(hud)")) {
        uint32_t hn = 0;
        xrEnumerateSwapchainImages(sHud.handle, 0, &hn, NULL);
        sHud.images = (XrSwapchainImageOpenGLKHR *)calloc(hn, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < hn; i++) sHud.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sHud.handle, hn, &hn, (XrSwapchainImageBaseHeader *)sHud.images);
        sHud.imgCount = hn;
    }

    vr_input_create(); // motion controllers (failure leaves VR up, just gamepad-only)

    printf("[VR] OpenXR ready; waiting for session to start.\n");
    printf("[VRBUILD] v0.4: per-preset-memory + diorama-1376 + paginated-vr-menu + theater-hidden (if you don't see this line, you are running an OLD exe)\n");
}

static void vr_poll_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(sInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *e = (const XrEventDataSessionStateChanged *)&ev;
            sState = e->state;
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (xrok(xrBeginSession(sSession, &sbi), "xrBeginSession")) {
                    sRunning = true;
                    printf("[VR] session running.\n");
                }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(sSession);
                sRunning = false;
                printf("[VR] session stopped.\n");
            } else if (e->state == XR_SESSION_STATE_VISIBLE ||
                       e->state == XR_SESSION_STATE_FOCUSED) {
                // Headset re-donned / session regained the foreground: the rest pose captured before
                // removal is stale and would drop the damped eye to the wrong height (through the floor).
                // Clear it so the next warmup re-captures a fresh rest at the current tracked pose.
                if (sHeadRestSet) {
                    printf("[VR] session state=%d: re-centering 6DoF rest.\n", (int)e->state);
                    sHeadRestSet = false;
                    sHeadWarmup  = 0;
                }
                // Re-don: also re-center the startup yaw and re-lock the menu eye height so the view and
                // the menu re-center fresh on the current gaze after the headset comes back.
                sYawRecenterSet   = false;
                sPanelEyeYValid   = false;
                sPanelAnchorValid = false;
            }
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER; // reset for next poll
    }
}

// Write the current diorama values to vr_tune.txt (next to the exe) so they can
// be read back directly - no copy/paste of console lines.
static void vr_write_tune_file(void) {
    FILE *f = fopen("vr_tune.txt", "w");
    if (!f) return;
    fprintf(f, "scale=%.1f dist=%.3f height=%.3f stereo=%.3f head=%.3f\n", sDioramaScale, sDioramaDist, sDioramaHeight, sStereoScale, sHeadScale);
    fclose(f);
}

// --- VR presets (selectable looks) -------------------------------------------
// First-person is now a preset: it enables coopdx's first-person camera (camera at Mario's head)
// and renders life-size at 1:1 instead of the shrunk diorama. F10 / d-pad up cycles
// Diorama / Third-person / First-person / Theater.
typedef struct { const char *name; float scale, dist, height, stereo, pitch; bool firstPerson; } VrPreset;
static const VrPreset sPresets[] = {
    // Diorama: small (scale 1376 ~= 5.8m world), low + close, strong stereo for miniature parallax. The
    // nose-down tilt is kept small (~4.5 deg): it's applied in eye space, so a larger tilt leaks into a
    // roll when you tilt your head (the "lopsided" look). The low + close framing carries the look-down.
    { "Diorama",      1376.0f,  0.25f, -0.35f, 0.45f, 0.08f, false },
    { "Third-person",     1200.0f, -0.17f,  0.00f, 0.21f, 0.00f, false },
    { "First-person",  100.0f,  0.00f,  0.00f, 0.50f, 0.00f, true  }, // life-size, eye at Mario's head, full 6DoF
    { "Theater",       1200.0f, -0.17f,  0.00f, 0.21f, 0.00f, false }, // flat game on a big world-locked screen (panel path)
};
#define VR_NUM_PRESETS ((int)(sizeof(sPresets) / sizeof(sPresets[0])))
static int sCurrentPreset = 1; // launch default = Third-person

// Per-preset user tunables: slider tweaks made while a preset is active are remembered FOR THAT PRESET
// (switching Diorama -> First-person -> Diorama brings your Diorama tweaks back), and every preset's set
// is saved to vr_settings.txt so it survives a relaunch. Seeded from the stock table; the menu setters
// write the live value back into the active preset's slot.
typedef struct { float scale, dist, height, stereo, head; } VrPresetTunables;
static VrPresetTunables sPresetUser[VR_NUM_PRESETS];
static bool sPresetUserSeeded = false;
static void vr_preset_user_seed(void) {
    if (sPresetUserSeeded) { return; }
    sPresetUserSeeded = true;
    for (int i = 0; i < VR_NUM_PRESETS; i++) {
        sPresetUser[i].scale  = sPresets[i].scale;
        sPresetUser[i].dist   = sPresets[i].dist;
        sPresetUser[i].height = sPresets[i].height;
        sPresetUser[i].stereo = sPresets[i].stereo;
        sPresetUser[i].head   = sPresets[i].firstPerson ? 1.0f : 0.4f; // life-size wants true 1:1 head motion; diorama wants damping
    }
}

static void vr_apply_preset(int idx) {
    if (idx < 0 || idx >= VR_NUM_PRESETS) return;
    vr_preset_user_seed();
    sCurrentPreset = idx;
    // Restore the tunables you last used in this preset (stock values until you've tweaked it). The
    // setters write through to sPresetUser, so the preset you're leaving keeps what you dialed in.
    sDioramaScale  = sPresetUser[idx].scale;
    sDioramaDist   = sPresetUser[idx].dist;
    sDioramaHeight = sPresetUser[idx].height;
    sStereoScale   = sPresetUser[idx].stereo;
    sHeadScale     = sPresetUser[idx].head;
    sDioramaPitchRad = sPresets[idx].pitch;
    sFirstPerson   = sPresets[idx].firstPerson;
    sHeadRestSet   = false; sHeadWarmup = 0;
    printf("[VR] preset %d/%d: %s (scale=%.0f dist=%.2f height=%.2f stereo=%.2f fp=%d)\n",
        idx + 1, VR_NUM_PRESETS, sPresets[idx].name,
        sDioramaScale, sDioramaDist, sDioramaHeight, sStereoScale, (int)sFirstPerson);
    vr_write_tune_file();
    vr_settings_mark_dirty();
}

// --- In-game VR menu accessors (used by djui_panel_vr.c) ---------------------
int   vr_get_preset_index(void)      { return sCurrentPreset; }
void  vr_set_preset_index(int idx)   { vr_apply_preset(idx); } // 0=Diorama, 1=Third-person, 2=First-person, 3=Theater
void  vr_cycle_preset(void)          { // F10 / d-pad up: Diorama -> Third-person -> First-person (-> Theater when enabled)
    int idx = (sCurrentPreset + 1) % VR_NUM_PRESETS;
#if !VR_THEATER_ENABLED
    if (idx == 3) { idx = (idx + 1) % VR_NUM_PRESETS; } // Theater is hidden until it's finished
#endif
    vr_apply_preset(idx);
}
int   vr_get_preset_count(void)      { return VR_NUM_PRESETS; }
const char* vr_get_preset_name(int i){ return (i >= 0 && i < VR_NUM_PRESETS) ? sPresets[i].name : ""; }
float vr_get_menu_dist(void)         { return sMenuDist; }
void  vr_set_menu_dist(float v)      { sMenuDist = v; vr_settings_mark_dirty(); }
float vr_get_menu_size(void)         { return sMenuSize; }
void  vr_set_menu_size(float v)      { sMenuSize = v; vr_settings_mark_dirty(); }
float vr_get_theater_dist(void)      { return sTheaterDist; }
void  vr_set_theater_dist(float v)   { sTheaterDist = (v < 1.0f) ? 1.0f : (v > 10.0f ? 10.0f : v); vr_settings_mark_dirty(); }
float vr_get_theater_size(void)      { return sTheaterSize; }
void  vr_set_theater_size(float v)   { sTheaterSize = (v < 3.0f) ? 3.0f : (v > 20.0f ? 20.0f : v); vr_settings_mark_dirty(); }
int   vr_get_theater_bg(void)        { return sTheaterBg; }
void  vr_set_theater_bg(int mode)    { sTheaterBg = (mode < 0 || mode > 1) ? 0 : mode; sBgLoadTried = false; vr_settings_mark_dirty(); } // 3D Model (2) is hidden until implemented -> Black
// The five preset-shaped tunables write through to the active preset's slot, so each view mode keeps
// what you dialed in across mode switches AND relaunches (every slot is saved to vr_settings.txt).
float vr_get_diorama_dist(void)      { return sDioramaDist; }
void  vr_set_diorama_dist(float v)   { sDioramaDist = v; vr_preset_user_seed(); sPresetUser[sCurrentPreset].dist = sDioramaDist; vr_settings_mark_dirty(); }
float vr_get_diorama_scale(void)     { return sDioramaScale; }
void  vr_set_diorama_scale(float v)  { sDioramaScale = (v < 30.0f) ? 30.0f : v; vr_preset_user_seed(); sPresetUser[sCurrentPreset].scale = sDioramaScale; vr_settings_mark_dirty(); }
float vr_get_stereo(void)            { return sStereoScale; }
void  vr_set_stereo(float v)         { sStereoScale = (v < 0.0f) ? 0.0f : v; vr_preset_user_seed(); sPresetUser[sCurrentPreset].stereo = sStereoScale; vr_settings_mark_dirty(); }
float vr_get_diorama_height(void)    { return sDioramaHeight; }
void  vr_set_diorama_height(float v) { sDioramaHeight = v; vr_preset_user_seed(); sPresetUser[sCurrentPreset].height = sDioramaHeight; vr_settings_mark_dirty(); }
float vr_get_head_scale(void)        { return sHeadScale; }
void  vr_set_head_scale(float v)     { sHeadScale = (v < 0.0f) ? 0.0f : (v > 1.5f ? 1.5f : v); vr_preset_user_seed(); sPresetUser[sCurrentPreset].head = sHeadScale; vr_settings_mark_dirty(); }
float vr_get_hud_size(void)          { return sHudSize; }
void  vr_set_hud_size(float v)       { sHudSize = (v < 0.5f) ? 0.5f : v; vr_settings_mark_dirty(); } // VR HUD panel width (m)
float vr_get_world_scale(void)       { return sWorldScale; }
void  vr_set_world_scale(float v)    { sWorldScale = (v < 0.25f) ? 0.25f : (v > 4.0f ? 4.0f : v); vr_settings_mark_dirty(); } // first-person world scale
// Tabletop is preset 0 - the model-on-a-table view that uses the free orbit camera (see camera.c).
bool  vr_is_tabletop_mode(void)      { return sRunning && sCurrentPreset == 0; }
bool  vr_is_theater_mode(void)       { return sRunning && sCurrentPreset == 3; } // flat game on a big screen (panel path)

// The headset's real refresh rate in Hz, taken from the runtime's predicted display period (filled by
// xrWaitFrame each frame). pc_main uses this to run the interpolation loop at the headset's cadence
// instead of the desktop monitor's, so exactly one rendered frame is emitted per headset frame.
// Returns 0 until the runtime reports a usable period, in which case the caller keeps its own default.
int vr_get_refresh_rate(void) {
    XrDuration period = sFrameState.predictedDisplayPeriod; // nanoseconds between displayed frames
    if (period > 0) {
        double hz = 1.0e9 / (double)period;
        if (hz >= 30.0 && hz <= 1000.0) { return (int)(hz + 0.5); }
    }
    return 0;
}

void  vr_set_first_person(bool on) {
    if (on) {
        // Apply the First-person preset by its flag, NOT VR_NUM_PRESETS-1: adding the Theater preset made
        // the last index Theater (the flat screen), so the old code dropped you onto the screen instead.
        for (int i = 0; i < VR_NUM_PRESETS; i++) {
            if (sPresets[i].firstPerson) { vr_apply_preset(i); break; }
        }
    } else if (sFirstPerson) {
        vr_apply_preset(1); // back to Third-person
    }
}
// Reset every VR tunable to its launch default (Third-person preset + default panel placement).
void vr_reset_defaults(void) {
    sPresetUserSeeded = false;  // forget every preset's remembered tweaks -> back to the stock table
    vr_apply_preset(1);     // Third-person: resets scale/dist/height/stereo/first-person/head-damping
    sMenuDist = 3.0f;
    sMenuSize = 4.8f;
    sHudSize  = 2.4f;
    sTheaterDist = 4.0f;
    sTheaterSize = 9.0f;
    sTheaterBg = 0;
    sMenuVOffset = 0.0f;
    sPanelAnchorValid = false;
    sPanelEyeYValid = false;  // re-lock the menu eye height
    sYawRecenterSet = false;  // re-center the view yaw on the next tracked pose
    sAnticlipOffsetM[0] = sAnticlipOffsetM[1] = sAnticlipOffsetM[2] = 0.0f;
    sWorldScale = 1.0f;
    sHeadMoveEnabled = false;
    printf("[VR] reset to defaults.\n");
    vr_settings_mark_dirty();
}

// Mark the VR settings changed; the file write is debounced in vr_begin_frame so dragging a slider
// (which fires this every frame) only writes once activity stops.
void vr_settings_mark_dirty(void) { sSettingsDirty = true; sSettingsFlushIn = 45; }

// Write the current VR menu state to vr_settings.txt next to the exe (simple key=value lines).
static void vr_settings_save(void) {
    vr_preset_user_seed();
    FILE *f = fopen("vr_settings.txt", "w");
    if (!f) { return; }
    fprintf(f,
        "preset=%d\nscale=%.3f\ndist=%.4f\nheight=%.4f\nstereo=%.4f\nhead=%.4f\n"
        "menudist=%.3f\nmenusize=%.3f\nhudsize=%.3f\nworldscale=%.3f\ntheaterdist=%.3f\ntheatersize=%.3f\ntheaterbg=%d\nanticlip=%d\nflipcam=%d\ninteractcam=%d\nhidehud=%d\nheadmove=%d\n",
        sCurrentPreset, sDioramaScale, sDioramaDist, sDioramaHeight, sStereoScale, sHeadScale,
        sMenuDist, sMenuSize, sHudSize, sWorldScale, sTheaterDist, sTheaterSize, sTheaterBg, sAnticlipEnabled ? 1 : 0,
        first_person_get_flip_cam() ? 1 : 0, first_person_get_interact_cam() ? 1 : 0,
        gMenuHideHud ? 1 : 0, sHeadMoveEnabled ? 1 : 0);
    // Per-preset tunables, one set per view mode, so every mode's tweaks survive a relaunch. The legacy
    // single-set keys above mirror the CURRENT preset so an older build reading this file still works.
    for (int i = 0; i < VR_NUM_PRESETS; i++) {
        fprintf(f, "p%d_scale=%.3f\np%d_dist=%.4f\np%d_height=%.4f\np%d_stereo=%.4f\np%d_head=%.4f\n",
            i, sPresetUser[i].scale, i, sPresetUser[i].dist, i, sPresetUser[i].height,
            i, sPresetUser[i].stereo, i, sPresetUser[i].head);
    }
    fclose(f);
}

// Read vr_settings.txt and apply it. Missing fields keep their current/default value. The preset INDEX is
// applied directly (not via vr_apply_preset) so the saved slider tweaks aren't overwritten by the preset's
// stock values - only the preset-derived first-person flag and view tilt are taken from the table.
static void vr_settings_load(void) {
    vr_preset_user_seed();
    FILE *f = fopen("vr_settings.txt", "r");
    if (!f) { return; }
    int preset   = sCurrentPreset;
    int anticlip = sAnticlipEnabled ? 1 : 0;
    int flip     = first_person_get_flip_cam() ? 1 : 0;
    int interact = first_person_get_interact_cam() ? 1 : 0;
    int hud      = gMenuHideHud ? 1 : 0;
    int headmove = sHeadMoveEnabled ? 1 : 0;
    int theaterbg = sTheaterBg;
    bool sawPerPreset = false; // file carries p<N>_* keys (new format: one tunable set per view mode)
    bool sawLegacy    = false; // file carries the old single-set keys (pre-per-preset format)
    char line[128], key[64];
    double val;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%63[^=]=%lf", key, &val) != 2) { continue; }
        // Per-preset tunables: p<index>_<field>.
        int pi = -1; char sub[32] = { 0 };
        if (sscanf(key, "p%d_%31s", &pi, sub) == 2 && pi >= 0 && pi < VR_NUM_PRESETS) {
            sawPerPreset = true;
            if      (!strcmp(sub, "scale"))  { sPresetUser[pi].scale  = (float)val; }
            else if (!strcmp(sub, "dist"))   { sPresetUser[pi].dist   = (float)val; }
            else if (!strcmp(sub, "height")) { sPresetUser[pi].height = (float)val; }
            else if (!strcmp(sub, "stereo")) { sPresetUser[pi].stereo = (float)val; }
            else if (!strcmp(sub, "head"))   { sPresetUser[pi].head   = (float)val; }
            continue;
        }
        if      (!strcmp(key, "preset"))      { preset         = (int)val; }
        else if (!strcmp(key, "scale"))       { sDioramaScale  = (float)val; sawLegacy = true; }
        else if (!strcmp(key, "dist"))        { sDioramaDist   = (float)val; sawLegacy = true; }
        else if (!strcmp(key, "height"))      { sDioramaHeight = (float)val; sawLegacy = true; }
        else if (!strcmp(key, "stereo"))      { sStereoScale   = (float)val; sawLegacy = true; }
        else if (!strcmp(key, "head"))        { sHeadScale     = (float)val; sawLegacy = true; }
        else if (!strcmp(key, "menudist"))    { sMenuDist      = (float)val; }
        else if (!strcmp(key, "menusize"))    { sMenuSize      = (float)val; }
        else if (!strcmp(key, "hudsize"))     { sHudSize       = (float)val; }
        else if (!strcmp(key, "worldscale"))  { sWorldScale    = (float)val; }
        else if (!strcmp(key, "theaterdist")) { sTheaterDist   = (float)val; }
        else if (!strcmp(key, "theatersize")) { sTheaterSize   = (float)val; }
        else if (!strcmp(key, "anticlip"))    { anticlip       = (int)val; }
        else if (!strcmp(key, "flipcam"))     { flip           = (int)val; }
        else if (!strcmp(key, "interactcam")) { interact       = (int)val; }
        else if (!strcmp(key, "hidehud"))     { hud            = (int)val; }
        else if (!strcmp(key, "headmove"))    { headmove       = (int)val; }
        else if (!strcmp(key, "theaterbg"))   { theaterbg      = (int)val; }
    }
    fclose(f);
    if (preset < 0 || preset >= VR_NUM_PRESETS) { preset = 1; }
#if !VR_THEATER_ENABLED
    if (preset == 3) { preset = 1; } // Theater is hidden until it's finished; land in Third-person
#endif
    if (theaterbg < 0 || theaterbg > 1) { theaterbg = 0; } // 3D Model (2) is hidden until implemented -> Black
    // Old single-set file: fold its one tunable set into the saved preset's slot so it sticks.
    if (sawLegacy && !sawPerPreset) {
        sPresetUser[preset].scale  = sDioramaScale;
        sPresetUser[preset].dist   = sDioramaDist;
        sPresetUser[preset].height = sDioramaHeight;
        sPresetUser[preset].stereo = sStereoScale;
        sPresetUser[preset].head   = sHeadScale;
    }
    sCurrentPreset   = preset;
    sDioramaScale    = sPresetUser[preset].scale;
    sDioramaDist     = sPresetUser[preset].dist;
    sDioramaHeight   = sPresetUser[preset].height;
    sStereoScale     = sPresetUser[preset].stereo;
    sHeadScale       = sPresetUser[preset].head;
    sFirstPerson     = sPresets[preset].firstPerson;
    sDioramaPitchRad = sPresets[preset].pitch;
    sAnticlipEnabled = (anticlip != 0);
    sHeadMoveEnabled = (headmove != 0);
    sTheaterBg       = theaterbg; // already clamped above (0=Black, 1=Panoramic)
    first_person_set_flip_cam(flip != 0);
    first_person_set_interact_cam(interact != 0);
    gMenuHideHud = (unsigned char)(hud ? 1 : 0);
    printf("[VR] settings restored: preset=%d scale=%.0f stereo=%.2f head=%.2f anticlip=%d\n",
        sCurrentPreset, sDioramaScale, sStereoScale, sHeadScale, sAnticlipEnabled ? 1 : 0);
}

// --- Geometry anti-clip bridge (pc_main does the world conversion + collision) -----------------
bool  vr_anticlip_is_enabled(void)   { return sAnticlipEnabled; }
void  vr_anticlip_set_enabled(bool e){ sAnticlipEnabled = e; if (!e) { sAnticlipOffsetM[0]=sAnticlipOffsetM[1]=sAnticlipOffsetM[2]=0.0f; } vr_settings_mark_dirty(); }

// --- Head-direction movement (VR first-person, opt-in): walk/turn toward where the head looks ---------
// Head yaw offset from the rest gaze, as an SM64 s16 angle. mario.c adds this to the move reference yaw.
int   vr_get_look_yaw(void)          {
    // Head-look steering, balanced + responsive. Called once per frame from the movement code. Raw HMD yaw is
    // noisy frame-to-frame and a head whip would jerk Mario, so low-pass it; a small deadzone keeps tiny
    // glances from nudging your heading, and subtracting the deadzone past the edge keeps it continuous (no
    // jump). Full magnitude past that, so "move where you look" still points you where you actually look.
    float raw = vr_head_yaw_rad() - sHeadRestYawRad;
    static float sLookSmooth = 0.0f;
    sLookSmooth += (raw - sLookSmooth) * 0.15f;
    float v = sLookSmooth;
    const float deadzone = 0.0524f; // ~3 degrees
    if      (v >  deadzone) { v -= deadzone; }
    else if (v < -deadzone) { v += deadzone; }
    else                    { v  = 0.0f; }
    return (short)(v * (32768.0f / 3.14159265358979f));
}
bool  vr_get_head_move(void)         { return sHeadMoveEnabled; }
void  vr_set_head_move(bool e)       { sHeadMoveEnabled = e; vr_settings_mark_dirty(); }
// Cyclopean eye position in game-camera space (game units). Returns false when there's nothing to
// resolve this frame (no tracked pose, first-person mode, or anti-clip disabled) - caller should then
// let the applied offset ease back to zero.
bool  vr_anticlip_get_head_campos(float out[3]) {
    // Tabletop (preset 0) handles wall collision with its own smooth camera-distance clamp (bettercamera);
    // running this world-nudge there too just makes the two fight, so skip it. Third-person still uses it.
    if (!sAnticlipEnabled || !sHeadCamPosValid || !sViewsValid || sCurrentPreset == 0) { return false; }
    out[0] = sHeadCamPos[0]; out[1] = sHeadCamPos[1]; out[2] = sHeadCamPos[2];
    return true;
}
// Set the smoothed anchor offset (meters) that vr_build_eye_matrix adds to the diorama anchor.
void  vr_anticlip_set_offset(const float m[3]) {
    sAnticlipOffsetM[0] = m[0]; sAnticlipOffsetM[1] = m[1]; sAnticlipOffsetM[2] = m[2];
}

// First-person flip cam: how much to roll the eye view this frame (radians).
void  vr_set_flip_roll(float radians) {
    // The target comes from Mario's animation frame, which advances at the 30fps game rate, so at the
    // 90fps headset rate a direct set would stair-step every 3 frames (choppy). Ease toward the target
    // each frame (called once per VR frame) so the flip pitch is continuous and smooth. Snap when very
    // close so it settles cleanly at 0 between flips.
    sFlipRollRad += (radians - sFlipRollRad) * 0.22f;
    if (fabsf(radians - sFlipRollRad) < 0.0008f) { sFlipRollRad = radians; }
}
// Pick the flip axis: true = side flip (roll about the forward axis), false = forward/back flip (pitch).
void  vr_set_flip_side(bool side) { sFlipIsSide = side; }

// The only VR hotkey: F10 cycles the view presets (Diorama / Third-person / First-person). Everything else
// is tuned from the in-game Options -> VR menu.
static void vr_poll_tuning_keys(void) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (!ks) return;

    static bool prevCycle = false;
    bool cyc = ks[SDL_SCANCODE_F10] != 0;
    // Only cycle while the headset is actually being worn - with it off the diorama presets don't apply,
    // so the hotkey would just shuffle useless modes in the flat view.
    if (cyc && !prevCycle && sState == XR_SESSION_STATE_FOCUSED) vr_cycle_preset();
    prevCycle = cyc;
}

void vr_begin_frame(void) {
    if (!sRequested) return;
    // Debounced write of changed VR settings: flush once the slider/toggle activity settles.
    if (sSettingsDirty && --sSettingsFlushIn <= 0) { vr_settings_save(); sSettingsDirty = false; }
    if (!sBootTried) {
        if (sBootRetryIn > 0) { sBootRetryIn--; }      // headset wasn't ready: wait out the retry delay
        else { sBootTried = true; vr_boot(); }         // first attempt, or the next retry
    }
    if (sSession == XR_NULL_HANDLE) return;

    vr_poll_events();
    vr_poll_tuning_keys();
    sViewsValid = false;
    sPoseTracked = false;
    sOverlayReady = false;
    sHudReady = false;
    sPanelMode = false;
    if (!sRunning) return;

    vr_input_sync(); // refresh motion-controller state for this frame

    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    sFrameState.type = XR_TYPE_FRAME_STATE;
    sFrameState.next = NULL;
    if (!xrok(xrWaitFrame(sSession, &fwi, &sFrameState), "xrWaitFrame")) return;

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (!xrok(xrBeginFrame(sSession, &fbi), "xrBeginFrame")) return;
    sFrameBegun = true;

    if (sFrameState.shouldRender) {
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = sFrameState.predictedDisplayTime;
        vli.space = sLocalSpace;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        sViews[0].type = XR_TYPE_VIEW;
        sViews[1].type = XR_TYPE_VIEW;
        uint32_t got = 0;
        if (XR_SUCCEEDED(xrLocateViews(sSession, &vli, &vs, 2, &got, sViews)) && got == 2) {
            sViewsValid = true;
            // Only treat the pose as usable for world-locking once the runtime says it's really tracked
            // (at first boot the pose can be unset/zero for a few frames while tracking comes up).
            sPoseTracked = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0
                        && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)    != 0;
            vr_build_eye_matrix(0);
            vr_build_eye_matrix(1);
            static bool sLoggedPose = false;
            if (!sLoggedPose) {
                sLoggedPose = true;
                printf("[VR] eye0 LOCAL pos = (%.2f, %.2f, %.2f); diorama anchor Y=%.2f dist=%.2f scale=%.0f\n",
                    sViews[0].pose.position.x, sViews[0].pose.position.y, sViews[0].pose.position.z,
                    sDioramaHeight, sDioramaDist, sDioramaScale);
            }
        }
    }
}

// Acquire eye `eye`'s swapchain image and bind it (color + shared depth) as the
// active render target with the eye-sized viewport. Returns false to skip.
bool vr_begin_eye(int eye) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender || !sViewsValid) return false;
    if (eye < 0 || eye >= (int)sViewCount || sEyeFbo == 0) return false;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sEye[eye].handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sEye[eye].handle, &swi);
    sEyeImgIdx[eye] = idx;

    glBindFramebuffer(GL_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEye[eye].images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sEyeDepthRB);
    glViewport(0, 0, (GLsizei)sEye[eye].w, (GLsizei)sEye[eye].h);
    return true; // gfx_run_dl_vr_eye() will clear + render into this target
}

// Release eye `eye`'s swapchain image and record its projection view for submit.
void vr_end_eye(int eye) {
    if (eye < 0 || eye >= (int)sViewCount) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sEye[eye].handle, &ri);

    memset(&sProjViews[eye], 0, sizeof(sProjViews[eye]));
    sProjViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    sProjViews[eye].pose = sRenderPose[eye]; // separation-adjusted pose we rendered with
    sProjViews[eye].fov  = sRenderFov[eye];  // symmetrized fov we rendered with
    sProjViews[eye].subImage.swapchain = sEye[eye].handle;
    sProjViews[eye].subImage.imageRect.offset.x = 0;
    sProjViews[eye].subImage.imageRect.offset.y = 0;
    sProjViews[eye].subImage.imageRect.extent.width  = (int32_t)sEye[eye].w;
    sProjViews[eye].subImage.imageRect.extent.height = (int32_t)sEye[eye].h;
}

// Acquire + bind the 2D overlay render target (mono). gfx_run_dl_vr_overlay()
// clears + renders the 2D content into it.
bool vr_begin_overlay(bool sky) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sOverlayFbo == 0) return false;
    VrSwapchain *sc = sky ? &sOverlay : &sHud;
    if (sc->handle == XR_NULL_HANDLE) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sc->handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc->handle, &swi);
    if (sky) sOverlayImgIdx = idx; else sHudImgIdx = idx;

    glBindFramebuffer(GL_FRAMEBUFFER, sOverlayFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sOverlayDepthRB);
    glViewport(0, 0, sOverlayW, sOverlayH);
    return true;
}

void vr_end_overlay(bool sky) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    VrSwapchain *sc = sky ? &sOverlay : &sHud;
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sc->handle, &ri);
    if (sky) sOverlayReady = true; else sHudReady = true;
}

// FLATSCREEN-ON-A-PANEL (non-gameplay screens). The caller (pc_main) decides per frame whether
// this is a non-gameplay screen; if so it renders ONE flat frame into the sHud swapchain and we
// submit it as a single large OPAQUE head-locked quad (no projection layer, no dome, no HUD).
void vr_set_panel_mode(bool on) { sPanelMode = on; }

// Choose the flat menu panel's crop/shape this frame: true = full 16:9 frame (in-game menus whose lists
// run to the edges), false = centered 4:3 region on a taller quad (the title/main menu, so no void).
// Either way the panel is world-locked. Changing it re-centers the panel on your current facing.
void vr_set_panel_full_frame(bool full) {
    if (full != sPanelFullFrame) { sPanelAnchorValid = false; sPanelEyeYValid = false; }
    sPanelFullFrame = full;
}

// Acquire + bind the HUD swapchain as the flat-panel render target (mirrors vr_begin_overlay(false)
// but flagged as panel mode). Returns false to skip if the session isn't ready.
bool vr_begin_panel(void) {
    sPanelMode = true;
    return vr_begin_overlay(false); // binds sOverlayFbo + sHud image + depth, sets viewport
}

void vr_end_panel(void) {
    vr_end_overlay(false); // releases sHud, sets sHudReady = true
}

// Theater Panoramic backdrop: load a theater_bg image into a GL texture once. ANY image works - png,
// jpg/jpeg, bmp or tga, any size, any aspect (a 2:1 equirectangular panorama covers the most view; a
// plain photo or wallpaper shows at its own shape). The game folder (next to the exe, same place as
// vr_settings.txt) is tried FIRST so an image can ship with the game; the per-user write folder still
// works as a fallback. stb_image is already compiled into gfx_pc.c, so we just declare the symbols.
static bool vr_backdrop_load_image(void) {
    if (sBgLoadTried) { return sBgTex != 0; }
    sBgLoadTried = true;
    extern unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
    extern void stbi_image_free(void *retval_from_stbi_load);
    extern const char *fs_get_write_path(const char *suffix);
    static const char *kBgNames[] = { "theater_bg.png", "theater_bg.jpg", "theater_bg.jpeg", "theater_bg.bmp", "theater_bg.tga" };
    const int kBgNameCount = (int)(sizeof(kBgNames) / sizeof(kBgNames[0]));
    int w = 0, h = 0, c = 0;
    const char *path = NULL;
    unsigned char *px = NULL;
    for (int i = 0; i < kBgNameCount && !px; i++) {           // game folder (next to the exe) - the shipped image
        path = kBgNames[i];
        px = stbi_load(path, &w, &h, &c, 4);
    }
    for (int i = 0; i < kBgNameCount && !px; i++) {           // fallback: per-user write folder
        path = fs_get_write_path(kBgNames[i]);
        px = stbi_load(path, &w, &h, &c, 4);
    }
    if (!px) { printf("[VR] Theater backdrop: no theater_bg image (.png/.jpg/.jpeg/.bmp/.tga) next to the exe or in the user folder\n"); return false; }
    if (sBgTex == 0) { glGenTextures(1, &sBgTex); }
    glBindTexture(GL_TEXTURE_2D, sBgTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);        // wrap horizontally (360)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    glBindTexture(GL_TEXTURE_2D, 0);
    sBgW = w; sBgH = h;
    printf("[VR] Theater backdrop: loaded %s (%dx%d)\n", path, w, h);
    return true;
}

// Blit the loaded panorama into the world-locked overlay swapchain so vr_submit can present it as a
// cylinder/equirect/quad backdrop layer. Called from pc_main in the Theater panel path when mode == Panoramic.
bool vr_render_backdrop_pano(void) {
    sBackdropReady = false;
    if (!vr_backdrop_load_image()) { return false; }
    bool beganOverlay = vr_begin_overlay(true);       // acquires sOverlay + binds sOverlayFbo + viewport
    { static int d = 0; if ((d++ % 30) == 0) printf("[BGDIAG] begin=%d handle=%p w=%d h=%d fbo=%u tex=%u\n",
        (int) beganOverlay, (void *) sOverlay.handle, (int) sOverlay.w, (int) sOverlay.h, (unsigned) sOverlayFbo, (unsigned) sBgTex); }
    if (!beganOverlay) { return false; }
    static GLuint sBgReadFbo = 0;
    if (sBgReadFbo == 0) { glGenFramebuffers(1, &sBgReadFbo); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBgReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sBgTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sOverlayFbo);
    glBlitFramebuffer(0, 0, sBgW, sBgH, 0, 0, (GLint)sOverlay.w, (GLint)sOverlay.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    vr_end_overlay(true);                              // releases sOverlay + sets sOverlayReady
    sBackdropReady = true;                             // sOverlay was genuinely acquired+blitted this frame
    return true;
}

void vr_submit(void) {
    if (!sRunning || !sFrameBegun) return;
    sFrameBegun = false;

    // Out of a menu this frame -> drop the world-lock anchor so the next menu re-centers in front of you.
    if (!sPanelMode) sPanelAnchorValid = false;

    XrCompositionLayerProjection   proj    = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad         hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    // FUNCTION scope, not block scope: layers[] holds POINTERS that xrEndFrame reads at the bottom of this
    // function. A block-scoped layer struct dies at its closing brace, the compiler reuses the stack slot,
    // and the runtime reads garbage - which is exactly an XR_ERROR_HANDLE_INVALID that looks like a bad
    // swapchain. (This is what broke every Theater backdrop layer type: cylinder AND quad.)
    XrCompositionLayerQuad         bgQuad  = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    // Game outputs STRAIGHT (non-premultiplied) alpha; the head-locked HUD quad must be UNPREMULTIPLIED.
    const XrCompositionLayerFlags kBlend = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                                         | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layerCount = 0;

    if (sPanelMode) {
        // Theater Panoramic backdrop: a user image, world-locked behind the cinema screen, emitted FIRST
        // (layers[0]) so the opaque panel quad composites on top. The 360 cylinder/equirect2 layers return
        // XR_ERROR_HANDLE_INVALID on this runtime (VirtualDesktopXR rejects them despite advertising support),
        // which broke the whole frame submission - so use a large flat QUAD backdrop, the same proven layer
        // type as the panel. It fills the forward view (black at the far edges) instead of a true 360 wrap.
        if (sTheaterBg == VR_BG_PANORAMIC && sBackdropReady && sOverlay.handle != XR_NULL_HANDLE
            && sLocalSpace != XR_NULL_HANDLE && sViewsValid) {
            float hx = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
            float hy = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
            float hz = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);
            bgQuad.layerFlags = 0;
            bgQuad.space = sLocalSpace;
            bgQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            bgQuad.subImage.swapchain = sOverlay.handle;
            bgQuad.subImage.imageRect.extent.width  = (int32_t) sOverlay.w;
            bgQuad.subImage.imageRect.extent.height = (int32_t) sOverlay.h;
            bgQuad.pose.orientation.w = 1.0f;            // faces the user (same convention as the panel quad)
            bgQuad.pose.position.x = hx;
            bgQuad.pose.position.y = hy;
            bgQuad.pose.position.z = hz - 12.0f;         // behind the screen, forward (-Z)
            // Size the quad to the IMAGE's aspect: the blit stretched the picture to fill the 16:9
            // swapchain, and the quad shape un-stretches it, so any image shows at its natural
            // proportions - a 2:1 panorama covers the most view, a portrait shot just stands taller.
            bgQuad.size.width  = 50.0f;                  // large: fills the forward view
            bgQuad.size.height = (sBgW > 0 && sBgH > 0) ? 50.0f * (float) sBgH / (float) sBgW
                                                        : 50.0f * (float) sOverlay.h / (float) sOverlay.w;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&bgQuad;
        }
        // FLATSCREEN-ON-A-PANEL: a non-gameplay screen was rendered flat into sHud. Present it as the
        // ONLY layer - one large OPAQUE quad. The flat frame is SM64 4:3 centered inside the 16:9
        // swapchain, so crop to the centered 4:3 region and size the quad 4:3 (no stretch, no bars).
        if (sHudReady) {
            hudQuad.layerFlags = 0;                                 // OPAQUE virtual screen
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            // Theater mode plays the flat game on a big cinema screen; menus/transitions use the menu panel.
            float panelSize = vr_is_theater_mode() ? sTheaterSize : sMenuSize;
            float panelDist = vr_is_theater_mode() ? sTheaterDist : sMenuDist;
            if (sPanelFullFrame) {
                // In-game menus: their lists run to the far left/right of the frame, so show the WHOLE
                // 16:9 swapchain (no crop) on a 16:9 quad - nothing cut.
                hudQuad.subImage.imageRect.offset.x = 0;
                hudQuad.subImage.imageRect.offset.y = 0;
                hudQuad.subImage.imageRect.extent.width  = (int32_t)sHud.w;
                hudQuad.subImage.imageRect.extent.height = (int32_t)sHud.h;
                hudQuad.size.width  = panelSize;
                hudQuad.size.height = panelSize * (float)sHud.h / (float)sHud.w; // 16:9
            } else {
                // Title/main menu: content is centered in the 4:3 region. Crop to it and use a taller
                // 4:3 quad so it fills your view with no empty void top/bottom (the original look).
                const int32_t cropH = (int32_t)sHud.h;
                const int32_t cropW = (int32_t)(sHud.h * 4 / 3);
                const int32_t cropX = ((int32_t)sHud.w - cropW) / 2;
                hudQuad.subImage.imageRect.offset.x = cropX;
                hudQuad.subImage.imageRect.offset.y = 0;
                hudQuad.subImage.imageRect.extent.width  = cropW;
                hudQuad.subImage.imageRect.extent.height = cropH;
                hudQuad.size.width  = panelSize;
                hudQuad.size.height = panelSize * 3.0f / 4.0f; // 4:3
            }

            if (sLocalSpace != XR_NULL_HANDLE) {
                // World-locked, but smart about it: the panel re-anchors to the head's yaw-only pose
                // (1) until the runtime reports a real tracked pose - so first boot doesn't freeze the
                //     panel to an un-tracked/zero pose, and
                // (2) whenever you turn your head more than ~55 degrees away from it - so it follows you
                //     back into view instead of getting lost behind you.
                // Otherwise it stays put, so within that window you can freely turn your head to read it.
                if (sViewsValid) {
                    float cqy = sViews[0].pose.orientation.y, cqw = sViews[0].pose.orientation.w;
                    float cn = sqrtf(cqy*cqy + cqw*cqw); if (cn < 1e-6f) { cqy = 0.0f; cqw = 1.0f; cn = 1.0f; }
                    cqy /= cn; cqw /= cn;
                    float hfx = -2.0f * cqw * cqy, hfz = -(1.0f - 2.0f * cqy * cqy); // current head forward
                    float afx = -2.0f * sPanelAnchorQw * sPanelAnchorQy, afz = -(1.0f - 2.0f * sPanelAnchorQy * sPanelAnchorQy);
                    float dot = hfx * afx + hfz * afz; // ~cos(angle between head and panel)
                    bool reAnchor = !sPanelAnchorValid || !sPoseTracked || (dot < 0.57f); // 0.57 ~= cos(55 deg)
                    if (reAnchor) {
                        sPanelAnchorPos[0] = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
                        sPanelAnchorPos[2] = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);
                        // Lock the panel Y to the FIRST tracked eye height and reuse it, so the menu sits at
                        // eye level instead of drifting low when re-anchored while looking down / at a bad Y.
                        if (!sPanelEyeYValid && sPoseTracked) {
                            sPanelEyeY = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
                            sPanelEyeYValid = true;
                        }
                        sPanelAnchorPos[1] = sPanelEyeYValid ? sPanelEyeY
                                          : 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
                        sPanelAnchorQy = cqy; sPanelAnchorQw = cqw;
                        sPanelAnchorValid = sPoseTracked; // only "lock in" once the pose is real
                    }
                }
                // Forward of the yaw-only quaternion Q=(0,qy,0,qw): R(Q)*(0,0,-1).
                float qy = sPanelAnchorQy, qw = sPanelAnchorQw;
                float fwdx = -2.0f * qw * qy;
                float fwdz = -(1.0f - 2.0f * qy * qy);
                hudQuad.space = sLocalSpace;
                hudQuad.pose.orientation.x = 0.0f;
                hudQuad.pose.orientation.y = qy;
                hudQuad.pose.orientation.z = 0.0f;
                hudQuad.pose.orientation.w = qw;
                hudQuad.pose.position.x = sPanelAnchorPos[0] + panelDist * fwdx;
                hudQuad.pose.position.y = (sPanelEyeYValid ? sPanelEyeY : sPanelAnchorPos[1]) + sMenuVOffset;
                hudQuad.pose.position.z = sPanelAnchorPos[2] + panelDist * fwdz;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&hudQuad;
            }
        }
    } else if (sFrameState.shouldRender && sViewsValid && sViewCount == 2) {
        // The sky is now a 3D sphere rendered INSIDE the eye (world-locked, full coverage,
        // no black poles), so the eye/projection layer is fully opaque - no separate sky layer.
        proj.layerFlags = 0;
        proj.space = sLocalSpace;
        proj.viewCount = 2;
        proj.views = sProjViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&proj;
        // 3) HUD/menus: head-locked, near. Over a world (perspective drew this frame) the quad is
        //    transparent except the UI, sized small (sHud*). (The opaque-large fallback for 2D-only
        //    frames now lives in the sPanelMode branch above, driven by the game-state predicate.)
        if (sHudReady && sViewSpace != XR_NULL_HANDLE) {
            hudQuad.layerFlags = kBlend; // HUD: source-alpha blend over the world
            hudQuad.space = sViewSpace;
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            hudQuad.subImage.imageRect.extent.width  = (int32_t)sHud.w;
            hudQuad.subImage.imageRect.extent.height = (int32_t)sHud.h;
            hudQuad.pose.orientation.w = 1.0f;
            hudQuad.pose.position.z = -sHudDist;
            hudQuad.size.width  = sHudSize;
            hudQuad.size.height = sHudSize * (float)sOverlayH / (float)sOverlayW;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&hudQuad;
        }
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = sFrameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : NULL;
    if (!xrok(xrEndFrame(sSession, &fei), "xrEndFrame")) {
        // Self-recovery: never let a bad Theater backdrop layer brick VR. If submission keeps failing while a
        // backdrop is active, drop back to Black so the panel keeps presenting.
        if (sPanelMode && sTheaterBg != VR_BG_BLACK) {
            static int sBgFails = 0;
            if (++sBgFails >= 8) {
                sTheaterBg = VR_BG_BLACK; sBgFails = 0;
                printf("[VR] Theater backdrop kept failing submission - reverted to Black.\n");
            }
        }
    }
    sBackdropReady = false; // consume; next frame must re-prove the backdrop was acquired
}

void vr_shutdown(void) {
    // Flush a VR menu change still waiting on the save debounce so quitting right after a tweak can't
    // lose it. Gated on the settings file having been loaded this session, so a flatscreen session
    // (which never loads it) can't overwrite the values saved by a previous VR session.
    if (sSettingsDirty && sSettingsLoaded) { vr_settings_save(); sSettingsDirty = false; }
    if (sBgTex)      { glDeleteTextures(1, &sBgTex); sBgTex = 0; }
    if (sEyeFbo)     { glDeleteFramebuffers(1, &sEyeFbo); sEyeFbo = 0; }
    if (sEyeDepthRB) { glDeleteRenderbuffers(1, &sEyeDepthRB); sEyeDepthRB = 0; }
    if (sOverlayFbo)     { glDeleteFramebuffers(1, &sOverlayFbo); sOverlayFbo = 0; }
    if (sOverlayDepthRB) { glDeleteRenderbuffers(1, &sOverlayDepthRB); sOverlayDepthRB = 0; }
    if (sOverlay.handle != XR_NULL_HANDLE) { xrDestroySwapchain(sOverlay.handle); sOverlay.handle = XR_NULL_HANDLE; }
    if (sOverlay.images) { free(sOverlay.images); sOverlay.images = NULL; }
    if (sHud.handle != XR_NULL_HANDLE) { xrDestroySwapchain(sHud.handle); sHud.handle = XR_NULL_HANDLE; }
    if (sHud.images) { free(sHud.images); sHud.images = NULL; }
    if (sViewSpace != XR_NULL_HANDLE) { xrDestroySpace(sViewSpace); sViewSpace = XR_NULL_HANDLE; }
    for (int e = 0; e < 2; e++) {
        if (sEye[e].handle != XR_NULL_HANDLE) { xrDestroySwapchain(sEye[e].handle); sEye[e].handle = XR_NULL_HANDLE; }
        if (sEye[e].images) { free(sEye[e].images); sEye[e].images = NULL; }
        sEye[e].imgCount = 0;
    }
    if (sLocalSpace != XR_NULL_HANDLE) { xrDestroySpace(sLocalSpace); sLocalSpace = XR_NULL_HANDLE; }
    if (sSession    != XR_NULL_HANDLE) { xrDestroySession(sSession);  sSession    = XR_NULL_HANDLE; }
    if (sActionSet  != XR_NULL_HANDLE) { xrDestroyActionSet(sActionSet); sActionSet = XR_NULL_HANDLE; } // also destroys its actions
    sActMove = sActCam = sActBtnA = sActBtnB = sActBtnX = sActBtnY = sActMenu = XR_NULL_HANDLE;
    sActLStick = sActRStick = sActLTrigger = sActRTrigger = sActLGrip = sActRGrip = sActHaptic = XR_NULL_HANDLE;
    sInputAttached = false;
    sCtrlButtons = 0;
    memset(sCtrlStick, 0, sizeof(sCtrlStick));
    if (sInstance   != XR_NULL_HANDLE) { xrDestroyInstance(sInstance); sInstance   = XR_NULL_HANDLE; }
    sRunning = false;
    sFrameBegun = false;
    sViewCount = 0;
}

#else // !_WIN32 - VR is Windows/WGL-only for now; stub out elsewhere.

bool  vr_is_active(void)     { return false; }
bool  vr_is_focused(void)    { return false; }
bool  vr_headset_present(void) { return false; }
bool  vr_first_person_active(void) { return false; }
void  vr_set_first_person(bool on) { (void)on; }
float vr_get_menu_dist(void)         { return 0.0f; } void vr_set_menu_dist(float v)     { (void)v; }
float vr_get_menu_size(void)         { return 0.0f; } void vr_set_menu_size(float v)     { (void)v; }
float vr_get_theater_dist(void)      { return 0.0f; } void vr_set_theater_dist(float v)  { (void)v; }
float vr_get_theater_size(void)      { return 0.0f; } void vr_set_theater_size(float v)  { (void)v; }
int   vr_get_theater_bg(void)        { return 0; } void vr_set_theater_bg(int mode) { (void)mode; }
bool  vr_render_backdrop_pano(void)  { return false; }
float vr_get_diorama_dist(void)      { return 0.0f; } void vr_set_diorama_dist(float v)  { (void)v; }
float vr_get_diorama_scale(void)     { return 0.0f; } void vr_set_diorama_scale(float v) { (void)v; }
float vr_get_stereo(void)            { return 0.0f; } void vr_set_stereo(float v)        { (void)v; }
float vr_get_diorama_height(void)    { return 0.0f; } void vr_set_diorama_height(float v){ (void)v; }
float vr_get_head_scale(void)        { return 0.0f; } void vr_set_head_scale(float v)    { (void)v; }
int   vr_get_refresh_rate(void)      { return 0; }
float vr_get_hud_size(void)          { return 0.0f; } void vr_set_hud_size(float v) { (void)v; }
float vr_get_world_scale(void)       { return 1.0f; } void vr_set_world_scale(float v) { (void)v; }
int   vr_get_look_yaw(void)          { return 0; }
bool  vr_get_head_move(void)         { return false; } void vr_set_head_move(bool e) { (void)e; }
bool  vr_is_tabletop_mode(void)      { return false; }
bool  vr_is_theater_mode(void)       { return false; }
void  vr_reset_defaults(void) {}
void  vr_settings_mark_dirty(void) {}
bool  vr_anticlip_is_enabled(void)   { return false; } void vr_anticlip_set_enabled(bool e) { (void)e; }
bool  vr_anticlip_get_head_campos(float out[3]) { (void)out; return false; }
void  vr_anticlip_set_offset(const float m[3]) { (void)m; }
void  vr_set_flip_roll(float radians) { (void)radians; }
void  vr_set_flip_side(bool side) { (void)side; }
int   vr_eye_count(void)     { return 0; }
int   vr_eye_width(int e)    { (void)e; return 0; }
int   vr_eye_height(int e)   { (void)e; return 0; }
const float *vr_eye_viewproj(int e) { (void)e; return 0; }
const float *vr_sky_viewproj(int e) { (void)e; return 0; }
void  vr_begin_frame(void)   {}
bool  vr_begin_eye(int e)    { (void)e; return false; }
void  vr_end_eye(int e)      { (void)e; }
void  vr_submit(void)        {}
void  vr_set_panel_mode(bool on) { (void)on; }
void  vr_set_panel_full_frame(bool full) { (void)full; }
bool  vr_begin_panel(void)   { return false; }
void  vr_end_panel(void)     {}
bool     vr_controllers_active(void) { return false; }
unsigned vr_controller_buttons(void) { return 0; }
void     vr_controller_stick(int hand, float out[2]) { (void)hand; out[0] = out[1] = 0.0f; }
void     vr_controller_rumble(float strength, float seconds) { (void)strength; (void)seconds; }
void     vr_controller_rumble_stop(void) {}
void  vr_shutdown(void)      {}

#endif
