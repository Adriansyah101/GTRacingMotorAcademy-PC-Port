/* prize.c -- PRIZE_MULT: the coins and XP a career race or event pays, times 3.

   The coins and XP of each event and race, and the bonus coins, times 3.  An enhancement, not a port fault.

   The original (read from the code):
   - the race's finish screen (0x27f164, reached through the dispatch table only) works out the prize from
     the race's table (race +0x28.., one row per result; a result already beaten pays a tenth) and pays it:
     coins 0x268704 (return 0x27fd24), XP 0x268e70 (0x27fd30); a time trial pays XP (0x280164) and coins
     (0x280170); then it writes the numbers with 0x13c82c (coins, 0x280004 / 0x280194) and 0x13de08 (XP,
     0x280060 / 0x2801f0);
   - the event's prize screen (0x29f4b0, FirstEventsScreen) pays the event's XP (0x268e70, 0x29f7b8), its
     cash (0x268724, left alone: coins and XP only) and its coins (0x2716c4, 0x29f7d0), and a
     bonus of coins (0x2716c4, 0x29fef0); it writes XP 0x29f7f8, coins 0x29fd14, the bonus 0x2a03e0;
   - the race's end (0x138080) pays the coins of the event's prize row for the result (0x2716c4, 0x13923c).

   Here the adders and the two number writers are wrapped (GNU ld --wrap; every one of these call sites
   is in another generated file than its callee) and the call's return address (the guest lr, which the
   generated code sets before each call) picks the prize calls only: their amount is multiplied, and the
   finish screens show the multiplied numbers.  Nothing in the race tables changes (the save keeps those
   tables too, so changing them would multiply again at every start).  Other coins and XP (daily bonus,
   gifts, selling a car, the shop, level-up cash) are not touched.
   GT_PRIZE_MULT=n sets the factor (default 3; 1 = the original). */
#include <stdlib.h>

#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_268704(CPU *c);   /* (profile manager, n): add coins */
void __real_F_268e70(CPU *c);   /* (profile manager, n): add XP (then the level-up check) */
void __real_F_2716c4(CPU *c);   /* (profile manager, n): add coins, count them for the quests */
void __real_F_13c82c(CPU *c);   /* (text, n, buffer, ...): a coin amount as text */
void __real_F_13de08(CPU *c);   /* (text, n, buffer, ...): an XP amount as text */

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
        if (f < 1 || f > 100)                   /* out of range: the original prizes */
            f = 1;
        if (f == 1)
            rt_log("[prize] race and event prizes as in the original\n");
        else
            rt_log("[prize] race and event coins and XP x%d\n", f);
    }
    return f;
}

/* r1 times the factor when the call comes from one of the sites */
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

/* REPEAT_PRIZE: a repeated result pays the full prize instead of the original's tenth (or 0 for an event).
   An enhancement: the original cuts repeated prizes on purpose.  All the tables hang off the career
   object [[0x70c050]] (G): the events at [G+8] (0x208 bytes each), the current event [G+0x10], the
   current race [G+0x14]; an event's races at [event+0x178] (0xf8 bytes each).
   Results are kept as 13 - place (12 = 1st); only 1st..3rd (>= 10) pay.

   Race (record = races + 0xf8 * i; best result at +0xf4, XP of k = 13 - place at +4k, coins at +0xc+4k):
   - finish 0x27f164: a better result than the record pays the places newly reached (0x27fc34 loop); any
     other top-3 result pays its place and the places below it down to 3rd, each divided by 10 (0x27fc90:
     smmul 0x66666667, asr 2).  Coins (fp) are paid at 0x27fd20 (r1, return 0x27fd24), XP (sl) at
     0x27fd2c (return 0x27fd30), shown at 0x280000 / 0x28005c;
   - race card 0x229134 (the 1st/2nd/3rd rows under a race on the event screen): a row whose place is
     already reached shows the same tenth (gold 0x2292dc, silver 0x229360, bronze 0x229dd4); XP and
     coins written at returns 0x229b30/0x229bd4 (1st), 0x229cc8/0x229d6c (2nd), 0x229e4c/0x229ef0 (3rd);
     the record there is [sp+0x34] + [sp+0x3c] * 8.
   Event (new result at event+0x194; per-tier "increments" 1st, 2nd, 3rd: coins at +0x40.., XP at +0xe8..;
   base XP of the result at +0x148 - 4 * result, getter 0x284c3c):
   - event prize screen 0x29f4b0: pays only a better result than before (tiers newly reached + base XP),
     else 0 (0x29f5f0); XP paid at return 0x29f7b8, coins 0x29f7d0, shown 0x29f7f8 / 0x29fd14;
   - event card 0x22f404: shows the tiers not reached yet, 0 once the cup is won (0x230748); coins and
     XP written at returns 0x2307f8 / 0x2308fc; the event there is [G+8] + [sp+0x64] * 8, best [sp+0x58].
   Here a repeated result pays what it paid the first time: the race's sums without the division, the
   event's tiers from the result down to 3rd plus the base XP; the cards show those amounts.

   Everything is worked out from G and sp, never from c->r[4..11]: the generated code keeps r4..r11 in C locals
   and syncs only r0..r3, sp and lr at a call, so those words are stale here (reading the record from r8 gives an
   absurd coin amount, 2131852776, capped to 0x7fffffff by PRIZE_MULT; the coin adder 0x2a1770 clamps a negative
   total to 0).  An amount is replaced only when the original amount is the one the original rule gives (the
   tenth, or 0).
   GT_REPEAT_PRIZE=0: the original cuts. */
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

/* a race record's prizes for places k..10 (13 - place), in full and as the original's tenths */
typedef struct { s32 xp, coins, xp10, coins10; } Sums;

static Sums race_sums(u32 rec, s32 k)
{
    Sums s = {0, 0, 0, 0};
    for (; k >= 10; k--) {
        s32 x = (s32)rd32(rec + 4 * k), m = (s32)rd32(rec + 0xc + 4 * k);
        s.xp += x;
        s.coins += m;
        s.xp10 += x / 10;               /* the smmul / asr 2 / sign fix-up is a division towards 0 */
        s.coins10 += m / 10;
    }
    return s;
}

static int sane(s32 v) { return v >= 0 && v <= 10000000; }

/* the race finish: set at the coin payment, used by the XP payment and the two numbers on the screen */
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
        return;                         /* not a top-3 result, or a better one (paid in full already) */
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

/* the race card's rows: k = 12 / 11 / 10 for 1st / 2nd / 3rd */
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
    scale(c, 1, NULL);                  /* the card shows what the finish pays */
}

/* an event's tiers from result r down to 3rd: coins, and XP with the result's base XP */
static void event_sums(u32 ev, s32 r, s32 *xp, s32 *coins, int base)
{
    *xp = base ? (s32)rd32(ev + 0x148 - 4 * r) : 0;
    *coins = 0;
    for (s32 j = 12 - r; j <= 2; j++) {
        *xp += (s32)rd32(ev + 0xe8 + 4 * j);
        *coins += (s32)rd32(ev + 0x40 + 4 * j);
    }
}

/* the event prize screen: set at the XP payment, used by the coin payment and the two numbers */
static struct { int on; s32 xp, coins; } evp;

static void event_finish_pay(CPU *c)
{
    evp.on = 0;
    if (!repeat_prize() || c->r[1] != 0)
        return;                         /* paid already: a better result than before */
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

/* the event card: 0 once the cup is won; with REPEAT_PRIZE the cup's full amount */
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

/* RESTORE_COINS: a one-time repair of the port
   fault above (the first REPEAT_PRIZE build zeroed a save's coins, 10,265 -> 0).  restore_coins.txt next
   to the exe holds the balance to restore; at the next race prize paid through 0x268704 the payment gets
   (balance - current coins) on top, so the game's own adder and its own save after the race store it;
   the file is then renamed restore_coins.done so it never runs twice.  The current coins are read like
   the getter 0x2a0f84: [profile + 0xc8 + 0xc] ^ [0x7739f8], rotated right by [0x7739f4]. */
#ifdef _WIN32
#include <stdio.h>
#include <string.h>
/* not <windows.h>: it defines IN, used above */
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
#define MAX_PATH 260

static int restore_path(char *buf, size_t n, const char *ext)
{
    char d[MAX_PATH];
    unsigned long k = GetModuleFileNameA(NULL, d, sizeof d);
    char *s = k ? strrchr(d, '\\') : NULL;
    if (!s)
        return 0;
    *s = 0;
    snprintf(buf, n, "%s\\restore_coins.%s", d, ext);
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
#else
static void restore_coins(CPU *c) { (void)c; }
#endif

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
