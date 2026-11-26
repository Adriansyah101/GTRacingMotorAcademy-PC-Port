#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_2b5fe8(CPU *c);
void __real_F_2b689c(CPU *c);
void __real_F_2b7e78(CPU *c);
void __real_F_270ac0(CPU *c);
void __real_F_266820(CPU *c);

#define P_GARAGE 4
#define P_VEC 8
#define G_CHOSEN 0x2c
#define G_COUNT 0x30
#define G_ENTRIES 0x34
#define ENTRY 0x84
#define E_ROW 0x28
#define WRITE_SITE 0x266968u
#define MAX_KEPT 256

static const u32 load_sites[] = {0x26beb8u, 0x26d7a4u, 0x26f0f0u};

static int on = -1;
static u32 ld_garage, ld_n, ld_k;
static u32 kept[MAX_KEPT];
static int nkept, fresh;
static int chosen_was = -1, chosen_now = -1;
static int writing, wr_missing;

typedef struct { u32 place, id; u8 bytes[ENTRY]; } Hidden;
static Hidden hidden[MAX_KEPT];
static int nhidden;
static int hidden_chosen = -1;
static int chosen_vis = -1;

static int enabled(void)
{
    if (on < 0) {
        const char *e = getenv("GT_MISSING_CARS");
        on = !(e && *e == '0');
    }
    return on;
}

static u32 table_rows(void)
{
    u32 tbl = gt_car_table();
    return tbl ? rd32(tbl + 8) : 0;
}

static u32 player(void) { return gt_profile(); }

static int first_with_row(u32 base, u32 n, u32 rows)
{
    for (u32 j = 0; base && j < n && j < 4096; j++)
        if (rd32(base + j * ENTRY + E_ROW) < rows)
            return (int)j;
    return -1;
}

static void choose(u32 garage, u32 was, u32 now, const char *where)
{
    wr32(garage + G_CHOSEN, now);
    if (chosen_now < 0)
        chosen_was = (int)was;
    chosen_now = (int)now;
    rt_log("[cars] the chosen car (garage slot %u) is not in the car table: slot %u chosen instead (%s)\n", was, now,
           where);
}

void gt_missing_cars_order(const u32 *place, int nh, int nvis, int *out)
{
    int n = 0, v = 0;
    for (int k = 0; k < nh; k++) {
        while (v < nvis && (u32)n < place[k])
            out[n++] = v++;
        out[n++] = -1 - k;
    }
    while (v < nvis)
        out[n++] = v++;
}

void __wrap_F_2b5fe8(CPU *c)
{
    u32 id = c->r[1], garage = c->r[2], lr = c->r[14];
    __real_F_2b5fe8(c);
    if (!enabled() || (lr != load_sites[0] && lr != load_sites[1] && lr != load_sites[2]))
        return;
    u32 n = garage ? rd32(garage + G_COUNT) : 0;
    if (garage != ld_garage || ld_k >= ld_n) {
        ld_garage = garage, ld_n = n, ld_k = 0, nkept = 0;
        chosen_was = chosen_now = -1;
        nhidden = 0, hidden_chosen = chosen_vis = -1;
        fresh = 1;
    }
    ld_k++;
    if (c->r[0] == 0xffffffffu && nkept < MAX_KEPT) {
        kept[nkept++] = id;
        rt_log("[cars] the save's car %u is not in the car table (an add-on off or gone): kept in the save\n", id);
    }
}

void __wrap_F_2b7e78(CPU *c)
{
    u32 tbl = c->r[0], e = c->r[1], rows = tbl ? rd32(tbl + 8) : 0;
    if (!enabled() || !e || rd32(e + E_ROW) < rows) {
        __real_F_2b7e78(c);
        return;
    }
    u32 pl = player(), g = pl ? rd32(pl + P_GARAGE) : 0, list = 0, n = 0;
    u32 vb = pl ? rd32(pl + P_VEC) : 0, ve = pl ? rd32(pl + P_VEC + 4) : 0;
    u32 gb = g ? rd32(g + G_ENTRIES) : 0, gn = g ? rd32(g + G_COUNT) : 0;
    if (vb && e >= vb && e < ve && (e - vb) % ENTRY == 0)
        list = vb, n = (ve - vb) / ENTRY;
    else if (gb && e >= gb && e < gb + gn * ENTRY && (e - gb) % ENTRY == 0)
        list = gb, n = gn;
    int j = list ? first_with_row(list, n, rows) : -1;
    if (j >= 0) {
        u32 slot = (e - list) / ENTRY;
        char where[64];
        snprintf(where, sizeof where, "loading it, from 0x%06x", c->r[14]);
        if (g && rd32(g + G_CHOSEN) == slot)
            choose(g, slot, (u32)j, where);
        else
            rt_log("[cars] garage slot %u is not in the car table: slot %u loaded in its place (from 0x%06x)\n", slot,
                   j, c->r[14]);
        c->r[1] = list + (u32)j * ENTRY;
        __real_F_2b7e78(c);
        return;
    }
    static int told;
    if (!told++)
        rt_log("[cars] a car not in the car table was to be loaded (from 0x%06x): skipped as a missing car file\n",
               c->r[14]);
    c->r[0] = 0;
}

static u32 close_up(u32 base, u32 n, u32 rows)
{
    u32 w = 0;
    for (u32 j = 0; j < n; j++)
        if (rd32(base + j * ENTRY + E_ROW) < rows) {
            if (w != j)
                memmove(GP(base + w * ENTRY), GP(base + j * ENTRY), ENTRY);
            w++;
        }
    return w;
}

static void hide(u32 g, u32 rows)
{
    u32 n = rd32(g + G_COUNT), base = rd32(g + G_ENTRIES), chosen = rd32(g + G_CHOSEN);
    int k = 0;
    nhidden = 0;
    for (u32 j = 0; base && j < n && j < 4096; j++) {
        if (rd32(base + j * ENTRY + E_ROW) < rows)
            continue;
        if (nhidden == MAX_KEPT) {
            rt_log("[cars] more than %d cars the table lacks: the rest stay in the garage\n", MAX_KEPT);
            return;
        }
        Hidden *h = &hidden[nhidden++];
        h->place = j;
        h->id = k < nkept ? kept[k] : 0;
        k++;
        memcpy(h->bytes, GP(base + j * ENTRY), ENTRY);
    }
    if (!nhidden)
        return;
    u32 before = 0;
    for (int i = 0; i < nhidden; i++)
        before += hidden[i].place < chosen;
    u32 vis_chosen = chosen - before;
    hidden_chosen = -1;
    for (int i = 0; chosen_was >= 0 && i < nhidden; i++)
        if (hidden[i].place == (u32)chosen_was)
            hidden_chosen = i;
    u32 left = close_up(base, n, rows);
    wr32(g + G_COUNT, left);
    if (vis_chosen >= left)
        vis_chosen = 0;
    wr32(g + G_CHOSEN, vis_chosen);
    chosen_vis = hidden_chosen >= 0 ? (int)vis_chosen : -1;
    u32 pl = player();
    u32 vb = pl ? rd32(pl + P_VEC) : 0, ve = pl ? rd32(pl + P_VEC + 4) : 0;
    u32 vleft = 0;
    if (vb && ve > vb && rd32(pl + P_GARAGE) == g) {
        vleft = close_up(vb, (ve - vb) / ENTRY, rows);
        wr32(pl + P_VEC + 4, vb + vleft * ENTRY);
    }
    for (int i = 0; i < nhidden; i++)
        rt_log("[cars] garage slot %u (car %u) left out of the game while its add-on is off; it stays in the save\n",
               hidden[i].place, hidden[i].id);
    rt_log("[cars] garage: %u cars to play (list %u), chosen slot %u%s\n", left, vb ? (ve > vb ? vleft : 0) : 0,
           vis_chosen, hidden_chosen >= 0 ? " (the save's chosen car is one of those left out)" : "");
}

void __wrap_F_270ac0(CPU *c)
{
    __real_F_270ac0(c);
    if (!enabled() || !ld_garage || ld_k < ld_n || !fresh)
        return;
    fresh = 0;
    u32 g = ld_garage, rows = table_rows(), n = rd32(g + G_COUNT), chosen = rd32(g + G_CHOSEN);
    u32 base = rd32(g + G_ENTRIES);
    u32 crow = chosen < n ? rd32(base + chosen * ENTRY + E_ROW) : 0xffffffffu;
    u32 cid = crow < rows ? rt_call3(0x2b689cu, gt_car_table(), crow, 0) : 0;
    rt_log("[cars] profile loaded: %u cars in the garage, chosen slot %u (car %u)\n", n, chosen, cid);
    if (dev_getenv("GT_CARS_LOG")) {
        char line[1024];
        int o = 0;
        for (u32 j = 0; j < n && j < 64 && o < (int)sizeof line - 16; j++) {
            u32 r = rd32(base + j * ENTRY + E_ROW);
            u32 id = r < rows ? rt_call3(0x2b689cu, gt_car_table(), r, 0) : 0xffffffffu;
            o += snprintf(line + o, sizeof line - (size_t)o, " %u:%d", j, (int)id);
        }
        u32 pl = player(), vb = pl ? rd32(pl + P_VEC) : 0, ve = pl ? rd32(pl + P_VEC + 4) : 0;
        rt_log("[cars] slots:%s; the player's list %u\n", line, ve > vb ? (ve - vb) / ENTRY : 0);
    }
    if (!rows || !n)
        return;
    if (!(chosen < n && rd32(base + chosen * ENTRY + E_ROW) < rows)) {
        int j = first_with_row(base, n, rows);
        if (j < 0) {
            rt_log("[cars] no car of the garage is in the car table\n");
            return;
        }
        choose(g, chosen, (u32)j, "after the load");
    }
    if (nkept)
        hide(g, rows);
}

void __wrap_F_2b689c(CPU *c)
{
    u32 lr = c->r[14], row = c->r[1], field = c->r[2];
    __real_F_2b689c(c);
    if (!writing || lr != WRITE_SITE || field != 0 || row < table_rows())
        return;
    if (nhidden) {
        if (wr_missing < nhidden)
            c->r[0] = hidden[wr_missing].id;
    } else if (wr_missing < nkept)
        c->r[0] = kept[wr_missing];
    wr_missing++;
}

static int write_whole(CPU *c, u32 pl)
{
    u32 g = rd32(pl + P_GARAGE);
    if (!g)
        return 0;
    u32 nvis = rd32(g + G_COUNT), img = rd32(g + G_ENTRIES), vchosen = rd32(g + G_CHOSEN);
    u32 vb = rd32(pl + P_VEC), ve = rd32(pl + P_VEC + 4);
    int total = (int)nvis + nhidden, order[4096 + MAX_KEPT];
    if (nvis > 4096)
        return 0;
    u8 *whole = malloc((size_t)total * ENTRY);
    if (!whole)
        return 0;
    if (vb != ve && (ve - vb) / ENTRY >= nvis)
        memmove(GP(img), GP(vb), nvis * ENTRY);
    u32 place[MAX_KEPT];
    for (int k = 0; k < nhidden; k++)
        place[k] = hidden[k].place;
    gt_missing_cars_order(place, nhidden, (int)nvis, order);
    u32 wchosen = 0;
    for (int i = 0; i < total; i++) {
        if (order[i] >= 0) {
            memcpy(whole + i * ENTRY, GP(img + (u32)order[i] * ENTRY), ENTRY);
            if ((u32)order[i] == vchosen)
                wchosen = (u32)i;
        } else
            memcpy(whole + i * ENTRY, hidden[-1 - order[i]].bytes, ENTRY);
    }
    if (hidden_chosen >= 0 && (int)vchosen == chosen_vis)
        for (int i = 0; i < total; i++)
            if (order[i] == -1 - hidden_chosen)
                wchosen = (u32)i;
    wr32(g + G_ENTRIES, GA(whole));
    wr32(g + G_COUNT, (u32)total);
    wr32(g + G_CHOSEN, wchosen);
    wr32(pl + P_VEC + 4, vb);
    writing = 1, wr_missing = 0;
    __real_F_266820(c);
    writing = 0;
    wr32(pl + P_VEC + 4, ve);
    wr32(g + G_ENTRIES, img);
    wr32(g + G_COUNT, nvis);
    wr32(g + G_CHOSEN, vchosen);
    free(whole);
    static int told;
    if (!told++)
        rt_log("[cars] profile saved with the %d car%s left out back in the garage%s (thread %lu)\n", nhidden,
               nhidden == 1 ? "" : "s", hidden_chosen >= 0 && (int)vchosen == chosen_vis ? ", the save's chosen car too"
                                                                                          : "",
               GetCurrentThreadId());
    return 1;
}

void __wrap_F_266820(CPU *c)
{
    if (!enabled() || (!nkept && chosen_now < 0)) {
        __real_F_266820(c);
        return;
    }
    if (nhidden && c->r[0] == player() && write_whole(c, c->r[0]))
        return;
    u32 g = ld_garage, rows = table_rows();
    u32 gb = g ? rd32(g + G_ENTRIES) : 0, gn = g ? rd32(g + G_COUNT) : 0;
    int was_lost = chosen_was >= 0 && ((u32)chosen_was >= gn || rd32(gb + (u32)chosen_was * ENTRY + E_ROW) >= rows);
    int restore = g && chosen_now >= 0 && rd32(g + G_CHOSEN) == (u32)chosen_now && was_lost;
    if (restore)
        wr32(g + G_CHOSEN, (u32)chosen_was);
    writing = 1, wr_missing = 0;
    __real_F_266820(c);
    writing = 0;
    if (restore)
        wr32(g + G_CHOSEN, (u32)chosen_now);
    static int told;
    if (!told++ && nkept)
        rt_log("[cars] profile saved with the %d car%s the table lacks%s\n", nkept, nkept == 1 ? "" : "s",
               restore ? ", the save's chosen car too" : "");
}
