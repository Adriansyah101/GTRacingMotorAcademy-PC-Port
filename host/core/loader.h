#ifndef GT_LOADER_H
#define GT_LOADER_H

#include "macho.h"
#include "rt.h"

extern u32 gt_init_funcs, gt_ninit;
const macho *gt_macho(void);
const char *gt_guard_name(u32 addr);
u32 rt_data_import(const char *name);

#endif
