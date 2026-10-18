import sys, collections
from capstone import x86 as X
from image import Image
from analysis import Analyzer, scan_code_pointers
img = Image(sys.argv[1])
ptrs = scan_code_pointers(img)
an = Analyzer(img)
seeds = {img.entry} | ptrs
for rnd in range(4):
    an.discover(seeds)
    imms = set()
    for insns in an.funcs.values():
        for i in insns.values():
            if i.kind in ('call', 'jmp', 'jcc'): continue
            for op in i.ops:
                if op.type == X.X86_OP_IMM and img.in_text(op.imm & 0xffffffff): imms.add(op.imm & 0xffffffff)
                if op.type == X.X86_OP_MEM and op.mem.base == 0 and img.in_text(op.mem.disp & 0xffffffff) and not i.kind=='ijmp': pass
    new = imms - an.entries
    print('round', rnd, 'funcs', len(an.funcs), 'new imm ptrs', len(new))
    if not new: break
    seeds = an.entries | new
an.compute_noreturn()
print('noreturn', len(an.noreturn))
gh = {int(l.split()[0], 16) for l in open(sys.argv[2])}
mine = set(an.funcs)
print('ghidra', len(gh), 'mine', len(mine), 'ghidra-not-mine', len(gh - mine), 'mine-not-ghidra', len(mine - gh))
print('sample ghidra-not-mine', [hex(x) for x in sorted(gh - mine)[:15]])
print('sample mine-not-ghidra', [hex(x) for x in sorted(mine - gh)[:15]])
print('warnings', len(an.warnings)); print('\n'.join(an.warnings[:20]))
cnt = collections.Counter()
covered = set()
for insns in an.funcs.values():
    for i in insns.values():
        covered.add(i.addr)
for a in covered: cnt[an.cache[a].mnem] += 1
print('unique insns', len(covered))
print(sorted(cnt.items(), key=lambda x: -x[1]))
