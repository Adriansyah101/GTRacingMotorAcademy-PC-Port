/* features.h -- the port's features (games/gtracing/features/, render/): their entry points and shared state.
   The PC input (pcinput.c) sets the control state below; steer.c and the menus' features read it. */
#ifndef GT_FEATURES_H
#define GT_FEATURES_H

#include "ui.h"

/* the controls (pcinput.c) */
extern int ui_steer_active;         /* a steering key is held or the pad's stick is out */
extern int ui_steer_dir;            /* -1 left, 0, 1 right (keys, or the stick past half way) */
extern int ui_gas, ui_brake;        /* held: Up/W or the right trigger, Down/S or the left trigger */
extern int ui_gear_up, ui_gear_down; /* held: Shift or the right shoulder, Ctrl or the left shoulder */
extern int ui_gear_tap;             /* 1 up, 2 down: pressed since steer.c last looked (short taps) */
extern int ui_cam_tap;              /* C or the pad's R1 pressed since steer.c last looked: next camera */
extern int ui_rear;                 /* held: V, look back (rear view) */
/* steer.c */
void steer_init(void);              /* keys / pad also drive the on-screen wheel and side buttons */
void steer_frame(void);             /* once a frame from ui_pump (test switch GT_STEER_TYPE) */
int steer_racing(void);             /* a race is being driven (state 2 .. 8): the idle pointer hides */
int steer_race_waiting(void);       /* the race waits for "Touch to continue" (state 1) */
/* replay.c */
extern float ui_zoom_wheel;         /* mouse wheel notches since the last frame (+ = up), for replay zoom */
void replay_frame(void);            /* once a frame from steer_frame: REPLAY_UNDIM, REPLAY_ZOOM */
const char *menu_top(void);         /* the menu SWF's top menu's name, or NULL (also when moved off) */
int menu_shown(u32 player);         /* a MenuFX player's menus are on the screen (not moved off) */
/* padmenu.c (PAD_MENU_SNAP) */
int padmenu_active(void);           /* the pad drives the menus now (not while racing, pause menu yes) */
void padmenu_frame(int pressed, int pad_present); /* bits: 1 up 2 down 4 left 8 right 16 select 32 back */
void padmenu_mouse_moved(void);     /* the real mouse moved: the pad lets the pointer go */
/* pcinput.c, for padmenu.c (landscape points) */
void ui_pointer_to(float lx, float ly);
void ui_pointer_at(float *lx, float *ly);
void ui_pad_tap(float lx, float ly);
void ui_pad_swipe(float x, float y, float x2, float y2, int frames);
void replay_init(void);             /* from steer_init: the replay update hook (REPLAY_ZOOM) */
/* pcctl.c */
void pcctl_init(void);              /* from steer_init: Options > Controls' Touch Screen row (PC_CONTROLS) */
void pcctl_frame(void);             /* once a frame from steer_frame: the keyboard choices stay in force */
int pcctl_keys_mode(void);          /* Touch Screen is off: keyboard / gamepad mode */
/* keymap.c (KEY_MAP) */
enum { KM_LEFT, KM_RIGHT, KM_GAS, KM_BRAKE, KM_GEAR_UP, KM_GEAR_DOWN, KM_REAR, KM_CAMERA, KM_PAUSE, KM_ACTIONS };
#define KM_KEYS 2                   /* keyboard keys per action; column KM_KEYS is the pad's */
void keymap_init(void);             /* from pcctl_init: the profile's keys (pc_keys.txt) */
int keymap_pad_kind(void);          /* 0 no pad, 1 Xbox-like (A), 2 PlayStation (Cross) */
/* display.c (DISPLAY_MENU) */
void display_init(void);            /* from pcctl_init: Options' Display tile */
int display_take_page(void);        /* ControlsMenu entered from the Display tile: show its PCDisplay page */
void display_menu_fill(u32 pl);
void display_menu_layout(u32 pl);
void display_menu_click(u32 pl, const char *path);
void display_fullscreen_toggle(void);
void display_fullscreen_pump(void);  /* ui_pump: a switch asked for from the game thread */
u32 gt_text_filter(u32 text);       /* notouch.c, from the generated 0x4fde00: NO_TOUCH_TEXT */
int keymap_key(SDL_Keycode k, int down, int repeat); /* every key event; 1 = taken (a cell waited for it) */
int keymap_key_held(int action);
int keymap_key_tap(int action);     /* a key of it went down since the last look (cleared) */
float keymap_pad(SDL_Gamepad *pad, int action);      /* 0 .. 1 */
int keymap_pad_menu_input(int action);               /* on a pad button the menus use */
int keymap_frame(SDL_Gamepad *pad); /* once a frame; 1 = the pad is busy with the page, not for the menus */
const char *keymap_title(int action);
const char *keymap_cell(int action, int col);        /* the cell's text (col 0, 1 keys, 2 pad) */
void keymap_capture(int action, int col);            /* that cell waits for a key / pad input */
void keymap_cancel(void);
int keymap_capturing(int *action, int *col);
void keymap_defaults(void);
int keymap_version(void);           /* changes with any cell's text */
/* exitbtn.c */
void exitbtn_init(void);            /* from steer_init: the main menu's Exit button (EXIT_BUTTON) */
/* hudhide.c */
void hudhide_init(void);            /* from steer_init: no touch controls on the race HUD with Touch Screen Off */
/* menu3d.c */
void menu3d_init(void);             /* from steer_init: MENU_3D, the 3D scene behind the main menu */
void menu3d_nav_update(u32 pl);     /* each NavigationMenu update (exitbtn.c) */
int menu3d_hide_backdrop(u32 ch);   /* a shape about to be drawn (widescreen.c): 1 = skip it (UPGRADE_3D) */
/* widescreen.c */
void wide_hud(u32 pl);              /* each race HUD draw (hudhide.c): its parts to the screen's edges */
void wide_nav(u32 pl);              /* each NavigationMenu update (exitbtn.c): its left bar to the left edge */
float wide_draw_shift(u32 ch);      /* how far (stage points) a character is drawn from its place (padmenu.c) */
/* pcinput.c */
void ui_tap(float x, float y);      /* one tap at a stage point, held 4 frames by the UI thread (as GT_TAPS's) */
int numpad_dir(float *x, float *y); /* the numpad's digits held: x right, y ahead; 0 = none (or opposite ones) */
/* padmenu.c */
const char *padmenu_top(void);      /* the top menu's name on the screen now, or NULL */
void replay_hint(int which);        /* replay.c: Cross on a replay hint: 1 the pinch (a zoom step), else the swipe (a turn) */
/* replay.c, racelook.c */
void replay_pad(float rx, float ry, int zoom_in, int zoom_out, int on, int back);
void race_look_input(float rx, float ry);
/* menu3d.c */
u32 menu3d_scene(void);             /* the menus' 3D scene object (topcars.c opens the Car Dealer through it) */
/* display.c */
void display_test_page(void);       /* GT_TEST_MENU (development build): as if the Display tile was pressed */
/* swffind.c (FAST_SWF_FIND) */
u32 swf_find_under(u32 pl, u32 parent, const char *rest);   /* `rest` under a character already found */
/* shop.c */
int shop_cash_new(void);            /* the title bar's NEW on the cash: the rate differs from the one last seen */
/* p2p.c (ONLINE_P2P, PLAYER_NAME, FIRST_NAME) */
void p2p_init(void);
void p2p_name_now(char *out, size_t cap);   /* the player's chosen name */
int p2p_nav_event(CPU *c);          /* NavigationMenu's event (exitbtn.c); 1 = taken */
int p2p_first_open(void);           /* FIRST_NAME's popup is open */
void p2p_nav_update(u32 pl);
int p2p_form_open(void);            /* a text form (name, room code) has the keyboard */
int p2p_form_event(const SDL_Event *e);     /* 1 = the form took the event */
u32 p2p_menu_redirect(u32 name);    /* a menu name about to be opened -> the one to open instead */
/* topcars.c (TOP_CARS) */
void topcars_init(void);
int topcars_event(CPU *c);          /* the Top 10 list's Buy / Rent -> the Car Dealer; 1 = taken */
/* board.c (RED_BULL_BOARD) */
void board_init(void);
void board_tick(void);              /* once a frame (pcinput.c) */
void board_nav_event(const char *name, int kind);
u32 board_menu_redirect(u32 name);  /* menu3d.c's menu push: the name to open instead */
/* maxmoney.c (the MAX_STATS cheat code) */
void maxmoney_nav_update(u32 self);
void maxmoney_frame(int menus);     /* a code entered applies */
int cheat_key(int letter);          /* a key went down (not a repeat): 'A'..'Z' or 0; 1 = taken */
int cheat_pad(int lb, int rb, int up, int down, int x, int y, int other);
/* render/: gpuprofile.c (GPU_PROFILE) */
int gpu_setting_auto(const char *key);      /* the key is missing, blank or "auto": the profile chooses */
int gpu_auto_msaa(void);
int gpu_auto_full_blur(void);
/* render/: postfx.c (POSTFX), fullblur.c (FULL_BLUR), softrefl.c (SOFT_REFLECTION) */
int gt_postfx_wanted(void);         /* the mode in gtracing.ini (the Display menu's choice) */
void gt_postfx_set(int m);          /* live, from the Display menu */
void gt_postfx_targets(int on);     /* fullblur.c: the post effect object's targets are being made */
void fullblur_choose(const char *v);
void fullblur_frame_end(void);
void softrefl_bind_fb(u32 fb);      /* glenhance.c: the game binds fb (leaving the map's: the map is drawn) */

#endif
