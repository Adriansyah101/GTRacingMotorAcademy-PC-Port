import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST = os.path.join(ROOT, 'host')
NM = r'C:\msys64\mingw32\bin\nm.exe' if os.name == 'nt' else 'nm'
LOWER = ('core', 'shims', os.path.join('shims', 'gl'))
GAME = (os.path.join('games', 'gtracing', 'features'), os.path.join('games', 'gtracing', 'render'))


def sources(dirs):
    return {os.path.splitext(os.path.basename(f))[0]: f
            for d in dirs for f in glob.glob(os.path.join(HOST, d, '*.[cS]'))}


def symbols(obj, elf=False):
    out = subprocess.run([NM, obj], capture_output=True, text=True).stdout
    defined, used = set(), set()

    def plain(n):
        return n[1:] if not elf and n.startswith('_') else n
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) == 2 and parts[0] == 'U':
            used.add(plain(parts[1]))
        elif len(parts) == 2 and parts[0] in 'wv':
            used.add(plain(parts[1]))
        elif len(parts) == 3 and parts[1] in 'TDBR':
            defined.add(plain(parts[2]))
    return defined, used


def main():
    elf = sys.argv[sys.argv.index('--platform') + 1] == 'linux' if '--platform' in sys.argv[:-1] else False
    out = os.path.join(ROOT, 'build', 'host', 'linux') if elf else os.path.join(ROOT, 'build', 'host')
    objdir = os.path.join(out, 'obj_release' if '--release' in sys.argv else 'obj')
    if not os.path.isdir(objdir):
        print('layer check: no objects in %s (build first)' % os.path.relpath(objdir, ROOT))
        return 1
    lower, game = sources(LOWER), sources(GAME)
    hooks = open(os.path.join(HOST, 'shims', 'game_hooks.h'), encoding='utf-8').read()
    declared = set(re.findall(r'\b(\w+)\s*\(', re.sub(r'/\*.*?\*/', '', hooks, flags=re.S)))
    syms = {}
    for name in list(lower) + list(game):
        obj = os.path.join(objdir, name + '.o')
        if os.path.exists(obj):
            syms[name] = symbols(obj, elf)
    game_defs = {s: n for n in game if n in syms for s in syms[n][0]}
    bad = 0
    for name in sorted(lower):
        if name not in syms:
            continue
        for s in sorted(syms[name][1]):
            if s in game_defs and s not in declared:
                print('%s uses %s (%s) without game_hooks.h' % (name, s, game_defs[s]))
                bad += 1
    game_headers = {os.path.basename(f) for d in GAME for f in glob.glob(os.path.join(HOST, d, '*.h'))}
    for name, src in sorted(lower.items()):
        for inc in re.findall(r'^#include "([^"]+)"', open(src, encoding='utf-8').read(), re.M):
            if inc in game_headers:
                print('%s includes the game layer\'s %s' % (os.path.basename(src), inc))
                bad += 1
    if '--unused' in sys.argv:
        used_anywhere = set()
        for o in glob.glob(os.path.join(objdir, '*.o')) + glob.glob(os.path.join(out, 'obj_gen', '*.o')):
            used_anywhere |= symbols(o, elf)[1]
        for name in sorted(syms):
            for s in sorted(syms[name][0] - used_anywhere):
                if not s.startswith(('h_', 'I_', 'F_', '__wrap_', 'p_gl', 'main')):
                    print('unused outside %s: %s' % (name, s))
    print('layer check: %s' % ('%d problems' % bad if bad else 'clean'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
