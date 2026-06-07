#include <PR/ultratypes.h>

#include "area.h"
#include "engine/math_util.h"
#include "geo_misc.h"
#include "gfx_dimensions.h"
#include "level_update.h"
#include "memory.h"
#include "save_file.h"
#include "segment2.h"
#include "sm64.h"
#include "hud.h"
#include "geo_commands.h"
#include "hardcoded.h"
#include "skybox.h"

/**
 * @file skybox.c
 *
 * Implements the skybox background.
 *
 * It's not exactly a sky"box": it's more of a sky tilemap. It renders a 3x3 grid of 32x32 pieces of the
 * whole skybox image, starting from the top left based on the camera's rotation. A skybox image has 64
 * unique 32x32 tiles, with the first two columns duplicated for a total of 80.
 *
 * The tiles are mapped to world space such that 2 full tiles fit on the screen, for a total of
 * 8 tiles around the full 360 degrees. Each tile takes up 45 degrees of the camera's field of view, and
 * the code draws 3 tiles or 135 degrees of the skybox in a frame. But only 2 tiles, or 90 degrees, can
 * fit on-screen at a time.
 *
 * @bug FOV is handled strangely by the code. It is used to scale and rotate the skybox, when really it
 * should probably only be used to calculate the distance drawn from the center of the looked-at tile.
 * But since the game always sets it to 90 degrees, the skybox always scales and rotates the same,
 * regardless of the camera's actual FOV. So even if the camera's FOV is 10 degrees the game draws a
 * full 90 degrees of the skybox, which makes the sky look really far away.
 *
 * @bug Skyboxes unnecessarily repeat the first 2 columns when they could just wrap the col index.
 * Although, the wasted space is only about 128 bytes for each image.
 */

/**
 * Describes the position, tiles, and orientation of the skybox image.
 *
 * Describes the scaled x and y offset into the tilemap, based on the yaw and pitch.  Computes the
 * upperLeftTile index into the skybox's tile list using scaledX and scaledY. See get_top_left_tile_idx.
 *
 * The skybox is always drawn behind everything, because in the level's geo script, the skybox is drawn
 * first, in a display list with the Z buffer disabled
 */
struct Skybox {
    /// The camera's yaw, from 0 to (M_PI*2), which maps to 0 to 360 degrees
    f32 yaw;
    /// The camera's pitch, which is bounded by +-(M_PI/2), which maps to -90 to 90 degrees
    f32 pitch;
    /// The skybox's X position in world space
    f32 scaledX;
    /// The skybox's Y position in world space
    f32 scaledY;
};

struct Skybox sSkyBoxInfo[2];

typedef const Texture *const SkyboxTexture[80];

extern u8 gRenderingInterpolated;

s8 gReadOnlyBackground;
s8 gOverrideBackground = -1;
Color gSkyboxColor = { 255, 255, 255 };

extern SkyboxTexture bbh_skybox_ptrlist;
extern SkyboxTexture bitdw_skybox_ptrlist;
extern SkyboxTexture bitfs_skybox_ptrlist;
extern SkyboxTexture bits_skybox_ptrlist;
extern SkyboxTexture ccm_skybox_ptrlist;
extern SkyboxTexture cloud_floor_skybox_ptrlist;
extern SkyboxTexture clouds_skybox_ptrlist;
extern SkyboxTexture ssl_skybox_ptrlist;
extern SkyboxTexture water_skybox_ptrlist;
extern SkyboxTexture wdw_skybox_ptrlist;
Texture* gCustomSkyboxPtrList[80] = { NULL };

SkyboxTexture *sSkyboxTextures[10] = {
    &water_skybox_ptrlist,
    &bitfs_skybox_ptrlist,
    &wdw_skybox_ptrlist,
    &cloud_floor_skybox_ptrlist,
    &ccm_skybox_ptrlist,
    &ssl_skybox_ptrlist,
    &bbh_skybox_ptrlist,
    &bitdw_skybox_ptrlist,
    &clouds_skybox_ptrlist,
    &bits_skybox_ptrlist,
};

/**
 * The skybox color mask.
 * The final color of each pixel is computed from the bitwise AND of the color and the texture.
 */
u8 sSkyboxColors[][3] = {
    { 0x50, 0x64, 0x5A },
    { 0xFF, 0xFF, 0xFF },
};

/**
 * Constant used to scale the skybox horizontally to a multiple of the screen's width
 */
#define SKYBOX_WIDTH (4 * SCREEN_WIDTH)
/**
 * Constant used to scale the skybox vertically to a multiple of the screen's height
 */
#define SKYBOX_HEIGHT (4 * SCREEN_HEIGHT)

/**
 * The tile's width in world space.
 * By default, two full tiles can fit in the screen.
 */
#define SKYBOX_TILE_WIDTH (SCREEN_WIDTH / 2)
/**
 * The tile's height in world space.
 * By default, two full tiles can fit in the screen.
 */
#define SKYBOX_TILE_HEIGHT (SCREEN_HEIGHT / 2)

/**
 * The horizontal length of the skybox tilemap in tiles.
 */
#define SKYBOX_COLS (10)
/**
 * The vertical length of the skybox tilemap in tiles.
 */
#define SKYBOX_ROWS (8)

static u16 sSkyboxTileNumX = 5;
static const u16 sSkyboxTileNumY = 3; // Shouldn't need to change this

struct GrowingArray *gBackgroundSkyboxVerts = NULL;

// VR: the sky is rendered as a 3D sphere INSIDE the eye (the projection layer is
// world-locked, so the sphere inherits that and fully wraps with no black poles).
Gfx *gVrSkyDomeGfx = NULL; // sphere display list, rebuilt each game frame
u32  gVrSkyDomeFrame = 0;  // gGlobalTimer when built (staleness guard for the eye render)

// Build the full 8x8 panorama as quads on a unit-ish sphere (radius in s16 range).
// Rendered with a rotation-only view-proj (vr.c sSkyVP) so it sits at infinity.
static Gfx *build_skybox_sphere_vr(s8 player, s8 background, s8 colorIndex) {
    extern u32 gGlobalTimer;
    // SMOOTH CLEAR-ZENITH DOME: parametric (ring x azimuth) sphere.
    //   DOME_AZ  = azimuth segments (16-gon -> NO octagonal pole pinch; 8 was the pinch).
    //   ringEl[] = latitude boundaries top->bottom (deg); upper rings densify toward the zenith.
    // The cloud->clear smoothing is a per-vertex SHADE-ALPHA ramp + a combiner LERP toward
    // ENVIRONMENT (the only guaranteed clear-sky color; row 0 is 8 distinct horizontal panorama
    // tiles, NOT a vertical gradient, so reusing its V cannot give a clean fade and would break
    // the other skyboxes). ENV alpha stays 255 -> output stays opaque (no black-eye blend bug).
    #define DOME_AZ        16
    #define DOME_SUBV      3       // fine sub-rings per panorama row (tile V is SPLIT across them -> NO vertical repeat)
    #define FADE_START_DEG 30.0f   // *** TUNABLE: how low the clouds sit *** clouds full below this
    #define FADE_END_DEG   60.0f   // fully clear (ENV) above this -> NO clouds at the zenith
    const s32 NRINGS = 8 * DOME_SUBV; // 8 panorama rows x sub-rings

    Gfx *dl = alloc_display_list((8 + NRINGS * DOME_AZ * 8) * sizeof(Gfx)); // 8 fixed + quads * 8 Gfx/quad
    if (dl == NULL) { return NULL; }
    Gfx *g = dl;
    gSPDisplayList(g++, dl_skybox_begin);
    gSPDisplayList(g++, dl_skybox_tile_tex_settings);
    // Cloud->clear LERP: RGB = (TEXEL0 - ENV) * shade.a + ENV ; A = ENV.a (=255, opaque).
    // shade.a = 1 -> pure panorama texel ; shade.a = 0 -> ENV clear-sky color. Deterministic per
    // eye (replaces the inherited FADEA combiner; SHADE is faithful with the dome's lighting off).
    gDPSetCombineLERP(g++, TEXEL0, ENVIRONMENT, SHADE_ALPHA, ENVIRONMENT, 0, 0, 0, ENVIRONMENT,
                           TEXEL0, ENVIRONMENT, SHADE_ALPHA, ENVIRONMENT, 0, 0, 0, ENVIRONMENT);
    gDPSetTextureFilter(g++, G_TF_BILERP); // bilinear-smooth the low-res 32x32 tiles (clouds were point-sampled/pixelated)

    const f32 R        = 1000.0f;
    const f32 DEG2RAD  = (f32)(M_PI / 180.0f);
    const f32 camYaw   = sSkyBoxInfo[player].yaw;   // game-camera yaw: world-anchors the dome
    const f32 camPitch = sSkyBoxInfo[player].pitch; // game-camera pitch: the diorama-only term the dome lacked
    const s32 subPerCol = DOME_AZ / 8;              // az segments per panorama column
    // 8 panorama rows (22.5 deg each); every row is split into DOME_SUBV fine sub-rings with the
    // tile's V SPLIT across them -> drawn ONCE per row (no vertical repeat/stretch) while the
    // per-vertex smoothstep alpha stays finely sampled (smooth cloud->clear fade, no banding).
    for (s32 ring = 0; ring < NRINGS; ring++) {
        const s32 row = ring / DOME_SUBV;
        const s32 sv  = ring % DOME_SUBV;
        const s32 srcRow = row;
        const f32 rowTopDeg = 90.0f - (f32) row      * 22.5f;
        const f32 rowBotDeg = 90.0f - (f32)(row + 1) * 22.5f;
        const f32 elTopDeg = rowTopDeg + (rowBotDeg - rowTopDeg) * ((f32) sv      / (f32) DOME_SUBV);
        const f32 elBotDeg = rowTopDeg + (rowBotDeg - rowTopDeg) * ((f32)(sv + 1) / (f32) DOME_SUBV);
        const f32 el0 = elTopDeg * DEG2RAD;  // top latitude of this sub-ring
        const f32 el1 = elBotDeg * DEG2RAD;  // bottom latitude
        const s32 vTop = sv       * (31 << 5) / DOME_SUBV; // V split -> the tile is drawn ONCE per row
        const s32 vBot = (sv + 1) * (31 << 5) / DOME_SUBV;
        #define SKY_FADE_A(elDeg) ({ \
            f32 _t = ((elDeg) - FADE_START_DEG) / (FADE_END_DEG - FADE_START_DEG); \
            if (_t < 0.0f) { _t = 0.0f; } if (_t > 1.0f) { _t = 1.0f; } \
            (u8)(255.0f * (1.0f - _t * _t * (3.0f - 2.0f * _t))); })
        const u8 aTop = SKY_FADE_A(elTopDeg);
        const u8 aBot = SKY_FADE_A(elBotDeg);
        #undef SKY_FADE_A
        for (s32 col = 0; col < DOME_AZ; col++) {
            s32 panCol = col / subPerCol;                  // which of the 8 panorama columns (subPerCol hoisted above)
            if (panCol < 0) { panCol = 0; }
            if (panCol > 7) { panCol = 7; }
            // Split the tile's U across its sub-segments so the panorama is NOT repeated/squished
            // (the "blocky sky": each tile had been drawn full-width on EVERY sub-segment -> the
            // 360 panorama wrapped in half the space). sub 0 = left half of the tile, sub 1 = right.
            const s32 subIdx = col % subPerCol;
            const s32 uLeft  = subIdx       * (31 << 5) / subPerCol;
            const s32 uRight = (subIdx + 1) * (31 << 5) / subPerCol;
            s32 tileIndex = srcRow * SKYBOX_COLS + panCol;
            if (tileIndex < 0)  { tileIndex = 0;  }
            if (tileIndex > 79) { tileIndex = 79; }
            const Texture *tex = (background < 0 || background >= 10)
                ? gCustomSkyboxPtrList[tileIndex]
                : (*(SkyboxTexture *) segmented_to_virtual(sSkyboxTextures[background]))[tileIndex];

            f32 cr = gSkyboxColor[0] / 255.0f;
            f32 cg = gSkyboxColor[1] / 255.0f;
            f32 cb = gSkyboxColor[2] / 255.0f;
            u8 *color = sSkyboxColors[colorIndex];
            gDPSetEnvColor(g++, color[0] * cr, color[1] * cg, color[2] * cb, 255);

            // Game-camera yaw added to the geometric azimuth (world-anchors the dome, like the flat
            // skybox scroll). camYaw/camPitch hoisted above; el0/el1/vTop/vBot/aTop/aBot computed per
            // sub-ring above; the camera PITCH is applied inside SKY_SPH as an X-rotation.
            f32 az0 = ((f32) col)       / (f32) DOME_AZ * 2.0f * M_PI + camYaw; // left  longitude (+ camera yaw)
            f32 az1 = ((f32)(col + 1))  / (f32) DOME_AZ * 2.0f * M_PI + camYaw; // right longitude (+ camera yaw)

            Vtx *v = alloc_display_list(4 * sizeof(Vtx));
            if (v == NULL) { continue; }
            // -Z forward base dir, then rotate about the X (right) axis by the CAMERA PITCH (FIX #1
            // pitch term): rotation by -camPitch so camera-down (camPitch<0) lifts the dome horizon
            // up to match the pitched diorama. If the horizon ends up tilted the wrong way, flip the
            // two sin signs.
            // az/camYaw azimuth offset + camPitch X-rotation are PRESERVED byte-for-byte. Only the
            // shade alpha (last arg) now carries the cloud->clear ramp instead of constant 255.
            #define SKY_SPH(idx, az, el, u, vv, a) do { \
                f32 _x = R * cosf(el) * sinf(az); \
                f32 _y = R * sinf(el); \
                f32 _z = -R * cosf(el) * cosf(az); \
                f32 _y2 =  _y * cosf(camPitch) + _z * sinf(camPitch); \
                f32 _z2 = -_y * sinf(camPitch) + _z * cosf(camPitch); \
                make_vertex(v, idx, (s16) _x, (s16) _y2, (s16) _z2, (u), (vv), 255, 255, 255, (a)); \
            } while (0)
            // vTop/vBot are the per-sub-ring V split (hoisted above) -> the tile is drawn ONCE per
            // row, finely tessellated, with NO vertical repeat/stretch.
            SKY_SPH(0, az0, el0, uLeft,  vTop, aTop);  // top-left
            SKY_SPH(1, az0, el1, uLeft,  vBot, aBot);  // bottom-left
            SKY_SPH(2, az1, el1, uRight, vBot, aBot);  // bottom-right
            SKY_SPH(3, az1, el0, uRight, vTop, aTop);  // top-right
            #undef SKY_SPH

            gLoadBlockTexture(g++, 32, 32, G_IM_FMT_RGBA, tex);
            gSPVertex(g++, VIRTUAL_TO_PHYSICAL(v), 4, 0);
            gSPDisplayList(g++, dl_draw_quad_verts_0123);
        }
    }
    gSPDisplayList(g++, dl_skybox_end);
    gSPEndDisplayList(g);
    gVrSkyDomeFrame = gGlobalTimer;
    #undef DOME_AZ
    #undef DOME_SUBV
    #undef FADE_START_DEG
    #undef FADE_END_DEG
    return dl;
}

/**
 * Convert the camera's yaw into an x position into the scaled skybox image.
 *
 * fov is always 90 degrees, set in draw_skybox_facing_camera.
 *
 * The calculation performed is equivalent to (360 / fov) * (yaw / 65536) * SCREEN_WIDTH
 * in other words: (the number of fov-sized parts of the circle there are) *
 *                 (how far is the camera rotated from 0, scaled 0 to 1)   *
 *                 (the screen width)
 */
f32 calculate_skybox_scaled_x(s8 player, f32 fov) {
    f32 yaw = sSkyBoxInfo[player].yaw;

    //! double literals are used instead of floats
    f32 scaledX = SCREEN_WIDTH * 180.0 * yaw / (fov * M_PI);

    if (scaledX > SKYBOX_WIDTH) {
        scaledX -= (s32) scaledX / SKYBOX_WIDTH * SKYBOX_WIDTH;
    }
    return SKYBOX_WIDTH - scaledX;
}

/**
 * Convert the camera's pitch into a y position in the scaled skybox image.
 *
 * fov may have been used in an earlier version, but the developers changed the function to always use
 * 90 degrees.
 */
f32 calculate_skybox_scaled_y(s8 player, UNUSED f32 fov) {
    // Convert pitch to degrees. Pitch is bounded between -90 (looking down) and 90 (looking up).
    f32 pitchInDegrees = sSkyBoxInfo[player].pitch * 180.0 / M_PI;

    // Scale by 360 / fov
    f32 degreesToScale = 360.0f * pitchInDegrees / 90.0;

    // Since pitch can be negative, and the tile grid starts 1 octant above the camera's focus, add
    // 5 octants to the y position
    f32 scaledY = degreesToScale + 5 * SKYBOX_TILE_HEIGHT;

    if (scaledY > SKYBOX_HEIGHT) {
        scaledY = SKYBOX_HEIGHT;
    }
    if (scaledY < SCREEN_HEIGHT) {
        scaledY = SCREEN_HEIGHT;
    }
    return scaledY;
}

/**
 * Generates vertices for the skybox tile.
 *
 * @param tileIndex The index into the 32x32 sections of the whole skybox image. The index is converted
 *                  into an x and y by modulus and division by SKYBOX_COLS. x and y are then scaled by
 *                  SKYBOX_TILE_WIDTH to get a point in world space.
 */
Vtx *make_skybox_rect(s32 tileRow, s32 tileCol, s32 row, s32 col) {
    u16 index = row * sSkyboxTileNumX + col;
    Vtx *verts;
    if (gRenderingInterpolated) {
        verts = gBackgroundSkyboxVerts->buffer[index];
    } else {
        verts = alloc_display_list(4 * sizeof(*verts));
        gBackgroundSkyboxVerts->buffer[index] = verts;
    }

    f32 x = tileCol * SKYBOX_TILE_WIDTH;
    f32 y = SKYBOX_HEIGHT - tileRow / SKYBOX_COLS * SKYBOX_TILE_HEIGHT;

    if (verts != NULL) {
        make_vertex(verts, 0, x, y, -1, 0, 0, 255, 255, 255, 255);
        make_vertex(verts, 1, x, y - SKYBOX_TILE_HEIGHT, -1, 0, 31 << 5, 255, 255, 255, 255);
        make_vertex(verts, 2, x + SKYBOX_TILE_WIDTH, y - SKYBOX_TILE_HEIGHT, -1, 31 << 5, 31 << 5, 255, 255, 255, 255);
        make_vertex(verts, 3, x + SKYBOX_TILE_WIDTH, y, -1, 31 << 5, 0, 255, 255, 255, 255);
    }
    return verts;
}

/**
 * Draws a 3x3 grid of 32x32 sections of the original skybox image.
 * The row and column are converted into an index into the skybox's tile list, which is then drawn in
 * world space so that the tiles will rotate with the camera.
 */
void draw_skybox_tile_grid(Gfx **dlist, s8 background, s8 player, s8 colorIndex) {
    s32 row;
    s32 col;

    s32 colOffset = (sSkyboxTileNumX / 2) - 1;
    for (row = 0; row < sSkyboxTileNumY; row++) {
        for (col = 0; col < sSkyboxTileNumX; col++) {
            s32 tileRow = (s32) (((SKYBOX_HEIGHT - sSkyBoxInfo[player].scaledY) / SKYBOX_TILE_HEIGHT) + row) * SKYBOX_COLS;
            s32 tileColTmp = ((floor(sSkyBoxInfo[player].scaledX / SKYBOX_TILE_WIDTH) + col) - colOffset);
            s32 tileCol = tileColTmp;
            if (tileCol >= SKYBOX_ROWS) { tileCol -= SKYBOX_ROWS; }
            if (tileCol < 0) { tileCol += SKYBOX_ROWS; }
            s32 tileIndex = tileRow + tileCol;

            // UGLY HACK: if the camera moves weird after a level transition this can go too high
            if (tileIndex < 0)  { tileIndex = 0;  }
            if (tileIndex > 79) { tileIndex = 79; }
            const Texture* texture = NULL;
            if (background < 0 || background >= 10) {
                texture = gCustomSkyboxPtrList[tileIndex];
            } else {
                texture = (*(SkyboxTexture *) segmented_to_virtual(sSkyboxTextures[background]))[tileIndex];
            }

            f32 r = gSkyboxColor[0] / 255.0f;
            f32 g = gSkyboxColor[1] / 255.0f;
            f32 b = gSkyboxColor[2] / 255.0f;
            u8 *color = sSkyboxColors[colorIndex];
            gDPSetEnvColor((*dlist)++, color[0] * r, color[1] * g, color[2] * b, 255);

            Vtx *vertices = make_skybox_rect(tileRow, tileColTmp, row, col);

            gLoadBlockTexture((*dlist)++, 32, 32, G_IM_FMT_RGBA, texture);
            gSPVertex((*dlist)++, VIRTUAL_TO_PHYSICAL(vertices), 4, 0);
            gSPDisplayList((*dlist)++, dl_draw_quad_verts_0123);
        }
    }
}

void *create_skybox_ortho_matrix(s8 player) {
    extern bool vr_is_active(void);
    f32 left, right, bottom, top;
    if (vr_is_active()) {
        // VR: capture the ENTIRE panorama (fed to the world-locked cylinder layer).
        left = 0.0f; right = (f32) SKYBOX_WIDTH;
        bottom = 0.0f; top = (f32) SKYBOX_HEIGHT;
    } else {
        left = sSkyBoxInfo[player].scaledX;
        right = sSkyBoxInfo[player].scaledX + SCREEN_WIDTH;
        bottom = sSkyBoxInfo[player].scaledY - SCREEN_HEIGHT;
        top = sSkyBoxInfo[player].scaledY;
    }

    extern Mtx* gBackgroundSkyboxMtx;
    Mtx *mtx;
    if (gRenderingInterpolated) {
        mtx = gBackgroundSkyboxMtx;
    } else {
        mtx = alloc_display_list(sizeof(*mtx));
        gBackgroundSkyboxMtx = mtx;
    }

    if (mtx != NULL) {
        guOrtho(mtx, left, right, bottom, top, 0.0f, 3.0f, 1.0f);
    } else {
    }

    return mtx;
}

/**
 * Creates the skybox's display list, then draws the 3x3 grid of tiles.
 */
// VR: draw the ENTIRE skybox panorama (8 cols x 8 rows) so the world-locked cylinder
// layer has a full 360 image instead of just the forward window. Allocates verts fresh
// (the interpolation buffers are sized for the small window).
void draw_skybox_panorama_vr(Gfx **dlist, s8 background, s8 player, s8 colorIndex) {
    (void) player;
    for (s32 r = 0; r < 8; r++) {
        for (s32 c = 0; c < 8; c++) {
            s32 tileIndex = r * SKYBOX_COLS + c;
            if (tileIndex < 0)  { tileIndex = 0;  }
            if (tileIndex > 79) { tileIndex = 79; }
            const Texture *texture = (background < 0 || background >= 10)
                ? gCustomSkyboxPtrList[tileIndex]
                : (*(SkyboxTexture *) segmented_to_virtual(sSkyboxTextures[background]))[tileIndex];

            f32 cr = gSkyboxColor[0] / 255.0f;
            f32 cg = gSkyboxColor[1] / 255.0f;
            f32 cb = gSkyboxColor[2] / 255.0f;
            u8 *color = sSkyboxColors[colorIndex];
            gDPSetEnvColor((*dlist)++, color[0] * cr, color[1] * cg, color[2] * cb, 255);

            Vtx *verts = alloc_display_list(4 * sizeof(*verts));
            if (verts == NULL) { continue; }
            f32 x = c * SKYBOX_TILE_WIDTH;
            f32 y = SKYBOX_HEIGHT - r * SKYBOX_TILE_HEIGHT;
            make_vertex(verts, 0, x, y, -1, 0, 0, 255, 255, 255, 255);
            make_vertex(verts, 1, x, y - SKYBOX_TILE_HEIGHT, -1, 0, 31 << 5, 255, 255, 255, 255);
            make_vertex(verts, 2, x + SKYBOX_TILE_WIDTH, y - SKYBOX_TILE_HEIGHT, -1, 31 << 5, 31 << 5, 255, 255, 255, 255);
            make_vertex(verts, 3, x + SKYBOX_TILE_WIDTH, y, -1, 31 << 5, 0, 255, 255, 255, 255);

            gLoadBlockTexture((*dlist)++, 32, 32, G_IM_FMT_RGBA, texture);
            gSPVertex((*dlist)++, VIRTUAL_TO_PHYSICAL(verts), 4, 0);
            gSPDisplayList((*dlist)++, dl_draw_quad_verts_0123);
        }
    }
}

Gfx *init_skybox_display_list(s8 player, s8 background, s8 colorIndex) {
    extern Gfx* gBackgroundSkyboxGfx;
    extern bool vr_is_active(void);
    bool vrFull = vr_is_active();

    s32 tileCount = vrFull ? (8 * 8) : (sSkyboxTileNumY * sSkyboxTileNumX);
    s32 dlCommandCount = 5 + tileCount * 8; // 5 for the start and end, plus the skybox tiles

    void *skybox;
    if (gRenderingInterpolated && !vrFull) {
        skybox = gBackgroundSkyboxGfx;
    } else {
        skybox = alloc_display_list(dlCommandCount * sizeof(Gfx));
        if (!vrFull) { gBackgroundSkyboxGfx = (Gfx*)skybox; }
    }

    Gfx *dlist = skybox;

    if (skybox == NULL) {
        return NULL;
    } else {
        Mtx *ortho = create_skybox_ortho_matrix(player);

        gSPDisplayList(dlist++, dl_skybox_begin);
        gSPMatrix(dlist++, VIRTUAL_TO_PHYSICAL(ortho), G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);
        gSPDisplayList(dlist++, dl_skybox_tile_tex_settings);
        if (vrFull) { draw_skybox_panorama_vr(&dlist, background, player, colorIndex); }
        else        { draw_skybox_tile_grid(&dlist, background, player, colorIndex); }
        gSPDisplayList(dlist++, dl_skybox_end);
        gSPEndDisplayList(dlist);
    }
    return skybox;
}

/**
 * Draw a skybox facing the direction from pos to foc.
 *
 * @param player Unused, determines which orientation info struct to update
 * @param background The skybox image to use
 * @param fov Unused. It SHOULD control how much the skybox is scaled, but the way it's coded it just
 *            controls how fast the skybox rotates. The given value is replaced with 90 right before the
 *            dl is created
 * @param posX,posY,posZ The camera's position
 * @param focX,focY,focZ The camera's focus.
 */
Gfx *create_skybox_facing_camera(s8 player, s8 background, f32 fov,
                                    f32 posX, f32 posY, f32 posZ,
                                    f32 focX, f32 focY, f32 focZ) {
    if (!gBackgroundSkyboxVerts) {
        gBackgroundSkyboxVerts = growing_array_init(NULL, sSkyboxTileNumY * sSkyboxTileNumX, malloc, free);
        gBackgroundSkyboxVerts->count = sSkyboxTileNumY * sSkyboxTileNumX;
        if (!gBackgroundSkyboxVerts) {
            sys_fatal("Cannot allocate skybox vertex buffer");
        }
    }

    if (!gRenderingInterpolated) {
        f32 skyboxAspectRatio = ((f32)sSkyboxTileNumX * (f32)SKYBOX_TILE_WIDTH) / ((f32)sSkyboxTileNumY * (f32)SKYBOX_TILE_HEIGHT);
        f32 half_width = skyboxAspectRatio / GFX_DIMENSIONS_ASPECT_RATIO * SCREEN_WIDTH / 2;
        if (half_width < SCREEN_WIDTH / 2) {
            // how many horizontal tiles are needed to match the screen aspect ratio
            f32 minTilesX = sSkyboxTileNumY * ((f32)SKYBOX_TILE_HEIGHT / (f32)SKYBOX_TILE_WIDTH) * GFX_DIMENSIONS_ASPECT_RATIO;
            sSkyboxTileNumX = (u16) ceilf(minTilesX);

            // Update vertex buffer size
            gBackgroundSkyboxVerts->count = sSkyboxTileNumY * sSkyboxTileNumX;
            growing_array_alloc(gBackgroundSkyboxVerts, 0);
        }
    }

    gReadOnlyBackground = background;
    background = gOverrideBackground == -1 ? background : gOverrideBackground;

    f32 cameraFaceX = focX - posX;
    f32 cameraFaceY = focY - posY;
    f32 cameraFaceZ = focZ - posZ;
    s8 colorIndex = 1;

    // If the first star is collected in JRB, make the sky darker and slightly green
    if (background == BACKGROUND_ABOVE_CLOUDS && gLevelValues.jrbDarkenSkybox && !(save_file_get_star_flags(gCurrSaveFileNum - 1, COURSE_JRB - 1) & 1)) {
        colorIndex = 0;
    }

    //! fov is always set to 90.0f. If this line is removed, then the game crashes because fov is 0 on
    //! the first frame, which causes a floating point divide by 0
    fov = 90.0f;

    sSkyBoxInfo[player].yaw = (M_PI / 2.0) - atan2(cameraFaceZ, cameraFaceX);
    if (sSkyBoxInfo[player].yaw < 0) { sSkyBoxInfo[player].yaw += M_PI * 2.0; }
    sSkyBoxInfo[player].pitch = (M_PI / 2.0) - atan2(sqrtf(cameraFaceX * cameraFaceX + cameraFaceZ * cameraFaceZ), cameraFaceY);

    // VR: the sky is composited as a world-locked cylinder layer by the runtime, so the
    // window is drawn for the plain game camera here (no head-driving - the runtime does it).

    sSkyBoxInfo[player].scaledX = calculate_skybox_scaled_x(player, fov);
    sSkyBoxInfo[player].scaledY = calculate_skybox_scaled_y(player, fov);

    // VR: also build the world-locked 3D sky sphere for the eye render (separate DL).
    extern bool vr_is_active(void);
    if (vr_is_active()) { gVrSkyDomeGfx = build_skybox_sphere_vr(player, background, colorIndex); }

    return init_skybox_display_list(player, background, colorIndex);
}
