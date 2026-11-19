/* layout.h -- the fixed address-space layout of the GT Racing host (32-bit Windows).
   Shared by C and the .S reservation.

   The game is a non-PIE armv7 Mach-O linked at 0x1000; its code is recompiled to C, its data
   lives at its own addresses so pointers mean the same thing to the game and to the host.  The
   exe is based at 0x10000 (the lowest address Windows allows) and its first section covers the
   game's range, so nothing else can be mapped there before main runs (gt.ld, reserve.S).

   Guest 0x1000..0x11000 cannot sit at its own address (nothing may be mapped below 0x10000 and
   the PE header page owns 0x10000..0x11000).  It is code except for one float table
   (0x5b0c..0x5b20, function 0x59d4) and the Mach-O header (read through _mh_execute_header):
   both are reached through LOWADDR() in the generated code / the loader, at LOWCOPY_BASE.

   GT Racing: __TEXT 0x1000..0x70b000, __DATA ..0x7a6000, __LINKEDIT ..0x8b5000. */
#ifndef GT_LAYOUT_H
#define GT_LAYOUT_H

#define GT_EXE_BASE        0x00010000
#define GT_RESERVE_START   0x00011000      /* first byte the exe reserves for the game */
#define GT_LOW_START       0x00001000      /* the copied range starts at the Mach-O header */
#define GT_LOW_END         0x00011000

#define GT_TEXT_END        0x0070B000
#define GT_MACHO_END       0x008B5000      /* end of __LINKEDIT, the last segment */
#define GT_LOWCOPY_BASE    0x008C0000      /* copy of game bytes [LOW_START, LOW_END) */
#define GT_IMPORT_BASE     0x008D0000      /* one 4-byte no-access address per import function */
#define GT_IMPORT_SIZE     0x00004000
#define GT_DATAGUARD_BASE  0x008D4000      /* no-access stand-ins for missing data imports */
#define GT_DATAGUARD_SIZE  0x00004000
#define GT_HOSTFN_BASE     0x008D8000      /* addresses handed out for host functions (IMPs, callbacks) */
#define GT_HOSTFN_SIZE     0x00008000
#define GT_RESERVE_END     0x00900000

#define GT_LOW_DELTA       (GT_LOWCOPY_BASE - GT_LOW_START)

#define GT_GAME_NAME   "GTRacingFreemiumHD"     /* the bundle and the binary in it */
#define GT_GAME_TITLE  "GT Racing: Motor Academy HD"

#endif
