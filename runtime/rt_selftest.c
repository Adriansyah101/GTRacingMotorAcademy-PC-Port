/* rt_selftest.c -- guest-style functions for --selftest (host/core/selftest.c), written with the same macros the
   code generator emits (GUEST_SETJMP, SJLJ_FRAME / SJLJ_SETJMP, RELOAD_ALL) and built with the same compiler as
   build/gen (clang), so the __builtin_setjmp buffers and the runtime's __builtin_longjmp (rt_jump.c) pair as they
   do in the game.  The menus and a race reach neither path: the game's guest setjmp users (0x532bf4, 0x54baec,
   the resolver's sigsetjmp at 0x3b22e8) and its exception handlers (1,400 functions register an SjLj context)
   only act on errors.

   rt_selftest_setjmp: setjmp(buf) with known callee-saved registers, then a callee with an SjLj frame of its own
   changes them and calls longjmp(buf, val); setjmp's second return must give val (1 for 0), the registers, sp,
   d8 and the runtime's frame list as they were at setjmp.

   rt_selftest_sjlj: a function with handlers.  It registers the function context the caller made, sets its
   call site and calls rt_selftest_thrower.  The unwinder resumes it at the landing pad (c->resume_pc = the
   context's resume address, r7 = its saved fp); a cleanup-only pad (selector 0) counts itself and continues the
   unwinding (_Unwind_SjLj_Resume), as a function with destructors does.  A handler returns
   r0 = the landing pad index (the call site the unwinder set), r1 = the selector, r2 = the exception (fc + 8). */
#include "arm_rt.h"

void h_Unwind_SjLj_Register(CPU *c, u32 fc);
void h_Unwind_SjLj_Unregister(CPU *c, u32 fc);
void h_Unwind_SjLj_Resume(CPU *c, u32 ue);
void I_longjmp(CPU *c);

#define ST_RESUME 0x00e5e1f0u           /* the resume address the test's contexts hold */

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
    I_longjmp(c);                       /* r0 = buf, r1 = val: never returns */
    SJLJ_POP();
}

/* r0 = the guest jmp_buf, r1 = the value for longjmp.  Returns r0 = what setjmp's second return gave. */
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
        c->r[1] = val;                  /* c->r[0] is still the buffer */
        st_jumper(c);
        TRAP(0, TRAP_NORETURN);
    }
    SYNC_ARGS_OUT();
    SYNC_HI_OUT();
}

/* r0 = the function context (personality, LSDA and jbuf filled in by the caller), r1 = the call site */
void rt_selftest_sjlj(CPU *c)
{
    LOCALS_IN();
    SJLJ_FRAME();
    if (SJLJ_SETJMP()) {
        RELOAD_ALL();
        if (c->resume_pc != ST_RESUME)
            TRAP(0, TRAP_RESUME);
        u32 fc = R7;                    /* the context's saved fp is the context itself (see selftest.c) */
        u32 lp = LD32(fc + 4), ue = LD32(fc + 8), sel = LD32(fc + 12);
        if (sel == 0) {                 /* cleanup only: on with the unwinding */
            rt_selftest_cleanups++;
            ST32(fc + 4, 0xffffffffu);  /* no landing pad left here */
            h_Unwind_SjLj_Resume(c, ue);
        }
        h_Unwind_SjLj_Unregister(c, fc);
        c->r[0] = lp, c->r[1] = sel, c->r[2] = ue;
        SJLJ_POP();
        return;
    }
    u32 fc = R0;
    ST32(fc + 4, R1);
    ST32(fc + 0x20, fc);                /* jbuf: fp, resume address, sp */
    ST32(fc + 0x24, ST_RESUME);
    ST32(fc + 0x28, SP);
    h_Unwind_SjLj_Register(c, fc);
    rt_selftest_thrower(c);
    h_Unwind_SjLj_Unregister(c, fc);    /* nothing thrown */
    c->r[0] = 0, c->r[1] = 0xffffffffu, c->r[2] = 0;
    SJLJ_POP();
}
