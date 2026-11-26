#include <stdlib.h>

#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_268704(CPU *c);
void __real_F_268e70(CPU *c);
void __real_F_2716c4(CPU *c);
void __real_F_13c82c(CPU *c);
void __real_F_13de08(CPU *c);

static const u32 pay_coins[] = {0x27fd24, 0x280170, 0x13923c, 0x29f7d0, 0x29fef0};
static const u32 pay_xp[] = {0x27fd30, 0x280164, 0x29f7b8};
static const u32 show_coins[] = {0x280004, 0x280194, 0x29fd14, 0x2a03e0};
static const u32 show_xp[] = {0x280060, 0x2801f0, 0x29f7f8};

#define IN(list, a) in_list(list, sizeof list / sizeof list[0], a)

static int in_list(const u32 *l, size_t n, u32 a)
{
    for (size_t i = 0; i < n; i++)
        if (l[i] == a)
            return 1;
    return 0;
}

static int factor(void)
{
    static int f = 0;
    if (!f) {
        f = rt_setting_int("GT_PRIZE_MULT", 3, -1, 101);
        if (f < 1 || f > 100)
            f = 1;
        if (f == 1)
            rt_log("[prize] race and event prizes as in the original\n");
        else
            rt_log("[prize] race and event coins and XP x%d\n", f);
    }
    return f;
}

static void scale(CPU *c, int hit, const char *what)
{
    if (!hit || factor() == 1)
        return;
    s32 n = (s32)c->r[1];
    if (n <= 0)
        return;
    s64 m = (s64)n * factor();
    if (m > 0x7fffffff)
        m = 0x7fffffff;
    c->r[1] = (u32)(s32)m;
    if (what)
        rt_log("[prize] %s %d -> %d (from 0x%06x)\n", what, n, (int)m, c->r[14]);
}

#define CAREER_PTR 0x70c050u

static int repeat_prize(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_REPEAT_PRIZE");
        on = !(e && *e == '0');
        rt_log("[prize] repeated results pay %s\n", on ? "the full prize" : "the original cut");
    }
    return on;
}

static u32 career(void)
{
    u32 p = rd32(CAREER_PTR);
    return p ? rd32(p) : 0;
}

typedef struct { s32 xp, coins, xp10, coins10; } Sums;

static Sums race_sums(u32 rec, s32 k)
{
    Sums s = {0, 0, 0, 0};
    for (; k >= 10; k--) {
        s32 x = (s32)rd32(rec + 4 * k), m = (s32)rd32(rec + 0xc + 4 * k);
        s.xp += x;
        s.coins += m;
        s.xp10 += x / 10;
        s.coins10 += m / 10;
    }
    return s;
}

static int sane(s32 v) { return v >= 0 && v <= 10000000; }

static struct { int on; Sums s; } fin;

static void race_finish_pay(CPU *c)
{
    fin.on = 0;
    if (!repeat_prize())
        return;
    u32 g = career(), car = rd32(c->r[13] + 0x54);
    if (!g || !car)
        return;
    u32 ev = rd32(g + 8) + rd32(g + 0x10) * 0x208, races = ev ? rd32(ev + 0x178) : 0;
    if (!races)
        return;
    u32 rec = races + rd32(g + 0x14) * 0xf8;
    s32 got = 13 - (s32)rd32(car + 0x2fc), best = (s32)rd32(rec + 0xf4);
    if (got < 10 || got > 12 || got > best)
        return;
    Sums s = race_sums(rec, got);
    if ((s32)c->r[1] != s.coins10 || !sane(s.coins) || !sane(s.xp)) {
        rt_log("[prize] repeated race result: coins %d, not the expected %d; left alone\n", (s32)c->r[1],
               s.coins10);
        return;
    }
    rt_log("[prize] repeated race result (place %d, best %d): coins %d -> %d, XP %d -> %d\n", 13 - got,
           13 - best, s.coins10, s.coins, s.xp10, s.xp);
    fin.on = 1;
    fin.s = s;
    c->r[1] = (u32)s.coins;
}

static void fin_swap(CPU *c, int coins, int last)
{
    if (!fin.on)
        return;
    if ((s32)c->r[1] == (coins ? fin.s.coins10 : fin.s.xp10))
        c->r[1] = (u32)(coins ? fin.s.coins : fin.s.xp);
    if (last)
        fin.on = 0;
}

static void race_card(CPU *c, s32 k, int coins)
{
    if (repeat_prize()) {
        u32 sp = c->r[13], rec = rd32(sp + 0x34) + rd32(sp + 0x3c) * 8;
        if (rec && (s32)rd32(rec + 0xf4) >= k) {
            Sums s = race_sums(rec, k);
            if ((s32)c->r[1] == (coins ? s.coins10 : s.xp10) && sane(s.coins) && sane(s.xp))
                c->r[1] = (u32)(coins ? s.coins : s.xp);
        }
    }
    scale(c, 1, NULL);
}

static void event_sums(u32 ev, s32 r, s32 *xp, s32 *coins, int base)
{
    *xp = base ? (s32)rd32(ev + 0x148 - 4 * r) : 0;
    *coins = 0;
    for (s32 j = 12 - r; j <= 2; j++) {
        *xp += (s32)rd32(ev + 0xe8 + 4 * j);
        *coins += (s32)rd32(ev + 0x40 + 4 * j);
    }
}

static struct { int on; s32 xp, coins; } evp;

static void event_finish_pay(CPU *c)
{
    evp.on = 0;
    if (!repeat_prize() || c->r[1] != 0)
        return;
    u32 g = career();
    u32 ev = g ? rd32(g + 8) + rd32(g + 0x10) * 0x208 : 0;
    if (!g || !ev)
        return;
    s32 r = (s32)rd32(ev + 0x194), xp, coins;
    if (r < 10 || r > 12)
        return;
    event_sums(ev, r, &xp, &coins, 1);
    if (!sane(xp) || !sane(coins))
        return;
    rt_log("[prize] repeated event result (place %d): coins 0 -> %d, XP 0 -> %d\n", 13 - r, coins, xp);
    evp.on = 1;
    evp.xp = xp;
    evp.coins = coins;
    c->r[1] = (u32)xp;
}

static void evp_swap(CPU *c, int coins, int last)
{
    if (!evp.on)
        return;
    if (c->r[1] == 0)
        c->r[1] = (u32)(coins ? evp.coins : evp.xp);
    if (last)
        evp.on = 0;
}

static void event_card(CPU *c, int coins)
{
    if (repeat_prize() && c->r[1] == 0) {
        u32 g = career(), sp = c->r[13];
        u32 ev = g ? rd32(g + 8) + rd32(sp + 0x64) * 8 : 0;
        if (ev && (s32)rd32(sp + 0x58) > 11) {
            s32 xp, m;
            event_sums(ev, 12, &xp, &m, 0);
            if (sane(xp) && sane(m))
                c->r[1] = (u32)(coins ? m : xp);
        }
    }
    scale(c, 1, NULL);
}

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
#define MAX_PATH 260
#else
#include "compat.h"
#endif

static int restore_path(char *buf, size_t n, const char *ext)
{
    char d[MAX_PATH];
    unsigned long k = GetModuleFileNameA(NULL, d, sizeof d);
    char *s = k ? strrchr(d, GT_SEPC) : NULL;
    if (!s)
        return 0;
    *s = 0;
    snprintf(buf, n, "%s" GT_SEP "restore_coins.%s", d, ext);
    return 1;
}

static void restore_coins(CPU *c)
{
    static int done = 0;
    char p[MAX_PATH + 32], q[MAX_PATH + 32];
    if (done || !restore_path(p, sizeof p, "txt"))
        return;
    FILE *f = fopen(p, "r");
    if (!f) {
        done = 1;
        return;
    }
    long want = 0;
    int ok = fscanf(f, "%ld", &want) == 1;
    fclose(f);
    u32 prof = rd32(c->r[0] + 4);
    if (!prof)
        return;
    done = 1;
    u32 k = rd32(GT_VALUE_KEY_ROT) & 31, v = rd32(prof + 0xc8 + 0xc) ^ rd32(GT_VALUE_KEY_XOR);
    s32 cur = (s32)(k ? (v >> k) | (v << (32 - k)) : v);
    if (!ok || want <= 0 || want > 10000000 || cur < 0 || cur > 10000000) {
        rt_log("[prize] restore_coins.txt: balance %ld, coins now %d; not used\n", want, cur);
        return;
    }
    if (want > cur) {
        rt_log("[prize] restore coins: %d -> %ld (+%ld, on top of this prize %d)\n", cur, want, want - cur,
               (s32)c->r[1]);
        c->r[1] += (u32)(want - cur);
    } else
        rt_log("[prize] restore coins: already %d (>= %ld), nothing added\n", cur, want);
    if (restore_path(q, sizeof q, "done")) {
        remove(q);
        rename(p, q);
    }
}

void __wrap_F_268704(CPU *c)
{
    if (c->r[14] == 0x27fd24)
        race_finish_pay(c);
    scale(c, IN(pay_coins, c->r[14]), "coins");
    if (IN(pay_coins, c->r[14]))
        restore_coins(c);
    __real_F_268704(c);
}

void __wrap_F_268e70(CPU *c)
{
    if (c->r[14] == 0x27fd30)
        fin_swap(c, 0, 0);
    else if (c->r[14] == 0x29f7b8)
        event_finish_pay(c);
    scale(c, IN(pay_xp, c->r[14]), "XP");
    __real_F_268e70(c);
}

void __wrap_F_2716c4(CPU *c)
{
    if (c->r[14] == 0x29f7d0)
        evp_swap(c, 1, 0);
    scale(c, IN(pay_coins, c->r[14]), "coins");
    __real_F_2716c4(c);
}

void __wrap_F_13c82c(CPU *c)
{
    switch (c->r[14]) {
    case 0x229bd4: race_card(c, 12, 1); break;
    case 0x229d6c: race_card(c, 11, 1); break;
    case 0x229ef0: race_card(c, 10, 1); break;
    case 0x2307f8: event_card(c, 1); break;
    default:
        if (c->r[14] == 0x280004)
            fin_swap(c, 1, 0);
        else if (c->r[14] == 0x29fd14)
            evp_swap(c, 1, 1);
        scale(c, IN(show_coins, c->r[14]), NULL);
    }
    __real_F_13c82c(c);
}

void __wrap_F_13de08(CPU *c)
{
    switch (c->r[14]) {
    case 0x229b30: race_card(c, 12, 0); break;
    case 0x229cc8: race_card(c, 11, 0); break;
    case 0x229e4c: race_card(c, 10, 0); break;
    case 0x2308fc: event_card(c, 0); break;
    default:
        if (c->r[14] == 0x280060)
            fin_swap(c, 0, 1);
        else if (c->r[14] == 0x29f7f8)
            evp_swap(c, 0, 0);
        scale(c, IN(show_xp, c->r[14]), NULL);
    }
    __real_F_13de08(c);
}
