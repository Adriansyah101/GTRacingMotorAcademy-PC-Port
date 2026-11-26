import re
import struct

from capstone import arm as A

M32 = 0xFFFFFFFF
LOW_START, LOW_END = 0x1000, 0x11000

GPR = {'r%d' % i: 'R%d' % i for i in range(13)}
GPR.update({'sb': 'R9', 'sl': 'R10', 'fp': 'R11', 'ip': 'R12', 'sp': 'SP', 'lr': 'LR', 'pc': 'PC'})
GPR_INDEX = {v: (13 if v == 'SP' else 14 if v == 'LR' else 15 if v == 'PC' else int(v[1:]))
             for v in set(GPR.values())}

COND = {A.ARM_CC_EQ: 'C_EQ', A.ARM_CC_NE: 'C_NE', A.ARM_CC_HS: 'C_HS', A.ARM_CC_LO: 'C_LO',
        A.ARM_CC_MI: 'C_MI', A.ARM_CC_PL: 'C_PL', A.ARM_CC_VS: 'C_VS', A.ARM_CC_VC: 'C_VC',
        A.ARM_CC_HI: 'C_HI', A.ARM_CC_LS: 'C_LS', A.ARM_CC_GE: 'C_GE', A.ARM_CC_LT: 'C_LT',
        A.ARM_CC_GT: 'C_GT', A.ARM_CC_LE: 'C_LE'}
COND_SUFFIX = ('eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le',
               'cs', 'cc')

ALU = {'and', 'eor', 'sub', 'rsb', 'add', 'adc', 'sbc', 'rsc', 'tst', 'teq', 'cmp', 'cmn',
       'orr', 'mov', 'bic', 'mvn', 'orn', 'addw', 'subw', 'adr', 'movw', 'movt'}
SHIFTS = {'lsl', 'lsr', 'asr', 'ror', 'rrx'}
LOGICAL = {'and', 'eor', 'tst', 'teq', 'orr', 'mov', 'bic', 'mvn', 'orn'}


class Unsupported(Exception):
    pass


def split_mnemonic(ins):
    parts = ins.mnemonic.split('.')
    head = parts[0]
    if ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
        for c in COND_SUFFIX:
            if head.endswith(c) and len(head) > 2:
                head = head[:-2]
                break
    dts = [p for p in parts[1:] if p not in ('w', 'n')]
    s = False
    if ins.update_flags and head.endswith('s') and (head[:-1] in ALU or head[:-1] in SHIFTS or
                                                    head[:-1] in ('mul', 'mla', 'umull', 'smull',
                                                                  'umlal', 'smlal')):
        head, s = head[:-1], True
    return head, s, dts


def hx(v):
    return '0x%xu' % (v & M32)


class Program:

    def __init__(self, macho, data):
        self.m = macho
        self.func_thumb = {f['start']: f['thumb'] for f in data['functions']}
        self.noreturn = set(data['noreturn'])
        self.stubs = {s['addr']: (s['name'], s['local']) for s in data['stubs']}
        text = macho.section('__TEXT', '__text')
        self.text_lo, self.text_hi = text.addr, text.end
        seg = next(g for g in macho.segments if g.name == '__TEXT')
        self.ro_lo, self.ro_hi = seg.vmaddr, seg.vmaddr + seg.filesize
        self.imports_used = {}

    def import_cname(self, name):
        c = 'I_' + re.sub(r'[^0-9A-Za-z_]', lambda m: '_%02x' % ord(m.group(0)), name.lstrip('_'))
        self.imports_used[c] = name
        return c

    def read_ro(self, a, n):
        if self.ro_lo <= a and a + n <= self.ro_hi:
            return self.m.read(a, n)
        return None


class Translator:
    def __init__(self, prog, fn, test=False):
        self.p = prog
        self.fn = fn
        self.thumb = fn['thumb']
        self.test = test
        self.ctl = {s: (k, t) for s, k, t in fn.get('ctl', [])}
        self.tables = fn.get('tables', [])
        self.labels = set()
        self.has_sjlj = bool(fn.get('taken'))
        self.datarefs = set(fn.get('datarefs', []))
        self.warn = []
        self.vec_len, self.vec_stride, self.fz, self.dn = 1, 1, False, False
        self.kconst = {}

    def addr_const(self, v):
        if LOW_START <= v < LOW_END and v in self.datarefs:
            return 'LOWADDR(%s)' % hx(v)
        return hx(v)

    def pcval(self, ins):
        return ins.address + (4 if self.thumb else 8)

    def rn(self, ins, reg):
        return GPR[ins.reg_name(reg)]

    def rd_expr(self, ins, reg, align=False):
        name = self.rn(ins, reg)
        if name == 'PC':
            v = self.pcval(ins)
            return hx(v & ~3 if align else v)
        return name

    def internal(self, t):
        return self.fn['start'] <= t < self.fn['end']

    def mode_of(self, t, default=None):
        th = self.p.func_thumb.get(t)
        return default if th is None else th

    def j_goto(self, t, site):
        if self.test:
            return ['NEXT = %s; goto out;' % hx(t | int(self.thumb))]
        self.labels.add(t)
        return ['goto L_%x;' % t]

    def j_exit(self):
        return ['SJLJ_POP();'] if self.has_sjlj else []

    def j_ret(self, value_expr=None):
        if self.test:
            return ['NEXT = %s; goto out;' % (value_expr or 'LR')]
        return self.j_exit() + ['RET_SYNC();', 'return;']

    def callee(self, t):
        if t in self.p.stubs:
            name, local = self.p.stubs[t]
            if local is not None:
                return 'F_%x(c);' % (local & ~1), False
            if name is None:
                raise Unsupported('stub %x without name' % t)
            cname = self.p.import_cname(name)
            if name in INLINE_IMPORTS:
                return '%s(c);' % INLINE_IMPORTS[name], False
            return '%s(c);' % cname, is_noreturn_import(name)
        if t not in self.p.func_thumb:
            raise Unsupported('call to %x: not a function start' % t)
        return 'F_%x(c);' % t, t in self.p.noreturn

    def j_tail(self, t, site, thumb_hint=None):
        if self.test:
            return ['NEXT = %s; goto out;' % hx(t | int(self.thumb))]
        call, _ = self.callee(t)
        return self.j_exit() + ['CALL_PRE(LR);', call, 'return;']

    def j_call(self, ins, t, thumb_target):
        ret = (ins.address + ins.size) | int(self.thumb)
        if self.test:
            return ['LR = %s; NEXT = %s; goto out;' % (hx(ret), hx(t | int(thumb_target)))], False
        stub = self.p.stubs.get(t)
        if stub and stub[1] is None and stub[0] in ('_setjmp', '_sigsetjmp'):
            return ['CALL_PRE(%s);' % hx(ret), 'SYNC_HI_OUT();',
                    'GUEST_SETJMP(%d);' % (stub[0] == '_sigsetjmp')], False
        call, noret = self.callee(t)
        if t in self.p.func_thumb and self.p.func_thumb[t] != thumb_target:
            self.warn.append('%x: call mode mismatch to %x' % (ins.address, t))
        lines = ['CALL_PRE(%s);' % hx(ret), call]
        if noret:
            lines.append('TRAP(%s, TRAP_NORETURN);' % hx(ins.address))
        else:
            lines.append('CALL_POST();')
        return lines, noret

    def j_icall(self, ins, expr):
        ret = (ins.address + ins.size) | int(self.thumb)
        if self.test:
            return ['{ u32 t_ = %s; LR = %s; NEXT = t_; goto out; }' % (expr, hx(ret))]
        return ['{ u32 t_ = %s; CALL_PRE(%s); CALL_IND(t_); CALL_POST(); }' % (expr, hx(ret))]

    def j_table(self, ins, idx):
        t = self.tables[idx]
        kind = t['kind']
        ops = ins.operands
        if kind in ('tbb', 'tbh'):
            sel = self.rd_expr(ins, ops[0].mem.index)
            cases = list(enumerate(t['targets']))
        elif kind == 'ldr':
            sel = self.rd_expr(ins, ops[1].mem.index)
            cases = list(enumerate(t['targets']))
        elif kind == 'addpc':
            sel = self.rd_expr(ins, ops[2].reg)
            cases = list(enumerate(t['targets']))
        elif kind == 'insn':
            sel = '(%s & ~1u)' % self.rd_expr(ins, ops[1].reg)
            cases = [(x, x) for x in t['targets']]
        elif kind == 'rel':
            sel = '((%s + %s) & ~1u)' % (self.rd_expr(ins, ops[1].reg), self.rd_expr(ins, ops[2].reg))
            cases = [(x, x) for x in t['targets']]
        else:
            raise Unsupported('table kind ' + kind)
        out = ['switch (%s) {' % sel]
        seen = set()
        for k, x in cases:
            if x is None or k in seen:
                continue
            seen.add(k)
            if self.test:
                out.append('case %s: NEXT = %s; goto out;' % (hx(k), hx(x | int(self.thumb))))
            else:
                self.labels.add(x)
                out.append('case %s: goto L_%x;' % (hx(k), x))
        if self.test:
            out.append('default: NEXT = 0xdeaddeadu; goto out; }')
        else:
            out.append('default: TRAP(%s, TRAP_TABLE); }' % hx(ins.address))
        return out

    def pc_write(self, ins, value_expr, interwork, pre=()):
        lines = list(pre)
        kind, t = self.ctl.get(ins.address, (None, None))
        if self.test and kind != 'table':
            if interwork:
                return lines + ['NEXT = %s; goto out;' % value_expr]
            return lines + ['NEXT = (%s) | 1u; goto out;' % value_expr if self.thumb else
                            'NEXT = (%s) & ~3u; goto out;' % value_expr]
        if kind == 'ret':
            return lines + self.j_ret(value_expr)
        if kind == 'br':
            return lines + self.j_goto(t, ins.address)
        if kind == 'tail':
            return lines + self.j_tail(t, ins.address)
        if kind == 'table':
            return lines + self.j_table(ins, t)
        self.warn.append('%x: unresolved write to pc: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
        return lines + ['TRAP(%s, TRAP_IJUMP);' % hx(ins.address)]

    def imm_carry(self, ins):
        b = ins.bytes
        if not self.thumb:
            w = struct.unpack('<I', b)[0]
            if (w >> 25) & 7 == 1 and (w >> 8) & 0xF:
                return True
            return False
        if ins.size != 4:
            return False
        hw1, hw2 = struct.unpack('<HH', b)
        if (hw1 & 0xFA00) == 0xF000 and not hw2 & 0x8000:
            imm12 = ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF)
            return (imm12 >> 10) != 0
        return False

    def operand2(self, ins, op, need_carry):
        if op.type == A.ARM_OP_IMM:
            v = op.imm & M32
            carry = None
            if need_carry and self.imm_carry(ins):
                carry = str(v >> 31) + 'u'
            return [], hx(v), carry
        if op.type != A.ARM_OP_REG:
            raise Unsupported('operand type %d' % op.type)
        x = self.rd_expr(ins, op.reg)
        st, sv = op.shift.type, op.shift.value
        return self.shift(ins, x, st, sv, need_carry)

    def shift(self, ins, x, st, sv, need_carry):
        if st == A.ARM_SFT_INVALID or (st == A.ARM_SFT_LSL and sv == 0):
            return [], x, None
        if st == A.ARM_SFT_LSL:
            return [], '(%s << %d)' % (x, sv), '((%s >> %d) & 1)' % (x, 32 - sv)
        if st == A.ARM_SFT_LSR:
            if sv == 32 or sv == 0:
                return [], '0u', '(%s >> 31)' % x
            return [], '(%s >> %d)' % (x, sv), '((%s >> %d) & 1)' % (x, sv - 1)
        if st == A.ARM_SFT_ASR:
            if sv == 32 or sv == 0:
                return [], '(u32)((s32)%s >> 31)' % x, '(%s >> 31)' % x
            return [], '(u32)((s32)%s >> %d)' % (x, sv), '((%s >> %d) & 1)' % (x, sv - 1)
        if st == A.ARM_SFT_ROR:
            return [], 'ror_i(%s, %d)' % (x, sv), '((%s >> %d) & 1)' % (x, sv - 1)
        if st == A.ARM_SFT_RRX:
            return [], '((CF << 31) | (%s >> 1))' % x, '(%s & 1)' % x
        rs = self.rd_expr(ins, sv)
        fn = {A.ARM_SFT_LSL_REG: 'lsl', A.ARM_SFT_LSR_REG: 'lsr', A.ARM_SFT_ASR_REG: 'asr',
              A.ARM_SFT_ROR_REG: 'ror'}.get(st)
        if fn is None:
            raise Unsupported('shift type %d' % st)
        if not need_carry:
            return [], '%s_r(%s, %s)' % (fn, x, rs), None
        return ['u32 sc_ = CF; u32 so_ = %s_rc(%s, %s, &sc_);' % (fn, x, rs)], 'so_', 'sc_'

    def translate(self, ins):
        head, s, dts = split_mnemonic(ins)
        cond = COND.get(ins.cc)
        try:
            body, term = self.dispatch(ins, head, s, dts)
        except Unsupported:
            raise
        except Exception as e:  # noqa: BLE001 - report the instruction
            raise Unsupported('%s %s: %r' % (ins.mnemonic, ins.op_str, e))
        self.track_const(ins, head, cond)
        if cond and body:
            return ['if (%s) {' % cond] + ['  ' + x for x in body] + ['}'], False
        return body, term

    def track_const(self, ins, head, cond):
        if head == 'vmsr':
            return
        _, written = ins.regs_access()
        ops = ins.operands
        new = None
        if not cond and ops and ops[0].type == A.ARM_OP_REG:
            d = ins.reg_name(ops[0].reg)
            last = ops[-1]
            if head in ('mov', 'movw') and len(ops) == 2 and last.type == A.ARM_OP_IMM:
                new = last.imm & M32
            elif head in ('orr', 'bic', 'and', 'movt') and last.type == A.ARM_OP_IMM:
                src = ins.reg_name(ops[1].reg) if len(ops) == 3 else d
                if src in self.kconst:
                    v, i = self.kconst[src], last.imm & M32
                    new = (v | i if head == 'orr' else v & ~i if head == 'bic' else v & i if head == 'and'
                           else (v & 0xFFFF) | (i << 16)) & M32
        for r in written:
            self.kconst.pop(ins.reg_name(r), None)
        if new is not None:
            self.kconst[ins.reg_name(ops[0].reg)] = new

    def dispatch(self, ins, head, s, dts):
        if head in ALU:
            return self.alu(ins, head, s)
        if head in SHIFTS:
            return self.shift_insn(ins, head, s)
        if head in ('ldr', 'ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrd', 'str', 'strb', 'strh', 'strd'):
            return self.ldst(ins, head)
        if head in ('ldm', 'ldmia', 'ldmib', 'ldmda', 'ldmdb', 'stm', 'stmia', 'stmib', 'stmda', 'stmdb',
                    'push', 'pop'):
            return self.ldm(ins, head)
        if head in ('b', 'bl', 'blx', 'bx', 'cbz', 'cbnz', 'tbb', 'tbh'):
            return self.branch(ins, head)
        if head in ('mul', 'mla', 'mls', 'umull', 'smull', 'umlal', 'smlal', 'smmul', 'smmla', 'smmls',
                    'smulbb', 'smulbt', 'smultb', 'smultt', 'smlabb', 'smlabt', 'smlatb', 'smlatt',
                    'smlawb', 'smlawt', 'smulwb', 'smulwt'):
            return self.mul(ins, head, s)
        if head in ('bfi', 'bfc', 'ubfx', 'sbfx', 'uxtb', 'uxth', 'sxtb', 'sxth', 'uxtab', 'uxtah',
                    'sxtab', 'sxtah', 'uxtb16', 'rev', 'rev16', 'revsh', 'pkhbt', 'pkhtb', 'clz',
                    'usat', 'ssat'):
            return self.bits(ins, head)
        if re.fullmatch('it[te]{0,3}', head):
            return [], False
        if head in ('nop', 'hint', 'pld', 'pli', 'yield'):
            return [], False
        if head in ('dmb', 'dsb', 'isb'):
            return ['MEMBAR();'], False
        if head in ('trap', 'udf', 'bkpt'):
            if self.test:
                return ['NEXT = 0xdead0001u; goto out;'], True
            return ['TRAP(%s, TRAP_UDF);' % hx(ins.address)], True
        if head == 'svc':
            imm = ins.operands[0].imm & M32 if ins.operands else 0
            if self.test:
                raise Unsupported('svc in test')
            return ['c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->r[12] = R12;',
                    'rt_svc(c, %s, %s);' % (hx(ins.address), hx(imm)), 'R0 = c->r[0]; R1 = c->r[1];'], False
        if head[0] == 'v':
            return self.vfp(ins, head, dts)
        raise Unsupported('no translation for %s' % ins.mnemonic)

    def alu(self, ins, head, s):
        ops = ins.operands
        if head in ('tst', 'teq', 'cmp', 'cmn'):
            s = True
        need_c = s and head in LOGICAL
        if head in ('mov', 'mvn', 'movw'):
            d, src, n = ops[0], ops[1], None
        elif head == 'movt':
            d = ops[0]
            dv = self.rn(ins, d.reg)
            return ['%s = (%s & 0xFFFFu) | %s;' % (dv, dv, hx((ops[1].imm & 0xFFFF) << 16))], False
        elif head == 'adr':
            d = ops[0]
            v = ((self.pcval(ins) & ~3) + ops[1].imm) & M32
            if self.rn(ins, d.reg) == 'PC':
                raise Unsupported('adr pc')
            return ['%s = %s;' % (self.rn(ins, d.reg), self.addr_const(v))], False
        elif head in ('tst', 'teq', 'cmp', 'cmn'):
            d, n, src = None, ops[0], ops[1]
        elif len(ops) == 3:
            d, n, src = ops
        elif len(ops) == 2:
            d, n, src = ops[0], ops[0], ops[1]
        else:
            raise Unsupported('alu operands')
        pre, b, carry = self.operand2(ins, src, need_c)
        a = None
        if n is not None:
            a = self.rd_expr(ins, n.reg, align=self.thumb and src.type == A.ARM_OP_IMM)
        if head in ('addw', 'subw'):
            head = head[:3]
        expr = {
            'and': lambda: '%s & %s' % (a, b), 'eor': lambda: '%s ^ %s' % (a, b),
            'orr': lambda: '%s | %s' % (a, b), 'bic': lambda: '%s & ~%s' % (a, b),
            'orn': lambda: '%s | ~%s' % (a, b), 'mov': lambda: b, 'movw': lambda: b,
            'mvn': lambda: '~%s' % b, 'add': lambda: '%s + %s' % (a, b), 'sub': lambda: '%s - %s' % (a, b),
            'rsb': lambda: '%s - %s' % (b, a), 'adc': lambda: '%s + %s + CF' % (a, b),
            'sbc': lambda: '%s - %s - !CF' % (a, b), 'rsc': lambda: '%s - %s - !CF' % (b, a),
            'tst': lambda: '%s & %s' % (a, b), 'teq': lambda: '%s ^ %s' % (a, b),
        }
        dv = self.rn(ins, d.reg) if d is not None else None
        lines = list(pre)
        if not s:
            if dv == 'PC':
                return self.pc_alu(ins, head, expr[head](), lines), self.uncond(ins)
            if (n is not None and self.rn(ins, n.reg) == 'PC' and src.type == A.ARM_OP_IMM
                    and head in ('add', 'sub')):
                pv = self.pcval(ins) & ~3 if self.thumb else self.pcval(ins)
                v = (pv + src.imm if head == 'add' else pv - src.imm) & M32
                lines.append('%s = %s;' % (dv, self.addr_const(v)))
                return wrap(lines), False
            lines.append('%s = %s;' % (dv, expr[head]()))
            return wrap(lines), False
        if dv == 'PC':
            raise Unsupported('flag-setting write to pc')
        if head in LOGICAL:
            if carry is not None and carry != 'sc_':
                lines.append('u32 c_ = %s;' % carry)
                carry = 'c_'
            if dv is None:
                lines.append('u32 r_ = %s; SET_NZ(r_);' % expr[head]())
            else:
                lines.append('%s = %s; SET_NZ(%s);' % (dv, expr[head](), dv))
            if carry is not None:
                lines.append('CF = %s;' % carry)
            return wrap(lines), False
        tgt = dv if dv is not None else 'r_'
        mac = {'add': 'ADDS(%s, %s, %s)' % (tgt, a, b), 'adc': 'ADCS(%s, %s, %s)' % (tgt, a, b),
               'sub': 'SUBS(%s, %s, %s)' % (tgt, a, b), 'sbc': 'SBCS(%s, %s, %s)' % (tgt, a, b),
               'rsb': 'SUBS(%s, %s, %s)' % (tgt, b, a), 'rsc': 'SBCS(%s, %s, %s)' % (tgt, b, a),
               'cmp': 'CMP(%s, %s)' % (a, b), 'cmn': 'CMN(%s, %s)' % (a, b)}[head]
        if dv is None and head not in ('cmp', 'cmn'):
            lines.append('u32 r_; %s;' % mac)
        else:
            lines.append(mac + ';')
        return wrap(lines), False

    def pc_alu(self, ins, head, expr, pre):
        interwork = not self.thumb
        return self.pc_write(ins, '(%s)' % expr, interwork, pre)

    def shift_insn(self, ins, head, s):
        ops = ins.operands
        d = self.rn(ins, ops[0].reg)
        if head == 'rrx':
            x = self.rd_expr(ins, ops[1].reg)
            pre, e, carry = self.shift(ins, x, A.ARM_SFT_RRX, 0, s)
        elif len(ops) == 2 and ops[1].type == A.ARM_OP_REG and ops[1].shift.type != A.ARM_SFT_INVALID:
            pre, e, carry = self.operand2(ins, ops[1], s)
        else:
            if len(ops) == 2:
                m, amt = ops[0], ops[1]
            else:
                m, amt = ops[1], ops[2]
            x = self.rd_expr(ins, m.reg)
            if amt.type == A.ARM_OP_IMM:
                st = {'lsl': A.ARM_SFT_LSL, 'lsr': A.ARM_SFT_LSR, 'asr': A.ARM_SFT_ASR,
                      'ror': A.ARM_SFT_ROR}[head]
                if head == 'ror' and amt.imm == 0:
                    raise Unsupported('ror #0')
                pre, e, carry = self.shift(ins, x, st, amt.imm, s)
            else:
                st = {'lsl': A.ARM_SFT_LSL_REG, 'lsr': A.ARM_SFT_LSR_REG, 'asr': A.ARM_SFT_ASR_REG,
                      'ror': A.ARM_SFT_ROR_REG}[head]
                pre, e, carry = self.shift(ins, x, st, amt.reg, s)
        if d == 'PC':
            raise Unsupported('shift into pc')
        lines = list(pre)
        if s and carry is not None and carry != 'sc_':
            lines.append('u32 c_ = %s;' % carry)
            carry = 'c_'
        lines.append('%s = %s;' % (d, e))
        if s:
            lines.append('SET_NZ(%s);' % d)
            if carry is not None:
                lines.append('CF = %s;' % carry)
        return wrap(lines), False

    def mul(self, ins, head, s):
        ops = ins.operands
        r = [self.rd_expr(ins, o.reg) for o in ops]
        dst = [self.rn(ins, o.reg) for o in ops]
        if 'PC' in dst[:1]:
            raise Unsupported('mul into pc')
        if head == 'mul':
            d, n, m = (dst[0], r[1], r[2]) if len(ops) == 3 else (dst[0], r[0], r[1])
            lines = ['%s = %s * %s;' % (d, n, m)]
            if s:
                lines.append('SET_NZ(%s);' % d)
            return wrap(lines), False
        if head in ('mla', 'mls'):
            op = '+' if head == 'mla' else '-'
            lines = ['%s = %s %s %s * %s;' % (dst[0], r[3], op, r[1], r[2])]
            if s:
                lines.append('SET_NZ(%s);' % dst[0])
            return wrap(lines), False
        if head in ('umull', 'smull', 'umlal', 'smlal'):
            cast = '(u64)' if head[0] == 'u' else '(u64)(s64)(s32)'
            mulx = '%s%s * %s%s' % (cast, r[2], cast, r[3]) if head[0] == 'u' else \
                '(u64)((s64)(s32)%s * (s32)%s)' % (r[2], r[3])
            acc = ' + (((u64)%s << 32) | %s)' % (r[1], r[0]) if head.endswith('mlal') else ''
            lines = ['{ u64 p_ = %s%s; %s = (u32)p_; %s = (u32)(p_ >> 32);' % (mulx, acc, dst[0], dst[1])]
            if s:
                lines[0] += ' NF = (u32)(p_ >> 63); ZF = p_ == 0;'
            lines[0] += ' }'
            return lines, False
        if head == 'smmul':
            return ['%s = smmul(%s, %s);' % (dst[0], r[1], r[2])], False
        if head in ('smmla', 'smmls'):
            return ['%s = %s(%s, %s, %s);' % (dst[0], head, r[1], r[2], r[3])], False
        if head.startswith('smul') and head[4:] in ('bb', 'bt', 'tb', 'tt'):
            x, y = head[4], head[5]
            return ['%s = (u32)((s32)%s * (s32)%s);' % (dst[0], half(r[1], x), half(r[2], y))], False
        if head.startswith('smla') and head[4:] in ('bb', 'bt', 'tb', 'tt'):
            x, y = head[4], head[5]
            return ['%s = add_q((u32)((s32)%s * (s32)%s), %s, &c->q);' % (
                dst[0], half(r[1], x), half(r[2], y), r[3])], False
        if head in ('smlawb', 'smlawt'):
            b16 = r[2] if head[-1] == 'b' else '(%s >> 16)' % r[2]
            return ['%s = smlaw(%s, %s, %s, &c->q);' % (dst[0], r[1], b16, r[3])], False
        if head in ('smulwb', 'smulwt'):
            b16 = r[2] if head[-1] == 'b' else '(%s >> 16)' % r[2]
            return ['%s = (u32)(((s64)(s32)%s * (s16)%s) >> 16);' % (dst[0], r[1], b16)], False
        raise Unsupported(head)

    def bits(self, ins, head):
        ops = ins.operands
        d = self.rn(ins, ops[0].reg)
        if d == 'PC':
            raise Unsupported('bits into pc')
        if head == 'bfc':
            lsb, w = ops[1].imm, ops[2].imm
            mask = ((1 << w) - 1) << lsb
            return ['%s &= %s;' % (d, hx(~mask))], False
        if head == 'bfi':
            n = self.rd_expr(ins, ops[1].reg)
            lsb, w = ops[2].imm, ops[3].imm
            mask = (((1 << w) - 1) << lsb) & M32
            return ['%s = (%s & %s) | ((%s << %d) & %s);' % (d, d, hx(~mask), n, lsb, hx(mask))], False
        if head in ('ubfx', 'sbfx'):
            n = self.rd_expr(ins, ops[1].reg)
            lsb, w = ops[2].imm, ops[3].imm
            if head == 'ubfx':
                return ['%s = (%s >> %d) & %s;' % (d, n, lsb, hx((1 << w) - 1))], False
            return ['%s = (u32)((s32)(%s << %d) >> %d);' % (d, n, 32 - lsb - w, 32 - w)], False
        if head in ('uxtb', 'uxth', 'sxtb', 'sxth', 'uxtb16', 'rev', 'rev16', 'revsh', 'clz'):
            m = ops[1]
            x = self.rd_expr(ins, m.reg)
            if m.shift.type == A.ARM_SFT_ROR and m.shift.value:
                x = 'ror_i(%s, %d)' % (x, m.shift.value)
            e = {'uxtb': '(u32)(u8)%s', 'uxth': '(u32)(u16)%s', 'sxtb': '(u32)(s32)(s8)%s',
                 'sxth': '(u32)(s32)(s16)%s', 'uxtb16': '(%s & 0x00FF00FFu)', 'rev': 'rev32(%s)',
                 'rev16': 'rev16(%s)', 'revsh': 'revsh(%s)', 'clz': 'clz32(%s)'}[head] % x
            return ['%s = %s;' % (d, e)], False
        if head in ('uxtab', 'uxtah', 'sxtab', 'sxtah'):
            n = self.rd_expr(ins, ops[1].reg)
            m = ops[2]
            x = self.rd_expr(ins, m.reg)
            if m.shift.type == A.ARM_SFT_ROR and m.shift.value:
                x = 'ror_i(%s, %d)' % (x, m.shift.value)
            e = {'uxtab': '(u32)(u8)%s', 'uxtah': '(u32)(u16)%s', 'sxtab': '(u32)(s32)(s8)%s',
                 'sxtah': '(u32)(s32)(s16)%s'}[head] % x
            return ['%s = %s + %s;' % (d, n, e)], False
        if head in ('pkhbt', 'pkhtb'):
            n = self.rd_expr(ins, ops[1].reg)
            m = ops[2]
            x = self.rd_expr(ins, m.reg)
            sv = m.shift.value if m.shift.type else 0
            if head == 'pkhbt':
                return ['%s = (%s & 0xFFFFu) | ((%s << %d) & 0xFFFF0000u);' % (d, n, x, sv)], False
            sh = '(u32)((s32)%s >> %d)' % (x, sv if sv else 31)
            return ['%s = (%s & 0xFFFF0000u) | (%s & 0xFFFFu);' % (d, n, sh)], False
        if head in ('usat', 'ssat'):
            bits = ops[1].imm
            m = ops[2]
            pre, x, _ = self.operand2(ins, m, False)
            return wrap(pre + ['%s = %s((s32)%s, %d, &c->q);' % (d, head, x, bits)]), False
        raise Unsupported(head)

    MEM_RE = re.compile(r'\[([^\]]*)\](!?)(.*)$')

    def mem_operand(self, ins):
        m = self.MEM_RE.search(ins.op_str)
        if not m:
            raise Unsupported('no memory operand')
        inside = [x.strip() for x in m.group(1).split(',')]
        bang, rest = m.group(2), m.group(3).strip()
        base = inside[0].split(':')[0]
        bname = GPR[base]
        if bname == 'PC':
            bexpr = None
        else:
            bexpr = bname

        def offset(parts):
            if not parts:
                return None
            p = parts[0]
            if p.startswith('#'):
                v = int(p[1:], 0)
                return ('imm', v)
            neg = p.startswith('-')
            r = GPR[p.lstrip('-+')]
            e = r
            if len(parts) > 1:
                sh = parts[1].split()
                st = sh[0]
                sv = int(sh[1][1:], 0) if len(sh) > 1 else 0
                e = {'lsl': '(%s << %d)' % (r, sv), 'lsr': '(%s >> %d)' % (r, sv) if sv < 32 else '0u',
                     'asr': '(u32)((s32)%s >> %d)' % (r, min(sv, 31)), 'ror': 'ror_i(%s, %d)' % (r, sv),
                     'rrx': '((CF << 31) | (%s >> 1))' % r}[st]
            return ('neg' if neg else 'pos', e)

        pre_off = offset(inside[1:])
        post_off = None
        if rest.startswith(','):
            post_off = offset([x.strip() for x in rest[1:].split(',')])

        def add(b, off):
            if off is None:
                return b
            k, v = off
            if k == 'imm':
                return '%s + %s' % (b, hx(v)) if v >= 0 else '%s - %s' % (b, hx(-v))
            return '%s %s %s' % (b, '-' if k == 'neg' else '+', v)

        if bexpr is None:
            pcv = self.pcval(ins)
            if pre_off is not None and pre_off[0] == 'imm' and not bang and post_off is None:
                lit = ((pcv & ~3) + pre_off[1]) & M32
                return hx(lit), 'PC', None, lit
            return add(hx(pcv), pre_off), 'PC', None, None
        if post_off is not None:
            return bexpr, bname, add(bexpr, post_off), None
        addr = add(bexpr, pre_off)
        return addr, bname, ('a_' if bang else None), None

    def ldst(self, ins, head):
        ops = ins.operands
        addr, bname, wb, lit = self.mem_operand(ins)
        size = {'ldr': 4, 'str': 4, 'ldrb': 1, 'strb': 1, 'ldrh': 2, 'strh': 2, 'ldrsb': 1, 'ldrsh': 2,
                'ldrd': 8, 'strd': 8}[head]
        regs = [self.rn(ins, o.reg) for o in ops if o.type == A.ARM_OP_REG]
        load = head.startswith('ld')
        rt = regs[0]
        kind, tix = self.ctl.get(ins.address, (None, None))
        if load and rt == 'PC' and kind == 'table' and not self.test:
            return self.j_table(ins, tix), self.uncond(ins)
        if load and lit is not None:
            raw = self.p.read_ro(lit, size)
            if raw is not None:
                if head == 'ldrd':
                    lo, hi = struct.unpack('<II', raw)
                    return ['%s = %s; %s = %s;' % (regs[0], hx(lo), regs[1], hx(hi))], False
                v = int.from_bytes(raw, 'little')
                if head == 'ldrsb':
                    v = v - 256 if v & 0x80 else v
                elif head == 'ldrsh':
                    v = v - 65536 if v & 0x8000 else v
                if rt == 'PC':
                    return self.pc_write(ins, hx(v), True), self.uncond(ins)
                return ['%s = %s;' % (rt, hx(v))], False
        lines = ['u32 a_ = %s;' % addr]
        ld = {1: 'LD8', 2: 'LD16', 4: 'LD32'}
        st = {1: 'ST8', 2: 'ST16', 4: 'ST32'}
        if load:
            if head == 'ldrd':
                lines.append('u32 v_ = LD32(a_), w_ = LD32(a_ + 4);')
            else:
                e = '%s(a_)' % ld[size]
                if head == 'ldrsb':
                    e = '(u32)(s32)(s8)' + e
                elif head == 'ldrsh':
                    e = '(u32)(s32)(s16)' + e
                elif size < 4:
                    e = '(u32)' + e
                lines.append('u32 v_ = %s;' % e)
            if wb:
                lines.append('%s = %s;' % (bname, wb))
            if rt == 'PC':
                return self.pc_write(ins, 'v_', True, ['{'] + lines) + ['}'], self.uncond(ins)
            if head == 'ldrd':
                lines.append('%s = v_; %s = w_;' % (regs[0], regs[1]))
            else:
                lines.append('%s = v_;' % rt)
        else:
            vals = [self.store_val(ins, r) for r in regs[:2 if head == 'strd' else 1]]
            if head == 'strd':
                lines.append('ST32(a_, %s); ST32(a_ + 4, %s);' % (vals[0], vals[1]))
            else:
                lines.append('%s(a_, %s);' % (st[size], vals[0]))
            if wb:
                lines.append('%s = %s;' % (bname, wb))
        return wrap(lines), False

    def store_val(self, ins, r):
        if r == 'PC':
            return hx(self.pcval(ins))
        return r

    def uncond(self, ins):
        return ins.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID)

    def ldm(self, ins, head):
        ops = ins.operands
        if head in ('push', 'pop'):
            base, regs = 'SP', [self.rn(ins, o.reg) for o in ops]
            wb = True
            mode = 'db' if head == 'push' else 'ia'
            load = head == 'pop'
        else:
            base = self.rn(ins, ops[0].reg)
            regs = [self.rn(ins, o.reg) for o in ops[1:]]
            wb = ins.writeback
            mode = head[3:] or 'ia'
            load = head.startswith('ld')
        n = len(regs)
        start = {'ia': base, 'ib': '%s + 4u' % base, 'da': '%s - %du' % (base, 4 * n - 4),
                 'db': '%s - %du' % (base, 4 * n)}[mode]
        if base == 'PC':
            raise Unsupported('ldm/stm with pc base')
        lines = ['u32 a_ = %s;' % start]
        new_base = '%s %s %du' % (base, '+' if mode in ('ia', 'ib') else '-', 4 * n)
        if load:
            tmps = []
            for i, r in enumerate(regs):
                lines.append('u32 v%d_ = LD32(a_ + %du);' % (i, 4 * i))
                tmps.append(('v%d_' % i, r))
            if wb:
                lines.append('%s = %s;' % (base, new_base))
            pcv = None
            for t, r in tmps:
                if r == 'PC':
                    pcv = t
                else:
                    lines.append('%s = %s;' % (r, t))
            if pcv is not None:
                return self.pc_write(ins, pcv, True, ['{'] + lines) + ['}'], self.uncond(ins)
            return wrap(lines), False
        for i, r in enumerate(regs):
            lines.append('ST32(a_ + %du, %s);' % (4 * i, self.store_val(ins, r)))
        if wb:
            lines.append('%s = %s;' % (base, new_base))
        return wrap(lines), False

    def branch(self, ins, head):
        ops = ins.operands
        cond_uncond = self.uncond(ins)
        if head == 'b':
            t = ops[0].imm & M32
            if self.internal(t):
                return self.j_goto(t, ins.address), cond_uncond
            return self.j_tail(t, ins.address), cond_uncond
        if head in ('bl', 'blx') and ops[0].type == A.ARM_OP_IMM:
            t = ops[0].imm & M32
            tthumb = self.thumb if head == 'bl' else not self.thumb
            lines, noret = self.j_call(ins, t, tthumb)
            return lines, (noret or self.test) and cond_uncond
        if head == 'blx':
            return self.j_icall(ins, self.rd_expr(ins, ops[0].reg)), self.test and cond_uncond
        if head == 'bx':
            r = self.rd_expr(ins, ops[0].reg)
            return self.pc_write(ins, r, True), cond_uncond
        if head in ('cbz', 'cbnz'):
            r = self.rd_expr(ins, ops[0].reg)
            t = ops[1].imm & M32
            jump = self.j_goto(t, ins.address) if self.internal(t) else self.j_tail(t, ins.address)
            return ['if (%s %s 0) { %s }' % (r, '==' if head == 'cbz' else '!=', ' '.join(jump))], False
        if head in ('tbb', 'tbh'):
            kind, t = self.ctl.get(ins.address, (None, None))
            if kind != 'table':
                raise Unsupported('tbb without table')
            return self.j_table(ins, t), True
        raise Unsupported(head)

    def vreg(self, ins, op):
        name = ins.reg_name(op.reg)
        return name[0], int(name[1:])

    def vfp(self, ins, head, dts):
        ops = ins.operands
        if head in ('vldr', 'vstr'):
            return self.vldr(ins, head)
        if head in ('vldmia', 'vldmdb', 'vstmia', 'vstmdb', 'vpush', 'vpop'):
            return self.vldm(ins, head)
        if head in ('vld1', 'vst1'):
            return self.vld1(ins, head, dts)
        if head == 'vmrs':
            if ins.op_str.startswith('apsr_nzcv'):
                return ['VMRS_NZCV();'], False
            return ['%s = c->fpscr;' % self.rn(ins, ops[0].reg)], False
        if head == 'vmsr':
            src = ops[-1].reg
            if ins.reg_name(src) not in GPR or 'fpscr' not in ins.op_str:
                raise Unsupported('vmsr form')
            r = self.rd_expr(ins, src)
            v = self.kconst.get(ins.reg_name(src))
            if v is None:
                self.vec_len, self.vec_stride, self.fz, self.dn = 1, 1, False, False
                self.warn.append('%x: vmsr with unknown value' % ins.address)
            else:
                self.vec_len = ((v >> 16) & 7) + 1
                self.vec_stride = 2 if (v >> 20) & 3 == 3 else 1
                self.fz, self.dn = bool(v >> 24 & 1), bool(v >> 25 & 1)
            return ['c->fpscr = %s;' % r], False
        if head == 'vmov':
            return self.vmov(ins, dts)
        if head == 'vorr':
            kinds = [self.vreg(ins, o) for o in ops]
            if all(k == 'd' for k, _ in kinds) and len(ops) == 3:
                (_, d), (_, n), (_, m) = kinds
                return ['c->V.d[%d] = c->V.d[%d] | c->V.d[%d];' % (d, n, m)], False
            raise Unsupported('vorr form')
        if head in ('vcmp', 'vcmpe'):
            k, a = self.vreg(ins, ops[0])
            ea = self.vf(k, a)
            if len(ops) > 1 and ops[1].type == A.ARM_OP_REG:
                eb = self.vf(*self.vreg(ins, ops[1]))
            else:
                eb = '0.0'
            return ['vfp_cmp(c, %s, %s);' % (ea, eb)], False
        if head == 'vcvt':
            return self.vcvt(ins, dts)
        if head in ('vadd', 'vsub', 'vmul', 'vdiv', 'vnmul', 'vmla', 'vmls', 'vnmla', 'vnmls',
                    'vabs', 'vneg', 'vsqrt', 'vmax', 'vmin'):
            return self.varith(ins, head, dts)
        raise Unsupported('vfp ' + head)

    @staticmethod
    def vf(k, i):
        return 'c->V.f[%d]' % i if k == 's' else 'c->V.df[%d]' % i

    def vldr(self, ins, head):
        k, i = self.vreg(ins, ins.operands[0])
        addr, bname, wb, lit = self.mem_operand(ins)
        if wb:
            raise Unsupported('vldr writeback')
        if head == 'vldr':
            if lit is not None:
                raw = self.p.read_ro(lit, 4 if k == 's' else 8)
                if raw is not None:
                    if k == 's':
                        return ['c->V.s[%d] = %s;' % (i, hx(struct.unpack('<I', raw)[0]))], False
                    return ['c->V.d[%d] = 0x%016xull;' % (i, struct.unpack('<Q', raw)[0])], False
            if k == 's':
                return ['c->V.s[%d] = LD32(%s);' % (i, addr)], False
            return ['c->V.d[%d] = LD64(%s);' % (i, addr)], False
        if k == 's':
            return ['ST32(%s, c->V.s[%d]);' % (addr, i)], False
        return ['ST64(%s, c->V.d[%d]);' % (addr, i)], False

    def vldm(self, ins, head):
        ops = ins.operands
        if head in ('vpush', 'vpop'):
            base, lst, wb = 'SP', ops, True
            mode = 'db' if head == 'vpush' else 'ia'
            load = head == 'vpop'
        else:
            base, lst, wb = self.rn(ins, ops[0].reg), ops[1:], ins.writeback
            mode = head[-2:]
            load = head.startswith('vld')
        regs = [self.vreg(ins, o) for o in lst]
        size = sum(4 if k == 's' else 8 for k, _ in regs)
        lines = ['u32 a_ = %s;' % (base if mode == 'ia' else '%s - %du' % (base, size))]
        off = 0
        for k, i in regs:
            if k == 's':
                lines.append('c->V.s[%d] = LD32(a_ + %du);' % (i, off) if load else
                             'ST32(a_ + %du, c->V.s[%d]);' % (off, i))
                off += 4
            else:
                lines.append('c->V.d[%d] = LD64(a_ + %du);' % (i, off) if load else
                             'ST64(a_ + %du, c->V.d[%d]);' % (off, i))
                off += 8
        if wb:
            lines.append('%s = %s %s %du;' % (base, base, '+' if mode == 'ia' else '-', size))
        return wrap(lines), False

    def vld1(self, ins, head, dts):
        regs = [self.vreg(ins, o) for o in ins.operands if o.type == A.ARM_OP_REG and
                ins.reg_name(o.reg)[0] == 'd']
        m = self.MEM_RE.search(ins.op_str)
        base = GPR[m.group(1).split(':')[0].strip()]
        rest = m.group(3).strip()
        lines = ['u32 a_ = %s;' % base]
        for j, (_, i) in enumerate(regs):
            lines.append('c->V.d[%d] = LD64(a_ + %du);' % (i, 8 * j) if head == 'vld1' else
                         'ST64(a_ + %du, c->V.d[%d]);' % (8 * j, i))
        if m.group(2):
            lines.append('%s = a_ + %du;' % (base, 8 * len(regs)))
        elif rest.startswith(','):
            lines.append('%s = a_ + %s;' % (base, GPR[rest[1:].strip()]))
        return wrap(lines), False

    def vmov(self, ins, dts):
        ops = ins.operands
        shape = ''.join('i' if o.type == A.ARM_OP_IMM else 'f' if o.type == A.ARM_OP_FP else
                        ('r' if ins.reg_name(o.reg) in GPR else ins.reg_name(o.reg)[0]) for o in ops)
        if shape == 'sr':
            return ['c->V.s[%d] = %s;' % (self.vreg(ins, ops[0])[1], self.rd_expr(ins, ops[1].reg))], False
        if shape == 'rs':
            return ['%s = c->V.s[%d];' % (self.rn(ins, ops[0].reg), self.vreg(ins, ops[1])[1])], False
        if shape == 'drr':
            d = self.vreg(ins, ops[0])[1]
            return ['c->V.s[%d] = %s; c->V.s[%d] = %s;' % (2 * d, self.rd_expr(ins, ops[1].reg), 2 * d + 1,
                                                           self.rd_expr(ins, ops[2].reg))], False
        if shape == 'rrd':
            d = self.vreg(ins, ops[2])[1]
            return ['{ u32 lo_ = c->V.s[%d], hi_ = c->V.s[%d]; %s = lo_; %s = hi_; }' % (
                2 * d, 2 * d + 1, self.rn(ins, ops[0].reg), self.rn(ins, ops[1].reg))], False
        if shape == 'ssrr':
            a, b = self.vreg(ins, ops[0])[1], self.vreg(ins, ops[1])[1]
            return ['c->V.s[%d] = %s; c->V.s[%d] = %s;' % (a, self.rd_expr(ins, ops[2].reg), b,
                                                           self.rd_expr(ins, ops[3].reg))], False
        if shape == 'rrss':
            a, b = self.vreg(ins, ops[2])[1], self.vreg(ins, ops[3])[1]
            return ['{ u32 x_ = c->V.s[%d], y_ = c->V.s[%d]; %s = x_; %s = y_; }' % (
                a, b, self.rn(ins, ops[0].reg), self.rn(ins, ops[1].reg))], False
        if dts and dts[0] in ('f32', 'f64'):
            return self.varith(ins, 'vmov', dts)
        if dts and dts[0] in ('i8', 'i16', 'i32', 'i64') and shape in ('di', 'qi'):
            k, i = self.vreg(ins, ops[0])
            v = ops[1].imm & ((1 << 64) - 1)
            bits = int(dts[0][1:])
            mask = (1 << bits) - 1
            word = 0
            for j in range(64 // bits):
                word |= (v & mask) << (j * bits)
            ds = [i] if k == 'd' else [2 * i, 2 * i + 1]
            return ['c->V.d[%d] = 0x%016xull;' % (d, word) for d in ds], False
        if shape == 'dd':
            a, b = self.vreg(ins, ops[0])[1], self.vreg(ins, ops[1])[1]
            return ['c->V.d[%d] = c->V.d[%d];' % (a, b)], False
        raise Unsupported('vmov shape ' + shape)

    def vcvt(self, ins, dts):
        ops = ins.operands
        if len(ops) != 2 or len(dts) != 2:
            raise Unsupported('vcvt form')
        (kd, d), (km, m) = self.vreg(ins, ops[0]), self.vreg(ins, ops[1])
        to, frm = dts
        if kd == 'd' and km == 'd' and to in ('f32', 's32', 'u32') and frm in ('f32', 's32', 'u32'):
            lanes = [(2 * d + j, 2 * m + j) for j in (0, 1)]
            return ['{ u32 x0_ = %s, x1_ = %s; c->V.s[%d] = x0_; c->V.s[%d] = x1_; }' % (
                self.cvt_expr(to, frm, lanes[0][1]), self.cvt_expr(to, frm, lanes[1][1]),
                lanes[0][0], lanes[1][0])], False
        if self.vec_len > 1 and kd == 's' and to == 'f32' and frm == 'f32':
            raise Unsupported('vcvt in vector mode')
        if to == 'f64' and frm == 'f32':
            return ['c->V.df[%d] = (double)c->V.f[%d];' % (d, m)], False
        if to == 'f32' and frm == 'f64':
            return ['c->V.f[%d] = (float)c->V.df[%d];' % (d, m)], False
        if to in ('f32', 'f64') and frm in ('s32', 'u32'):
            cast = '(s32)' if frm == 's32' else ''
            return ['%s = (%s)%sc->V.s[%d];' % (self.vf(kd, d), 'float' if to == 'f32' else 'double',
                                                cast, m)], False
        if to in ('s32', 'u32') and frm in ('f32', 'f64'):
            fn = 'cvt_f2s' if to == 's32' else 'cvt_f2u'
            return ['c->V.s[%d] = %s(%s);' % (d, fn, self.vf(km, m))], False
        raise Unsupported('vcvt %s.%s' % (to, frm))

    @staticmethod
    def cvt_expr(to, frm, lane):
        if frm == 'f32':
            return '%s(c->V.f[%d])' % ('cvt_f2s' if to == 's32' else 'cvt_f2u', lane)
        if to == 'f32':
            return 'f2bits((float)%sc->V.s[%d])' % ('(s32)' if frm == 's32' else '', lane)
        raise Unsupported('cvt lane')

    def varith(self, ins, head, dts):
        ops = ins.operands
        regs = [self.vreg(ins, o) if o.type == A.ARM_OP_REG else None for o in ops]
        dt = dts[0] if dts else 'f32'
        kinds = {r[0] for r in regs if r}
        if dt == 'f32' and kinds == {'d'}:
            return self.neon_f32(ins, head, regs, ops)
        if dt == 'f32' and kinds == {'s'}:
            return self.vfp_scalar(ins, head, regs, ops, 's')
        if dt == 'f64' and kinds == {'d'}:
            return self.vfp_scalar(ins, head, regs, ops, 'd')
        raise Unsupported('varith %s %s' % (head, dt))

    def vfp_scalar(self, ins, head, regs, ops, k):
        imm = None
        if head == 'vmov' and ops[1].type == A.ARM_OP_FP:
            imm = ops[1].fp
        dst = regs[0][1]
        srcs = [r[1] for r in regs[1:] if r]
        ln, stride = self.vec_len, self.vec_stride
        bank = 8 if k == 's' else 4
        if k == 'd' and dst >= 16:
            ln = 1
        if ln > 1 and dst // bank == 0:
            ln = 1
        def elem(r, i, vector):
            if not vector:
                return r
            b = r - r % bank
            return b + (r - b + i * stride) % bank

        vec_m = bool(srcs) and (srcs[-1] // bank != 0)
        out = []
        for i in range(ln):
            d = elem(dst, i, ln > 1)
            if head in ('vabs', 'vneg', 'vsqrt', 'vmov'):
                m = elem(srcs[0], i, ln > 1 and vec_m) if srcs else None
                out.append(self.vstmt(head, k, None, m, d, imm))
            else:
                n = elem(srcs[0], i, ln > 1)
                m = elem(srcs[1], i, ln > 1 and vec_m)
                out.append(self.vstmt(head, k, n, m, d, None))
        return out, False

    def fzwrap(self, e, k):
        if k == 's' and self.fz:
            e = 'ftz(%s)' % e
        if k == 's' and self.dn:
            e = 'dnan(%s)' % e
        return e

    def vstmt(self, head, k, n, m, d, imm):
        bits = 'c->V.s[%d]' if k == 's' else 'c->V.d[%d]'
        if head == 'vmov':
            if imm is not None:
                if k == 's':
                    return '%s = %s;' % (bits % d, hx(struct.unpack('<I', struct.pack('<f', imm))[0]))
                return '%s = 0x%016xull;' % (bits % d, struct.unpack('<Q', struct.pack('<d', imm))[0])
            return '%s = %s;' % (bits % d, bits % m)
        if head in ('vabs', 'vneg'):
            if k == 's':
                op = '& 0x7FFFFFFFu' if head == 'vabs' else '^ 0x80000000u'
            else:
                op = '& 0x7FFFFFFFFFFFFFFFull' if head == 'vabs' else '^ 0x8000000000000000ull'
            return '%s = %s %s;' % (bits % d, bits % m, op)
        f = lambda i: self.vfin(k, i)  # noqa: E731
        fix = 'nanfix_f' if k == 's' else 'nanfix_d'
        zero = '0.0f' if k == 's' else '0.0'
        if head == 'vsqrt':
            e = '%s(%s)' % ('__builtin_sqrtf' if k == 's' else '__builtin_sqrt', f(m))
            e = '%s(%s, %s, %s, %s)' % (fix, e, f(m), zero, zero)
            return '%s = %s;' % (self.vf(k, d), self.fzwrap(e, k))
        inputs = [f(n), f(m)] + ([f(d)] if head in ('vmla', 'vmls', 'vnmla', 'vnmls') else [zero])
        e = '%s(%s, %s)' % (fix, self.vexpr(head, k, n, m, d), ', '.join(inputs))
        return '%s = %s;' % (self.vf(k, d), self.fzwrap(e, k))

    def vexpr(self, head, k, n, m, d):
        f = lambda i: self.vfin(k, i)  # noqa: E731
        a, b, acc = f(n), f(m), f(d)
        p = '%s * %s' % (a, b)
        if k == 's' and self.fz and head in ('vmla', 'vmls', 'vnmla', 'vnmls'):
            p = 'ftz(%s)' % p
        return {'vadd': '%s + %s' % (a, b), 'vsub': '%s - %s' % (a, b), 'vmul': p,
                'vdiv': '%s / %s' % (a, b), 'vnmul': '-(%s)' % p,
                'vmla': '%s + %s' % (acc, p), 'vmls': '%s - %s' % (acc, p),
                'vnmla': '-%s - %s' % (acc, p), 'vnmls': '-%s + %s' % (acc, p)}[head]

    def vfin(self, k, i):
        e = self.vf(k, i)
        if k == 's' and self.fz:
            return 'ftz(%s)' % e
        return e

    def neon_f32(self, ins, head, regs, ops):
        d = regs[0][1]
        srcs = [r[1] for r in regs[1:] if r]
        lanes = []
        for j in (0, 1):
            if head == 'vneg':
                e = 'c->V.s[%d] ^ 0x80000000u' % (2 * srcs[0] + j)
            elif head == 'vabs':
                e = 'c->V.s[%d] & 0x7FFFFFFFu' % (2 * srcs[0] + j)
            else:
                a = 'ftz(c->V.f[%d])' % (2 * srcs[0] + j)
                b = 'ftz(c->V.f[%d])' % (2 * srcs[1] + j)
                if head in ('vmax', 'vmin'):
                    e = 'f2bits(f%s_neon(c->V.f[%d], c->V.f[%d]))' % (head[1:], 2 * srcs[0] + j,
                                                                   2 * srcs[1] + j)
                else:
                    op = {'vadd': '+', 'vsub': '-', 'vmul': '*'}.get(head)
                    if op is None:
                        raise Unsupported('neon ' + head)
                    e = 'f2bits(neon_f(%s %s %s))' % (a, op, b)
            lanes.append(e)
        return ['{ u32 x0_ = %s, x1_ = %s; c->V.s[%d] = x0_; c->V.s[%d] = x1_; }' % (
            lanes[0], lanes[1], 2 * d, 2 * d + 1)], False


def half(r, which):
    return '(s16)%s' % r if which == 'b' else '(s16)(%s >> 16)' % r


def wrap(lines):
    if any(x.startswith('u32 ') or x.startswith('u64 ') for x in lines):
        return ['{'] + ['  ' + x for x in lines] + ['}']
    return lines


NORETURN_IMPORTS = {
    'abort', 'exit', 'Exit', 'cxa_throw', 'cxa_rethrow', 'Unwind_SjLj_Resume',
    'stack_chk_fail', 'assert_rtn', 'longjmp', 'siglongjmp', 'objc_exception_throw',
    'cxa_call_unexpected', 'pthread_exit', 'cxa_bad_cast', 'cxa_bad_typeid',
    'objc_terminate', 'ZSt9terminatev', 'cxa_pure_virtual',
}


INLINE_IMPORTS = {'_fmax': 'imp_fmax', '_fmin': 'imp_fmin'}


def is_noreturn_import(name):
    if not name:
        return False
    n = name.lstrip('_')
    return n in NORETURN_IMPORTS or (n.startswith('ZSt') and '__throw_' in n)
