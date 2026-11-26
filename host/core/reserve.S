#include "layout.h"

#ifdef _WIN32
    .section .gtimg,"bw"
    .globl _gt_reserve_start
    .globl _gt_reserve_end
_gt_reserve_start:
    .space GT_RESERVE_END - GT_RESERVE_START
_gt_reserve_end:
#else
    .section .gtimg,"aw",@nobits
    .globl gt_reserve_start
    .globl gt_reserve_end
gt_reserve_start:
    .space GT_RESERVE_END - GT_RESERVE_START
gt_reserve_end:
    .section .note.GNU-stack,"",@progbits
#endif
