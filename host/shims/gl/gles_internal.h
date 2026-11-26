#ifndef GT_GLES_INTERNAL_H
#define GT_GLES_INTERNAL_H

#include "abi.h"
#include "game_hooks.h"

extern int gles_api;
void gles_context_created(void);
void gles_context_current(void *gl);
void gles_context_destroyed(void *gl);
void gles_state_forget(void);
void gles_pvrtc_stats(void);

static inline u32 st_rand(u32 *x)
{
    *x ^= *x << 13, *x ^= *x >> 17, *x ^= *x << 5;
    return *x;
}

#endif
