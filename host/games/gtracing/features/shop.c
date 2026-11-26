#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "ns.h"
#include "rt.h"
#include "features.h"
#include "game.h"

#define SHOP_KIND 0x7a2d30u
#define SHOP_TAB 0x7a2c58u
#define STORE_SLOT 0x7a4b24u
#define PAY_INDEX_SLOT 0x70bd9cu
#define FN_PAY_SUCCESS 0x1d3834

static u32 shop_menu;
#define FN_PARSE_LIST 0x2ee7fc
#define FN_COINS 0x268f30
#define FN_SPEND_COINS 0x2a1a0c

#define ITEM_SIZE 0x118

#define BASE_RATE 1100
#define SLOT_SECONDS 1800
static int fixed_rate;
static int list_rate;
static const int rate_of[] = {250, 500, 750, 1100, 1250, 1500, 1750, 2000};
static const int rate_weight[] = {1, 3, 7, 35, 20, 15, 11, 8};

static long long price_slot(void)
{
    const char *t = dev_getenv("GT_PRICE_SLOT");
    return t && *t ? atoll(t) : (long long)(time(NULL) / SLOT_SECONDS);
}

static int rate_now(void)
{
    if (fixed_rate)
        return fixed_rate;
    unsigned long long z = (unsigned long long)price_slot() * 0x9e3779b97f4a7c15ull + 0x47545043ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    int r = (int)((z ^ (z >> 31)) % 100);
    for (int i = 0; i < (int)(sizeof rate_of / sizeof rate_of[0]); i++)
        if ((r -= rate_weight[i]) < 0)
            return rate_of[i];
    return BASE_RATE;
}
#define COIN_RATE (list_rate ? list_rate : rate_now())

static int seen_rate = -1;

static const char *seen_path(char *buf, size_t n)
{
    char g[512];
    snprintf(g, sizeof g, "%s/Library/Preferences/cash_rate.txt", gt_guest_home);
    return gt_host_path(g, buf, n);
}

static void seen_set(int rate)
{
    if (rate == seen_rate)
        return;
    seen_rate = rate;
    char p[1024];
    FILE *f = fopen(seen_path(p, sizeof p), "w");
    if (f) {
        fprintf(f, "%d\n# DYNAMIC_PRICE (shop.c): the coins a cash last shown in the shop\n", rate);
        fclose(f);
    }
}

static int enabled(void);

int shop_cash_new(void)
{
    if (!enabled() || fixed_rate)
        return 0;
    if (seen_rate < 0) {
        char p[1024];
        FILE *f = fopen(seen_path(p, sizeof p), "r");
        seen_rate = BASE_RATE;
        if (f) {
            if (fscanf(f, "%d", &seen_rate) != 1)
                seen_rate = BASE_RATE;
            fclose(f);
        }
    }
    return rate_now() != seen_rate;
}

static const int packs[] = {1, 5, 10, 30, 100, 500, 1000};
#define NPACKS ((int)(sizeof packs / sizeof packs[0]))

void __real_F_2ef358(CPU *c);
void __real_F_2ef104(CPU *c);
void __real_F_1d69d0(CPU *c);

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_COIN_SHOP");
        on = !(e && *e == '0');
        const char *d = getenv("GT_DYNAMIC_PRICE");
        int r = rt_setting_int("GT_COIN_RATE", 0, 0, 100000);
        if (r > 0)
            fixed_rate = r;
        else if (d && *d == '0')
            fixed_rate = BASE_RATE;
        if (on)
            rt_log("[shop] cash for coins: 1 cash = %d coins (%s), 5: 10%% off, 10: 20%%, 30 and more: 30%%\n",
                   rate_now(), fixed_rate ? "fixed" : "this half hour's");
        else
            rt_log("[shop] original shop (offline: nothing)\n");
    }
    return on;
}

static int discount(int cash)
{
    if (cash % 5)
        return 0;
    return cash == 5 ? 10 : cash == 10 ? 20 : 30;
}

static int pack_price(int cash)
{
    int p = cash * COIN_RATE;
    return p - p * discount(cash) / 100;
}

static u32 profile(void) { return gt_profile(); }

static u32 wtext(const char *s)
{
    size_t n = strlen(s);
    u16 *w = malloc((n + 1) * 2);
    for (size_t i = 0; i <= n; i++)
        w[i] = (u8)s[i];
    return GA(w);
}

static void thousands(char *out, int v)
{
    char t[16];
    int n = snprintf(t, sizeof t, "%d", v), k = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0)
            out[k++] = ',';
        out[k++] = t[i];
    }
    out[k] = 0;
}

static u32 find_item(u32 facade, const char *id)
{
    u32 b = rd32(facade + 0x44), e = rd32(facade + 0x48);
    for (u32 it = b; it + ITEM_SIZE <= e; it += ITEM_SIZE)
        if (!strcmp((const char *)GP(it), id))
            return it;
    return 0;
}

static void build_list(CPU *c, u32 facade)
{
    list_rate = rate_now();
    seen_set(list_rate);
    char *reply = malloc(64 * NPACKS + 16);
    int k = sprintf(reply, "%d", NPACKS);
    for (int i = 0; i < NPACKS; i++)
        k += sprintf(reply + k, ",GTPC_Cash_%d,%d Cash,%d,%d", packs[i], packs[i], packs[i], pack_price(packs[i]));
    u32 store = rd32(facade + 4);
    wr32(facade + 4, 0);
    u32 args[2] = {facade, GA(reply)};
    rt_guest_call(c, FN_PARSE_LIST, 2, args);
    wr32(facade + 4, store);
    free(reply);
    for (int i = 0; i < NPACKS; i++) {
        char id[32], txt[32];
        snprintf(id, sizeof id, "GTPC_Cash_%d", packs[i]);
        u32 it = find_item(facade, id);
        if (!it)
            continue;
        int p = pack_price(packs[i]);
        float fp = (float)p;
        memcpy(GP(it + 0x104), &fp, 4);
        thousands(txt, p);
        strcat(txt, " COINS");
        wr32(it + 0x108, wtext(txt));
        wr32(it + 0x114, wtext(txt));
    }
    wr32(facade + 8, 3);
    *(u8 *)GP(facade + 0x50) = 1;
    rt_log("[shop] %u packs ready, 1 cash = %d coins\n", (rd32(facade + 0x48) - rd32(facade + 0x44)) / ITEM_SIZE,
           list_rate);
}

void __wrap_F_2ef358(CPU *c)
{
    u32 facade = c->r[0];
    if (!enabled()) {
        __real_F_2ef358(c);
        return;
    }
    if (rd32(SHOP_KIND) == 1)
        wr32(SHOP_KIND, 2);
    build_list(c, facade);
}

#define POPUP "ShopSubPackMenu.FlurryPopup"
#define MENU_WAITING 0x840

static int pending = -1;
static int pending_buy;

static void popup_text(CPU *c, u32 swf, const char *field, const char *s)
{
    char path[96];
    snprintf(path, sizeof path, POPUP ".Texts.%s", field);
    u32 a[4] = {swf, GA(path), GA((void *)s), 0};
    rt_guest_call(c, GT_FN_SWF_SET_TEXT, 4, a);
}

static void popup_goto(CPU *c, u32 swf, const char *label, int play)
{
    u32 a[4] = {swf, GA((void *)POPUP), GA((void *)label), (u32)play};
    rt_guest_call(c, GT_FN_SWF_GOTO_LABEL, 4, a);
}

#define FN_SET_MATRIX 0x50ddbc
#define CH_MATRIX 0x5c
#define CH_OWN 0x64

static const char *const yes_part[2] = {POPUP ".btnYes", POPUP ".Texts.txtYes"};
static const char *const no_part[2] = {POPUP ".btnNo", POPUP ".Texts.txtNo"};
static int one_button;
static int closing;
static float home_x[2];
static int have_home[2];
#define SLIDE_OUT_FRAMES 120

static u32 popup_find(CPU *c, u32 swf, const char *path)
{
    u32 a[2] = {swf, GA((void *)path)};
    return rt_guest_call(c, GT_FN_SWF_FIND, 2, a);
}

static void popup_place(CPU *c, u32 ch, float x)
{
    float m[6];
    memcpy(m, GP(rd32(ch + CH_MATRIX)), sizeof m);
    m[2] = x;
    u32 a[2] = {ch, GA(m)};
    rt_guest_call(c, FN_SET_MATRIX, 2, a);
}

static int placed_by_us(u32 ch)
{
    u32 own = rd32(ch + CH_OWN);
    return own && rd32(ch + CH_MATRIX) == own + 0x20;
}

static float mat_x(u32 ch)
{
    return *(float *)GP(rd32(ch + CH_MATRIX) + 8);
}

static void popup_center(CPU *c, u32 swf)
{
    for (int k = 0; k < 2; k++) {
        u32 y = popup_find(c, swf, yes_part[k]), n = popup_find(c, swf, no_part[k]);
        if (!y || !n)
            continue;
        if (!placed_by_us(n))
            home_x[k] = mat_x(n), have_home[k] = 1;
        if (!have_home[k])
            continue;
        float mid = (mat_x(y) + home_x[k]) / 2;
        if (!placed_by_us(n) || mat_x(n) != mid)
            popup_place(c, n, mid);
    }
}

static void popup_layout(CPU *c, u32 swf, int one)
{
    one_button = one;
    closing = 0;
    for (int k = 0; k < 2; k++) {
        u32 a[3] = {swf, GA((void *)yes_part[k]), (u32)!one};
        rt_guest_call(c, GT_FN_SWF_SET_VISIBLE, 3, a);
        u32 n = popup_find(c, swf, no_part[k]);
        if (!one && n && have_home[k] && placed_by_us(n) && mat_x(n) != home_x[k])
            popup_place(c, n, home_x[k]);
    }
    if (one)
        popup_center(c, swf);
}

static int coins_now(CPU *c)
{
    u32 a[1] = {profile()};
    return (int)rt_guest_call(c, FN_COINS, 1, a);
}

static void buy_now(CPU *c, u32 facade, u32 it)
{
    const char *id = (const char *)GP(it);
    int cash = (int)rd32(it + 0x10c);
    int price = pack_price(cash);
    u32 pm = profile();
    u32 a[2] = {pm, 0};
    int coins = coins_now(c);
    if (coins < price) {
        rt_log("[shop] %s: %d coins needed, %d there\n", id, price, coins);
    } else {
        a[0] = rd32(pm + 4) + 0xc8, a[1] = (u32)price;
        rt_guest_call(c, FN_SPEND_COINS, 2, a);
        a[0] = pm, a[1] = (u32)cash;
        rt_guest_call(c, GT_FN_ADD_CASH, 2, a);
        rt_log("[shop] %s: %d coins -> %d cash\n", id, price, cash);
        if (shop_menu) {
            wr32(rd32(PAY_INDEX_SLOT), (it - rd32(facade + 0x44)) / ITEM_SIZE);
            a[0] = shop_menu;
            rt_guest_call(c, FN_PAY_SUCCESS, 1, a);
        }
    }
    wr32(facade + 8, 3);
}

static int exchange(CPU *c, u32 facade, u32 it)
{
    const char *id = (const char *)GP(it);
    if (!enabled() || !it || strncmp(id, "GTPC_Cash_", 10))
        return 0;
    if (!shop_menu) {
        buy_now(c, facade, it);
        return 1;
    }
    int cash = (int)rd32(it + 0x10c);
    int price = pack_price(cash), coins = coins_now(c);
    char p[16], h[16], msg[96];
    thousands(p, price);
    thousands(h, coins);
    u32 swf = rd32(shop_menu + 4);
    pending = (int)((it - rd32(facade + 0x44)) / ITEM_SIZE);
    pending_buy = coins >= price;
    popup_goto(c, swf, "Show", 1);
    popup_layout(c, swf, !pending_buy);
    if (pending_buy) {
        snprintf(msg, sizeof msg, "BUY %d CASH FOR %s COINS?", cash, p);
        popup_text(c, swf, "field", msg);
        popup_text(c, swf, "txtYes.Field", "YES");
        popup_text(c, swf, "txtNo.Field", "NO");
    } else {
        snprintf(msg, sizeof msg, "NOT ENOUGH COINS: %d CASH COSTS %s COINS, YOU HAVE %s.", cash, p, h);
        popup_text(c, swf, "field", msg);
        popup_text(c, swf, "txtNo.Field", "OK");
    }
    rt_log("[shop] %s: asking (%d coins, %d there)\n", id, price, coins);
    wr32(facade + 8, 3);
    return 1;
}

void __real_F_1ff744(CPU *c);
void __wrap_F_1ff744(CPU *c)
{
    u32 menu = c->r[0], ev = c->r[1];
    if (pending >= 0 && enabled() && rd32(ev + 8) == 6) {
        const char *name = (const char *)GP(rd32(ev + 4));
        int yes = !strcmp(name, "btnYes"), no = !strcmp(name, "btnNo");
        u32 a[1] = {ev};
        if ((yes || no) && strstr((const char *)GP(rt_guest_call(c, GT_FN_EVENT_PATH, 1, a)), "FlurryPopup")) {
            int i = pending;
            pending = -1;
            popup_goto(c, rd32(menu + 4), "Hided", 0);
            if (one_button)
                closing = SLIDE_OUT_FRAMES;
            u32 facade = rd32(STORE_SLOT);
            if (yes && pending_buy && facade) {
                u32 b = rd32(facade + 0x44), n = (rd32(facade + 0x48) - b) / ITEM_SIZE;
                if ((u32)i < n)
                    buy_now(c, facade, b + (u32)i * ITEM_SIZE);
            } else {
                rt_log("[shop] not bought\n");
            }
            wr32(menu + MENU_WAITING, 0);
            static const char none[] = "";
            u32 old = rd32(ev + 4);
            wr32(ev + 4, GA((void *)none));
            c->r[0] = menu, c->r[1] = ev;
            __real_F_1ff744(c);
            wr32(ev + 4, old);
            return;
        }
    }
    __real_F_1ff744(c);
}

void __real_F_1feaec(CPU *c);
void __wrap_F_1feaec(CPU *c)
{
    u32 menu = c->r[0];
    __real_F_1feaec(c);
    if (!one_button || menu != shop_menu || !enabled())
        return;
    if (pending < 0 && --closing <= 0)
        popup_layout(c, rd32(menu + 4), 0);
    else
        popup_center(c, rd32(menu + 4));
}

void __wrap_F_2ef104(CPU *c)
{
    u32 facade = c->r[0];
    u32 it = c->r[1] ? find_item(facade, (const char *)GP(c->r[1])) : 0;
    if (!exchange(c, facade, it))
        __real_F_2ef104(c);
}

void __real_F_2ef6f0(CPU *c);
void __wrap_F_2ef6f0(CPU *c)
{
    u32 facade = c->r[0], i = c->r[1];
    u32 b = rd32(facade + 0x44), n = (rd32(facade + 0x48) - b) / ITEM_SIZE;
    if (!(i < n && exchange(c, facade, b + i * ITEM_SIZE)))
        __real_F_2ef6f0(c);
}

void __wrap_F_1d69d0(CPU *c)
{
    u32 menu = c->r[0];
    int restore = 0;
    if (enabled()) {
        shop_menu = menu;
        pending = -1;
        restore = one_button;
        u32 k = rd32(SHOP_KIND);
        if (k == 1 || k == 2) {
            wr32(SHOP_KIND, 2);
            wr32(SHOP_TAB, 1);
            u32 facade = rd32(STORE_SLOT);
            if (facade && list_rate && list_rate != rate_now())
                build_list(c, facade);
            else if (list_rate)
                seen_set(list_rate);
        }
    }
    __real_F_1d69d0(c);
    if (restore)
        popup_layout(c, rd32(menu + 4), 0);
}

void __real_F_2f6ef8(CPU *c);
void __wrap_F_2f6ef8(CPU *c)
{
    if (!rd32(STORE_SLOT)) {
        c->r[0] = 0;
        return;
    }
    __real_F_2f6ef8(c);
}
