#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "features.h"
#include "game.h"

#define SCENE_CAM_SLOT 0x7885acu
#define SCENE_CAM_FN 0x250d54u
#define CAM_TABLE 0x7725e0u
#define CAM_COUNT 15
#define CAM_SIZE 0x34

#define SC_MOVING 0x105
#define SC_CAM_NOW 0x168
#define SC_CAM_AT 0x1fc

static int on = -1;
static int nav_top;
static u8 scene_was;
static u32 nav_pl;
static u32 scene;
static float swing_deg = 20.0f, swing_sec = 30.0f;
static float pivot[2] = {46, -128};
static void (*cam_fn)(CPU *);
static int upgrade_on, upgrade_top;
static void menu_car_remember(void);

static float cam[11] = {-1078.4f, 652.1f, 392.0f, -410.5f, 547.4f, 210.5f, 0.0122f, 0.0419f, -3.049f, 0.0157f, 61.6f};

static u32 cam_entry(const char *name)
{
    for (int i = 0; i < CAM_COUNT; i++) {
        u32 e = CAM_TABLE + CAM_SIZE * i, n = rd32(e);
        if (n && !strcmp((const char *)GP(n), name))
            return e;
    }
    return 0;
}

static const char cam_name[] = "PCMainMenu3D";

u32 menu3d_scene(void) { return scene; }

static void cam_hook(CPU *c)
{
    u32 self = c->r[0], how = c->r[2];
    scene = self;
    c->r[1] = p2p_menu_redirect(c->r[1]);
    c->r[1] = board_menu_redirect(c->r[1]);
    char name[64];
    snprintf(name, sizeof name, "%s", c->r[1] ? (const char *)GP(c->r[1]) : "");
    int nav = !strcmp(name, "NavigationMenu");
    upgrade_top = upgrade_on && !strcmp(name, "AftermarketMenu");
    rt_log("[menu3d] menu %s (how %u, back %u)%s\n", name, how, c->r[3], upgrade_top ? ": over the 3D scene" : "");
    u32 g = gt_game();
    if (on > 0 && g) {
        volatile u8 *f = (volatile u8 *)GP(g + G_MENU_SCENE);
        if (nav && !nav_top) {
            scene_was = *f;
        } else if (!nav && nav_top) {
            if (!cam_entry(name))
                *f = scene_was;
            if (nav_pl)
                rt_call3(GT_FN_SWF_SET_VISIBLE, nav_pl, GA((void *)"_root.AnimatedBG"), 1);
        }
    }
    cam_fn(c);
    if (on <= 0)
        return;
    nav_top = nav;
    if (nav)
        rt_call4(SCENE_CAM_FN, self, GA((void *)cam_name), how, 1);
}

static const char *char_name(u32 ch)
{
    u8 tag = *(u8 *)GP(ch + 0x44);
    return tag == 0xff ? (const char *)GP(rd32(ch + 0x50)) : (const char *)GP(ch + 0x45);
}

int menu3d_hide_backdrop(u32 ch)
{
    if (!upgrade_top || on <= 0)
        return 0;
    u32 p = rd32(ch + 0x40);
    if (!p || strcmp(char_name(p), "newBG"))
        return 0;
    u32 g = gt_game();
    if (g)
        *(volatile u8 *)GP(g + G_MENU_SCENE) = 1;
    return 1;
}

#define CH_VISIBLE 0xab
static void hide_flat_backdrop(u32 pl)
{
    static const char *const path[2] = {"_root.newBG", "_root.AnimatedBG"}, *const name[2] = {"newBG", "AnimatedBG"};
    static u32 kept_pl, kept[2];
    for (int i = 0; i < 2; i++) {
        u32 ch = kept_pl == pl ? kept[i] : 0;
        if (ch && (rd32(ch) < GT_RESERVE_START || rd32(ch) >= GT_MACHO_END || strcmp(char_name(ch), name[i])))
            ch = 0;
        if (!ch)
            ch = rt_call2(GT_FN_SWF_FIND, pl, GA((void *)path[i]));
        kept[i] = ch;
        if (ch && *(volatile u8 *)GP(ch + CH_VISIBLE))
            *(volatile u8 *)GP(ch + CH_VISIBLE) = 0;
    }
    kept_pl = pl;
}

void menu3d_nav_update(u32 pl)
{
    if (on <= 0 || !pl || !nav_top)
        return;
    nav_pl = pl;
    static int first;
    if (!first++)
        rt_log("[menu3d] main menu shown over the 3D scene\n");
    if (first % 120 == 1)
        menu_car_remember();
    u32 g = gt_game();
    if (g)
        *(volatile u8 *)GP(g + G_MENU_SCENE) = 1;
    hide_flat_backdrop(pl);
    static int test_frame = -1;
    static char test_menu[40];
    if (test_frame < 0) {
        const char *e = dev_getenv("GT_TEST_MENU"), *colon = e ? strchr(e, ':') : NULL;
        test_frame = colon ? atoi(e) : 0;
        if (colon)
            snprintf(test_menu, sizeof test_menu, "%s", colon + 1);
    }
    if (test_frame > 0 && scene && (int)eagl_frames() >= test_frame) {
        test_frame = 0;
        dev_log("[menu3d] test: %s opened (GT_TEST_MENU)\n", test_menu);
        if (!strcmp(test_menu, "DisplayMenu")) {
            display_test_page();
            snprintf(test_menu, sizeof test_menu, "ControlsMenu");
        }
        rt_call4(rd32(rd32(scene) + 0x34), scene, GA(test_menu), 1, 0);
        return;
    }
    static ULONGLONG t0;
    static int swinging;
    if (!scene || swing_deg <= 0 || *(volatile u8 *)GP(scene + SC_MOVING) ||
        rd32(scene + SC_CAM_AT) != GA((void *)cam_name)) {
        swinging = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (!swinging) {
        swinging = 1;
        t0 = now;
    }
    double a = -swing_deg * 3.14159265358979 / 180.0 *
               (swing_sec > 0 ? (1 - cos(2 * 3.14159265358979 * (double)(now - t0) / 1000.0 / swing_sec)) / 2 : 1.0);
    float ca = (float)cos(a), sa = (float)sin(a), v[11];
    memcpy(v, cam, sizeof v);
    for (int k = 0; k < 2; k++) {
        float x = cam[3 * k] - pivot[0], z = cam[3 * k + 2] - pivot[1];
        v[3 * k] = pivot[0] + x * ca - z * sa;
        v[3 * k + 2] = pivot[1] + x * sa + z * ca;
    }
    memcpy(GP(scene + SC_CAM_NOW), v, sizeof v);
}

#define CAR_REC 0x128
#define CAR_F_ID 0
#define CAR_F_MENU 0x24
static u32 car_field(u32 base, int i, int f) { return rd32(base + (u32)i * CAR_REC + 4 + 4 * (u32)f); }

static int garage_rows(int *out, int max)
{
    u32 player = gt_profile(), garage = player ? rd32(player + 4) : 0;
    u32 gn = garage ? rd32(garage + 0x30) : 0, gbase = garage ? rd32(garage + 0x34) : 0;
    int k = 0;
    for (u32 j = 0; gbase && j < gn && k < max; j++)
        out[k++] = (int)rd32(gbase + 0x28 + j * 0x84);
    return k;
}

static const char *menu_cars_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/menu_cars.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static int saved_rows[1024], n_saved = -1;

static void load_saved(void)
{
    char p[1024];
    n_saved = 0;
    FILE *f = fopen(menu_cars_path(p, sizeof p), "r");
    if (!f)
        return;
    char line[64];
    while (n_saved < 1024 && fgets(line, sizeof line, f))
        if (line[0] >= '0' && line[0] <= '9')
            saved_rows[n_saved++] = atoi(line);
    fclose(f);
}

static void menu_car_remember(void)
{
    static int rows[1024];
    int k = garage_rows(rows, 1024);
    if (k <= 0)
        return;
    if (n_saved < 0)
        load_saved();
    if (k == n_saved && !memcmp(rows, saved_rows, sizeof rows[0] * (size_t)k))
        return;
    char p[1024];
    FILE *f = fopen(menu_cars_path(p, sizeof p), "w");
    if (!f)
        return;
    fprintf(f, "# MENU_CAR_RANDOM (menu3d.c): the garage's car rows, for the main menu's car at the next start\n");
    for (int i = 0; i < k; i++)
        fprintf(f, "%d\n", rows[i]);
    fclose(f);
    memcpy(saved_rows, rows, sizeof rows[0] * (size_t)k);
    n_saved = k;
    rt_log("[menucar] %d garage cars remembered for the next start\n", k);
}

void gt_menu_car(CPU *c, u32 holder)
{
    u32 pick = c->r[0];
    u32 list = holder ? rd32(holder) : 0, base = list ? rd32(list) : 0;
    int n = list ? (int)rd32(list + 8) : 0;
    static int on = -1, logged;
    if (on < 0) {
        const char *e = getenv("GT_MENU_CAR_RANDOM");
        on = !(e && *e == '0');
    }
    if (base && n > 0 && !logged++ && dev_getenv("GT_MENU_CAR_LOG")) {
        dev_log("[menucar] %d cars, the game picked row %u\n", n, pick);
        for (int i = 0; i < n; i++) {
            char line[512];
            int o = snprintf(line, sizeof line, "[menucar] %3d:", i);
            for (int f = 0; f < 0x30 && o < (int)sizeof line - 12; f++)
                o += snprintf(line + o, sizeof line - (size_t)o, " %x", car_field(base, i, f));
            dev_log("%s\n", line);
        }
    }
    if (!on || !base || n <= 0 || n > 1024)
        return;
    static int rows[1024], g[1024];
    static u8 mine[1024];
    int gk = garage_rows(g, 1024);
    const char *from = "now";
    if (gk <= 0) {
        if (n_saved < 0)
            load_saved();
        gk = n_saved;
        memcpy(g, saved_rows, sizeof g[0] * (size_t)gk);
        from = "remembered";
    }
    memset(mine, 0, sizeof mine);
    for (int j = 0; j < gk; j++)
        if (g[j] >= 0 && g[j] < n)
            mine[g[j]] = 1;
    int k = 0, marked = 0, owned = 0;
    for (int i = 0; i < n; i++) {
        int m = car_field(base, i, CAR_F_MENU) != 0;
        marked += m;
        owned += !m && mine[i];
        if (m || mine[i])
            rows[k++] = i;
    }
    if (k < 2)
        return;
    static u64 seed;
    static int last = -1;
    if (!seed) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        seed = (u64)t.QuadPart ^ ((u64)GetCurrentProcessId() << 32) ^ 0x9e3779b97f4a7c15ull;
    }
    int r;
    do {
        seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
        r = rows[(seed >> 11) % (u64)k];
    } while (r == last);
    last = r;
    c->r[0] = (u32)r;
    rt_log("[menucar] main menu car: row %d (car %u) of %d (%d marked by the game, %d more in the garage, %s); the "
           "game's pick was row %u\n", r, car_field(base, r, CAR_F_ID), k, marked, owned, from, pick);
}

void menu3d_init(void)
{
    const char *e = getenv("GT_MENU_3D");
    on = !(e && *e == '0');
    if (!on) {
        rt_log("[menu3d] off (GT_MENU_3D=0): the main menu's flat picture\n");
        return;
    }
    e = getenv("GT_UPGRADE_3D");
    upgrade_on = e && *e == '1';
    const char *t = dev_getenv("GT_MENU3D_CAM");
    if (t) {
        float v[11];
        if (sscanf(t, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                   &v[8], &v[9], &v[10]) == 11)
            memcpy(cam, v, sizeof cam);
    }
    if (dev_getenv("GT_MENU3D_SWING"))
        sscanf(dev_getenv("GT_MENU3D_SWING"), "%f,%f", &swing_deg, &swing_sec);
    if (dev_getenv("GT_MENU3D_PIVOT"))
        sscanf(dev_getenv("GT_MENU3D_PIVOT"), "%f,%f", &pivot[0], &pivot[1]);
    u32 entry = cam_entry("CareerMapMenu");
    cam_fn = rt_lookup(SCENE_CAM_FN);
    if (!entry || !cam_fn || rd32(SCENE_CAM_SLOT) != SCENE_CAM_FN) {
        rt_log("[menu3d] camera table or slot 0x%x not as expected: flat picture\n", SCENE_CAM_SLOT);
        on = 0;
        return;
    }
    gt_unprotect(entry, CAM_SIZE);
    wr32(entry, GA((void *)cam_name));
    memcpy(GP(entry + 4), cam, sizeof cam);
    gt_unprotect(SCENE_CAM_SLOT, 4);
    wr32(SCENE_CAM_SLOT, rt_host_addr(cam_hook, "menu3d cam_hook"));
    rt_log("[menu3d] 3D scene behind the main menu (camera %.1f %.1f %.1f -> %.1f %.1f %.1f, fov %.1f)\n", cam[0],
           cam[1], cam[2], cam[3], cam[4], cam[5], cam[10]);
}
