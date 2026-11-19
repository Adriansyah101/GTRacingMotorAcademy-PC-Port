"""Count the instructions stage 1 found, by mnemonic (without condition) and mode.

Usage: python -I census.py <executable> <functions.json> [out.txt]
Tells the code generator which instruction kinds it must handle.
"""
import json
import os
import sys
from collections import Counter

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402

COND = ('eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'cs', 'cc')


def base_mnemonic(ins):
    """Mnemonic with the condition code and '.w'/'.n' removed (data type kept)."""
    m = ins.mnemonic
    parts = m.split('.')
    head = parts[0]
    if ins.cc not in (0, 15):            # ARM_CC_INVALID / AL
        for c in COND:
            if head.endswith(c) and head not in ('teq', 'vcmpe'):
                head = head[:-2]
                break
    rest = [p for p in parts[1:] if p not in ('w', 'n')]
    return '.'.join([head] + rest)


def iter_insns(m, funcs):
    md = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
    for t in md.values():
        t.detail = True
    for f in funcs:
        for a, e in f['runs']:
            for ins in md[f['thumb']].disasm(m.read(a, e - a), a):
                yield f, ins


def main():
    m = MachO(sys.argv[1])
    funcs = json.load(open(sys.argv[2]))['functions']
    out = sys.argv[3] if len(sys.argv) > 3 else None
    cnt = Counter()
    for f, ins in iter_insns(m, funcs):
        cnt[('T' if f['thumb'] else 'A', base_mnemonic(ins))] += 1
    lines = ['%-2s %-24s %d' % (k[0], k[1], n) for k, n in sorted(cnt.items(), key=lambda x: -x[1])]
    text = '\n'.join(lines)
    if out:
        open(out, 'w').write(text + '\n')
    print('distinct', len(cnt), 'total', sum(cnt.values()))


if __name__ == '__main__':
    main()
