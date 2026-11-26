#ifndef GT_GAME_HOOKS_H
#define GT_GAME_HOOKS_H

#include "arm_rt.h"

void glx_drawable_changed(u32 rb);
u32 glx_present_fbo(void);
void glx_frame_end(void);
void gt_frame_end(void);
void wide_frame_start(void);

void gpu_profile_start(void);
u32 glx_logical_fbo(u32 raw);
void glx_tex_level(u32 target, u32 level);
void softrefl_draw(void);

void ui_pump(void);
void ui_make_cursor(void);
void steer_init(void);
void wide_hud_touch(float *x, float y);

void softrefl_note_file(const char *path);

void board_poll(void);
void board_peer_lan(u32 ip_be);
void board_peer_tun(int i);
void board_rx_tun(int i, const u8 *p, u32 n);
void p2p_poll(void);
void p2p_bound(u16 port);

int board_selftest(char *why, size_t len);
int cheat_selftest(char *why, size_t len);
void gt_missing_cars_order(const u32 *place, int nh, int nvis, int *out);

#endif
