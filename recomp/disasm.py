"""Print disassembly around an address in the mode of the function containing it.

Usage: python -I disasm.py <executable> <addr> [count=40] [before_bytes=0] [arm|thumb]
"""
import bisect
import os
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402


def main():
    m = MachO(sys.argv[1])
    addr = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    before = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
    starts = sorted(m.function_starts)
    keys = [a for a, _ in starts]
    i = bisect.bisect_right(keys, addr) - 1
    fstart, thumb = starts[i]
    if len(sys.argv) > 5:
        thumb = sys.argv[5] == 'thumb'
    fend = keys[i + 1] if i + 1 < len(keys) else fstart + 0x10000
    print('function %08x-%08x %s %s' % (fstart, fend, 'Thumb' if thumb else 'ARM', m.names.get(fstart, '')))
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
    a = max(fstart, addr - before)
    n = 0
    while n < count:
        code = m.read(a, 0x400)
        got = False
        for ins in md.disasm(code, a):
            mark = '>' if ins.address == addr else ' '
            extra = ''
            t = ins.op_str.lstrip('#')
            if ins.mnemonic.startswith('b') and t.startswith('0x'):
                v = int(t, 16)
                extra = m.stubs.get(v) or m.names.get(v) or ''
                if any(v == s for s, _ in starts) and not extra:
                    extra = 'sub_%x' % v
            if ins.address == fend:
                print('---------- next function %08x' % fend)
            print('%s%08x %-10s %-8s %-36s %s' % (mark, ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, extra))
            a = ins.address + ins.size
            n += 1
            got = True
            if n >= count:
                break
        if not got:
            w = m.read(a, 4)
            print(' %08x %-10s .word (no decode)' % (a, w.hex()))
            a += 2 if thumb else 4
            n += 1


if __name__ == '__main__':
    main()
