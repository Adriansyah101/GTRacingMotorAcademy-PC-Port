"""Stage 2 function tests: whole generated functions (the real code, with its
labels, gotos, jump-table switches, loops and returns) against Unicorn.

Usage: python -I tests/functest.py <exe> <functions.json> <work_dir>
       [--max N] [--states N] [--seed N]

Leaf functions only (no calls, imports, calls through registers, tail jumps or
SjLj labels): each is called with random arguments in r0-r3 (small integers or
pointers into pattern memory) and a fresh stack, in the DLL (codegen output,
ARM_TEST_MEM) and in Unicorn until it returns. Compared: r0-r11, sp, d0-d31,
the FPSCR flags and control bits, and every written byte. A trap in both
counts as a match; runs that hit the step limit on either side are skipped.
"""
import ctypes
import json
import os
import random
import subprocess
import sys
import time
from collections import Counter

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL
from unicorn import UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED
from unicorn import arm_const as U

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
sys.path.insert(0, HERE)
from macho import MachO  # noqa: E402
from armc import Program  # noqa: E402
from codegen import emit_function  # noqa: E402
from difftest import CPU, UC_R, UC_D, pat_page, rand_f32, rand_f64, same_d, arg, is_nan32  # noqa: E402

MAGIC = 0x0FFF0000          # return address: the run ends when pc gets here
UC_LIMIT = 1000000
WBUF = (ctypes.c_uint32 * (1 << 20))()
M32 = 0xFFFFFFFF


def leaf_functions(data):
    out = []
    for f in data['functions']:
        if f['calls'] or f['imports'] or f['icalls'] or f['taken'] or f['tails'] or f['ijumps']:
            continue
        if any(k == 'tail' for _, k, _ in f.get('ctl', [])):
            continue
        if f.get('origin') in ('tail', 'split'):
            continue            # pieces entered by a jump: they run in another function's frame
        out.append(f)
    return out


def build(m, data, fns, work):
    prog = Program(m, data)
    md = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
    for d in md.values():
        d.detail = True
    src = ['#define ARM_TEST_MEM', '#include <stdint.h>',
           'extern uint32_t tm_steps, tm_limit; void tm_bail(int);',
           '#define LABEL_HOOK() do { if (++tm_steps > tm_limit) tm_bail(2); } while (0)',
           '#include "arm_rt.h"', '#define EXPORT __declspec(dllexport)',
           'extern void *tm_jb[5]; extern int tm_code;', '']
    src += ['void F_%x(CPU *c);' % f['start'] for f in fns]
    errs = 0
    for f in fns:
        text, e, w, u = emit_function(prog, f, md)
        errs += len(e)
        src.append(text)
    src.append('static void (*const fns[])(CPU *) = {%s};' % ', '.join('F_%x' % f['start'] for f in fns))
    src.append('EXPORT int run_fn(int k, CPU *c) {')
    src.append('  tm_steps = 0; tm_code = 0;')
    src.append('  if (__builtin_setjmp(tm_jb)) return tm_code;')
    src.append('  fns[k](c);')
    src.append('  return 0;')
    src.append('}')
    path = os.path.join(work, 'functests.c')
    with open(path, 'w', newline='\n') as fp:
        fp.write('\n'.join(src) + '\n')
    dll = os.path.join(work, 'functest.dll')
    cmd = ['clang', '--target=x86_64-w64-mingw32', '-shared', '-O1', '-fno-strict-aliasing',
           '-ffp-contract=off', '-fwrapv', '-w', '-I', os.path.join(ROOT, 'runtime'), path,
           os.path.join(HERE, 'tmem.c'), '-o', dll]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        print(p.stderr[:5000])
        sys.exit(1)
    return dll, errs


class FnRunner:
    def __init__(self, image, base):
        self.img, self.base = image, base
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(U.UC_CPU_ARM_CORTEX_A8)
        size = (len(image) + 0xFFF) & ~0xFFF
        uc.mem_map(base, size, UC_PROT_ALL)
        uc.mem_write(base, bytes(image))
        uc.mem_map(MAGIC, 0x1000, UC_PROT_ALL)
        uc.reg_write(U.UC_ARM_REG_C1_C0_2, uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)
        self.uc, self.dyn, self.dirty = uc, [], []
        self.img_bytes = bytes(image)

    def changed(self):
        """Bytes that differ from the start (no write hook: it slows loops down a lot)."""
        uc, out = self.uc, set()
        for p in self.dyn:
            cur, ref = bytes(uc.mem_read(p, 0x1000)), pat_page(p >> 12)
            if cur != ref:
                out.update(p + k for k in range(0x1000) if cur[k] != ref[k])
        cur = bytes(uc.mem_read(self.base, len(self.img_bytes)))
        if cur != self.img_bytes:
            for o in range(0, len(cur), 0x10000):
                if cur[o:o + 0x10000] == self.img_bytes[o:o + 0x10000]:
                    continue
                for q in range(o, min(o + 0x10000, len(cur)), 0x1000):
                    a, b = cur[q:q + 0x1000], self.img_bytes[q:q + 0x1000]
                    if a != b:
                        self.dirty.append(q)
                        out.update(self.base + q + k for k in range(len(a)) if a[k] != b[k])
        return out

    def on_unmapped(self, uc, access, addr, size, value, user):
        if len(self.dyn) >= 1500:
            # Pointer chasing through random memory: give up (C bails similarly).
            self.too_big = True
            uc.emu_stop()
            return False
        for p in {addr >> 12, (addr + max(size, 1) - 1) >> 12}:
            if (p << 12) in self.dyn:
                continue
            uc.mem_map(p << 12, 0x1000, UC_PROT_ALL)
            uc.mem_write(p << 12, pat_page(p))
            self.dyn.append(p << 12)
        return True

    def reset(self):
        for q in self.dirty:
            self.uc.mem_write(self.base + q, self.img_bytes[q:q + 0x1000])
        for p in self.dyn:
            self.uc.mem_unmap(p, 0x1000)
        self.dyn, self.dirty = [], []

    def byte(self, a):
        try:
            return self.uc.mem_read(a, 1)[0]
        except UcError:
            if self.base <= a < self.base + len(self.img):
                return self.img[a - self.base]
            return pat_page(a >> 12)[a & 0xFFF]

    def run(self, f, st):
        r = self.run1(f, st)
        if r['outcome'] != 'ret':
            self.changed()          # find what a trapped run wrote, so reset() puts it back
        return r

    def run1(self, f, st):
        uc = self.uc
        self.reset()
        regs, fpscr, d = st
        for i, v in enumerate(regs):
            uc.reg_write(UC_R[i], v)
        cpsr = uc.reg_read(U.UC_ARM_REG_CPSR) & 0x1F
        if f['thumb']:
            cpsr |= 0x20
        uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
        uc.reg_write(U.UC_ARM_REG_FPSCR, fpscr)
        for i, v in enumerate(d):
            uc.reg_write(UC_D[i], v)
        self.too_big = False
        try:
            uc.emu_start(f['start'] | int(f['thumb']), MAGIC, count=UC_LIMIT)
        except UcError as e:
            if self.too_big:
                return {'outcome': 'limit', 'pc': uc.reg_read(U.UC_ARM_REG_PC)}
            return {'outcome': 'trap', 'err': str(e), 'pc': uc.reg_read(U.UC_ARM_REG_PC)}
        if self.too_big:
            return {'outcome': 'limit', 'pc': uc.reg_read(U.UC_ARM_REG_PC)}
        pc = uc.reg_read(U.UC_ARM_REG_PC)
        if pc != MAGIC:
            return {'outcome': 'limit', 'pc': pc}
        return {'outcome': 'ret', 'r': [uc.reg_read(r) for r in UC_R[:14]],
                'fpscr': uc.reg_read(U.UC_ARM_REG_FPSCR), 'd': [uc.reg_read(r) for r in UC_D],
                'written': self.changed()}


def rand_state(rng):
    regs = []
    for i in range(13):
        k = rng.random()
        if k < 0.45:
            regs.append(0x10000000 + rng.randrange(0, 0x4000000) * 4)
        elif k < 0.8:
            regs.append(rng.randrange(0, 40))
        else:
            regs.append(rng.getrandbits(32))
    regs.append(0x30000000 - rng.randrange(0, 256) * 16)        # sp
    regs.append(MAGIC)                                           # lr
    d = [rand_f32(rng) | rand_f32(rng) << 32 if i < 16 and rng.random() < 0.6 else rand_f64(rng)
         for i in range(32)]
    return regs, rng.getrandbits(4) << 28, d


def run_c(dll, k, st):
    regs, fpscr, d = st
    c = CPU()
    for i, v in enumerate(regs):
        c.r[i] = v
    c.fpscr = fpscr
    for i, v in enumerate(d):
        c.d[i] = v
    dll.tm_reset()
    code = dll.run_fn(k, ctypes.byref(c))
    if code:
        return {'outcome': 'trap' if code >= 100 else 'limit' if code == 2 else 'bail%d' % code,
                'site': dll.tm_last_site()}
    buf = WBUF
    n = dll.tm_written(buf, 1 << 20)
    return {'outcome': 'ret', 'r': list(c.r[:14]), 'fpscr': c.fpscr, 'd': list(c.d), 'written': set(buf[:n])}


def compare(dll, ucr, c, u):
    diffs = []
    for i in range(14):
        if i in (9, 12):
            # r9 and r12 are scratch registers in the iOS ARM ABI: a callee may
            # leave them changed, and the C function does not write them back.
            continue
        if c['r'][i] != u['r'][i]:
            diffs.append('r%d c=%08x uc=%08x' % (i, c['r'][i], u['r'][i]))
    if (c['fpscr'] ^ u['fpscr']) & 0xF3F70000:
        diffs.append('fpscr c=%08x uc=%08x' % (c['fpscr'], u['fpscr']))
    for i in range(32):
        if not same_d(c['d'][i], u['d'][i]):
            diffs.append('d%d c=%016x uc=%016x' % (i, c['d'][i], u['d'][i]))
    bad = 0
    nan_words = set()
    for a in sorted(c['written'] | u['written']):
        cb, ub = dll.tm_peek(a), ucr.byte(a)
        if cb != ub:
            w = a & ~3
            cw = sum(dll.tm_peek(w + k) << 8 * k for k in range(4))
            uw = sum(ucr.byte(w + k) << 8 * k for k in range(4))
            if is_nan32(cw) and is_nan32(uw):
                # A NaN made by an operation: x86 gives 0xffc00000, ARM 0x7fc00000.
                nan_words.add(w)
                continue
            if bad < 2:
                diffs.append('mem %08x c=%02x uc=%02x' % (a, cb, ub))
            bad += 1
    if bad > 2:
        diffs.append('... %d bytes differ' % bad)
    if nan_words:
        NAN_ONLY[0] += 1
    return diffs


NAN_ONLY = [0]


def main():
    exe, fjson, work = sys.argv[1], sys.argv[2], sys.argv[3]
    n_max, n_states, seed = arg('--max', 4000), arg('--states', 3), arg('--seed', 1)
    os.makedirs(work, exist_ok=True)
    rng = random.Random(seed)
    t0 = time.time()
    m = MachO(exe)
    data = json.load(open(fjson))
    leaves = leaf_functions(data)
    fns = leaves if len(leaves) <= n_max else sorted(rng.sample(leaves, n_max), key=lambda f: f['start'])
    dll_path, errs = build(m, data, fns, work)
    t1 = time.time()
    dll = ctypes.CDLL(os.path.abspath(dll_path))
    dll.tm_peek.argtypes = [ctypes.c_uint32]
    dll.tm_init.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    dll.run_fn.argtypes = [ctypes.c_int, ctypes.c_void_p]
    dll.tm_written.argtypes = [ctypes.c_void_p, ctypes.c_int]
    dll.tm_last_site.restype = ctypes.c_uint32
    segs = [g for g in m.segments if g.name in ('__TEXT', '__DATA')]
    base = segs[0].vmaddr
    image = bytearray(max(g.vmaddr + g.vmsize for g in segs) - base)
    for g in segs:
        image[g.vmaddr - base:g.vmaddr - base + g.filesize] = m.data[g.fileoff:g.fileoff + g.filesize]
    keep = (ctypes.c_uint8 * len(image)).from_buffer(image)
    dll.tm_init(keep, base, len(image))
    ucr = FnRunner(image, base)
    outcomes = Counter()
    fails = []
    tested_fns = set()
    for k, f in enumerate(fns):
        for _ in range(n_states):
            st = rand_state(rng)
            if '--trace' in sys.argv:
                print('%x uc' % f['start'], flush=True)
            u = ucr.run(f, st)
            if '--trace' in sys.argv:
                print('%x c' % f['start'], flush=True)
            c = run_c(dll, k, st)
            pair = (c['outcome'], u['outcome'])
            if 'limit' in pair or c['outcome'].startswith('bail'):
                outcomes['skipped ' + '/'.join(pair)] += 1
                continue
            if pair == ('trap', 'trap'):
                outcomes['both trap'] += 1
                continue
            if pair != ('ret', 'ret'):
                outcomes['outcome differs'] += 1
                fails.append((f, st, ['outcome c=%s uc=%s %s' % (c['outcome'], u['outcome'],
                                                                   u.get('err', '') or hex(c.get('site', 0)))]))
                continue
            d = compare(dll, ucr, c, u)
            outcomes['compared'] += 1
            tested_fns.add(f['start'])
            if d:
                outcomes['differs'] += 1
                fails.append((f, st, d))
    t2 = time.time()
    lines = ['Stage 2 function test  (%s)' % time.strftime('%Y-%m-%d %H:%M'),
             'leaf functions %d, tested %d (compared at least once %d), states %d, seed %d, codegen errors %d' % (
                 len(leaves), len(fns), len(tested_fns), n_states, seed, errs),
             'time: build %.0f s, run %.0f s' % (t1 - t0, t2 - t1),
             'outcomes: ' + ', '.join('%s %d' % kv for kv in outcomes.most_common()),
             'runs whose memory differed only in the sign of a made NaN: %d' % NAN_ONLY[0],
             'failing functions %d' % len({f['start'] for f, _, _ in fails}), '']
    seenf = Counter()
    for f, st, d in fails:
        seenf[f['start']] += 1
        if seenf[f['start']] > 2:
            continue
        regs, fpscr, _ = st
        lines.append('FAIL %x %s %s' % (f['start'], 'T' if f['thumb'] else 'A', f.get('name') or ''))
        lines.append('   in: r=%s' % ' '.join('%08x' % v for v in regs))
        lines.append('   ' + '; '.join(d[:8]))
    with open(os.path.join(work, 'functest_report.txt'), 'w') as fp:
        fp.write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:6]))


if __name__ == '__main__':
    main()
