#ifndef LIVEBLOCKS_H
#define LIVEBLOCKS_H
#include <stddef.h>

void gt_live_init(void);
int gt_live_has(unsigned a, unsigned n);
int gt_live_selftest(char *why, size_t len);
#endif
