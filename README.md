# GTRacingMotorAcademy-PC-Port

<p align="center">
  <a href="https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/releases/latest"><img src="https://img.shields.io/github/v/release/Adriansyah101/GTRacingMotorAcademy-PC-Port" alt="Latest release"></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-MIT%20OR%20GPL--3.0-blue" alt="License: MIT OR GPL-3.0-only"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue" alt="Platform: Windows and Linux">
  <img src="https://img.shields.io/badge/CPU-x86%20(Intel%20%2F%20AMD)-blue" alt="CPU: x86 (Intel or AMD)">
  <a href="https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/releases"><img src="https://img.shields.io/github/downloads/Adriansyah101/GTRacingMotorAcademy-PC-Port/total" alt="Downloads"></a>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#system-requirements">Requirements</a> •
  <a href="#setup-instructions-for-players">How to install</a> •
  <a href="#controls">Controls</a> •
  <a href="#configuration">Configuration</a> •
  <a href="#troubleshooting">Troubleshooting</a> •
  <a href="#known-issues">Known Issues</a> •
  <a href="PATCHNOTES.md">Patch Notes</a> •
  <a href="#privacy">Privacy</a> •
  <a href="#build-instructions-for-developers">How to compile</a> •
  <a href="#contributing">Contributing</a> •
  <a href="#license">License</a>
</p>

GT Racing: Motor Academy is a racing game by Gameloft for the iPhone and iPad, built around driving licences: tests
and events earn the licences, cars and money that open the next races.

This repository is a native Windows and Linux x86 port of **the iPad game GT Racing: Motor Academy Free+ HD, version
1.3.4b**. The game's 32-bit ARMv7 binary is statically recompiled: every function is translated ahead of time into
C, compiled for x86 and linked with a host that replaces the iOS libraries the game imports (UIKit, OpenGL ES,
OpenAL, GameKit, Foundation and the rest) with SDL3 and desktop OpenGL. There is no CPU emulation and no iOS
emulation at run time. Both platforms run the same generated code and the same host; Linux adds a small platform
layer (`host/core/compat_linux.c`).

The port adds PC features, all on by default. Each has a `GT_*` key in `gtracing.ini`; `0` restores the game's own
behaviour. Current version: **1.0.1**.

## Features

| Feature | Where to change it | Default |
|---|---|---|
| Any resolution, 16:10, 16:9 and 21:9 screens with the menus and HUD kept in shape (`WIDE_UI`) | Options > Display\*, `GT_RESOLUTION` `GT_WIDE` `GT_WIDE_UI` | the monitor's |
| Full screen or a window | Alt+Enter, Options > Display\*, `GT_FULLSCREEN` | full screen |
| MSAA and anisotropic filtering | Options > Display\*, `GT_MSAA` `GT_ANISOTROPIC` | 4x, 16x |
| Soft car reflections on the Scandinavia tracks: glass softened, body sharp | `GT_SOFT_REFLECTION` `GT_SOFT_REFLECTION_TRACKS` | 3 |
| PostFX: the paid 2010 iPad version's bloom, or the game's unused bloom pass, fixed | Options > Display\*, `GT_POSTFX` | Enhanced |
| The iPad 2's full motion blur in the iPad 3 mode | Options > Display\*, `GT_FULL_BLUR` | auto |
| The 3D car scene behind the main menu | `GT_MENU_3D` | on |
| Keyboard, mouse and gamepads (Xbox and PlayStation layouts), remappable keys | Options > Controls\*, `GT_PC_CONTROLS` | on, Touch Screen Off |
| Menus driven by the keys or a pad, with a pointer that snaps to the buttons | `GT_PAD_MENU` | on |
| Look back and a 360-degree look around the car | `GT_RACE_LOOK` | on |
| Replay zoom with the mouse wheel, replay control from the pad | `GT_REPLAY_ZOOM` `GT_REPLAY_PAD` | on |
| Local and Bluetooth play over a LAN or a VPN; Online play peer to peer through room codes | `GT_LAN_MULTIPLAYER` | on |
| The Red Bull leaderboard, kept locally and shared through a cloud server | `GT_RED_BULL_BOARD` `GT_CLOUD_BOARD` | on |
| Coin-for-cash shop, full prizes for repeated results (3x), car service paid in coins | `GT_COIN_SHOP` `GT_REPEAT_PRIZE` `GT_PRIZE_MULT` `GT_SERVICE_PRICE` | on |
| Offline Top 10 Cars list | `GT_TOP_CARS` | on |
| Exit tile on the main menu, Name tile on the multiplayer page | `GT_EXIT_BUTTON`, Name\* | on |
| Add-on cars and the paid edition's licence tests and audio, loaded from `add-on\` | `GT_ADDONS` | on when the files are there |

\* The PC menus (Options > Display, the keys page in Options > Controls, the Exit and Name tiles) are the game's own
menu files with changes, so they are not in the download. They go in the optional `patch\` folder. Without them the
game shows its own menus and `gtracing.ini` holds the same settings.

[`PATCHNOTES.md`](PATCHNOTES.md) lists what changed in each release, [`FIXES.md`](FIXES.md) the engine, rendering and
platform fixes.

## Disclaimer

**GT Racing: Motor Academy** is a trademark of Gameloft. The game and its assets are the property of their respective
owners, as are the car makers' names and marks. This is an unofficial, non-commercial fan project. It is not
affiliated with Gameloft, and Gameloft has not made, approved or supported it.

The authors do not promote or condone piracy. **No copyrighted Gameloft assets are distributed**: this repository and
its releases hold no textures, models, sound, menus, data, pictures or executable code of the game. To use this
software you need your own legally obtained copy of the iPad game (`GTRacingFreemiumHD.app`).

> [!NOTE]
> The program's icon is drawn from your copy of the game when the program is built; this repository holds none of the
> game's art.

## System Requirements

| | Windows | Linux |
|---|---|---|
| Processor | x86 (Intel or AMD). The program is 32-bit; tested on 64-bit Windows | x86 (Intel or AMD). The program is 32-bit (i386); runs on a 64-bit system with the 32-bit libraries below |
| System | Windows 10 or 11. Tested on Windows 11 | Debian 13, Ubuntu 25.04 or newer (glibc 2.38+). Tested on Debian 13 under WSL2 |
| Libraries | Included (`SDL3.dll`) | The system's 32-bit SDL 3.2+, OpenGL and Mesa (or the GPU vendor's 32-bit driver); libcurl for the online leaderboard (optional) |
| Graphics | OpenGL 2.1 or newer. Tested on an NVIDIA RTX 3050 Ti and on AMD Radeon graphics built into the processor | OpenGL 2.1 or newer, on X11 or Wayland |
| Disk | About 1.5 GB for the game, 21 MB for the port | About 1.5 GB for the game, 21 MB for the port |

**Not supported:** ARM processors and macOS. Windows on ARM runs x86 programs through its own emulation; the port has
not been tried there.

## Setup Instructions (For Players)

1. Get your own copy of the iPad game, `GTRacingFreemiumHD.app`, version 1.3.4b. An `.ipa` is a zip archive; the
   folder is `Payload\GTRacingFreemiumHD.app` inside it.

   > [!IMPORTANT]
   > The port is made from **one build** of the game. Other builds are not supported. SHA-1 of
   > `GTRacingFreemiumHD.app\GTRacingFreemiumHD` **must** be `eaeca1915d8b0a250583afed587f61ecb85a728a`.
   >
   > To check yours: `certutil -hashfile "GTRacingFreemiumHD.app\GTRacingFreemiumHD" SHA1` on Windows,
   > `sha1sum GTRacingFreemiumHD.app/GTRacingFreemiumHD` on Linux.

2. Download `GTRacing-PC-Port-1.0.1-Windows-x86.zip` from
   [**Releases**](https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/releases/latest). Check it with
   `certutil -hashfile GTRacing-PC-Port-1.0.1-Windows-x86.zip SHA256` against the SHA-256 in
   [`PATCHNOTES.md`](PATCHNOTES.md). `SHA256SUMS.txt` in the zip lists each file inside it.
3. Extract it to `<game_dir>`, any folder you can write to.
4. Copy the contents of your `GTRacingFreemiumHD.app` into the empty `<game_dir>\GTRacingFreemiumHD.app` folder, so
   that `<game_dir>\GTRacingFreemiumHD.app\GTRacingFreemiumHD` exists.
5. Start `gtracing.exe`. Keep `SDL3.dll` next to it.

### Linux

1. Install the 32-bit libraries. On Debian 13, Ubuntu 25.04 or newer:

   ```
   sudo dpkg --add-architecture i386
   sudo apt update
   sudo apt install libsdl3-0:i386 libgl1:i386 libgl1-mesa-dri:i386
   sudo apt install libcurl4t64:i386    # optional: the online leaderboard
   ```

   Other distributions: the same libraries in their 32-bit form. Ubuntu 24.04 and older have no SDL3 package. With
   NVIDIA's own driver, install its 32-bit libraries too.
2. Download `GTRacing-PC-Port-1.0.1-Linux-x86.tar.gz`, check it with `sha256sum` against
   [`PATCHNOTES.md`](PATCHNOTES.md), and unpack it: `tar -xzf GTRacing-PC-Port-1.0.1-Linux-x86.tar.gz`.
3. Copy the contents of your `GTRacingFreemiumHD.app` into the empty `GTRacingFreemiumHD.app` folder inside it. File
   and folder names may keep any mix of upper and lower case: the port looks them up without regard to case, as the
   iPad's file system does.
4. Run `./gtracing` from that folder (`chmod +x gtracing` first if the archive lost its permissions).

The folder then holds the following (on Linux, `gtracing` in place of `gtracing.exe`, and no `SDL3.dll`):

```
<game_dir>\
  gtracing.exe
  SDL3.dll
  gtracing.ini               settings
  README.txt
  IMPORTANT.TXT              where to find the files for patch\ and add-on\
  SHA256SUMS.txt
  GTRacingFreemiumHD.app\    your copy of the game (empty in the zip)
  patch\                     optional: the PC menus (empty in the zip)
  add-on\                    optional: one folder per add-on (empty in the zip)
    hyundai\                 Hyundai Genesis Coupe and i-oniq
    dodge\                   Dodge Charger 2012
    ferrari\                 Ferrari 288 GTO
    premium-licences\        the paid edition's three extra licence tests
    premium-audio\           the paid edition's 44 kHz audio
  home\                      made at the first start: your save
  gtracing.log               written at each start
```

Each empty folder in the archive holds a note, which the game ignores, on what goes in it.

**Add-ons** are built from other editions of the game you own, by the tools in [Add-ons](#add-ons). None is in the
download, and the game runs without them. Delete or move an add-on's folder to turn it off. A save that holds an
add-on car still loads without that add-on; the car is hidden until the folder is back.

### Updating, backing up and uninstalling

- **Progress** is in `home\`: `home\Documents` holds the game's save, `home\Library\Preferences` the port's own files
  (controls, keys, player name, the local leaderboard). Back up `home\` and `gtracing.ini`.
- **Updating from 1.0.0:** extract 1.0.1 to a new folder, put the game in it, then copy `home\`, `gtracing.ini`,
  `patch\` and `add-on\` from the old folder. Saves and settings files have the same format on both platforms, so a
  Windows `home\` also works on Linux.
- **Uninstalling:** delete the folder. The only files written outside it are the garage's screenshots, in
  `%USERPROFILE%\Pictures\GT Racing` on Windows and `~/Pictures/GT Racing` on Linux.
- `gtracing.log`, `gtracing.dmp` (Windows) and `gtracing.crash` (Linux) are safe to delete at any time.

### Command line

| Option | What it does |
|---|---|
| `--app "<path to GTRacingFreemiumHD.app>"` | Use a copy of the game kept elsewhere. Without it, the port looks next to itself, then up to three folders up |
| `--selftest` | Run the host's self-checks instead of the game |

## Controls

| | Keyboard | Gamepad (Xbox / PlayStation) |
|---|---|---|
| Steer | A / D, Left / Right | left stick |
| Gas / brake | W / S, Up / Down | RT / LT (R2 / L2) |
| Gear up / down | Shift / Ctrl | A / X (Cross / Square) |
| Change camera | C | RB (R1) |
| Look back | V (held) | LB (L1) held |
| Look around (chase views) | numpad 1-9 | right stick |
| Pause | Esc | Menu (Options) |
| Menus | W A S D or the arrows, Enter, Esc | D-pad or left stick, A (Cross), B (Circle) |
| Full screen / window | Alt+Enter | |

The mouse stands in for the touch screen: click to tap, drag to swipe, wheel to pinch in the replay.

Options > Controls has a **Touch Screen** switch, Off by default. Off gives keyboard and pad options (camera tilt,
manual or automatic gears, auto gas), the keys page (Options > Controls > Keyboard / Gamepad, with the PC menus), a
race HUD without touch icons, and "press" or "click" in place of "touch" in the texts. On restores the game's touch
controls.

## Configuration

`gtracing.ini` next to the program holds `KEY=VALUE` lines; `#` starts a comment. An environment variable of the same
name overrides the file.

- A missing or blank key, or a value that is not a number (`GT_FPS=abc`), takes the default. A number outside a
  setting's range is clamped (`GT_CURSOR_SIZE=9` is 6).
- A UTF-8 byte order mark is skipped. Lines over 510 characters and keys that are not names (letters, digits, `_`) are
  skipped and logged.
- `GT_INI=<file>` reads another file. `GT_HOME` (save folder) and `GT_LOG` (log file; `0`: no log) are paths.
- The in-game **Display** page writes back to the file. Full screen, anti-aliasing, anisotropic filtering, motion blur
  and PostFX apply at once; resolution and aspect ratio apply after a restart (Restart to Apply).

### Keys in the release `gtracing.ini`

| Key | Default | Meaning |
|---|---|---|
| `GT_DEVICE` | `ipad3` | `ipad3`: the iPad 3's Retina art; `ipad2`: the iPad 2's |
| `GT_RESOLUTION` | `monitor` | picture height in pixels: `monitor`, a number such as `1080`, or `0` for the device's own |
| `GT_WIDE` | `monitor` | screen shape: `monitor`, or the width in points: `1024` (4:3), `1229` (16:10), `1366` (16:9), `1792` (21:9) |
| `GT_FULLSCREEN` | `1` | `1` full screen, `0` a window |
| `GT_MSAA` | `auto` | anti-aliasing samples: `0`, `2`, `4`, `8`, or `auto` (4) |
| `GT_ANISOTROPIC` | `16` | anisotropic filtering: `1` (off), `2`, `4`, `8`, `16` |
| `GT_FULL_BLUR` | `auto` | motion blur: `1` full, `0` light, `auto` (light on integrated GPUs) |
| `GT_POSTFX` | `4` | `0` Off (as shipped), `2` Classic, `3` Classic Natural, `4` Enhanced |
| `GT_BACKGROUND_THROTTLE` | `1` | 30 fps behind other windows, 10 fps and a paused race when minimized (never in multiplayer) |
| `GT_CURSOR_SIZE` | `3` | mouse pointer size, a multiple of 32 px (1 to 6) |
| `GT_PAD_GLIDE` | `140` | ms for the pad pointer to glide to the next button; `0` jumps |

### Other keys

These are not in the release file. A line added to it changes them.

| Key | Default | Meaning |
|---|---|---|
| `GT_SOFT_REFLECTION` | `3` | car reflections on the listed tracks: the windows' reflection and the reflection's sky and far mountains softened, the body's track reflection kept sharp; `1` to `6` = softer; `0` = the original |
| `GT_SOFT_REFLECTION_TRACKS` | blank | tracks by file name, comma-separated, or `all`; blank = `snowrally,snowrallyalt` (the two Scandinavia tracks) |
| `GT_LAN_MULTIPLAYER` | `1` | Local, Bluetooth and Online multiplayer |
| `GT_LAN_PEERS` | blank | extra addresses Local should reach, comma-separated |
| `GT_P2P_PORT` | `28200` | the UDP port an Online room opens on the router |
| `GT_CLOUD_BOARD` | `1` | share the Red Bull leaderboard through `GT_CLOUD_BOARD_URL`; `0` keeps it offline |
| `GT_CLOUD_BOARD_URL` | `https://gt-redbull-board.gtracing-pc.workers.dev` | the leaderboard server (blank: this default) |
| `GT_FPS`, `GT_VSYNC` | `60`, `1` | frame cap (`0`: none); vsync (`0`: off) |
| `GT_LANG` | `en` | the game's language (e.g. `fr`) |
| `GT_GPU_PROFILE` | `auto` | `igpu` or `full` skips the GPU detection that `auto` settings follow |

### Feature switches

Each port addition is on by default and `0` turns it off.

| Key | Feature |
|---|---|
| `GT_DISPLAY_MENU` | the Display page in place of Customer Care |
| `GT_WIDE_UI`, `GT_WIDE_HUD` | menus and HUD kept in shape on wide screens; HUD at the screen's edges |
| `GT_EVENT_SIDES`, `GT_UPGRADE_SIDES` | the neighbouring carousel cards on wide screens |
| `GT_MENU_3D`, `GT_MENU_CAR_RANDOM` | the 3D scene behind the main menu, and a varied car in it |
| `GT_UPGRADE_3D` | the 3D scene behind the upgrade menu (off by default; `1` turns it on) |
| `GT_PC_CONTROLS` | the Touch Screen switch and the keyboard / pad rows in Options > Controls |
| `GT_TOUCH_SCREEN`, `GT_CAMERA_TILT_DEFAULT` | `1`: Touch Screen On, or Camera Tilt On, when no choice is saved yet |
| `GT_HIDE_TOUCH_HUD`, `GT_NO_TOUCH_TEXT` | touch icons hidden and "touch" texts reworded with Touch Screen Off |
| `GT_PAD_MENU`, `GT_CURSOR` | the snapping pad pointer; the gold mouse pointer (`0`: the Windows arrow) |
| `GT_RACE_LOOK` | the 360-degree look (`GT_RACE_LOOK_SMOOTH`, ms, default 60) |
| `GT_TILT_ROLL`, `GT_TILT_SMOOTH` | camera tilt strength (percent, default 25) and smoothing (ms, default 400) |
| `GT_REPLAY_UNDIM`, `GT_REPLAY_ZOOM`, `GT_REPLAY_PAD`, `GT_REPLAY_TOUCH_ICONS`, `GT_REPLAY_HINT_PAD` | replay fixes and controls |
| `GT_EXIT_BUTTON` | the Exit tile |
| `GT_COIN_SHOP`, `GT_DYNAMIC_PRICE` | the coin-for-cash shop and its changing rate (`GT_COIN_RATE`, default 1100) |
| `GT_PRIZE_MULT`, `GT_REPEAT_PRIZE` | prize multiplier (default 3; `1` or a value outside 1 to 100 is the original); full prizes for repeated results |
| `GT_SERVICE_PRICE` | car service price in coins (default 250; `0` is the original service) |
| `GT_TOP_CARS`, `GT_RED_BULL_BOARD`, `GT_FIRST_NAME` | Top 10 Cars; the local Red Bull leaderboard; the name asked at the first start |
| `GT_ADDONS`, `GT_MISSING_CARS` | the add-on folders; loading a save whose add-on cars are missing |
| `GT_CRASH_DUMP` | the crash minidump (Windows) or crash report (Linux) |

Port fixes and optimisations (`GT_SWAP_COPY`, `GT_UTF16_CSTRING`, `GT_GL_STATE_CACHE` and the others) are also on by
default, and `0` turns each off, for comparison or when a driver misbehaves. [`FIXES.md`](FIXES.md) lists each key with
the fault or cost it addresses.

### Performance tips

A race runs at over 300 fps uncapped on the GPUs above; the cap is 60 fps (`GT_FPS`). On a slower PC, in this order:

1. Set `GT_MSAA` to 2 or 0.
2. Set PostFX to Classic Natural or Off, and Motion Blur to light (`GT_FULL_BLUR=0`).
3. Lower `GT_RESOLUTION`, for example to `720`.

## Troubleshooting

| Problem | What to do |
|---|---|
| "no GTRacingFreemiumHD.app next to the program (or give --app &lt;folder&gt;)" | Put `GTRacingFreemiumHD.app` next to `gtracing.exe` (or up to three folders up), or name it with `--app` |
| "The GTRacingFreemiumHD.app folder next to gtracing.exe has no game in it" | Copy your game's files into it, so that `GTRacingFreemiumHD.app\GTRacingFreemiumHD` exists |
| "loading the game: not ARM ..." or another loading error | The file is not the iPad game's program. Check its SHA-1 (step 1) |
| "SDL3.dll was not found" | Keep `SDL3.dll` next to `gtracing.exe` |
| "Windows protected your PC" (SmartScreen) or an antivirus warning | The exe is not signed. Click **More info**, then **Run anyway**, or build it from this source. The SHA-256 checks the download |
| No Display page, keys page, Exit or Name tile | `patch\` is empty. The same settings are in `gtracing.ini` |
| A new resolution or aspect ratio does not show | Use Restart to Apply on the Display page, or start the game again |
| Other players are not found in Local or Bluetooth | Both PCs must be on one network or one VPN. Allow `gtracing.exe` on private networks in Windows Firewall. `GT_LAN_PEERS` can name the other PC's address |
| An Online room cannot be joined | The host's router must open UDP `GT_P2P_PORT` (28200) through UPnP or NAT-PMP. Otherwise forward it by hand, or play Local over a VPN |
| The Red Bull leaderboard shows only local times | The server is not reached: check the connection and that `GT_CLOUD_BOARD=0` is not set |
| Linux: "error while loading shared libraries: libSDL3.so.0" | Install `libsdl3-0:i386` (step 1 of [Linux](#linux)) |
| Linux: "cannot execute: required file not found" | The 32-bit loader is missing: install `libc6:i386` with the libraries above |
| Linux: "Permission denied" | `chmod +x gtracing` |
| Linux: the leaderboard log says "no 32-bit libcurl" | Install `libcurl4t64:i386`, or leave the board offline |
| A black screen, a crash or an error box | Report it with `gtracing.log` (see below) |

## Known Issues

1. **Few GPUs tested:** an NVIDIA RTX 3050 Ti and integrated AMD Radeon graphics, on Windows 11. Intel graphics and
   AMD graphics cards are untested.
2. **Multiplayer** has been tested with two copies of the game on one PC. Port opening on a real router (UPnP,
   NAT-PMP) is untested.
3. **Resolution and aspect ratio need a restart.** The game sizes its screen, render targets and 3D view once at
   start.
4. **Linux has been tested under WSL2 only** (Debian 13, Mesa's software renderer, SDL's offscreen video driver):
   `--selftest`, the main menu, and a race through the pause menu back to the menus. A Linux desktop with a GPU
   driver, Wayland and X11 windows, gamepads, sound devices, the online leaderboard over libcurl and multiplayer
   between two Linux PCs are untested.
5. Quirks of the original game, kept as they are, are listed in
   [`FIXES.md`](FIXES.md#known-quirks-of-the-original-game).

### Reporting a bug

Open an issue on the [**Issues**](https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/issues) page with:

- the Windows version or the Linux distribution, graphics card and driver;
- the port's version, and the release archive or a build from source;
- what happened and the steps that lead to it;
- `gtracing.log`, attached as a file, and `gtracing.dmp` (Windows) or `gtracing.crash` (Linux) after a crash if the
  [Privacy](#privacy) section suits you.

## Privacy

- **Logs stay local and scrubbed.** `gtracing.log` stays on your computer until you send it. Folder names, the user
  name, the PC's name and the public IP address are written as tags (`%USERPROFILE%` or `~`, `%EXE%`, `%APP%`,
  `%HOME%`, `%USER%`, `%PC%`, `%IP%`) or blanked (`host/core/scrub.c`). `GT_LOG=0` writes no log.
- **Crash files are scrubbed the same way.** The Windows minidump (`gtracing.dmp`) holds thread stacks, registers and
  the module list without paths, with every copy of those folders and of each environment value overwritten. The Linux
  report (`gtracing.crash`) is text: the signal, host and guest registers, host stack addresses and loaded libraries.
  `GT_CRASH_DUMP=0` writes neither.
- **The game sees made-up identifiers:** no environment variables, the host name `iPad`, a fixed device identifier and
  the GameKit name `GT Racing PC`.
- **Network traffic** is multiplayer (the local network; Online also asks the router through UPnP or NAT-PMP) and the
  leaderboard server, which receives a random player number, the player name, the car and the lap time, with the user
  agent `GTRacingPC-board/1`. `GT_CLOUD_BOARD=0` keeps the board offline.
- **No build-machine paths.** Every compile maps the project folder to `.` (`-ffile-prefix-map`), and the release build
  reads no diagnostic keys and holds no trace strings. `tools/privacy_scan.py` checks programs, logs, dumps, release
  folders and archives for the user name, the PC name, the Git identity, the user folder and the project folder.

## Build Instructions (For Developers)

The Windows exe is built on Windows. The Linux program is built on Linux, or on Windows through WSL. You need:

- `GTRacingFreemiumHD.app` (iPad, 1.3.4b) in the repository root. It is read, never modified;
- Python 3.12 or newer, with `capstone` 5 for stage 1 (and `unicorn` 2.1.3 for the tests);
- Windows: clang from [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) on `PATH`, for the generated game code, and
  MSYS2's mingw32 gcc in `C:\msys64\mingw32`, for the host and the link;
- Linux (Debian 13): `gcc-multilib`, `libsdl3-dev:i386` and `libgl-dev:i386` (after `dpkg --add-architecture i386`).
  gcc compiles both the generated code and the host.

```
cd GTRacingMotorAcademy-PC-Port

# stage 1, once per binary: every function of the game, into analysis\stage1
python -I recomp\discover.py GTRacingFreemiumHD.app\GTRacingFreemiumHD analysis\stage1

# the generated C, into build\gen, compiled with clang (later builds can skip it with --no-gencc)
python -I host\build.py --gen

# build\host\release\gtracing.exe, with SDL3.dll next to it
python -I host\build.py --release --no-gencc

build\host\release\gtracing.exe --app "%CD%\GTRacingFreemiumHD.app"
```

On Linux, after stage 1 and `--gen` (run there or on Windows, the generated C is the same):

```
python3 -I host/build.py --platform linux --release
build/host/linux/release/gtracing --app "$PWD/GTRacingFreemiumHD.app"
```

| Build | Command | Output | `GT_DEV` |
|---|---|---|---|
| Development | `python -I host\build.py` | `build\host\gtracing.exe` | `1`: test switches, traces and diagnostic logs compiled in |
| Release | `python -I host\build.py --release` | `build\host\release\gtracing.exe` | `0`: diagnostic keys not read (`dev_getenv` is `NULL`), diagnostic log lines compiled out |
| Linux development | `python3 -I host/build.py --platform linux` | `build/host/linux/gtracing` | `1` |
| Linux release | `python3 -I host/build.py --platform linux --release` | `build/host/linux/release/gtracing` | `0` |

`--platform` takes `windows`, `linux` or `all`; without it the build is for the system it runs on. On Windows,
`--platform linux` and `--platform all` run the Linux build in WSL (`wsl.exe`, its default distribution).

The Windows build also patches the game's menu SWFs into `build\host\patch\` (the PC menus), makes the icon from the
game's artwork and copies `SDL3.dll` next to the exe; the Linux build writes the same patched menus to
`build/host/linux/patch/`. All four builds compile with `-Wall -Wextra` and no warnings. The Linux build adds
`-malign-double`, so that 64-bit fields keep the alignment the Windows build and the game's ARM code give them, and
links with `host/core/gt_linux.ld`, which places the game's range at its own addresses as `gt.ld` does for the exe.

### How it works

1. **Discovery** (`recomp/discover.py`): 23,873 functions (14,348 ARM, 9,525 Thumb, 1.8 million instructions) with
   their blocks, jump tables and literal pools, and no unresolved indirect jump.
2. **C generation** (`recomp/armc.py`, `recomp/codegen.py`): one C function `F_<addr>(CPU *c)` per guest function,
   with the iOS ARM ABI, VFP and NEON per element, and each instruction form tested against Unicorn (Cortex-A8).
3. **Runtime** (`host/core/`, `runtime/`): `gt.ld` (exe) and `gt_linux.ld` (ELF) reserve the game's range from
   0x11000, so the image loads at its own addresses. Imports bind to ARM-ABI bridges generated from `imports.sig`.
4. **iOS libraries** (`host/shims/`): Objective-C, libc, libstdc++, Foundation, UIKit, OpenAL, GameKit and the rest,
   with OpenGL ES on desktop OpenGL and SDL3 for the window, input and audio.
5. **The port** (`host/games/gtracing/`): features and fixes, hooked through GNU ld `--wrap`, single instructions
   replaced at generation time (`HOST_INSNS`) and SWF patches at build time (`tools/swfpatch_*.py`).

The ABI and runtime details are in [`FIXES.md`](FIXES.md#abi-and-runtime), and where new code goes in
[`CONTRIBUTING.md`](CONTRIBUTING.md#where-code-goes).

### Repository layout

| Path | What |
|---|---|
| `recomp/` | Mach-O reader, function discovery, disassembly, the ARM/Thumb/VFP/NEON-to-C translator, parallel compile, import bridges |
| `runtime/` | `arm_rt.h` (CPU state and memory macros for the generated C), `rt_jump.c`, `rt_selftest.c` |
| `host/build.py` | builds the development or release program, for Windows, Linux or both |
| `host/core/` | loader, threads, guest calls, traps, log, settings, crash dumps, `--selftest`, `gt.ld`, `gt_linux.ld`, `imports.sig`; `compat.h`, `compat_net.h` and `*_linux.c`: the Linux platform layer |
| `host/shims/` | the iOS libraries; `gl/` holds OpenGL ES and EAGL. They reach the game only through `game_hooks.h` |
| `host/games/gtracing/` | `layout.h`, `game.h`, `features/` (port additions, PC input), `render/` (picture additions) |
| `host/third_party/SDL3/` | SDL 3 for i686 |
| `tests/` | `difftest.py`, `functest.py`: instructions and whole functions against Unicorn |
| `tools/` | SWF patches, add-on builders, icon, release packaging, privacy scan, layer check, playtest, debugging helpers |
| `cloud/redbull-board/` | the leaderboard server (Cloudflare Worker + D1), see its [README](cloud/redbull-board/README.md) |

`.gitignore` keeps out the game, anything taken from it and the build output.

### Tests

- `--selftest`: 124 host checks without the game (runtime, fast paths against plain ones, leaderboard merge, settings
  parsing, input, guest `setjmp` / `longjmp`, C++ and Objective-C exceptions).
- `tools/layer_check.py`: the shims reach the game only through `game_hooks.h`, on both platforms.
- `tools/playtest.ps1`: scripted runs on a copy of a save (menus, races, Garage, replay), then the privacy scan.
- `tests/difftest.py` and `tests/functest.py`: single instructions and whole functions against Unicorn.

The commands, and the checks a pull request must pass, are in
[`CONTRIBUTING.md`](CONTRIBUTING.md#mandatory-checks).

### Release package

- Windows: `python -I tools\package_release.py --version 1.0.1 --zip` makes
  `build\release\GTRacing-PC-Port-1.0.1-Windows-x86\` and its zip.
- Linux: `python3 -I tools/package_release.py --platform linux --version 1.0.1 --archive` makes
  `GTRacing-PC-Port-1.0.1-Linux-x86/` and its `.tar.gz` (`README-linux.txt` as `README.txt`, LF line ends, mode 755,
  no owner names). On Windows it needs `--no-run`, since the stripped program's self-test runs on Linux.
- Both strip the release program and check that its loaded sections (and, for the ELF, segments) are unchanged.
- Both copy only the program, `SDL3.dll` (Windows), `gtracing.ini`, `README.txt` and `IMPORTANT.TXT`, plus the empty
  `GTRacingFreemiumHD.app`, `patch` and `add-on` folders with their notes.
- Both run the privacy scan and `--selftest` on the stripped program, and check the archive against the folder.

### Add-ons

Add-ons are folders in `add-on\`, read as if their files were in the game's bundle. Each is built from an edition of
the game you own; none is part of this repository.

| Tool | Builds |
|---|---|
| `tools/hyundai_addon.py` | `add-on\hyundai`: Hyundai Genesis Coupe and i-oniq, from GT Racing: Hyundai Edition (Android OBB) |
| `tools/android_free_addon.py` | `add-on\dodge`, `add-on\ferrari`: Dodge Charger 2012 and Ferrari 288 GTO, from Android GT Racing Free+ 1.4.3 |
| `tools/premium_addon.py` | `add-on\premium-licences`, `add-on\premium-audio`: the paid iPad edition's three extra licence tests and its 44 kHz audio |

Run each with `python -I tools\<tool>.py` for its arguments.

## Contributing

Tests on untried hardware help most: Intel graphics, an AMD graphics card, an older PC, and multiplayer between two
PCs. Report results on the [Issues](https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/issues) page with
the log. For code, read [`CONTRIBUTING.md`](CONTRIBUTING.md).

## Credits

- **[Adriansyah101](https://github.com/Adriansyah101)**, for the port.
- **Gameloft**, for the game.
- **[SDL](https://www.libsdl.org/)**, for the window, input, sound and gamepads.
- **[llvm-mingw](https://github.com/mstorsjo/llvm-mingw)**, for the clang that compiles the generated game code.
- **[MSYS2](https://www.msys2.org/)** and **[mingw-w64](https://www.mingw-w64.org/)**, for the Windows toolchain.
- **[Capstone](https://www.capstone-engine.org/)**, for function discovery, and
  **[Unicorn](https://www.unicorn-engine.org/)**, for the instruction tests.
- **[Cloudflare Workers](https://workers.cloudflare.com/)** and D1, for the leaderboard server.

## License

The code in this repository is dual-licensed under the [MIT License](LICENSE-MIT) or the
[GNU General Public License, version 3](LICENSE-GPL-3.0), at your choice (SPDX: `MIT OR GPL-3.0-only`).

SDL 3 in `host/third_party/SDL3` is under the zlib license ([LICENSE.txt](host/third_party/SDL3/LICENSE.txt)). The
licenses cover this project's code only, not the game, its files or its trademarks.
