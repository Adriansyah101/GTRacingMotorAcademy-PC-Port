import glob
import os
import re
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
LOW_DELTA = '0x8bf000u'
SDL = os.path.join(HERE, 'third_party', 'SDL3')
SRC_DIRS = [os.path.join(HERE, *d.split('/')) for d in
            ('core', 'shims', 'shims/gl', 'games/gtracing', 'games/gtracing/features', 'games/gtracing/render')]
CORE = SRC_DIRS[0]
INCLUDES = [a for d in SRC_DIRS for a in ('-I', d)]
LOWER_DIRS = SRC_DIRS[:4]
LOWER_INCLUDES = [a for d in LOWER_DIRS for a in ('-I', d)]


def includes_for(src):
    d = os.path.dirname(os.path.abspath(src))
    return LOWER_INCLUDES if d in LOWER_DIRS or d.startswith(OUT) else INCLUDES


def quote_includes(src):
    d = os.path.dirname(os.path.abspath(src))
    dirs = LOWER_DIRS if d in LOWER_DIRS or d.startswith(OUT) else SRC_DIRS
    return [a for x in dirs for a in ('-iquote', x)]

PREFIX_MAP = '-ffile-prefix-map=%s=.' % ROOT
GCC_FLAGS = ['-O2', '-g', '-c', '-std=gnu11', '-m32', '-msse2', '-mfpmath=sse', '-fno-strict-aliasing',
             '-fwrapv', '-Wall', '-Wextra', '-D__USE_MINGW_ANSI_STDIO=1',
             PREFIX_MAP, '-DARM_LOW_DELTA=' + LOW_DELTA] + [
             '-I', os.path.join(ROOT, 'runtime'), '-I', os.path.join(OUT, 'gen'),
             '-I', os.path.join(SDL, 'include')]


def sources(pattern, linux=False):
    found = sorted(f for d in SRC_DIRS for f in glob.glob(os.path.join(d, pattern)))
    if linux:
        found = [f for f in found if not os.path.exists(f[:-2] + '_linux' + f[-2:])]
    else:
        found = [f for f in found if not f[:-2].endswith('_linux')]
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


def build_windows(args):
    t0 = time.time()
    release = '--release' in args
    variant = os.path.join(OUT, 'release') if release else OUT
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
    for name in ('rt_jump', 'rt_selftest'):
        src = os.path.join(ROOT, 'runtime', name + '.c')
        obj = os.path.join(objhost, name + '.o')
        if newer(src, obj, headers):
            jobs.append((['clang'] + CLANG_FLAGS + [src, '-o', obj], None, src))
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
    art, radius = os.path.join(ROOT, 'iTunesArtwork'), '0.16'
    if not os.path.exists(art):
        art, radius = os.path.join(ROOT, 'GTRacingFreemiumHD.app', 'iTunesArtwork'), '0.203'
    ico = os.path.join(OUT, 'app.ico')
    res = os.path.join(objhost, 'app_res.o')
    icon_tool = os.path.join(ROOT, 'tools', 'make_icon.py')
    stamp = os.path.join(OUT, 'app_ico.src')
    src_was = open(stamp).read() if os.path.exists(stamp) else ''
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
            '-Wl,--wrap=F_2d8c1c', '-Wl,--wrap=F_2d9720', '-Wl,--wrap=F_2d90c0',
            '-Wl,--wrap=F_2ef358', '-Wl,--wrap=F_2ef104', '-Wl,--wrap=F_1d69d0', '-Wl,--wrap=F_2f6ef8',
            '-Wl,--wrap=F_2ef6f0', '-Wl,--wrap=F_1ff744', '-Wl,--wrap=F_1feaec',
            '-Wl,--wrap=F_268704', '-Wl,--wrap=F_268e70', '-Wl,--wrap=F_2716c4',
            '-Wl,--wrap=F_13c82c', '-Wl,--wrap=F_13de08',
            '-Wl,--wrap=F_34a85c', '-Wl,--wrap=F_2a4f38',
            '-Wl,--wrap=F_2a4ba4',
            '-Wl,--wrap=F_2a36e8',
            '-Wl,--wrap=F_14a660',
            '-Wl,--wrap=F_2659b0', '-Wl,--wrap=F_26a634', '-Wl,--wrap=F_26a980', '-Wl,--wrap=F_265920',
            '-Wl,--wrap=F_517608', '-Wl,--wrap=F_515980', '-Wl,--wrap=F_4eb4ac', '-Wl,--wrap=F_500b58',
            '-Wl,--wrap=F_4ca684',
            '-Wl,--wrap=F_1bb6c0',
            '-Wl,--wrap=F_12bc00',
            '-Wl,--wrap=F_212170', '-Wl,--wrap=F_376e68',
            '-Wl,--wrap=F_2b5fe8', '-Wl,--wrap=F_2b689c', '-Wl,--wrap=F_2b7e78', '-Wl,--wrap=F_270ac0',
            '-Wl,--wrap=F_266820',
            '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free',
            '-Wl,-Map=' + os.path.join(variant, 'gtracing.map'), '@' + rsp, '-o', exe,
            os.path.join(SDL, 'lib', 'libSDL3.dll.a'),
            '-lws2_32', '-liphlpapi', '-lwinmm', '-lgdi32', '-lopengl32', '-ladvapi32', '-lole32',
            '-lgdiplus', '-lshell32',
            '-lwinhttp']
    rc, out = run(link, env)
    if out:
        print(out[:6000])
    if rc:
        print('link failed')
        sys.exit(1)
    dll = os.path.join(SDL, 'bin', 'SDL3.dll')
    if newer(dll, os.path.join(variant, 'SDL3.dll')):
        shutil.copy2(dll, variant)
    patch_swfs(py, os.path.join(OUT, 'patch'))
    print('built %s (%s build, %.1f MB) in %.1f s' % (os.path.relpath(exe, ROOT), 'release' if release else 'development',
                                                       os.path.getsize(exe) / 1e6, time.time() - t0))



def patch_swfs(py, out_dir):
    patcher = os.path.join(ROOT, 'tools', 'swfpatch_controls.py')
    exit_patcher = os.path.join(ROOT, 'tools', 'swfpatch_exit.py')
    name_patcher = os.path.join(ROOT, 'tools', 'swfpatch_name.py')
    display_patcher = os.path.join(ROOT, 'tools', 'swfpatch_display.py')
    cashnew_patcher = os.path.join(ROOT, 'tools', 'swfpatch_cashnew.py')
    for name in ('MZlN_MiNH_XiZEKXiiMlHM.FAK', 'MZlN_MiNH_XiZELBKXiiMlHM.FAK', 'MZlN_MiNH_XiZEKXiiMlHM_EdAXiF.FAK',
                 'LHBMiNHFKXiiMlHM.FAK', 'LHBMiNHFLBKXiiMlHM.FAK', 'LHBMiNHFKXiiMlHM_EdAXiF.FAK'):
        swf = os.path.join(ROOT, 'GTRacingFreemiumHD.app', name)
        patched = os.path.join(out_dir, name)
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


LINUX_OUT = os.path.join(OUT, 'linux')
LINUX_FLAGS = ['-O2', '-g', '-c', '-std=gnu11', '-m32', '-msse2', '-mfpmath=sse', '-malign-double', '-fno-pie',
               '--param=min-pagesize=0', '-fno-delete-null-pointer-checks', '-fno-strict-aliasing', '-fwrapv',
               '-Wall', '-Wextra', '-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64',
               PREFIX_MAP, '-DARM_LOW_DELTA=' + LOW_DELTA, '-iquote', os.path.join(ROOT, 'runtime'),
               '-iquote', os.path.join(OUT, 'gen')]
LINUX_GEN_FLAGS = ['-m32', '-msse2', '-mfpmath=sse', '-malign-double', '-fno-pie', '--param=min-pagesize=0',
                   '-fno-delete-null-pointer-checks', '-O2', '-c', '-fno-strict-aliasing', '-ffp-contract=off',
                   '-fwrapv', '-DARM_LOW_DELTA=' + LOW_DELTA, '-iquote', os.path.join(ROOT, 'runtime'), PREFIX_MAP]
LINUX_LIBS = ['-lSDL3', '-lGL', '-lm', '-lpthread', '-ldl']


def linux_wraps():
    src = open(os.path.abspath(__file__), encoding='utf-8').read()
    return sorted(set(re.findall(r"'-Wl,--wrap=(\w+)'", src))) + ['fopen', 'fopen64', 'fclose', 'remove', 'rename']


def build_linux(args):
    t0 = time.time()
    release = '--release' in args
    cc = os.environ.get('CC', 'gcc')
    variant = os.path.join(LINUX_OUT, 'release') if release else LINUX_OUT
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
    objgen = os.path.join(LINUX_OUT, 'obj_gen')
    if '--no-gencc' not in args:
        rc, out = run([py, '-I', os.path.join(ROOT, 'recomp', 'compile_all.py'), GEN, objgen, 'i686-linux',
                       '-DARM_LOW_DELTA=' + LOW_DELTA, PREFIX_MAP])
        print(out)
        if rc:
            sys.exit(rc)
    objhost = os.path.join(LINUX_OUT, 'obj_release' if release else 'obj')
    os.makedirs(objhost, exist_ok=True)
    headers = sources('*.h') + [os.path.join(ROOT, 'runtime', 'arm_rt.h')]
    c_srcs = sources('*.c', linux=True) + [os.path.join(OUT, 'gen', 'bridges.c')]
    s_srcs = sources('*.S', linux=True)
    jobs = []
    for src in c_srcs:
        obj = os.path.join(objhost, os.path.basename(src)[:-2] + '.o')
        if newer(src, obj, headers):
            jobs.append(([cc] + LINUX_FLAGS + quote_includes(src) + [dev_flag, src, '-o', obj], None, src))
    for src in s_srcs:
        obj = os.path.join(objhost, os.path.basename(src)[:-2] + '.o')
        if newer(src, obj, headers):
            jobs.append(([cc, '-c', '-m32', PREFIX_MAP] + quote_includes(src) + [src, '-o', obj], None, src))
    for name in ('rt_jump', 'rt_selftest'):
        src = os.path.join(ROOT, 'runtime', name + '.c')
        obj = os.path.join(objhost, name + '.o')
        if newer(src, obj, headers):
            jobs.append(([cc] + LINUX_GEN_FLAGS + [src, '-o', obj], None, src))
    made = {os.path.basename(x)[:-2] + '.o' for x in c_srcs + s_srcs} | {'rt_jump.o', 'rt_selftest.o'}
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
    gen_objs = [o for o in sorted(glob.glob(os.path.join(objgen, '*.o')))
                if os.path.basename(o) not in ('imports_stub.o', 'imports.o')]
    host_objs = sorted(glob.glob(os.path.join(objhost, '*.o')))
    exe = os.path.join(variant, 'gtracing')
    rsp = os.path.join(variant, 'link.rsp')
    with open(rsp, 'w') as fp:
        fp.write('\n'.join('"%s"' % o for o in host_objs + gen_objs))
    link = [cc, '-m32', '-no-pie', '-O2', '-Wl,-Ttext-segment=0x10000', '-Wl,-T,' + os.path.join(CORE, 'gt_linux.ld'),
            '-Wl,-z,noexecstack', '-Wl,-Map=' + os.path.join(variant, 'gtracing.map')]
    link += ['-Wl,--wrap=' + w for w in linux_wraps()]
    link += ['@' + rsp, '-o', exe] + LINUX_LIBS
    rc, out = run(link)
    if out:
        print(out[:6000])
    if rc:
        print('link failed')
        sys.exit(1)
    patch_swfs(py, os.path.join(LINUX_OUT, 'patch'))
    print('built %s (%s build, %.1f MB) in %.1f s' % (os.path.relpath(exe, ROOT), 'release' if release else 'development',
                                                       os.path.getsize(exe) / 1e6, time.time() - t0))


def in_wsl(args):
    p = subprocess.run(['wsl.exe', '-e', 'wslpath', '-a', os.path.abspath(__file__).replace('\\', '/')],
                       capture_output=True, text=True)
    if p.returncode or not p.stdout.strip():
        print('--platform linux on Windows needs WSL with a 32-bit gcc and SDL3 (see CONTRIBUTING.md)')
        return 1
    return subprocess.run(['wsl.exe', '-e', 'python3', p.stdout.strip(), '--platform', 'linux'] + args).returncode


def main():
    args = sys.argv[1:]
    plat = 'windows' if os.name == 'nt' else 'linux'
    if '--platform' in args:
        i = args.index('--platform')
        if i + 1 >= len(args) or args[i + 1] not in ('windows', 'linux', 'all'):
            print('--platform takes windows, linux or all')
            sys.exit(2)
        plat = args[i + 1]
        args = args[:i] + args[i + 2:]
    if plat in ('windows', 'all'):
        if os.name != 'nt':
            print('the Windows build needs Windows and MSYS2 (mingw32)')
            sys.exit(1)
        build_windows(args)
    if plat in ('linux', 'all'):
        rc = in_wsl(args) if os.name == 'nt' else build_linux(args)
        if rc:
            sys.exit(rc)


if __name__ == '__main__':
    main()
