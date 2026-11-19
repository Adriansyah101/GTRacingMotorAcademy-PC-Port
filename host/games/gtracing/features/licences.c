/* PREMIUM_LICENCES (port addition): the medals of class C's licence tests when the add-on
   add-on\premium-licences (tools/premium_addon.py) gives class C the paid build's six tests.

   The add-on brings back the three tests the free build cut, in the paid build's order (Full Lap 1
   at the end): Braking 1, Cornering 1, Cornering 2, Overtaking 1, Braking 2, Full Lap 1.  The profile
   keeps a test's medal by its PLACE in the class list (License.bin), not by its id: two bits per place in one
   word per class (profile data +0x134 + class * 4) and one "skipped" bit per place (+0x144, bit class * 6 +
   place).  The free build's places are Braking 1, Cornering 1, Full Lap 1, so the paid order showed a save's
   Full Lap 1 medal on Cornering 2 (seen on a test save).  The medals stay on the tests they were won on;
   here, rather than editing the save (it carries a check the game verifies), the place the game
   asks for is turned into the place the save keeps: the free build's three keep theirs (0 1 2), the new three
   follow (3 4 5).  So a save stays the free build's layout with or without the add-on.

   The game's accessors, all called from other generated files (so --wrap reaches every call):
     0x2659b0 (profile, class, place) -> medal            0x26a634 (profile, class, place, medal)
     0x26a980 (profile, class, place, ...) skip one test  0x265920 (profile, class, place) -> skipped
   (0x26a788 skip all and 0x2659cc class passed walk every place: no order.)  Only when class C's list is
   the add-on's (licence manager [0x70c044] +0x240: class records of 0x20 bytes, +0x18 u8 count, +0x1c ids). */
#include "rt.h"
#include "features.h"
#include "game.h"


void __real_F_2659b0(CPU *c);
void __real_F_26a634(CPU *c);
void __real_F_26a980(CPU *c);
void __real_F_265920(CPU *c);

/* display place -> saved place, for class C's list 0 1 2 5 4 3 (test ids) */
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

/* A place past the class's list has no medal (an original-game slip, fixed in optimisation pass 4).  The
   medal counters 0x2a148c (all classes) and 0x2a13f8 (one class, one medal) loop place = count .. 0, one place
   too many (an original slip, unseen in the free build where that place never holds a medal).  With the add-on
   taken away after its tests were driven, class C's place 3 holds Cornering 2's medal (to_saved), so the
   counts showed one medal more than class C's three tests have.  The medal stays in the save for the add-on. */
static int past_list(CPU *c)
{
    u32 mgr = rd32(rd32(GT_LICENCES_SLOT)), cls = mgr ? rd32(mgr + 0x240) : 0;
    if (!cls || c->r[1] >= 4 || premium_order())         /* classes 0-3 (C B A S), as 0x2a148c reads them */
        return 0;
    return c->r[2] >= *(u8 *)GP(cls + c->r[1] * 0x20 + 0x18);
}

void __wrap_F_2659b0(CPU *c)
{
    /* class C's medal word in the log when it changes: two bits a place, the add-on's three at places 3-5 */
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
