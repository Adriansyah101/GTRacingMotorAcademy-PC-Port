/* abi.h -- reading arguments the way the iOS ARM ABI passes them (bridges.c and raw bridges).
   Word i of the argument list is r0-r3 for i < 4, then the guest stack at sp + 4 * (i - 4). */
#ifndef GT_ABI_H
#define GT_ABI_H

#include "rt.h"

typedef struct { float x, y; } GtPoint;
typedef struct { float w, h; } GtSize;
typedef struct { float x, y, w, h; } GtRect;
typedef struct { float a, b, c, d, tx, ty; } GtAffine;

static inline u32 arg_word(CPU *c, int i)
{
    return i < 4 ? c->r[i] : *(ua32 *)GP(c->r[13] + 4u * (u32)(i - 4));
}
static inline float arg_f(CPU *c, int i) { return bits2f(arg_word(c, i)); }
static inline double arg_d(CPU *c, int i)
{
    return bits2d((u64)arg_word(c, i) | (u64)arg_word(c, i + 1) << 32);
}
static inline u64 arg_l(CPU *c, int i) { return (u64)arg_word(c, i) | (u64)arg_word(c, i + 1) << 32; }

static inline void ret_i(CPU *c, u32 v) { c->r[0] = v; }
static inline void ret_l(CPU *c, u64 v) { c->r[0] = (u32)v; c->r[1] = (u32)(v >> 32); }
static inline void ret_f(CPU *c, float v) { c->r[0] = f2bits(v); }
static inline void ret_d(CPU *c, double v) { ret_l(c, d2bits(v)); }

/* The variadic words from word `first` on, made contiguous (what an ARM callee does by pushing
   r0-r3 next to its stack arguments).  The layout is an i386 va_list: 4-byte words, 64-bit
   values in two words with no alignment, so the host's vsnprintf can take it as it is. */
#define VA_WORDS 96
static inline const u32 *va_words(CPU *c, int first, u32 *buf)
{
    int n = 0;
    for (int i = first; i < 4; i++)
        buf[n++] = c->r[i];
    u32 sp = c->r[13] + 4u * (u32)(first > 4 ? first - 4 : 0);
    while (n < VA_WORDS)
        buf[n++] = *(ua32 *)GP(sp), sp += 4;
    return buf;
}

#endif
