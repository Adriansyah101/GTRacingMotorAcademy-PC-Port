/* liveblocks.h -- LIVE_BLOCKS (liveblocks.c): the large heap blocks allocated now */
#ifndef LIVEBLOCKS_H
#define LIVEBLOCKS_H
#include <stddef.h>

void gt_live_init(void);                       /* main.c, after gtracing.ini: GT_LIVE_BLOCKS */
int gt_live_has(unsigned a, unsigned n);       /* [a, a+n) inside one live tracked block */
int gt_live_selftest(char *why, size_t len);   /* --selftest: 0, or what went wrong */
#endif
