#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define SET_STEERING 0x5c
#define SET_THROTTLE 0x58
#define SET_GEARBOX 0x60
#define SET_LEVEL_HORIZON 0x70

#define SWF_GOTO_FRAME 0x50bc64u
#define SWF_PLAY_LABEL 0x50afc4u
#define SWF_VISIBLE 0xab

#define CTL_VTBL 0x788078u
#define CTL_ENTER_FN 0x243320u
#define CTL_UPDATE_FN 0x243830u
#define CTL_EVENT_FN 0x243a4cu

static int on = -1;
static struct {
    int touch;
    int manual, autogas;
    int touch_steer, touch_throttle, touch_gearbox;
    int tilt;
} cfg = {-1, 0, 0, -1, -1, -1, -1};

static const char *cfg_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/pc_controls.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static void cfg_load(void)
{
    char p[MAX_PATH * 2], line[128];
    FILE *f = fopen(cfg_path(p, sizeof p), "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "touch=%d", &v) == 1) cfg.touch = v != 0;
        else if (sscanf(line, "manual=%d", &v) == 1) cfg.manual = v != 0;
        else if (sscanf(line, "autogas=%d", &v) == 1) cfg.autogas = v != 0;
        else if (sscanf(line, "touch_steering=%d", &v) == 1) cfg.touch_steer = v;
        else if (sscanf(line, "touch_throttle=%d", &v) == 1) cfg.touch_throttle = v;
        else if (sscanf(line, "touch_gearbox=%d", &v) == 1) cfg.touch_gearbox = v;
        else if (sscanf(line, "camera_tilt=%d", &v) == 1) cfg.tilt = v != 0;
    }
    fclose(f);
}

static void cfg_save(void)
{
    char p[MAX_PATH * 2];
    FILE *f = fopen(cfg_path(p, sizeof p), "w");
    if (!f) {
        rt_log("[pcctl] cannot write %s\n", p);
        return;
    }
    fprintf(f, "# PC_CONTROLS (Options > Controls): Touch Screen on/off and the keyboard / gamepad choices\n"
               "touch=%d\nmanual=%d\nautogas=%d\n"
               "# the touch screen's settings, put back when Touch Screen is turned on again\n"
               "touch_steering=%d\ntouch_throttle=%d\ntouch_gearbox=%d\n",
            cfg.touch, cfg.manual, cfg.autogas, cfg.touch_steer, cfg.touch_throttle, cfg.touch_gearbox);
    if (cfg.tilt >= 0)
        fprintf(f, "# Camera Tilt with the keyboard / gamepad\ncamera_tilt=%d\n", cfg.tilt);
    fclose(f);
}

static u32 settings(void) { return *(volatile u8 *)GP(rd32(GT_SETTINGS_GUARD)) ? rd32(GT_SETTINGS) : 0; }

static void set_int(u32 s, u32 off, int v, const char *what)
{
    if ((int)rd32(s + off) != v) {
        rt_log("[pcctl] %s %d -> %d\n", what, (int)rd32(s + off), v);
        wr32(s + off, (u32)v);
    }
}

static void keys_apply(void)
{
    u32 s = settings();
    if (!s || cfg.touch)
        return;
    set_int(s, SET_STEERING, 2, "steering");
    set_int(s, SET_THROTTLE, cfg.autogas ? 4 : 0, "throttle");
    set_int(s, SET_GEARBOX, cfg.manual ? 0 : 2, "gearbox");
}

static int in_race(void)
{
    u32 g = gt_game();
    return g && rd32(g + G_RACE);
}

static int restore_after_race;

static void touch_restore(u32 s)
{
    if (cfg.touch_steer >= 0)
        set_int(s, SET_STEERING, cfg.touch_steer, "steering");
    if (cfg.touch_throttle >= 0)
        set_int(s, SET_THROTTLE, cfg.touch_throttle, "throttle");
    if (cfg.touch_gearbox >= 0)
        set_int(s, SET_GEARBOX, cfg.touch_gearbox, "gearbox");
}

static void set_touch(int touch)
{
    u32 s = settings();
    if (touch == cfg.touch || !s)
        return;
    cfg.touch = touch;
    if (!touch) {
        if (!in_race() || cfg.touch_steer < 0) {
            cfg.touch_steer = (int)rd32(s + SET_STEERING);
            cfg.touch_throttle = (int)rd32(s + SET_THROTTLE);
            cfg.touch_gearbox = (int)rd32(s + SET_GEARBOX);
        }
        restore_after_race = 0;
        keys_apply();
    } else {
        touch_restore(s);
        restore_after_race = in_race();
    }
    rt_log("[pcctl] Touch Screen %s%s\n", touch ? "on" : "off (keyboard / gamepad)", in_race() ? " (in a race)" : "");
    cfg_save();
}

static int camera_tilt(void)
{
    u32 s = settings();
    return s && *(volatile u8 *)GP(s + SET_LEVEL_HORIZON);
}

static u32 find(u32 pl, const char *path) { return rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)); }

static const char *const touch_parts[] = {
    "AllTabs.PresetChange", "AllTabs.Display", "AllTabs.SteeringChange", "AllTabs.ThrottleChange",
    "AllTabs.GearsChange", "AllTabs.btnSteeringTab", "AllTabs.btnThrottleTab", "AllTabs.btnGearsTab",
    "AllTabs.TabSmall1", "AllTabs.TabSmall2", "AllTabs.TabSmall3", "AllTabs.TabBig1", "AllTabs.TabBig2",
    "AllTabs.TabBig3", "btnCustom", "btnPresets", NULL};
static u32 hidden_by_us[32];
static const char *const key_rows[] = {"PCTilt", "PCGas", "PCGearTitle", "PCGear", "PCKeysOpen", NULL};
static int keys_page;
static int display_page;

static void set_visible(u32 ch, int v)
{
    if (ch && *(volatile u8 *)GP(ch + SWF_VISIBLE) != (u8)v)
        *(volatile u8 *)GP(ch + SWF_VISIBLE) = (u8)v;
}

static void switch_rest(u32 pl, const char *row, int v)
{
    char p[96];
    snprintf(p, sizeof p, "ControlsMenu.%s.btnOnOff.SliderOnOff", row);
    rt_call4(SWF_GOTO_FRAME, pl, GA(p), v ? 10 : 2, 0);
}

static void set_text(u32 pl, const char *path, const char *text)
{
    char p[96];
    snprintf(p, sizeof p, "ControlsMenu.%s", path);
    if (find(pl, p))
        rt_call4(GT_FN_SWF_SET_TEXT, pl, GA(p), GA((void *)text), 0);
}

static u32 shown_player;
static int shown_keys = -1;

static void hi(u32 pl, const char *cell, int v)
{
    char p[96];
    snprintf(p, sizeof p, "ControlsMenu.%s.Hi", cell);
    set_visible(find(pl, p), v);
}

static void keys_fill(u32 pl)
{
    char cell[32], p[48];
    int wa = -1, wc = -1;
    keymap_capturing(&wa, &wc);
    for (int a = 0; a < KM_ACTIONS; a++)
        for (int c = 0; c <= KM_KEYS; c++) {
            snprintf(cell, sizeof cell, "PCKeys.Cell%d%d", a + 1, c + 1);
            snprintf(p, sizeof p, "%s.txtKey", cell);
            set_text(pl, p, keymap_cell(a, c));
            hi(pl, cell, a == wa && c == wc);
        }
    shown_keys = keymap_version();
}

static void menu_fill(u32 pl)
{
    char p[48];
    set_text(pl, "PCKeysOpen.txtKey", "Keyboard / Gamepad");
    set_text(pl, "PCKeys.Head1", "Key 1");
    set_text(pl, "PCKeys.Head2", "Key 2");
    set_text(pl, "PCKeys.Head3", "Gamepad");
    for (int a = 0; a < KM_ACTIONS; a++) {
        snprintf(p, sizeof p, "PCKeys.Title%d", a + 1);
        set_text(pl, p, keymap_title(a));
    }
    set_text(pl, "PCKeys.CellDefault.txtKey", "Default");
    set_text(pl, "PCKeys.CellBack.txtKey", "Back");
    hi(pl, "PCKeysOpen", 0);
    hi(pl, "PCKeys.CellDefault", 0);
    hi(pl, "PCKeys.CellBack", 0);
    keys_fill(pl);
    display_menu_fill(pl);
    set_text(pl, "PCTouch.txtTitle", "Touch Screen");
    set_text(pl, "PCTilt.txtTitle", "Camera Tilt");
    set_text(pl, "PCGas.txtTitle", "Auto Gas");
    set_text(pl, "PCGearTitle.typefield", "Transmission:");
    set_text(pl, "PCGear.txtSelectorType", cfg.manual ? "Manual" : "Automatic");
    switch_rest(pl, "PCTouch", cfg.touch);
    switch_rest(pl, "PCTilt", camera_tilt());
    switch_rest(pl, "PCGas", cfg.autogas);
    shown_player = pl;
}

static void menu_layout(u32 pl)
{
    char p[96];
    for (int i = 0; touch_parts[i]; i++) {
        snprintf(p, sizeof p, "ControlsMenu.%s", touch_parts[i]);
        u32 ch = find(pl, p);
        if (!ch)
            continue;
        if (!cfg.touch || display_page) {
            if (*(volatile u8 *)GP(ch + SWF_VISIBLE)) {
                set_visible(ch, 0);
                hidden_by_us[i] = ch;
            }
        } else if (hidden_by_us[i]) {
            if (hidden_by_us[i] == ch)
                set_visible(ch, 1);
            hidden_by_us[i] = 0;
        }
    }
    for (int i = 0; key_rows[i]; i++) {
        snprintf(p, sizeof p, "ControlsMenu.%s", key_rows[i]);
        set_visible(find(pl, p), !cfg.touch && !keys_page && !display_page);
    }
    set_visible(find(pl, "ControlsMenu.PCKeys"), !cfg.touch && keys_page && !display_page);
    set_visible(find(pl, "ControlsMenu.PCTouch"), !display_page);
    set_visible(find(pl, "ControlsMenu.PCDisplay"), display_page);
    if (display_page)
        display_menu_layout(pl);
    if (keymap_version() != shown_keys)
        keys_fill(pl);
}

static void (*ctl_enter_fn)(CPU *), (*ctl_update_fn)(CPU *), (*ctl_event_fn)(CPU *);
static int enter_logs, update_logs;

static void ctl_enter_hook(CPU *c)
{
    u32 self = c->r[0];
    ctl_enter_fn(c);
    u32 pl = rd32(self + 4);
    if (enter_logs++ < 20)
        rt_log("[pcctl] Controls menu entered (frame %u): Touch Screen %s\n", eagl_frames(), cfg.touch ? "on" : "off");
    keys_page = 0;
    display_page = display_take_page();
    keymap_cancel();
    if (pl) {
        menu_fill(pl);
        menu_layout(pl);
    }
}

static void ctl_update_hook(CPU *c)
{
    u32 self = c->r[0];
    ctl_update_fn(c);
    u32 pl = rd32(self + 4);
    if (update_logs++ < 3)
        rt_log("[pcctl] Controls menu update (frame %u)\n", eagl_frames());
    if (pl) {
        if (pl != shown_player)
            menu_fill(pl);
        menu_layout(pl);
    }
}

static void ctl_event_hook(CPU *c)
{
    u32 self = c->r[0], ev = c->r[1], pl = rd32(self + 4);
    int kind = (int)rd32(ev + 8);
    u32 path = rt_call1(GT_FN_EVENT_PATH, ev);
    const char *ps = path ? (const char *)GP(path) : "";
    const char *row = strstr(ps, "PCTouch.") ? "PCTouch" : strstr(ps, "PCTilt.") ? "PCTilt"
                    : strstr(ps, "PCGas.") ? "PCGas" : strstr(ps, "PCGear.") ? "PCGear"
                    : strstr(ps, "PCKeysOpen.") ? "PCKeysOpen" : strstr(ps, "PCKeys.") ? "PCKeys"
                    : strstr(ps, "PCDisplay.") ? "PCDisplay" : NULL;
    static int logs;
    if (!row) {
        if (logs++ < 60 && kind == 6)
            rt_log("[pcctl] click on %s (the game's)\n", ps);
        ctl_event_fn(c);
        return;
    }
    if (logs++ < 60 && kind == 6)
        rt_log("[pcctl] click on %s\n", ps);
    rt_call2(GT_FN_MENU_BASE_EVENT, self, ev);
    if (kind != 6 || !pl)
        return;
    if (!strcmp(row, "PCDisplay")) {
        display_menu_click(pl, ps);
        return;
    }
    if (!strcmp(row, "PCKeysOpen")) {
        keymap_cancel();
        keys_page = 1;
        menu_layout(pl);
        rt_log("[pcctl] Keyboard / Gamepad page\n");
        return;
    }
    if (!strcmp(row, "PCKeys")) {
        const char *cell = strstr(ps, "PCKeys.Cell") ? strstr(ps, "PCKeys.Cell") + 11 : "";
        if (!strncmp(cell, "Back", 4)) {
            keymap_cancel();
            keys_page = 0;
            menu_layout(pl);
        } else if (!strncmp(cell, "Default", 7))
            keymap_defaults();
        else if (cell[0] >= '1' && cell[0] <= '9' && cell[1] >= '1' && cell[1] <= '0' + KM_KEYS + 1)
            keymap_capture(cell[0] - '1', cell[1] - '1');
        menu_layout(pl);
        return;
    }
    u32 s = settings();
    int v = -1;
    if (!strcmp(row, "PCTouch")) {
        set_touch(!cfg.touch);
        v = cfg.touch;
        keys_page = 0;
        keymap_cancel();
        menu_layout(pl);
    } else if (!strcmp(row, "PCTilt") && s) {
        v = !camera_tilt();
        *(volatile u8 *)GP(s + SET_LEVEL_HORIZON) = (u8)v;
        rt_log("[pcctl] Camera Tilt %s (settings +0x70 = %d)\n", v ? "on" : "off", v);
    } else if (!strcmp(row, "PCGas")) {
        v = cfg.autogas = !cfg.autogas;
        keys_apply();
        cfg_save();
        rt_log("[pcctl] Auto Gas %s\n", v ? "on" : "off");
    } else if (!strcmp(row, "PCGear")) {
        cfg.manual = !cfg.manual;
        keys_apply();
        cfg_save();
        set_text(pl, "PCGear.txtSelectorType", cfg.manual ? "Manual" : "Automatic");
        rt_log("[pcctl] Transmission %s\n", cfg.manual ? "manual" : "automatic");
    }
    if (v >= 0) {
        u32 btn = rd32(ev);
        u32 slider = btn ? rt_call3(GT_FN_SWF_CHILD, pl, GA("SliderOnOff"), btn) : 0;
        if (slider)
            rt_call4(SWF_PLAY_LABEL, pl, slider, GA(v ? "On" : "Off"), 1);
        else
            switch_rest(pl, row, v);
    }
}

void pcctl_init(void)
{
    keymap_init();
    display_init();
    const char *e = getenv("GT_PC_CONTROLS");
    on = !(e && *e == '0');
    if (!on) {
        rt_log("[pcctl] off (GT_PC_CONTROLS=0): the original Controls menu\n");
        return;
    }
    cfg_load();
    if (cfg.touch < 0) {
        const char *t = getenv("GT_TOUCH_SCREEN");
        cfg.touch = t && *t == '1';
        rt_log("[pcctl] no saved Touch Screen choice: %s by default\n", cfg.touch ? "on (GT_TOUCH_SCREEN=1)" : "off");
    }
    gt_hook_slot(CTL_VTBL + 0x14, CTL_ENTER_FN, &ctl_enter_fn, ctl_enter_hook, "pcctl ctl_enter_hook");
    gt_hook_slot(CTL_VTBL + 0x1c, CTL_UPDATE_FN, &ctl_update_fn, ctl_update_hook, "pcctl ctl_update_hook");
    gt_hook_slot(CTL_VTBL + 0x2c, CTL_EVENT_FN, &ctl_event_fn, ctl_event_hook, "pcctl ctl_event_hook");
    rt_log("[pcctl] Touch Screen %s (%s)\n", cfg.touch ? "on" : "off: keyboard / gamepad",
           cfg.touch ? "the game's Controls settings" : cfg.manual ? "manual" : "automatic");
}

static void camera_tilt_frame(u32 s)
{
    static int applied;
    if (!s || in_race())
        return;
    u8 *b = (u8 *)GP(s + SET_LEVEL_HORIZON);
    if (!applied) {
        const char *top = padmenu_top();
        if (cfg.touch || !top || (strcmp(top, "MainMenu") && strcmp(top, "NavigationMenu")))
            return;
        if (cfg.tilt < 0) {
            const char *d = getenv("GT_CAMERA_TILT_DEFAULT");
            cfg.tilt = d && *d == '1';
            rt_log("[pcctl] no saved Camera Tilt choice: %s by default (the save had %d)\n",
                   cfg.tilt ? "on (GT_CAMERA_TILT_DEFAULT=1)" : "off", *b);
            cfg_save();
        }
        if (*b != (u8)cfg.tilt)
            rt_log("[pcctl] Camera Tilt %s (settings +0x70 %d -> %d)\n", cfg.tilt ? "on" : "off", *b, cfg.tilt);
        *(volatile u8 *)b = (u8)cfg.tilt;
        applied = 1;
        return;
    }
    if ((*b != 0) != cfg.tilt) {
        cfg.tilt = *b != 0;
        rt_log("[pcctl] Camera Tilt %s: kept in pc_controls.txt\n", cfg.tilt ? "on" : "off");
        cfg_save();
    }
}

void pcctl_frame(void)
{
    if (on <= 0)
        return;
    u32 s = settings();
    if (!cfg.touch && cfg.touch_steer < 0) {
        const char *top = padmenu_top();
        if (!s || in_race() || !top || (strcmp(top, "MainMenu") && strcmp(top, "NavigationMenu")))
            return;
        cfg.touch_steer = (int)rd32(s + SET_STEERING);
        cfg.touch_throttle = (int)rd32(s + SET_THROTTLE);
        cfg.touch_gearbox = (int)rd32(s + SET_GEARBOX);
        rt_log("[pcctl] the save's touch settings kept (steering %d, throttle %d, gearbox %d); keyboard / gamepad "
               "mode now\n", cfg.touch_steer, cfg.touch_throttle, cfg.touch_gearbox);
        cfg_save();
    }
    camera_tilt_frame(s);
    if (s && !in_race()) {
        if (restore_after_race) {
            restore_after_race = 0;
            touch_restore(s);
        } else if (cfg.touch) {
            cfg.touch_steer = (int)rd32(s + SET_STEERING);
            cfg.touch_throttle = (int)rd32(s + SET_THROTTLE);
            cfg.touch_gearbox = (int)rd32(s + SET_GEARBOX);
        }
    }
    keys_apply();
}

int pcctl_keys_mode(void) { return on > 0 && !cfg.touch; }
