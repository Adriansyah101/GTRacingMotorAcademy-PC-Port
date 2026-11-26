# Contributing

Tests on untried hardware help as much as code: see [Contributing](README.md#contributing) in the README. This page
covers changes to the source.

## Ground rules

- **Nothing from the game goes in.** No game files, art, sound, menus, text, dumps or anything derived from them,
  including patched SWFs, extracted textures and add-on packs. `.gitignore` keeps out `*.app/`, `analysis/`, `build/`
  and the other usual places; check your commit as well.
- **The iPad game is the reference.** A fix for a port fault is on with no option, or behind a switch kept for
  comparison. A fix for an original-game fault and every enhancement get their own `GT_*` key, on by default, with
  `0` giving the game's own behaviour.
- **Native UI only.** In-game settings go on the game's own menus (SWF patches in `tools/swfpatch_*.py`), not in an
  overlay.
- **Release builds stay clean.** Diagnostics, traces and test switches go behind `GT_DEV`, read through `dev_getenv`.
  A release exe must hold no path or name from the machine that built it, and the log must not name the player's
  folders.

## Where code goes

| Folder | What belongs there |
|---|---|
| `recomp/` | the translator and generators. A change here changes `build/gen/`: rebuild with `--gen` |
| `runtime/` | the support code the generated C includes |
| `host/core/` | the loader and runtime. It knows no game feature |
| `host/shims/` | the iOS libraries. They reach the game only through `game_hooks.h` |
| `host/games/gtracing/` | everything about this game: `features/` and `render/` |
| `tools/` | build-time patches, add-on builders, packaging and checks |

Two folders must not hold the same file name: object files are named after their source, and `host/build.py` stops
if two collide.

### Windows and Linux

The Windows code is the reference; the Linux build adds to it without changing it.

- Shared files include `compat.h`, never `windows.h`. On Windows it includes `windows.h` and nothing else; on Linux it
  gives the Win32 names the host uses (locks, threads, events, timers, files, memory) over POSIX, in
  `host/core/compat_linux.c`. `compat_net.h` does the same for Winsock.
- Code that differs goes in an `#ifdef _WIN32` / `#else` pair, or in a file named `<name>_linux.c`, which the Linux
  build compiles in place of `<name>.c`. The Windows build never compiles a `*_linux.c` file.
- Paths are built with `GT_SEP` and `GT_SEPC` (`rt.h`), not a literal backslash.
- A change for Linux only must leave the Windows exe's `.text`, `.data`, `.rdata`, `.idata`, `.rsrc` and `.tls`
  sections byte for byte as they were. Compare them before and after (the `PE` class in `tools/package_release.py`
  reads them).

## Building

As in [Build Instructions](README.md#build-instructions-for-developers):

```
python -I host\build.py --gen
python -I host\build.py --no-gencc
python -I host\build.py --release --no-gencc
python -I host\build.py --platform linux
python -I host\build.py --platform linux --release
```

On Windows the last two run in WSL. On Linux, run `python3 -I host/build.py --platform linux` there.

## Mandatory checks

Run all of these before a pull request, on all four builds (Windows and Linux, development and release).

1. **Zero warnings.** Every build compiles with `-Wall -Wextra`, the generated code included. Any warning fails the
   change.
2. **Layer check.** `python -I tools\layer_check.py`, the same with `--release`, with `--platform linux` and with
   `--platform linux --release` must print `layer check: clean`.
3. **Self-test.** `--selftest` must report `0 of 124 checks failed` (or 0 of the new total) in the log. Run it with a
   scratch save, log and settings file, never your own:

   ```
   set GT_HOME=%TEMP%\gt-test\home
   set GT_LOG=%TEMP%\gt-test\gtracing.log
   set GT_INI=%TEMP%\gt-test\test.ini
   set GT_CLOUD_BOARD=0
   build\host\gtracing.exe --selftest --app "%CD%\GTRacingFreemiumHD.app"
   build\host\release\gtracing.exe --selftest --app "%CD%\GTRacingFreemiumHD.app"
   ```

   On Linux, with a scratch folder `$T` that holds a copy of a save in `$T/home`:

   ```
   GT_HOME=$T/home GT_LOG=$T/gtracing.log GT_INI=$T/test.ini GT_CLOUD_BOARD=0 \
       build/host/linux/release/gtracing --selftest --app "$PWD/GTRacingFreemiumHD.app"
   ```

4. **Privacy scan.** `python -I tools\privacy_scan.py build\host\gtracing.exe build\host\release\gtracing.exe
   build\host\linux\gtracing build\host\linux\release\gtracing %TEMP%\gt-test\gtracing.log` and
   `python -I tools\privacy_scan.py --release` must print `none found`. Scan any new document and each self-test log
   too.
5. **Playtest**, for changes the game can show: `tools\playtest.ps1` runs scripted scenarios (main menu, Display page,
   races with PostFX switched live, Garage with add-on cars, Test Center and replay) on a copy of a save in `%TEMP%`.
   Look at its screenshots, not only at PASS. It drives the Windows exe; on Linux run the same test switches
   (`GT_TAPS`, `GT_KEYNAV`, `GT_PEDALS`, `GT_QUIT_AT`, `GT_SHOTS`) by hand. `SDL_VIDEO_DRIVER=offscreen` runs the
   game without a window.
6. **Generated code**, for changes in `recomp/`: instructions and whole functions against Unicorn.

   ```
   python -I tests\difftest.py GTRacingFreemiumHD.app\GTRacingFreemiumHD analysis\stage1\functions.json build\difftest
   python -I tests\functest.py GTRacingFreemiumHD.app\GTRacingFreemiumHD analysis\stage1\functions.json build\functest
   ```

## Code style

- **C:** GNU C11 (`-std=gnu11`), the style of the surrounding file, `-Wall -Wextra` clean with MinGW's gcc and
  Linux's gcc. Name addresses as `F_<addr>` or by a constant in `game.h`; do not repeat a magic number in two files.
- **Python:** the standard library only (plus `capstone` / `unicorn` where already used), run with `python -I`.
- **No comments.** The source carries no narrative, explanatory or AI-generated comments and no docstrings. The only
  exceptions are:
  - `/* fall through */`, which `-Wimplicit-fallthrough` reads;
  - `# noqa` and other tool pragmas;
  - licence notices (the FSF notice in `gt.ld`);
  - the module docstrings of `tools/make_icon.py` and `tools/privacy_scan.py`, printed as their usage text.

  The reasons for a change go in its pull request and in `FIXES.md`.
- **`host/core/selftest.c` keeps its line numbers.** `CHECK` prints `__LINE__`, and reports refer to those lines. Add
  new checks at the end of a block or replace a removed line with a blank one; do not shift existing checks.
- **Settings:** read numbers through `rt_setting_int` and its kin, with a default and a range, so malformed values
  take the default and out-of-range values are clamped.

## Documenting a change

- [`FIXES.md`](FIXES.md): the fault, whose it is (port or original game), the addresses involved and the fix.
- [`CHANGELOG.md`](CHANGELOG.md): one entry under `Unreleased`, in `Added`, `Changed`, `Fixed` or `Removed`.
- [`PATCHNOTES.md`](PATCHNOTES.md): one line for players, at release time.
- [`README.md`](README.md): a new key in the configuration tables.

## Pull requests

- One change per pull request where possible.
- Say what it does, why, and how it was tested: which build, which checks, which scenarios, and what you looked at in
  the game.
- Include the result lines of the checks above (warnings, `layer check`, `checks failed`, `privacy scan`).
- Out of scope: game files or anything derived from them, network services beyond multiplayer and the leaderboard,
  overlays, and changes to the save format without a migration.
