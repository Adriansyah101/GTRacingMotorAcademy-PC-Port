/* game.h -- the game's own globals, functions and structure offsets that several of the port's features use
   (features/, render/).  Addresses are those of the shipped binary (free edition 1.3.4b).  A name is the game's
   own where its metadata has one (Objective-C classes, RenderFX's ActionScript names), otherwise what the code
   there does.  Addresses one feature alone uses stay in that feature's file, next to the analysis that found them.

   The *_SLOT globals are non-lazy pointers: the slot holds the address of a variable that holds the object, so the
   object is rd32(rd32(slot)) (0 before the game has made it). */
#ifndef GT_GAME_H
#define GT_GAME_H

#include "rt.h"

/* globals */
#define GT_G_SLOT          0x70c048u  /* -> -> the game object (G below) */
#define GT_PROFILE_SLOT    0x70c040u  /* -> -> the player's profile manager: coins, cash, XP, records; its garage +4 */
#define GT_LICENCES_SLOT   0x70c044u  /* -> -> the licence / level manager: the XP table, licence classes +0x240 */
#define GT_CAR_TABLE_SLOT  0x70c054u  /* -> -> the car table: {rows, ?, count}, a row 0x128 bytes */
#define GT_SETTINGS        0x70ba90u  /* -> CGameSettings (valid once its guard byte is set) */
#define GT_SETTINGS_GUARD  0x70bab8u  /* -> its static-initialisation guard */
#define GT_SCREEN_PX_W     0x7711bcu  /* int: the screen in pixels, portrait (F_2ff958 x the scale): width ... */
#define GT_SCREEN_PX_H     0x7711c0u  /* ... and height (the landscape picture's width) */
#define GT_VALUE_KEY_XOR   0x7739f8u  /* the profile's protected numbers: stored ^ this, rotated right by ... */
#define GT_VALUE_KEY_ROT   0x7739f4u  /* ... this & 31 (the getter 0x2a0f84) */

static inline u32 gt_game(void) { return rd32(rd32(GT_G_SLOT)); }
static inline u32 gt_profile(void) { return rd32(rd32(GT_PROFILE_SLOT)); }
static inline u32 gt_car_table(void) { return rd32(rd32(GT_CAR_TABLE_SLOT)); }

/* the game object G (CGame) */
#define G_RACE          0x17acu       /* the race being played, 0 in the menus */
#define G_MENUS         0x19d4u       /* the main menus' MenuFX player (0 in the Paddocks); its menus [+0x114], count +0x118 */
#define G_NEXT_MENU     0x19d8u       /* char[256]: the menu the menus' loader opens after a race */
#define G_RACE_MENUS    0x1b2cu       /* the in-race menus' MenuFX player (PauseMenu ...) */
#define G_PRIZE_MENUS   0x1b38u       /* the event / licence prize screen's player (0x29f4e8 loads it) */
#define G_PADDOCK_MENUS 0x1b44u       /* the Paddocks' player (LotteryMenu) */
#define G_REPLAY        0x1be4u       /* the replay (its slider: 0x1af678) */
#define G_REPLAY_ON     0x4179u       /* u8: a replay plays (0 while a race is driven) */
#define G_REPLAY_VIEW   0x4300u       /* int: the replay's camera view, 1 .. 7 */
#define G_ONLINE_REQ    0x4394u       /* the online request object (GLX), 0 = none */
#define G_MENU_SCENE    0x4403u       /* u8: the menus draw the 3D scene behind them (0x2520c4 -> 0x24ef64) */

/* the menus' vtables: the slots the port hooks */
#define GT_VT_UPDATE    0x1cu         /* Update(menu) */
#define GT_VT_EVENT     0x2cu         /* OnEvent(menu, event): event {control, name, kind (6 = released on it)} */
#define GT_VT_MAIN_MENU 0x786d98u     /* MainMenu (the first start's main menu) */
#define GT_VT_SCENE     0x788578u     /* the menus' state (the scene, 0x25292c) */

/* RenderFX (gameswf) on a MenuFX player; paths are dotted ("HUD.MovingElements.Home") */
#define GT_FN_SWF_FIND        0x50b9ecu   /* (player, path) -> character or 0 */
#define GT_FN_SWF_CHILD       0x50a81cu   /* (player, name, parent) -> character or 0 */
#define GT_FN_SWF_SET_TEXT    0x50c038u   /* (player, path, const char *utf8, html) */
#define GT_FN_SWF_SET_VISIBLE 0x50bde0u   /* (player, path, bool) */
#define GT_FN_SWF_GOTO_LABEL  0x50bc94u   /* (player, clip path, label, play) */
#define GT_FN_SWF_SET_MEMBER  0x50bce0u   /* (player, path, member name, value) */
#define GT_FN_WORLD_MATRIX    0x4ca618u   /* character -> its world matrix {a, b, tx, c, d, ty} (twips), refreshed if dirty */

/* the menus */
#define GT_FN_MENU_BASE_EVENT 0x2589a0u   /* the menus' common event handler */
#define GT_FN_EVENT_PATH      0x23d988u   /* (event) -> the control's full path */

/* the profile manager */
#define GT_FN_ADD_COINS 0x268704u         /* (profile manager, n) */
#define GT_FN_ADD_CASH  0x268724u         /* (profile manager, n) */
#define GT_FN_ADD_XP    0x268e70u         /* (profile manager, n), then the level-up check */
#define GT_FN_CAR_INDEX 0x2b5fe8u         /* (car table, car id) -> row, < 0 = none */

/* A menu's vtable slot holding fn becomes h (games/gtracing/features/hook.c): *orig is the game's function, for h to
   call.  Checked first: the slot must hold fn and fn must be a recompiled function, else nothing changes and 0 is
   returned (the caller logs what goes without the hook).  The page stays writable (several hooks share a page). */
int gt_hook_slot(u32 slot, u32 fn, void (**orig)(CPU *), void (*h)(CPU *), const char *name);
/* the bytes at addr made writable (game data the port changes in place, a table entry for instance) */
void gt_unprotect(u32 addr, u32 n);

#endif
