#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define NAV_VTBL 0x786ed8u
#define NAV_UPDATE_FN 0x1e95c8u
#define NAV_EVENT_FN 0x1eb5acu
#define CH_VISIBLE 0xab

#define BUTTON "btnPCExit"
#define POPUP "NavigationMenu.FlurryPopup"

static int on = -1;
static int asking;
static void (*nav_event_fn)(CPU *), (*nav_update_fn)(CPU *);

static void set_text(u32 pl, const char *field, const char *s)
{
    char p[96];
    snprintf(p, sizeof p, POPUP ".Texts.%s", field);
    if (rt_call2(GT_FN_SWF_FIND, pl, GA(p)))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(p), GA((void *)s), 0);
}

static void ask(u32 pl)
{
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)POPUP), GA((void *)"Show"), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)POPUP ".btnYes"), 1);
    rt_call3(GT_FN_SWF_SET_VISIBLE, pl, GA((void *)POPUP ".Texts.txtYes"), 1);
    set_text(pl, "field", "EXIT THE GAME?");
    set_text(pl, "txtYes.Field", "YES");
    set_text(pl, "txtNo.Field", "NO");
    asking = 1;
    rt_log("[exit] Exit pressed: asking\n");
}

static void nav_event_hook(CPU *c)
{
    u32 self = c->r[0], ev = c->r[1], pl = rd32(self + 4);
    int kind = (int)rd32(ev + 8);
    const char *name = rd32(ev + 4) ? (const char *)GP(rd32(ev + 4)) : "";
    board_nav_event(name, kind);
    if (topcars_event(c))
        return;
    if (p2p_nav_event(c)) {
        static const char none[] = "";
        u32 old = rd32(ev + 4);
        wr32(ev + 4, GA((void *)none));
        nav_event_fn(c);
        wr32(ev + 4, old);
        return;
    }
    if (!strcmp(name, BUTTON)) {
        rt_call2(GT_FN_MENU_BASE_EVENT, self, ev);
        if (on > 0 && kind == 6 && pl && !asking && !p2p_first_open())
            ask(pl);
        return;
    }
    int yes = !strcmp(name, "btnYes"), no = !strcmp(name, "btnNo");
    if (asking && (yes || no)) {
        u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
        if (path && strstr((const char *)GP(path), "FlurryPopup")) {
            if (kind == 6) {
                asking = 0;
                if (pl)
                    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)POPUP), GA((void *)"Hided"), 0);
                if (yes) {
                    rt_log("[exit] YES: the game ends (as when the window is closed)\n");
                    ui_quit = 1;
                } else {
                    rt_log("[exit] NO: back to the menu\n");
                }
            }
            static const char none[] = "";
            u32 old = rd32(ev + 4);
            wr32(ev + 4, GA((void *)none));
            nav_event_fn(c);
            wr32(ev + 4, old);
            return;
        }
    }
    nav_event_fn(c);
}

static void nav_update_hook(CPU *c)
{
    u32 self = c->r[0];
    nav_update_fn(c);
    u32 pl = rd32(self + 4);
    menu3d_nav_update(pl);
    if (pl)
        wide_nav(pl);
    p2p_nav_update(pl);
    maxmoney_nav_update(self);
    if (on == 0 && pl) {
        u32 ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)"NavigationMenu." BUTTON));
        if (ch && *(volatile u8 *)GP(ch + CH_VISIBLE))
            *(volatile u8 *)GP(ch + CH_VISIBLE) = 0;
    }
}

void exitbtn_init(void)
{
    const char *e = getenv("GT_EXIT_BUTTON");
    on = !(e && *e == '0');
    if (gt_hook_slot(NAV_VTBL + 0x2c, NAV_EVENT_FN, &nav_event_fn, nav_event_hook, "exitbtn nav_event_hook") &&
        gt_hook_slot(NAV_VTBL + 0x1c, NAV_UPDATE_FN, &nav_update_fn, nav_update_hook, "exitbtn nav_update_hook"))
        rt_log("[exit] %s\n", on ? "Exit button on the main menu" : "off (GT_EXIT_BUTTON=0): the button is hidden");
}
