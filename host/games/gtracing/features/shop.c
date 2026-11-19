/* shop.c -- COIN_SHOP: the game's own shop screen (the + buttons) offline, selling cash for coins.

   The original "add coins / add cash" screen used for an exchange: 1 cash = COIN_RATE coins, packs of 5
   and more 10 % off, 10 and more 20 % off, 30 and more 30 % off.  An enhancement, not a
   port fault: the original fills the shop
   from Gameloft's IAP server (iap.gameloft.com/partners/igcontents/, action=consumablelist: the packs,
   their amounts and dollar prices) and then the App Store; both are gone, so on any device today the +
   buttons only log "No WIFI" and the shop slides back out.

   The original (read from the code):
   - the + buttons set the shop kind [0x7a2d30] (1 coins, 2 cash, 3 exp) and open ShopSubPackMenu; its
     enter (0x1d69d0) asks the store for products when it is not ready (app delegate _requestProduct ->
     CStoreFacade::requestProduct 0x2ef358, vtable slot +0x30);
   - requestProduct checks the network (0x2ec394), asks the server, whose text reply
     "N,id,name,amount,price,..." is parsed by 0x2ee7fc: currency "US$" clears the items and adds one
     st_ProductInfo (0x118 bytes, vector at facade+0x44) per pack (0x2ee688), then StoreKit gives the
     localized price text; state facade+8 = 3 and facade+0x50 = 1 make the shop ready;
   - the shop's update (0x1feaec) draws the packs when the state is 3 (0x1d64b4, one button per item whose
     id contains "Cash", "Credit" (coins) or "Exp" for the open kind; fields +0x10c / +0x100 amount,
     +0x110 price, +0x108 the button's price text (UTF-16, StoreKit's localized price), +0x104 / +0x114
     a promotion's price and normal price text);
   - a pack's button calls CStoreFacade::buyWithRequest(index) (0x2ef6f0, slot +0x38; buy(id) 0x2ef104,
     slot +0x1c, is the other way in): network check, App Store payment, then the delivery 0x12917c adds
     the amount (0x268724 cash, 0x268704 coins).

   Here (all three functions are reached through the dispatch table only, so the linker's --wrap
   catches them; host/build.py adds the options):
   - requestProduct gives the parser a fixed list of cash packs (no network, no App Store), writes each
     price text in coins and marks the store ready;
   - a pack's button asks first in the shop's own yes / no popup (see "The confirmation" below); YES takes
     the coins (0x2a1a0c, the game's own spend, which also counts them as spent) and adds the cash
     (0x268724); too few coins: the popup says so, with one OK (the X button, centred), and nothing is taken or given;
   - the coin + button opens the same cash list (selling coins for cash would make the 10% discount a
     money loop).
   GT_COIN_SHOP=0 keeps the original (the + buttons do nothing offline). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "ns.h"
#include "rt.h"
#include "features.h"
#include "game.h"

#define SHOP_KIND 0x7a2d30u        /* 1 coins, 2 cash, 3 exp (set by the + buttons) */
#define SHOP_TAB 0x7a2c58u         /* 0 exp, 1 cash, 2 coins (title and rows) */
#define STORE_SLOT 0x7a4b24u       /* the CStoreFacade (0 after applicationWillTerminate) */
#define PAY_INDEX_SLOT 0x70bd9cu   /* -> the bought item's index (PaymentSuccess's g_PaySuccess_Ret) */
#define FN_PAY_SUCCESS 0x1d3834    /* ShopSubPackMenu::PaymentSuccess(menu) */

static u32 shop_menu;              /* the ShopSubPackMenu (from its enter) */
#define FN_PARSE_LIST 0x2ee7fc     /* (facade, const char *reply) */
#define FN_COINS 0x268f30          /* (profile manager) -> coins */
#define FN_SPEND_COINS 0x2a1a0c    /* (profile data = [pm+4]+0xc8, n) */

#define ITEM_SIZE 0x118

/* The prices, rebalanced: the cars' cash prices reach 1,500 ..
   3,500 (class S; one class B car 5,000) and packs of at most 30 cash meant over a hundred buys for one car.
   Read from the cars' files (.ba01 record word 28 cash, word 6 coins): where a car has both prices the game
   itself puts 1 cash at ~3,900 coins, so the exchange stays well under the game's own scale.  Packs 1, 5,
   10, 30, 100, 500, 1,000 (the game's are 1 .. 30 in steps of 5), the same discounts.  Not 250 or 500 coins a cash:
   some cash-only cars sell (record word 7, 0x1264d4) for more coins than their cash cost: the Aston Martin
   DBS Volante / Virage and Lamborghini Gallardo LP570-4 are 100 cash and sell for 77,000, a buy-and-sell
   loop at those rates.  A dearer cash rather than a capped sale balances it:
   1,100 coins a cash, so 100 cash at 30 % off is 77,000, no gain; the 3,500-cash car is
   ~2,695,000 coins.  GT_COIN_RATE=n sets the coins a cash.

   DYNAMIC_PRICE: a cash rate that changes, now and then a bigger discount (cheaper rates rarer), with a
   NEW label on the title bar's cash icon when a new rate shows up.  The rate changes every half hour of the
   PC's clock (half hours since 1970, UTC), drawn from the slot number (the same half hour gives the same
   rate, so a restart does not draw again):
       250 1 %, 500 3 %, 750 7 %, 1,100 35 %, 1,250 20 %, 1,500 15 %, 1,750 11 %, 2,000 8 %.
   The rates under 1,100 bring back the buy-and-sell gain on the few cars above while they last, a
   rare reward rather than a capped sale.  A list once made keeps its rate until the
   shop is entered again (the popup and the purchase use the rate shown).  When the rate differs from the
   last one the player saw in the shop (<home>\Library\Preferences\cash_rate.txt; none: 1,100), the title
   bar's cashNew (the main menu's own "NEW!" label, added by tools/swfpatch_cashnew.py) is drawn
   (shop_cash_new(), asked by widescreen.c's leaf drawing); opening the cash list marks it seen.
   GT_DYNAMIC_PRICE=0: always 1,100.  GT_PRICE_SLOT=n: test, that half hour's draw. */
#define BASE_RATE 1100
#define SLOT_SECONDS 1800
static int fixed_rate;             /* GT_COIN_RATE, or BASE_RATE with GT_DYNAMIC_PRICE=0; 0 = dynamic */
static int list_rate;              /* the rate of the list made last (0: none yet) */
static const int rate_of[] = {250, 500, 750, 1100, 1250, 1500, 1750, 2000};
static const int rate_weight[] = {1, 3, 7, 35, 20, 15, 11, 8};   /* out of 100 */

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
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;          /* splitmix64 */
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    int r = (int)((z ^ (z >> 31)) % 100);
    for (int i = 0; i < (int)(sizeof rate_of / sizeof rate_of[0]); i++)
        if ((r -= rate_weight[i]) < 0)
            return rate_of[i];
    return BASE_RATE;
}
#define COIN_RATE (list_rate ? list_rate : rate_now())

static int seen_rate = -1;         /* the rate last shown in the shop (file), -1 not read yet */

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

/* the title bar's NEW on the cash: the rate differs from the one last seen in the shop */
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
        int r = rt_setting_int("GT_COIN_RATE", 0, 0, 100000);   /* coins a cash: 1,000 cash stays below 2^31 */
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

/* the rates: coin_rate coins a cash; 5 cash 10% off, 10 cash 20% off, 30 and more 30% off */
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

/* a new UTF-16 copy of s (freed by the game with delete[] when the items are cleared) */
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

/* the cash packs at this hour's rate, as the store's items */
static void build_list(CPU *c, u32 facade)
{
    list_rate = rate_now();
    seen_set(list_rate);
    char *reply = malloc(64 * NPACKS + 16);
    int k = sprintf(reply, "%d", NPACKS);
    for (int i = 0; i < NPACKS; i++)
        k += sprintf(reply + k, ",GTPC_Cash_%d,%d Cash,%d,%d", packs[i], packs[i], packs[i], pack_price(packs[i]));
    /* currency "US$": the parser clears and adds the items; the App Store request it then sends goes
       to a nil store */
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
        memcpy(GP(it + 0x104), &fp, 4);               /* no promotion (the "% MORE" badge is the game's) */
        thousands(txt, p);
        strcat(txt, " COINS");
        wr32(it + 0x108, wtext(txt));                 /* the button's price (moneytxt) */
        wr32(it + 0x114, wtext(txt));                 /* the promotion's normal price (unused here) */
    }
    wr32(facade + 8, 3);                              /* ready */
    *(u8 *)GP(facade + 0x50) = 1;
    rt_log("[shop] %u packs ready, 1 cash = %d coins\n", (rd32(facade + 0x48) - rd32(facade + 0x44)) / ITEM_SIZE,
           list_rate);
}

/* CStoreFacade::requestProduct */
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

/* The confirmation: a yes / no popup before a purchase, so nothing is bought by mistake.  The shop's own yes /
   no popup, ShopSubPackMenu.FlurryPopup (in
   the original it offers a Flurry video for free cash; Flurry is gone): its texts are set like
   PromotionMenu's (0x1d4114: 0x50c038(swf, path, utf8, 0)), it opens and closes with the frame labels
   "Show" / "Hided" (0x50bc94(swf, clip, label, play)), and its buttons reach the shop's event handler
   0x1ff744 as a release (type 6) named btnYes / btnNo. */
#define POPUP "ShopSubPackMenu.FlurryPopup"
#define MENU_WAITING 0x840         /* ShopSubPackMenu: 2 while a purchase runs (set by a pack's release) */

static int pending = -1;           /* the pack waiting for YES (its index), -1 none */
static int pending_buy;            /* 1: YES buys it; 0: the "not enough coins" note (both buttons close) */

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

/* The "not enough coins" note has one button, the X, in the
   middle: YES (button and label) hidden, NO (the X) moved to the middle of the
   two.  A gameswf character (0x50b9ec(swf, path) finds it) points at its local matrix from +0x5c (sx,
   shx, tx, shy, sy, ty; twips) and keeps its visible flag at +0xab (what 0x50bde0 writes).  The game's
   set_matrix 0x50ddbc(character, matrix) copies the matrix into the character's own block (+0x64, the
   matrix at +0x20) and points +0x5c there.  The popup's "Show" tween places the buttons again on every
   frame (+0x5c then points at the tween's matrix), so a set _x (0x50bce0 "_x") lasts one frame only:
   the shop menu's update (0x1feaec, vtable only) puts NO back in the middle whenever the timeline has
   placed it, and remembers where the timeline put it for the YES / NO popup. */
#define FN_SET_MATRIX 0x50ddbc     /* (character, const float m[6]) */
#define CH_MATRIX 0x5c             /* character: -> its local matrix */
#define CH_OWN 0x64                /* character: its own transform block (the matrix at +0x20), or 0 */

static const char *const yes_part[2] = {POPUP ".btnYes", POPUP ".Texts.txtYes"};
static const char *const no_part[2] = {POPUP ".btnNo", POPUP ".Texts.txtNo"};
static int one_button;             /* the note's layout is on (also while the note slides out) */
static int closing;                /* frames of the note's slide-out left, then YES / NO again */
static float home_x[2];            /* NO's x where the timeline put it (the label is not tweened) */
static int have_home[2];
#define SLIDE_OUT_FRAMES 120       /* 2 s: the slide-out is shorter; a new popup ends it at once */

static u32 popup_find(CPU *c, u32 swf, const char *path)
{
    u32 a[2] = {swf, GA((void *)path)};
    return rt_guest_call(c, GT_FN_SWF_FIND, 2, a);
}

/* character ch's matrix, with x */
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

/* NO in the middle of YES and NO's own place.  Its own place is read whenever the timeline has just
   put it (the button, every frame of the tween); the label is not tweened, so once we have moved it
   (to the middle, or back) the place it was first found at is the one kept. */
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

/* one: the single centred button; 0: YES and NO in their own places */
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

/* buys pack it now (coins checked again: they may have changed since the popup opened) */
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
        /* the game's own ShopSubPackMenu::PaymentSuccess: title bar redrawn, the bought pack's
           animation (it reads the pack's index from g_PaySuccess_Ret) */
        if (shop_menu) {
            wr32(rd32(PAY_INDEX_SLOT), (it - rd32(facade + 0x44)) / ITEM_SIZE);
            a[0] = shop_menu;
            rt_guest_call(c, FN_PAY_SUCCESS, 1, a);
        }
    }
    wr32(facade + 8, 3);
}

/* a pack's button: opens the confirmation for item it; 0 if it is not one of ours */
static int exchange(CPU *c, u32 facade, u32 it)
{
    const char *id = (const char *)GP(it);
    if (!enabled() || !it || strncmp(id, "GTPC_Cash_", 10))
        return 0;
    if (!shop_menu) {                                 /* no shop screen to ask on */
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
    } else {                                          /* one OK (the X button) in the middle */
        snprintf(msg, sizeof msg, "NOT ENOUGH COINS: %d CASH COSTS %s COINS, YOU HAVE %s.", cash, p, h);
        popup_text(c, swf, "field", msg);
        popup_text(c, swf, "txtNo.Field", "OK");
    }
    rt_log("[shop] %s: asking (%d coins, %d there)\n", id, price, coins);
    wr32(facade + 8, 3);
    return 1;
}

/* ShopSubPackMenu's event handler (vtable 0x787914 only): our popup's YES / NO.  With no popup of ours
   open, FlurryPopup's YES keeps the original (Flurry's video offer). */
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
            /* the note keeps its one button while it slides out; YES / NO come back after (the update) */
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
            /* the pack's release set menu+0x840 = 2 (waiting for the App Store; taps on the packs are
               ignored until PaymentSuccess or a failure clears it): the wait is over */
            wr32(menu + MENU_WAITING, 0);
            /* the rest of the handler (the sheet, the base menu) sees a button it does not know */
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

/* ShopSubPackMenu's update (vtable only): the note's single button stays in the middle, also while
   the note slides out; then the popup is YES / NO again, as the game left it */
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

/* CStoreFacade::buy(id) (slot +0x1c) */
void __wrap_F_2ef104(CPU *c)
{
    u32 facade = c->r[0];
    u32 it = c->r[1] ? find_item(facade, (const char *)GP(c->r[1])) : 0;
    if (!exchange(c, facade, it))
        __real_F_2ef104(c);
}

/* CStoreFacade::buyWithRequest(index) (slot +0x38): the shop's pack buttons */
void __real_F_2ef6f0(CPU *c);
void __wrap_F_2ef6f0(CPU *c)
{
    u32 facade = c->r[0], i = c->r[1];
    u32 b = rd32(facade + 0x44), n = (rd32(facade + 0x48) - b) / ITEM_SIZE;
    if (!(i < n && exchange(c, facade, b + i * ITEM_SIZE)))
        __real_F_2ef6f0(c);
}

/* ShopSubPackMenu's enter: the coin + button shows the cash packs too.  The title and the rows follow
   [0x7a2c58] (0 exp, 1 cash, 2 coins), which the + buttons leave as it was (exp "XP" on the first open). */
void __wrap_F_1d69d0(CPU *c)
{
    u32 menu = c->r[0];
    int restore = 0;
    if (enabled()) {
        shop_menu = menu;
        pending = -1;
        restore = one_button;                         /* left the shop before the note had gone */
        u32 k = rd32(SHOP_KIND);
        if (k == 1 || k == 2) {
            wr32(SHOP_KIND, 2);
            wr32(SHOP_TAB, 1);
            /* DYNAMIC_PRICE: a list made at another hour's rate is made again; the list shown is seen */
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

/* AppDelegate -updatePurchase: the shop queues it on the main thread (waitUntilDone NO) and the port runs
   the queue once more after applicationWillTerminate has deleted the store (0x2f5090): it read the
   deleted store's vtable through a NULL pointer at exit.  A port fault (iOS kills the app instead). */
void __real_F_2f6ef8(CPU *c);
void __wrap_F_2f6ef8(CPU *c)
{
    if (!rd32(STORE_SLOT)) {
        c->r[0] = 0;
        return;
    }
    __real_F_2f6ef8(c);
}
