#include "rt.h"
#include "features.h"
#include "game.h"

void __real_F_2659b0(CPU *c);
void __real_F_26a634(CPU *c);
void __real_F_26a980(CPU *c);
void __real_F_265920(CPU *c);

static const u32 to_saved[6] = {0, 1, 3, 4, 5, 2};

static int premium_order(void)
{
    static const u32 ids[6] = {0, 1, 2, 5, 4, 3};
    u32 mgr = rd32(rd32(GT_LICENCES_SLOT));
    if (!mgr || !rd32(mgr + 0x240))
        return 0;
    u32 cls = rd32(mgr + 0x240);
    if (*(u8 *)GP(cls + 0x18) != 6 || !rd32(cls + 0x1c))
        return 0;
    for (int i = 0; i < 6; i++)
        if (rd32(rd32(cls + 0x1c) + 4 * i) != ids[i])
            return 0;
    return 1;
}

static void remap(CPU *c)
{
    if (c->r[1] == 0 && c->r[2] < 6 && premium_order()) {
        static int told;
        if (!told) {
            told = 1;
            rt_log("[licence] class C in the paid build's order: medals kept at the free build's places\n");
        }
        c->r[2] = to_saved[c->r[2]];
    }
}

static int past_list(CPU *c)
{
    u32 mgr = rd32(rd32(GT_LICENCES_SLOT)), cls = mgr ? rd32(mgr + 0x240) : 0;
    if (!cls || c->r[1] >= 4 || premium_order())
        return 0;
    return c->r[2] >= *(u8 *)GP(cls + c->r[1] * 0x20 + 0x18);
}

void __wrap_F_2659b0(CPU *c)
{
    static u32 shown = 0xffffffffu;
    u32 data = c->r[0] ? rd32(c->r[0] + 4) : 0, word = data ? rd32(data + 0x134) : 0;
    if (data && word != shown) {
        shown = word;
        rt_log("[licence] class C medals 0x%03x (places 0-2: %u %u %u, 3-5: %u %u %u)\n", word, word & 3,
               word >> 2 & 3, word >> 4 & 3, word >> 6 & 3, word >> 8 & 3, word >> 10 & 3);
    }
    if (past_list(c)) {
        c->r[0] = 0;
        return;
    }
    remap(c);
    __real_F_2659b0(c);
}
void __wrap_F_26a634(CPU *c) { remap(c); __real_F_26a634(c); }
void __wrap_F_26a980(CPU *c) { remap(c); __real_F_26a980(c); }
void __wrap_F_265920(CPU *c) { remap(c); __real_F_265920(c); }
