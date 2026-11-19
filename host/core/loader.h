/* loader.h -- the mapped game image (loader.c). */
#ifndef GT_LOADER_H
#define GT_LOADER_H

#include "macho.h"
#include "rt.h"

extern u32 gt_init_funcs, gt_ninit;    /* __mod_init_func: address and count */
const macho *gt_macho(void);
const char *gt_guard_name(u32 addr);   /* missing data import behind a guard address */
/* Host data for a data import (imports.c etc.), 0 = not provided. */
u32 rt_data_import(const char *name);

#endif
