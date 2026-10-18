"""CRI AFS archive reader.

`AFS` is CRI Middleware's plain archive container -- the same house that
supplies `ROFS`, `CVM`, ADX and Sofdec.  It is the simplest thing in that
catalogue and it has no compression of its own:

    +0x00  "AFS\0"
    +0x04  u32   member count
    +0x08  count x (u32 offset, u32 size)      the extent table
    +...   u32 offset, u32 size                the *directory* block's own extent
    ...
    directory block: count x 48 bytes
    +0x00  char[32]  name
    +0x20  u16 year, month, day, hour, minute, second
    +0x2C  u32 size

Everything is little-endian and every member starts on a 2,048-byte boundary,
because the archive is meant to be read a sector at a time off a DVD.

The directory block is the reason this file exists.  ISO 9660 gives one
timestamp per file and this disc has 37 of them; the AFS directories carry one
per *member*, and there are 25,013 members.  That is the only build chronology
a shipped disc contains, and on this corpus it has been the most informative
thing on every disc that had one.

    python tools/afs.py IMAGE.iso --lba N --size N            # header + census
    python tools/afs.py IMAGE.iso --lba N --size N --list     # every member
    python tools/afs.py IMAGE.iso --lba N --size N --dates    # the chronology
    python tools/afs.py FILE.afs --list
"""

import os
import struct
import sys

SECTOR = 2048


class AFS:
    def __init__(self, fh, base=0, length=None):
        self.fh = fh
        self.base = base
        fh.seek(base)
        head = fh.read(8)
        if head[:4] != b'AFS\x00':
            raise ValueError('not an AFS archive (magic %r)' % head[:4])
        self.count = struct.unpack_from('<I', head, 4)[0]
        fh.seek(base + 8)
        tab = fh.read(self.count * 8 + 8)
        self.entries = [struct.unpack_from('<II', tab, i * 8)
                        for i in range(self.count)]
        self.dir_off, self.dir_size = struct.unpack_from(
            '<II', tab, self.count * 8)
        self.length = length
        self._names = None

    def names(self):
        """[(name, (Y,M,D,h,m,s), size)] from the directory block, or []."""
        if self._names is not None:
            return self._names
        self._names = []
        if not self.dir_off or self.dir_size < self.count * 48:
            return self._names
        self.fh.seek(self.base + self.dir_off)
        blk = self.fh.read(self.count * 48)
        for i in range(self.count):
            r = blk[i * 48:i * 48 + 48]
            if len(r) < 48:
                break
            name = r[:32].split(b'\x00')[0].decode('ascii', 'replace')
            y, mo, d, h, mi, s = struct.unpack_from('<6H', r, 32)
            size = struct.unpack_from('<I', r, 44)[0]
            self._names.append((name, (y, mo, d, h, mi, s), size))
        return self._names

    def read(self, i):
        off, size = self.entries[i]
        self.fh.seek(self.base + off)
        return self.fh.read(size)


def stamp(t):
    y, mo, d, h, mi, s = t
    if not 1990 <= y <= 2030:
        return '(no date)'
    return '%04d-%02d-%02d %02d:%02d:%02d' % (y, mo, d, h, mi, s)


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    path = argv[1]
    base = 0
    length = None
    if '--lba' in argv:
        base = int(argv[argv.index('--lba') + 1], 0) * SECTOR
    if '--size' in argv:
        length = int(argv[argv.index('--size') + 1], 0)
    fh = open(path, 'rb')
    a = AFS(fh, base, length)
    names = a.names()

    print('%s  base 0x%X' % (os.path.basename(path), base))
    print('members         %d' % a.count)
    print('extent table    0x08 .. 0x%X' % (8 + a.count * 8))
    print('directory       offset 0x%X, %d bytes (%d x 48 = %d)'
          % (a.dir_off, a.dir_size, a.count, a.count * 48))
    tot = sum(s for _o, s in a.entries)
    print('member bytes    %d' % tot)
    if length:
        print('archive bytes   %d  (%.2f%% is member payload)'
              % (length, 100.0 * tot / length))
    if names:
        ds = sorted(stamp(t) for _n, t, _s in names if stamp(t) != '(no date)')
        print('dated members   %d of %d' % (len(ds), len(names)))
        if ds:
            print('date range      %s .. %s' % (ds[0], ds[-1]))
    else:
        print('directory       absent or unreadable -- no per-member names')

    if '--list' in argv:
        print()
        print('%5s %-24s %10s %12s  %s'
              % ('#', 'NAME', 'SIZE', 'OFFSET', 'STAMP'))
        for i, (off, size) in enumerate(a.entries):
            n, t = ('', (0,) * 6)
            if i < len(names):
                n, t, _ds = names[i]
            print('%5d %-24s %10d %12d  %s' % (i, n, size, off, stamp(t)))

    if '--dates' in argv:
        print()
        hist = {}
        for _n, t, _s in names:
            if stamp(t) == '(no date)':
                hist['(no date)'] = hist.get('(no date)', 0) + 1
            else:
                d = '%04d-%02d-%02d' % t[:3]
                hist[d] = hist.get(d, 0) + 1
        print('%-12s %8s' % ('DAY', 'MEMBERS'))
        for d in sorted(hist):
            print('%-12s %8d' % (d, hist[d]))


if __name__ == '__main__':
    main(sys.argv)
