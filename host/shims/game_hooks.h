/* game_hooks.h -- every call the host's lower layers (the iOS shims in shims/ and shims/gl/, and core/'s --selftest)
   make into the game layer (games/gtracing/).

   The shims stand in for Apple's libraries and know nothing of the game; where a port feature has to act at a
   point only a shim sees (a frame presented, a socket read, a file opened, a touch handed over), the shim calls one
   of these.  Keeping them in one header is the boundary: a shim includes this, never a game file.  Shader patches
   and host uniforms are registered instead (gl.h gles_add_shader_patch / gles_add_host_uniform). */
#ifndef GT_GAME_HOOKS_H
#define GT_GAME_HOOKS_H

#include "arm_rt.h"

/* the frame (eagl.c) */
void glx_drawable_changed(u32 rb);      /* glenhance.c: the drawable's renderbuffer was (re)made: MSAA's buffers too */
u32 glx_present_fbo(void);              /* glenhance.c: MSAA's framebuffer to resolve at present (0: none) */
void glx_frame_end(void);               /* glenhance.c: anti-aliasing / anisotropic changes from the Display menu */
void gt_frame_end(void);                /* display.c: the game's end-of-frame work (Motion Blur's target) */
void wide_frame_start(void);            /* widescreen.c (WIDE_UI): the side bands of a frame with only menus */

/* GL (gles.c, gles_tex.c) */
void gpu_profile_start(void);           /* gpuprofile.c (GPU_PROFILE): the kind of GPU, read on a thread of its own */
u32 glx_logical_fbo(u32 raw);           /* glenhance.c: the framebuffer the game asked for, under MSAA's own */
void glx_tex_level(u32 target, u32 level); /* glenhance.c: a texture level was made: anisotropic filtering on it */

/* touches and the app (uikit.c)
   The PC input itself (pcinput.c: SDL's events -> touches, tilt, the controls) runs in ui_pump, once a frame from
   the run loop; what it and uikit.c share is uikit_input.h. */
void ui_pump(void);                     /* pcinput.c: SDL's events -> touches, keys, quit (main thread) */
void ui_make_cursor(void);              /* pcinput.c: the gold pointer (GT_CURSOR=0: the system's), once the window exists */
void steer_init(void);                  /* steer.c: the PC controls, once the app is up (and the features under it) */
void wide_hud_touch(float *x, float y); /* widescreen.c: a touch (stage points) on a moved HUD part -> its own place */

/* files (imp_libc.c) */
void softrefl_note_file(const char *path); /* softrefl.c (SOFT_REFLECTION): a file the game opened (the track) */

/* the network (net.c, gk.c) */
void board_poll(void);                  /* board.c (RED_BULL_BOARD): the leaderboards to and from the other players */
void board_peer_lan(u32 ip_be);         /* board.c: a player the game talks to over the LAN */
void board_peer_tun(int i);             /* board.c: a player met through the ONLINE_P2P tunnel */
void board_rx_tun(int i, const u8 *p, u32 n); /* board.c: that player's leaderboard data */
void p2p_poll(void);                    /* p2p.c (ONLINE_P2P): the tunnel's work */
void p2p_bound(u16 port);               /* p2p.c: the game bound its Local host (28190) or join search (54046) port */

/* --selftest (core/selftest.c): the game layer's own checks; 0 = passed, else why in `why` */
int board_selftest(char *why, size_t len);   /* board.c: the leaderboard's merge, the URL rule, the request body */
int cheat_selftest(char *why, size_t len);   /* maxmoney.c: the cheat code's key and pad sequences */
void gt_missing_cars_order(const u32 *place, int nh, int nvis, int *out);   /* missingcars.c: MISSING_CARS' order */

#endif
