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
static bool sRunning    = false; // session is between xrBeginSession/xrEndSession
static bool sFrameBegun = false; // xrBeginFrame issued, owes an xrEndFrame
static bool sViewsValid = false; // sViews holds valid per-eye poses for this frame

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
static float sDioramaDist   = -0.17f;  // meters the anchor sits in front (-Z); default = Close-up preset
static float sDioramaHeight = 0.0f;    // meters above LOCAL origin (~eye height); default = Close-up preset
static float sClipMargin    = 0.30f;   // anti-clip: keep the diorama this far from the head (backs off on lean-in)
// (eye-space clip planes are computed per-frame from sDioramaScale so a bigger
//  world isn't far-clipped to black - see vr_build_eye_matrix.)

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
static XrPosef sRenderPose[2];
static XrFovf  sRenderFov[2]; // symmetrized fov actually rendered + submitted

static PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetGLReq = NULL;

bool vr_is_active(void)      { return sRunning; }
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
    if (eye == 0 && !sHeadRestSet && ++sHeadWarmup >= 15) {
        sHeadRest[0] = cx; sHeadRest[1] = cy; sHeadRest[2] = cz; sHeadRestSet = true;
        printf("[VR] 6DoF rest captured at (%.2f, %.2f, %.2f)\n", cx, cy, cz);
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
    if (headToAnchor < sClipMargin) effDist += (sClipMargin - headToAnchor);

    float A[4][4] = {{0}};
    float invS = 1.0f / sDioramaScale;
    A[0][0] = invS; A[1][1] = invS; A[2][2] = invS; A[3][3] = 1.0f;
    A[3][0] = 0.0f; A[3][1] = sDioramaHeight; A[3][2] = -effDist;

    // Clip planes adapt to the world scale so a big/life-size world isn't
    // far-clipped to black. SM64 geometry spans ~±8000 units.
    float worldHalfM = 8000.0f / sDioramaScale;
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

    float V[4][4], P[4][4], AV[4][4];
    mat_view_from_pose(V, pose);
    mat_proj_fov(P, fov, zn, zf);
    mat_mul(AV, A, V);
    mat_mul(sEyeVP[eye], AV, P);

    // Eye-sphere dome view-proj: rotation-only (zero translation -> sky at infinity, no
    // parallax) with a far projection so the radius-1000 sphere fits. World-locked because
    // it tracks the head's ROTATION only (the eye/projection layer is world-locked).
    XrPosef skyPose = pose;
    skyPose.position.x = skyPose.position.y = skyPose.position.z = 0.0f;
    float Vsky[4][4], Psky[4][4];
    mat_view_from_pose(Vsky, skyPose);
    mat_proj_fov(Psky, fov, 1.0f, 5000.0f);
    mat_mul(sSkyVP[eye], Vsky, Psky);

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
    printf("[VR] booting OpenXR...\n");

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
    if (XR_SUCCEEDED(xrGetInstanceProperties(sInstance, &props)))
        printf("[VR] runtime: %s\n", props.runtimeName);
    printf("[VR] surround sky (cylinder layer): %s\n", sHasCylinder ? "yes" : "no - flat quad fallback");
    printf("[VR] full-sphere sky (equirect2 layer): %s\n", sHasEquirect2 ? "yes - no black poles" : "no - cylinder fallback");

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrok(xrGetSystem(sInstance, &sgi, &sSystemId), "xrGetSystem")) { vr_shutdown(); return; }

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

    printf("[VR] OpenXR ready; waiting for session to start.\n");
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
typedef struct { const char *name; float scale, dist, height, stereo; } VrPreset;
static const VrPreset sPresets[] = {
    { "Diorama",      1200.0f,  0.37f, -0.20f, 0.21f },
    { "Close-up",     1200.0f, -0.17f,  0.00f, 0.21f },
    // The old "First-person" entry was just a very close diorama, not real first person.
    // Removed for now; true first-person VR (camera at Mario's head, 1:1 scale) is the goal.
};
#define VR_NUM_PRESETS ((int)(sizeof(sPresets) / sizeof(sPresets[0])))
static int sCurrentPreset = 1; // launch default = Close-up

static void vr_apply_preset(int idx) {
    if (idx < 0 || idx >= VR_NUM_PRESETS) return;
    sCurrentPreset = idx;
    sDioramaScale  = sPresets[idx].scale;
    sDioramaDist   = sPresets[idx].dist;
    sDioramaHeight = sPresets[idx].height;
    sStereoScale   = sPresets[idx].stereo;
    printf("[VR] preset %d/%d: %s (scale=%.0f dist=%.2f height=%.2f stereo=%.2f)\n",
        idx + 1, VR_NUM_PRESETS, sPresets[idx].name,
        sDioramaScale, sDioramaDist, sDioramaHeight, sStereoScale);
    vr_write_tune_file();
}

// Live diorama tuning (dev): F10 cycles presets; F1/F2 scale, F3/F4 distance,
// F5/F6 height, F8/F9 stereo, F7 reset to the current preset.
static void vr_poll_tuning_keys(void) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (!ks) return;

    // F10 cycles through the presets (edge-triggered so a tap advances one).
    static bool prevCycle = false;
    bool cyc = ks[SDL_SCANCODE_F10] != 0;
    if (cyc && !prevCycle) vr_apply_preset((sCurrentPreset + 1) % VR_NUM_PRESETS);
    prevCycle = cyc;

    bool changed = false;
    if (ks[SDL_SCANCODE_F1]) { sDioramaScale  *= 0.98f; changed = true; } // bigger world
    if (ks[SDL_SCANCODE_F2]) { sDioramaScale  *= 1.02f; changed = true; } // smaller world
    if (ks[SDL_SCANCODE_F3]) { sDioramaDist   -= 0.01f; changed = true; }
    if (ks[SDL_SCANCODE_F4]) { sDioramaDist   += 0.01f; changed = true; }
    if (ks[SDL_SCANCODE_F5]) { sDioramaHeight -= 0.01f; changed = true; }
    if (ks[SDL_SCANCODE_F6]) { sDioramaHeight += 0.01f; changed = true; }
    if (ks[SDL_SCANCODE_F8]) { sStereoScale   -= 0.01f; changed = true; } // gentler stereo (less cross-eye)
    if (ks[SDL_SCANCODE_F9]) { sStereoScale   += 0.01f; changed = true; } // stronger stereo (more depth)
    if (ks[SDL_SCANCODE_LEFTBRACKET])  { sHeadScale -= 0.02f; changed = true; } // [  steadier (less 6DoF parallax)
    if (ks[SDL_SCANCODE_RIGHTBRACKET]) { sHeadScale += 0.02f; changed = true; } // ]  more 6DoF parallax
    if (ks[SDL_SCANCODE_F7]) { vr_apply_preset(sCurrentPreset); sHeadRestSet = false; sHeadWarmup = 0; } // reset preset + recenter 6DoF
    if (sDioramaScale < 30.0f)     sDioramaScale = 30.0f;
    if (sDioramaScale > 100000.0f) sDioramaScale = 100000.0f;
    if (sStereoScale < 0.0f) sStereoScale = 0.0f;
    if (sStereoScale > 4.0f) sStereoScale = 4.0f;
    if (sHeadScale < 0.0f) sHeadScale = 0.0f;
    if (sHeadScale > 1.5f) sHeadScale = 1.5f;
    if (changed) {
        vr_write_tune_file(); // keep vr_tune.txt current so I can read the values back
        static int throttle = 0;
        if ((throttle++ % 15) == 0)
            printf("[VR] tune: scale=%.0f dist=%.2f height=%.2f stereo=%.2f head=%.2f\n",
                sDioramaScale, sDioramaDist, sDioramaHeight, sStereoScale, sHeadScale);
    }
}

void vr_begin_frame(void) {
    if (!sRequested) return;
    if (!sBootTried) { sBootTried = true; vr_boot(); }
    if (sSession == XR_NULL_HANDLE) return;

    vr_poll_events();
    vr_poll_tuning_keys();
    sViewsValid = false;
    sOverlayReady = false;
    sHudReady = false;
    sPanelMode = false;
    if (!sRunning) return;

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

// Acquire + bind the HUD swapchain as the flat-panel render target (mirrors vr_begin_overlay(false)
// but flagged as panel mode). Returns false to skip if the session isn't ready.
bool vr_begin_panel(void) {
    sPanelMode = true;
    return vr_begin_overlay(false); // binds sOverlayFbo + sHud image + depth, sets viewport
}

void vr_end_panel(void) {
    vr_end_overlay(false); // releases sHud, sets sHudReady = true
}

void vr_submit(void) {
    if (!sRunning || !sFrameBegun) return;
    sFrameBegun = false;

    XrCompositionLayerProjection   proj    = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad         hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    // Game outputs STRAIGHT (non-premultiplied) alpha; the head-locked HUD quad must be UNPREMULTIPLIED.
    const XrCompositionLayerFlags kBlend = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                                         | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layerCount = 0;

    if (sPanelMode) {
        // FLATSCREEN-ON-A-PANEL: a non-gameplay screen was rendered flat into sHud. Present it as
        // the ONLY layer - one large OPAQUE head-locked quad (no projection layer, no dome, no
        // separate HUD), so it stays centered in front of you (just farther away than before). The
        // flat frame is SM64 4:3 centered inside the 16:9 swapchain, so crop the subImage to the
        // centered 4:3 region and size the quad 4:3 -> no stretch, no black bars.
        if (sHudReady && sViewSpace != XR_NULL_HANDLE) {
            const int32_t cropH = (int32_t)sHud.h;                 // full height (1080)
            const int32_t cropW = (int32_t)(sHud.h * 4 / 3);       // centered 4:3 width (1440)
            const int32_t cropX = ((int32_t)sHud.w - cropW) / 2;   // x offset (240)
            hudQuad.layerFlags = 0;                                 // OPAQUE virtual screen
            hudQuad.space = sViewSpace;                             // head-locked -> always centered
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            hudQuad.subImage.imageRect.offset.x = cropX;
            hudQuad.subImage.imageRect.offset.y = 0;
            hudQuad.subImage.imageRect.extent.width  = cropW;
            hudQuad.subImage.imageRect.extent.height = cropH;
            hudQuad.pose.orientation.w = 1.0f;
            hudQuad.pose.position.z = -sMenuDist;                   // comfortable distance (farther = less in-your-face)
            hudQuad.size.width  = sMenuSize;                        // large virtual screen width
            hudQuad.size.height = sMenuSize * 3.0f / 4.0f;          // 4:3 -> no stretch
            layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&hudQuad;
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
    xrok(xrEndFrame(sSession, &fei), "xrEndFrame");
}

void vr_shutdown(void) {
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
    if (sInstance   != XR_NULL_HANDLE) { xrDestroyInstance(sInstance); sInstance   = XR_NULL_HANDLE; }
    sRunning = false;
    sFrameBegun = false;
    sViewCount = 0;
}

#else // !_WIN32 - VR is Windows/WGL-only for now; stub out elsewhere.

bool  vr_is_active(void)     { return false; }
bool  vr_headset_present(void) { return false; }
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
bool  vr_begin_panel(void)   { return false; }
void  vr_end_panel(void)     {}
void  vr_shutdown(void)      {}

#endif
