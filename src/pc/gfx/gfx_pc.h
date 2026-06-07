#ifndef GFX_PC_H
#define GFX_PC_H

#include "types.h"
#include "pc/gfx/gfx.h"

enum ShaderFlag {
    SHADER_FLAG_HUE,
    SHADER_FLAG_SATURATION,
    SHADER_FLAG_BRIGHTNESS,
    SHADER_FLAG_CONTRAST,
    SHADER_FLAG_EXPOSURE,
    SHADER_FLAG_DITHERING,
    SHADER_FLAG_POSTERIZATION,
    SHADER_FLAG_SCANLINES,
    SHADER_FLAG_MAX
};

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

extern Vec3f gLightingDir;
extern Color gLightingColor[2];
extern Color gVertexColor;
extern Color gFogColor;
extern f32 gFogIntensity;

extern bool gFullbright;

extern int gShaderFlags[SHADER_FLAG_MAX];
extern f32 gDefaultShaderFlagValues[SHADER_FLAG_MAX];
extern f32 gShaderFlagValues[SHADER_FLAG_MAX];
extern bool gShaderFlagsEnabled;

#ifdef __cplusplus
extern "C" {
#endif

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title);
struct GfxRenderingAPI *gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_run_dl_vr_eye(Gfx *commands, const float eyeViewProj[16], const float skyViewProj[16], int eyeW, int eyeH); // VR: render one eye
void gfx_run_dl_vr_overlay(Gfx *commands, int w, int h, bool sky); // VR: render 2D overlay (sky or HUD)
void gfx_end_frame_render(void);
void gfx_display_frame(void);
void gfx_end_frame(void);
void gfx_shutdown(void);
void gfx_pc_precomp_shader(uint32_t rgb1, uint32_t alpha1, uint32_t rgb2, uint32_t alpha2, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
