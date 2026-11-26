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
