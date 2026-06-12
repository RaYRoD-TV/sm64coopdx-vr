#pragma once
#include "djui.h"

#define DJUI_DEFAULT_PANEL_WIDTH (500.0f + (16 * 2.0f))
#define DJUI_PANEL_HEADER_OFFSET (-16)
#define DJUI_PANEL_MOVE_MAX 1.0f

// DjuiBase.tag value used to mark the VR settings panel, so VR can keep rendering the live stereo diorama
// underneath it (instead of the flat menu panel) while you adjust the diorama sliders. See djui_panel_vr.c.
#define DJUI_PANEL_TAG_VR 0x5652 // 'V','R'

struct DjuiPanel {
    struct DjuiBase* base;
    struct DjuiPanel* parent;
    struct DjuiBase* defaultElementBase;
    bool temporary;
    bool (*on_back)(struct DjuiBase*);
    void (*on_panel_destroy)(struct DjuiBase*);
};

extern bool gDjuiPanelDisableBack;

bool djui_panel_is_active(void);
bool djui_panel_is_vr_panel(void); // true when the active panel is the VR settings panel (tagged DJUI_PANEL_TAG_VR)
bool djui_panel_active_is_left_docked(void);
struct DjuiPanel* djui_panel_add(struct DjuiBase* caller, struct DjuiThreePanel* threePanel, struct DjuiBase* defaultElementBase);
void djui_panel_back(void);
void djui_panel_update(void);
void djui_panel_shutdown(void);
