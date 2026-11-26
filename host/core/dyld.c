#include <stdio.h>
#include <string.h>

#include "ns.h"

static char exe_path[512];
static const char *guest_exe(void)
{
    if (!exe_path[0])
        snprintf(exe_path, sizeof exe_path, "%s/%s", gt_guest_bundle, GT_GAME_NAME);
    return exe_path;
}

u32 h_dladdr(u32 addr, u32 info)
{
    if (!info)
        return 0;
    if (addr >= GT_LOW_START && addr < GT_MACHO_END) {
        wr32(info, GA(guest_exe()));
        wr32(info + 4, gt_mh_header);
        wr32(info + 8, 0);
        wr32(info + 12, 0);
        return 1;
    }
    const char *n = rt_addr_name(addr);
    if (n) {
        wr32(info, GA("/usr/lib/libSystem.B.dylib"));
        wr32(info + 4, 0);
        wr32(info + 8, GA(n));
        wr32(info + 12, addr);
        return 1;
    }
    return 0;
}

u32 h_dlsym(u32 handle, u32 name)
{
    (void)handle;
    char sym[256];
    snprintf(sym, sizeof sym, "_%s", (const char *)GP(name));
    u32 a = rt_import_addr(sym);
    rt_log("[dyld] dlsym(%s): %s\n", (const char *)GP(name), a ? "an import" : "none");
    return a;
}

u32 h_dyld_image_count(void) { return 1; }
u32 h_dyld_get_image_name(u32 i) { return i == 0 ? GA(guest_exe()) : 0; }
u32 h_dyld_get_image_header(u32 i) { return i == 0 ? gt_mh_header : 0; }
