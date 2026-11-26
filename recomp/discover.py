import json
import os
import sys
import time
from collections import Counter, defaultdict

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from capstone import arm as A

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import MachO  # noqa: E402

CC_AL = A.ARM_CC_AL
PC, LR, SP = A.ARM_REG_PC, A.ARM_REG_LR, A.ARM_REG_SP

LOAD_SIZE = {
    A.ARM_INS_LDR: 4, A.ARM_INS_LDRB: 1, A.ARM_INS_LDRSB: 1, A.ARM_INS_LDRH: 2,
    A.ARM_INS_LDRSH: 2, A.ARM_INS_LDRD: 8, A.ARM_INS_PLD: 0, A.ARM_INS_PLI: 0,
}
ARM_NOPS = {b'\x00\x00\xa0\xe1', b'\x00\xf0\x20\xe3'}
THUMB_NOPS = {b'\x00\xbf', b'\xc0\x46'}
NORETURN_HINT = ('abort', 'exit', '_exit', '__cxa_throw', '__cxa_rethrow',
                 '_Unwind_SjLj_Resume', '__stack_chk_fail', '__assert_rtn',
                 'longjmp', 'siglongjmp', 'objc_exception_throw', '__cxa_call_unexpected',
                 'pthread_exit', '__cxa_bad_cast', '__cxa_pure_virtual')


def is_noreturn_import(name):
    if not name:
        return False
    n = name.lstrip('_')
    return n in NORETURN_IMPORTS or (n.startswith('ZSt') and '__throw_' in n)


NORETURN_IMPORTS = {
    'abort', 'exit', 'Exit', 'cxa_throw', 'cxa_rethrow', 'Unwind_SjLj_Resume',
    'stack_chk_fail', 'assert_rtn', 'longjmp', 'siglongjmp', 'objc_exception_throw',
    'cxa_call_unexpected', 'pthread_exit', 'cxa_bad_cast', 'cxa_bad_typeid',
    'objc_terminate', 'ZSt9terminatev', 'cxa_pure_virtual',
}


class Func:
    __slots__ = ('start', 'end', 'thumb', 'insns', 'blocks', 'calls', 'imports',
                 'icalls', 'ijumps', 'rets', 'tables', 'literals', 'taken', 'tails',
                 'flags', 'refs', 'cand', 'after_uncond', 'datarefs', 'walked', 'preds', 'ctl')

    def __init__(self, start, end, thumb):
        self.cand = {}
        self.after_uncond = {}
        self.datarefs = set()
        self.walked = False
        self.preds = {}
        self.start, self.end, self.thumb = start, end, thumb
        self.insns = {}
        self.blocks = set()
        self.calls = []
        self.imports = []
        self.icalls = []
        self.ijumps = []
        self.rets = 0
        self.tables = []
        self.literals = {}
        self.taken = set()
        self.tails = []
        self.flags = []
        self.refs = []
        self.ctl = {}

class Discover:
    def __init__(self, path):
        self.m = MachO(path)
        m = self.m
        text = m.section('__TEXT', '__text')
        self.text_lo, self.text_hi = text.addr, text.end
        stub = m.section('__TEXT', '__symbol_stub4')
        self.stub_lo, self.stub_hi = stub.addr, stub.end
        self.starts = sorted(m.function_starts)
        self.mode = {a: t for a, t in self.starts}
        self.md = {}
        for thumb in (False, True):
            md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
            md.detail = True
            self.md[thumb] = md
        self.funcs = []
        self.func_at = {}

    def run(self):
        self.data_words = self.collect_data_words()
        self.added = {}
        self.rounds = []
        starts = dict(self.starts)
        self.noreturn = set()
        for rnd in range(40):
            self.build(starts)
            for f in self.funcs:
                self.walk(f)
            self.update_noreturn()
            new = self.find_new_starts()
            self.rounds.append(len(new))
            if not new:
                break
            for a, (t, why) in new.items():
                starts[a] = t
                self.added[a] = (t, why)
        self.check_calls()
        self.check_consistency()
        self.scan_data_pointers()

    def update_noreturn(self):
        while True:
            nr = {f.start for f in self.funcs
                  if f.insns and f.rets == 0 and not f.tails and not f.ijumps}
            new = nr - self.noreturn
            if not new:
                return
            self.noreturn |= new
            for i, f in enumerate(self.funcs):
                if any(t in new for _, t, _ in f.calls):
                    g = Func(f.start, f.end, f.thumb)
                    self.funcs[i] = g
                    self.func_at[g.start] = g
                    self.walk(g)

    def build(self, starts):
        old = {(f.start, f.end, f.thumb): f for f in self.funcs}
        self.funcs, self.func_at = [], {}
        order = sorted(starts.items())
        bounds = [a for a, _ in order[1:]] + [self.text_hi]
        for (a, t), e in zip(order, bounds):
            e = min(e, self.text_hi)
            f = old.get((a, e, t)) or Func(a, e, t)
            self.funcs.append(f)
            self.func_at[a] = f
        self.func_keys = [f.start for f in self.funcs]

    def containing(self, a):
        import bisect
        i = bisect.bisect_right(self.func_keys, a) - 1
        return self.funcs[i] if i >= 0 else None

    def collect_data_words(self):
        m = self.m
        out = []
        for s in m.sections:
            if s.name in ('__text', '__symbol_stub4', '__stub_helper', '__cstring',
                          '__objc_methname', '__objc_classname', '__objc_methtype',
                          '__ustring', '__gcc_except_tab') or s.type == 1:
                continue
            raw = m.read(s.addr, s.size - s.size % 4)
            for i in range(0, len(raw), 4):
                v = int.from_bytes(raw[i:i + 4], 'little')
                if self.text_lo <= (v & ~1) < self.text_hi:
                    out.append((s.seg + ',' + s.name, s.addr + i, v))
        return out

    def gap_starts(self):
        out = {}
        self.gap_ranges = []
        for f in self.funcs:
            _, _, un = self.coverage(f)
            for a, n in un:
                end = a + n
                while a < end:
                    if not f.thumb:
                        if a % 4 == 0 and a + 4 <= end and \
                                (self.m.read(a, 4) in ARM_NOPS or self.m.read(a, 4) == b'\0' * 4):
                            a += 4
                        elif a % 4 and self.m.read(a, 2) == b'\0\0':
                            a += 2
                        else:
                            break
                    elif self.m.read(a, 2) in THUMB_NOPS or self.m.read(a, 2) == b'\0\0':
                        a += 2
                    else:
                        break
                if a < end:
                    out[a] = f
                    self.gap_ranges.append((a, end))
        self.gap_ranges.sort()
        return out

    def in_gap(self, t):
        import bisect
        i = bisect.bisect_right(self.gap_ranges, (t, 0xFFFFFFFF)) - 1
        return i >= 0 and self.gap_ranges[i][0] <= t < self.gap_ranges[i][1]

    def looks_like_entry(self, a, thumb, end):
        md = self.md[thumb]
        n = 0
        for ins in md.disasm(self.m.read(a, min(end - a, 64)), a):
            if n == 0 and ins.id in (A.ARM_INS_PUSH, A.ARM_INS_STMDB) and \
                    any(op.type == A.ARM_OP_REG and op.reg == LR for op in ins.operands):
                return 'prologue'
            n += 1
            if n >= 6:
                return 'decodes'
        return None

    def find_new_starts(self):
        gaps = self.gap_starts()
        new = {}
        for f in self.funcs:
            for site, t, tthumb in f.calls:
                if t not in self.func_at and self.text_lo <= t < self.text_hi:
                    g = self.containing(t)
                    if t not in g.insns:
                        new[t] = (tthumb, 'call')
            for site, t, cond, tthumb in f.tails:
                if t not in self.func_at and self.text_lo <= t < self.text_hi:
                    g = self.containing(t)
                    if t not in g.insns and (t in gaps or self.in_gap(t)):
                        new.setdefault(t, (tthumb, 'tail'))
                    elif t in g.insns and g.thumb == tthumb:
                        new.setdefault(t, (tthumb, 'split'))
        for sec, at, v in self.data_words:
            a = v & ~1
            if a in self.func_at or a in new:
                continue
            g = gaps.get(a)
            if g is not None and self.looks_like_entry(a, bool(v & 1), g.end):
                new[a] = (bool(v & 1), 'data ' + sec)
        for a, g in gaps.items():
            if a in new or a in self.func_at:
                continue
            for thumb in (g.thumb, not g.thumb):
                if thumb is False and a % 4:
                    continue
                if self.looks_like_entry(a, thumb, g.end) == 'prologue':
                    new[a] = (thumb, 'prologue')
                    break
        return new

    def pc_value(self, f, addr):
        return addr + (4 if f.thumb else 8)

    def walk(self, f):
        if f.walked:
            return
        f.walked = True
        work = [f.start]
        f.blocks.add(f.start)
        while True:
            while work:
                x = work.pop()
                if x in f.insns:
                    continue
                self.decode_run(f, x, work)
            for t, bit in list(f.cand.items()):
                skip = f.after_uncond.get(t)
                if (f.thumb and bit & 1) or bit & 2 or (skip is not None and 0 < skip - t <= 8):
                    del f.cand[t]
                    f.taken.add(t)
                    if t not in f.insns:
                        f.blocks.add(t)
                        work.append(t)
            if not work:
                break
        f.datarefs = set(f.cand)
        f.cand = {}

    def decode_run(self, f, x, work):
        md = self.md[f.thumb]
        code = self.m.read(x, f.end - x)
        addr = x
        regs = {}
        last = None
        hist = []
        for ins in md.disasm(code, x):
            addr = ins.address
            if addr in f.insns:
                f.blocks.add(addr)
                return
            f.insns[addr] = ins.size
            hist.append(ins)
            last = ins
            stop = self.classify(f, ins, work, regs, hist)
            addr = ins.address + ins.size
            if stop:
                return
            self.track(f, ins, regs)
            if addr >= f.end:
                break
        if addr >= f.end:
            site = last.address if last else x
            if addr == f.end and f.end in self.func_at:
                f.tails.append((site, f.end, False, f.thumb))
                f.flags.append(('fallthrough', site, '%x' % f.end))
                return
            name = ''
            if last is not None and last.id in (A.ARM_INS_BL, A.ARM_INS_BLX):
                name = self.callee_name(last)
            f.flags.append(('falls_off_end', site, name))
        else:
            f.flags.append(('bad_decode', addr, ''))

    def callee_name(self, ins):
        op = ins.operands[0]
        if op.type != A.ARM_OP_IMM:
            return 'reg'
        t = op.imm & 0xFFFFFFFF
        if t in self.m.stubs:
            return self.m.stubs[t] or 'local-stub'
        return self.m.names.get(t, 'sub_%x' % t)

    def track(self, f, ins, regs):
        M = 0xFFFFFFFF
        iid, ops = ins.id, ins.operands
        _, written = ins.regs_access()
        cond = ins.cc not in (CC_AL, A.ARM_CC_INVALID)
        pcv = self.pc_value(f, ins.address)
        new = None
        d = ops[0].reg if ops and ops[0].type == A.ARM_OP_REG else None

        def val(op):
            if op.type == A.ARM_OP_IMM:
                return ('c', op.imm & M)
            if op.type != A.ARM_OP_REG:
                return None
            v = ('c', pcv) if op.reg == PC else regs.get(op.reg)
            st, sv = op.shift.type, op.shift.value
            if st == 0:
                return v if v is not None else ('s', op.reg, 0)
            if st == A.ARM_SFT_LSL:
                if v is not None and v[0] == 'c':
                    return ('c', (v[1] << sv) & M)
                if v is None or (v[0] == 's' and v[2] == 0):
                    return ('s', op.reg, sv)
            return None

        if not cond and d is not None:
            if iid in (A.ARM_INS_MOV, A.ARM_INS_MOVS) and len(ops) == 2:
                new = val(ops[1])
                if new is not None and new[0] == 's' and new[2] == 0 and ops[1].reg != d:
                    new = regs.get(ops[1].reg)
            elif iid == A.ARM_INS_MOVW:
                new = ('c', ops[1].imm & 0xFFFF)
            elif iid == A.ARM_INS_MOVT:
                v = regs.get(d)
                if v is not None and v[0] == 'c':
                    new = ('c', (v[1] & 0xFFFF) | ((ops[1].imm & 0xFFFF) << 16))
            elif iid == A.ARM_INS_MVN and ops[1].type == A.ARM_OP_IMM:
                new = ('c', ~ops[1].imm & M)
            elif iid == A.ARM_INS_ORR and ops[-1].type == A.ARM_OP_IMM:
                v = regs.get(ops[1].reg if len(ops) == 3 else d)
                if v is not None and v[0] == 'c':
                    new = ('c', v[1] | (ops[-1].imm & M))
            elif iid == A.ARM_INS_ADR:
                new = ('c', ((pcv & ~3) + ops[1].imm) & M)
            elif iid in (A.ARM_INS_ADD, A.ARM_INS_ADDW, A.ARM_INS_SUB, A.ARM_INS_SUBW,
                         A.ARM_INS_SUBS) and len(ops) in (2, 3):
                a, b = (ops[1], ops[2]) if len(ops) == 3 else (ops[0], ops[1])
                sub = iid in (A.ARM_INS_SUB, A.ARM_INS_SUBW, A.ARM_INS_SUBS)
                if a.type == A.ARM_OP_REG and a.reg == PC and b.type == A.ARM_OP_IMM:
                    base = pcv & ~3 if f.thumb else pcv
                    new = ('c', (base - b.imm if sub else base + b.imm) & M)
                else:
                    va = val(a) if len(ops) == 3 else (regs.get(d) or ('s', d, 0))
                    vb = val(b)
                    if va and vb:
                        if va[0] == 'c' and vb[0] == 'c':
                            new = ('c', (va[1] - vb[1] if sub else va[1] + vb[1]) & M)
                        elif not sub and va[0] == 'c' and vb[0] == 's':
                            new = ('i', va[1], vb[1], vb[2])
                        elif not sub and vb[0] == 'c' and va[0] == 's':
                            new = ('i', vb[1], va[1], va[2])
            elif iid == A.ARM_INS_LSL and len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
                v = regs.get(ops[1].reg)
                if v is not None and v[0] == 'c':
                    new = ('c', (v[1] << ops[2].imm) & M)
                elif v is None:
                    new = ('s', ops[1].reg if ops[1].reg != d else None, ops[2].imm)
            elif iid == A.ARM_INS_LDR and len(ops) >= 2 and ops[1].type == A.ARM_OP_MEM and not ins.writeback:
                mem = ops[1].mem
                if mem.base == PC and mem.index == 0:
                    lit = (pcv & ~3) + mem.disp
                    if self.in_file(lit):
                        new = ('c', self.m.u32(lit))
                elif mem.index:
                    vb, vi = regs.get(mem.base), regs.get(mem.index)
                    if vb is not None and vb[0] == 'c' and vi is None:
                        new = ('l', vb[1], mem.index, mem.lshift)
                    elif vi is not None and vi[0] == 'c' and (vb is None or vb[0] == 's'):
                        new = ('l', vi[1], vb[1] if vb else mem.base, vb[2] if vb else 0)
        size = LOAD_SIZE.get(iid)
        if size and len(ops) >= 2 and ops[1].type == A.ARM_OP_MEM:
            mem = ops[1].mem
            vb = ('c', pcv) if mem.base == PC else regs.get(mem.base)
            vi = regs.get(mem.index) if mem.index else ('c', 0)
            if vb and vi and vb[0] == 'c' and vi[0] == 'c' and (mem.index or mem.base != PC):
                a = (vb[1] + (vi[1] << mem.lshift) + (0 if mem.index else mem.disp)) & M
                if f.start <= a < f.end:
                    f.literals[a] = max(size, f.literals.get(a, 0))
        for r in written:
            regs.pop(r, None)
        if iid in (A.ARM_INS_BL, A.ARM_INS_BLX):
            for r in (A.ARM_REG_R0, A.ARM_REG_R1, A.ARM_REG_R2, A.ARM_REG_R3, A.ARM_REG_R12, LR):
                regs.pop(r, None)
        if new is not None and d is not None and d != PC:
            regs[d] = new
            if new[0] == 'c' and iid not in (A.ARM_INS_MOVW, A.ARM_INS_LDR, A.ARM_INS_ADR) and \
                    not (iid in (A.ARM_INS_ADD, A.ARM_INS_SUB) and not f.thumb and len(ops) == 3 and
                         ops[1].type == A.ARM_OP_REG and ops[1].reg == PC and ops[2].type == A.ARM_OP_IMM):
                pic = iid == A.ARM_INS_ADD and any(
                    o.type == A.ARM_OP_REG and o.reg == PC for o in ops[1:]) and \
                    ops[-1].type == A.ARM_OP_REG
                self.note_value(f, ins.address, new[1], pic=pic)

    def jump_const(self, f, ins, v, interwork, cond, work):
        t = v & ~1
        thumb = bool(v & 1) if interwork else f.thumb
        if f.start <= t < f.end and thumb == f.thumb:
            self.branch(f, ins, t, cond, work)
            f.ctl[ins.address] = ('br', t)
        else:
            f.tails.append((ins.address, t, cond, thumb))
            f.ctl[ins.address] = ('tail', t)

    def classify(self, f, ins, work, regs, hist):
        iid = ins.id
        cond = ins.cc not in (CC_AL, A.ARM_CC_INVALID)
        ops = ins.operands
        _, written = ins.regs_access()
        pcw = PC in written

        for op in ops:
            if op.type == A.ARM_OP_MEM and op.mem.base == PC and op.mem.index == 0:
                lit = (self.pc_value(f, ins.address) & ~3) + op.mem.disp
                size = LOAD_SIZE.get(iid)
                if iid in (A.ARM_INS_VLDR,):
                    size = 8 if ins.reg_name(ops[0].reg).startswith('d') else 4
                if size is None:
                    f.flags.append(('pc_mem_unknown', ins.address, ins.mnemonic))
                elif size:
                    f.literals[lit] = max(size, f.literals.get(lit, 0))
                    if size == 4 and self.in_file(lit):
                        self.note_value(f, ins.address, self.m.u32(lit))
        if iid == A.ARM_INS_ADR:
            tgt = (self.pc_value(f, ins.address) & ~3) + ops[1].imm
            self.note_value(f, ins.address, tgt, adr=True)

        if iid in (A.ARM_INS_BL, A.ARM_INS_BLX) and ops[0].type == A.ARM_OP_IMM:
            t = ops[0].imm & 0xFFFFFFFF
            tthumb = f.thumb if iid == A.ARM_INS_BL else not f.thumb
            if self.stub_lo <= t < self.stub_hi:
                name = self.m.stubs.get(t)
                f.imports.append((ins.address, name))
                if tthumb:
                    f.flags.append(('stub_called_as_thumb', ins.address, ''))
                if is_noreturn_import(name):
                    return not cond
            else:
                f.calls.append((ins.address, t, tthumb))
                if t in self.noreturn:
                    return not cond
            return False
        if iid == A.ARM_INS_BLX:
            f.icalls.append(ins.address)
            return False
        if iid in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
            self.branch(f, ins, ops[1].imm & 0xFFFFFFFF, True, work)
            return False
        if iid == A.ARM_INS_B:
            self.branch(f, ins, ops[0].imm & 0xFFFFFFFF, cond, work)
            return not cond
        if iid == A.ARM_INS_BX:
            v = regs.get(ops[0].reg)
            if ops[0].reg == LR and v is None:
                f.rets += 1
                f.ctl[ins.address] = ('ret', None)
            elif v is not None and v[0] == 'c':
                self.jump_const(f, ins, v[1], True, cond, work)
            else:
                f.ijumps.append((ins.address, ins.mnemonic + ' ' + ins.op_str))
            return not cond
        if iid in (A.ARM_INS_TBB, A.ARM_INS_TBH):
            self.thumb_table(f, ins, hist, work)
            return True
        if iid in (A.ARM_INS_TRAP, A.ARM_INS_UDF, A.ARM_INS_BKPT):
            return not cond
        if iid in (A.ARM_INS_ADD, A.ARM_INS_SUB) and not f.thumb and len(ops) == 3 and \
                ops[1].type == A.ARM_OP_REG and ops[1].reg == PC and ops[2].type == A.ARM_OP_IMM:
            v = self.pc_value(f, ins.address)
            v = v + ops[2].imm if iid == A.ARM_INS_ADD else v - ops[2].imm
            v &= 0xFFFFFFFF
            if pcw:
                self.branch(f, ins, v, cond, work)
                f.ctl[ins.address] = ('br', v)
                return not cond
            self.note_value(f, ins.address, v, adr=True)
            return False
        if not pcw:
            return False

        text = ins.mnemonic + ' ' + ins.op_str
        if iid in (A.ARM_INS_POP, A.ARM_INS_LDM) or \
                (iid == A.ARM_INS_LDR and ops[1].mem.base == SP):
            if iid == A.ARM_INS_LDM and ops[0].reg != SP:
                f.ijumps.append((ins.address, text))
            else:
                f.rets += 1
                f.ctl[ins.address] = ('ret', None)
            return not cond
        if iid == A.ARM_INS_MOV and ops[1].type == A.ARM_OP_REG and ops[1].reg == LR \
                and LR not in regs:
            f.rets += 1
            f.ctl[ins.address] = ('ret', None)
            return not cond
        if iid == A.ARM_INS_MOV and ops[1].type == A.ARM_OP_REG and ops[1].shift.type == 0:
            v = regs.get(ops[1].reg)
            if v is not None and v[0] == 'c':
                self.jump_const(f, ins, v[1], not f.thumb, cond, work)
                return not cond
            if v is not None and v[0] == 'i' and v[3] == 2:
                self.insn_table(f, ins, hist, v, work)
                return not cond
        if iid == A.ARM_INS_LDR and ops[1].type == A.ARM_OP_MEM and ops[1].mem.index:
            self.ldr_table(f, ins, hist, work, regs)
            return not cond
        if iid == A.ARM_INS_ADD and not f.thumb and len(ops) == 3 and \
                ops[1].type == A.ARM_OP_REG and ops[1].reg == PC and ops[2].type == A.ARM_OP_REG:
            self.add_table(f, ins, hist, work)
            return not cond
        if iid == A.ARM_INS_ADD and len(ops) == 3 and ops[1].type == A.ARM_OP_REG and \
                ops[2].type == A.ARM_OP_REG and ops[2].shift.type == 0:
            if self.rel_table(f, ins, hist, work, regs):
                return not cond
        f.ijumps.append((ins.address, text))
        return not cond

    def in_file(self, a):
        return self.m.file_offset(a) is not None and self.m.file_offset(a + 3) is not None

    def note_value(self, f, site, v, adr=False, pic=False):
        a = v & ~1
        if not (self.text_lo <= a < self.text_hi):
            return
        if f.start <= a < f.end and a != f.start and a not in f.taken:
            f.cand[a] = f.cand.get(a, 0) | (v & 1) | (2 if pic else 0)
        f.refs.append((site, v))

    def branch(self, f, ins, t, cond, work):
        if f.start <= t < f.end:
            f.blocks.add(t)
            work.append(t)
            f.preds.setdefault(t, []).append(ins.address)
            if not cond:
                f.after_uncond[ins.address + ins.size] = t
            if cond:
                nxt = ins.address + ins.size
                f.blocks.add(nxt)
        else:
            f.tails.append((ins.address, t, cond, f.thumb))

    def back_from(self, f, a, n):
        md = self.md[f.thumb]
        back = []
        for _ in range(n):
            cand = [a - k for k in (2, 4) if a - k in f.insns and f.insns[a - k] == k]
            if not cand:
                break
            a = cand[-1] if f.thumb else cand[0]
            ins = next(md.disasm(self.m.read(a, f.insns[a]), a), None)
            if ins is None:
                break
            back.append(ins)
        return list(reversed(back))

    def prev_insns(self, f, hist, n=20):
        prev = hist[:-1][-n:]
        if len(prev) >= n:
            return prev
        first = prev[0].address if prev else hist[-1].address
        return self.back_from(f, first, n - len(prev)) + prev

    def cmp_imm(self, prev, i):
        ops = prev[i].operands
        if ops[1].type == A.ARM_OP_IMM:
            return ops[1].imm
        if ops[1].type == A.ARM_OP_REG:
            r = ops[1].reg
            for p in reversed(prev[:i]):
                po = p.operands
                if p.id in (A.ARM_INS_MOV, A.ARM_INS_MOVW) and po and po[0].type == A.ARM_OP_REG \
                        and po[0].reg == r and po[1].type == A.ARM_OP_IMM:
                    return po[1].imm
                _, w = p.regs_access()
                if r in w:
                    return None
        return None

    def bound_in(self, prev, reg, last_ls):
        for i in range(len(prev) - 1, -1, -1):
            p = prev[i]
            ops = p.operands
            if p.id == A.ARM_INS_CMP and ops[0].type == A.ARM_OP_REG and ops[0].reg == reg:
                n = self.cmp_imm(prev, i)
                if n is not None:
                    return n + 1
            _, w = p.regs_access()
            if reg is not None and reg in w:
                break
        seen_hi = last_ls
        for i in range(len(prev) - 1, -1, -1):
            p = prev[i]
            if p.cc in (A.ARM_CC_HI, A.ARM_CC_LS):
                seen_hi = True
            if p.id == A.ARM_INS_CMP and seen_hi:
                n = self.cmp_imm(prev, i)
                if n is not None:
                    return n + 1
        return None

    def bound_from_cmp(self, f, hist, reg):
        n = self.bound_in(self.prev_insns(f, hist), reg, hist[-1].cc == A.ARM_CC_LS)
        if n is not None:
            return n
        md = self.md[f.thumb]
        for site in f.preds.get(hist[0].address, []):
            br = next(md.disasm(self.m.read(site, f.insns.get(site, 4)), site), None)
            if br is None:
                continue
            prev = self.back_from(f, site, 10) + [br]
            n = self.bound_in(prev, None, br.cc == A.ARM_CC_LS)
            if n is not None:
                return n
        return None

    def insn_table(self, f, ins, hist, v, work):
        base = v[1]
        n = self.bound_from_cmp(f, hist, v[2])
        if n is None:
            f.flags.append(('table_no_bound', ins.address, 'insn'))
            return
        targets = [base + 4 * i for i in range(n)]
        self.add_targets(f, ins, 'insn', base, 0, targets, work)

    def rel_table(self, f, ins, hist, work, regs):
        ra, rb = ins.operands[1].reg, ins.operands[2].reg
        prev = self.prev_insns(f, hist)
        base = None
        for r in (rb, ra):
            v = regs.get(r)
            if v is not None and v[0] == 'c' and f.start <= v[1] < f.end:
                base, breg = v[1], r
                break
        for p in ([] if base is not None else reversed(prev)):
            ops = p.operands
            if len(ops) == 3 and p.id in (A.ARM_INS_ADD,) and ops[1].type == A.ARM_OP_REG and \
                    ops[1].reg == PC and ops[2].type == A.ARM_OP_IMM and ops[0].reg in (ra, rb):
                base = self.pc_value(f, p.address) + ops[2].imm
                breg = ops[0].reg
                break
            if p.id == A.ARM_INS_ADR and ops[0].reg in (ra, rb):
                base = (self.pc_value(f, p.address) & ~3) + ops[1].imm
                breg = ops[0].reg
                break
        if base is None:
            return False
        entry = ra if breg == rb else rb
        v = regs.get(entry)
        idx = v[2] if v is not None and v[0] == 'l' and v[2] is not None else entry
        n = self.bound_from_cmp(f, hist, idx)
        if n is None:
            f.flags.append(('table_no_bound', ins.address, 'rel'))
            return True
        words = [self.m.u32(base + 4 * i) for i in range(n)]
        targets = [(base + (w if w < 0x80000000 else w - 0x100000000)) & 0xFFFFFFFF for w in words]
        f.literals[base] = 4 * n
        f.cand.pop(base, None)
        self.add_targets(f, ins, 'rel', base, 4 * n, targets, work)
        return True

    def add_targets(self, f, ins, kind, base, size, targets, work):
        bad = [t for t in targets if t is not None and not (f.start <= t < f.end)]
        if bad:
            f.flags.append(('table_entry_dropped', ins.address, '%d' % len(bad)))
            targets = [t if t is not None and f.start <= t < f.end else None for t in targets]
        f.tables.append({'site': ins.address, 'kind': kind, 'base': base,
                         'size': size, 'targets': targets})
        f.ctl[ins.address] = ('table', len(f.tables) - 1)
        for t in targets:
            if t is not None:
                f.blocks.add(t)
                work.append(t)

    def thumb_table(self, f, ins, hist, work):
        mem = ins.operands[0].mem
        if mem.base != PC:
            f.ijumps.append((ins.address, ins.mnemonic + ' ' + ins.op_str))
            return
        n = self.bound_from_cmp(f, hist, mem.index)
        base = ins.address + 4
        if n is None:
            f.flags.append(('table_no_bound', ins.address, ins.mnemonic))
            return
        esz = 1 if ins.id == A.ARM_INS_TBB else 2
        raw = self.m.read(base, n * esz)
        vals = list(raw) if esz == 1 else [raw[i] | raw[i + 1] << 8 for i in range(0, len(raw), 2)]
        targets = [base + 2 * v for v in vals]
        k = n
        while True:
            after = [t for t in targets[:k] if t > base]
            lim = min(n, (min(after) - base) // esz) if after else n
            if lim >= k:
                break
            k = lim
        if k < n:
            f.flags.append(('table_entry_dropped', ins.address, '%d' % (n - k)))
            targets = targets[:k] + [None] * (n - k)
        size = (k * esz + 1) & ~1
        f.literals[base] = size
        self.add_targets(f, ins, ins.mnemonic, base, size, targets, work)

    def ldr_table(self, f, ins, hist, work, regs):
        mem = ins.operands[1].mem
        n = self.bound_from_cmp(f, hist, mem.index)
        if mem.base == PC:
            base = self.pc_value(f, ins.address)
        else:
            v = regs.get(mem.base)
            base = v[1] if v is not None and v[0] == 'c' else None
        if base is None or n is None or mem.lshift != 2:
            f.ijumps.append((ins.address, ins.mnemonic + ' ' + ins.op_str))
            return
        targets = [self.m.u32(base + 4 * i) & ~1 for i in range(n)]
        f.literals[base] = 4 * n
        self.add_targets(f, ins, 'ldr', base, 4 * n, targets, work)
        f.cand.pop(base, None)

    def add_table(self, f, ins, hist, work):
        op = ins.operands[2]
        n = self.bound_from_cmp(f, hist, op.reg)
        if n is None or op.shift.value != 2:
            f.ijumps.append((ins.address, ins.mnemonic + ' ' + ins.op_str))
            return
        base = ins.address + 8
        targets = [base + 4 * i for i in range(n)]
        self.add_targets(f, ins, 'addpc', base, 4 * n, targets, work)

    def check_calls(self):
        for f in self.funcs:
            for site, t, tthumb in f.calls:
                g = self.func_at.get(t)
                if g is None:
                    f.flags.append(('call_not_start', site, '%x' % t))
                elif g.thumb != tthumb:
                    f.flags.append(('call_mode_mismatch', site, '%x' % t))
            for site, t, cond, tthumb in f.tails:
                g = self.func_at.get(t)
                if self.stub_lo <= t < self.stub_hi:
                    continue
                if g is None:
                    f.flags.append(('branch_into_other', site, '%x' % t))
                elif g.thumb != tthumb:
                    f.flags.append(('tail_mode_mismatch', site, '%x' % t))
            for site, v in f.refs:
                a = v & ~1
                g = self.func_at.get(a)
                if g is not None and a != f.start and g.thumb != bool(v & 1):
                    f.flags.append(('ref_mode_mismatch', site, '%x' % v))

    def check_consistency(self):
        for f in self.funcs:
            owner = {}
            for a, n in f.insns.items():
                for b in range(a, a + n):
                    if b in owner:
                        f.flags.append(('insn_overlap', a, '%x' % owner[b]))
                        break
                    owner[b] = a
            run_as_code = {t['base'] for t in f.tables if t['kind'] in ('tbb', 'tbh')
                           and t['base'] in t['targets']}
            for a, n in f.literals.items():
                if a in run_as_code:
                    continue
                for b in range(a, a + n):
                    if b in owner:
                        f.flags.append(('data_overlaps_code', a, '%x' % owner[b]))
                        break
            for t in f.tables:
                for x in t['targets']:
                    if x is not None and x not in f.insns:
                        f.flags.append(('table_target_not_insn', t['site'], '%x' % x))

    def scan_data_pointers(self):
        m = self.m
        self.data_ptrs = Counter()
        self.data_bad = []
        for s in m.sections:
            if s.name in ('__text', '__symbol_stub4', '__stub_helper', '__cstring',
                          '__objc_methname', '__objc_classname', '__objc_methtype',
                          '__ustring', '__gcc_except_tab') or s.type == 1:
                continue
            raw = m.read(s.addr, s.size - s.size % 4)
            for i in range(0, len(raw), 4):
                v = int.from_bytes(raw[i:i + 4], 'little')
                a = v & ~1
                if not (self.text_lo <= a < self.text_hi):
                    continue
                g = self.func_at.get(a)
                if g is None:
                    self.data_ptrs['not_start'] += 1
                    self.data_bad.append((s.seg + ',' + s.name, s.addr + i, v))
                elif g.thumb == bool(v & 1):
                    self.data_ptrs['ok'] += 1
                else:
                    self.data_ptrs['mode_mismatch'] += 1
                    self.data_bad.append((s.seg + ',' + s.name, s.addr + i, v))

    def coverage(self, f):
        covered = bytearray(f.end - f.start)
        for a, n in f.insns.items():
            covered[a - f.start:a - f.start + n] = b'\1' * n
        for a, n in f.literals.items():
            if f.start <= a < f.end:
                lo, hi = a - f.start, min(a - f.start + n, len(covered))
                covered[lo:hi] = b'\2' * (hi - lo)
        raw = self.m.read(f.start, f.end - f.start)
        gaps = []
        i = 0
        while i < len(covered):
            if covered[i]:
                i += 1
                continue
            j = i
            while j < len(covered) and not covered[j]:
                j += 1
            gaps.append((i, j))
            i = j
        pad = unreached = 0
        un = []
        self.last_dataref_bytes = 0
        for i, j in gaps:
            chunk = raw[i:j]
            g = f.start + i
            if self.is_padding(chunk, f.thumb, g):
                pad += j - i
            elif any(g <= d < g + 4 for d in f.datarefs):
                self.last_dataref_bytes += j - i
            else:
                unreached += j - i
                un.append((g, j - i))
        return pad, unreached, un

    def is_padding(self, chunk, thumb, addr):
        if not chunk.strip(b'\0'):
            return True
        step = 2 if thumb else 4
        nops = THUMB_NOPS if thumb else ARM_NOPS
        if len(chunk) % step == 0 and all(chunk[k:k + step] in nops for k in range(0, len(chunk), step)):
            return True
        return len(chunk) < 4 and (addr + len(chunk)) % 4 == 0 and not chunk.strip(b'\0\xbf')


def runs_of(f):
    runs = []
    for a in sorted(f.insns):
        n = f.insns[a]
        if runs and runs[-1][1] == a:
            runs[-1][1] = a + n
        else:
            runs.append([a, a + n])
    return runs


def main():
    exe, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    d = Discover(exe)
    d.run()
    t1 = time.time()
    stats = Counter()
    flags = Counter()
    flag_examples = defaultdict(list)
    noret_callees = Counter()
    unreached_funcs = []
    funcs_json = []
    for f in d.funcs:
        pad, un, unlist = d.coverage(f)
        stats['bytes'] += f.end - f.start
        stats['code_bytes'] += sum(f.insns.values())
        stats['insns'] += len(f.insns)
        stats['literal_bytes'] += sum(n for a, n in f.literals.items() if f.start <= a < f.end)
        stats['pad_bytes'] += pad
        stats['adr_data_bytes'] += d.last_dataref_bytes
        stats['unreached_bytes'] += un
        stats['blocks'] += len(f.blocks)
        stats['calls'] += len(f.calls)
        stats['import_calls'] += len(f.imports)
        stats['icalls'] += len(f.icalls)
        stats['tables'] += len(f.tables)
        stats['taken_labels'] += len(f.taken)
        stats['thumb' if f.thumb else 'arm'] += 1
        if un:
            unreached_funcs.append((un, f.start, unlist[:3]))
        for kind, site, extra in f.flags:
            flags[kind] += 1
            if len(flag_examples[kind]) < 12:
                flag_examples[kind].append('%08x %s (fn %08x %s)' % (site, extra, f.start, d.m.names.get(f.start, '')))
            if kind == 'falls_off_end':
                noret_callees[extra] += 1
        for site, text in f.ijumps:
            flags['indirect_jump'] += 1
            if len(flag_examples['indirect_jump']) < 20:
                flag_examples['indirect_jump'].append('%08x %s (fn %08x)' % (site, text, f.start))
        funcs_json.append({
            'start': f.start, 'end': f.end, 'thumb': f.thumb,
            'name': d.m.names.get(f.start),
            'origin': d.added[f.start][1] if f.start in d.added else 'starts',
            'blocks': sorted(f.blocks),
            'taken': sorted(f.taken),
            'datarefs': sorted(f.datarefs),
            'tables': f.tables,
            'literals': sorted([a, n] for a, n in f.literals.items()),
            'calls': [[s, t] for s, t, _ in f.calls],
            'imports': [[s, n] for s, n in f.imports],
            'tails': [[s, t, c, th] for s, t, c, th in f.tails],
            'icalls': f.icalls,
            'ijumps': [s for s, _ in f.ijumps],
            'runs': runs_of(f),
            'ctl': sorted([s, k, t] for s, (k, t) in f.ctl.items()),
            'flags': [[k, s, e] for k, s, e in f.flags],
        })
    m = d.m
    stubs = [{'addr': addr, 'name': name} for addr, name in sorted(m.stubs.items())]
    slots = []
    for slot, name in sorted(m.ptr_slots.items()):
        v = m.u32(slot)
        tgt = v & ~1
        local = tgt in d.func_at and d.func_at[tgt].thumb == bool(v & 1)
        slots.append({'slot': slot, 'name': name, 'local': v if local else None})
    local_by_name = {s['name']: s['local'] for s in slots if s['local'] is not None and s['name']}
    for s in stubs:
        s['local'] = local_by_name.get(s['name'])
    stats['stubs_local'] = sum(1 for s in stubs if s['local'] is not None)
    stats['stubs_import'] = len(stubs) - stats['stubs_local']
    with open(os.path.join(out, 'functions.json'), 'w') as fp:
        json.dump({'exe': os.path.basename(exe), 'entry': d.m.entry,
                   'noreturn': sorted(d.noreturn),
                   'stubs': stubs, 'pointer_slots': slots,
                   'functions': funcs_json}, fp, separators=(',', ':'))
    unreached_funcs.sort(reverse=True)
    lines = []
    w = lines.append
    w('Stage 1 discovery report  (%s)' % time.strftime('%Y-%m-%d %H:%M'))
    w('executable: %s' % exe)
    w('time: walk %.1f s, total %.1f s' % (t1 - t0, time.time() - t0))
    w('functions: %d (ARM %d, Thumb %d)' % (len(d.funcs), stats['arm'], stats['thumb']))
    why = Counter(r.split(' ')[0] + ('/T' if t else '/A') for t, r in d.added.values())
    w('  from LC_FUNCTION_STARTS %d, added %d in %d rounds %s: %s' % (
        len(d.starts), len(d.added), len(d.rounds), d.rounds, dict(why)))
    for k in ('bytes', 'code_bytes', 'literal_bytes', 'adr_data_bytes', 'pad_bytes', 'unreached_bytes', 'insns',
              'blocks', 'calls', 'import_calls', 'icalls', 'tables', 'taken_labels',
              'stubs_local', 'stubs_import'):
        w('  %-16s %d' % (k, stats[k]))
    w('  noreturn functions %d' % len(d.noreturn))
    w('')
    w('data words pointing into __text: %s' % dict(d.data_ptrs))
    for sec, a, v in d.data_bad[:20]:
        w('  %-22s %08x -> %08x' % (sec, a, v))
    w('')
    w('flags:')
    for k, n in flags.most_common():
        w('  %-24s %d' % (k, n))
        for e in flag_examples[k]:
            w('      ' + e)
    w('')
    w('falls_off_end by last callee:')
    for k, n in noret_callees.most_common(40):
        known = any(h in k for h in NORETURN_HINT)
        w('  %5d %s%s' % (n, k, '' if known else '   <-- check'))
    w('')
    w('functions with unreached bytes: %d' % len(unreached_funcs))
    for un, a, ul in unreached_funcs[:40]:
        w('  %08x %-40s %6d  %s' % (a, (d.m.names.get(a) or '')[:40], un,
                                    ' '.join('%08x+%d' % x for x in ul)))
    rep = '\n'.join(lines) + '\n'
    with open(os.path.join(out, 'stage1_report.txt'), 'w') as fp:
        fp.write(rep)
    print(rep[:6000])


if __name__ == '__main__':
    main()
