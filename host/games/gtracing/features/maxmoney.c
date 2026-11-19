/* maxmoney.c -- MAX_STATS cheat code: coins, cash and the level at the game's limits.

   Coins, cash and the level set to the game's own maximum, by a cheat code only: the release ini carries no
   switch, and the effect comes only when the player enters the code in the main menu.  The old switch
   GT_MAX_MONEY is not read (a value left in an ini is only logged).  The code is typed in the main menu:
   - keyboard: the 13 letters I W A N T M A X S T A T S in a row (any case).  W, A, S and D also move the menu
     pointer (KEY_MENU, pcinput.c); once the code has started (an I), each letter that continues it is taken by the
     code and not passed on, so the pointer does not jump while the word is typed.  A key that does not continue
     the code starts it again (an I starts a new try at once); Shift, Ctrl, Alt, the Windows keys and Caps Lock are
     left out, so capitals can be typed;
   - gamepad (SDL's positional names, the same on Xbox and PlayStation pads): hold LB + RB (L1 + R1), then press
     Up, Up, Down, Down, X, Y, X, Y (Square, Triangle, Square, Triangle).  Letting go of either bumper, or any other
     button, starts it again.  While both bumpers are held in the main menu, the D-pad and the left stick do not
     move the menu pointer (padmenu.c gets no direction), so the code does not move the pointer either.
   Only in the main menu: the top menu is MainMenu (the title screen) or NavigationMenu (the main menu's cards),
   no race is loaded, and no text form of p2p.c is open (FIRST_NAME's name popup, the user name and room code
   forms).  Anywhere else (the Garage, the pause menu, a race, the shop...) the keys and buttons do what they always
   did and the code's progress is dropped; leaving the main menu drops it too.  GT_CHEAT_CODES=0 turns the code
   off (not in the release ini).

   What it does (through the game's own code, so the save keeps it with its check value: 0x26b154 after each add):
   XP up to what level 40 asks for (1,757,063 in the shipped table), through the game's add-XP 0x268e70 (its
   level-up check 0x268c00 runs as after a race), then coins (0x268704) and cash (0x268724) topped up to
   MAX_AMOUNT.  On NavigationMenu the title bar (coins, cash, level) is then redrawn by the game's own
   0x25b89c(menu, 1, 1), as ShopSubPackMenu::PaymentSuccess (0x1d3834) does after a purchase (when: see
   maxmoney_nav_update).  A save under level 40 also gets the game's own level-up screen.  The code works any
   number of times; spending in the game is kept until the next time it is entered.

   The game's limits (read from the shipped code):
   - level: the setter 0x2a0f14 clamps to 1..40 and the level-up loops (0x268c00, 0x268e9c) stop at 40;
     the XP a level needs is 0x2745cc(table [[0x70c044]], level) (8-byte rows, first word);
   - coins (profile +0xc8 +0xc) and cash (+0x68) are signed 32-bit, stored xor-and-rotated; the adders
     0x2a1770 / 0x2a17cc add without an upper cap and set a negative total to 0.  So 2,147,483,647 is the
     type's end, but one more prize on top of it wraps negative and the adder then zeroes the purse.
     MAX_AMOUNT keeps a margin: 999,999,999 (+1.1 billion headroom), nine digits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"
#include "features.h"
#include "game.h"

#define FN_XP_FOR 0x2745ccu        /* (table, level) -> XP that level asks for */
#define FN_TITLE_BAR 0x25b89cu     /* (menu, 1, 1): "<menu>.titleBar" coins, cash and level redrawn */
#define MAX_LEVEL 40
#define MAX_AMOUNT 999999999

#define CHEAT_WORD "IWANTMAXSTATS"

/* the pad's buttons as the code sees them (bits of a frame's presses) */
enum { CHEAT_PAD_UP = 1, CHEAT_PAD_DOWN = 2, CHEAT_PAD_X = 4, CHEAT_PAD_Y = 8, CHEAT_PAD_OTHER = 16 };

/* the code's progress; the live one below, --selftest drives its own */
typedef struct {
    int key_pos;        /* letters of CHEAT_WORD typed so far */
    int pad_pos;        /* steps of pad_seq pressed so far, with both bumpers held */
} Cheat;

static Cheat live;
static int pending;     /* a code was completed: the game frame applies it (maxmoney_frame) */

static const int pad_seq[8] = {CHEAT_PAD_UP, CHEAT_PAD_UP, CHEAT_PAD_DOWN, CHEAT_PAD_DOWN,
                               CHEAT_PAD_X, CHEAT_PAD_Y, CHEAT_PAD_X, CHEAT_PAD_Y};

/* the input, kept apart from the game */

/* a key went down (repeats left out).  ctx: the main menu (cheat_context).  letter: 'A'..'Z' for a letter key, 0
   for any other key (the modifiers are not passed here).  Returns 0: the key goes on to the menus; 1: the
   code took it (a letter that continues a code already started); 2: it completed the code (taken too) */
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
    s->key_pos = letter == CHEAT_WORD[0];   /* a wrong key: again from the start (an I is its first letter) */
    return 0;
}

/* one frame of the pad.  lb, rb: the bumpers held; press: the buttons that went down this frame (CHEAT_PAD_*
   bits; CHEAT_PAD_OTHER for any button that is not in the code).  Returns 0: nothing for the code (the bumpers are
   not both held, or not in the main menu); 1: both bumpers held in the main menu (the menus get no direction this
   frame); 2: the code was completed */
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
    s->pad_pos = press == pad_seq[0];       /* a wrong button (or two at once): again from the start */
    return 1;
}

/* the game */

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

/* the main menu, with no race loaded and no text form open */
static int cheat_context(void)
{
    if (!enabled())
        return 0;
    const char *top = padmenu_top();
    if (!top || (strcmp(top, "MainMenu") && strcmp(top, "NavigationMenu")))
        return 0;
    u32 g = gt_game();
    if (!g || rd32(g + G_RACE))             /* a race is loaded (the pause menu, the end menus) */
        return 0;
    return !p2p_form_open();
}

/* pcinput.c: a key went down (not a repeat) */
int cheat_key(int letter)
{
    int r = cheat_key_step(&live, cheat_context(), letter);
    if (r == 2) {
        rt_log("[cheat] max stats code typed\n");
        pending = 1;
    }
    return r != 0;
}

/* pcinput.c: the pad, each frame: the bumpers held, and which buttons went down this frame (up / down: the D-pad;
   x / y: West / North, Square / Triangle on a PlayStation pad; other: any other button).  Nonzero: both bumpers
   are held in the main menu (no direction to the menus this frame) */
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

static s32 unmask(u32 prof, u32 off)   /* the getters 0x2a0f84 / 0x2a0f5c / 0x2a0fac / 0x2a0fd4 */
{
    u32 k = rd32(GT_VALUE_KEY_ROT) & 31, v = rd32(prof + 0xc8 + off) ^ rd32(GT_VALUE_KEY_XOR);
    return (s32)(k ? (v >> k) | (v << (32 - k)) : v);
}

/* 1: applied */
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
    /* coins and cash to exactly MAX_AMOUNT, down too (the adders take a negative amount): a save from the first
       MAX_MONEY build holds 1,000,039,624 coins.  Read again after each pass: in a test (a save
       at level 14 going to 40) the first pass ended 1,000 coins over, paid by the game while it ran (not traced
       further; the second pass takes it back) */
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

static int pending_frames;  /* frames a completed code has waited for NavigationMenu's update */
static int bar_wait;        /* NavigationMenu updates left before its title bar is redrawn */

/* pcinput.c, each frame (on the game's thread): the code's progress dropped off the main menu.  A code completed on
   the title screen (MainMenu, which has no title bar) applies here, as MAX_MONEY always did; on NavigationMenu it
   waits for that menu's own update (maxmoney_nav_update), or applies here after 30 frames if none comes */
void maxmoney_frame(int menus)
{
    if (!enabled())
        return;
    (void)menus;                            /* the main menu's own test: the pad menu option may be off */
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

/* exitbtn.c: NavigationMenu::Update (0x1e95c8, slot +0x1c) has just run.  A code entered on NavigationMenu applies
   here, inside the game's own update, where the game also redraws its title bar; the redraw follows 3 updates later.
   Why not from the input code: the XP add starts the game's level-up screen ("You reached LV:40"), an overlay
   inside NavigationMenu, and a title bar redraw in that same frame from outside the update faulted in one run of
   two (0x25b89c -> 0x508b68 called an empty vtable slot of a text field, inferred to be one the overlay was
   replacing). */
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

/* --selftest: the keyboard word, the pad sequence, the resets and the main menu rule, without the game */
int cheat_selftest(char *why, size_t len)
{
    int fails = 0;
#define CT(cond, ...) do { if (!(cond) && !fails++) snprintf(why, len, __VA_ARGS__); } while (0)
    Cheat s = {0, 0};
    const char *w = CHEAT_WORD;
    int r = 0, taken = 0;
    for (int i = 0; w[i]; i++) {                       /* typed in lower case, as most do */
        r = cheat_key_step(&s, 1, w[i] - 'A' + 'a');
        taken += r != 0;
        CT(i == 12 ? r == 2 : r == (i > 0), "keyboard: letter %d (%c) gave %d", i, w[i], r);
    }
    CT(taken == 12 && s.key_pos == 0, "keyboard: %d letters taken, progress %d after the word", taken, s.key_pos);
    /* W, A and S of the word are taken (not passed to the menus) */
    s.key_pos = 0;
    cheat_key_step(&s, 1, 'I');
    CT(cheat_key_step(&s, 1, 'W') == 1, "keyboard: W after I not taken");
    CT(cheat_key_step(&s, 1, 'A') == 1, "keyboard: A after IW not taken");
    /* a wrong letter starts again, and is passed on */
    CT(cheat_key_step(&s, 1, 'D') == 0 && s.key_pos == 0, "keyboard: a wrong letter kept progress %d", s.key_pos);
    /* a wrong I starts a new try at once: IWANI WANTMAXSTATS completes */
    s.key_pos = 0;
    const char *t = "IWANIWANTMAXSTATS";
    for (int i = 0; t[i]; i++)
        r = cheat_key_step(&s, 1, t[i]);
    CT(r == 2, "keyboard: IWANIWANTMAXSTATS did not complete (%d)", r);
    /* a non-letter key (0) starts again */
    s.key_pos = 0;
    cheat_key_step(&s, 1, 'I');
    cheat_key_step(&s, 1, 'W');
    CT(cheat_key_step(&s, 1, 0) == 0 && s.key_pos == 0, "keyboard: another key kept progress %d", s.key_pos);
    /* outside the main menu: nothing taken, nothing completed, progress dropped */
    s.key_pos = 0;
    for (int i = 0; w[i]; i++)
        r = cheat_key_step(&s, i < 6, w[i]);
    CT(r == 0 && s.key_pos == 0, "keyboard: the word finished outside the main menu gave %d", r);
    s.key_pos = 0;
    taken = 0;
    for (int i = 0; w[i]; i++)
        taken += cheat_key_step(&s, 0, w[i]) != 0;
    CT(taken == 0 && s.key_pos == 0, "keyboard: %d letters taken outside the main menu", taken);

    /* the pad: LB + RB held, Up Up Down Down X Y X Y; frames with no press in between */
    Cheat p = {0, 0};
    for (int i = 0; i < 8; i++) {
        CT(cheat_pad_step(&p, 1, 1, 1, 0) == 1, "pad: a frame with the bumpers held did not hold the menus");
        r = cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
        CT(i == 7 ? r == 2 : r == 1, "pad: step %d gave %d", i, r);
    }
    CT(p.pad_pos == 0, "pad: progress %d after the sequence", p.pad_pos);
    /* without both bumpers: nothing, and the menus keep the D-pad */
    CT(cheat_pad_step(&p, 1, 1, 0, CHEAT_PAD_UP) == 0 && p.pad_pos == 0, "pad: Up with LB only was counted");
    CT(cheat_pad_step(&p, 1, 0, 1, CHEAT_PAD_UP) == 0 && p.pad_pos == 0, "pad: Up with RB only was counted");
    /* a bumper let go mid-way starts again */
    for (int i = 0; i < 5; i++)
        cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    cheat_pad_step(&p, 1, 1, 0, 0);
    CT(p.pad_pos == 0, "pad: RB let go kept progress %d", p.pad_pos);
    for (int i = 5; i < 8; i++)
        r = cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    CT(r != 2, "pad: the sequence's end alone completed it");
    /* a wrong button starts again (Up then counts as the first step) */
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
    /* outside the main menu: the menus keep the D-pad and the sequence does nothing */
    p.pad_pos = 0;
    for (int i = 0; i < 8; i++)
        r = cheat_pad_step(&p, 0, 1, 1, pad_seq[i]);
    CT(r == 0 && p.pad_pos == 0, "pad: the sequence outside the main menu gave %d", r);
    /* the main menu left mid-way: progress dropped, the rest alone does not complete */
    for (int i = 0; i < 4; i++)
        cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    cheat_pad_step(&p, 0, 1, 1, 0);
    for (int i = 4; i < 8; i++)
        r = cheat_pad_step(&p, 1, 1, 1, pad_seq[i]);
    CT(r != 2, "pad: completed across a step off the main menu");
#undef CT
    return fails;
}
