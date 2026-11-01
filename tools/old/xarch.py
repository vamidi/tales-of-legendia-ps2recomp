"""Compare two routines that were compiled for different instruction sets.

`decoder_lineage.py`, inherited from the 2002 pipeline, compares two MIPS
routines: it can ask whether the words are identical and, when they are not,
whether the *opcode sequence* is the same.  Both questions rely on the two
sides sharing a mnemonic vocabulary.

The 2003 GameCube build does not share one with anything else in the corpus.
So this file asks a weaker question that survives the change of machine: map
every instruction to what it *does* -- load a byte, store a byte, add a
constant, shift right, compare, branch -- and compare those sequences.

A weaker question needs controls, or the number means nothing, so the tool
insists on them:

    --control PATH ARCH ADDR

Give it an unrelated routine of similar size and it prints that score next to
the real one.  Two routines from the same source, recompiled, should sit far
above a routine picked at random; if they do not, the measurement has failed
and the honest thing is to say so.

    python tools/xarch.py A.elf mips 0x001C93D0 B.dol ppc 0x8005D088 \\
        --words 200 --control B.dol ppc 0x80005800
"""

import difflib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dismips
import disppc

# What an instruction does, with the machine taken out of it.
CLASS = {}
for _n in ('lb', 'lbu', 'lbz', 'lbzu', 'lbzx', 'lbzux'):
    CLASS[_n] = 'load8'
for _n in ('lh', 'lhu', 'lhz', 'lhzu', 'lha', 'lhax', 'lhzx'):
    CLASS[_n] = 'load16'
for _n in ('lw', 'lwu', 'ld', 'lwz', 'lwzu', 'lwzx', 'lwzux', 'lmw'):
    CLASS[_n] = 'load32'
for _n in ('sb', 'stb', 'stbu', 'stbx', 'stbux'):
    CLASS[_n] = 'store8'
for _n in ('sh', 'sth', 'sthu', 'sthx'):
    CLASS[_n] = 'store16'
for _n in ('sw', 'sd', 'stw', 'stwu', 'stwx', 'stwux', 'stmw'):
    CLASS[_n] = 'store32'
for _n in ('addi', 'addiu', 'daddi', 'daddiu', 'addic', 'addic.', 'li',
           'subfic'):
    CLASS[_n] = 'addimm'
for _n in ('add', 'addu', 'daddu', 'dadd', 'addc', 'adde'):
    CLASS[_n] = 'add'
for _n in ('sub', 'subu', 'dsubu', 'subf', 'subfc', 'neg'):
    CLASS[_n] = 'sub'
for _n in ('and', 'andi', 'andi.', 'andc'):
    CLASS[_n] = 'and'
for _n in ('or', 'ori', 'oris', 'orc', 'nor'):
    CLASS[_n] = 'or'
for _n in ('xor', 'xori', 'eqv'):
    CLASS[_n] = 'xor'
for _n in ('sll', 'sllv', 'dsll', 'slwi', 'slw'):
    CLASS[_n] = 'shl'
for _n in ('srl', 'srlv', 'dsrl', 'srwi', 'srw', 'rlwinm', 'rlwnm',
           'rlwimi'):
    CLASS[_n] = 'shr'
for _n in ('sra', 'srav', 'sraw', 'srawi'):
    CLASS[_n] = 'sar'
for _n in ('slt', 'slti', 'sltu', 'sltiu', 'cmpw', 'cmpwi', 'cmplw',
           'cmplwi'):
    CLASS[_n] = 'cmp'
for _n in ('beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bge', 'ble',
           'bgt', 'blt', 'bso', 'bns', 'bc'):
    CLASS[_n] = 'branch'
for _n in ('j', 'b', 'bctr'):
    CLASS[_n] = 'jump'
for _n in ('jal', 'jalr', 'bl', 'bctrl'):
    CLASS[_n] = 'call'
for _n in ('jr', 'blr'):
    CLASS[_n] = 'return'
for _n in ('lui', 'lis'):
    CLASS[_n] = 'himm'
for _n in ('mfspr', 'mtspr', 'mfcr', 'mtcrf', 'mflo', 'mfhi'):
    CLASS[_n] = 'sysreg'
CLASS['nop'] = 'nop'


def classify(mnem):
    m = mnem.rstrip('.')
    if m.startswith('bc '):
        m = 'bc'
    return CLASS.get(m, 'other:' + m)


def load(path, arch, addr, n):
    """(words, mnemonics, raw bytes) for n instructions at a virtual address."""
    data = open(path, 'rb').read()
    if arch == 'mips':
        d, va, e = dismips.load(path)
        off = None
        if e is None:
            if d[:8] == b'PS-X EXE':
                org = struct.unpack_from('<I', d, 0x18)[0]
                off = 0x800 + addr - org
            else:
                off = addr
        else:
            for p in e.phdrs:
                if p[0] == 1 and p[2] <= addr < p[2] + p[4]:
                    off = p[1] + addr - p[2]
        if off is None:
            raise SystemExit('%s: 0x%08X not mapped' % (path, addr))
        words = [struct.unpack_from('<I', d, off + 4 * i)[0] for i in range(n)]
        mn = [dismips.disasm(w, addr + 4 * i).split()[0]
              for i, w in enumerate(words)]
        return words, mn, d[off:off + 4 * n]
    off = dol_offset(data, addr)
    words = [struct.unpack_from('>I', data, off + 4 * i)[0] for i in range(n)]
    mn = []
    for i, w in enumerate(words):
        t = disppc.disasm(w, addr + 4 * i)
        mn.append(t.split()[0] if not t.startswith('bc ') else 'bc')
    return words, mn, data[off:off + 4 * n]


def dol_offset(data, addr):
    """Map a virtual address through a GameCube DOL header."""
    for i in range(18):
        off = struct.unpack_from('>I', data, i * 4)[0]
        a = struct.unpack_from('>I', data, 0x48 + i * 4)[0]
        size = struct.unpack_from('>I', data, 0x90 + i * 4)[0]
        if size and a <= addr < a + size:
            return off + addr - a
    raise SystemExit('0x%08X is not in any DOL section' % addr)


def longest_common_bytes(a, b):
    """Longest run of identical bytes at any alignment."""
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    m = sm.find_longest_match(0, len(a), 0, len(b))
    return m.size, m.a, m.b


def compare(pa, aa, va, pb, ab, vb, n, label=''):
    wa, ma, ba = load(pa, aa, va, n)
    wb, mb, bb = load(pb, ab, vb, n)
    ca = [classify(x) for x in ma]
    cb = [classify(x) for x in mb]
    ratio = difflib.SequenceMatcher(None, ca, cb, autojunk=False).ratio()
    ident = sum(1 for x, y in zip(wa, wb) if x == y) if aa == ab else 0
    run, oa, ob = longest_common_bytes(ba, bb)
    if label:
        print(label)
    print('  A %s %s @ 0x%08X' % (os.path.basename(pa), aa, va))
    print('  B %s %s @ 0x%08X' % (os.path.basename(pb), ab, vb))
    print('  instructions compared      %d' % n)
    if aa == ab:
        print('  identical words, in place  %d (%.1f%%)'
              % (ident, 100.0 * ident / n))
    print('  longest identical byte run %d bytes%s'
          % (run, (' (A+%d, B+%d)' % (oa, ob)) if run >= 8 else ''))
    print('  action sequence            %.1f%% similar' % (100.0 * ratio))
    print()
    return ratio


def main(argv):
    a = [x for x in argv[1:] if not x.startswith('--')]
    if len(a) < 6:
        raise SystemExit(__doc__)
    n = int(argv[argv.index('--words') + 1]) if '--words' in argv else 160
    r = compare(a[0], a[1], int(a[2], 0), a[3], a[4], int(a[5], 0), n,
                'the two routines')
    i = 0
    controls = []
    while '--control' in argv[i:]:
        k = argv.index('--control', i)
        controls.append((argv[k + 1], argv[k + 2], int(argv[k + 3], 0)))
        i = k + 1
    for cp, ca_, cv in controls:
        c = compare(a[0], a[1], int(a[2], 0), cp, ca_, cv, n,
                    'control: the same A against an unrelated routine')
        print('  the pair scores %+.1f points over this control'
              % (100.0 * (r - c)))
        print()


if __name__ == '__main__':
    main(sys.argv)
