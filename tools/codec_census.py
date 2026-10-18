"""Decode every block the packer produced on this disc, and count its habits.

The test is the one section 7 of tales-blockcodec-doc prescribes: take the
nine-byte header, decode with the shared reference decoder, and keep only the
results whose length matches the length the header itself declares.  The
decoder is `tales_block.py` copied from the corpus without an edit, and that is
the point -- a title that needed a patched decoder would be a new dialect, and
this one does not.

Where the blocks are is not guessed.  `binfs.py` reads the index the executable
carries, so every offset here was declared by the game rather than found by a
scan, and a block that fails is a real failure rather than a false positive.
`--scan` does the opposite job: it sweeps a container for headers the index
does not point at, which is where an orphaned or superseded asset would show up.

One level of nesting is followed.  A compressed member decodes to a small
archive -- a u32 count, then that many (u32 offset, u32 size) pairs, then the
payloads -- and those payloads may themselves be blocks.  `--nested` decodes
those too and reports them separately, because the 2002 title's method
distribution differed sharply between its top level and its nested level and
the comparison is only meaningful if the two are kept apart.

    python tools/codec_census.py IMAGE.iso [--nested] [--container DAT.BIN]
    python tools/codec_census.py IMAGE.iso --archives [--limit N]
    python tools/codec_census.py IMAGE.iso --scan DAT.BIN --step 64 [--budget S]
"""

import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tales_block
from binfs import Fs, kind, CONTAINERS

NAMES = {0: 'stored', 1: 'lzss', 3: 'lzss + run escape'}


def is_block(head, size):
    return kind(head, size).startswith('block:')


def parse_bundle(buf):
    """[(off, size)] if buf is a count+offsets bundle, else None.

    The second level of DAT.BIN.  A top-level member may be a bundle: a u32
    count, then that many u32 offsets relative to the member, each a multiple of
    256, with the header padded out to the first of them.  Sizes are implied --
    a sub-member runs to the next offset, and the last runs to the end of the
    member.  There are no names and no types; what a sub-member is has to be
    read off its own first bytes.
    """
    if len(buf) < 264:
        return None
    n = struct.unpack_from('<I', buf, 0)[0]
    if not 2 <= n <= 4096 or 4 + 4 * n > len(buf):
        return None
    offs = [struct.unpack_from('<I', buf, 4 + 4 * i)[0] for i in range(n)]
    if offs[0] != (4 + 4 * n + 255) // 256 * 256:
        return None
    if any(x % 256 for x in offs):
        return None
    if any(offs[i] >= offs[i + 1] for i in range(n - 1)):
        return None
    if offs[-1] >= len(buf):
        return None
    ends = offs[1:] + [len(buf)]
    return list(zip(offs, [e - o for o, e in zip(offs, ends)]))


def parse_archive(buf):
    """(count, [(off, size)]) if buf is a count+pairs archive, else None."""
    if len(buf) < 12:
        return None
    n = struct.unpack_from('<I', buf, 0)[0]
    if not 1 <= n <= 4096 or 4 + 8 * n > len(buf):
        return None
    ent = []
    for i in range(n):
        o, s = struct.unpack_from('<II', buf, 4 + 8 * i)
        if o + s > len(buf):
            return None
        ent.append((o, s))
    # The members must tile the buffer end to end.  That is what makes this a
    # recognition rather than a guess: a random buffer does not satisfy it.
    if ent[0][0] > 64:
        return None
    for i in range(len(ent) - 1):
        if ent[i][0] + ent[i][1] != ent[i + 1][0]:
            return None
    if ent[-1][0] + ent[-1][1] != len(buf):
        return None
    return n, ent


class Tally:
    def __init__(self, label):
        self.label = label
        self.methods = {}
        self.packed = 0
        self.unpacked = 0
        self.ok = 0
        self.bad = []
        self.largest = (0, None)
        self.smallest = (1 << 40, None)
        self.expanded = 0

    def add(self, m, packed, unpacked, where, produced):
        self.methods[m] = self.methods.get(m, 0) + 1
        self.packed += packed
        self.unpacked += unpacked
        if produced == unpacked:
            self.ok += 1
        else:
            self.bad.append((where, unpacked, produced))
        if packed > self.largest[0]:
            self.largest = (packed, where)
        if packed < self.smallest[0]:
            self.smallest = (packed, where)
        if packed >= unpacked:
            self.expanded += 1

    def report(self):
        n = sum(self.methods.values())
        print('--- %s: %d blocks' % (self.label, n))
        if not n:
            return
        for m in sorted(self.methods):
            print('    method %d %-20s %6d  (%.1f%%)'
                  % (m, NAMES.get(m, '?'), self.methods[m],
                     100.0 * self.methods[m] / n))
        print('    decode to declared length      %d of %d' % (self.ok, n))
        for w, want, got in self.bad[:10]:
            print('      MISMATCH %s: declared %s, produced %s' % (w, want, got))
        print('    packed                         %d' % self.packed)
        print('    unpacked                       %d' % self.unpacked)
        if self.packed:
            print('    ratio                          %.2fx'
                  % (self.unpacked / float(self.packed)))
        print('    largest packed block           %d bytes  (%s)' % self.largest)
        print('    smallest packed block          %d bytes  (%s)' % self.smallest)
        print('    blocks that did not shrink     %d' % self.expanded)


def census(fs, containers, nested):
    top = Tally('top level')
    inner = Tally('inside bundles, one level down')
    archives = 0
    arch_members = 0
    bundles = 0
    bundle_members = 0
    for name in containers:
        for i, o, s, _p in fs.members(name):
            if s < 16:
                continue
            head = fs.read(name, o, min(s, 4096))
            if nested and not is_block(head, s):
                body = fs.read(name, o, s)
                b = parse_bundle(body)
                if b:
                    bundles += 1
                    bundle_members += len(b)
                    for j, (bo, bs) in enumerate(b):
                        sub = body[bo:bo + bs]
                        if len(sub) < 16 or not is_block(sub[:16], len(sub)):
                            continue
                        m2, p2, u2 = tales_block.header(sub)
                        try:
                            o2 = tales_block.unpack(sub, 0, 'psx')
                        except tales_block.BlockError as e:
                            inner.bad.append(('%s#%d[%d]' % (name, i, j), u2,
                                              str(e)))
                            continue
                        inner.add(m2, p2, u2,
                                  '%s#%d[%d]' % (name, i, j), len(o2))
                continue
            if not is_block(head, s):
                continue
            data = fs.read(name, o, s)
            m, packed, unpacked = tales_block.header(data)
            try:
                out = tales_block.unpack(data, 0, 'psx')
            except tales_block.BlockError as e:
                top.bad.append(('%s#%d' % (name, i), unpacked, str(e)))
                continue
            top.add(m, packed, unpacked, '%s#%d' % (name, i), len(out))
            if not nested:
                continue
            a = parse_archive(out)
            if not a:
                continue
            archives += 1
            arch_members += a[0]
            for j, (mo, ms) in enumerate(a[1]):
                sub = out[mo:mo + ms]
                if len(sub) < 16 or not is_block(sub[:16], len(sub)):
                    continue
                m2, p2, u2 = tales_block.header(sub)
                try:
                    o2 = tales_block.unpack(sub, 0, 'psx')
                except tales_block.BlockError as e:
                    inner.bad.append(('%s#%d.%d' % (name, i, j), u2, str(e)))
                    continue
                inner.add(m2, p2, u2, '%s#%d.%d' % (name, i, j), len(o2))
    top.report()
    print()
    if nested:
        print('bundles recognised among top-level members: %d, holding %d members'
              % (bundles, bundle_members))
        print('archives recognised inside decoded blocks:  %d, holding %d members'
              % (archives, arch_members))
        print()
        inner.report()


def cmd_archives(fs, limit):
    shown = 0
    for name in CONTAINERS:
        for i, o, s, _p in fs.members(name):
            if s < 16:
                continue
            head = fs.read(name, o, 16)
            if not is_block(head, s):
                continue
            out = tales_block.unpack(fs.read(name, o, s), 0, 'psx')
            a = parse_archive(out)
            if not a:
                continue
            kinds = {}
            for mo, ms in a[1]:
                k = kind(out[mo:mo + 16], ms)
                kinds[k] = kinds.get(k, 0) + 1
            print('%s#%-6d %9d bytes  %3d members  %s'
                  % (name, i, len(out), a[0],
                     ' '.join('%s x%d' % (k, v) for k, v in sorted(kinds.items()))))
            shown += 1
            if shown >= limit:
                return


def cmd_scan(fs, name, step, budget):
    """Headers the index does not point at."""
    known = set(o for _i, o, _s, _p in fs.members(name))
    ent = fs.entries[name]
    found = []
    seen = set()
    t0 = time.time()
    stopped = None
    CH = 1 << 24
    # The overlap must exceed the largest block on the disc, or a block that
    # straddles a chunk boundary is missed; and the overlap region is read
    # twice, so hits are de-duplicated by absolute offset rather than counted
    # twice.  Both of those were wrong in the first draft and both changed the
    # number, which is why they are spelled out here.
    OVER = 1 << 21
    pos = 0
    fs.fh.seek(ent.lba * 2048)
    prev = b''
    while pos < ent.size:
        if time.time() - t0 > budget:
            stopped = pos
            break
        chunk = fs.fh.read(CH)
        if not chunk:
            break
        buf = prev + chunk
        base = pos - len(prev)
        for off in range(0, len(buf) - 9, step):
            a = base + off
            if a in known or a % 64:
                continue
            if not tales_block.plausible(buf, off, 'psx'):
                continue
            m, p, u = tales_block.header(buf, off)
            if off + 9 + p > len(buf):
                continue
            try:
                out = tales_block.unpack(buf, off, 'psx')
            except tales_block.BlockError:
                continue
            if len(out) == u and a not in seen:
                seen.add(a)
                found.append((a, m, p, u))
        prev = buf[-OVER:]
        pos = base + len(buf)
    print('%s: swept %d of %d bytes at step %d in %.1fs%s'
          % (name, min(pos, ent.size), ent.size, step, time.time() - t0,
             '  ABANDONED at %d (budget %ds)' % (stopped, budget) if stopped else ''))
    print('blocks that decode but are not in the index: %d' % len(found))
    for a, m, p, u in found[:40]:
        print('    offset %12d  method %d  packed %8d  unpacked %8d' % (a, m, p, u))


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    fs = Fs(argv[1])
    rest = argv[2:]
    if '--scan' in rest:
        k = rest.index('--scan')
        step = int(rest[rest.index('--step') + 1]) if '--step' in rest else 64
        budget = int(rest[rest.index('--budget') + 1]) if '--budget' in rest else 600
        cmd_scan(fs, rest[k + 1], step, budget)
    elif '--archives' in rest:
        lim = int(rest[rest.index('--limit') + 1]) if '--limit' in rest else 30
        cmd_archives(fs, lim)
    else:
        cs = ([rest[rest.index('--container') + 1]] if '--container' in rest
              else list(CONTAINERS))
        census(fs, cs, '--nested' in rest)


if __name__ == '__main__':
    main(sys.argv)
