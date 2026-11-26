#include "arm_rt.h"

void h_Unwind_SjLj_Register(CPU *c, u32 fc);
void h_Unwind_SjLj_Unregister(CPU *c, u32 fc);
void h_Unwind_SjLj_Resume(CPU *c, u32 ue);
void I_longjmp(CPU *c);

#define ST_RESUME 0x00e5e1f0u

void (*rt_selftest_thrower)(CPU *c);
int rt_selftest_cleanups;

static __attribute__((noinline)) void st_jumper(CPU *c)
{
    LOCALS_IN();
    SJLJ_FRAME();
    if (SJLJ_SETJMP()) {
        RELOAD_ALL();
        TRAP(0, TRAP_RESUME);
    }
    R4 = 0xbad00004u, R5 = 0xbad00005u, R6 = 0xbad00006u, R7 = 0xbad00007u;
    R8 = 0xbad00008u, R10 = 0xbad0000au, R11 = 0xbad0000bu;
    c->V.d[8] = 0xbadbadbadbadbadbull;
    SP -= 64;
    SYNC_ARGS_OUT();
    SYNC_HI_OUT();
    I_longjmp(c);
    SJLJ_POP();
}

void rt_selftest_setjmp(CPU *c)
{
    LOCALS_IN();
    u32 val = R1;
    R4 = 0x44444444u, R5 = 0x55555555u, R6 = 0x66666666u, R7 = 0x77777777u;
    R8 = 0x88888888u, R10 = 0xaaaaaaaau, R11 = 0xbbbbbbbbu;
    c->V.d[8] = 0x0808080808080808ull;
    SYNC_HI_OUT();
    c->r[13] = SP;
    GUEST_SETJMP(0);
    if (R0 == 0) {
        c->r[1] = val;
        st_jumper(c);
        TRAP(0, TRAP_NORETURN);
    }
    SYNC_ARGS_OUT();
    SYNC_HI_OUT();
}

void rt_selftest_sjlj(CPU *c)
{
    LOCALS_IN();
    SJLJ_FRAME();
    if (SJLJ_SETJMP()) {
        RELOAD_ALL();
        if (c->resume_pc != ST_RESUME)
            TRAP(0, TRAP_RESUME);
        u32 fc = R7;
        u32 lp = LD32(fc + 4), ue = LD32(fc + 8), sel = LD32(fc + 12);
        if (sel == 0) {
            rt_selftest_cleanups++;
            ST32(fc + 4, 0xffffffffu);
            h_Unwind_SjLj_Resume(c, ue);
        }
        h_Unwind_SjLj_Unregister(c, fc);
        c->r[0] = lp, c->r[1] = sel, c->r[2] = ue;
        SJLJ_POP();
        return;
    }
    u32 fc = R0;
    ST32(fc + 4, R1);
    ST32(fc + 0x20, fc);
    ST32(fc + 0x24, ST_RESUME);
    ST32(fc + 0x28, SP);
    h_Unwind_SjLj_Register(c, fc);
    rt_selftest_thrower(c);
    h_Unwind_SjLj_Unregister(c, fc);
    c->r[0] = 0, c->r[1] = 0xffffffffu, c->r[2] = 0;
    SJLJ_POP();
}
