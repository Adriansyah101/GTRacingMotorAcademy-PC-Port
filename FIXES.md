# Fixes

Engine, rendering, ABI and platform issues fixed in the host layer of GTRacingMotorAcademy-PC-Port, and the quirks
of the original iPad game (Free+ HD 1.3.4b) that stay as they are. Addresses are guest addresses in
`GTRacingFreemiumHD`. A key in `code` style is a `gtracing.ini` key; `0` restores the previous behaviour.

Each fix is one of two kinds:

- **Port fault:** caused by running the game on a PC through the host. Fixed with no option, or with a switch kept
  for comparison.
- **Original-game fault:** present on the iPad too. Fixed behind its own key.

**Contents:** [Rendering](#rendering) • [Widescreen UI](#widescreen-ui) •
[Timing and frame pacing](#timing-and-frame-pacing) • [ABI and runtime](#abi-and-runtime) • [Input](#input) •
[Audio](#audio) • [Platform](#platform) • [Linux](#linux) • [Original-game faults](#original-game-faults) •
[Known quirks](#known-quirks-of-the-original-game)

## Rendering

OpenGL ES 1.1 / 2.0 runs on desktop OpenGL 2.1 (`host/shims/gl/`).

- **PVRTC textures** (port fault): desktop GL has no PVRTC formats. `gles_tex.c` decodes the four PVRTC formats
  (2 and 4 bits, RGB and RGBA) to RGBA in software at upload. `GT_PVRTC_FAST` decodes pictures of 64 K pixels or more
  in bands of 16 rows on the Windows thread pool: a race load's decoding drops from 632 ms to about 130 ms, byte for
  byte equal to the one-thread result (checked by `--selftest`).
- **Shaders** (port fault): GLSL ES is rewritten to GLSL 1.20 and OES names are mapped. Host shader patches are
  registered through `gl.h`, so the shims know no game shader.
- **Buffer deleted during a draw** (port fault, a crash in the driver): the loader thread could delete a vertex buffer
  between the main thread's binding check and its draw. `glDeleteBuffers` and the draws take one lock. A buffer
  deleted and recreated under the same name is rechecked against what the context still has bound
  (`GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING`, `GL_ARRAY_BUFFER_BINDING`, `GL_ELEMENT_ARRAY_BUFFER_BINDING`), only after a
  delete, so draws outside loading pay nothing.
- **Buffer still bound in another context** (port fault, `GT_GL_DEFER_DELETE`): GL on iOS keeps such a buffer alive.
  The host holds the delete until no context's shadow state names the buffer (swept at deletes and at each present,
  at most 256 pending). This removed a stream of "attribute 3 left out" draws in the menu after a race.
- **Mixed streamed draws** (port fault, latent, `GT_STREAM_MIXED_KEEP`): a streamed draw was rebased to vertex 0,
  which also moved arrays read from buffers. Draws that mix client arrays and buffers keep their vertex numbers.
- **Swap behaviour** (port fault, `GT_SWAP_COPY`): the window's pixel format swaps by copy, so a full-screen window
  stays composed by Windows and its volume pop-up stays visible over the game.
- **One-pixel column at the left edge** (port fault): `renderbufferStorage:fromDrawable:` (`eagl.c`) rounded a
  fractional drawable width up (1365 points at scale 1.40625 = 1919.53 px became 1920), while the game sizes its
  viewports with the fraction dropped and drew 1919 columns. The renderbuffer is rounded down too.
- **Car reflections on the Scandinavia tracks** (`GT_SOFT_REFLECTION`, `GT_SOFT_REFLECTION_TRACKS`): the car shader
  (`gtr_high_light_reflection`) reads a 1024 x 1024 sphere map that `F_2a4ba4` redraws each frame from the race frame
  (`F_16eee8`). On `snowrally` and `snowrallyalt` the walls, gantry and cliffs beside the car fill that map, and the
  windows, read heavily magnified, show streaks and swirls. Same mechanism on every track; not a port fault.
  `render/softrefl.c` acts only while that map is drawn on a listed track:
  - the backdrop draw (depth test and blending off) gets mipmaps once per track load, `GL_LINEAR_MIPMAP_LINEAR` and
    `GL_TEXTURE_MIN_LOD = GT_SOFT_REFLECTION`; its filter and LOD are restored when the game leaves the map's
    framebuffer, so the race view's sky stays sharp;
  - the glass branch of the shader (no `BODYREFLECTION`) samples the map 17 times (centre, 8 taps at radius r, 8 at
    r/2 rotated 45 degrees) with r = 0.004 x `GT_SOFT_REFLECTION` while the map is live, else one sample as shipped;
  - the body's fresnel reflection is unchanged.
- **GL driver syncs** (performance, `GT_GL_STATE_CACHE`, `GT_GL_ARRAY_CACHE`, `GT_GL_QUERY_FREE`, `GT_STREAM_VBO`):
  bindings, enables and state queries are shadowed per context, so repeated calls and queries no longer reach the
  threaded driver; client arrays stream through a persistent ring buffer fenced per quarter.
- **Fixed-point and index loops** (performance, `GT_FIXED_FAST`, `GT_INDEX_FAST`): `GL_FIXED` vertex conversion and
  index range scans use SSE2, checked against the plain loops by `--selftest`.

## Widescreen UI

The game lays out gameswf menus for a 4:3 stage (`render/widescreen.c`, `GT_WIDE_UI`, `GT_WIDE_HUD`).

- gameswf is drawn at one scale centred on a wide screen; parts off the stage are not drawn, and black bands fill the
  sides. Title bars and backdrops are stretched across the screen.
- The game's own drawing placed relative to menu items follows the same transform: `GetBounds` is redirected at
  `0x26322c` (`HOST_INSNS`, `gt_wide_bounds`).
- The cockpit picture and its 3D steering wheel are enlarged to the screen's shape at `0x166254` (`gt_wheel_proj`).
- The race HUD sits at the screen's edges, as on the iPad.
- Each display object's parent transform and carousel-card state are worked out once per draw pass and cached
  (performance, `GT_WIDE_CACHE`).
- Carousel cards that leave the stage are hidden; `GT_EVENT_SIDES` and `GT_UPGRADE_SIDES` show the neighbouring cards.
- Menu 3D pictures (prize car, coin, GarageScene): see the viewport fit under
  [Original-game faults](#original-game-faults).

## Timing and frame pacing

- **Run-loop wait** (port fault): once a frame the game calls `CFRunLoopRunInMode(mode, 0.001)`, at most 1 ms on the
  iPad. The first host did it as `Sleep(whole ms + 1)`, 2 to 2.5 ms on Windows' 1 ms tick, which held uncapped menus
  at 234 fps and cost 1 to 1.5 ms of every frame on slow PCs. The wait and NSRunLoop's `runMode:beforeDate:` use a
  high-resolution waitable timer (`GT_PRECISE_WAIT`): 317 fps uncapped.
- **`sched_yield`** (port fault, `GT_SCHED_YIELD`): it returned at once, so the game's wait-for-thread loops spun
  17,000 to 115,000 times a frame while loading. It calls `SwitchToThread`, as iOS gives up the time slice.
- **Frame cap**: the present in `eagl.c` caps at `GT_FPS` (60 by default, the iPad's rate), with vsync on unless
  `GT_VSYNC=0`.
- **Background** (`GT_BACKGROUND_THROTTLE`): 30 fps behind other windows, 10 fps minimized, and a minimized race
  pauses as an iPad app does in the background. Never during multiplayer.
- **Start-up stall** (port fault): once multiplayer gave the game real sockets, Gameloft's GLXPlayer client got a TCP
  socket and waited on `SockUtility::GetHostByName` (`0x443574`), an asynchronous `gethostbyname` retried every 2 s,
  for a server that no longer exists. The main thread ran at 1 frame a second and the main menu came at 13.1 s. The
  game's `socket(SOCK_STREAM)` fails as offline again (every TCP user in the game is a dead internet service;
  multiplayer uses UDP, and `gk.c` / `p2p.c` open their TCP sockets on Winsock directly): main menu at 3.6 s.
- **Memory checks** (performance, `GT_LIVE_BLOCKS`): the GL wrapper asked `VirtualQuery` whether client arrays were
  readable on every draw. A table of live heap blocks answers instead: 28 to 50 calls a race frame became 5 to 9.

## ABI and runtime

- **Calls**: each import gets an ARM-ABI bridge generated from its signature in `host/core/imports.sig`
  (`recomp/bridgegen.py`): core registers, the stack, VFP arguments, structure returns and 64-bit words.
- **Floating point**: NaN results carry ARM's default NaN bit pattern; VFP short vectors and NEON are expanded per
  element. Every emitted instruction form is tested against Unicorn (`tests/difftest.py`).
- **Exceptions**: the game uses SjLj exceptions. The runtime keeps the SjLj frame list for host frames too, and guest
  `setjmp` / `longjmp` restore r4-r11, sp, d8 and the frame list. `--selftest` throws a `std::runtime_error`
  through a hand-made LSDA, catches by base class and with `catch (...)`, and runs a cleanup on the way.
- **libstdc++**: `std::string` keeps the copy-on-write layout the game was compiled against; trees, lists and
  iostreams follow the same ABI.
- **UTF-16 C strings** (port fault, `GT_UTF16_CSTRING`): the ASCII shortcut of `cStringUsingEncoding:` and
  `CFStringGetCStringPtr` answered UTF-16 requests with 8-bit text, so a new profile's name was saved as "v". UTF-16
  encodings now get UTF-16. Older saves load as before.
- **Generated code**: discovery finds functions from `LC_FUNCTION_STARTS`, call targets and a register tracker for
  SjLj resume labels. Registers and flags are C locals, branches are `goto` and jump tables `switch`; indirect calls go
  through a direct-mapped dispatch table.
- **Address space**: the exe is based at 0x10000 and its first section, `.gtimg`, reserves the game's range from
  0x11000 (`gt.ld`, `reserve.S`), so the image loads at its own addresses with no relocation. The Linux program is a
  fixed-address ELF with the same layout: `gt_linux.ld` puts an empty, writable `.gtimg` segment at 0x11000, which the
  kernel maps before any library loads.

## Input

- **Touch**: the mouse is the first finger (the right button a second); a touch screen gives real touches.
  `UITouch` / `UIEvent` are built from SDL events.
- **Steering** (`features/steer.c`): keys and the left stick write tilt into the game's `UIAccelerometer` object for
  the tilt settings. For the wheel and button settings, the player car's control update (vtable slot `0x7859c4` ->
  `0x1b37f0`) is wrapped and the HUD's touch slots are written as the wheel or buttons would be.
- **Camera tilt** (`GT_TILT_ROLL`, `GT_TILT_SMOOTH`): with keys or a pad, the camera's lean is scaled to 25 % and
  smoothed over 400 ms.
- **Menus** (`features/padmenu.c`, `GT_PAD_MENU`): the pad pointer snaps to buttons read from the menu SWF's display
  list and the game's hit test, and glides between them (`GT_PAD_GLIDE`).
- **Remapping** (`features/keymap.c`): driving keys and pad inputs per action, stored in `pc_keys.txt`.
- **Touch Screen Off** (`GT_PC_CONTROLS`, `GT_HIDE_TOUCH_HUD`, `GT_NO_TOUCH_TEXT`): hides the HUD's pedals and gear
  arrows and rewords "touch" and "tap" (hooked at `0x4fde1c`).

## Audio

- **OpenAL** (`host/shims/al.c`): static and streamed sources, pitch, gain, 3D attenuation and pan are mixed into one
  SDL audio stream. Music reaches it at 44,100 Hz stereo, effects at 22,050 Hz, as on the iPad.
- **AudioSession**: the iOS session calls are answered in the same shim.
- **Paid-edition audio** (`add-on\premium-audio`): the paid build's 44,100 / 22,050 Hz sounds load in place of the free
  build's through the add-on folders.

## Platform

- **Network** (`host/shims/net.c`): names do not resolve and `NSURLConnection` fails as on an iPad with no
  connection, so the dead Gameloft services give up at once. `GT_LAN_MULTIPLAYER` opens real sockets (Winsock with the
  Darwin ABI) to local addresses only, reports the PC's adapters through `getifaddrs` and implements GameKit's
  `GKSession` over UDP beacons and TCP for the Bluetooth mode (`gk.c`).
- **Online play** (`features/p2p.c`): Gameloft's lobby is gone. The Online tile runs the Local game over the internet,
  with keyed room codes `GT-XXXX-XXXX-XXXX-XXXX` and the host's UDP port opened by NAT-PMP or UPnP.
- **Garage Screenshot button** (port fault, crash): `-[EAGLView saveViewToPhotoLibrary:]` needs `CGImage`,
  `+[UIImage imageWithCGImage:]` and `UIImageWriteToSavedPhotosAlbum`. `cg.c` provides them and writes a landscape PNG
  through GDI+ to `%USERPROFILE%\Pictures\GT Racing`.
- **Missing add-on cars** (port fault of the add-on design, `GT_MISSING_CARS`): a save keeps a car's number, and the
  car table is built from the files present. A car whose add-on is gone is hidden, and the save loads.
- **Crashes** (`host/core/crashdump.c`): a minidump and a message box, scrubbed as described in the README's Privacy
  section.
- **Settings parsing** (`host/core/settings.c`): malformed numbers take the default, out-of-range numbers are clamped,
  NaN and infinities are rejected, a UTF-8 BOM is skipped, over-long lines and invalid key names are skipped and
  logged.

## Linux

The Linux program runs the same generated code and host as the Windows exe. What Windows gave for free, and what
differs:

- **File names in any case** (`compat_linux.c`, `gt_ci_path`): the iPad's file system and NTFS ignore case, ext4
  does not. Every open, stat, listing, rename and delete maps `\` to `/` and, when the exact name is not there, finds
  each folder and file whatever its case (one cached listing per folder, refreshed when the folder changes).
- **Sorted folder listings**: NTFS and the iPad's HFS+ list a folder in name order; `readdir` does not.
  `FindFirstFileA` reads the whole folder and sorts it without regard to case, so `NSFileManager`, the `patch\`
  index and the add-on scan see the same order as on Windows.
- **Double fclose on game streams** (original game, harmless on iOS and Windows): the game closes some of its `FILE`s a
  second time, for example while it loads the profile (`F_270ac0`). iOS and msvcrt keep `FILE`s in a fixed pool, so
  a closed stream stays a valid, closed `FILE`: the second `fclose` returns `EOF`. glibc frees the `FILE` at the
  first `fclose`; the second one aborted with "double free", or, once the memory held another stream, closed that
  one. `fopen` (`fopen64` under `_FILE_OFFSET_BITS=64`) and `fclose` are wrapped: `fclose` flushes the stream, closes
  its file and keeps the `FILE` as a closed stream that is never freed, so no later `fopen` gets its address; a
  second `fclose` meets a closed stream, is logged once and returns `EOF` with `EBADF`.
- **Double free on race exit** (seen when a race ends; iOS and Windows let it pass): leaving a race frees a list of
  objects, some of them already freed (`F_289c64` -> `F_1b1bf0`, the game's `free`). iOS's malloc reports such a
  free and goes on, and msvcrt's `HeapFree` returns an error; glibc stops the program ("double free or corruption").
  On Linux `free` holds each block back in a queue (up to 64 K blocks or 32 MB, blocks over 256 KB excepted) before
  glibc gets it, so a second free of a held block is seen, logged once and ignored; `realloc` takes a held block
  back out.
- **OpenGL calling convention** (port fault, Linux only): the GL entry points were typed `stdcall`, which is right for
  `opengl32.dll` and wrong for Linux's `libGL`, where the caller pops the arguments. The stack moved 4 bytes per
  argument at each call and the next SSE spill faulted. `GT_GLAPI` is `stdcall` on Windows only.
- **Shared contexts on EGL** (port fault, Linux only): the game fills buffers and uploads textures from other
  threads through a context shared with the drawing one. WGL lets that context use the window's surface while the
  main thread draws to it; EGL (Wayland, and SDL's offscreen driver) refuses with `EGL_BAD_ACCESS`. SDL's
  `SDL_GL_MakeCurrent(NULL, context)` is no way out: on its EGL drivers it releases the context and still reports
  success, so every GL call on that thread did nothing, `glMapBufferOES` returned `NULL` and the race start wrote
  a mesh's transformed vertices to address 0 (`F_253114` -> `F_184ee8`). When the window's surface is busy,
  `eagl.c` now calls `eglMakeCurrent` itself with no surface (`EGL_KHR_surfaceless_context`); on GLX the first call
  succeeds as on Windows.
- **Low-page reads** (`main.c`): the reads of guest 0x1000-0x10fff that Windows serves through a vectored exception
  handler are served from `SIGSEGV`: the register is moved to the low copy at 0x8c0000, the trap flag single-steps
  the instruction and `SIGTRAP` moves it back.
- **Socket options**: Winsock uses the Darwin `SOL_SOCKET` numbers the game passes; Linux has its own. `net.c` maps
  them, `SO_RCVTIMEO` takes a `timeval`, `select` gets its `nfds`, and `MSG_NOSIGNAL` keeps a closed peer from
  stopping the game with `SIGPIPE`.
- **Structure layout**: MSVC's 32-bit ABI and the game's ARM code align 64-bit fields to 8 bytes; the i386 System V
  ABI to 4. The Linux build uses `-malign-double` everywhere, and the host reads file information through `statx`,
  whose layout does not depend on that flag.
- **Stack size**: the exe asks Windows for a 64 MB main stack. The Linux program runs the game on a thread with a
  64 MB stack and gives each signal handler an alternate stack.

## Original-game faults

Fixed behind their own keys, on by default.

- **Dimmed replay** (`GT_REPLAY_UNDIM`): the race end's `FinishLinePopup` plays "Show" on `LowBarMenu.btnGreyOverlay`
  (menu sprite 8), and the grey rectangle stays over the replay's 3D picture and HUD. The overlay is hidden in the
  replay.
- **Licence medal count** (`features/licences.c`): the counters at `0x2a148c` (all classes) and `0x2a13f8` (one class,
  one medal) loop from place = count down to 0, one place past the list. The free build never shows it. With
  `add-on\premium-licences` removed after its tests were driven, class C's extra place still held a medal and was
  counted. A place past a class's list now has no medal; the save keeps it for the add-on.
- **Menu 3D picture placement**: the viewport fit at `0x257200` makes the box the screen's shape but moves it the wrong
  way (`x0 += (w - new_w) * -0.5`), about 60 pixels off on the iPad and hundreds on a wide screen. `HOST_INSNS
  0x257244` hands `gt_wide_fit` a box placed by the iPad's own 4:3 fit, then widened around its middle.

## Known quirks of the original game

Kept as they are.

1. **Restart is hidden in the pause menu** when the car's service points are 0 (`0x280494`).
2. **Display and Controls open about 40 frames late:** the game's own camera flight to the menu runs first.
3. **Resolution and aspect ratio need a restart:** the screen, the render targets and the 3D view's shape are sized
   once at start.
4. **No intro video:** the bundle's one movie, `gameloft_1024x768.mov`, is played only by the app delegate's
   `startMovie:` (`0x2fd0e8`), which nothing sends.
5. **Reflection map on near scenery:** the race's live reflection map is a wide view from the car on every track. Tall
   scenery right beside the track (the Scandinavia walls and cliffs) fills it; `GT_SOFT_REFLECTION` softens it there.
6. **Gameloft's servers are gone.** Ads, Customer Care, Gameloft LIVE and the online lobby do not work. The Display
   page takes Customer Care's tile; the port's peer-to-peer Online play and its own Red Bull leaderboard replace the
   lobby and the online board.
7. **gameswf name lookups are slow:** the game walks the display tree by name, 20 to 30 % of the main thread in the
   menus. The iPad does the same walk; `GT_FAST_SWF_FIND` and `GT_SWF_MISS_CACHE` do it in host code with a cache of
   misses (main menu 403 to 462 fps uncapped).
