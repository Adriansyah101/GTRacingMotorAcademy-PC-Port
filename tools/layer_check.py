"""Check the host's layer boundary in the built objects: every game-layer symbol (games/gtracing/features/, render/)
that core/ or shims/ use must be declared in host/shims/game_hooks.h, and no lower-layer source includes a
game-layer header.  Also lists the external functions and data an object defines that no other object uses
(candidates for static).

Usage: python -I tools/layer_check.py [--release] [--unused]
Exit code 1 when the boundary is crossed.  Run after host/build.py (it reads build/host/obj or obj_release).
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST = os.path.join(ROOT, 'host')
NM = r'C:\msys64\mingw32\bin\nm.exe'
LOWER = ('core', 'shims', os.path.join('shims', 'gl'))
GAME = (os.path.join('games', 'gtracing', 'features'), os.path.join('games', 'gtracing', 'render'))


def sources(dirs):
    return {os.path.splitext(os.path.basename(f))[0]: f
            for d in dirs for f in glob.glob(os.path.join(HOST, d, '*.[cS]'))}


def symbols(obj):
    out = subprocess.run([NM, obj], capture_output=True, text=True).stdout
    defined, used = set(), set()
    for ln in out.splitlines():
        parts = ln.split()
        if len(parts) == 2 and parts[0] == 'U':
            used.add(parts[1][1:] if parts[1].startswith('_') else parts[1])
        elif len(parts) == 2 and parts[0] in 'wv':             # a weak reference (a weak call) counts as a use
            used.add(parts[1][1:] if parts[1].startswith('_') else parts[1])
        elif len(parts) == 3 and parts[1] in 'TDBR':
            defined.add(parts[2][1:] if parts[2].startswith('_') else parts[2])
    return defined, used


def main():
    objdir = os.path.join(ROOT, 'build', 'host', 'obj_release' if '--release' in sys.argv else 'obj')
    lower, game = sources(LOWER), sources(GAME)
    hooks = open(os.path.join(HOST, 'shims', 'game_hooks.h'), encoding='utf-8').read()
    declared = set(re.findall(r'\b(\w+)\s*\(', re.sub(r'/\*.*?\*/', '', hooks, flags=re.S)))
    syms = {}
    for name in list(lower) + list(game):
        obj = os.path.join(objdir, name + '.o')
        if os.path.exists(obj):
            syms[name] = symbols(obj)
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
        for o in glob.glob(os.path.join(objdir, '*.o')) + glob.glob(os.path.join(ROOT, 'build', 'host', 'obj_gen', '*.o')):
            used_anywhere |= symbols(o)[1]
        for name in sorted(syms):
            for s in sorted(syms[name][0] - used_anywhere):
                # h_ / I_: import bridges (bridges.c names them); __wrap_: ld --wrap; p_gl: the GL entry points
                if not s.startswith(('h_', 'I_', 'F_', '__wrap_', 'p_gl', 'main')):
                    print('unused outside %s: %s' % (name, s))
    print('layer check: %s' % ('%d problems' % bad if bad else 'clean'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
