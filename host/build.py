"""Build the GT Racing host (Windows, 32-bit): build/host/gtracing.exe.

Usage: python -I host/build.py [--release] [--gen] [--no-gencc]
  --release   the release build: GT_DEV=0 (rt.h), build/host/release/gtracing.exe (objects in build/host/obj_release).
              Without it, the development build: GT_DEV=1, build/host/gtracing.exe (objects in build/host/obj), with
              the diagnostic keys and traces the tests use (tools/playtest.ps1).  tools/package_release.py takes the
              release build.
  --gen       run recomp/codegen.py first (build/gen)
  --no-gencc  do not recompile build/gen (objects already in build/host/obj_gen)

Generated game code: clang (llvm-mingw) for i686, like recomp/compile_all.py.  Host runtime:
MSYS2's mingw32 gcc (as in the Fast Five host), which also links, with host/core/gt.ld so the
exe's first section reserves the game's address range (layout.h).

The host's sources, split by kind as in the Fast Five and Gangstar hosts:
  core/                     loading the game image, the runtime, calls between host and game, gt.ld, imports.sig
  shims/                    the iOS libraries the game imports (libc, C++, Objective-C, Foundation, UIKit, OpenAL,
                            GameKit, the network), shims/gl/ its OpenGL ES and EAGL
  games/gtracing/           layout.h (the game's address layout), features/ (port additions and fixes) and
                            render/ (picture additions)
  third_party/              SDL 3
A file includes another by its bare name.  core/ and shims/ have only their own folders and games/gtracing/
(layout.h) on the include path, the game layer every folder: the shims reach the game only through
shims/game_hooks.h (LOWER_INCLUDES below).
"""
import glob
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'GTRacingFreemiumHD.app', 'GTRacingFreemiumHD')
GEN = os.path.join(ROOT, 'build', 'gen')
OUT = os.path.join(ROOT, 'build', 'host')
MSYS = r'C:\msys64\mingw32\bin'
GCC = os.path.join(MSYS, 'gcc.exe')
LOW_DELTA = '0x8bf000u'          # layout.h GT_LOW_DELTA
SDL = os.path.join(HERE, 'third_party', 'SDL3')     # SDL 3 for i686 (from the Fast Five host)
SRC_DIRS = [os.path.join(HERE, *d.split('/')) for d in
            ('core', 'shims', 'shims/gl', 'games/gtracing', 'games/gtracing/features', 'games/gtracing/render')]
CORE = SRC_DIRS[0]
INCLUDES = [a for d in SRC_DIRS for a in ('-I', d)]
# The layer boundary, kept by the include path: core/ and shims/ see their own folders and games/gtracing/ (layout.h,
# the address-space layout) only, so a shim cannot include a game feature's header (features.h, ...).  Their calls
# into the game layer are the ones declared in shims/game_hooks.h; shader patches are registered (gl.h).
LOWER_DIRS = SRC_DIRS[:4]
LOWER_INCLUDES = [a for d in LOWER_DIRS for a in ('-I', d)]


def includes_for(src):
    d = os.path.dirname(os.path.abspath(src))
    return LOWER_INCLUDES if d in LOWER_DIRS or d.startswith(OUT) else INCLUDES

# -ffile-prefix-map (privacy), on every compile of every build (host C, assembly, runtime/, the generated code):
# -g and __FILE__ would put each source's full path in the exe, so the Windows user's folder and name.  The
# project's folder is written as "." and the compilers run from it (run() below), so the debug info's working folder
# is "." too and the exe names no folder outside the project.  tools/privacy_scan.py checks the result.
PREFIX_MAP = '-ffile-prefix-map=%s=.' % ROOT
GCC_FLAGS = ['-O2', '-g', '-c', '-std=gnu11', '-m32', '-msse2', '-mfpmath=sse', '-fno-strict-aliasing',
             '-fwrapv', '-Wall', '-Wextra', '-D__USE_MINGW_ANSI_STDIO=1',
             PREFIX_MAP, '-DARM_LOW_DELTA=' + LOW_DELTA] + [
             '-I', os.path.join(ROOT, 'runtime'), '-I', os.path.join(OUT, 'gen'),
             '-I', os.path.join(SDL, 'include')]


def sources(pattern):
    """The host's files matching pattern in every source folder (object names are the bare file names, so
    two folders must not hold the same name)."""
    found = sorted(f for d in SRC_DIRS for f in glob.glob(os.path.join(d, pattern)))
    names = [os.path.basename(f) for f in found]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        print('the same file name in two host folders: %s' % ', '.join(dup))
        sys.exit(1)
    return found
CLANG_FLAGS = ['--target=i686-w64-mingw32', '-msse2', '-mfpmath=sse', '-O2', '-c', '-fno-strict-aliasing',
               '-ffp-contract=off', '-fwrapv', '-DARM_LOW_DELTA=' + LOW_DELTA, '-I', os.path.join(ROOT, 'runtime'),
               PREFIX_MAP]


def run(cmd, env=None):
    # from the project's folder: every path here is whole, and the debug info's working folder is then the one
    # -ffile-prefix-map writes as "."
    p = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=ROOT)
    return p.returncode, (p.stdout + p.stderr).strip()


def msys_env():
    env = dict(os.environ)
    env['PATH'] = MSYS + os.pathsep + env['PATH']
    return env


def newer(src, obj, deps=()):
    if not os.path.exists(obj):
        return True
    t = os.path.getmtime(obj)
    return any(os.path.getmtime(x) > t for x in [src] + list(deps))


def main():
    args = sys.argv[1:]
    t0 = time.time()
    release = '--release' in args
    variant = os.path.join(OUT, 'release') if release else OUT     # the exe's folder
    os.makedirs(variant, exist_ok=True)
    dev_flag = '-DGT_DEV=%d' % (0 if release else 1)
    os.makedirs(os.path.join(OUT, 'gen'), exist_ok=True)
    py = sys.executable
    if '--gen' in args:
        rc, out = run([py, '-I', os.path.join(ROOT, 'recomp', 'codegen.py'), EXE,
                       os.path.join(ROOT, 'analysis', 'stage1', 'functions.json'), GEN])
        print(out)
        if rc:
            sys.exit(rc)
    rc, out = run([py, '-I', os.path.join(ROOT, 'recomp', 'bridgegen.py'), EXE,
                   os.path.join(CORE, 'imports.sig'), os.path.join(OUT, 'gen')])
    print(out)
    if rc:
        sys.exit(rc)
    objgen = os.path.join(OUT, 'obj_gen')
    if '--no-gencc' not in args:
        rc, out = run([py, '-I', os.path.join(ROOT, 'recomp', 'compile_all.py'), GEN, objgen, 'i686',
                       '-DARM_LOW_DELTA=' + LOW_DELTA, PREFIX_MAP])
        print(out)
        if rc:
            sys.exit(rc)
    objhost = os.path.join(OUT, 'obj_release' if release else 'obj')
    os.makedirs(objhost, exist_ok=True)
    env = msys_env()
    headers = sources('*.h') + [os.path.join(ROOT, 'runtime', 'arm_rt.h')]
    jobs = []
    for src in sources('*.c') + [os.path.join(OUT, 'gen', 'bridges.c')]:
        obj = os.path.join(objhost, os.path.basename(src)[:-2] + '.o')
        if newer(src, obj, headers):
            jobs.append(([GCC] + GCC_FLAGS + includes_for(src) + [dev_flag, src, '-o', obj], env, src))
    for src in sources('*.S'):
        obj = os.path.join(objhost, os.path.basename(src)[:-2] + '.o')
        if newer(src, obj, headers):
            jobs.append(([GCC, '-c', '-m32', PREFIX_MAP] + includes_for(src) + [src, '-o', obj], env, src))
    # runtime/: built with clang like build/gen, so their __builtin_setjmp / __builtin_longjmp match its buffers
    # (rt_jump.c: the runtime's longjmp; rt_selftest.c: guest-style functions for --selftest)
    for name in ('rt_jump', 'rt_selftest'):
        src = os.path.join(ROOT, 'runtime', name + '.c')
        obj = os.path.join(objhost, name + '.o')
        if newer(src, obj, headers):
            jobs.append((['clang'] + CLANG_FLAGS + [src, '-o', obj], None, src))
    # an object whose source is gone (a renamed or removed file) would still be linked: dropped
    made = {os.path.basename(s)[:-2] + '.o' for s in sources('*.c') + sources('*.S')}
    made |= {'bridges.o', 'rt_jump.o', 'rt_selftest.o', 'app_res.o'}
    for obj in glob.glob(os.path.join(objhost, '*.o')):
        if os.path.basename(obj) not in made:
            os.remove(obj)
    bad = 0
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for (cmd, e, src), (rc, out) in zip(jobs, ex.map(lambda j: run(j[0], j[1]), jobs)):
            if rc or out:
                print('%s:\n%s' % (os.path.basename(src), out[:4000]))
            bad += rc != 0
    if bad:
        print('%d host files failed' % bad)
        sys.exit(1)
    # APP_ICON (tools/make_icon.py): the exe's icon (icon 1, which Explorer, the taskbar and SDL's window take)
    # from the iTunesArtwork, made here so Gameloft's picture is never in the repository: the IPA's one in the
    # project folder (the Red Bull F1 car, as the home-screen icon) or else the bundle's older Camaro one
    art, radius = os.path.join(ROOT, 'iTunesArtwork'), '0.16'
    if not os.path.exists(art):
        art, radius = os.path.join(ROOT, 'GTRacingFreemiumHD.app', 'iTunesArtwork'), '0.203'
    ico = os.path.join(OUT, 'app.ico')
    res = os.path.join(objhost, 'app_res.o')
    icon_tool = os.path.join(ROOT, 'tools', 'make_icon.py')
    stamp = os.path.join(OUT, 'app_ico.src')        # which artwork the icon was made from (an old file's date
    src_was = open(stamp).read() if os.path.exists(stamp) else ''   # alone would not make it again)
    if os.path.exists(art) and (newer(art, res, [icon_tool]) or src_was != art):
        rc, out = run([py, '-I', icon_tool, art, ico, radius])
        print(out)
        if rc:
            sys.exit(rc)
        rcfile = os.path.join(OUT, 'app.rc')
        with open(rcfile, 'w') as fp:
            fp.write('1 ICON "%s"\n' % ico.replace('\\', '/'))
        rc, out = run([os.path.join(MSYS, 'windres.exe'), '--target=pe-i386', rcfile, '-o', res], env)
        if rc:
            print('windres:\n%s' % out)
            sys.exit(rc)
        open(stamp, 'w').write(art)
    gen_objs = [o for o in sorted(glob.glob(os.path.join(objgen, '*.o')))
                if os.path.basename(o) not in ('imports_stub.o', 'imports.o')]
    host_objs = sorted(glob.glob(os.path.join(objhost, '*.o')))
    exe = os.path.join(variant, 'gtracing.exe')
    rsp = os.path.join(variant, 'link.rsp')
    with open(rsp, 'w') as fp:
        fp.write('\n'.join('"%s"' % o.replace('\\', '/') for o in host_objs + gen_objs))
    link = [GCC, '-m32', '-mconsole', '-static', '-O2', '-T', os.path.join(CORE, 'gt.ld'),
            '-Wl,--image-base,0x10000', '-Wl,--disable-dynamicbase', '-Wl,--disable-reloc-section',
            '-Wl,--large-address-aware', '-Wl,--nxcompat', '-Wl,--stack,0x4000000',
            # service.c (CAR_SERVICE): the car service popups, called from other generated files only
            '-Wl,--wrap=F_2d8c1c', '-Wl,--wrap=F_2d9720', '-Wl,--wrap=F_2d90c0',
            # shop.c (COIN_SHOP): CStoreFacade requestProduct / buy, ShopSubPackMenu enter (dispatch only)
            '-Wl,--wrap=F_2ef358', '-Wl,--wrap=F_2ef104', '-Wl,--wrap=F_1d69d0', '-Wl,--wrap=F_2f6ef8',
            '-Wl,--wrap=F_2ef6f0', '-Wl,--wrap=F_1ff744', '-Wl,--wrap=F_1feaec',
            # prize.c (PRIZE_MULT): the coin / XP adders and number writers, picked by the caller's lr
            '-Wl,--wrap=F_268704', '-Wl,--wrap=F_268e70', '-Wl,--wrap=F_2716c4',
            '-Wl,--wrap=F_13c82c', '-Wl,--wrap=F_13de08',
            # fullblur.c (FULL_BLUR): the iPad 3's blur shader name / render target size
            '-Wl,--wrap=F_34a85c', '-Wl,--wrap=F_2a4f38',
            # softrefl.c (SOFT_REFLECTION): the race frame's reflection map (called from code_011.c only)
            '-Wl,--wrap=F_2a4ba4',
            # postfx.c (POSTFX=2): the empty F_2a36e8 the race frame calls before the HUD (from code_011.c only)
            '-Wl,--wrap=F_2a36e8',
            # postfx.c (POSTFX=2/3/4 behind the menus' 3D scene): F_14a660 after it (from code_019.c; lr picks the call)
            '-Wl,--wrap=F_14a660',
            # licences.c (PREMIUM_LICENCES): class C's medal places when the add-on gives the paid order
            '-Wl,--wrap=F_2659b0', '-Wl,--wrap=F_26a634', '-Wl,--wrap=F_26a980', '-Wl,--wrap=F_265920',
            # widescreen.c (WIDE_UI): gameswf's begin_display and is_visible (dispatch only), the display
            # list's leaves and the display callback (called from other generated files)
            '-Wl,--wrap=F_517608', '-Wl,--wrap=F_515980', '-Wl,--wrap=F_4eb4ac', '-Wl,--wrap=F_500b58',
            '-Wl,--wrap=F_4ca684',
            # p2p.c (ONLINE_P2P): GameTypeMenu's handler (dispatch only), the Online tile takes Local's path
            '-Wl,--wrap=F_1bb6c0',
            # topcars.c (TOP_CARS): the Top Ten Cars request (GS_LoadMainMenu, the main menu's retry)
            '-Wl,--wrap=F_12bc00',
            # board.c (RED_BULL_BOARD): the online menus' "Connection Lost" popup (not for the Red Bull leaderboard),
            # the race end screen's Leaderboard button (its menu after the race)
            '-Wl,--wrap=F_212170', '-Wl,--wrap=F_376e68',
            # missingcars.c (MISSING_CARS): the car table's id -> row and field reads (the loaders' and the
            # writer's sites, in other generated files), the car file's loader, the profile's load and writer
            '-Wl,--wrap=F_2b5fe8', '-Wl,--wrap=F_2b689c', '-Wl,--wrap=F_2b7e78', '-Wl,--wrap=F_270ac0',
            '-Wl,--wrap=F_266820',
            # liveblocks.c (LIVE_BLOCKS, pass 6): the C library's heap calls of every object, the large blocks kept
            # from their allocation to their free (gles.c's readable() asks Windows less)
            '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free',
            '-Wl,-Map=' + os.path.join(variant, 'gtracing.map'), '@' + rsp, '-o', exe,
            os.path.join(SDL, 'lib', 'libSDL3.dll.a'),
            '-lws2_32', '-liphlpapi', '-lwinmm', '-lgdi32', '-lopengl32', '-ladvapi32', '-lole32',
            '-lgdiplus', '-lshell32',   # cg.c: the garage's screenshot as a PNG in Pictures
            '-lwinhttp']                # board.c CLOUD_BOARD: Windows' own HTTP (no DLL in the release folder)
    rc, out = run(link, env)
    if out:
        print(out[:6000])
    if rc:
        print('link failed')
        sys.exit(1)
    dll = os.path.join(SDL, 'bin', 'SDL3.dll')
    if newer(dll, os.path.join(variant, 'SDL3.dll')):
        shutil.copy2(dll, variant)
    # PC_CONTROLS (pcctl.c): the menu SWFs (main menus and in-race menus, every device variant) with the
    # Controls menu's extra rows, read instead of the bundle's
    # EXIT_BUTTON (exitbtn.c): then the main menu's Exit button, in the main menu files that have the gear
    # (btnSettings; the _EdAXiF layout has none)
    # DISPLAY_MENU (display.c): and Options' Customer Care tile made the Display tile, in the main menu files
    # and the in-race ones (the pause menu's Options: with only the main menus patched, the
    # race's Options showed Customer Care's tile that opened nothing); the _EdAXiF
    # layouts have no Customer Care tile
    patcher = os.path.join(ROOT, 'tools', 'swfpatch_controls.py')
    exit_patcher = os.path.join(ROOT, 'tools', 'swfpatch_exit.py')
    name_patcher = os.path.join(ROOT, 'tools', 'swfpatch_name.py')
    display_patcher = os.path.join(ROOT, 'tools', 'swfpatch_display.py')
    # DYNAMIC_PRICE (shop.c): the title bars' NEW label on the cash, in the main menu files
    cashnew_patcher = os.path.join(ROOT, 'tools', 'swfpatch_cashnew.py')
    for name in ('MZlN_MiNH_XiZEKXiiMlHM.FAK', 'MZlN_MiNH_XiZELBKXiiMlHM.FAK', 'MZlN_MiNH_XiZEKXiiMlHM_EdAXiF.FAK',
                 'LHBMiNHFKXiiMlHM.FAK', 'LHBMiNHFLBKXiiMlHM.FAK', 'LHBMiNHFKXiiMlHM_EdAXiF.FAK'):
        swf = os.path.join(ROOT, 'GTRacingFreemiumHD.app', name)
        patched = os.path.join(OUT, 'patch', name)
        with_exit = name in ('MZlN_MiNH_XiZEKXiiMlHM.FAK', 'MZlN_MiNH_XiZELBKXiiMlHM.FAK')
        with_display = not name.endswith('_EdAXiF.FAK')
        with_cashnew = name.startswith('MZlN_MiNH_')
        if os.path.exists(swf) and newer(swf, patched, [patcher] + ([exit_patcher, name_patcher] if with_exit else []) +
                                         ([display_patcher] if with_display else []) +
                                         ([cashnew_patcher] if with_cashnew else [])):
            os.makedirs(os.path.dirname(patched), exist_ok=True)
            rc, out = run([py, '-I', patcher, swf, patched])
            print(out)
            if rc:
                sys.exit(rc)
            if with_exit:
                rc, out = run([py, '-I', exit_patcher, patched, patched])
                print(out)
                if rc:
                    sys.exit(rc)
                # PLAYER_NAME (p2p.c): the name tile on the multiplayer page
                rc, out = run([py, '-I', name_patcher, patched, patched])
                print(out)
                if rc:
                    sys.exit(rc)
            if with_display:
                rc, out = run([py, '-I', display_patcher, patched, patched])
                print(out)
                if rc:
                    sys.exit(rc)
            if with_cashnew:
                rc, out = run([py, '-I', cashnew_patcher, patched, patched])
                print(out)
                if rc:
                    sys.exit(rc)
    print('built %s (%s build, %.1f MB) in %.1f s' % (os.path.relpath(exe, ROOT), 'release' if release else 'development',
                                                       os.path.getsize(exe) / 1e6, time.time() - t0))


if __name__ == '__main__':
    main()
