/* reserve.S -- the game's address range as an uninitialised section of the exe itself: it costs no
   file space, and the kernel reserves it when it maps the exe (see gt.ld, layout.h). */
#include "layout.h"

    .section .gtimg,"bw"
    .globl _gt_reserve_start
    .globl _gt_reserve_end
_gt_reserve_start:
    .space GT_RESERVE_END - GT_RESERVE_START
_gt_reserve_end:
