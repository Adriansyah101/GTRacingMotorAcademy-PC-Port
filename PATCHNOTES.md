# Patch Notes

Release notes for GTRacingMotorAcademy-PC-Port, the Windows and Linux port of GT Racing: Motor Academy Free+ HD
1.3.4b. A name in `code` style is a key in `gtracing.ini`. The technical changes are in [`CHANGELOG.md`](CHANGELOG.md).

## v1.0.1

**Download:** from [Releases](https://github.com/Adriansyah101/GTRacingMotorAcademy-PC-Port/releases/tag/v1.0.1).

| File | SHA-256 |
|---|---|
| `GTRacing-PC-Port-1.0.1-Windows-x86.zip` | `0d36e9337d0acce23eb618b9f8d62fcd17f8fe224fe1518cfd9320a5d8f8cf2e` |
| `GTRacing-PC-Port-1.0.1-Linux-x86.tar.gz` | `6af7bf22f3dd9223e8774cdaa2f83f20b30ecdf7b00b9289ef7d925b815ffe79` |

Check them with `certutil -hashfile GTRacing-PC-Port-1.0.1-Windows-x86.zip SHA256` on Windows and
`sha256sum GTRacing-PC-Port-1.0.1-Linux-x86.tar.gz` on Linux. `SHA256SUMS.txt` inside each archive lists each file
in it.

### Linux

- **New: a Linux version** for x86 PCs (32-bit program, 64-bit systems with the 32-bit libraries). It is the same
  game and the same port as on Windows: every setting, menu, add-on, multiplayer mode and the leaderboard.
- Needs the 32-bit SDL3 and OpenGL: on Debian 13 or Ubuntu 25.04 and newer, `libsdl3-0:i386`, `libgl1:i386` and
  `libgl1-mesa-dri:i386`; `libcurl4t64:i386` for the online leaderboard. The tar.gz's `README.txt` has the commands.
- The game's files can keep any mix of upper and lower case in their names.
- Saves and `gtracing.ini` from the Windows version work as they are.
- Crash reports are text files (`gtracing.crash`); garage screenshots go to `~/Pictures/GT Racing`.
- Tested under WSL2 (Debian 13) only; see Known Issues in the README.

### Rendering

- **Sharper car reflections on the Scandinavia tracks** (`GT_SOFT_REFLECTION`): the streaks and swirls in the car
  windows are softened, and so are the sky and far mountains in the reflection. The track reflected on the car's body
  is now as sharp as in the original game; 1.0.0 softened all of it. Other tracks are unchanged.
  `GT_SOFT_REFLECTION=0` gives the original reflection.

### Repository

- The repository is now **GTRacingMotorAcademy-PC-Port** (it was GTRacingMotorAcademyRecomp). Old links redirect.

Nothing else in the game changed. Saves, `gtracing.ini`, `patch\` and `add-on\` from 1.0.0 work as they are.

### Upgrading from v1.0.0

1. Extract `GTRacing-PC-Port-1.0.1-Windows-x86.zip` to a new folder.
2. Copy the contents of your `GTRacingFreemiumHD.app` into its empty `GTRacingFreemiumHD.app` folder, or move the
   folder over from 1.0.0.
3. Copy `home\` (your save), `gtracing.ini`, and `patch\` and `add-on\` if you filled them, from the 1.0.0 folder.
4. Start `gtracing.exe`. Delete the 1.0.0 folder once the new one runs.

On Linux, follow the tar.gz's `README.txt`. To move a Windows save over, copy `home\`, `gtracing.ini`, `patch\` and
`add-on\` from the Windows folder into the Linux one.

## v1.0.0

The first release: the iPad game on Windows (x86, 32-bit) by static recompilation, with no game files included.

- Any resolution, 16:10, 16:9 and 21:9 with the menus and HUD kept in shape, full screen or window (Alt+Enter), MSAA,
  anisotropic filtering, full motion blur and PostFX (Classic, Classic Natural, Enhanced).
- Keyboard, mouse and gamepads (Xbox and PlayStation layouts), remappable keys, a pad pointer that snaps to menu
  buttons, look back and a 360-degree look around the car, replay zoom and pad control.
- An in-game Display page, an Exit tile and a Name tile, with the PC menus in the optional `patch\` folder.
- Local and Bluetooth play over a LAN or VPN, Online play through room codes, and a Red Bull leaderboard kept locally
  and shared through a server (`GT_CLOUD_BOARD=0` keeps it offline).
- A coin-for-cash shop, full prizes for repeated results, a car service paid in coins and an offline Top 10 Cars list.
- Add-on cars and the paid edition's licence tests and audio, loaded from `add-on\` when you build them from editions
  you own.
- Fixes for port faults (driver crashes on deleted buffers, full-screen pop-ups, a one-pixel column, a slow start-up,
  the garage Screenshot button, a new profile's name) and original-game faults (dimmed replay, licence medal count,
  off-centre menu 3D pictures).
- Checked settings: a malformed or out-of-range value in `gtracing.ini` takes its default or the nearest allowed
  value.
