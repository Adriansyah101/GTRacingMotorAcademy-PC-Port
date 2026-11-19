"""Name the host function at an address of gtracing.exe (from the build's gtracing.map).

Usage: python -I tools/addr2sym.py [--map FILE] 0xf54646 [more addresses]
  --map FILE  the map of the exe the addresses come from: build/host/release/gtracing.map for the release build,
              or a release's <out>/symbols/<folder>.map (tools/package_release.py).  Default: the development
              build's build/host/gtracing.map.
Recompiled game functions are F_<guest address>; host functions keep their C names.
"""
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, 'build', 'host', 'gtracing.map')


def load(path):
    syms = []
    pat = re.compile(r'^\s+0x([0-9a-f]{8})\s+([A-Za-z_][\w.$@?]*)\s*$')
    for line in open(path, encoding='latin1'):
        m = pat.match(line)
        if m:
            syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def main():
    args = sys.argv[1:]
    path = MAP
    if '--map' in args:
        i = args.index('--map')
        if i + 1 >= len(args):
            sys.exit('--map needs a file')
        path = args[i + 1]
        del args[i:i + 2]
    syms = load(path)
    addrs = [a for a, _ in syms]
    for arg in args:
        a = int(arg, 16)
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            print('%s: ?' % arg)
            continue
        print('%s: %s+0x%x' % (arg, syms[i][1], a - syms[i][0]))


if __name__ == '__main__':
    main()
