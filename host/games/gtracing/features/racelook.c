#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define PI_F 3.14159265f

int steer_racing(void);
int keymap_right_stick_bound(void);

static int opt = -1;
static float smooth_s;
static float target;
static int asked;
static float shown;
static LARGE_INTEGER t_last;

static const char *look_test(void)
{
    static const char *env;
    static char keys[12];
    if (!env)
        env = dev_getenv("GT_RACE_LOOKTEST") ? dev_getenv("GT_RACE_LOOKTEST") : "";
    int now = (int)eagl_frames();
    keys[0] = 0;
    for (const char *p = env; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        int from, to;
        char k[10];
        if (sscanf(p, "%d:%d:%9[0-9]", &from, &to, k) == 3 && now >= from && now <= to &&
            strlen(keys) + strlen(k) < sizeof keys)
            strcat(keys, k);
    }
    return keys;
}

int numpad_dir(float *x, float *y)
{
    static const signed char dir[10][2] = {
        {0, 0}, {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    static const SDL_Scancode kp[10] = {SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
                                        SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
                                        SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9};
    const bool *ks = SDL_GetKeyboardState(NULL);
    const char *test = look_test();
    float kx = 0, ky = 0;
    for (int d = 1; d <= 9; d++)
        if ((ks && ks[kp[d]]) || strchr(test, '0' + d))
            kx += dir[d][0], ky += dir[d][1];
    *x = kx, *y = ky;
    return kx != 0 || ky != 0;
}

void race_look_input(float rx, float ry)
{
    if (opt < 0) {
        opt = !(getenv("GT_RACE_LOOK") && *getenv("GT_RACE_LOOK") == '0');
        smooth_s = rt_setting_float("GT_RACE_LOOK_SMOOTH", 60, 0, 2000) / 1000.0f;
        rt_log("[look] race chase views: right stick / numpad look around %s (eased %.0f ms)\n",
               opt ? "on" : "off (GT_RACE_LOOK=0)", smooth_s * 1000);
    }
    asked = 0;
    if (!opt || !steer_racing())
        return;
    float kx, ky;
    if (numpad_dir(&kx, &ky)) {
        target = atan2f(kx, ky);
        asked = 1;
        return;
    }
    if (keymap_right_stick_bound())
        return;
    if (rx * rx + ry * ry > 0.35f * 0.35f) {
        target = atan2f(rx, -ry);
        asked = 1;
    }
}

float gt_race_look(u32 cam)
{
    u32 g = gt_game(), race = g ? rd32(g + G_RACE) : 0;
    int view = (int)rd32(cam + 0x23c);
    if (!race || rd32(race + 0x10a8) != cam)
        return 0;
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = t_last.QuadPart ? (float)(t.QuadPart - t_last.QuadPart) / (float)f.QuadPart : 0.0f;
    t_last = t;
    int on = opt > 0 && !*(volatile u8 *)GP(g + G_REPLAY_ON) && view >= 4 && view <= 6 && steer_racing();
    float want = on && asked ? target : 0.0f;
    static int was, logs;
    if (!on) {
        shown = 0;
        was = 0;
        return 0;
    }
    float d = remainderf(want - shown, 2 * PI_F);
    if (smooth_s <= 0 || dt <= 0 || dt > 0.25f)
        shown += d;
    else
        shown += d * (1.0f - expf(-dt / smooth_s));
    if (fabsf(remainderf(want - shown, 2 * PI_F)) < 0.002f)
        shown = want;
    shown = remainderf(shown, 2 * PI_F);
    if (asked != was && logs++ < 40)
        rt_log("[look] %s (view %d)\n", asked ? "look" : "ahead again", view);
    if (asked && logs < 40 && fabsf(want - shown) < 0.002f) {
        static float logged = 99;
        if (logged != want) {
            logged = want;
            logs++;
            rt_log("[look] %.0f deg to the right of ahead\n", want * 180 / PI_F);
        }
    }
    was = asked;
    return -shown;
}
