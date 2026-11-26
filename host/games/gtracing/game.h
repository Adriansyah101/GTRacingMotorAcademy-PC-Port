#ifndef GT_GAME_H
#define GT_GAME_H

#include "rt.h"

#define GT_G_SLOT          0x70c048u
#define GT_PROFILE_SLOT    0x70c040u
#define GT_LICENCES_SLOT   0x70c044u
#define GT_CAR_TABLE_SLOT  0x70c054u
#define GT_SETTINGS        0x70ba90u
#define GT_SETTINGS_GUARD  0x70bab8u
#define GT_SCREEN_PX_W     0x7711bcu
#define GT_SCREEN_PX_H     0x7711c0u
#define GT_VALUE_KEY_XOR   0x7739f8u
#define GT_VALUE_KEY_ROT   0x7739f4u

static inline u32 gt_game(void) { return rd32(rd32(GT_G_SLOT)); }
static inline u32 gt_profile(void) { return rd32(rd32(GT_PROFILE_SLOT)); }
static inline u32 gt_car_table(void) { return rd32(rd32(GT_CAR_TABLE_SLOT)); }

#define G_RACE          0x17acu
#define G_MENUS         0x19d4u
#define G_NEXT_MENU     0x19d8u
#define G_RACE_MENUS    0x1b2cu
#define G_PRIZE_MENUS   0x1b38u
#define G_PADDOCK_MENUS 0x1b44u
#define G_REPLAY        0x1be4u
#define G_REPLAY_ON     0x4179u
#define G_REPLAY_VIEW   0x4300u
#define G_ONLINE_REQ    0x4394u
#define G_MENU_SCENE    0x4403u

#define GT_VT_UPDATE    0x1cu
#define GT_VT_EVENT     0x2cu
#define GT_VT_MAIN_MENU 0x786d98u
#define GT_VT_SCENE     0x788578u

#define GT_FN_SWF_FIND        0x50b9ecu
#define GT_FN_SWF_CHILD       0x50a81cu
#define GT_FN_SWF_SET_TEXT    0x50c038u
#define GT_FN_SWF_SET_VISIBLE 0x50bde0u
#define GT_FN_SWF_GOTO_LABEL  0x50bc94u
#define GT_FN_SWF_SET_MEMBER  0x50bce0u
#define GT_FN_WORLD_MATRIX    0x4ca618u

#define GT_FN_MENU_BASE_EVENT 0x2589a0u
#define GT_FN_EVENT_PATH      0x23d988u

#define GT_FN_ADD_COINS 0x268704u
#define GT_FN_ADD_CASH  0x268724u
#define GT_FN_ADD_XP    0x268e70u
#define GT_FN_CAR_INDEX 0x2b5fe8u

int gt_hook_slot(u32 slot, u32 fn, void (**orig)(CPU *), void (*h)(CPU *), const char *name);
void gt_unprotect(u32 addr, u32 n);

#endif
