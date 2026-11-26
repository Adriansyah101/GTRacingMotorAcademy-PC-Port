#include <stdlib.h>

#include "rt.h"
#include "features.h"
#include "game.h"

#define SERVICE_PRICE 250

#define COL_SERVICE_PRICE 0xa
#define FN_CURRENT_CAR 0x2657e8u
#define FN_SERVICE_MGR 0x2d68b4u
#define FN_CASH_PRICE 0x2d6afcu

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

void __wrap_F_2d8c1c(CPU *c)
{
    int p = price();
    if (p) {
        set_table_price(p);
        c->r[3] = (u32)p;
    }
    __real_F_2d8c1c(c);
}

void __real_F_2d90c0(CPU *c);
void __wrap_F_2d90c0(CPU *c)
{
    if (price())
        c->r[3] = 0;
    __real_F_2d90c0(c);
}

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
    __real_F_2d9720(c);
    return cash;
}

void __wrap_F_2d9720(CPU *c)
{
    u32 popup = c->r[0], swf = c->r[1], menu = c->r[2], car = c->r[3];
    int p = price();
    u32 which = rd32(popup + 0xc);
    if (!p || (which != 1 && which != 2)) {
        __real_F_2d9720(c);
        return;
    }
    if (which == 2) {
        u32 cash = finish_free(c, popup, swf, menu, car);
        rt_log("[service] running service finished free (cash %u given and taken back)\n", cash);
        return;
    }
    set_table_price(p);
    __real_F_2d9720(c);
    if (rd32(popup + 0xc) != 2)
        return;
    u32 cash = finish_free(c, popup, swf, menu, car);
    rt_log("[service] serviced for %d coins (cash %u given and taken back)\n", p, cash);
}
