#ifndef GT_UI_H
#define GT_UI_H

#include <SDL3/SDL.h>

#include "abi.h"
#include "ns.h"

#define UI_SCREEN_W 768.0f
#define UI_SCREEN_H ui_screen_h
extern float ui_screen_h;
extern int ui_wide_ui;
extern float ui_stage_dx;
#define UI_ORIENTATION 3

extern SDL_Window *ui_window;
extern int ui_quit;

void ui_init(void);
void cg_init(void);
int ui_is_main_thread(void);
int ui_background_fps(void);
int ui_minimized(void);
void ui_view_to_screen(id view, GtAffine *m);
id ui_layer_view(id layer);
void ui_layer_size(id layer, float *w, float *h, float *scale);

void eagl_init(void);
void eagl_window_created(void);
u32 eagl_frames(void);
void eagl_drawable(u32 *rb, int *w, int *h);
void gles_load(void);
void al_init(void);
void al_stop_all(void);
extern float ui_tilt;
void ui_gravity(double g[3]);

#endif
