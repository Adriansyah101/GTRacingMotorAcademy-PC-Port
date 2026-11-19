/* service.c -- CAR_SERVICE: the car service is paid in coins only and done at once.

   An oil change for 250 coins, no cash and no hour of
   waiting, the way a garage service works in other racing games.  An enhancement of the economy, not a
   port fault: the original takes 50 coins, then makes the car wait 60 minutes or takes 1 cash (the paid
   currency, sold through the IAP that cannot work offline) to finish at once.

   The original flow (read from the code):
   - btnNoCSP on the pre-race screen (0x1c4c24) opens the coin popup: F_2d8c1c(popup, swf, menu, price),
     price from 0x2d6960 = the car table's column 0xa (50 for every car); the popup sets popup+0xc = 1.
   - btnYes / btnOk there (0x1c4a6c) calls F_2d9720(popup, swf, menu, -1).  Its case popup+0xc == 1 checks
     the coins against 0x2d6960, spends them (0x2a1948), starts the 60-minute service (0x2d686c ->
     manager vtable 0x78aa34 = 0x2d6ef8), hides CSPPayCoinPopup and opens the cash popup (F_2d90c0,
     popup+0xc = 2) with the price 0x2d6afc (1 cash while the service runs).
   - Its case popup+0xc == 2 checks and spends that cash, finishes the service (0x2d6890 -> 0x2d7848:
     points to the maximum, servicing flag 0, saved), redraws the oil can (0x2d94cc), hides the cash
     popup and refreshes the menu (0x2d8ee0).

   Here (both functions are called from code_014 / code_015 only, so the linker's --wrap reaches every
   call; host/build.py adds the options):
   - the car table's column 0xa is set to SERVICE_PRICE for every car, so the popup shows it and case 1
     checks and spends it;
   - after case 1 has run (popup+0xc is then 2), the cash it is about to ask for is given first and case 2
     is run at once: the cash comes and goes in the same call (net 0), and the game's own code finishes
     the service, redraws the can, closes the popups and refreshes the menu.
   Not enough coins: case 1 does nothing (as the original), and nothing more is done.
   - a service already running (started under the original rules, e.g. in an older save):
     btnNoCSP opens the cash popup straight away; it shows 0 (F_2d90c0, also wrapped) and its button
     runs case 2 the same way, so it finishes for nothing.

   GT_SERVICE_PRICE=n changes the price; GT_SERVICE_PRICE=0 keeps the original service (50 coins,
   1 hour or 1 cash). */
#include <stdlib.h>

#include "rt.h"
#include "features.h"
#include "game.h"

#define SERVICE_PRICE 250

#define COL_SERVICE_PRICE 0xa      /* column k at record + 4 + 4k */
#define FN_CURRENT_CAR 0x2657e8u   /* (profile manager) -> chosen car id */
#define FN_SERVICE_MGR 0x2d68b4u   /* () -> service point manager */
#define FN_CASH_PRICE 0x2d6afcu    /* (manager, car) -> cash to finish now */

void __real_F_2d8c1c(CPU *c);
void __real_F_2d9720(CPU *c);

static int price(void)
{
    static int p = -1;
    if (p < 0) {
        p = rt_setting_int("GT_SERVICE_PRICE", SERVICE_PRICE, 0, 1000000);
        rt_log(p ? "[service] car service: %d coins, done at once\n" : "[service] car service: the original\n", p);
    }
    return p;
}

static void set_table_price(int p)
{
    u32 tbl = gt_car_table();
    if (!tbl)
        return;
    u32 rec = rd32(tbl), n = rd32(tbl + 8);
    for (u32 i = 0; rec && i < n && i < 4096; i++)
        wr32(rec + i * 0x128 + 4 + COL_SERVICE_PRICE * 4, (u32)p);
}

/* the coin popup: shows the price */
void __wrap_F_2d8c1c(CPU *c)
{
    int p = price();
    if (p) {
        set_table_price(p);
        c->r[3] = (u32)p;
    }
    __real_F_2d8c1c(c);
}

/* the cash popup (a service started before CAR_SERVICE, e.g. in an older save): finishing it costs 0 */
void __real_F_2d90c0(CPU *c);
void __wrap_F_2d90c0(CPU *c)
{
    if (price())
        c->r[3] = 0;
    __real_F_2d90c0(c);
}

/* case 2 of the yes / ok button: give the cash it is about to take (net 0) and run it */
static u32 finish_free(CPU *c, u32 popup, u32 swf, u32 menu, u32 car)
{
    u32 profile = gt_profile();
    u32 args[2] = {profile, 0};
    if (car == 0xffffffffu)
        car = rt_guest_call(c, FN_CURRENT_CAR, 1, args);
    args[0] = rt_guest_call(c, FN_SERVICE_MGR, 0, args);
    args[1] = car;
    u32 cash = rt_guest_call(c, FN_CASH_PRICE, 2, args);
    if (cash) {
        args[0] = profile, args[1] = cash;
        rt_guest_call(c, GT_FN_ADD_CASH, 2, args);
    }
    c->r[0] = popup, c->r[1] = swf, c->r[2] = menu, c->r[3] = car;
    __real_F_2d9720(c);                       /* finishes the service, closes the popups */
    return cash;
}

/* the popups' yes / ok button */
void __wrap_F_2d9720(CPU *c)
{
    u32 popup = c->r[0], swf = c->r[1], menu = c->r[2], car = c->r[3];
    int p = price();
    u32 which = rd32(popup + 0xc);
    if (!p || (which != 1 && which != 2)) {
        __real_F_2d9720(c);
        return;
    }
    if (which == 2) {                         /* a service already running: finish it free */
        u32 cash = finish_free(c, popup, swf, menu, car);
        rt_log("[service] running service finished free (cash %u given and taken back)\n", cash);
        return;
    }
    set_table_price(p);
    __real_F_2d9720(c);                       /* pays the coins, starts the service, opens the cash popup */
    if (rd32(popup + 0xc) != 2)
        return;                               /* not paid (too few coins) */
    u32 cash = finish_free(c, popup, swf, menu, car);
    rt_log("[service] serviced for %d coins (cash %u given and taken back)\n", p, cash);
}
