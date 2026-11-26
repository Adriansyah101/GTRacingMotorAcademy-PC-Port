#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "features.h"
#include "game.h"

#define FN_XP_FOR 0x2745ccu
#define FN_TITLE_BAR 0x25b89cu
#define MAX_LEVEL 40
#define MAX_AMOUNT 999999999

#define CHEAT_WORD "IWANTMAXSTATS"

enum { CHEAT_PAD_UP = 1, CHEAT_PAD_DOWN = 2, CHEAT_PAD_X = 4, CHEAT_PAD_Y = 8, CHEAT_PAD_OTHER = 16 };

typedef struct {
    int key_pos;
    int pad_pos;
} Cheat;

static Cheat live;
static int pending;

static const int pad_seq[8] = {CHEAT_PAD_UP, CHEAT_PAD_UP, CHEAT_PAD_DOWN, CHEAT_PAD_DOWN,
                               CHEAT_PAD_X, CHEAT_PAD_Y, CHEAT_PAD_X, CHEAT_PAD_Y};

static int cheat_key_step(Cheat *s, int ctx, int letter)
{
    if (!ctx) {
        s->key_pos = 0;
        return 0;
    }
    if (letter >= 'a' && letter <= 'z')
        letter -= 'a' - 'A';
    if (letter && letter == CHEAT_WORD[s->key_pos]) {
        int started = s->key_pos > 0;
        if (++s->key_pos == (int)sizeof CHEAT_WORD - 1) {
            s->key_pos = 0;
            return 2;
        }
        return started;
    }
    s->key_pos = letter == CHEAT_WORD[0];
    return 0;
}

static int cheat_pad_step(Cheat *s, int ctx, int lb, int rb, int press)
{
    if (!ctx || !lb || !rb) {
        s->pad_pos = 0;
        return 0;
    }
    if (!press)
        return 1;
    if (press == pad_seq[s->pad_pos]) {
        if (++s->pad_pos == (int)(sizeof pad_seq / sizeof pad_seq[0])) {
            s->pad_pos = 0;
            return 2;
        }
        return 1;
    }
    s->pad_pos = press == pad_seq[0];
    return 1;
}

static int enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT_CHEAT_CODES");
        on = !(e && *e == '0');
        const char *old = getenv("GT_MAX_MONEY");
        if (old && *old)
            rt_log("[cheat] GT_MAX_MONEY=%s is no longer read: the max stats are a cheat code in the main menu\n",
                   old);
        if (!on)
            rt_log("[cheat] cheat codes off (GT_CHEAT_CODES=0)\n");
    }
    return on;
}

static int cheat_context(void)
{
    if (!enabled())
        return 0;
    const char *top = padmenu_top();
    if (!top || (strcmp(top, "MainMenu") && strcmp(top, "NavigationMenu")))
        return 0;
    u32 g = gt_game();
    if (!g || rd32(g + G_RACE))
        return 0;
    return !p2p_form_open();
}

int cheat_key(int letter)
{
    int r = cheat_key_step(&live, cheat_context(), letter);
    if (r == 2) {
        rt_log("[cheat] max stats code typed\n");
        pending = 1;
    }
    return r != 0;
}

int cheat_pad(int lb, int rb, int up, int down, int x, int y, int other)
{
    int press = (up ? CHEAT_PAD_UP : 0) | (down ? CHEAT_PAD_DOWN : 0) | (x ? CHEAT_PAD_X : 0) |
                (y ? CHEAT_PAD_Y : 0) | (other ? CHEAT_PAD_OTHER : 0);
    int r = cheat_pad_step(&live, lb && rb ? cheat_context() : 0, lb, rb, press);
    if (r == 2) {
        rt_log("[cheat] max stats code entered on the pad\n");
        pending = 1;
    }
    return r != 0;
}

static s32 unmask(u32 prof, u32 off)
{
    u32 k = rd32(GT_VALUE_KEY_ROT) & 31, v = rd32(prof + 0xc8 + off) ^ rd32(GT_VALUE_KEY_XOR);
    return (s32)(k ? (v >> k) | (v << (32 - k)) : v);
}

static int max_stats(void)
{
    u32 pm = gt_profile(), tab = rd32(rd32(GT_LICENCES_SLOT));
    if (!pm || !rd32(pm + 4) || !tab || !rd32(tab)) {
        rt_log("[cheat] max stats: no profile loaded yet, nothing changed\n");
        return 0;
    }
    u32 prof = rd32(pm + 4);
    s32 lv = unmask(prof, 0x60), xp = unmask(prof, 0x64), coins = unmask(prof, 0xc), cash = unmask(prof, 0x68);
    s32 need = (s32)rt_call2(FN_XP_FOR, tab, MAX_LEVEL - 1), need40 = (s32)rt_call2(FN_XP_FOR, tab, MAX_LEVEL);
    s32 need2 = (s32)rt_call2(FN_XP_FOR, tab, 2);
    if (lv < 1 || lv > MAX_LEVEL || xp < 0 || coins < 0 || cash < 0 || need2 <= 0 || need <= need2) {
        rt_log("[cheat] max stats: the profile looks wrong (level %d, XP %d, coins %d, cash %d, table %d / %d), "
               "nothing changed\n", lv, xp, coins, cash, need2, need);
        return 0;
    }
    s32 want_xp = need40 > need ? need40 : need;
    if (want_xp > xp && want_xp < 100000000)
        rt_call2(GT_FN_ADD_XP, pm, (u32)(want_xp - xp));
    for (int pass = 0; pass < 3; pass++) {
        s32 c2 = unmask(prof, 0xc), k2 = unmask(prof, 0x68);
        if (c2 == MAX_AMOUNT && k2 == MAX_AMOUNT)
            break;
        if (pass)
            rt_log("[cheat] max stats: pass %d, coins %d, cash %d\n", pass + 1, c2, k2);
        if (c2 != MAX_AMOUNT)
            rt_call2(GT_FN_ADD_COINS, pm, (u32)(MAX_AMOUNT - (c2 < 0 ? 0 : c2)));
        if (k2 != MAX_AMOUNT)
            rt_call2(GT_FN_ADD_CASH, pm, (u32)(MAX_AMOUNT - (k2 < 0 ? 0 : k2)));
    }
    rt_log("[cheat] max stats activated: level %d -> %d, XP %d -> %d, coins %d -> %d, cash %d -> %d\n", lv,
           unmask(prof, 0x60), xp, unmask(prof, 0x64), coins, unmask(prof, 0xc), cash, unmask(prof, 0x68));
    return 1;
}

static int pending_frames;
static int bar_wait;

void maxmoney_frame(int menus)
{
    if (!enabled())
        return;
    (void)menus;
    int ctx = cheat_context();
    if (!ctx)
        live.key_pos = live.pad_pos = 0;
    if (!pending) {
        pending_frames = 0;
        return;
    }
    const char *top = padmenu_top();
    if (!ctx) {
        pending = 0;
        rt_log("[cheat] max stats: left the main menu before it applied, nothing changed\n");
    } else if ((top && !strcmp(top, "MainMenu")) || ++pending_frames > 30) {
        pending = 0;
        max_stats();
    }
}

void maxmoney_nav_update(u32 self)
{
    if (!enabled())
        return;
    if (pending && cheat_context()) {
        pending = 0;
        if (max_stats())
            bar_wait = 3;
    } else if (bar_wait > 0 && --bar_wait == 0) {
        rt_call3(FN_TITLE_BAR, self, 1, 1);
        rt_log("[cheat] max stats: title bar redrawn\n");
    }
}

int cheat_selftest(char *why, size_t len)
{
    int fails = 0;
#define CT(cond, ...) do { if (!(cond) && !fails++) snprintf(why, len, __VA_ARGS__); } while (0)
    Cheat s = {0, 0};
    const char *w = CHEAT_WORD;
    int r = 0, taken = 0;
    for (int i = 0; w[i]; i++) {
        r = cheat_key_step(&s, 1, w[i] - 'A' + 'a');
        taken += r != 0;
        CT(i == 12 ? r == 2 : r == (i > 0), "keyboard: letter %d (%c) gave %d", i, w[i], r);
    }
    CT(taken == 12 && s.key_pos == 0, "keyboard: %d letters taken, progress %d after the word", taken, s.key_pos);
    s.key_pos = 0;
    cheat_key_step(&s, 1, 'I');
    CT(cheat_key_step(&s, 1, 'W') == 1, "keyboard: W after I not taken");
    CT(cheat_key_step(&s, 1, 'A') == 1, "keyboard: A after IW not taken");
    CT(cheat_key_step(&s, 1, 'D') == 0 && s.key_pos == 0, "keyboard: a wrong letter kept progress %d", s.key_pos);
    s.key_pos = 0;
    const char *t = "IWANIWANTMAXSTATS";
    for (int i = 0; t[i]; i++)
        r = cheat_key_step(&s, 1, t[i]);
    CT(r == 2, "keyboard: IWANIWANTMAXSTATS did not complete (%d)", r);
    s.key_pos = 0;
    cheat_key_step(&s, 1, 'I');
    cheat_key_step(&s, 1, 'W');
    CT(cheat_key_step(&s, 1, 0) == 0 && s.key_pos == 0, "keyboard: another key kept progress %d", s.key_pos);
    s.key_pos = 0;
    for (int i = 0; w[i]; i++)
        r = cheat_key_step(&s, i < 6, w[i]);
    CT(r == 0 && s.key_pos == 0, "keyboard: the word finished outside the main menu gave %d", r);
    s.key_pos = 0;
    taken = 0;
    for (int i = 0; w[i]; i++)
        taken += cheat_key_step(&s, 0, w[i]) != 0;
    CT(taken == 0 && s.key_pos == 0, "keyboard: %d letters taken outside the main menu", taken);

    Cheat p = {0, 0};
    for (int i = 0; i < 8; i++) {
        CT(cheat_pad_step(&p, 1, 1, 1, 0) == 1, "pad: a frame with the bumpers held did not hold the menus");
        r = cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
        CT(i == 7 ? r == 2 : r == 1, "pad: step %d gave %d", i, r);
    }
    CT(p.pad_pos == 0, "pad: progress %d after the sequence", p.pad_pos);
    CT(cheat_pad_step(&p, 1, 1, 0, CHEAT_PAD_UP) == 0 && p.pad_pos == 0, "pad: Up with LB only was counted");
    CT(cheat_pad_step(&p, 1, 0, 1, CHEAT_PAD_UP) == 0 && p.pad_pos == 0, "pad: Up with RB only was counted");
    for (int i = 0; i < 5; i++)
        cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    cheat_pad_step(&p, 1, 1, 0, 0);
    CT(p.pad_pos == 0, "pad: RB let go kept progress %d", p.pad_pos);
    for (int i = 5; i < 8; i++)
        r = cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    CT(r != 2, "pad: the sequence's end alone completed it");
    p.pad_pos = 0;
    cheat_pad_step(&p, 1, 1, 1, CHEAT_PAD_UP);
    cheat_pad_step(&p, 1, 1, 1, CHEAT_PAD_UP);
    cheat_pad_step(&p, 1, 1, 1, CHEAT_PAD_DOWN);
    CT(cheat_pad_step(&p, 1, 1, 1, CHEAT_PAD_OTHER) == 1 && p.pad_pos == 0, "pad: a wrong button kept progress %d",
       p.pad_pos);
    cheat_pad_step(&p, 1, 1, 1, CHEAT_PAD_X);
    CT(p.pad_pos == 0, "pad: X as the first step was counted");
    cheat_pad_step(&p, 1, 1, 1, CHEAT_PAD_UP | CHEAT_PAD_DOWN);
    CT(p.pad_pos == 0, "pad: two buttons at once were counted");
    p.pad_pos = 0;
    for (int i = 0; i < 8; i++)
        r = cheat_pad_step(&p, 0, 1, 1, pad_seq[i]);
    CT(r == 0 && p.pad_pos == 0, "pad: the sequence outside the main menu gave %d", r);
    for (int i = 0; i < 4; i++)
        cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    cheat_pad_step(&p, 0, 1, 1, 0);
    for (int i = 4; i < 8; i++)
        r = cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    CT(r != 2, "pad: completed across a step off the main menu");
#undef CT
    return fails;
}
