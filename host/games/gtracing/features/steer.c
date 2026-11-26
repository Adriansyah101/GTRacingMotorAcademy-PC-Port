#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define CONTROL_VTBL 0x7859c4u
#define CONTROL_FN 0x1b37f0u

#define WHEEL_DEGREES 90.0f
#define WHEEL_RIGHT 1.0f

static void (*control_fn)(CPU *);
static int wheel_owned;
static int btn_held[2];

static int steering_type(void)
{
    if (!*(volatile u8 *)GP(rd32(GT_SETTINGS_GUARD)))
        return -1;
    return (int)rd32(rd32(GT_SETTINGS) + 0x5c);
}

static void slot_set(u32 g, int i, int held, float v)
{
    *(volatile u8 *)GP(g + 0x1b60 + 8 * i) = (u8)held;
    *(volatile float *)GP(g + 0x1b64 + 8 * i) = v;
}

static void key_event(u32 g, int press, u32 bit)
{
    u32 in = rd32(g + 0x17a4);
    u32 k = in ? rd32(in + 4) : 0;
    if (!k)
        return;
    if (press)
        wr32(k + 0x14, rd32(k + 0x14) & ~bit);
    u32 a = k + (press ? 0x10 : 0x14);
    wr32(a, rd32(a) | bit);
}

static int settings_int(u32 off) { return steering_type() < 0 ? -1 : (int)rd32(rd32(GT_SETTINGS) + off); }

static const struct { u32 flag, val, bit; } hud_keys[4] = {
    {0x1b50, 0x1b54, 8}, {0x1b58, 0x1b5c, 4}, {0x1b70, 0, 0x10000}, {0x1b78, 0, 0x20000}};
static int hud_held[4], slider_owned;

static void hud_key(u32 g, int i, int want)
{
    if (want == hud_held[i])
        return;
    hud_held[i] = want;
    *(volatile u8 *)GP(g + hud_keys[i].flag) = (u8)want;
    if (hud_keys[i].val)
        *(volatile float *)GP(g + hud_keys[i].val) = want ? 1.0f : 0.0f;
    key_event(g, want, hud_keys[i].bit);
}

static void pedals_apply(u32 g)
{
    int throttle = settings_int(0x58), manual = settings_int(0x60) == 0;
    int pedals = throttle == 0 || throttle == 1;
    hud_key(g, 0, pedals && ui_gas);
    hud_key(g, 1, (pedals || throttle == 4) && ui_brake);
    static int gear_wait[2];
    for (int i = 0; i < 2; i++) {
        if (ui_gear_tap & (1 << i))
            gear_wait[i] = 30;
        else if (gear_wait[i] > 0 && (hud_held[2 + i] && !*(volatile u8 *)GP(g + hud_keys[2 + i].flag))) {
            gear_wait[i] = 0;
            if (dev_getenv("GT_STEER"))
                rt_log("[steer] gear %s taken\n", i ? "down" : "up");
        } else if (gear_wait[i] > 0 && --gear_wait[i] == 0 && dev_getenv("GT_STEER"))
            rt_log("[steer] gear %s dropped (not taken in 30 updates)\n", i ? "down" : "up");
        hud_key(g, 2 + i, manual && gear_wait[i] > 0);
    }
    ui_gear_tap = 0;

    float s = throttle == 2 ? 100.0f * (float)(ui_gas - ui_brake) : 0.0f;
    if (s != 0.0f) {
        *(volatile u8 *)GP(g + 0x1b50) = 1;
        *(volatile float *)GP(g + 0x1b54) = s;
        slider_owned = 1;
    } else if (slider_owned) {
        *(volatile u8 *)GP(g + 0x1b50) = 0;
        *(volatile float *)GP(g + 0x1b54) = 0.0f;
        slider_owned = 0;
    }
}

static void steer_apply(void)
{
    u32 g = gt_game();
    if (!g)
        return;
    int type = steering_type();
    float t = ui_tilt;
    int active = ui_steer_active;

    if (type == 1) {
        if (active) {
            slot_set(g, 0, 1, t * WHEEL_RIGHT * WHEEL_DEGREES);
            wheel_owned = 1;
        } else if (wheel_owned) {
            slot_set(g, 0, 0, 0.0f);
            wheel_owned = 0;
        }
    } else if (wheel_owned) {
        wheel_owned = 0;
    }

    if (type == 0) {
        int want[2] = {active && ui_steer_dir < 0, active && ui_steer_dir > 0};
        for (int i = 0; i < 2; i++) {
            if (want[i] == btn_held[i])
                continue;
            btn_held[i] = want[i];
            slot_set(g, i, want[i], want[i] ? 1.0f : 0.0f);
            key_event(g, want[i], 1u << i);
        }
    } else {
        btn_held[0] = btn_held[1] = 0;
    }
    pedals_apply(g);
}

static void force_setting(const char *env, u32 off, int *forced)
{
    if (*forced == -2)
        *forced = dev_getenv(env) ? rt_parse_int(dev_getenv(env), -1, -0x7fffffff, 0x7fffffff) : -1;
    if (*forced >= 0 && settings_int(off) >= 0 && settings_int(off) != *forced) {
        wr32(rd32(GT_SETTINGS) + off, (u32)*forced);
        dev_log("[steer] setting +0x%x forced to %d (%s)\n", off, *forced, env);
    }
}

#define ACCEL_INSTANCE 0x79d648u
#define ACCEL_IVAR_X 0x75c83cu

static void tilt_feed(void)
{
    u32 a = rd32(ACCEL_INSTANCE);
    if (!a)
        return;
    double g[3];
    ui_gravity(g);
    for (int i = 0; i < 3; i++)
        *(volatile float *)GP(a + rd32(ACCEL_IVAR_X + 4 * i)) = (float)g[i];
}

#define RACE_CAM_RESET 0x160e9cu
#define RACE_CAM_SET 0x19c6dcu
#define REPLAY_CAM_NEXT 0x129fa0u

static void camera_key(void)
{
    if (!ui_cam_tap)
        return;
    ui_cam_tap = 0;
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    if (!race || steering_type() < 0)
        return;
    int state = (int)rd32(race + 0x1d84);
    u32 cam = rd32(race + 0x10a8);
    if (state == 13) {
        if (*(u8 *)GP(g + 0x42dd))
            return;
        rt_call2(REPLAY_CAM_NEXT, g, 1);
        rt_log("[cam] replay camera -> %d\n", (int)rd32(g + G_REPLAY_VIEW));
    } else if (state >= 2 && state <= 8 && !*(u8 *)GP(g + G_REPLAY_ON)) {
        u32 s = rd32(GT_SETTINGS);
        wr32(s + 0x34, rd32(s + 0x34) + 1);
        rt_call1(RACE_CAM_RESET, race);
        int v = (int)rd32(s + 0x34);
        if (v > 6 || v <= 0)
            wr32(s + 0x34, v > 6 ? 1 : 6);
        if (cam)
            rt_call2(RACE_CAM_SET, cam, rd32(s + 0x34));
        rt_log("[cam] race camera -> %d\n", (int)rd32(s + 0x34));
    } else {
        rt_log("[cam] key ignored: race state %d, G+0x4179 %d\n", state, *(u8 *)GP(g + G_REPLAY_ON));
    }
}

static void rear_view(void)
{
    static int held;
    if (ui_rear == held)
        return;
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0, cam = race ? rd32(race + 0x10a8) : 0;
    if (ui_rear && (!cam || (int)rd32(race + 0x1d84) == 13))
        return;
    held = ui_rear;
    if (cam)
        wr32(cam + 0x248, held ? 1 : 2);
}

int steer_racing(void)
{
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    if (!race || steering_type() < 0)
        return 0;
    int state = (int)rd32(race + 0x1d84);
    return state >= 2 && state <= 8;
}

int steer_race_waiting(void)
{
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    if (!race || steering_type() < 0)
        return 0;
    int state = (int)rd32(race + 0x1d84);
    return state == 0 || state == 1;
}

#define VT_INPUT_DRIVER 0x7859b8u
#define VT_AI_DRIVER 0x7885e8u
static void test_autopilot(void)
{
    static int test = -1, last_kind = -1;
    if (test < 0) {
        const char *e = dev_getenv("GT_TEST_AUTOPILOT");
        test = e && *e == '1';
        if (test)
            dev_log("[steer] test: the player's car is driven by its AI driver (GT_TEST_AUTOPILOT)\n");
    }
    if (!test)
        return;
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    if (!race)
        return;
    u32 cnt = rd32(race + 0x15f4), pi = rd32(race + 0x15f0);
    if (cnt == 0 || cnt > 16 || pi >= cnt)
        return;
    u32 car = rd32(race + 0x15f8 + 4 * pi);
    u32 ai = car ? rd32(car + 0x8c4) : 0, input = car ? rd32(car + 0x8c8) : 0, act = car ? rd32(car + 0x8c0) : 0;
    int kind = !ai || !input || rd32(ai) != VT_AI_DRIVER || rd32(input) != VT_INPUT_DRIVER ? 3
               : act == ai                                                               ? 1
               : act == input                                                            ? 0
                                                                                         : 2;
    if (kind != last_kind)
        dev_log("[steer] test: the player's active driver is %s (lap %d)\n",
               kind == 1   ? "the AI driver"
               : kind == 0 ? "the input driver"
               : kind == 3 ? "the input driver (the car has no AI driver)"
                           : "another",
               car ? (s32)rd32(car + 0x4a4) : -1);
    last_kind = kind;
    if (kind == 0)
        wr32(car + 0x8c0, ai);
}

void steer_frame(void)
{
    test_autopilot();
    tilt_feed();
    camera_key();
    rear_view();
    replay_frame();
    pcctl_frame();
    static int steer = -2, throttle = -2, gearbox = -2;
    force_setting("GT_STEER_TYPE", 0x5c, &steer);
    force_setting("GT_THROTTLE_TYPE", 0x58, &throttle);
    force_setting("GT_GEARBOX", 0x60, &gearbox);
    static int tilt = -2;
    if (tilt == -2)
        tilt = dev_getenv("GT_CAMERA_TILT") ? rt_parse_int(dev_getenv("GT_CAMERA_TILT"), -1, -0x7fffffff, 0x7fffffff)
                                            : -1;
    if (tilt >= 0 && settings_int(0x5c) >= 0 && *(u8 *)GP(rd32(GT_SETTINGS) + 0x70) != (u8)tilt) {
        *(volatile u8 *)GP(rd32(GT_SETTINGS) + 0x70) = (u8)tilt;
        dev_log("[steer] Camera Tilt forced to %d (GT_CAMERA_TILT)\n", tilt);
    }
}

static u32 roll_cam;
static float roll_game, roll_mid, roll_shown;
static float roll_scale = -1, roll_tau;
static LARGE_INTEGER roll_t;

static void roll_before(u32 self)
{
    (void)self;
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0, cam = race ? rd32(race + 0x10a8) : 0;
    if (cam && cam == roll_cam)
        *(volatile float *)GP(cam + 0x224) = roll_game;
}

static void roll_after(void)
{
    if (roll_scale < 0) {
        roll_scale = rt_setting_float("GT_TILT_ROLL", 25, 0, 100) / 100.0f;
        roll_tau = rt_setting_float("GT_TILT_SMOOTH", 400, 0, 5000) / 2000.0f;
        rt_log("[steer] camera tilt: %.0f %% of the game's roll, smoothed over %.0f ms (GT_TILT_ROLL, GT_TILT_SMOOTH)\n",
               roll_scale * 100, roll_tau * 2000);
    }
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0, cam = race ? rd32(race + 0x10a8) : 0;
    if (!cam)
        return;
    float now_game = *(volatile float *)GP(cam + 0x224), target = now_game * roll_scale;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = roll_t.QuadPart ? (float)(t.QuadPart - roll_t.QuadPart) / (float)f.QuadPart : 0.0f;
    roll_t = t;
    if (cam != roll_cam || dt <= 0 || dt > 0.25f) {
        if (cam != roll_cam)
            roll_mid = roll_shown = target;
        roll_cam = cam;
        dt = dt > 0.25f ? 0.25f : dt;
    }
    roll_game = now_game;
    float a = roll_tau > 0 ? 1.0f - expf(-dt / roll_tau) : 1.0f;
    roll_mid += (target - roll_mid) * a;
    roll_shown += (roll_mid - roll_shown) * a;
    *(volatile float *)GP(cam + 0x224) = roll_shown;
    static int dbg = -1, n;
    if (dbg < 0)
        dbg = dev_getenv("GT_STEER") != NULL;
    if (GT_DEV && dbg && (n++ % 15 == 0) && (now_game != 0 || roll_shown != 0))
        rt_log("[steer] camera roll: game %.4f shown %.4f (tilt %.2f)\n", now_game, roll_shown, ui_tilt);
}

static void control_hook(CPU *c)
{
    u32 self = c->r[0];
    steer_apply();
    roll_before(self);
    control_fn(c);
    roll_after();
    static int n, dbg = -1;
    if (dbg < 0)
        dbg = dev_getenv("GT_STEER") != NULL;
    u32 g = gt_game(), car = rd32(self + 0xc);
    static int last_gear = -1;
    if (GT_DEV && dbg && car && (int)rd32(car + 0xa3c) != last_gear) {
        last_gear = (int)rd32(car + 0xa3c);
        rt_log("[steer] gear now %d (target %d, frame %u)\n", last_gear, (int)rd32(car + 0xa40), eagl_frames());
    }
    if (GT_DEV && dbg && g && car && n++ % 30 == 0) {
        rt_log("[steer] type %d slot0 %d %.1f slot1 %d %.1f tilt %.2f car angle %.3f\n", steering_type(),
               *(u8 *)GP(g + 0x1b60), *(float *)GP(g + 0x1b64), *(u8 *)GP(g + 0x1b68), *(float *)GP(g + 0x1b6c),
               ui_tilt, *(float *)GP(car + 0x2d8));
        rt_log("[steer] throttle %d gearbox %d keys g%d b%d u%d d%d -> flags %x gas %.0f brake %.0f gear %d\n",
               settings_int(0x58), settings_int(0x60), ui_gas, ui_brake, ui_gear_up, ui_gear_down, rd32(self + 0x98),
               *(float *)GP(self + 0x9c), *(float *)GP(self + 0xa0), (int)rd32(car + 0xa3c));
    }
}

#define REPLAY_EVENT_SLOT 0x7896a4u
#define REPLAY_EVENT_FN 0x29c558u
static void (*replay_event_fn)(CPU *);

static void replay_event_hook(CPU *c)
{
    u32 self = c->r[0], ev = c->r[1];
    int type = (int)rd32(ev + 8);
    u32 name = rd32(ev + 4), ctl = rd32(ev);
    float x = *(float *)GP(ev + 0xc), y = *(float *)GP(ev + 0x10);
    float rect[4] = {0, 0, 0, 0}, m[6] = {1, 0, 0, 0, 1, 0}, pm[6] = {1, 0, 0, 0, 1, 0}, sx = x, sy = y,
          px = x, py = y;
    int fixed = 0, slider = name && ctl && !strcmp((const char *)GP(name), "btnTimeSlider");
    if (slider && (type == 4 || type == 5)) {
        u32 sp = c->r[13], buf = (sp - 0x40) & ~7u, parent = rd32(ctl + 0x40);
        c->r[13] = sp - 0x100;
        u32 args[2] = {ctl, buf};
        rt_guest_call(c, rd32(rd32(ctl) + 0x12c), 2, args);
        memcpy(rect, GP(buf), sizeof rect);
        u32 world = rt_guest_call(c, GT_FN_WORLD_MATRIX, 1, args);
        if (world)
            memcpy(m, GP(world), sizeof m);
        if (parent) {
            args[0] = parent;
            u32 pworld = rt_guest_call(c, GT_FN_WORLD_MATRIX, 1, args);
            if (pworld)
                memcpy(pm, GP(pworld), sizeof pm);
        }
        c->r[13] = sp;
        sx = m[0] * x + m[1] * y + m[2], sy = m[3] * x + m[4] * y + m[5];
        float det = pm[0] * pm[4] - pm[1] * pm[3];
        if (det != 0) {
            float dx = sx - pm[2], dy = sy - pm[5];
            px = (pm[4] * dx - pm[1] * dy) / det, py = (pm[0] * dy - pm[3] * dx) / det;
        }
        float w = rect[1] - rect[0], h = rect[3] - rect[2];
        int dragging = type == 5 && *(u8 *)GP(self + 0x4e5);
        if (w > 0 && h > 0 && px >= rect[0] - w * 0.07f && px <= rect[1] + w * 0.07f &&
            (dragging || (py >= rect[2] - h * 0.5f && py <= rect[3] + h * 0.5f))) {
            *(float *)GP(ev + 0xc) = (px < rect[0] ? rect[0] : px > rect[1] ? rect[1] : px) - rect[0];
            *(float *)GP(ev + 0x10) = (py < rect[2] ? rect[2] : py > rect[3] ? rect[3] : py) - rect[2];
            fixed = 1;
        }
    }
    replay_event_fn(c);
    static int moves, others, dbg = -1;
    if (dbg < 0)
        dbg = dev_getenv("GT_STEER") != NULL;
    if (GT_DEV && dbg && slider && (type == 5 ? moves++ < 300 : others++ < 300))
        rt_log("[replay ui] type %d at (%.1f, %.1f) parent (%.1f, %.1f) bounds x %.1f..%.1f y %.1f..%.1f%s"
               " -> scrubbing %d\n", type, x, y, px, py, rect[0], rect[1], rect[2], rect[3],
               fixed ? " (made relative)" : "", *(u8 *)GP(self + 0x4e5));
}

void steer_init(void)
{
    replay_init();
    pcctl_init();
    exitbtn_init();
    hudhide_init();
    menu3d_init();
    p2p_init();
    topcars_init();
    board_init();
    gt_hook_slot(REPLAY_EVENT_SLOT, REPLAY_EVENT_FN, &replay_event_fn, replay_event_hook, "steer replay_event_hook");
    if (!gt_hook_slot(CONTROL_VTBL, CONTROL_FN, &control_fn, control_hook, "steer control_hook")) {
        rt_log("[steer] keys steer by tilt only\n");
        return;
    }
    rt_log("[steer] keys / pad drive the on-screen wheel and side buttons too\n");
}
