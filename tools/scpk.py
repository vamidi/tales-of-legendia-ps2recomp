"""Walker for SCPK, the bundle container -- carried over from the 2002 title.

The 2002 PlayStation 2 game put its bundles inside `FILE.FPB` and called them
SCPK.  This disc has no `FILE.FPB` and no `.cab`, but it has **744 SCPK
bundles** among the top-level members of `DAT.BIN`, holding 378 MB, and they
parse with the 2002 pipeline's parser unchanged:

    +0x00  char[4]  'SCPK'
    +0x04  u16      version
    +0x06  u16      flags / kind
    +0x08  u32      count      number of members
    +0x0C  u32      0          reserved
    +0x10  u32[]    size[count]   member sizes in bytes
    +...   members, concatenated in order with no padding

`parse()` and `block_head()` below are copied from
ps2-talesofdestiny2-doc/tools/scpk.py without an edit; only the layer that
finds the bundles is new, because the index moved from `FILE.FPB` to the three
`.BIN` containers this disc uses.  That split is deliberate -- if the format had
drifted, the copied half would have had to change, and it did not.

    python tools/scpk.py IMAGE.iso --census
    python tools/scpk.py IMAGE.iso --dupes
    python tools/scpk.py IMAGE.iso --member N
    python tools/scpk.py IMAGE.iso --list [--limit N]
"""

import collections
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tales_block
from binfs import Fs, CONTAINERS


def parse(d):
    """(version, kind, [(offset, size)]) for a bundle held in memory."""
    if d[:4] != b'SCPK':
        return None
    ver, kind = struct.unpack_from('<HH', d, 4)
    count, reserved = struct.unpack_from('<2I', d, 8)
    if count > 4096 or 0x10 + 4 * count > len(d):
        return None
    sizes = struct.unpack_from('<%dI' % count, d, 0x10)
    out = []
    o = 0x10 + 4 * count
    for s in sizes:
        out.append((o, s))
        o += s
    return ver, kind, reserved, out


def block_head(d, o):
    if o + 9 > len(d):
        return None
    m = d[o]
    if m not in (0, 1, 3):
        return None
    p, u = struct.unpack_from('<2I', d, o + 1)
    if p == 0 or u == 0 or o + 9 + p > len(d):
        return None
    if m == 0:
        return (m, p, u) if p == u else None
    if u <= p or u > p * 64:
        return None
    return (m, p, u)


def iter_bundles(fs):
    """Every top-level container member whose first four bytes are 'SCPK'."""
    for name in CONTAINERS:
        for i, o, s, _p in fs.members(name):
            if s < 16:
                continue
            if fs.read(name, o, 4) != b'SCPK':
                continue
            yield '%s#%d' % (name, i), fs.read(name, o, s)


def cmd_census(fs):
    nb = 0
    total_members = 0
    kinds = collections.Counter()
    counts = collections.Counter()
    comp = 0
    packed = unpacked = 0
    exact = mismatch = 0
    inner_magic = collections.Counter()
    for k, d in iter_bundles(fs):
        p = parse(d)
        if not p:
            continue
        ver, kind, reserved, members = p
        nb += 1
        kinds[(ver, kind, reserved)] += 1
        counts[len(members)] += 1
        total_members += len(members)
        for o, s in members:
            if s == 0:
                continue
            h = block_head(d, o)
            if h:
                comp += 1
                m, pk, un = h
                try:
                    out = tales_block.unpack(d[o:o + 9 + pk], 0, tales_block.PSX)
                except Exception:
                    mismatch += 1
                    continue
                packed += pk
                unpacked += len(out)
                if len(out) == un:
                    exact += 1
                else:
                    mismatch += 1
                inner_magic[out[:4].hex()] += 1
            else:
                inner_magic['raw:' + d[o:o + 4].hex()] += 1
    print('bundles                 %d' % nb)
    print('bundle members          %d' % total_members)
    print('header (ver, kind, res) %s' % dict(kinds))
    print('member counts           %s' % dict(sorted(counts.items())))
    print()
    print('compressed members      %d' % comp)
    print('decoded to declared len %d' % exact)
    print('mismatched              %d' % mismatch)
    print('packed bytes            %d' % packed)
    print('unpacked bytes          %d' % unpacked)
    if packed:
        print('ratio                   %.3fx' % (unpacked / packed))
    print()
    print('first four bytes after decoding, top 20:')
    for m, n in inner_magic.most_common(20):
        print('  %-16s %6d' % (m, n))


def cmd_dupes(fs):
    seen = collections.Counter()
    bytes_by_hash = {}
    total = 0
    for k, d in iter_bundles(fs):
        p = parse(d)
        if not p:
            continue
        for o, s in p[3]:
            if s == 0:
                continue
            h = hashlib.sha1(d[o:o + s]).hexdigest()
            seen[h] += 1
            bytes_by_hash[h] = s
            total += s
    dup_bytes = sum(bytes_by_hash[h] * (n - 1) for h, n in seen.items() if n > 1)
    print('distinct bundle members %d' % len(seen))
    print('bundle member instances %d' % sum(seen.values()))
    print('bytes stored            %d' % total)
    print('bytes that are copies   %d (%.1f%%)'
          % (dup_bytes, 100 * dup_bytes / total if total else 0))
    print()
    print('%-42s %8s %12s %12s' % ('SHA-1', 'COPIES', 'EACH', 'TOTAL'))
    for h, n in seen.most_common(20):
        print('%-42s %8d %12d %12d' % (h, n, bytes_by_hash[h], bytes_by_hash[h] * n))


def cmd_list(fs, limit):
    print('%-14s %10s %6s %5s %5s  %s'
          % ('MEMBER', 'BYTES', 'COUNT', 'VER', 'KIND', 'FIRST MEMBER'))
    n = 0
    for k, d in iter_bundles(fs):
        p = parse(d)
        if not p:
            print('%-14s %10d   does not parse' % (k, len(d)))
            continue
        ver, kind, reserved, members = p
        print('%-14s %10d %6d %5d %5d  %s'
              % (k, len(d), len(members), ver, kind,
                 d[members[0][0]:members[0][0] + 8].hex() if members else '-'))
        n += 1
        if n >= limit:
            return


def cmd_member(fs, want):
    d = None
    for k, buf in iter_bundles(fs):
        if k == want:
            d = buf
            break
    if d is None:
        print('%s is not an SCPK bundle' % want)
        return
    k = want
    p = parse(d)
    if not p:
        print('%s is not an SCPK bundle' % k)
        return
    ver, kind, reserved, members = p
    print('%s, %d bytes, SCPK version %d kind %d reserved %d, %d entries'
          % (k, len(d), ver, kind, reserved, len(members)))
    print('%-5s %-10s %-10s %-8s %-10s %-10s %s'
          % ('#', 'OFFSET', 'SIZE', 'METHOD', 'PACKED', 'UNPACKED', 'DECODED MAGIC'))
    for i, (o, s) in enumerate(members):
        h = block_head(d, o)
        if h:
            m, pk, un = h
            try:
                out = tales_block.unpack(d[o:o + 9 + pk], 0, tales_block.PSX)
                mg = out[:8].hex()
                ok = '' if len(out) == un else ' MISMATCH %d' % len(out)
            except Exception as e:
                mg, ok = '-', ' ' + type(e).__name__
            print('%-5d 0x%08X %-10d %-8d %-10d %-10d %s%s'
                  % (i, o, s, m, pk, un, mg, ok))
        else:
            print('%-5d 0x%08X %-10d %-8s %-10s %-10s raw:%s'
                  % (i, o, s, '-', '-', '-', d[o:o + 8].hex()))


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    fs = Fs(argv[1])
    rest = argv[2:]
    if '--census' in rest:
        cmd_census(fs)
    elif '--dupes' in rest:
        cmd_dupes(fs)
    elif '--member' in rest:
        cmd_member(fs, rest[rest.index('--member') + 1])
    elif '--list' in rest:
        lim = int(rest[rest.index('--limit') + 1]) if '--limit' in rest else 40
        cmd_list(fs, lim)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
