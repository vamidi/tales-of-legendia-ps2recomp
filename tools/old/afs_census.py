"""Census every member of every AFS archive on the disc.

Two passes, because they cost very different amounts.

`--headers` reads sixteen bytes per member and reports what kind of thing each
one is: a `CPS` compressed member with its declared sizes, or one of the plain
signatures this disc also carries.  It touches a few hundred kilobytes and
finishes in seconds, which makes it the right thing to run first and the right
thing to quote a denominator from.

`--decode` actually runs `tools/cps.py` over every `CPS` member and checks the
output length against that member's own declared unpacked size.  That is the
test the corpus asks for -- N of N blocks decode to their declared length --
and it is slow, because an LZSS is a byte at a time in Python.  Budget roughly
a minute per hundred megabytes of *output*.

The decoder is never told the unpacked size.  It stops when the input runs out,
which is what section 1 of the codec specification says drives the compressed
paths, so comparing the length it produced against the length the header claims
is a test rather than a tautology.

    python tools/afs_census.py IMAGE.iso --headers
    python tools/afs_census.py IMAGE.iso --decode
    python tools/afs_census.py IMAGE.iso --decode --only BATTLE
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cps

SECTOR = 2048

# name, lba, size -- from tools/iso9660.py --csv, quoted here so the census is
# reproducible without re-walking the volume.
ARCHIVES = [
    ('SYS_REG.AFS', 1981234, 2113536),
    ('FIELD.AFS', 1348178, 35233792),
    ('MAP.AFS', 961346, 792231936),
    ('BATTLE.AFS', 1365382, 661231616),
    ('SND.AFS', 1688249, 600033280),
    ('MOVIE.AFS', 526568, 890425344),
]

SIGS = [
    (b'CPS\x00', 'CPS (compressed)'),
    (b'AFS\x00', 'AFS (nested archive)'),
    (b'\x00\x00\x01\xba', 'MPEG program stream'),
    (b'VAGp', 'VAGp'),
    (b'SCPK', 'SCPK'),
    (b'MSCF', 'MSCF (cabinet)'),
    (b'CVMH', 'CVMH (CRI ROFS volume)'),
    (b'TIM2', 'TIM2'),
    (b'MFH\x00', 'MFH'),
]


def is_stored(head):
    """A CPS member whose payload is exactly its declared unpacked length.

    There is no method byte in this format.  Where the corpus's nine-byte
    header carries method 0 for a stored block, this one carries only two
    sizes -- and on 2,957 members of BATTLE.AFS the packed size is the
    unpacked size plus exactly the sixteen header bytes, which leaves the
    token stream no room to exist.  Those members are raw.  The test below is
    the only one the header supports, and it is presumably the test the game
    makes too, since nothing else in the sixteen bytes distinguishes them.
    """
    p, u, _r = struct.unpack('<III', head[4:16])
    return p - 16 == u


def sniff(head):
    for sig, label in SIGS:
        if head.startswith(sig):
            return label
    if head[:4] == b'\x80\x00\x00\x24' or head[:2] == b'\x80\x00':
        return 'ADX'
    return 'other'


def members(fh, lba):
    base = lba * SECTOR
    fh.seek(base)
    if fh.read(4) != b'AFS\x00':
        return None, []
    count = struct.unpack('<I', fh.read(4))[0]
    tab = fh.read(count * 8 + 8)
    ents = [struct.unpack_from('<II', tab, i * 8) for i in range(count)]
    return base, ents


def names_of(fh, base, count):
    fh.seek(base + 8 + count * 8)
    d_off, d_size = struct.unpack('<II', fh.read(8))
    if not d_off or d_size < count * 48:
        return []
    fh.seek(base + d_off)
    blk = fh.read(count * 48)
    out = []
    for i in range(count):
        r = blk[i * 48:i * 48 + 48]
        out.append(r[:32].split(b'\x00')[0].decode('ascii', 'replace')
                   if len(r) >= 32 else '')
    return out


def do_headers(fh, only):
    grand = {}
    gcount = 0
    gpacked = gunpacked = 0
    print('%-14s %8s  %-24s %8s %14s %14s %7s'
          % ('ARCHIVE', 'MEMBERS', 'KIND', 'COUNT', 'PACKED', 'UNPACKED',
             'RATIO'))
    for name, lba, _size in ARCHIVES:
        if only and only not in name:
            continue
        base, ents = members(fh, lba)
        if base is None:
            print('%-14s  not an AFS' % name)
            continue
        kinds = {}
        packed = unpacked = 0
        for off, msize in ents:
            if msize == 0:
                kinds['(empty)'] = kinds.get('(empty)', 0) + 1
                continue
            fh.seek(base + off)
            head = fh.read(16)
            k = sniff(head)
            if k.startswith('CPS') and is_stored(head):
                k = 'CPS (stored, raw payload)'
            kinds[k] = kinds.get(k, 0) + 1
            if k == 'CPS (compressed)':
                p, u, _r = struct.unpack('<III', head[4:16])
                packed += p
                unpacked += u
        gcount += len(ents)
        gpacked += packed
        gunpacked += unpacked
        first = True
        for k in sorted(kinds, key=lambda x: -kinds[x]):
            if k == 'CPS (compressed)':
                print('%-14s %8s  %-24s %8d %14d %14d %6.1f%%'
                      % (name if first else '', len(ents) if first else '',
                         k, kinds[k], packed, unpacked,
                         100.0 * packed / unpacked if unpacked else 0))
            else:
                print('%-14s %8s  %-24s %8d %14s %14s %7s'
                      % (name if first else '', len(ents) if first else '',
                         k, kinds[k], '', '', ''))
            first = False
            grand[k] = grand.get(k, 0) + kinds[k]
    print()
    print('%-14s %8d members across %d archives'
          % ('TOTAL', gcount, len(ARCHIVES)))
    for k in sorted(grand, key=lambda x: -grand[x]):
        print('   %-26s %8d' % (k, grand[k]))
    print()
    print('   CPS packed   %14d' % gpacked)
    print('   CPS unpacked %14d' % gunpacked)
    if gunpacked:
        print('   ratio        %13.1f%%' % (100.0 * gpacked / gunpacked))


def do_decode(fh, only):
    tot = ok = bad = 0
    tp = tu = 0
    expanded = 0
    smallest = largest = None
    fails = []
    for name, lba, _size in ARCHIVES:
        if only and only not in name:
            continue
        base, ents = members(fh, lba)
        if base is None:
            continue
        nm = names_of(fh, base, len(ents))
        a_tot = a_ok = 0
        for i, (off, msize) in enumerate(ents):
            if msize < 16:
                continue
            fh.seek(base + off)
            head = fh.read(16)
            if head[:4] != cps.MAGIC or is_stored(head):
                continue
            p, u, _r = struct.unpack('<III', head[4:16])
            fh.seek(base + off)
            buf = fh.read(msize)
            tot += 1
            a_tot += 1
            who = nm[i] if i < len(nm) else ''
            try:
                out = cps.unpack(buf, 0)
            except Exception as e:
                bad += 1
                fails.append((name, i, who, 'exception: %s' % e))
                continue
            tp += p
            tu += u
            if p >= u:
                expanded += 1
            if len(out) == u:
                ok += 1
                a_ok += 1
            else:
                bad += 1
                fails.append((name, i, who,
                              'produced %d, declared %d' % (len(out), u)))
            if smallest is None or p < smallest[0]:
                smallest = (p, u, name, who)
            if largest is None or p > largest[0]:
                largest = (p, u, name, who)
        print('%-14s %7d CPS members, %7d exact' % (name, a_tot, a_ok))
        sys.stdout.flush()
    print()
    print('CPS compressed members decoded  (stored members excluded)')
    print('CPS members     %d' % tot)
    print('exact           %d' % ok)
    print('mismatched      %d' % bad)
    print('packed bytes    %d' % tp)
    print('unpacked bytes  %d' % tu)
    if tu:
        print('ratio           %.2f%%' % (100.0 * tp / tu))
    print('expanded        %d  (members whose packed size >= unpacked)'
          % expanded)
    if smallest:
        print('smallest        %d packed -> %d  (%s / %s)' % smallest)
    if largest:
        print('largest         %d packed -> %d  (%s / %s)' % largest)
    for f in fails[:60]:
        print('  MISMATCH %s[%d] %s: %s' % f)


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    only = argv[argv.index('--only') + 1] if '--only' in argv else None
    fh = open(argv[1], 'rb')
    if '--decode' in argv:
        do_decode(fh, only)
    else:
        do_headers(fh, only)


if __name__ == '__main__':
    main(sys.argv)
