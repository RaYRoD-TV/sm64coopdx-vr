#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "sm64.h"

#include "pc/lua/smlua.h"
#include "pc/lua/utils/smlua_text_utils.h"
#include "game/memory.h"
#include "audio/data.h"
#include "audio/external.h"

#include "network/network.h"
#include "lua/smlua.h"

#include "rom_assets.h"
#include "rom_checker.h"
#include "pc_main.h"
#include "loading.h"
#include "cliopts.h"
#include "vr/vr.h"
#include "configfile.h"
#include "thread.h"
#include "controller/controller_api.h"
#include "controller/controller_keyboard.h"
#include "controller/controller_mouse.h"
#include "fs/fs.h"

#include "game/display.h" // for gGlobalTimer
#include "game/game_init.h"
#include "game/main.h"
#include "game/rumble_init.h"
#include "game/level_update.h"  // sCurrPlayMode, PLAY_MODE_*, gCurrCreditsEntry, gVrInActSelector
#include "game/ingame_menu.h"   // gMenuMode, get_dialog_id
#include "game/area.h"          // gWarpTransition.isActive - door/level transition active
#include "dialog_ids.h"         // DIALOG_NONE
#include "game/camera.h"               // gCamera (->mtx is the renderer's world->camera matrix)
#include "engine/surface_collision.h"  // find_floor / find_ceil / find_wall_collisions
#include "engine/math_util.h"          // Mat4, mtxf_inverse_non_affine
#include "game/object_list_processor.h" // gCheckingSurfaceCollisionsForCamera

#include "pc/lua/utils/smlua_audio_utils.h"

#include "pc/network/version.h"
#include "pc/network/socket/socket.h"
#include "pc/network/network_player.h"
#include "pc/update_checker.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_unicode.h"
#include "pc/djui/djui_panel.h"
#include "pc/djui/djui_panel_modlist.h"
#include "pc/djui/djui_ctx_display.h"
#include "pc/djui/djui_fps_display.h"
#include "pc/djui/djui_lua_profiler.h"
#include "pc/debuglog.h"
#include "pc/utils/misc.h"
#include "pc/mods/mods.h"

#include "debug_context.h"
#include "menu/intro_geo.h"

#include "gfx_dimensions.h"
#include "game/segment2.h"

#include "engine/math_util.h"

#ifdef DISCORD_SDK
#include "pc/discord/discord.h"
#endif

#include "pc/mumble/mumble.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <SDL2/SDL.h>

extern Vp gViewportFullscreen;

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

s32 gRumblePakPfs;
u32 gNumVblanks = 0;

u8 gRenderingInterpolated = 0;
f32 gRenderingDelta = 0;
f32 gFramePercentage = 0.f;

#define FRAMERATE 30
static const f64 sFrameTime = (1.0 / ((double)FRAMERATE));
static f64 sFpsTimeLast = 0;
static f64 sFrameTimeStart = 0;
static u32 sDrawnFrames = 0;

bool gGameInited = false;
bool gGfxInited = false;

f32 gMasterVolume;

u8 gLuaVolumeMaster = 127;
u8 gLuaVolumeLevel = 127;
u8 gLuaVolumeSfx = 127;
u8 gLuaVolumeEnv = 127;

struct AudioAPI* gAudioApi = &audio_null;
struct GfxWindowManagerAPI* gWindowApi = &gfx_dummy_wm_api;
struct GfxRenderingAPI* gRenderApi = &gfx_dummy_renderer_api;

extern void gfx_run(Gfx *commands);
extern void gfx_run_dl_vr_eye(Gfx *commands, const float eyeViewProj[16], const float skyViewProj[16], int eyeW, int eyeH);
extern void gfx_run_dl_vr_overlay(Gfx *commands, int w, int h, bool sky);
extern void gfx_run_dl_vr_panel(Gfx *commands, int w, int h);
extern void thread5_game_loop(void *arg);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {}
void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {}

void send_display_list(struct SPTask *spTask) {
    if (!gGameInited) { return; }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 560 // gAudioBufferParameters.maxAiBufferLength
#define SAMPLES_LOW 528 // gAudioBufferParameters.minAiBufferLength
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

extern void patch_mtx_before(void);
extern void patch_screen_transition_before(void);
extern void patch_title_screen_before(void);
extern void patch_dialog_before(void);
extern void patch_hud_before(void);
extern void patch_paintings_before(void);
extern void patch_bubble_particles_before(void);
extern void patch_snow_particles_before(void);
extern void patch_djui_before(void);
extern void patch_djui_hud_before(void);
extern void patch_scroll_targets_before(void);

extern void patch_mtx_interpolated(f32 delta);
extern void patch_screen_transition_interpolated(f32 delta);
extern void patch_title_screen_interpolated(f32 delta);
extern void patch_dialog_interpolated(f32 delta);
extern void patch_hud_interpolated(f32 delta);
extern void patch_paintings_interpolated(f32 delta);
extern void patch_bubble_particles_interpolated(f32 delta);
extern void patch_snow_particles_interpolated(f32 delta);
extern void patch_djui_interpolated(f32 delta);
extern void patch_djui_hud(f32 delta);
extern void patch_scroll_targets_interpolated(f32 delta);

static void patch_interpolations_before(void) {
    patch_mtx_before();
    patch_screen_transition_before();
    patch_title_screen_before();
    patch_dialog_before();
    patch_hud_before();
    patch_paintings_before();
    patch_bubble_particles_before();
    patch_snow_particles_before();
    patch_djui_before();
    patch_djui_hud_before();
    patch_scroll_targets_before();
}

static inline void patch_interpolations(f32 delta) {
    patch_mtx_interpolated(delta);
    patch_screen_transition_interpolated(delta);
    patch_title_screen_interpolated(delta);
    patch_dialog_interpolated(delta);
    patch_hud_interpolated(delta);
    patch_paintings_interpolated(delta);
    patch_bubble_particles_interpolated(delta);
    patch_snow_particles_interpolated(delta);
    patch_djui_interpolated(delta);
    patch_djui_hud(delta);
    patch_scroll_targets_interpolated(delta);
}

static void compute_fps(f64 curTime) {
    u32 fps = round((f64) sDrawnFrames / MAX(0.001, curTime - sFpsTimeLast));
    djui_fps_display_update(fps);
    sFpsTimeLast = curTime;
    sDrawnFrames = 0;
}

static s32 get_num_frames_to_draw(f64 t, u32 frameLimit) {
    if (frameLimit % FRAMERATE == 0) {
        return frameLimit / FRAMERATE;
    }
    s64 numFramesCurr = (s64) (t * (f64) frameLimit);
    s64 numFramesNext = (s64) ((t + sFrameTime) * (f64) frameLimit);
    return (s32) MAX(1, numFramesNext - numFramesCurr);
}

static u32 get_display_refresh_rate(void) {
    static u32 refreshRate = 0;
    if (!refreshRate) {
        SDL_DisplayMode mode;
        if (SDL_GetCurrentDisplayMode(0, &mode) == 0) {
            if (mode.refresh_rate > 0) { refreshRate = (u32) mode.refresh_rate; }
        } else {
            refreshRate = 60;
        }
    }
    return refreshRate;
}

static u32 get_target_refresh_rate(void) {
    if (configFramerateMode == RRM_MANUAL) { return configFrameLimit; }
    if (configFramerateMode == RRM_UNLIMITED) { return 3000; } // Has no effect
    return get_display_refresh_rate();
}

static void select_graphics_backend(void) {
    if (gCLIOpts.headless) {
        return;
    }

#if defined(_WIN32)
    if (configGraphicsBackend == GAPI_GL && !gfx_sdl_check_opengl_compatibility()) {
        configGraphicsBackend = GAPI_D3D11;
    }
#endif
    int backend = configGraphicsBackend;
#if defined(_WIN32)
    if (gCLIOpts.backend != -1) { backend = gCLIOpts.backend; }
#endif

    // OpenXR is bound to the GL context, so VR requires the GL backend.
    if (gCLIOpts.vr) { backend = GAPI_GL; }

    switch (backend) {
        case GAPI_GL:
            gWindowApi = &gfx_sdl;
            gRenderApi = &gfx_opengl_api;
            gAudioApi  = &audio_sdl;
            break;
#if defined(_WIN32)
        case GAPI_D3D11:
            gWindowApi = &gfx_dxgi;
            gRenderApi = &gfx_direct3d11_api;
            gAudioApi  = &audio_sdl;
            break;
#endif
        default:
            gWindowApi = &gfx_sdl;
            gRenderApi = &gfx_opengl_api;
            gAudioApi  = &audio_sdl;
            break;
    }

    if (!gAudioApi->init()) {
        gAudioApi = &audio_null;
    }
}

// VR: is this frame a NON-GAMEPLAY screen (menu / UI), so it should be rendered as a flat frame
// on one opaque head-locked panel instead of the stereo diorama? Keyed on the game's own state
// machine (not on whether 3D happened to draw), so it's robust for hybrid screens like the
// act/course/star selector that render perspective + 3D models behind 2D text.
//   TRUE  -> flat-frame-on-panel (title, main menu, file/save select, act/star select, dialog,
//            pause + course-grid + course-complete, credits, attract-mode demo)
//   FALSE -> active gameplay: keep the existing diorama + sky dome + HUD path (unchanged)
static bool vr_frame_is_nongameplay(void) {
    extern bool gDjuiInMainMenu;        // pc/djui/djui.h
    extern bool djui_panel_is_active(void); // pc/djui/djui_panel.h - any DJUI panel open (player, dynos, pause, options)
    extern bool djui_panel_is_vr_panel(void);
    extern bool vr_is_theater_mode(void);
    // Theater mode plays the whole flat game (and its menus) on the big world-locked screen, so always use
    // the panel path while it's active.
    if (vr_is_theater_mode())              return true;
    // The VR settings menu stays in the stereo diorama (menu floats as a head-locked overlay) so you can see
    // diorama slider changes live - overrides the pause/panel checks below, which would force the flat panel.
    if (djui_panel_is_vr_panel())          return false;
    // A door / level transition (the fade / circle / star wipe) is a 2D fullscreen effect. In the stereo
    // path it lands in the small head-locked HUD overlay and only covers part of the view, so for its
    // duration render the flat panel instead, which makes the transition fill the whole view.
    if (gWarpTransition.isActive)          return true;
    if (gDjuiInMainMenu)                   return true; // title / main menu / connect / options (3D backdrop)
    if (djui_panel_is_active())            return true; // in-game menus: Player, DynOS, pause, options, etc.
    if (gVrInActSelector)                  return true; // act/course/star select (the hybrid that broke)
    if (gCurrCreditsEntry != NULL)         return true; // credits / ending
    if (gCurrDemoInput != NULL)            return true; // attract-mode demo
    // NOTE: dialog boxes (talking to characters, reading signs) intentionally stay in the stereo VR
    // view - the world keeps rendering in 3D and the text box shows as the head-locked HUD overlay, so
    // a conversation no longer yanks you out to the flat panel. (was: get_dialog_id() != DIALOG_NONE)
    if (gMenuMode != -1)                   return true; // pause star-grid / course-complete (2D fullscreen)
    if (sCurrPlayMode == PLAY_MODE_PAUSED) return true; // pause (frozen world -> flat panel for consistency)
    return false;                                       // active gameplay
}

// VR geometry anti-clip (diorama / close-up only). vr.c hands us the cyclopean eye position in
// game-camera space; we convert it to world via the renderer's camera matrix, run level collision,
// and hand back an anchor offset (meters) that nudges the whole shrunk world off any wall/floor/
// ceiling the eye pokes into. First-person is excluded by vr_anticlip_get_head_campos() returning
// false there (moving the eye off the head causes sickness). One-frame latency: the offset we set is
// applied on the next vr_begin_frame(); the correction is clamped + eased so the world never lurches.
static void vr_anticlip_resolve(void) {
    static float applied[3] = { 0.0f, 0.0f, 0.0f };
    float target[3] = { 0.0f, 0.0f, 0.0f };
    float camPos[3];
    bool active = vr_anticlip_get_head_campos(camPos) && (gCamera != NULL);

    // diagnostics (rate-limited)
    float wx = 0, wy = 0, wz = 0, fY = 0, cY = 0, dwx = 0, dwy = 0, dwz = 0;
    bool haveFloor = false, haveCeil = false;

    if (active) {
        Mat4 inv;
        if (mtxf_inverse_non_affine(inv, gCamera->mtx)) {
            // camera-space (row vector) -> world: world = camPos * inv(gCamera->mtx)
            wx = camPos[0]*inv[0][0] + camPos[1]*inv[1][0] + camPos[2]*inv[2][0] + inv[3][0];
            wy = camPos[0]*inv[0][1] + camPos[1]*inv[1][1] + camPos[2]*inv[2][1] + inv[3][1];
            wz = camPos[0]*inv[0][2] + camPos[1]*inv[1][2] + camPos[2]*inv[2][2] + inv[3][2];
            // s16 cast guard inside find_floor
            if (wx < -30000.0f) wx = -30000.0f; else if (wx > 30000.0f) wx = 30000.0f;
            if (wz < -30000.0f) wz = -30000.0f; else if (wz > 30000.0f) wz = 30000.0f;

            float scale    = vr_get_diorama_scale();
            float marginWU = 0.10f * scale; // 10 cm physical standoff in world units

            float cwx = wx, cwy = wy, cwz = wz;
            gCheckingSurfaceCollisionsForCamera = TRUE;

            struct Surface *floor = NULL, *ceil = NULL;
            fY = find_floor(cwx, cwy, cwz, &floor);
            cY = find_ceil (cwx, cwy, cwz, &ceil);
            haveFloor = (floor != NULL) && (fY > -10000.0f);
            haveCeil  = (ceil  != NULL) && (cY <  19000.0f);
            if (haveFloor && cwy < fY + marginWU) cwy = fY + marginWU;
            if (haveCeil  && cwy > cY - marginWU) cwy = cY - marginWU;
            if (haveFloor && haveCeil && fY + marginWU > cY - marginWU) cwy = 0.5f * (fY + cY);

            struct WallCollisionData wcd;
            memset(&wcd, 0, sizeof(wcd));
            wcd.x = cwx; wcd.y = cwy; wcd.z = cwz; wcd.offsetY = 0.0f; wcd.radius = marginWU;
            find_wall_collisions(&wcd);
            cwx = wcd.x; cwz = wcd.z;

            // a wall push may have slid us over a step - re-clamp the floor once
            fY = find_floor(cwx, cwy, cwz, &floor);
            if (floor != NULL && fY > -10000.0f && cwy < fY + marginWU) cwy = fY + marginWU;

            gCheckingSurfaceCollisionsForCamera = FALSE;

            dwx = cwx - wx; dwy = cwy - wy; dwz = cwz - wz;

            // world delta -> camera-space delta (rotation rows only), then -> LOCAL meters, negated
            // (move the world opposite to the eye-pushout so the fixed head ends up outside geometry).
            float dcx = dwx*gCamera->mtx[0][0] + dwy*gCamera->mtx[1][0] + dwz*gCamera->mtx[2][0];
            float dcy = dwx*gCamera->mtx[0][1] + dwy*gCamera->mtx[1][1] + dwz*gCamera->mtx[2][1];
            float dcz = dwx*gCamera->mtx[0][2] + dwy*gCamera->mtx[1][2] + dwz*gCamera->mtx[2][2];
            float invScale = 1.0f / scale;
            target[0] = -dcx * invScale;
            target[1] = -dcy * invScale;
            target[2] = -dcz * invScale;
        } else {
            active = false;
        }
    }

    // Ease toward the target (zero when inactive). Clamp per-frame change so the world drifts gently,
    // and hard-bound the total so a bad read can never throw the world far.
    for (int i = 0; i < 3; i++) {
        float d = target[i] - applied[i];
        // Escape geometry FAST so a quick camera move (close-up swinging low, a jump) can't dip the view
        // through a floor before the correction catches up; relax back to neutral slowly so the world never
        // lurches. "Escaping" = the correction is growing in magnitude (pushing the eye further out).
        float at = (target[i]  < 0.0f) ? -target[i]  : target[i];
        float aa = (applied[i] < 0.0f) ? -applied[i] : applied[i];
        bool escaping = (at > aa);
        float maxStep = !active ? 0.04f : (escaping ? 0.10f : 0.02f);
        if (d >  maxStep) d =  maxStep;
        if (d < -maxStep) d = -maxStep;
        applied[i] += d;
        if (applied[i] >  0.80f) applied[i] =  0.80f; // a bit more headroom for deep floor dips in close-up
        if (applied[i] < -0.80f) applied[i] = -0.80f;
    }
    vr_anticlip_set_offset(applied);

    if (active) {
        static int dbg = 0;
        if ((dbg++ % 30) == 0) {
            printf("[VRanticlip] eyeW=(%.0f,%.0f,%.0f) fY=%s cY=%s dW=(%.0f,%.0f,%.0f) offM=(%.3f,%.3f,%.3f)\n",
                wx, wy, wz,
                haveFloor ? "y" : "-", haveCeil ? "y" : "-",
                dwx, dwy, dwz, applied[0], applied[1], applied[2]);
        }
    }
}

void produce_interpolation_frames_and_delay(void) {
    u32 refreshRate = get_target_refresh_rate();

    gRenderingInterpolated = true;

    u32 displayRefreshRate = get_display_refresh_rate();
    bool shouldDelay = configFramerateMode != RRM_UNLIMITED;
    if (configWindow.vsync && displayRefreshRate <= refreshRate) {
        shouldDelay = false;
        refreshRate = displayRefreshRate;
    }

    // VR: xrWaitFrame inside vr_begin_frame() is the authoritative pacer. Size the interpolation count
    // from the headset's real refresh so we emit one rendered frame per headset frame, and drop coopdx's
    // monitor-locked delay so it can't fight the headset's pacing - that fight is what shows up as judder.
    if (vr_is_active()) {
        int hz = vr_get_refresh_rate();
        if (hz >= 30 && hz <= 1000) { refreshRate = (u32)hz; }
        shouldDelay = false;
    }

    f64 targetTime = sFrameTimeStart + sFrameTime;
    s32 numFramesToDraw = get_num_frames_to_draw(sFrameTimeStart, refreshRate);

    f64 curTime = clock_elapsed_f64();
    f64 loopStartTime = curTime;
    f64 expectedTime = 0;
    u16 framesDrawn = 0;
    const f64 interpFrameTime = sFrameTime / (f64) numFramesToDraw;

    // interpolate and render
    // make sure to draw at least one frame to prevent the game from freezing completely
    // (including inputs and window events) if the game update duration is greater than 33ms
    do {
        curTime = clock_elapsed_f64();
        ++framesDrawn;

        // when we know how many frames to draw, use a precise delta
        f64 idealTime = shouldDelay ? (sFrameTimeStart + interpFrameTime * framesDrawn) : curTime;
        f32 delta = clamp((idealTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gFramePercentage = clamp((curTime - sFrameTimeStart) / sFrameTime, 0.f, 1.f);
        gRenderingDelta = delta;

        vr_begin_frame();
        gfx_start_frame();
        if (!gSkipInterpolationTitleScreen) { patch_interpolations(delta); }
        send_display_list(gGfxSPTask);
        gfx_end_frame_render();
        if (vr_is_active()) {
            Gfx *vrDl = (Gfx *) gGfxSPTask->task.t.data_ptr;
            // EXPERIMENTAL first-person: when the VR toggle (F11) is on, turn on coopdx's first-person
            // camera so the game renders from Mario's head. Only act on change so we don't fight the
            // game's own first-person handling every frame.
            {
                extern void set_first_person_enabled(bool enable); // game/first_person_cam.h
                static bool sVrFpPrev = false;
                bool vrFp = vr_first_person_active();
                if (vrFp != sVrFpPrev) { set_first_person_enabled(vrFp); sVrFpPrev = vrFp; }
            }
            // D-pad up cycles the VR mode, same as F10. gPlayer1Controller only carries fresh input
            // during gameplay (a menu routes input elsewhere), and we also skip it while a panel is open,
            // so it never fights menu navigation.
            {
                extern struct Controller *gPlayer1Controller; // game/game_init.h
                extern bool djui_panel_is_active(void);        // pc/djui/djui_panel.h
                // Edge-detect off buttonDown (held state) rather than buttonPressed: buttonPressed is true
                // for a single game-logic frame and the VR loop can miss it, so the cycle dropped inputs.
                static u16 sPrevDpadUp = 0;
                u16 dpadUp = gPlayer1Controller ? (u16)(gPlayer1Controller->buttonDown & U_JPAD) : 0;
                // Only while the headset is worn - with it off the diorama presets don't apply to the flat view.
                if (dpadUp && !sPrevDpadUp && !djui_panel_is_active() && vr_is_focused()) { vr_cycle_preset(); }
                sPrevDpadUp = dpadUp;
            }
            // First-person flip cam: feed Mario's synthetic flip roll into the eye view (vr.c rolls the
            // eye + sky). Returns 0 unless the FP Flip Cam toggle is on and a flip is in progress.
            {
                extern f32 first_person_flip_roll_rad(void);                  // game/first_person_cam.h
                extern bool first_person_flip_is_side(struct MarioState *m);  // side flip rolls; others pitch
                vr_set_flip_roll(first_person_flip_roll_rad());
                vr_set_flip_side(first_person_flip_is_side(&gMarioStates[0]));
            }
            // Tabletop: back out of the C-up look-around state so it can't freeze movement in the diorama.
            { extern void first_person_exit_lookaround_for_tabletop(void); first_person_exit_lookaround_for_tabletop(); }
            if (vr_frame_is_nongameplay()) {
                // FLATSCREEN-ON-A-PANEL: render the WHOLE flat frame (2D + 3D, game projection,
                // no diorama, no 2D/3D split) once into the panel swapchain and submit it as the
                // sole large opaque head-locked quad. Every menu/UI screen looks like the desktop
                // game, floating on a VR screen.
                vr_set_panel_mode(true);
                // Every menu is world-locked so you can turn your head to look around it. The crop is
                // driven by how the screen is laid out: left-docked menus (Mods, lobbies, Player, DynOS)
                // and any non-panel screen (act/star select, dialogs) use the full 16:9 frame so nothing
                // is cut; centered panels (title, options, pause) use the 4:3 region so there's no void.
                {
                    extern bool djui_panel_active_is_left_docked(void); // pc/djui/djui_panel.h
                    extern bool djui_panel_is_active(void);
                    vr_set_panel_full_frame(djui_panel_active_is_left_docked() || !djui_panel_is_active());
                }
                if (vr_begin_panel()) {
                    gfx_run_dl_vr_panel(vrDl, vr_overlay_width(), vr_overlay_height());
                    vr_end_panel();
                }
                // Theater backdrop: mode 1 (Panoramic) renders the user panorama into the backdrop swapchain so
                // vr_submit composites it behind the screen. Black (0) and Model (2) skip this -> plain black.
                {
                    extern bool vr_render_backdrop_pano(void);
                    extern int  vr_get_theater_bg(void);
                    if (vr_is_theater_mode() && vr_get_theater_bg() == 1) { vr_render_backdrop_pano(); }
                }
                vr_submit(); // panel (+ theater backdrop) layers
            } else {
                // ACTIVE GAMEPLAY: existing stereo diorama + world-locked sky dome + head-locked
                // HUD overlay (unchanged).
                // Geometry anti-clip: read this frame's eye + camera, run collision, set the anchor
                // offset for the next frame so the eye can't poke through walls/floors/ceilings.
                vr_anticlip_resolve();
                int eyes = vr_eye_count();
                for (int e = 0; e < eyes; e++) {
                    if (vr_begin_eye(e)) {
                        gfx_run_dl_vr_eye(vrDl, vr_eye_viewproj(e), vr_sky_viewproj(e), vr_eye_width(e), vr_eye_height(e));
                        vr_end_eye(e);
                    }
                }
                // The sky is now a 3D sphere rendered INSIDE the eye (world-locked). Only the
                // head-locked HUD remains as an overlay layer. gVrFrameHasPerspective still routes
                // 2D: post-3D 2D -> HUD; when no world exists, ALL 2D -> head-locked HUD (menus/title).
                extern bool gVrSeenPerspective, gVrFrameHasPerspective;
                gVrFrameHasPerspective = gVrSeenPerspective;
                if (vr_begin_overlay(false)) { // HUD / menus (head-locked)
                    gfx_run_dl_vr_overlay(vrDl, vr_overlay_width(), vr_overlay_height(), false);
                    vr_end_overlay(false);
                }
                vr_submit();
            }
        }
        gfx_display_frame();

        // delay if our framerate is capped
        if (shouldDelay) {
            expectedTime += (targetTime - curTime) / (f64) numFramesToDraw;
            f64 now = clock_elapsed_f64();
            f64 elapsedTime = now - loopStartTime;
            f64 delay = (expectedTime - elapsedTime);
            if (delay > 0.0) {
                precise_delay_f64(delay);
            }
        }

        sDrawnFrames++;
        if (shouldDelay) { numFramesToDraw--; }
    } while ((curTime = clock_elapsed_f64()) < targetTime && numFramesToDraw > 0);

    // compute and update the frame rate every second
    if ((curTime = clock_elapsed_f64()) >= sFpsTimeLast + 1.0) {
        compute_fps(curTime);
    }

    // advance frame start time
    if (curTime > sFrameTimeStart + 2 * sFrameTime) {
        sFrameTimeStart = curTime;
    } else {
        sFrameTimeStart += sFrameTime;
    }

    gRenderingInterpolated = false;
}

// It's just better to have this off the stack, Because the size isn't small.
// It also may help static analysis and bug catching.
static s16 sAudioBuffer[SAMPLES_HIGH * 2 * 2] = { 0 };

inline static void buffer_audio(void) {
    bool shouldMute = (configMuteFocusLoss && !gWindowApi->has_focus()) || (gMasterVolume == 0);
    if (!shouldMute) {
        set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * (f32)gLuaVolumeLevel / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_SFX,   (f32)configSfxVolume / 127.0f * (f32)gLuaVolumeSfx / 127.0f);
        set_sequence_player_volume(SEQ_PLAYER_ENV,   (f32)configEnvVolume / 127.0f * (f32)gLuaVolumeEnv / 127.0f);
    }

    int samplesLeft = gAudioApi->buffered();
    u32 numAudioSamples = samplesLeft < gAudioApi->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
    for (s32 i = 0; i < 2; i++) {
        create_next_audio_buffer(sAudioBuffer + i * (numAudioSamples * 2), numAudioSamples);
    }

    if (!shouldMute) {
        for (u16 i=0; i < ARRAY_COUNT(sAudioBuffer); i++) {
            sAudioBuffer[i] *= gMasterVolume;
        }
        gAudioApi->play((u8 *)sAudioBuffer, 2 * numAudioSamples * 4);
    }
}

void *audio_thread(UNUSED void *arg) {
    // As long as we have an audio api and that we're threaded, Loop.
    while (gAudioApi) {
        f64 curTime = clock_elapsed_f64();

        // Buffer the audio.
        lock_mutex(&gAudioThread);
        buffer_audio();
        unlock_mutex(&gAudioThread);

        // Delay till the next frame for smooth audio at the correct speed.
        // delay
        f64 targetDelta = 1.0 / (f64)FRAMERATE;
        f64 now = clock_elapsed_f64();
        f64 actualDelta = now - curTime;
        if (actualDelta < targetDelta) {
            f64 delay = ((targetDelta - actualDelta) * 1000.0);
            gWindowApi->delay((u32)delay);
        }
    }

    // Exit the thread if our loop breaks.
    exit_thread();

    return NULL;
}

void produce_one_frame(void) {
    CTX_EXTENT(CTX_NETWORK, network_update);

    CTX_EXTENT(CTX_INTERP, patch_interpolations_before);

    CTX_EXTENT(CTX_GAME_LOOP, game_loop_one_iteration);

    CTX_EXTENT(CTX_SMLUA, smlua_update);

    // If we aren't threaded
    if (gAudioThread.state == INVALID) {
        CTX_EXTENT(CTX_AUDIO, buffer_audio);
    }

    CTX_EXTENT(CTX_RENDER, produce_interpolation_frames_and_delay);
}

// used for rendering 2D scenes fullscreen like the loading or crash screens
void produce_one_dummy_frame(void (*callback)(), u8 clearColorR, u8 clearColorG, u8 clearColorB) {
    // measure frame start time
    f64 frameStart = clock_elapsed_f64();
    f64 targetFrameTime = 1.0 / 60.0; // update at 60fps

    // start frame
    gfx_start_frame();
    config_gfx_pool();
    init_render_image();
    create_dl_ortho_matrix();
    djui_gfx_displaylist_begin();

    // fix scaling issues
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&gViewportFullscreen));
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, BORDER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - BORDER_HEIGHT);

    // clear screen
    create_dl_translation_matrix(MENU_MTX_PUSH, GFX_DIMENSIONS_FROM_LEFT_EDGE(0), 240.f, 0.f);
    create_dl_scale_matrix(MENU_MTX_NOPUSH, (GFX_DIMENSIONS_ASPECT_RATIO * SCREEN_HEIGHT) / 130.f, 3.f, 1.f);
    gDPSetEnvColor(gDisplayListHead++, clearColorR, clearColorG, clearColorB, 0xFF);
    gSPDisplayList(gDisplayListHead++, dl_draw_text_bg_box);
    gSPPopMatrix(gDisplayListHead++, G_MTX_MODELVIEW);

    // call the callback
    callback();

    // render frame
    djui_gfx_displaylist_end();
    end_master_display_list();
    alloc_display_list(0);
    gfx_run((Gfx*) gGfxSPTask->task.t.data_ptr); // send_display_list
    display_and_vsync();

    // delay to go easy on the cpu
    f64 frameEnd = clock_elapsed_f64();
    f64 elapsed = frameEnd - frameStart;
    f64 remaining = targetFrameTime - elapsed;
    if (remaining > 0) {
        gWindowApi->delay((u32)(remaining * 1000.0));
    }

    gfx_end_frame();
}

void audio_shutdown(void) {
    if (gAudioApi) {
        if (gAudioApi->shutdown) gAudioApi->shutdown();
        gAudioApi = NULL;
    }
}

void game_deinit(void) {
    if (gGameInited) { configfile_save(configfile_name()); }
    controller_shutdown();
    audio_shutdown();
    vr_shutdown();
    network_shutdown(true, true, false, false);
    smlua_text_utils_shutdown();
    smlua_shutdown();
    mods_shutdown();
    djui_shutdown();
    gfx_shutdown();
    gGameInited = false;
}

void game_exit(void) {
    LOG_INFO("exiting cleanly");
    game_deinit();
    exit(0);
}

void* main_game_init(UNUSED void* dummy) {
    // load language
    if (!djui_language_init(configLanguage)) { snprintf(configLanguage, MAX_CONFIG_STRING, "%s", ""); }

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading"));
    dynos_gfx_init();
    enable_queued_dynos_packs();
    sync_objects_init_system();

    if (gCLIOpts.network != NT_SERVER && !gCLIOpts.skipUpdateCheck) {
        check_for_updates();
    }

    LOADING_SCREEN_MUTEX(loading_screen_set_segment_text("Loading ROM Assets"));
    rom_assets_load();
    smlua_text_utils_init();

    mods_init();
    enable_queued_mods();
    LOADING_SCREEN_MUTEX(
        gCurrLoadingSegment.percentage = 0;
        loading_screen_set_segment_text("Starting Game");
    );

    audio_init();
    sound_init();
    network_player_init();
    mumble_init();

    gGameInited = true;
    return NULL;
}

int main(int argc, char *argv[]) {
    // handle terminal arguments
    if (!parse_cli_opts(argc, argv)) { return 0; }

    // One-step VR: with no flag, auto-detect a connected headset so the SAME exe runs in VR
    // when a headset is present and flat otherwise. --vr forces it on, --novr forces it off.
    if (!gCLIOpts.vr && !gCLIOpts.novr && vr_headset_present()) {
        gCLIOpts.vr = true;
        printf("VR headset detected -- enabling VR.\n");
    }

    if (gCLIOpts.vr) { vr_request_enable(); }

#ifdef _WIN32
    // handle Windows console
    if (gCLIOpts.console || gCLIOpts.headless) {
        SetConsoleOutputCP(CP_UTF8);
    } else {
        FreeConsole();
        freopen("NUL", "w", stdout);
    }
#endif

#ifdef _WIN32
    if (gCLIOpts.savePath[0]) {
        char portable_path[SYS_MAX_PATH] = {};
        sys_windows_short_path_from_mbs(portable_path, SYS_MAX_PATH, gCLIOpts.savePath);
        fs_init(portable_path);
    } else {
        fs_init(sys_user_path());
    }
#else
    fs_init(gCLIOpts.savePath[0] ? gCLIOpts.savePath : sys_user_path());
#endif

    configfile_load();

    // In VR the headset's own frame loop (xrWaitFrame, inside vr_begin_frame) paces every frame. Desktop
    // -window vsync would add a SECOND pacer locked to the monitor's refresh, and when the monitor and
    // headset run at different rates the two clocks beat against each other into visible judder in the
    // headset. Turn the desktop mirror's vsync off up front so only the headset paces the frame.
    if (vr_is_requested()) { configWindow.vsync = 0; }

    legacy_folder_handler();

    select_graphics_backend();

    // create the window almost straight away
    if (!gGfxInited) {
        gfx_init(gWindowApi, gRenderApi, TITLE);
        gWindowApi->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up,
            keyboard_on_text_input, keyboard_on_text_editing);
        gWindowApi->set_scroll_callback(mouse_on_scroll);
    }

    // render the rom setup screen
    if (!main_rom_handler()) {
        if (!gCLIOpts.hideLoadingScreen) {
            render_rom_setup_screen(); // holds the game load until a valid rom is provided
        } else {
            printf("ERROR: could not find valid vanilla us sm64 rom in game's user folder\n");
            return 0;
        }
    }

    // start the thread for setting up the game
    bool threadSuccess = false;
    if (!gCLIOpts.hideLoadingScreen && !gCLIOpts.headless) {
        if (init_thread_handle(&gLoadingThread, main_game_init, NULL, NULL, 0) == 0) {
            render_loading_screen(); // render the loading screen while the game is setup
            threadSuccess = true;
            destroy_mutex(&gLoadingThread);
        }
    }
    if (!threadSuccess) {
        main_game_init(NULL); // failsafe incase threading doesn't work
    }

    // initialize sm64 data and controllers
    thread5_game_loop(NULL);

    // Initialize the audio thread if possible.
    // init_thread_handle(&gAudioThread, audio_thread, NULL, NULL, 0);

    loading_screen_reset();

    // initialize djui
    djui_init();
    djui_unicode_init();
    djui_init_late();
    djui_console_message_dequeue();

    show_update_popup();

    if (can_update_game()) {
        djui_open_update_panel();
    }

    // initialize network
    if (gCLIOpts.network == NT_CLIENT) {
        network_set_system(NS_SOCKET);
        snprintf(gGetHostName, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", gCLIOpts.joinIp);
        configJoinPort = gCLIOpts.networkPort;
        network_init(NT_CLIENT, false);
    } else if (gCLIOpts.network == NT_SERVER || gCLIOpts.coopnet) {
        if (gCLIOpts.network == NT_SERVER) {
            configNetworkSystem = NS_SOCKET;
            configHostPort = gCLIOpts.networkPort;
        } else {
            configNetworkSystem = NS_COOPNET;
            snprintf(configPassword, MAX_CONFIG_STRING, "%s", gCLIOpts.coopnetPassword);
        }

        // horrible, hacky fix for mods that access marioObj straight away
        // best fix: host with the standard main menu method
        static struct Object sHackyObject = { 0 };
        gMarioStates[0].marioObj = &sHackyObject;

        extern void djui_panel_do_host(bool reconnecting, bool playSound);
        djui_panel_do_host(NULL, false);
    } else {
        network_init(NT_NONE, false);
    }

    // main loop
    while (true) {
        debug_context_reset();
        CTX_BEGIN(CTX_TOTAL);
        gWindowApi->main_loop(produce_one_frame);
#ifdef DISCORD_SDK
        discord_update();
#endif
        mumble_update();
#ifdef DEBUG
        fflush(stdout);
        fflush(stderr);
#endif
        CTX_END(CTX_TOTAL);

#ifdef DEVELOPMENT
        djui_ctx_display_update();
#endif
        djui_lua_profiler_update();
    }

    return 0;
}
