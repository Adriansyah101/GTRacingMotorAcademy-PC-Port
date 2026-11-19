"""Print a game function's instructions (from build/gen's comments) with what its pc-relative
addresses are: selectors, class references, C strings, CFStrings, imports.

Usage: python -I tools/fdis.py 0x5cb95 [max_lines]
The address may have the Thumb bit; calls show the callee (import name or F_<addr>).
"""
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from macho import MachO  # noqa: E402

GEN = os.path.join(ROOT, 'build', 'gen')
EXE = os.path.join(ROOT, 'GTRacingFreemiumHD.app', 'GTRacingFreemiumHD')


def cstr(m, a, n=96):
    try:
        b = m.read(a, n)
    except ValueError:
        return None
    s = b.split(b'\0')[0]
    if len(s) < 2 or any(c < 9 or (13 < c < 32) or c > 126 for c in s):
        return None
    return s.decode('latin1')


def describe(m, a, sects):
    s = m.section_at(a)
    if s is None:
        return None
    if s.name == '__objc_selrefs':
        return 'sel %s' % cstr(m, m.u32(a))
    if s.name in ('__objc_classrefs', '__objc_superrefs'):
        v = m.u32(a)
        if v:
            ro = m.u32(v + 16) & ~3
            return 'class %s' % cstr(m, m.u32(ro + 16))
        return 'class (import) %s' % m.ptr_slots.get(a, sects.get(a, '?'))
    if s.name == '__cfstring':
        return 'CFString "%s"' % cstr(m, m.u32(a + 8), 160)
    if s.name in ('__cstring', '__const', '__data'):
        t = cstr(m, a, 160)
        if t:
            return '"%s"' % t
    if s.name in ('__nl_symbol_ptr', '__la_symbol_ptr'):
        return 'ptr %s' % m.ptr_slots.get(a)
    return '%s,%s' % (s.seg, s.name)


def xrefs(target):
    """Every instruction whose movw/movt + pc arithmetic lands on target (all of build/gen)."""
    pat = re.compile(r'/\* ([0-9a-f]+) (.*) \*/')
    fn_pat = re.compile(r'^void F_([0-9a-f]+)\(CPU \*c\) \{')
    starts = {}
    for f in sorted(glob.glob(os.path.join(GEN, 'code_*.c'))):
        regs, fn, thumb = {}, None, False
        with open(f, encoding='latin1') as fp:
            lines = fp.readlines()
        for k, ln in enumerate(lines):
            fm = fn_pat.match(ln)
            if fm:
                fn = int(fm.group(1), 16)
                regs = {}
                # Thumb if the next instruction is 2 bytes after this one
                a = [int(x.group(1), 16) for x in (pat.search(lines[j]) for j in range(k + 1, min(k + 12, len(lines)))) if x]
                thumb = len(a) > 1 and a[1] - a[0] == 2
                continue
            mm = pat.search(ln)
            if not mm:
                continue
            pc_addr, ins = int(mm.group(1), 16), mm.group(2)
            mv = re.match(r'movw (\w+), #(0x[0-9a-f]+|\d+)$', ins)
            mt = re.match(r'movt (\w+), #(0x[0-9a-f]+|\d+)$', ins)
            ad = re.match(r'add (\w+), pc(?:, (\w+))?$', ins)
            ld = re.match(r'ldr (\w+), \[pc, (\w+)\]$', ins)
            if mv:
                regs[mv.group(1)] = int(mv.group(2), 0)
            elif mt and mt.group(1) in regs:
                regs[mt.group(1)] = (regs[mt.group(1)] & 0xffff) | int(mt.group(2), 0) << 16
            elif ad or ld:
                g = ad or ld
                r = g.group(2) or g.group(1)
                if r in regs and (pc_addr + (4 if thumb else 8) + regs[r]) & 0xffffffff == target:
                    print('F_%x  %08x  %s' % (fn, pc_addr, ins))


def main():
    if sys.argv[1] == '--xref':
        xrefs(int(sys.argv[2], 16))
        return
    raw = int(sys.argv[1], 16)
    thumb = raw & 1
    addr = raw & ~1
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 400
    m = MachO(EXE)
    binds = {}
    name = 'F_%x' % addr
    lines = None
    for f in sorted(glob.glob(os.path.join(GEN, 'code_*.c'))):
        with open(f, encoding='latin1') as fp:
            text = fp.read()
        i = text.find('\nvoid %s(CPU *c) {' % name)
        if i >= 0:
            j = text.find('\nvoid F_', i + 10)
            lines = text[i:j if j > 0 else len(text)].splitlines()
            break
    if lines is None:
        sys.exit('no %s in build/gen' % name)
    regs = {}
    out = 0
    pat = re.compile(r'/\* ([0-9a-f]+) (.*) \*/')
    for ln in lines:
        mm = pat.search(ln)
        if not mm:
            t = ln.strip()
            if re.match(r'^(I_\w+|F_\w+)\(c\);', t):
                print('        -> %s' % t[:-4])
            continue
        pc_addr, ins = int(mm.group(1), 16), mm.group(2)
        note = ''
        mv = re.match(r'movw (\w+), #(0x[0-9a-f]+|\d+)$', ins)
        mt = re.match(r'movt (\w+), #(0x[0-9a-f]+|\d+)$', ins)
        ad = re.match(r'add (\w+), pc(?:, (\w+))?$', ins)
        ld = re.match(r'ldr (\w+), \[pc, (\w+)\]$', ins)
        if mv:
            regs[mv.group(1)] = int(mv.group(2), 0)
        elif mt and mt.group(1) in regs:
            regs[mt.group(1)] = (regs[mt.group(1)] & 0xffff) | int(mt.group(2), 0) << 16
        elif ad or ld:
            g = ad or ld
            r = (g.group(2) if ad else g.group(2)) or g.group(1)
            if r in regs:
                pcv = pc_addr + (4 if thumb else 8)
                a = (pcv + regs[r]) & 0xffffffff
                d = describe(m, a, binds)
                note = '   ; [%#x] %s' % (a, d) if ld else '   ; %#x %s' % (a, d)
        print('%08x  %s%s' % (pc_addr, ins, note))
        out += 1
        if out >= limit:
            break


if __name__ == '__main__':
    main()
