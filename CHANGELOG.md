# Changelog

All notable changes to GTRacingMotorAcademy-PC-Port. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).
Player-facing notes are in [`PATCHNOTES.md`](PATCHNOTES.md), fault details in [`FIXES.md`](FIXES.md).

## [Unreleased]

## [1.0.1]

### Added

- Linux x86 (32-bit ELF) build and release, `GTRacing-PC-Port-1.0.1-Linux-x86.tar.gz`, from the same generated code
  and host as the Windows exe:
  - `host/build.py --platform linux|windows|all` (Linux: gcc `-m32 -malign-double -Wall -Wextra`; on Windows through
    WSL); `recomp/compile_all.py` target `i686-linux`;
  - `host/core/gt_linux.ld`: a fixed-address ELF based at 0x10000 with the game's range as an empty `.gtimg` segment
    at 0x11000, mapped by the kernel at start; `reserve.S` for both formats;
  - `host/core/compat.h`, `compat_linux.c`: the Win32 calls the host makes, on POSIX (recursive mutexes, rwlocks,
    condition variables, threads, events, waitable timers, `mmap` / `mprotect`, `clock_gettime`), and a file layer
    that maps `\` to `/` and finds file and folder names whatever their case;
  - `host/core/compat_net.h`: Winsock names on POSIX sockets; Darwin socket options mapped to Linux numbers in
    `net.c`; adapters and the default route from `getifaddrs` and `/proc/net/route`;
  - the cloud leaderboard over libcurl, loaded with `dlopen` (`libcurl.so.4`); without it the board stays local;
  - crashes: `sigaction` handlers (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`) on an alternate stack, the
    low-page read fix through `SIGTRAP` single steps, and a scrubbed text report, `gtracing.crash`
    (`crashdump_linux.c`), with an SDL message box;
  - the clipboard for room codes and the crash box through SDL, the restart of Options > Display through
    `posix_spawn`, garage screenshots as PNG in `~/Pictures/GT Racing`;
  - `tools/release/README-linux.txt`; `tools/package_release.py --platform linux --archive` (ELF strip check, tar.gz
    with mode 755 and no owner names); `tools/layer_check.py --platform linux`; `tools/privacy_scan.py` reads
    `.tar.gz` members and the Linux user's names.
- `PATCHNOTES.md`, `CHANGELOG.md`, `FIXES.md` and `CONTRIBUTING.md`.
- The version number in the first line of the release `README.txt`.

### Changed

- `GT_SOFT_REFLECTION` softens only the reflection's backdrop and the glass, not the whole reflection map:
  - the backdrop draw of the map pass (depth test and blending off) gets mipmaps once per track load,
    `GL_LINEAR_MIPMAP_LINEAR` and `GL_TEXTURE_MIN_LOD = GT_SOFT_REFLECTION`, restored when the game leaves the map's
    framebuffer;
  - the glass branch of `gtr_high_light_reflection` (no `BODYREFLECTION`) samples the map 17 times at radius
    0.004 x `GT_SOFT_REFLECTION` (`softrefl.c` shader patch, uniform `gt_glass_soft`);
  - the body reflection and the race view's own sky are as sharp as the original's.
- Repository renamed from `GTRacingMotorAcademyRecomp` to `GTRacingMotorAcademy-PC-Port`; badge, release and issue
  links updated.
- `tools/privacy_scan.py`: the public allowlist is `<owner>/GTRacingMotorAcademy-PC-Port`, plus the owner's profile
  link and copyright line, so the repository's own credits and licence pass the scan.
- `README.md` restructured: overview, features, setup with the `add-on\` layout, configuration reference, build;
  technical detail moved to `FIXES.md`.
- `README.md` corrections: the Online room code is `GT-XXXX-XXXX-XXXX-XXXX` (it said `GT-XXXXX-XXXXX`); `GT_VSYNC` is
  on unless `GT_VSYNC=0` (it said off by default); the garage's Screenshot button writes to
  `%USERPROFILE%\Pictures\GT Racing` (it said nothing is written outside the game folder).
- `tools/release/README.txt` names the version and the update steps from 1.0.0.
- Shared host files include `compat.h` instead of `windows.h` and build paths with `GT_SEP` / `GT_SEPC`; Windows-only
  code sits in `#ifdef _WIN32` blocks. The Windows release exe's `.text`, `.data`, `.rdata`, `.idata`, `.rsrc` and
  `.tls` sections are byte-identical to the build before the Linux work; the development exe reads four test switches
  with `rt_parse_int` instead of `atoi`.
- `host/build.py`: the SWF patch step is shared by both platforms (`patch_swfs`); `--platform` chooses the target.
- `tools/package_release.py`: the stripped program's check run sets `GT_CLOUD_BOARD=0`.

### Fixed

- Linux only, found while porting (details in [`FIXES.md`](FIXES.md#linux)): GL entry points typed `stdcall`; the
  loading thread's shared context, which SDL released on EGL (`glMapBufferOES` returned `NULL`, a crash at the race
  start); the game's second `fclose` of a stream and second `free` of a block when a race ends, which glibc aborts
  on; folder listings out of name order.

### Removed

- Every comment and docstring in `host/`, `recomp/`, `runtime/`, `tests/`, `tools/` and `cloud/`, except
  licence notices, `/* fall through */`, `# noqa` and the two usage docstrings. No code change: the exe's `.text`,
  `.data`, `.rdata`, `.idata`, `.rsrc` and `.tls` sections are byte-identical to the build before the removal.

## [1.0.0]

### Added

- Static recompiler: function discovery (`recomp/discover.py`, 23,873 functions), ARM / Thumb / VFP / NEON to C
  translation (`recomp/armc.py`, `recomp/codegen.py`), import bridges from `host/core/imports.sig`, instruction tests
  against Unicorn.
- Host: Mach-O loader at the game's own addresses (`.gtimg` reserve at 0x11000), Objective-C 2 runtime, libc,
  libstdc++, pthreads and unwinding, Foundation, CoreFoundation, CoreGraphics, UIKit, OpenAL on SDL3, GameKit, and
  OpenGL ES 1.1 / 2.0 on desktop OpenGL 2.1 with software PVRTC decoding.
- Display: any resolution (`GT_RESOLUTION`), 16:10 / 16:9 / 21:9 (`GT_WIDE`, `GT_WIDE_UI`, `GT_WIDE_HUD`), full screen
  or window (`GT_FULLSCREEN`, Alt+Enter), MSAA (`GT_MSAA`), anisotropic filtering (`GT_ANISOTROPIC`), full motion blur
  (`GT_FULL_BLUR`), PostFX Classic, Classic Natural and Enhanced (`GT_POSTFX`), soft reflections on the Scandinavia
  tracks (`GT_SOFT_REFLECTION`), GPU profile (`GT_GPU_PROFILE`).
- In-game Display page (`GT_DISPLAY_MENU`) and the PC menus as build-time SWF patches (`tools/swfpatch_*.py`).
- Input: keyboard, mouse and gamepads (`GT_PC_CONTROLS`), remappable keys (`pc_keys.txt`), pad menu pointer
  (`GT_PAD_MENU`, `GT_PAD_GLIDE`), 360-degree look (`GT_RACE_LOOK`), replay zoom and pad control (`GT_REPLAY_ZOOM`,
  `GT_REPLAY_PAD`), Touch Screen Off HUD and texts (`GT_HIDE_TOUCH_HUD`, `GT_NO_TOUCH_TEXT`).
- Menus: 3D scene behind the main menu (`GT_MENU_3D`, `GT_MENU_CAR_RANDOM`) and the upgrade menu (`GT_UPGRADE_3D`),
  Exit tile (`GT_EXIT_BUTTON`), player name (`GT_FIRST_NAME`).
- Game: coin-for-cash shop (`GT_COIN_SHOP`, `GT_DYNAMIC_PRICE`), prize multiplier and repeated prizes
  (`GT_PRIZE_MULT`, `GT_REPEAT_PRIZE`), car service in coins (`GT_SERVICE_PRICE`), Top 10 Cars (`GT_TOP_CARS`).
- Multiplayer: Local and Bluetooth over LAN or VPN (`GT_LAN_MULTIPLAYER`), Online peer to peer with room codes and
  UPnP / NAT-PMP port opening (`GT_P2P_PORT`).
- Red Bull leaderboard offline (`GT_RED_BULL_BOARD`) and through a Cloudflare Worker + D1 server
  (`GT_CLOUD_BOARD`, `GT_CLOUD_BOARD_URL`, `cloud/redbull-board/`).
- Add-on folders (`GT_ADDONS`, `GT_MISSING_CARS`) and their builders: `tools/hyundai_addon.py`,
  `tools/android_free_addon.py`, `tools/premium_addon.py`.
- Crash minidump and message box (`GT_CRASH_DUMP`), log scrubbing (`host/core/scrub.c`), `GT_LOG=0`.
- Release packaging (`tools/package_release.py`), privacy scan (`tools/privacy_scan.py`), layer check
  (`tools/layer_check.py`), playtest runner (`tools/playtest.ps1`), `--selftest` (124 checks).

### Changed

- Performance: native gameswf name search and miss cache (`GT_FAST_SWF_FIND`, `GT_SWF_MISS_CACHE`), GL state and
  array shadowing (`GT_GL_STATE_CACHE`, `GT_GL_ARRAY_CACHE`, `GT_GL_QUERY_FREE`), streaming vertex buffers
  (`GT_STREAM_VBO`), SSE2 fixed-point and index loops (`GT_FIXED_FAST`, `GT_INDEX_FAST`), threaded PVRTC decoding
  (`GT_PVRTC_FAST`), live heap block table (`GT_LIVE_BLOCKS`), high-resolution waits (`GT_PRECISE_WAIT`),
  `sched_yield` as `SwitchToThread` (`GT_SCHED_YIELD`), background throttling (`GT_BACKGROUND_THROTTLE`).

### Fixed

- Port faults: buffer deletion during draws, buffers bound in another context (`GT_GL_DEFER_DELETE`), rebased mixed
  draws (`GT_STREAM_MIXED_KEEP`), full-screen composition (`GT_SWAP_COPY`), the one-pixel left column, UTF-16 C
  strings (`GT_UTF16_CSTRING`), the start-up stall on dead TCP services, the garage Screenshot button, run-loop wait
  precision. Details in [`FIXES.md`](FIXES.md).
- Original-game faults: dimmed replay (`GT_REPLAY_UNDIM`), licence medal count past the list, off-centre menu 3D
  pictures.
- `gtracing.ini` parsing: malformed numbers take the default, out-of-range numbers are clamped, NaN and infinities are
  rejected, a UTF-8 BOM is skipped, over-long lines and invalid keys are skipped and logged.

[Unreleased]: https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/compare/v1.0.1...HEAD
[1.0.1]: https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/releases/tag/v1.0.0
