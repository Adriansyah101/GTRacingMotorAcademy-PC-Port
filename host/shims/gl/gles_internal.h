/* gles_internal.h -- what the parts of the OpenGL ES layer share: gles.c (the calls, the state, the vertex arrays,
   the shaders) and gles_tex.c (the texture formats, PVRTC). */
#ifndef GT_GLES_INTERNAL_H
#define GT_GLES_INTERNAL_H

#include "abi.h"
#include "game_hooks.h"

/* the contexts (gles.c, for eagl.c) */
extern int gles_api;                    /* the API of the last EAGLContext made: 1 or 2 */
void gles_context_created(void);
void gles_context_current(void *gl);
void gles_context_destroyed(void *gl);
void gles_state_forget(void);           /* the shadowed state is unknown (after the host's own GL work) */
void gles_pvrtc_stats(void);            /* gles_tex.c: GT_GL_STATS's PVRTC line */

/* --selftest's random bytes (xorshift32): the same sequence on every run */
static inline u32 st_rand(u32 *x)
{
    *x ^= *x << 13, *x ^= *x >> 17, *x ^= *x << 5;
    return *x;
}

#endif
