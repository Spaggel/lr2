"""x86-32 -> C code generator.

Each guest function becomes `void f_XXXXXXXX(CPU *restrict c)`.  Guest
registers live in the CPU struct (restrict lets the C compiler keep them in
host registers between guest calls).  Arithmetic flags are function-local and
only computed when a later instruction reads them (see liveness()).

Calling convention between recompiled functions mirrors the guest exactly:
`call` pushes the real return address on the guest stack, and the callee's
`ret n` pops it (plus n bytes).  Imports implemented by the runtime follow the
same rule, so stdcall/cdecl just work.
"""
from capstone import x86 as X
from analysis import JCC, LOOPS
import overrides

CF, ZF, SF, OF, PF = 1, 2, 4, 8, 16
ALL = CF | ZF | SF | OF | PF
FLAGNAMES = [(CF, 'cf'), (ZF, 'zf'), (SF, 'sf'), (OF, 'of'), (PF, 'pf')]

# --------------------------------------------------------------- registers
REG32 = {}
for n in ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'):
    REG32[getattr(X, 'X86_REG_' + n.upper())] = (n, 32)
for n, full in (('ax', 'eax'), ('cx', 'ecx'), ('dx', 'edx'), ('bx', 'ebx'),
                ('sp', 'esp'), ('bp', 'ebp'), ('si', 'esi'), ('di', 'edi')):
    REG32[getattr(X, 'X86_REG_' + n.upper())] = (full, 16)
for n, full in (('al', 'eax'), ('cl', 'ecx'), ('dl', 'edx'), ('bl', 'ebx')):
    REG32[getattr(X, 'X86_REG_' + n.upper())] = (full, 8)
for n, full in (('ah', 'eax'), ('ch', 'ecx'), ('dh', 'edx'), ('bh', 'ebx')):
    REG32[getattr(X, 'X86_REG_' + n.upper())] = (full, 'h')

MASK = {8: '0xffu', 16: '0xffffu', 32: '0xffffffffu'}
SIGN = {8: '0x80u', 16: '0x8000u', 32: '0x80000000u'}

COND = {
    'o': 'of', 'no': '!of', 'b': 'cf', 'ae': '!cf', 'e': 'zf', 'ne': '!zf',
    'be': '(cf | zf)', 'a': '(!cf && !zf)', 's': 'sf', 'ns': '!sf',
    'p': 'pf', 'np': '!pf', 'l': '(sf != of)', 'ge': '(sf == of)',
    'le': '(zf || sf != of)', 'g': '(!zf && sf == of)',
}
COND_USES = {
    'o': OF, 'no': OF, 'b': CF, 'ae': CF, 'e': ZF, 'ne': ZF, 'be': CF | ZF,
    'a': CF | ZF, 's': SF, 'ns': SF, 'p': PF, 'np': PF, 'l': SF | OF,
    'ge': SF | OF, 'le': ZF | SF | OF, 'g': ZF | SF | OF,
}
ALIASES = {'c': 'b', 'nae': 'b', 'nb': 'ae', 'nc': 'ae', 'z': 'e', 'nz': 'ne',
           'na': 'be', 'nbe': 'a', 'pe': 'p', 'po': 'np', 'nge': 'l', 'nl': 'ge',
           'ng': 'le', 'nle': 'g'}


def cc_of(mnem, prefix):
    cc = mnem[len(prefix):]
    return ALIASES.get(cc, cc)


class GenError(Exception):
    pass


def strip_prefix(mnem):
    """Split 'rep movsd' / 'lock inc' into (prefix, base mnemonic)."""
    parts = mnem.split()
    if len(parts) == 2:
        return parts[0], parts[1]
    return '', mnem


def x87_opcode(i):
    """Return (opcode byte D8..DF, modrm) for an x87 instruction, else None."""
    if i.cs is None:
        return None
    b = i.cs.bytes
    k = 0
    while k < len(b) and b[k] in (0x9b, 0x66, 0x67, 0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65):
        k += 1
    if k + 1 < len(b) and 0xd8 <= b[k] <= 0xdf:
        return b[k], b[k + 1]
    return None


# ---------------------------------------------------------------- flags use/def
def flag_effect(i):
    """Return (uses, kills, defs) bitmasks for the flags of instruction i.
    kills = flags unconditionally overwritten; defs = flags possibly written."""
    pre, m = strip_prefix(i.mnem)
    if i.kind in ('call', 'icall'):
        return 0, ALL, 0
    if x87_opcode(i):
        return 0, 0, 0
    if m in ('add', 'sub', 'cmp', 'neg', 'and', 'or', 'xor', 'test',
             'imul', 'mul', 'div', 'idiv', 'shld', 'shrd', 'cmpxchg', 'xadd',
             'popfd', 'popf'):
        return 0, ALL, ALL
    if m in ('adc', 'sbb'):
        return CF, ALL, ALL
    if m in ('inc', 'dec'):
        return 0, ZF | SF | OF | PF, ZF | SF | OF | PF
    if m in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
        rot = m in ('rol', 'ror', 'rcl', 'rcr')
        d = CF | OF if rot else ALL
        u = CF if m in ('rcl', 'rcr') else 0
        if len(i.ops) == 1:
            return u, d, d                 # count 1
        if i.ops[1].type == X.X86_OP_IMM:
            n = i.ops[1].imm & 31
            if n == 0:
                return 0, 0, 0
            return u, d, d
        return u | d, 0, d                 # count in cl: may be 0
    if m in ('bt', 'bts', 'btr', 'btc'):
        return 0, CF, CF
    if m in ('bsf', 'bsr'):
        return 0, ZF, ZF
    if m in ('clc', 'stc'):
        return 0, CF, CF
    if m == 'cmc':
        return CF, CF, CF
    if m == 'sahf':
        return 0, CF | ZF | SF | PF, CF | ZF | SF | PF
    if m in ('lahf', 'pushfd', 'pushf'):
        return ALL if m != 'lahf' else CF | ZF | SF | PF, 0, 0
    if m in ('scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
        if pre:
            return ALL, 0, ALL             # rep with ecx=0 leaves flags
        return 0, ALL, ALL
    if m.startswith('set'):
        return COND_USES[cc_of(m, 'set')], 0, 0
    if m.startswith('cmov'):
        return COND_USES[cc_of(m, 'cmov')], 0, 0
    if m in JCC:
        return COND_USES[cc_of(m, 'j')], 0, 0
    if m in ('loope', 'loopne'):
        return ZF, 0, 0
    return 0, 0, 0


def liveness(insns, exit_uses):
    """Backward flag liveness.  insns: addr->Insn.  exit_uses(i) gives extra
    flags read when control leaves the function at i (tail calls).
    Returns (live_out dict, live_in dict)."""
    eff = {a: flag_effect(i) for a, i in insns.items()}
    extra = {a: exit_uses(i) for a, i in insns.items()}
    live_in = {a: 0 for a in insns}
    live_out = {a: 0 for a in insns}
    order = sorted(insns, reverse=True)
    changed = True
    while changed:
        changed = False
        for a in order:
            i = insns[a]
            out = extra[a]
            for s in i.succ:
                if s in live_in:
                    out |= live_in[s]
            u, k, _ = eff[a]
            lin = u | (out & ~k)
            if out != live_out[a] or lin != live_in[a]:
                live_out[a] = out
                live_in[a] = lin
                changed = True
    return live_out, live_in


# ------------------------------------------------------------- the generator
class FuncGen:
    def __init__(self, cg, entry, insns, live_out, live_in):
        self.cg = cg
        self.img = cg.img
        self.entry = entry
        self.insns = insns
        self.live_out = live_out
        self.live_in = live_in
        self.out = []
        self.flags_used = 0

    # -- operand helpers -----------------------------------------------
    def addr(self, op):
        mm = op.mem
        parts = []
        if mm.base:
            parts.append(self.reg_read32(mm.base))
        if mm.index:
            r = self.reg_read32(mm.index)
            parts.append(f'{r} * {mm.scale}' if mm.scale != 1 else r)
        disp = mm.disp & 0xffffffff
        if disp or not parts:
            parts.append(f'0x{disp:x}u')
        if mm.segment == X.X86_REG_FS:
            parts.append('c->fs_base')
        expr = ' + '.join(parts)
        return f'(uint32_t)({expr})'

    def reg_read32(self, r):
        n, k = REG32[r]
        if k != 32:
            raise GenError('16/8-bit register in address')
        return f'c->{n}'

    def rd(self, op, size=None):
        if op.type == X.X86_OP_REG:
            if op.reg not in REG32:
                raise GenError(f'unsupported register {op.reg}')
            n, k = REG32[op.reg]
            if k == 32:
                return f'c->{n}'
            if k == 16:
                return f'(uint16_t)c->{n}'
            if k == 8:
                return f'(uint8_t)c->{n}'
            return f'(uint8_t)(c->{n} >> 8)'
        if op.type == X.X86_OP_IMM:
            sz = size or op.size * 8
            v = op.imm & ((1 << sz) - 1)
            return f'0x{v:x}u'
        if op.type == X.X86_OP_MEM:
            sz = size or op.size * 8
            return f'M{sz}({self.addr(op)})'
        raise GenError('bad operand')

    def wr(self, op, val):
        if op.type == X.X86_OP_REG:
            n, k = REG32[op.reg]
            if k == 32:
                return f'c->{n} = {val};'
            if k == 16:
                return f'c->{n} = (c->{n} & 0xffff0000u) | (uint16_t)({val});'
            if k == 8:
                return f'c->{n} = (c->{n} & 0xffffff00u) | (uint8_t)({val});'
            return f'c->{n} = (c->{n} & 0xffff00ffu) | ((uint32_t)(uint8_t)({val}) << 8);'
        if op.type == X.X86_OP_MEM:
            sz = op.size * 8
            return f'M{sz}({self.addr(op)}) = {val};'
        raise GenError('bad dest operand')

    def size(self, op):
        if op.type == X.X86_OP_REG:
            k = REG32[op.reg][1]
            return {32: 32, 16: 16, 8: 8, 'h': 8}[k]
        return op.size * 8

    # -- emission -------------------------------------------------------
    def emit(self, s):
        self.out.append('    ' + s)

    def need(self, i, f):
        return self.live_out[i.addr] & f

    def flags_res(self, i, r, bits, defs=ALL):
        """Emit ZF/SF/PF from result r if live."""
        o = []
        if defs & ZF and self.need(i, ZF):
            o.append(f'zf = ({r} == 0);')
        if defs & SF and self.need(i, SF):
            o.append(f'sf = ({r} >> {bits - 1}) & 1;')
        if defs & PF and self.need(i, PF):
            o.append(f'pf = PARITY({r});')
        return o

    def setf(self, i, f, expr):
        if self.need(i, f):
            name = dict(FLAGNAMES)[f]
            return [f'{name} = {expr};']
        return []

    def block(self, lines):
        decl = lines and lines[0].split(' ', 1)[0] in ('uint32_t', 'int32_t', 'uint64_t', 'int64_t', 'double')
        if len(lines) == 1 and not decl:
            self.emit(lines[0])
        else:
            self.emit('{ ' + ' '.join(lines) + ' }')

    def flags_from_ctx(self, i, f):
        """After a helper that stored flags into c->cf..., load the live ones."""
        o = []
        for bit, name in FLAGNAMES:
            if f & bit and self.need(i, bit):
                o.append(f'{name} = c->{name};')
        return o

    def flags_to_ctx(self, f):
        o = []
        for bit, name in FLAGNAMES:
            if f & bit:
                o.append(f'c->{name} = {name};')
        return o

    def tailcall(self, target, i):
        live = self.cg.func_live_in.get(target, 0)
        self.flags_used |= live
        o = self.flags_to_ctx(live)
        return o + [f'{self.cg.fname(target)}(c); return;']

    # -- function ------------------------------------------------------
    def generate(self):
        insns = self.insns
        order = sorted(insns)
        labels = {self.entry}
        for a in order:
            i = insns[a]
            for s in i.succ:
                if s != a + i.size:
                    labels.add(s)
        body = []
        self.out = body
        lin = self.live_in[self.entry]
        if lin:
            self.emit(' '.join(f'{n} = c->{n};' for b, n in FLAGNAMES if lin & b))
        if order[0] != self.entry:
            self.emit(f'goto L_{self.entry:08x};')
        fell_into = None
        for idx, a in enumerate(order):
            i = insns[a]
            if a in labels or fell_into != a:
                body.append(f'L_{a:08x}:')
            self.emit(f'/* {a:08x}: {i.mnem} {i.ops_str} */')
            try:
                if a in overrides.PATCHES:
                    self.emit(overrides.PATCHES[a])
                    self.mark_flags([overrides.PATCHES[a]])
                else:
                    self.insn(i)
            except GenError as e:
                self.emit(f'TRAP(c, 0x{a:x}u, "codegen: {e}");')
                self.cg.errors.append(f'{a:08x} {i.mnem} {i.ops_str}: {e}')
            nxt_addr = a + i.size
            falls = i.kind not in ('ret', 'ijmp', 'trap', 'jmp') and nxt_addr in i.succ
            nxt = order[idx + 1] if idx + 1 < len(order) else None
            if falls and nxt_addr != nxt:
                self.emit(f'goto L_{nxt_addr:08x};')
            fell_into = nxt_addr if falls else None
        decl = []
        for b, n in FLAGNAMES:
            if self.flags_used & b or lin & b:
                decl.append(n)
        head = [f'void {self.cg.fname(self.entry)}(CPU *restrict c) {{',
                '    uint8_t *const m = g_mem; (void)m;']
        if decl:
            head.append('    uint32_t ' + ', '.join(f'{n} = 0' for n in decl) + ';')
        # unused labels are harmless with -Wno-unused-label
        return '\n'.join(head + body + ['}', ''])

    def mark_flags(self, lines):
        for ln in lines:
            for b, n in FLAGNAMES:
                if ln.startswith(f'{n} ='):
                    self.flags_used |= b

    def emit_flags_block(self, pre, flags, post=()):
        lines = list(pre) + list(flags) + list(post)
        self.mark_flags(flags)
        self.block(lines)

    # -- instruction dispatch -------------------------------------------
    def insn(self, i):
        fx = x87_opcode(i)
        if fx:
            return self.cg.x87.gen(self, i, *fx)
        pre, m = strip_prefix(i.mnem)
        k = i.kind
        nxt = i.addr + i.size
        ops = i.ops

        if k == 'ret':
            n = ops[0].imm if ops else 0
            self.emit(f'c->esp += {4 + n}; return;')
            return
        if k == 'trap':
            self.emit(f'TRAP(c, 0x{i.addr:x}u, "{m}");')
            return
        if k == 'call':
            t = i.target
            self.emit(f'PUSH32(0x{nxt:x}u);')
            if t in self.cg.an.funcs:
                self.emit(f'{self.cg.fname(t)}(c);')
            else:
                self.emit(f'call_indirect(c, 0x{t:x}u);')
            if not i.succ:
                self.emit(f'NORETURN(c, 0x{i.addr:x}u);')
            return
        if k == 'icall':
            imp = self.cg.an.iat_of(ops[0])
            self.emit(f'PUSH32(0x{nxt:x}u);')
            if imp:
                self.emit(f'{self.cg.impname(imp)}(c);')
                self.cg.used_imports.add(imp)
            else:
                # target evaluated before the push in x86; esp-relative
                # operands must see the pre-push esp.
                tgt = self.rd(ops[0], 32)
                self.out[-1] = f'    {{ uint32_t t = {tgt}; PUSH32(0x{nxt:x}u); call_indirect(c, t); }}'
            if not i.succ:
                self.emit(f'NORETURN(c, 0x{i.addr:x}u);')
            return
        if k == 'jmp':
            if i.succ:
                self.emit(f'goto L_{i.target:08x};')
            elif i.target in self.cg.an.funcs:
                self.block(self.tailcall(i.target, i))
            else:
                self.emit(f'call_indirect(c, 0x{i.target:x}u); return;')
            return
        if k == 'ijmp':
            imp = self.cg.an.iat_of(ops[0])
            if imp:
                self.cg.used_imports.add(imp)
                self.emit(f'{self.cg.impname(imp)}(c); return;')
                return
            tgt = self.rd(ops[0], 32)
            if i.table:
                live = 0
                for t in set(i.table):
                    live |= self.live_in.get(t, 0)
                cases = ' '.join(f'case 0x{t:x}u: goto L_{t:08x};' for t in sorted(set(i.table)))
                self.emit(f'switch ({tgt}) {{ {cases} default: BADJUMP(c, 0x{i.addr:x}u, {tgt}); }}')
            else:
                self.emit(f'call_indirect(c, {tgt}); return;')
            return
        if m in JCC:
            cc = cc_of(m, 'j')
            self.flags_used |= COND_USES[cc]
            t = i.target
            if t in self.insns and t in i.succ:
                self.emit(f'if ({COND[cc]}) goto L_{t:08x};')
            else:
                self.emit(f'if ({COND[cc]}) {{ ' + ' '.join(self.tailcall(t, i)) + ' }')
            return
        if m in LOOPS:
            t = i.target
            jump = f'goto L_{t:08x};' if t in self.insns else ' '.join(self.tailcall(t, i))
            if m == 'loop':
                self.emit(f'if (--c->ecx != 0) {{ {jump} }}')
            elif m == 'loope':
                self.flags_used |= ZF
                self.emit(f'if (--c->ecx != 0 && zf) {{ {jump} }}')
            elif m == 'loopne':
                self.flags_used |= ZF
                self.emit(f'if (--c->ecx != 0 && !zf) {{ {jump} }}')
            elif m == 'jecxz':
                self.emit(f'if (c->ecx == 0) {{ {jump} }}')
            else:
                self.emit(f'if ((uint16_t)c->ecx == 0) {{ {jump} }}')
            return
        h = getattr(self, 'i_' + m, None)
        if h is None:
            raise GenError(f'unhandled instruction {m}')
        h(i, ops, pre)

    # -- data movement ---------------------------------------------------
    def i_mov(self, i, ops, pre):
        if ops[0].type == X.X86_OP_REG and ops[0].reg not in REG32 or \
                ops[1].type == X.X86_OP_REG and ops[1].reg not in REG32:
            raise GenError('segment register move')
        self.emit(self.wr(ops[0], self.rd(ops[1], self.size(ops[0]))))

    def i_movzx(self, i, ops, pre):
        self.emit(self.wr(ops[0], f'(uint32_t){self.rd(ops[1])}'))

    def i_movsx(self, i, ops, pre):
        s = self.size(ops[1])
        t = {8: 'int8_t', 16: 'int16_t'}[s]
        self.emit(self.wr(ops[0], f'(uint32_t)(int32_t)({t}){self.rd(ops[1])}'))

    def i_lea(self, i, ops, pre):
        self.emit(self.wr(ops[0], self.addr(ops[1])))

    def i_push(self, i, ops, pre):
        sz = self.size(ops[0]) if ops[0].type != X.X86_OP_IMM else ops[0].size * 8
        if sz == 16:
            self.emit(f'{{ uint32_t v = {self.rd(ops[0])}; c->esp -= 2; M16(c->esp) = v; }}')
        else:
            self.emit(f'PUSH32({self.rd(ops[0], 32)});')

    def i_pop(self, i, ops, pre):
        if self.size(ops[0]) == 16:
            self.emit(f'{{ uint32_t v = M16(c->esp); c->esp += 2; {self.wr(ops[0], "v")} }}')
        else:
            self.emit(f'{{ uint32_t v = M32(c->esp); c->esp += 4; {self.wr(ops[0], "v")} }}')

    def i_pushal(self, i, ops, pre):
        self.emit('{ uint32_t s = c->esp; PUSH32(c->eax); PUSH32(c->ecx); PUSH32(c->edx); PUSH32(c->ebx); '
                  'PUSH32(s); PUSH32(c->ebp); PUSH32(c->esi); PUSH32(c->edi); }')
    i_pushad = i_pushal

    def i_popal(self, i, ops, pre):
        self.emit('{ c->edi = M32(c->esp); c->esi = M32(c->esp + 4); c->ebp = M32(c->esp + 8); '
                  'c->ebx = M32(c->esp + 16); c->edx = M32(c->esp + 20); c->ecx = M32(c->esp + 24); '
                  'c->eax = M32(c->esp + 28); c->esp += 32; }')
    i_popad = i_popal

    def i_pushfd(self, i, ops, pre):
        self.flags_used |= ALL
        self.emit('PUSH32(MAKE_EFLAGS(c, cf, pf, zf, sf, of));')
    i_pushf = i_pushfd

    def i_popfd(self, i, ops, pre):
        self.flags_used |= ALL
        self.emit('{ uint32_t v = M32(c->esp); c->esp += 4; c->eflags_hi = v; c->df = (v >> 10) & 1; '
                  'cf = v & 1; pf = (v >> 2) & 1; zf = (v >> 6) & 1; sf = (v >> 7) & 1; of = (v >> 11) & 1; }')
    i_popf = i_popfd

    def i_xchg(self, i, ops, pre):
        a, b = ops
        self.emit(f'{{ uint32_t t = {self.rd(a)}; {self.wr(a, self.rd(b))} {self.wr(b, "t")} }}')

    def i_bswap(self, i, ops, pre):
        self.emit(self.wr(ops[0], f'__builtin_bswap32({self.rd(ops[0])})'))

    def i_leave(self, i, ops, pre):
        self.emit('c->esp = c->ebp; c->ebp = M32(c->esp); c->esp += 4;')

    def i_cdq(self, i, ops, pre):
        self.emit('c->edx = (uint32_t)((int32_t)c->eax >> 31);')

    def i_cwde(self, i, ops, pre):
        self.emit('c->eax = (uint32_t)(int32_t)(int16_t)c->eax;')

    def i_cbw(self, i, ops, pre):
        self.emit('c->eax = (c->eax & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)c->eax;')

    def i_cwd(self, i, ops, pre):
        self.emit('c->edx = (c->edx & 0xffff0000u) | (((int16_t)c->eax < 0) ? 0xffffu : 0);')

    def i_xlatb(self, i, ops, pre):
        self.emit('c->eax = (c->eax & 0xffffff00u) | M8(c->ebx + (uint8_t)c->eax);')

    def i_nop(self, i, ops, pre):
        pass

    def i_wait(self, i, ops, pre):
        pass

    def i_cld(self, i, ops, pre):
        self.emit('c->df = 0;')

    def i_std(self, i, ops, pre):
        self.emit('c->df = 1;')

    def i_clc(self, i, ops, pre):
        self.emit_flags_block([], self.setf(i, CF, '0'))

    def i_stc(self, i, ops, pre):
        self.emit_flags_block([], self.setf(i, CF, '1'))

    def i_cmc(self, i, ops, pre):
        self.flags_used |= CF
        self.emit('cf ^= 1;')

    def i_sahf(self, i, ops, pre):
        f = []
        f += self.setf(i, CF, '(c->eax >> 8) & 1')
        f += self.setf(i, PF, '(c->eax >> 10) & 1')
        f += self.setf(i, ZF, '(c->eax >> 14) & 1')
        f += self.setf(i, SF, '(c->eax >> 15) & 1')
        if f:
            self.emit_flags_block([], f)

    def i_lahf(self, i, ops, pre):
        self.flags_used |= CF | PF | ZF | SF
        self.emit('c->eax = (c->eax & 0xffff00ffu) | ((uint32_t)(cf | 2 | (pf << 2) | (zf << 6) | (sf << 7)) << 8);')

    def i_cpuid(self, i, ops, pre):
        self.emit('do_cpuid(c);')

    def i_rdtsc(self, i, ops, pre):
        self.emit('do_rdtsc(c);')

    # -- set/cmov --------------------------------------------------------
    def generic_cc(self, i, ops, m):
        if m.startswith('set'):
            cc = cc_of(m, 'set')
            self.flags_used |= COND_USES[cc]
            self.emit(self.wr(ops[0], f'({COND[cc]}) ? 1u : 0u'))
            return True
        if m.startswith('cmov'):
            cc = cc_of(m, 'cmov')
            self.flags_used |= COND_USES[cc]
            self.emit(f'if ({COND[cc]}) {self.wr(ops[0], self.rd(ops[1]))}')
            return True
        return False

    def __getattr__(self, name):
        if name.startswith('i_set') or name.startswith('i_cmov'):
            m = name[2:]
            return lambda i, ops, pre: self.generic_cc(i, ops, m)
        raise AttributeError(name)

    # -- arithmetic ------------------------------------------------------
    def arith(self, i, ops, op, write=True):
        bits = self.size(ops[0])
        mk = MASK[bits]
        a = self.rd(ops[0])
        b = self.rd(ops[1], bits)
        pre = [f'uint32_t a = {a}, b = {b};']
        f = []
        if op in ('add', 'adc'):
            if op == 'adc':
                self.flags_used |= CF
                pre.append(f'uint64_t w = (uint64_t)a + b + cf; uint32_t r = (uint32_t)w & {mk};')
                f += self.setf(i, CF, f'(uint32_t)(w >> {bits}) & 1')
            else:
                pre.append(f'uint32_t r = (a + b) & {mk};')
                f += self.setf(i, CF, '(r < a)')
            f += self.setf(i, OF, f'(((a ^ r) & (b ^ r)) >> {bits - 1}) & 1')
        elif op in ('sub', 'sbb', 'cmp'):
            if op == 'sbb':
                self.flags_used |= CF
                pre.append(f'uint32_t r = (a - b - cf) & {mk};')
                f += self.setf(i, CF, '((uint64_t)a < (uint64_t)b + cf)')
            else:
                pre.append(f'uint32_t r = (a - b) & {mk};')
                f += self.setf(i, CF, '(a < b)')
            f += self.setf(i, OF, f'(((a ^ b) & (a ^ r)) >> {bits - 1}) & 1')
        elif op in ('and', 'or', 'xor', 'test'):
            cop = {'and': '&', 'test': '&', 'or': '|', 'xor': '^'}[op]
            pre.append(f'uint32_t r = a {cop} b;')
            f += self.setf(i, CF, '0')
            f += self.setf(i, OF, '0')
        f += self.flags_res(i, 'r', bits)
        post = [self.wr(ops[0], 'r')] if write else []
        # Fast path: xor reg,reg
        if op == 'xor' and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and \
                ops[0].reg == ops[1].reg:
            pre = ['uint32_t r = 0;']
        self.emit_flags_block(pre, f, post)

    def i_add(self, i, ops, pre): self.arith(i, ops, 'add')
    def i_adc(self, i, ops, pre): self.arith(i, ops, 'adc')
    def i_sub(self, i, ops, pre): self.arith(i, ops, 'sub')
    def i_sbb(self, i, ops, pre): self.arith(i, ops, 'sbb')
    def i_and(self, i, ops, pre): self.arith(i, ops, 'and')
    def i_or(self, i, ops, pre): self.arith(i, ops, 'or')
    def i_xor(self, i, ops, pre): self.arith(i, ops, 'xor')
    def i_cmp(self, i, ops, pre): self.arith(i, ops, 'cmp', write=False)
    def i_test(self, i, ops, pre): self.arith(i, ops, 'test', write=False)

    def i_inc(self, i, ops, pre):
        bits = self.size(ops[0])
        f = self.setf(i, OF, f'(r == {SIGN[bits]})') + self.flags_res(i, 'r', bits)
        self.emit_flags_block([f'uint32_t r = ({self.rd(ops[0])} + 1) & {MASK[bits]};'], f, [self.wr(ops[0], 'r')])

    def i_dec(self, i, ops, pre):
        bits = self.size(ops[0])
        f = self.setf(i, OF, f'(r == {SIGN[bits]} - 1)') + self.flags_res(i, 'r', bits)
        self.emit_flags_block([f'uint32_t r = ({self.rd(ops[0])} - 1) & {MASK[bits]};'], f, [self.wr(ops[0], 'r')])

    def i_neg(self, i, ops, pre):
        bits = self.size(ops[0])
        f = self.setf(i, CF, '(a != 0)') + self.setf(i, OF, f'(a == {SIGN[bits]})') + self.flags_res(i, 'r', bits)
        self.emit_flags_block([f'uint32_t a = {self.rd(ops[0])}, r = (0u - a) & {MASK[bits]};'], f, [self.wr(ops[0], 'r')])

    def i_not(self, i, ops, pre):
        self.emit(self.wr(ops[0], f'~{self.rd(ops[0])}'))

    # -- shifts ----------------------------------------------------------
    def shift(self, i, ops, op):
        bits = self.size(ops[0])
        mk = MASK[bits]
        if len(ops) == 1:
            cnt, dyn = '1', False
        elif ops[1].type == X.X86_OP_IMM:
            n = ops[1].imm & 31
            if n == 0:
                return
            cnt, dyn = str(n), False
        else:
            cnt, dyn = '(c->ecx & 31)', True
        pre = [f'uint32_t a = {self.rd(ops[0])}, n = {cnt};']
        f = []
        if op == 'shl':
            pre.append(f'uint32_t r = (uint32_t)((uint64_t)a << n) & {mk};')
            f += self.setf(i, CF, f'(uint32_t)(((uint64_t)a << n) >> {bits}) & 1')
            f += self.setf(i, OF, f'((r >> {bits - 1}) ^ (uint32_t)(((uint64_t)a << n) >> {bits})) & 1')
            f += self.flags_res(i, 'r', bits)
        elif op == 'shr':
            pre.append('uint32_t r = (uint32_t)((uint64_t)a >> n);')
            f += self.setf(i, CF, '(uint32_t)((uint64_t)a >> (n - 1)) & 1')
            f += self.setf(i, OF, f'(a >> {bits - 1}) & 1')
            f += self.flags_res(i, 'r', bits)
        elif op == 'sar':
            st = {8: 'int8_t', 16: 'int16_t', 32: 'int32_t'}[bits]
            pre.append(f'int64_t sa = ({st})a; uint32_t r = (uint32_t)(sa >> n) & {mk};')
            f += self.setf(i, CF, '(uint32_t)(sa >> (n - 1)) & 1')
            f += self.setf(i, OF, '0')
            f += self.flags_res(i, 'r', bits)
        elif op in ('rol', 'ror'):
            pre[-1] = pre[-1].replace(f'n = {cnt}', f'n = {cnt} % {bits}')
            if op == 'rol':
                pre.append(f'uint32_t r = n ? ((a << n) | (a >> ({bits} - n))) & {mk} : a;')
                f += self.setf(i, CF, 'r & 1')
                f += self.setf(i, OF, f'((r >> {bits - 1}) ^ r) & 1')
            else:
                pre.append(f'uint32_t r = n ? ((a >> n) | (a << ({bits} - n))) & {mk} : a;')
                f += self.setf(i, CF, f'(r >> {bits - 1}) & 1')
                f += self.setf(i, OF, f'((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1')
        elif op in ('rcl', 'rcr'):
            self.flags_used |= CF
            pre.append(f'uint32_t r = a; uint32_t k = n % {bits + 1}; uint32_t cc = cf;')
            if op == 'rcl':
                pre.append(f'while (k--) {{ uint32_t o = (r >> {bits - 1}) & 1; r = ((r << 1) | cc) & {mk}; cc = o; }}')
                f += self.setf(i, OF, f'((r >> {bits - 1}) ^ cc) & 1')
            else:
                pre.append(f'while (k--) {{ uint32_t o = r & 1; r = (r >> 1) | (cc << {bits - 1}); cc = o; }}')
                f += self.setf(i, OF, f'((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1')
            f += self.setf(i, CF, 'cc')
        post = [self.wr(ops[0], 'r')]
        self.mark_flags(f)
        if dyn:
            self.emit('{ ' + ' '.join(pre) + ' if (n) { ' + ' '.join(post + f) + ' } }')
        else:
            self.block(pre + f + post)

    def i_shl(self, i, ops, pre): self.shift(i, ops, 'shl')
    i_sal = i_shl
    def i_shr(self, i, ops, pre): self.shift(i, ops, 'shr')
    def i_sar(self, i, ops, pre): self.shift(i, ops, 'sar')
    def i_rol(self, i, ops, pre): self.shift(i, ops, 'rol')
    def i_ror(self, i, ops, pre): self.shift(i, ops, 'ror')
    def i_rcl(self, i, ops, pre): self.shift(i, ops, 'rcl')
    def i_rcr(self, i, ops, pre): self.shift(i, ops, 'rcr')

    def dshift(self, i, ops, left):
        if self.size(ops[0]) != 32:
            raise GenError('16-bit shld/shrd')
        if ops[2].type == X.X86_OP_IMM:
            cnt, dyn = str(ops[2].imm & 31), False
            if cnt == '0':
                return
        else:
            cnt, dyn = '(c->ecx & 31)', True
        pre = [f'uint32_t a = {self.rd(ops[0])}, b = {self.rd(ops[1])}, n = {cnt};']
        if left:
            pre.append('uint32_t r = (uint32_t)(((((uint64_t)a << 32) | b) << n) >> 32);')
            f = self.setf(i, CF, '(a >> (32 - n)) & 1')
        else:
            pre.append('uint32_t r = (uint32_t)(((((uint64_t)b << 32) | a) >> n));')
            f = self.setf(i, CF, '(a >> (n - 1)) & 1')
        f += self.setf(i, OF, '((r ^ a) >> 31) & 1') + self.flags_res(i, 'r', 32)
        post = [self.wr(ops[0], 'r')]
        self.mark_flags(f)
        if dyn:
            self.emit('{ ' + ' '.join(pre) + ' if (n) { ' + ' '.join(post + f) + ' } }')
        else:
            self.block(pre + f + post)

    def i_shld(self, i, ops, pre): self.dshift(i, ops, True)
    def i_shrd(self, i, ops, pre): self.dshift(i, ops, False)

    # -- bit tests -------------------------------------------------------
    def bt(self, i, ops, op):
        bits = self.size(ops[0])
        if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
            # bit string: register offset may reach beyond the operand
            pre = [f'uint32_t o = {self.rd(ops[1])}; uint32_t ea = {self.addr(ops[0])} + (uint32_t)((int32_t)o >> 3);',
                   'uint32_t a = M8(ea), n = o & 7;']
            f = self.setf(i, CF, '(a >> n) & 1')
            w = {'bts': 'M8(ea) = a | (1u << n);', 'btr': 'M8(ea) = a & ~(1u << n);',
                 'btc': 'M8(ea) = a ^ (1u << n);'}.get(op)
            self.emit_flags_block(pre, f, [w] if w else [])
            return
        pre = [f'uint32_t a = {self.rd(ops[0])}, n = {self.rd(ops[1], bits)} % {bits};']
        f = self.setf(i, CF, '(a >> n) & 1')
        post = []
        if op == 'bts':
            post = [self.wr(ops[0], 'a | (1u << n)')]
        elif op == 'btr':
            post = [self.wr(ops[0], 'a & ~(1u << n)')]
        elif op == 'btc':
            post = [self.wr(ops[0], 'a ^ (1u << n)')]
        self.emit_flags_block(pre, f, post)

    def i_bt(self, i, ops, pre): self.bt(i, ops, 'bt')
    def i_bts(self, i, ops, pre): self.bt(i, ops, 'bts')
    def i_btr(self, i, ops, pre): self.bt(i, ops, 'btr')
    def i_btc(self, i, ops, pre): self.bt(i, ops, 'btc')

    def i_bsf(self, i, ops, pre):
        self.flags_used |= ZF
        self.emit(f'{{ uint32_t a = {self.rd(ops[1])}; zf = (a == 0); if (a) {self.wr(ops[0], "__builtin_ctz(a)")} }}')

    def i_bsr(self, i, ops, pre):
        self.flags_used |= ZF
        self.emit(f'{{ uint32_t a = {self.rd(ops[1])}; zf = (a == 0); if (a) {self.wr(ops[0], "31 - __builtin_clz(a)")} }}')

    # -- multiply / divide ----------------------------------------------
    def i_mul(self, i, ops, pre):
        bits = self.size(ops[0])
        s = self.rd(ops[0])
        if bits == 32:
            pre_ = [f'uint64_t p = (uint64_t)c->eax * {s}; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32);']
            hi = 'c->edx'
        elif bits == 16:
            pre_ = [f'uint32_t p = (uint32_t)(uint16_t)c->eax * {s}; c->eax = (c->eax & 0xffff0000u) | (p & 0xffff); '
                    'c->edx = (c->edx & 0xffff0000u) | (p >> 16);']
            hi = '(p >> 16)'
        else:
            pre_ = [f'uint32_t p = (uint32_t)(uint8_t)c->eax * {s}; c->eax = (c->eax & 0xffff0000u) | (p & 0xffff);']
            hi = '(p >> 8)'
        f = self.setf(i, CF, f'({hi} != 0)') + self.setf(i, OF, f'({hi} != 0)')
        f += self.flags_res(i, '(uint32_t)p', bits)
        self.emit_flags_block(pre_, f)

    def i_imul(self, i, ops, pre):
        if len(ops) == 1:
            bits = self.size(ops[0])
            s = self.rd(ops[0])
            if bits == 32:
                pre_ = [f'int64_t p = (int64_t)(int32_t)c->eax * (int32_t){s}; '
                        'c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32);']
                ov = '(p != (int32_t)p)'
            elif bits == 16:
                pre_ = [f'int32_t p = (int32_t)(int16_t)c->eax * (int16_t){s}; '
                        'c->eax = (c->eax & 0xffff0000u) | ((uint32_t)p & 0xffff); '
                        'c->edx = (c->edx & 0xffff0000u) | (((uint32_t)p >> 16) & 0xffff);']
                ov = '(p != (int16_t)p)'
            else:
                pre_ = [f'int32_t p = (int32_t)(int8_t)c->eax * (int8_t){s}; '
                        'c->eax = (c->eax & 0xffff0000u) | ((uint32_t)p & 0xffff);']
                ov = '(p != (int8_t)p)'
            f = self.setf(i, CF, ov) + self.setf(i, OF, ov)
            f += self.flags_res(i, '(uint32_t)p', bits)
            self.emit_flags_block(pre_, f)
            return
        bits = self.size(ops[0])
        if len(ops) == 2:
            a, b = self.rd(ops[0]), self.rd(ops[1], bits)
        else:
            a, b = self.rd(ops[1]), self.rd(ops[2], bits)
        st = {16: 'int16_t', 32: 'int32_t'}[bits]
        pre_ = [f'int64_t p = (int64_t)({st}){a} * ({st}){b}; uint32_t r = (uint32_t)p & {MASK[bits]};']
        ov = f'(p != ({st})p)'
        f = self.setf(i, CF, ov) + self.setf(i, OF, ov) + self.flags_res(i, 'r', bits)
        self.emit_flags_block(pre_, f, [self.wr(ops[0], 'r')])

    def i_div(self, i, ops, pre):
        bits = self.size(ops[0])
        s = self.rd(ops[0])
        a = f'0x{i.addr:x}u'
        if bits == 32:
            self.emit(f'{{ uint32_t d = {s}; if (!d) DIVZERO(c, {a}); uint64_t n = ((uint64_t)c->edx << 32) | c->eax; '
                      'c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); }')
        elif bits == 16:
            self.emit(f'{{ uint32_t d = {s}; if (!d) DIVZERO(c, {a}); uint32_t n = ((c->edx & 0xffff) << 16) | (c->eax & 0xffff); '
                      'c->eax = (c->eax & 0xffff0000u) | ((n / d) & 0xffff); c->edx = (c->edx & 0xffff0000u) | (n % d); }')
        else:
            self.emit(f'{{ uint32_t d = {s}; if (!d) DIVZERO(c, {a}); uint32_t n = c->eax & 0xffff; '
                      'c->eax = (c->eax & 0xffff0000u) | ((n % d) << 8) | ((n / d) & 0xff); }')

    def i_idiv(self, i, ops, pre):
        bits = self.size(ops[0])
        s = self.rd(ops[0])
        a = f'0x{i.addr:x}u'
        if bits == 32:
            self.emit(f'{{ int32_t d = (int32_t){s}; if (!d) DIVZERO(c, {a}); int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); '
                      'c->eax = (uint32_t)(int32_t)(n / d); c->edx = (uint32_t)(int32_t)(n % d); }')
        elif bits == 16:
            self.emit(f'{{ int32_t d = (int16_t){s}; if (!d) DIVZERO(c, {a}); int32_t n = (int32_t)(((c->edx & 0xffff) << 16) | (c->eax & 0xffff)); '
                      'c->eax = (c->eax & 0xffff0000u) | ((uint32_t)(n / d) & 0xffff); c->edx = (c->edx & 0xffff0000u) | ((uint32_t)(n % d) & 0xffff); }')
        else:
            self.emit(f'{{ int32_t d = (int8_t){s}; if (!d) DIVZERO(c, {a}); int32_t n = (int16_t)c->eax; '
                      'c->eax = (c->eax & 0xffff0000u) | (((uint32_t)(n % d) & 0xff) << 8) | ((uint32_t)(n / d) & 0xff); }')

    # -- atomics ---------------------------------------------------------
    def i_cmpxchg(self, i, ops, pre):
        bits = self.size(ops[0])
        acc = {32: 'c->eax', 16: '(uint16_t)c->eax', 8: '(uint8_t)c->eax'}[bits]
        accw = {32: 'c->eax = d;', 16: 'c->eax = (c->eax & 0xffff0000u) | d;', 8: 'c->eax = (c->eax & 0xffffff00u) | d;'}[bits]
        f = self.setf(i, CF, '(a < d)') + self.setf(i, OF, f'(((a ^ d) & (a ^ r)) >> {bits - 1}) & 1') + \
            self.flags_res(i, 'r', bits)
        f = [x.replace('zf = (r == 0)', 'zf = (a == d)') for x in f]
        self.emit_flags_block([f'uint32_t a = {acc}, d = {self.rd(ops[0])}, r = (a - d) & {MASK[bits]};',
                               f'if (a == d) {{ {self.wr(ops[0], self.rd(ops[1]))} }} else {{ {accw} }}'], f)

    def i_xadd(self, i, ops, pre):
        bits = self.size(ops[0])
        f = self.setf(i, CF, '(r < a)') + self.setf(i, OF, f'(((a ^ r) & (b ^ r)) >> {bits - 1}) & 1') + \
            self.flags_res(i, 'r', bits)
        self.emit_flags_block([f'uint32_t a = {self.rd(ops[0])}, b = {self.rd(ops[1])}, r = (a + b) & {MASK[bits]};',
                               self.wr(ops[1], 'a'), self.wr(ops[0], 'r')], f)

    # -- string ops ------------------------------------------------------
    def strop(self, i, base, size, pre):
        rep = {'': 0, 'rep': 1, 'repe': 1, 'repz': 1, 'repne': 2, 'repnz': 2}[pre]
        if base in ('movs', 'stos', 'lods'):
            self.emit(f'STR_{base.upper()}(c, {size}, {1 if rep else 0});')
            return
        # cmps/scas pass flags through the ctx (rep with ecx=0 keeps them)
        if rep:
            self.flags_used |= ALL
            self.emit(' '.join(self.flags_to_ctx(ALL)))
        self.emit(f'STR_{base.upper()}(c, {size}, {rep});')
        f = self.flags_from_ctx(i, ALL)
        if f:
            self.emit_flags_block([], f)

    def _str(base, size):
        def h(self, i, ops, pre):
            self.strop(i, base, size, pre)
        return h
    i_movsb = _str('movs', 1); i_movsw = _str('movs', 2); i_movsd = _str('movs', 4)
    i_stosb = _str('stos', 1); i_stosw = _str('stos', 2); i_stosd = _str('stos', 4)
    i_lodsb = _str('lods', 1); i_lodsw = _str('lods', 2); i_lodsd = _str('lods', 4)
    i_scasb = _str('scas', 1); i_scasw = _str('scas', 2); i_scasd = _str('scas', 4)
    i_cmpsb = _str('cmps', 1); i_cmpsw = _str('cmps', 2); i_cmpsd = _str('cmps', 4)
    del _str

    # -- things we refuse ----------------------------------------------------
    def _trap(self, i, ops, pre):
        self.emit(f'TRAP(c, 0x{i.addr:x}u, "{i.mnem}");')
    i_les = i_lds = i_cli = i_sti = i_in = i_out = i_insb = i_insd = i_outsb = i_outsd = _trap
    i_arpl = i_bound = i_lcall = i_ljmp = i_aaa = i_aas = i_daa = i_das = i_aam = i_aad = _trap
    i_salc = i_icebp = i_retf = i_lret = i_enter = _trap
