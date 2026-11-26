#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define MENU_HOST 0x7a2564u
#define REPLAY_STATE 13

#define PINCH_ACTIVE 0x7a273cu
#define PINCH_DELTA 0x7a2748u

float ui_zoom_wheel;

static int option(const char *env)
{
    const char *e = getenv(env);
    return !(e && *e == '0');
}

static u32 replay_race(void)
{
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    return race && (int)rd32(race + 0x1d84) == REPLAY_STATE ? race : 0;
}

int menu_shown(u32 pl)
{
    if (rd32(pl + 0xf8) & 0x80)
        return 1;
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    int st = race ? (int)rd32(race + 0x1d84) : -1;
    if (!g || (race && st != 12 && st != REPLAY_STATE)
        || (pl != rd32(g + G_RACE_MENUS) && (race || pl != rd32(g + G_PRIZE_MENUS))))
        return 0;
    int n = (int)rd32(pl + 0x118);
    u32 arr = rd32(pl + 0x114), e = n > 0 && arr ? rd32(arr + 4 * (n - 1)) : 0;
    return e && rd32(e + 0x58) == 1 && strncmp((const char *)GP(e + 8), "PauseMenu", 9);
}

static u32 menu_player(const char **top)
{
    *top = NULL;
    u32 host = rd32(MENU_HOST), pl = host ? rd32(host + 0x1b2c) : 0;
    if (!pl || !rd32(pl + 0x3c) || !rd32(pl + 0x40) || !menu_shown(pl))
        return 0;
    int n = (int)rd32(pl + 0x118);
    u32 arr = rd32(pl + 0x114);
    if (n > 0 && arr) {
        u32 e = rd32(arr + 4 * (n - 1));
        if (e)
            *top = (const char *)GP(e + 8);
    }
    return pl;
}

const char *menu_top(void)
{
    const char *top;
    return menu_player(&top) ? top : NULL;
}

static void replay_undim(u32 race)
{
    static int on = -1, done;
    if (on < 0) {
        on = option("GT_REPLAY_UNDIM");
        rt_log("[replay] grey overlay %s during replays\n", on ? "hidden" : "kept (original)");
    }
    if (!on)
        return;
    const char *top;
    u32 pl = race ? menu_player(&top) : 0;
    if (!pl || !top || strcmp(top, "ReplayMenu")) {
        done = 0;
        return;
    }
    if (done)
        return;
    done = 1;
    static const char path[] = "LowBarMenu.btnGreyOverlay", label[] = "Hide";
    if (!rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path)))
        return;
    rt_call4(GT_FN_SWF_GOTO_LABEL, pl, GA((void *)path), GA((void *)label), 1);
    rt_log("[replay] %s -> %s\n", path, label);
}

static float zoom_pending;
static int zoom_logs;
static int hint_turn_frames;
static float hint_turn_from;

static void replay_zoom(u32 race)
{
    static int on = -1;
    if (on < 0)
        on = option("GT_REPLAY_ZOOM");
    float notches = ui_zoom_wheel;
    ui_zoom_wheel = 0;
    u32 g = gt_game();
    int view = g ? (int)rd32(g + G_REPLAY_VIEW) : -1;
    if (!on || !race || view < 4 || view > 7 || *(volatile u8 *)GP(PINCH_ACTIVE)) {
        if (fabsf(notches) >= 1 && on && race && zoom_logs++ < 40)
            rt_log("[replay] wheel %+.0f ignored: replay view %d does not zoom (4 .. 7 do)\n", notches, view);
        zoom_pending = 0;
        return;
    }
    if (notches != 0) {
        int screen = (int)rd32(GT_SCREEN_PX_H);
        float range = 0.8f * (float)(screen > 0 ? screen : 1024);
        zoom_pending += notches * range / 10.0f;
        if (fabsf(notches) >= 1 && zoom_logs++ < 40)
            rt_log("[replay] wheel %+.0f: zoom %d (range %.0f), replay view %d\n", notches,
                   (int)rd32(race + 0xac4), range, view);
    }
}

#define REPLAY_UPDATE_SLOT 0x782edcu
#define REPLAY_UPDATE_FN 0x139c94u
static void (*replay_update_fn)(CPU *);

static void replay_update_hook(CPU *c)
{
    if (zoom_pending == 0 || *(volatile u8 *)GP(PINCH_ACTIVE)) {
        replay_update_fn(c);
        return;
    }
    float step = zoom_pending * 0.25f;
    if (fabsf(step) < 2.0f)
        step = fabsf(zoom_pending) < 2.0f ? zoom_pending : (zoom_pending > 0 ? 2.0f : -2.0f);
    zoom_pending -= step;
    u32 saved = rd32(PINCH_DELTA);
    wr32(PINCH_DELTA, (u32)(int)lrintf(-step));
    replay_update_fn(c);
    wr32(PINCH_DELTA, saved);
    u32 race = zoom_pending == 0 ? replay_race() : 0;
    if (race && zoom_logs <= 40)
        rt_log("[replay] zoom now %d\n", (int)rd32(race + 0xac4));
}

void replay_init(void)
{
    if (!gt_hook_slot(REPLAY_UPDATE_SLOT, REPLAY_UPDATE_FN, &replay_update_fn, replay_update_hook,
                      "replay replay_update_hook"))
        rt_log("[replay] no wheel zoom\n");
}

static void test_wheel(void)
{
    static const char *list;
    if (!list)
        list = dev_getenv("GT_WHEEL") ? dev_getenv("GT_WHEEL") : "";
    int f;
    float n;
    while (*list && sscanf(list, "%d:%f", &f, &n) == 2 && (int)eagl_frames() >= f) {
        ui_zoom_wheel += n;
        dev_log("[replay] test wheel %+.0f at frame %u\n", n, eagl_frames());
        const char *s = strchr(list, ';');
        list = s ? s + 1 : "";
    }
}

#define CH_VISIBLE 0xab
#define SPR_LIST 0xb8

static u32 icons_hidden[8];
static int nicons;

static int unnamed(u32 ch)
{
    u8 tag = *(u8 *)GP(ch + 0x44);
    const char *n = tag == 0xff ? (const char *)GP(rd32(ch + 0x50)) : (const char *)GP(ch + 0x45);
    return !*n;
}

static void hide_icon(u32 ch)
{
    volatile u8 *v = (volatile u8 *)GP(ch + CH_VISIBLE);
    if (!*v)
        return;
    *v = 0;
    for (int i = 0; i < nicons; i++)
        if (icons_hidden[i] == ch)
            return;
    if (nicons < 8)
        icons_hidden[nicons++] = ch;
}

static void replay_touch_icons(u32 race)
{
    static int on = -1;
    if (on < 0)
        on = option("GT_REPLAY_TOUCH_ICONS");
    if (!on)
        return;
    const char *top;
    u32 pl = race ? menu_player(&top) : 0;
    if (pcctl_keys_mode()) {
        if (!pl || !top || strcmp(top, "ReplayMenu"))
            return;
        static const char cam[] = "ReplayMenu.btnCamera", menu[] = "ReplayMenu";
        u32 ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)cam)), rm = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)menu));
        int had = nicons;
        if (ch)
            hide_icon(ch);
        u32 arr = rm ? rd32(rm + SPR_LIST) : 0;
        int n = rm ? (int)rd32(rm + SPR_LIST + 4) : 0;
        for (int i = 0; arr && i < n && i < 64; i++) {
            u32 k = rd32(arr + 4 * i);
            if (k && unnamed(k))
                hide_icon(k);
        }
        if (nicons != had)
            rt_log("[replay] Touch Screen off: the replay's camera, pinch and swipe icons hidden\n");
        return;
    }
    if (!nicons)
        return;
    static const char menu[] = "ReplayMenu";
    u32 host = rd32(MENU_HOST), mp = host ? rd32(host + 0x1b2c) : 0;
    u32 rm = mp && rd32(mp + 0x3c) && rd32(mp + 0x40) ? rt_call2(GT_FN_SWF_FIND, mp, GA((void *)menu)) : 0;
    for (int i = 0; i < nicons; i++) {
        u32 ch = icons_hidden[i];
        if (rm && rd32(ch + 0x40) == rm)
            *(volatile u8 *)GP(ch + CH_VISIBLE) = 1;
    }
    nicons = 0;
    rt_log("[replay] Touch Screen on: the replay's touch icons shown again\n");
}

#define SEEK_STEP 40

void ui_pad_drag(float dx, float dy);
int ui_pad_dragging(void);
void ui_pad_tap(float x, float y);

static int stage_box(u32 ch, float b[4])
{
    static float rect[4];
    rect[0] = 1, rect[1] = 0;
    rt_call2(rd32(rd32(ch) + 0x12c), ch, GA(rect));
    if (!(rect[0] < rect[1] && rect[2] <= rect[3]))
        return 0;
    float m[6] = {1, 0, 0, 0, 1, 0};
    u32 parent = rd32(ch + 0x40), w = parent ? rt_call1(GT_FN_WORLD_MATRIX, parent) : 0;
    if (w)
        memcpy(m, GP(w), sizeof m);
    b[0] = b[2] = 1e30f, b[1] = b[3] = -1e30f;
    for (int k = 0; k < 4; k++) {
        float u = rect[k & 1], v = rect[2 + (k >> 1)];
        float x = m[0] * u + m[1] * v + m[2], y = m[3] * u + m[4] * v + m[5];
        b[0] = x < b[0] ? x : b[0], b[1] = x > b[1] ? x : b[1];
        b[2] = y < b[2] ? y : b[2], b[3] = y > b[3] ? y : b[3];
    }
    return 1;
}

static void replay_seek(u32 pl, int dir)
{
    u32 g = gt_game(), r = g ? g + G_REPLAY : 0;
    if (!r)
        return;
    int start = (int)rd32(r + 0x2580), now = (int)rd32(r + 0x2584), len = (int)rd32(r + 0x2588);
    static const char path[] = "ReplayMenu.ReplayBar.ReplayBarFull.btnTimeSlider";
    u32 sl = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path));
    float b[4];
    if (len <= 0 || !sl || !stage_box(sl, b) || b[1] < 0 || b[0] > 20480 || b[2] > 15360) {
        rt_log("[replay] pad %s 10 s: no slider on the screen (length %d)\n", dir > 0 ? "on" : "back", len);
        return;
    }
    if (ui_pad_dragging())
        return;
    int t = now - start + dir * SEEK_STEP;
    t = t < 0 ? 0 : t > len ? len : t;
    float frac = ((float)t + 0.5f) / (float)len;
    float xt = b[0] + frac * (b[1] - b[0]);
    xt = xt < b[0] + 20 ? b[0] + 20 : xt > b[1] - 20 ? b[1] - 20 : xt;
    float x = xt / 20, y = (b[2] + b[3]) / 2 / 20;
    ui_pad_tap(x, y);
    rt_log("[replay] pad %s 10 s: %d:%02d -> %d:%02d of %d:%02d (slider tap at %.0f, %.0f)\n", dir > 0 ? "on" : "back",
           (now - start) / 240, (now - start) / 4 % 60, t / 240, t / 4 % 60, len / 240, len / 4 % 60, x, y);
}

static const char *pad_test(void)
{
    static const char *env;
    static char keys[16];
    if (!env)
        env = dev_getenv("GT_REPLAY_PADTEST") ? dev_getenv("GT_REPLAY_PADTEST") : "";
    int now = (int)eagl_frames();
    keys[0] = 0;
    for (const char *p = env; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        int from, to;
        char k[8];
        if (sscanf(p, "%d:%d:%7[LRUDiofb]", &from, &to, k) == 3 && now >= from && now <= to &&
            strlen(keys) + strlen(k) < sizeof keys)
            strcat(keys, k);
    }
    return keys;
}

void replay_pad(float rx, float ry, int zoom_in, int zoom_out, int on, int back)
{
    static int opt = -1;
    static float speed;
    if (opt < 0) {
        opt = option("GT_REPLAY_PAD");
        speed = rt_setting_float("GT_REPLAY_PAD_SPEED", 14, 1, 100);
    }
    const char *k = pad_test();
    if (*k) {
        rx += (strchr(k, 'R') != NULL) - (strchr(k, 'L') != NULL);
        ry += (strchr(k, 'D') != NULL) - (strchr(k, 'U') != NULL);
        zoom_in |= strchr(k, 'i') != NULL, zoom_out |= strchr(k, 'o') != NULL;
        on |= strchr(k, 'f') != NULL, back |= strchr(k, 'b') != NULL;
    }
    {
        float kx, ky;
        if (numpad_dir(&kx, &ky)) {
            float n = sqrtf(kx * kx + ky * ky);
            rx += kx / n, ry -= ky / n;
        }
        const bool *ks = SDL_GetKeyboardState(NULL);
        if (ks) {
            zoom_in |= ks[SDL_SCANCODE_KP_PLUS], zoom_out |= ks[SDL_SCANCODE_KP_MINUS];
            on |= ks[SDL_SCANCODE_KP_MULTIPLY], back |= ks[SDL_SCANCODE_KP_DIVIDE];
        }
    }
    const char *top = NULL;
    u32 pl = opt && replay_race() ? menu_player(&top) : 0;
    int in_replay = pl && top && !strcmp(top, "ReplayMenu");
    float m = sqrtf(rx * rx + ry * ry);
    static int turning, turn_logs;
    if (!in_replay || m < 0.2f) {
        if (!hint_turn_frames)
            ui_pad_drag(0, 0);
        turning = 0;
    } else {
        float a = ((m > 1 ? 1 : m) - 0.2f) / 0.8f, v = speed * a * a / m;
        ui_pad_drag(rx * v, ry * v);
        if (!turning++ && turn_logs++ < 20) {
            u32 g = gt_game();
            rt_log("[replay] pad: right stick turns the camera (replay view %d)\n", g ? (int)rd32(g + G_REPLAY_VIEW) : -1);
        }
    }
    static int zoom_was;
    int z = in_replay ? (zoom_in && !zoom_out ? 1 : zoom_out && !zoom_in ? -1 : 0) : 0;
    if (z) {
        ui_zoom_wheel += 0.15f * (float)z;
        if (z != zoom_was && zoom_logs++ < 40) {
            u32 g = gt_game();
            rt_log("[replay] pad zoom %s (replay view %d)\n", z > 0 ? "in" : "out", g ? (int)rd32(g + G_REPLAY_VIEW) : -1);
        }
    }
    zoom_was = z;
    static int seek_was, next_at;
    int now = (int)eagl_frames();
    int dir = in_replay ? (on && !back ? 1 : back && !on ? -1 : 0) : 0;
    if (dir && (dir != seek_was || now >= next_at)) {
        next_at = now + (dir != seek_was ? 30 : 15);
        replay_seek(pl, dir);
    }
    seek_was = dir;
}

void replay_hint(int which)
{
    u32 race = replay_race(), g = gt_game();
    int view = g ? (int)rd32(g + G_REPLAY_VIEW) : -1;
    if (!race)
        return;
    if (which == 1) {
        int screen = (int)rd32(GT_SCREEN_PX_H);
        float range = 0.8f * (float)(screen > 0 ? screen : 1024), now = (float)(int)rd32(race + 0xac4) + zoom_pending;
        if (view < 4 || view > 7) {
            rt_log("[replay] pinch hint: replay view %d does not zoom (4 .. 7 do)\n", view);
            return;
        }
        float step = range / 4, level = floorf(now / step + 0.5f);
        float to = level >= 4 ? 0 : fminf((level + 1) * step, range);
        zoom_pending += to - now;
        rt_log("[replay] pinch hint: zoom %.0f -> %.0f of %.0f (replay view %d)\n", now, to, range, view);
    } else {
        if (view < 4 || view > 7) {
            rt_log("[replay] swipe hint: replay view %d does not turn (4 .. 7 do)\n", view);
            return;
        }
        float yaw;
        memcpy(&yaw, GP(race + 0xac0), 4);
        hint_turn_from = yaw;
        hint_turn_frames = 120;
        rt_log("[replay] swipe hint: turning the camera 90 degrees (yaw %.2f, replay view %d)\n", yaw, view);
    }
}

static void replay_hint_turn(u32 race)
{
    if (!hint_turn_frames)
        return;
    float yaw;
    memcpy(&yaw, GP(race + 0xac0), 4);
    const float quarter = 1.5707963f, full = 6.2831853f;
    float d = fmodf(fabsf(yaw - hint_turn_from), full);
    if (d > full / 2)
        d = full - d;
    if (d >= quarter || !--hint_turn_frames) {
        ui_pad_drag(0, 0);
        rt_log("[replay] swipe hint: turned %.0f degrees in %d frames (yaw %.2f)\n", d * 57.29578f,
               120 - hint_turn_frames, yaw);
        hint_turn_frames = 0;
        return;
    }
    ui_pad_drag(d > 0.8f * quarter ? 6.0f : 14.0f, 0);
}

void replay_frame(void)
{
    test_wheel();
    u32 race = replay_race();
    replay_undim(race);
    replay_zoom(race);
    replay_touch_icons(race);
    if (race)
        replay_hint_turn(race);
    else
        hint_turn_frames = 0;
}
