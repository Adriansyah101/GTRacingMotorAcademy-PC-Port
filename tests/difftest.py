"""Stage 2 difference tests: the C translation of real game instructions
against Unicorn (Cortex-A8: ARMv7, VFPv3 with short vectors, NEON).

Usage:
  python -I tests/difftest.py <exe> <functions.json> <work_dir>
         [--per-form N] [--random N] [--states N] [--seed N] [--only-key TEXT]

1. picks sample "units" from the code stage 1 found: up to N per instruction
   form (mode, mnemonic, flags, condition, operand shape) plus N random
   ones; an IT instruction and the instructions it covers form one unit;
2. translates each unit with armc.Translator in test mode into tests.c and
   builds difftest.dll (clang, x86_64) with tests/tmem.c as guest memory;
3. runs every unit from several random states in the DLL and in Unicorn and
   compares registers, flags, Q, FPSCR flags, d0-d31, written memory and the
   next pc.
"""
import ctypes
import json
import os
import random
import re
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone import arm as A
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_PROT_ALL
from unicorn import UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_CODE
from unicorn import arm_const as U

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from macho import MachO  # noqa: E402
from armc import Program, Translator, Unsupported, hx, split_mnemonic  # noqa: E402

M32 = 0xFFFFFFFF
SKIP_HEADS = {'trap', 'udf', 'bkpt', 'svc'}
MEM_HEADS = {'ldm', 'ldmib', 'ldmda', 'ldmdb', 'stm', 'stmib', 'stmda', 'stmdb', 'push', 'pop',
             'vldmia', 'vldmdb', 'vstmia', 'vstmdb', 'vpush', 'vpop'}


def arg(name, default):
    if name in sys.argv:
        return type(default)(sys.argv[sys.argv.index(name) + 1])
    return default


def shape_of(ins):
    s = ins.op_str
    s = re.sub(r'\b(r[0-9]+|sb|sl|fp|ip)\b', 'R', s)
    s = re.sub(r'\bs[0-9]+\b', 'S', s)
    s = re.sub(r'\bd[0-9]+\b', 'D', s)
    s = re.sub(r'\bq[0-9]+\b', 'Q', s)
    s = re.sub(r'#0(?![x0-9.])', '#Z', s)
    s = re.sub(r'#-[0-9a-fx.e+-]+', '#-', s)
    s = re.sub(r'#[0-9a-fx.e+-]+', '#', s)
    return s


def form_key(ins, thumb):
    head, s, dts = split_mnemonic(ins)
    cond = ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID)
    return '%s %s%s%s%s %s' % ('T' if thumb else 'A', head, 's' if s else '', ''.join('.' + d for d in dts),
                               '?' if cond else '', shape_of(ins))


# 1. units
def decode_fn(m, md, fn):
    out = []
    for a, e in fn['runs']:
        out.extend(md[fn['thumb']].disasm(m.read(a, e - a), a))
    return out


def units_of(fn, insns):
    """Split a function's instructions into test units (IT blocks together)."""
    units, i = [], 0
    ctl = {s: (k, t) for s, k, t in fn.get('ctl', [])}
    while i < len(insns):
        ins = insns[i]
        head = split_mnemonic(ins)[0]
        if re.fullmatch('it[te]{0,3}', head):
            n = len(head) - 1
            grp = insns[i:i + 1 + n]
            i += 1 + n
        else:
            grp = [ins]
            i += 1
        # Must be contiguous.
        if any(grp[k].address + grp[k].size != grp[k + 1].address for k in range(len(grp) - 1)):
            continue
        heads = [split_mnemonic(x)[0] for x in grp]
        if any(h in SKIP_HEADS for h in heads):
            continue
        table = None
        for x in grp:
            k, t = ctl.get(x.address, (None, None))
            if k == 'table':
                tb = fn['tables'][t]
                if tb['kind'] in ('insn', 'rel'):
                    table = 'skip'
                else:
                    table = t
        if table == 'skip':
            continue
        units.append((grp, table))
    return units


def select_units(m, data, per_form, n_random, rng):
    md = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
    for d in md.values():
        d.detail = True
    seen = Counter()
    chosen = {}                     # key -> list of (fstart, [addrs], table)
    total = 0
    pool = []
    for fn in data['functions']:
        insns = decode_fn(m, md, fn)
        vec = any(x.mnemonic.startswith('vmsr') for x in insns)
        for grp, table in units_of(fn, insns):
            key = ' | '.join(form_key(x, fn['thumb']) for x in grp)
            seen[key] += 1
            item = (fn['start'], [x.address for x in grp], table)
            if vec:
                # VFP short-vector functions: every instruction (FPSCR.LEN > 1 inside).
                pool.append((key, item))
            lst = chosen.setdefault(key, [])
            if len(lst) < per_form:
                lst.append(item)
            else:
                j = rng.randrange(seen[key])
                if j < per_form:
                    lst[j] = item
            total += 1
            if rng.random() < n_random / 1.85e6:
                pool.append((key, item))
    units = [(k, it) for k, lst in chosen.items() for it in lst]
    picked = {(it[0], it[1][0]) for _, it in units}
    units += [(k, it) for k, it in pool if (it[0], it[1][0]) not in picked]
    return units, seen, total


# 2. C source
def gen_tests(m, data, units):
    prog = Program(m, data)
    fns = {f['start']: f for f in data['functions']}
    md = {False: Cs(CS_ARCH_ARM, CS_MODE_ARM), True: Cs(CS_ARCH_ARM, CS_MODE_THUMB)}
    for d in md.values():
        d.detail = True
    by_fn = defaultdict(list)
    for i, (key, (fs, addrs, table)) in enumerate(units):
        by_fn[fs].append((addrs[0], i))
    src = ['#include "arm_rt.h"', '#define EXPORT __declspec(dllexport)', '']
    meta = {}
    skipped = Counter()
    for fs, lst in by_fn.items():
        fn = fns[fs]
        insns = decode_fn(m, md, fn)
        at = {x.address: k for k, x in enumerate(insns)}
        vec = any(x.mnemonic.startswith('vmsr') for x in insns)
        tr = Translator(prog, fn, test=True)
        pos = 0
        for a0, i in sorted(lst):
            if vec:
                # FPSCR (vector length) is tracked along the function.
                while pos < at[a0]:
                    try:
                        tr.translate(insns[pos])
                    except Unsupported:
                        pass
                    pos += 1
            else:
                tr = Translator(prog, fn, test=True)
            key, (_, addrs, table) = units[i]
            state = (tr.vec_len, tr.vec_stride, tr.fz, tr.dn)
            body = []
            try:
                for a in addrs:
                    lines, _ = tr.translate(insns[at[a]])
                    body += lines
            except Unsupported as e:
                skipped[str(e)[:60]] += 1
                continue
            if vec:
                pos = at[addrs[-1]] + 1
            last = insns[at[addrs[-1]]]
            nxt = (last.address + last.size) | int(fn['thumb'])
            src.append('static void T_%d(CPU *c) {' % i)
            src.append('  LOCALS_IN(); NF = c->n; ZF = c->z; CF = c->c; VF = c->v;')
            src.append('  u32 NEXT = %s;' % hx(nxt))
            src += ['  ' + x for x in body]
            src.append('  goto out;')
            src.append('out:;')
            src.append('  c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->r[4] = R4; c->r[5] = R5;')
            src.append('  c->r[6] = R6; c->r[7] = R7; c->r[8] = R8; c->r[9] = R9; c->r[10] = R10;')
            src.append('  c->r[11] = R11; c->r[12] = R12; c->r[13] = SP; c->r[14] = LR;')
            src.append('  c->n = NF; c->z = ZF; c->c = CF; c->v = VF; c->r[15] = NEXT;')
            src.append('}')
            tinfo = None
            if table is not None:
                tinfo = table_regs(fn, fn['tables'][table], insns[at[addrs[-1]]] if True else None,
                                   [insns[at[a]] for a in addrs])
            meta[i] = {'key': key, 'fn': fs, 'addrs': addrs, 'thumb': fn['thumb'],
                       'count': sum(1 for a in addrs
                                    if not re.fullmatch('it[te]{0,3}', split_mnemonic(insns[at[a]])[0])),
                       'text': ' ; '.join('%s %s' % (insns[at[a]].mnemonic, insns[at[a]].op_str) for a in addrs),
                       'state': state, 'table': tinfo,
                       'mem': any('[' in insns[at[a]].op_str or split_mnemonic(insns[at[a]])[0] in MEM_HEADS
                                  for a in addrs),
                       'align16': any(insns[at[a]].mnemonic.startswith(('vld1', 'vst1')) for a in addrs)}
    ids = sorted(meta)
    src.append('typedef void (*TestFn)(CPU *);')
    src.append('static const TestFn tests[] = {%s};' % ', '.join('T_%d' % i for i in ids))
    src.append('EXPORT int test_count(void) { return %d; }' % len(ids))
    src.append('EXPORT void run_test(int k, CPU *c) { tests[k](c); }')
    return '\n'.join(src) + '\n', [meta[i] for i in ids], skipped


def table_regs(fn, tb, last, grp):
    """Registers to set so a table jump takes a real entry: (index_reg, choices, base_reg, base)."""
    ins = next(x for x in grp if x.address == tb['site'])
    ops = ins.operands
    choices = [k for k, t in enumerate(tb['targets']) if t is not None]
    if tb['kind'] in ('tbb', 'tbh'):
        return {'idx': ins.reg_name(ops[0].mem.index), 'choices': choices}
    if tb['kind'] == 'ldr':
        mem = ops[1].mem
        base = None if ins.reg_name(mem.base) == 'pc' else ins.reg_name(mem.base)
        return {'idx': ins.reg_name(mem.index), 'choices': choices, 'base': base, 'base_val': tb['base']}
    if tb['kind'] == 'addpc':
        return {'idx': ins.reg_name(ops[2].reg), 'choices': choices}
    return None


# 3. run
class CPU(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 16), ('n', ctypes.c_uint32), ('z', ctypes.c_uint32),
                ('c', ctypes.c_uint32), ('v', ctypes.c_uint32), ('q', ctypes.c_uint32),
                ('ge', ctypes.c_uint32), ('fpscr', ctypes.c_uint32), ('resume_pc', ctypes.c_uint32),
                ('d', ctypes.c_uint64 * 32), ('rt', ctypes.c_void_p)]


REGNUM = {'r%d' % i: i for i in range(13)}
REGNUM.update({'sb': 9, 'sl': 10, 'fp': 11, 'ip': 12, 'sp': 13, 'lr': 14})
UC_R = [getattr(U, 'UC_ARM_REG_R%d' % i) for i in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]
UC_D = [getattr(U, 'UC_ARM_REG_D%d' % i) for i in range(32)]

FLOATS = [0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-40, -1e-40, float('inf'), float('-inf'), 3e9, -3e9,
          2147483648.0, -2147483648.0, 2147483520.0, 4294967296.0, 4294967040.0, 1.5, -2.5, 255.5,
          65535.0, 1e30, -1e30, 1e-30, 0.1, 100.0]


def rand32(rng):
    k = rng.random()
    if k < 0.15:
        return rng.choice([0, 1, 2, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFFFF, 0x10000, 31, 32, 33, 0xFF,
                           0x100, 0xFFFFFF00, 0x8000, 0x7FFF])
    if k < 0.35:
        return rng.randrange(0, 64)
    if k < 0.45:
        return (-rng.randrange(1, 64)) & M32
    return rng.getrandbits(32)


def rand_f32(rng):
    k = rng.random()
    if k < 0.3:
        return struct.unpack('<I', struct.pack('<f', rng.choice(FLOATS)))[0]
    if k < 0.35:
        return rng.choice([0x7FC00000, 0xFFC00000, 0x7F800001, 0x00000001, 0x80000001])
    if k < 0.8:
        return struct.unpack('<I', struct.pack('<f', rng.uniform(-1e4, 1e4)))[0]
    if k < 0.9:
        return struct.unpack('<I', struct.pack('<f', float(rng.randrange(-100000, 100000))))[0]
    return rng.getrandbits(32)


def rand_f64(rng):
    k = rng.random()
    if k < 0.3:
        return struct.unpack('<Q', struct.pack('<d', rng.choice(FLOATS)))[0]
    if k < 0.8:
        return struct.unpack('<Q', struct.pack('<d', rng.uniform(-1e6, 1e6)))[0]
    if k < 0.9:
        return struct.unpack('<Q', struct.pack('<d', float(rng.randrange(-10 ** 9, 10 ** 9))))[0]
    return rng.getrandbits(64)


def rand_state(rng, meta):
    regs = [rand32(rng) for _ in range(15)]
    if meta['mem']:
        al = 15 if meta['align16'] else 3
        # Keep addresses away from the ends of the address space (no wrap).
        regs = [v & ~al & 0x3FFFFFFF for v in regs]
        regs = [v | 0x10000 if v < 0x10000 else v for v in regs]
    t = meta['table']
    if t:
        regs[REGNUM[t['idx']]] = rng.choice(t['choices'])
        if t.get('base'):
            regs[REGNUM[t['base']]] = t['base_val']
    flags = [rng.getrandbits(1) for _ in range(4)]
    q = rng.getrandbits(1)
    ln, stride, fz, dn = meta['state']
    fpscr = (rng.getrandbits(4) << 28) | ((ln - 1) << 16) | ((3 if stride == 2 else 0) << 20) | \
        (int(fz) << 24) | (int(dn) << 25)
    d = []
    for i in range(32):
        if i < 16 and rng.random() < 0.6:
            d.append(rand_f32(rng) | rand_f32(rng) << 32)
        else:
            d.append(rand_f64(rng))
    return regs, flags, q, fpscr, d


def pat_page(p):
    base = (p << 12) >> 2
    return struct.pack('<1024I', *[((base + i) * 0x9E3779B1) & M32 for i in range(1024)])


class UcRunner:
    def __init__(self, image, img_base):
        self.img, self.base = image, img_base
        uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(U.UC_CPU_ARM_CORTEX_A8)
        size = (len(image) + 0xFFF) & ~0xFFF
        uc.mem_map(img_base, size, UC_PROT_ALL)
        uc.mem_write(img_base, bytes(image))
        uc.reg_write(U.UC_ARM_REG_C1_C0_2, uc.reg_read(U.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(U.UC_ARM_REG_FPEXC, 0x40000000)
        self.uc = uc
        self.dyn = []
        self.writes = []
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.on_unmapped)
        uc.hook_add(UC_HOOK_CODE, self.on_code)
        self.img_end = img_base + size
        self.unit, self.done = set(), set()
        self.snap, self.unit_writes = None, []

    def state(self, pc=None):
        uc = self.uc
        cpsr = uc.reg_read(U.UC_ARM_REG_CPSR)
        if pc is None:
            pc = uc.reg_read(U.UC_ARM_REG_PC)
        return {'r': [uc.reg_read(r) for r in UC_R], 'flags': [(cpsr >> b) & 1 for b in (31, 30, 29, 28)],
                'q': (cpsr >> 27) & 1, 'fpscr': uc.reg_read(U.UC_ARM_REG_FPSCR),
                'd': [uc.reg_read(r) for r in UC_D], 'next': pc | ((cpsr >> 5) & 1)}

    def on_code(self, uc, addr, size, user):
        # Before the first instruction outside the unit (or a second visit):
        # take the state there. Unicorn may run on a little after emu_stop
        # when the unit is an IT block that branches; that part is ignored.
        if (addr not in self.unit or addr in self.done) and self.snap is None:
            self.snap = self.state(addr)
            uc.emu_stop()
        self.done.add(addr)

    def on_write(self, uc, access, addr, size, value, user):
        self.writes.append((addr, size))
        if self.snap is None:
            self.unit_writes.append((addr, size))

    def on_unmapped(self, uc, access, addr, size, value, user):
        for p in {addr >> 12, (addr + max(size, 1) - 1) >> 12}:
            if (p << 12) in self.dyn:
                continue
            uc.mem_map(p << 12, 0x1000, UC_PROT_ALL)
            uc.mem_write(p << 12, pat_page(p))
            self.dyn.append(p << 12)
        return True

    def reset(self):
        for addr, size in self.writes:
            lo, hi = max(addr, self.base), min(addr + size, self.base + len(self.img))
            if lo < hi:
                self.uc.mem_write(lo, bytes(self.img[lo - self.base:hi - self.base]))
        for p in self.dyn:
            self.uc.mem_unmap(p, 0x1000)
        self.dyn, self.writes = [], []

    def byte(self, a):
        try:
            return self.uc.mem_read(a, 1)[0]
        except UcError:
            if self.base <= a < self.base + len(self.img):
                return self.img[a - self.base]
            return pat_page(a >> 12)[a & 0xFFF]

    def run(self, meta, st):
        uc = self.uc
        self.reset()
        regs, flags, q, fpscr, d = st
        for i, v in enumerate(regs):
            uc.reg_write(UC_R[i], v)
        cpsr = uc.reg_read(U.UC_ARM_REG_CPSR) & 0x1F
        cpsr |= flags[0] << 31 | flags[1] << 30 | flags[2] << 29 | flags[3] << 28 | q << 27
        if meta['thumb']:
            cpsr |= 0x20
        uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
        uc.reg_write(U.UC_ARM_REG_FPSCR, fpscr)
        for i, v in enumerate(d):
            uc.reg_write(UC_D[i], v)
        start = meta['addrs'][0] | int(meta['thumb'])
        err = None
        try:
            # Unicorn's instruction count is unreliable in IT blocks; the code
            # hook stops it when execution leaves the unit.
            self.unit, self.done = set(meta['addrs']), set()
            self.snap, self.unit_writes = None, []
            uc.emu_start(start, 0xFFFFFFFE, count=64)
        except UcError as e:
            err = str(e)
        res = self.snap
        if res is None:
            res = self.state()
        else:
            err = None                          # an error after leaving the unit does not count
        res['err'] = err
        res['written'] = {a + k for a, s in self.unit_writes for k in range(s)}
        return res


def run_c(dll, k, st):
    regs, flags, q, fpscr, d = st
    c = CPU()
    for i, v in enumerate(regs):
        c.r[i] = v
    c.n, c.z, c.c, c.v = flags
    c.q, c.fpscr = q, fpscr
    for i, v in enumerate(d):
        c.d[i] = v
    dll.tm_reset()
    dll.run_test(k, ctypes.byref(c))
    buf = (ctypes.c_uint32 * 8192)()
    n = dll.tm_written(buf, 8192)
    return {'r': list(c.r[:15]), 'flags': [c.n, c.z, c.c, c.v], 'q': c.q, 'fpscr': c.fpscr,
            'd': list(c.d), 'next': c.r[15], 'written': set(buf[:n])}


def is_nan32(x):
    return (x & 0x7F800000) == 0x7F800000 and x & 0x007FFFFF


def is_nan64(x):
    return (x >> 52 & 0x7FF) == 0x7FF and x & ((1 << 52) - 1)


def same_d(a, b):
    if a == b:
        return True
    if '--strict-nan' in sys.argv:
        return False
    if is_nan64(a) and is_nan64(b):
        return True
    for sh in (0, 32):
        x, y = a >> sh & M32, b >> sh & M32
        if x != y and not (is_nan32(x) and is_nan32(y)):
            return False
    return True


def norm_next(v):
    return v & ~1 | 1 if v & 1 else v & ~3


def compare(dll, ucr, meta, cres, ures):
    diffs = []
    for i in range(15):
        if cres['r'][i] != ures['r'][i]:
            diffs.append('r%d c=%08x uc=%08x' % (i, cres['r'][i], ures['r'][i]))
    if cres['flags'] != ures['flags']:
        diffs.append('nzcv c=%s uc=%s' % (cres['flags'], ures['flags']))
    if cres['q'] != ures['q']:
        diffs.append('q c=%d uc=%d' % (cres['q'], ures['q']))
    if (cres['fpscr'] ^ ures['fpscr']) & 0xF0000000:
        diffs.append('fpscr c=%08x uc=%08x' % (cres['fpscr'], ures['fpscr']))
    for i in range(32):
        if not same_d(cres['d'][i], ures['d'][i]):
            diffs.append('d%d c=%016x uc=%016x' % (i, cres['d'][i], ures['d'][i]))
    if norm_next(cres['next']) != norm_next(ures['next']):
        diffs.append('next c=%08x uc=%08x' % (cres['next'], ures['next']))
    for a in sorted(cres['written'] | ures['written']):
        if a >= 0xFFFF0000:
            continue                            # wrapped addresses: Unicorn's top page is unreliable
        cb, ub = dll.tm_peek(a), ucr.byte(a)
        if cb != ub:
            diffs.append('mem %08x c=%02x uc=%02x' % (a, cb, ub))
            break
    return diffs


def main():
    exe, fjson, work = sys.argv[1], sys.argv[2], sys.argv[3]
    per_form, n_random = arg('--per-form', 4), arg('--random', 3000)
    n_states, seed = arg('--states', 4), arg('--seed', 1)
    only = arg('--only-key', '')
    os.makedirs(work, exist_ok=True)
    rng = random.Random(seed)
    t0 = time.time()
    m = MachO(exe)
    data = json.load(open(fjson))
    units, seen, total = select_units(m, data, per_form, n_random, rng)
    if only:
        units = [u for u in units if only in u[0]]
    t1 = time.time()
    src, metas, skipped = gen_tests(m, data, units)
    with open(os.path.join(work, 'tests.c'), 'w', newline='\n') as fp:
        fp.write(src)
    dll_path = os.path.join(work, 'difftest.dll')
    cmd = ['clang', '--target=x86_64-w64-mingw32', '-shared', '-O1', '-DARM_TEST_MEM', '-fno-strict-aliasing',
           '-ffp-contract=off', '-fwrapv', '-w', '-I', os.path.join(ROOT, 'runtime'),
           os.path.join(work, 'tests.c'), os.path.join(HERE, 'tmem.c'), '-o', dll_path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        print(p.stderr[:5000])
        sys.exit(1)
    t2 = time.time()
    dll = ctypes.CDLL(os.path.abspath(dll_path))
    dll.tm_peek.restype = ctypes.c_int
    dll.tm_peek.argtypes = [ctypes.c_uint32]
    dll.tm_init.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
    dll.run_test.argtypes = [ctypes.c_int, ctypes.c_void_p]
    dll.tm_written.argtypes = [ctypes.c_void_p, ctypes.c_int]
    # Guest image: __TEXT and __DATA at their addresses (zero-fill included).
    segs = [g for g in m.segments if g.name in ('__TEXT', '__DATA')]
    img_base = segs[0].vmaddr
    img_end = max(g.vmaddr + g.vmsize for g in segs)
    image = bytearray(img_end - img_base)
    for g in segs:
        image[g.vmaddr - img_base:g.vmaddr - img_base + g.filesize] = m.data[g.fileoff:g.fileoff + g.filesize]
    keep = (ctypes.c_uint8 * len(image)).from_buffer(image)
    dll.tm_init(keep, img_base, len(image))
    ucr = UcRunner(image, img_base)
    fails = defaultdict(list)
    uc_errors = Counter()
    runs = 0
    for k, meta in enumerate(metas):
        for _ in range(n_states):
            st = rand_state(rng, meta)
            ures = ucr.run(meta, st)
            if ures['err']:
                uc_errors[meta['key'] + ' :: ' + ures['err']] += 1
                continue
            cres = run_c(dll, k, st)
            runs += 1
            d = compare(dll, ucr, meta, cres, ures)
            if d:
                fails[meta['key']].append((meta, st, d))
    t3 = time.time()
    lines = ['Stage 2 difference test  (%s)' % time.strftime('%Y-%m-%d %H:%M'),
             'units in code %d, forms %d; tested units %d, runs %d, states per unit %d, seed %d' % (
                 total, len(seen), len(metas), runs, n_states, seed),
             'time: select %.0f s, build %.0f s, run %.0f s' % (t1 - t0, t2 - t1, t3 - t2),
             'failing forms %d (failing runs %d), Unicorn errors %d, untranslatable %d' % (
                 len(fails), sum(len(v) for v in fails.values()), sum(uc_errors.values()),
                 sum(skipped.values())),
             '']
    covered = sum(seen[k] for k in {mm['key'] for mm in metas})
    lines.insert(2, 'instruction units covered by a tested form: %d of %d (%.2f%%)' % (
        covered, total, 100.0 * covered / max(total, 1)))
    for key, lst in sorted(fails.items(), key=lambda kv: -seen[kv[0]]):
        lines.append('FAIL %s   (%d in code, %d failing runs)' % (key, seen[key], len(lst)))
        for meta, st, d in lst[:2]:
            regs, flags, q, fpscr, dd = st
            lines.append('   %x: %s' % (meta['addrs'][0], meta['text']))
            lines.append('      in: r=%s nzcv=%s fpscr=%08x' % (' '.join('%08x' % v for v in regs),
                                                               ''.join(map(str, flags)), fpscr))
            lines.append('      ' + '; '.join(d[:6]))
    if uc_errors:
        lines.append('')
        lines.append('Unicorn errors:')
        for k, n in uc_errors.most_common(40):
            lines.append('  %4d %s' % (n, k))
    if skipped:
        lines.append('')
        lines.append('not translatable in test mode:')
        for k, n in skipped.most_common():
            lines.append('  %4d %s' % (n, k))
    with open(os.path.join(work, 'difftest_report.txt'), 'w') as fp:
        fp.write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:7]))


if __name__ == '__main__':
    main()
