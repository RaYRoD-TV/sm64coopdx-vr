#ifndef CONTROLLER_VR_H
#define CONTROLLER_VR_H

#include "controller_api.h"

// VR motion controllers (Quest Touch, Index) presented to the game as a standard gamepad.
// Reads the OpenXR action state polled by src/pc/vr/vr.c; inert when VR is off.
extern struct ControllerAPI controller_vr;

#endif
