"""Function discovery and control-flow recovery for the x86 image.

A "function" here is simply an entry address plus every instruction reachable
from it without following calls.  Code shared between functions (via jumps into
the middle of another body) is duplicated into each, which is semantically
safe for recompilation.  A jump to another function's entry is a tail call.
"""
import capstone
from capstone import x86 as X

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns',
       'jp', 'jnp', 'jl', 'jge', 'jle', 'jg'}
LOOPS = {'loop', 'loope', 'loopne', 'jecxz', 'jcxz'}

# Imports that never return.
NORETURN_IMPORTS = {'ExitProcess', 'TerminateProcess', 'RaiseException',
                    'ExitThread', 'FatalAppExitA'}


class Insn:
    __slots__ = ('addr', 'size', 'mnem', 'ops_str', 'ops', 'prefix', 'cs',
                 'succ', 'kind', 'target', 'table')

    def __init__(self, ci):
        self.addr = ci.address
        self.size = ci.size
        self.mnem = ci.mnemonic
        self.ops_str = ci.op_str
        self.ops = list(ci.operands)
        self.prefix = list(ci.prefix)
        self.cs = ci
        self.kind = 'normal'   # normal|jmp|jcc|call|ret|ijmp|icall|trap
        self.target = None     # direct branch/call target
        self.succ = []         # in-function successor addresses
        self.table = None      # resolved jump-table targets for ijmp


class Analyzer:
    def __init__(self, img, extra_entries=()):
        self.img = img
        self.cache = {}
        self.funcs = {}          # entry -> Function
        self.entries = set()
        self.noreturn = set()    # function entries that never return
        self.warnings = []
        self.extra_entries = set(extra_entries)

    # ---------------------------------------------------------------- decode
    def decode(self, a):
        if a in self.cache:
            return self.cache[a]
        insn = None
        if self.img.in_text(a):
            code = self.img.read(a, 16)
            for ci in md.disasm(code, a, 1):
                insn = Insn(ci)
        if insn is None:
            insn = Insn.__new__(Insn)
            insn.addr, insn.size, insn.mnem, insn.ops_str = a, 1, '(bad)', ''
            insn.ops, insn.prefix, insn.cs = [], [], None
            insn.kind, insn.target, insn.succ, insn.table = 'trap', None, [], None
            self.cache[a] = insn
            return insn
        self.classify(insn)
        self.cache[a] = insn
        return insn

    def iat_of(self, op):
        """If op is a memory operand [disp] naming an IAT slot, return (dll,name)."""
        if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            return self.img.imports.get(op.mem.disp & 0xffffffff)
        return None

    def classify(self, i):
        m = i.mnem
        nxt = i.addr + i.size
        if m in ('ret', 'retf'):
            i.kind = 'ret'
        elif m in ('int3', 'hlt', 'ud2', '(bad)', 'int', 'into', 'iretd'):
            i.kind = 'trap'
        elif m == 'jmp':
            op = i.ops[0]
            if op.type == X.X86_OP_IMM:
                i.kind, i.target = 'jmp', op.imm & 0xffffffff
            else:
                i.kind = 'ijmp'
        elif m in JCC or m in LOOPS:
            i.kind, i.target = 'jcc', i.ops[0].imm & 0xffffffff
            i.succ = [nxt, i.target]
        elif m == 'call':
            op = i.ops[0]
            if op.type == X.X86_OP_IMM:
                i.kind, i.target = 'call', op.imm & 0xffffffff
            else:
                i.kind = 'icall'
            i.succ = [nxt]
        else:
            i.succ = [nxt]

    # ---------------------------------------------------------- jump tables
    def resolve_table(self, i, linear_prev):
        """Resolve `jmp [reg*4 + table]`.  linear_prev: instructions before i
        in the same straight-line path (most recent last)."""
        op = i.ops[0]
        if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0:
            return None
        table = op.mem.disp & 0xffffffff
        if not self.img.in_image(table):
            return None
        idx = op.mem.index
        bytetab = None
        bound = None
        for p in reversed(linear_prev[-12:]):
            # mov cl, byte ptr [reg + bt]  /  movzx ecx, byte ptr [reg + bt]
            if p.mnem in ('mov', 'movzx') and len(p.ops) == 2 and \
                    p.ops[0].type == X.X86_OP_REG and \
                    reg_full(p.ops[0].reg) == reg_full(idx) and \
                    p.ops[1].type == X.X86_OP_MEM and p.ops[1].size == 1 and \
                    bytetab is None:
                bytetab = p.ops[1].mem.disp & 0xffffffff
                idx = p.ops[1].mem.base or p.ops[1].mem.index
                continue
            if p.mnem == 'cmp' and p.ops[0].type == X.X86_OP_REG and \
                    reg_full(p.ops[0].reg) == reg_full(idx):
                if p.ops[1].type == X.X86_OP_IMM:
                    bound = p.ops[1].imm & 0xffffffff
                elif p.ops[1].type == X.X86_OP_REG:
                    bound = self.const_before(linear_prev, p, p.ops[1].reg)
                break
            if p.mnem == 'and' and p.ops[0].type == X.X86_OP_REG and \
                    reg_full(p.ops[0].reg) == reg_full(idx) and \
                    p.ops[1].type == X.X86_OP_IMM:
                bound = p.ops[1].imm & 0xffffffff
                break
        if bound is None or bound > 4096:
            return self.table_fallback(i, table)
        if bytetab is not None:
            n = max(self.img.u8(bytetab + k) for k in range(bound + 1)) + 1
        else:
            n = bound + 1
        targets = [self.img.u32(table + 4 * k) for k in range(n)]
        # Entries the index can never select may overlap neighbouring code.
        targets = [t for t in targets if self.img.in_text(t)]
        return targets or None

    def const_before(self, path, at, reg):
        k = path.index(at)
        for p in reversed(path[:k]):
            if p.ops and p.ops[0].type == X.X86_OP_REG and reg_full(p.ops[0].reg) == reg_full(reg):
                if p.mnem == 'mov' and p.ops[1].type == X.X86_OP_IMM:
                    return p.ops[1].imm & 0xffffffff
                return None
        return None

    def table_fallback(self, i, table):
        """Bound unknown: read entries while they look like nearby code.
        Over-reading only adds unreachable cases; under-reading would break."""
        targets = []
        for k in range(512):
            t = self.img.u32(table + 4 * k)
            if not self.img.in_text(t) or abs(t - i.addr) > 0x8000:
                if not targets and k < 4:
                    continue    # leading entries may overlap code (never selected)
                break
            targets.append(t)
        if targets:
            self.warnings.append(f'jump table at {i.addr:08x}: bound unknown, read {len(targets)} entries')
        return targets or None

    # ------------------------------------------------------------- functions
    def build_function(self, entry):
        insns = {}
        work = [(entry, [])]
        calls = set()
        while work:
            a, path = work.pop()
            while a not in insns:
                i = self.decode(a)
                insns[a] = i
                if i.kind == 'call':
                    calls.add(i.target)
                    if self.call_noreturn(i):
                        i.succ = []
                        break
                if i.kind == 'icall':
                    imp = self.iat_of(i.ops[0])
                    if imp and imp[1] in NORETURN_IMPORTS:
                        i.succ = []
                        break
                if i.kind == 'jmp':
                    t = i.target
                    if t in self.entries and t != entry:
                        i.succ = []          # tail call
                    else:
                        i.succ = [t]
                elif i.kind == 'ijmp':
                    if i.table is None:
                        i.table = self.resolve_table(i, path) or []
                        if not i.table and not self.iat_of(i.ops[0]):
                            self.warnings.append(
                                f'unresolved indirect jump at {i.addr:08x} in {entry:08x}: {i.mnem} {i.ops_str}')
                    i.succ = [t for t in sorted(set(i.table))]
                if i.kind == 'jcc':
                    t = i.target
                    for s in i.succ:
                        if s != a + i.size and not (s in self.entries and s != entry):
                            work.append((s, []))
                    if t in self.entries and t != entry:
                        i.succ = [a + i.size]  # conditional tail call; target kept
                    nexta = a + i.size
                    path = path + [i]
                    a = nexta
                    continue
                if i.kind == 'ijmp':
                    for s in i.succ:
                        work.append((s, []))
                    break
                if not i.succ:
                    break
                path = path + [i]
                if len(i.succ) == 1:
                    a = i.succ[0]
                else:
                    for s in i.succ[1:]:
                        work.append((s, []))
                    a = i.succ[0]
        return insns, calls

    def call_noreturn(self, i):
        return i.target in self.noreturn

    def discover(self, seeds):
        self.entries = set(seeds) | self.extra_entries
        pending = sorted(self.entries)
        while pending:
            new = []
            for e in pending:
                insns, calls = self.build_function(e)
                self.funcs[e] = insns
                for t in calls:
                    if self.img.in_text(t) and t not in self.entries:
                        self.entries.add(t)
                        new.append(t)
            pending = sorted(new)
        # Bodies built before some entries were known may have absorbed code
        # that is actually a separate function; rebuild once with the final set.
        for e in sorted(self.entries):
            self.funcs[e], _ = self.build_function(e)

    def compute_noreturn(self):
        """A function is noreturn if no ret / tail call / indirect jump out is
        reachable.  Iterate to a fixed point, rebuilding bodies."""
        changed = True
        rounds = 0
        while changed and rounds < 8:
            rounds += 1
            changed = False
            for e, insns in self.funcs.items():
                if e in self.noreturn:
                    continue
                returns = False
                for i in insns.values():
                    if i.kind == 'ret':
                        returns = True
                    elif i.kind == 'jmp' and not i.succ:
                        if i.target not in self.noreturn:
                            returns = True
                    elif i.kind == 'jcc' and i.target in self.entries and i.target != e:
                        if i.target not in self.noreturn:
                            returns = True
                    elif i.kind == 'ijmp' and not i.table:
                        returns = True
                    elif i.kind == 'trap':
                        pass
                    if returns:
                        break
                if not returns:
                    self.noreturn.add(e)
                    changed = True
            if changed:
                for e in sorted(self.entries):
                    self.funcs[e], _ = self.build_function(e)


_FULL = {}
for _r32, _subs in {
        'EAX': ('AL', 'AH', 'AX'), 'ECX': ('CL', 'CH', 'CX'), 'EDX': ('DL', 'DH', 'DX'),
        'EBX': ('BL', 'BH', 'BX'), 'ESP': ('SP',), 'EBP': ('BP',), 'ESI': ('SI',),
        'EDI': ('DI',)}.items():
    full = getattr(X, 'X86_REG_' + _r32)
    _FULL[full] = full
    for s in _subs:
        _FULL[getattr(X, 'X86_REG_' + s)] = full


def reg_full(r):
    return _FULL.get(r, r)


def scan_code_pointers(img):
    """Dwords in data sections that point into .text (vtables, callbacks)."""
    out = set()
    for name, start, end, _ in img.sections:
        if name in ('.rdata', '.data'):
            for a in range(start, min(end, img.base + img.size) - 3, 4):
                v = img.u32(a)
                if img.in_text(v):
                    out.add(v)
    return out


def plausible_entry(img, v, known):
    """Filter for pointer-derived entries: known functions, or 16-aligned
    addresses preceded by inter-function padding (MSVC aligns functions)."""
    if v in known:
        return True
    if v % 16:
        return False
    return img.u8(v - 1) in (0xcc, 0x90, 0xc3) or img.u8(v - 3) == 0xc2


def discover_all(img, ghidra_entries=()):
    from capstone import x86 as X
    known = set(ghidra_entries)
    an = Analyzer(img, extra_entries=known)
    data_ptrs = scan_code_pointers(img)
    seeds = {img.entry} | {v for v in data_ptrs if plausible_entry(img, v, known)}
    for _ in range(6):
        an.discover(seeds)
        # Data pointers that are not in the middle of decoded code are taken
        # as entries too (CRT initializer tables, callbacks in asm modules).
        starts, inside = set(), set()
        for insns in an.funcs.values():
            for i in insns.values():
                starts.add(i.addr)
                inside.update(range(i.addr + 1, i.addr + i.size))
        extra = {v for v in data_ptrs if v not in an.entries and (v in starts or v not in inside)}
        if extra:
            an.discover(an.entries | extra)
        imms = set()
        for insns in an.funcs.values():
            for i in insns.values():
                if i.kind in ('call', 'jmp', 'jcc'):
                    continue
                for op in i.ops:
                    if op.type == X.X86_OP_IMM:
                        v = op.imm & 0xffffffff
                        if img.in_text(v) and (v in starts or v not in inside):
                            imms.add(v)
        new = imms - an.entries
        if not new:
            break
        seeds = an.entries | new
    an.compute_noreturn()
    return an
