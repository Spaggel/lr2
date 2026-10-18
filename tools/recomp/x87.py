"""x87 FPU code generation, decoded from opcode/modrm bytes.

The register stack is modelled as double[8] plus a TOP index in the CPU
struct.  Doubles (53-bit mantissa) stand in for 80-bit extended precision;
the game runs the FPU in single/double precision mode anyway (D3D8 default).
"""
from capstone import x86 as X

ARITH = ['add', 'mul', 'com', 'comp', 'sub', 'subr', 'div', 'divr']


def binop(name, a, b):
    """C expression for a <op> b where name in add/mul/sub/subr/div/divr."""
    return {
        'add': f'{a} + {b}', 'mul': f'{a} * {b}', 'sub': f'{a} - {b}',
        'subr': f'{b} - {a}', 'div': f'{a} / {b}', 'divr': f'{b} / {a}',
    }[name]


class X87Gen:
    def gen(self, fg, i, op, modrm):
        mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
        e = fg.emit
        mem = None
        if mod != 3:
            memops = [o for o in i.ops if o.type == X.X86_OP_MEM]
            if not memops:
                raise_gen(fg, 'x87 memory operand missing')
            mem = fg.addr(memops[0])
        h = getattr(self, f'op_{op:02x}')
        h(fg, e, mem, reg, rm, i)

    # D8: float32 arithmetic / ST0,STi arithmetic
    def op_d8(self, fg, e, mem, reg, rm, i):
        src = f'(double)MF32({mem})' if mem else f'ST({rm})'
        self.arith_st0(e, reg, src)

    # DC: float64 arithmetic / STi,ST0 arithmetic
    def op_dc(self, fg, e, mem, reg, rm, i):
        if mem:
            self.arith_st0(e, reg, f'MF64({mem})')
            return
        # register form: destination is ST(i).  Note the sub/subr and
        # div/divr encodings are swapped relative to D8.
        name = {0: 'add', 1: 'mul', 2: 'com', 3: 'comp', 4: 'subr', 5: 'sub', 6: 'divr', 7: 'div'}[reg]
        if name in ('com', 'comp'):
            self.arith_st0(e, reg, f'ST({rm})')
            return
        e(f'ST({rm}) = {binop(name, f"ST({rm})", "ST(0)")};')

    # DA: int32 arithmetic (mod!=3); fucompp is DA E9
    def op_da(self, fg, e, mem, reg, rm, i):
        if mem:
            self.arith_st0(e, reg, f'(double)(int32_t)M32({mem})')
        elif reg == 5 and rm == 1:
            e('FCOM(ST(0), ST(1)); FPOP(); FPOP();')
        else:
            raise_gen(fg, 'fcmov')

    # DE: int16 arithmetic / arithmetic-and-pop
    def op_de(self, fg, e, mem, reg, rm, i):
        if mem:
            self.arith_st0(e, reg, f'(double)(int16_t)M16({mem})')
            return
        if reg == 3 and rm == 1:
            e('FCOM(ST(0), ST(1)); FPOP(); FPOP();')     # fcompp
            return
        name = {0: 'add', 1: 'mul', 4: 'subr', 5: 'sub', 6: 'divr', 7: 'div'}.get(reg)
        if name is None:
            raise_gen(fg, f'DE /{reg} reg')
        e(f'ST({rm}) = {binop(name, f"ST({rm})", "ST(0)")}; FPOP();')

    def arith_st0(self, e, reg, src):
        name = ARITH[reg]
        if name == 'com':
            e(f'FCOM(ST(0), {src});')
        elif name == 'comp':
            e(f'FCOM(ST(0), {src}); FPOP();')
        else:
            e(f'ST(0) = {binop(name, "ST(0)", src)};')

    # D9: loads/stores of float32, control word, constants, transcendental
    def op_d9(self, fg, e, mem, reg, rm, i):
        if mem:
            if reg == 0:
                e(f'FPUSH((double)MF32({mem}));')
            elif reg == 2:
                e(f'MF32({mem}) = (float)ST(0);')
            elif reg == 3:
                e(f'MF32({mem}) = (float)ST(0); FPOP();')
            elif reg == 4:
                e(f'fpu_ldenv(c, {mem});')
            elif reg == 5:
                e(f'c->fcw = M16({mem});')
            elif reg == 6:
                e(f'fpu_stenv(c, {mem});')
            elif reg == 7:
                e(f'M16({mem}) = (uint16_t)c->fcw;')
            else:
                raise_gen(fg, f'D9 /{reg} mem')
            return
        if reg == 0:
            e(f'{{ double v = ST({rm}); FPUSH(v); }}')
        elif reg == 1:
            e(f'{{ double t = ST(0); ST(0) = ST({rm}); ST({rm}) = t; }}')
        elif reg == 2 and rm == 0:
            pass                                    # fnop
        else:
            k = (reg << 3) | rm
            code = {
                0x20: 'ST(0) = -ST(0);',
                0x21: 'ST(0) = fabs(ST(0));',
                0x24: 'FCOM(ST(0), 0.0);',
                0x25: 'fpu_fxam(c);',
                0x28: 'FPUSH(1.0);',
                0x29: 'FPUSH(3.321928094887362347870319429489390175864831393);',
                0x2a: 'FPUSH(1.442695040888963407359924681001892137426645954);',
                0x2b: 'FPUSH(3.141592653589793238462643383279502884197169399);',
                0x2c: 'FPUSH(0.301029995663981195213738894724493026768189881);',
                0x2d: 'FPUSH(0.693147180559945309417232121458176568075500134);',
                0x2e: 'FPUSH(0.0);',
                0x30: 'ST(0) = exp2(ST(0)) - 1.0;',
                0x31: 'ST(1) = ST(1) * log2(ST(0)); FPOP();',
                0x32: 'ST(0) = tan(ST(0)); FPUSH(1.0);',
                0x33: 'ST(1) = atan2(ST(1), ST(0)); FPOP();',
                0x34: '{ double v = ST(0); int ex; double mt = frexp(v, &ex); ST(0) = (double)(ex - 1); FPUSH(mt * 2.0); }',
                0x35: 'ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x400u;',
                0x36: 'c->ftop = (c->ftop - 1) & 7;',
                0x37: 'c->ftop = (c->ftop + 1) & 7;',
                0x38: 'ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400u;',
                0x39: 'ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();',
                0x3a: 'ST(0) = sqrt(ST(0));',
                0x3b: '{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); }',
                0x3c: 'ST(0) = fpu_round(c, ST(0));',
                0x3d: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                0x3e: 'ST(0) = sin(ST(0)); c->fsw &= ~0x400u;',
                0x3f: 'ST(0) = cos(ST(0)); c->fsw &= ~0x400u;',
            }.get(k)
            if code is None:
                raise_gen(fg, f'D9 {0xc0 | k:02x}')
            e(code)

    # DB: int32 load/store, float80, init
    def op_db(self, fg, e, mem, reg, rm, i):
        if mem:
            if reg == 0:
                e(f'FPUSH((double)(int32_t)M32({mem}));')
            elif reg == 2:
                e(f'M32({mem}) = (uint32_t)fpu_toint32(c, ST(0));')
            elif reg == 3:
                e(f'M32({mem}) = (uint32_t)fpu_toint32(c, ST(0)); FPOP();')
            elif reg == 5:
                e(f'FPUSH(f80_load(m, {mem}));')
            elif reg == 7:
                e(f'f80_store(m, {mem}, ST(0)); FPOP();')
            else:
                raise_gen(fg, f'DB /{reg} mem')
            return
        k = (reg << 3) | rm
        if k == 0x22:
            e('c->fsw &= 0x3800u;')                 # fnclex
        elif k == 0x23:
            e('fpu_init(c);')                       # fninit
        elif reg in (5, 6):
            raise_gen(fg, 'fucomi/fcomi')
        else:
            raise_gen(fg, f'DB {0xc0 | k:02x}')

    # DD: float64 load/store, register free/store/ucom, fnsave/frstor, fnstsw m16
    def op_dd(self, fg, e, mem, reg, rm, i):
        if mem:
            if reg == 0:
                e(f'FPUSH(MF64({mem}));')
            elif reg == 2:
                e(f'MF64({mem}) = ST(0);')
            elif reg == 3:
                e(f'MF64({mem}) = ST(0); FPOP();')
            elif reg == 4:
                e(f'fpu_frstor(c, {mem});')
            elif reg == 6:
                e(f'fpu_fnsave(c, {mem});')
            elif reg == 7:
                e(f'M16({mem}) = FSTSW();')
            else:
                raise_gen(fg, f'DD /{reg} mem')
            return
        if reg == 0:
            pass                                    # ffree: tag word not modelled
        elif reg == 2:
            e(f'ST({rm}) = ST(0);')
        elif reg == 3:
            e(f'ST({rm}) = ST(0); FPOP();')
        elif reg == 4:
            e(f'FCOM(ST(0), ST({rm}));')
        elif reg == 5:
            e(f'FCOM(ST(0), ST({rm})); FPOP();')
        else:
            raise_gen(fg, f'DD reg /{reg}')

    # DF: int16/int64 load/store, fnstsw ax
    def op_df(self, fg, e, mem, reg, rm, i):
        if mem:
            if reg == 0:
                e(f'FPUSH((double)(int16_t)M16({mem}));')
            elif reg == 2:
                e(f'M16({mem}) = (uint16_t)fpu_toint16(c, ST(0));')
            elif reg == 3:
                e(f'M16({mem}) = (uint16_t)fpu_toint16(c, ST(0)); FPOP();')
            elif reg == 5:
                e(f'FPUSH((double)(int64_t)M64({mem}));')
            elif reg == 7:
                e(f'M64({mem}) = (uint64_t)fpu_toint64(c, ST(0)); FPOP();')
            else:
                raise_gen(fg, f'DF /{reg} mem (bcd)')
            return
        if reg == 4 and rm == 0:
            e('c->eax = (c->eax & 0xffff0000u) | FSTSW();')
        else:
            raise_gen(fg, f'DF reg /{reg}')


def raise_gen(fg, msg):
    from codegen import GenError
    raise GenError(msg)
