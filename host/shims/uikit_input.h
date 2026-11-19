/* uikit_input.h -- what uikit.c (the UIKit classes) and pcinput.c (the PC's input for the game) share. */
#ifndef GT_UIKIT_INPUT_H
#define GT_UIKIT_INPUT_H

#include "ui.h"

#define MAX_FINGERS 10                  /* touches at once (the mouse is finger 0, host taps and swipes 9) */

/* WIDE_UI: the stage (1024 x 768, what gameswf and the host's points use) is drawn ui_stage_dx landscape points
   from the left on a wider screen; on a 4:3 screen both are the identity (see uikit.c) */
static inline float stage_from_landscape(float l) { return ui_wide_ui ? l - ui_stage_dx : l * 1024.0f / UI_SCREEN_H; }
static inline float landscape_from_stage(float s) { return ui_wide_ui ? s + ui_stage_dx : s * UI_SCREEN_H / 1024.0f; }

/* uikit.c */
void ui_touch_event(int finger, int phase, float lx, float ly);  /* phase 0 began, 1 moved, 3 ended; stage points */
void ui_accel_tick(void);               /* the accelerometer's delegate called when its interval is due */

/* pcinput.c */

#endif
