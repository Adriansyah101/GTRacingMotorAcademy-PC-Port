#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define HUD_VTBL 0x788c48u
#define HUD_DRAW_FN 0x27bf84u
#define FN_SET_ALPHA 0x50bdfcu
#define CH_VISIBLE 0xab
#define CH_CXFORM 0x58
#define CX_ALPHA_MULT 0x18
#define PAUSE_ICON "HUD.MovingElements.Home"

static const char *const parts[] = {
    "AccelBrake.btnAccel", "AccelBrake.btnBrake", "AccelBrake.btnBrakeLeft", "AccelBrake.btnBrakeRight", "Gears", NULL};
static u32 hidden_by_us[8];
static u32 faded;
static int on = -1;
static void (*hud_draw_fn)(CPU *);

static u32 find(u32 pl, const char *path) { return rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)); }
u32 swf_find_under(u32 pl, u32 parent, const char *rest);

static void hide_parts(u32 pl)
{
    int hide = pcctl_keys_mode();
    int show = !hide && steer_racing() && !menu_top();
    u32 hud = find(pl, "HUD");
    for (int i = 0; parts[i]; i++) {
        u32 ch = swf_find_under(pl, hud, parts[i]);
        volatile u8 *v = ch ? (volatile u8 *)GP(ch + CH_VISIBLE) : NULL;
        if (hide) {
            if (v && *v) {
                *v = 0;
                hidden_by_us[i] = ch;
            }
        } else if (show && hidden_by_us[i]) {
            if (v && hidden_by_us[i] == ch)
                *v = 1;
            hidden_by_us[i] = 0;
        }
    }
    u32 icon = swf_find_under(pl, hud, PAUSE_ICON + 4);
    if (hide && icon) {
        u32 cx = rd32(icon + CH_CXFORM);
        if (!cx || *(volatile float *)GP(cx + CX_ALPHA_MULT) != 0.0f)
            rt_call3(FN_SET_ALPHA, pl, GA((void *)PAUSE_ICON), 0);
        if (icon != faded)
            rt_log("[hudhide] Touch Screen off: pause icon, pedals and gear arrows not drawn\n");
        faded = icon;
    } else if (!hide && faded) {
        if (icon == faded)
            rt_call3(FN_SET_ALPHA, pl, GA((void *)PAUSE_ICON), 0x3f800000u);
        faded = 0;
        rt_log("[hudhide] Touch Screen on: the HUD's touch controls are drawn again\n");
    }
}

static void hud_draw_hook(CPU *c)
{
    u32 pl = rd32(c->r[0] + 4);
    static int logged;
    if (!logged++)
        rt_log("[hudhide] HUD drawn (player 0x%x)\n", pl);
    if (on > 0 && pl)
        hide_parts(pl);
    if (pl)
        wide_hud(pl);
    hud_draw_fn(c);
}

void hudhide_init(void)
{
    const char *e = getenv("GT_HIDE_TOUCH_HUD");
    on = !(e && *e == '0');
    if (!on)
        rt_log("[hudhide] off (GT_HIDE_TOUCH_HUD=0): the touch controls stay on the race HUD\n");
    if (!gt_hook_slot(HUD_VTBL + 0x20, HUD_DRAW_FN, &hud_draw_fn, hud_draw_hook, "hudhide hud_draw_hook"))
        rt_log("[hudhide] touch controls stay, HUD not widened\n");
}
