#ifndef GT_UIKIT_INPUT_H
#define GT_UIKIT_INPUT_H

#include "ui.h"

#define MAX_FINGERS 10

static inline float stage_from_landscape(float l) { return ui_wide_ui ? l - ui_stage_dx : l * 1024.0f / UI_SCREEN_H; }
static inline float landscape_from_stage(float s) { return ui_wide_ui ? s + ui_stage_dx : s * UI_SCREEN_H / 1024.0f; }

void ui_touch_event(int finger, int phase, float lx, float ly);
void ui_accel_tick(void);

#endif
