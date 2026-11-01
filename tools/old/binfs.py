"""The three `.BIN` containers and the index the executable carries for them.

The disc has seven files and no directories.  Three of them are containers --
`MOV.BIN`, `DAT.BIN`, `FLD.BIN` -- holding 4.29 GB between them, and none of
the three has a header, a magic number, a member count or a name table.  The
index is compiled into `SLPS_254.50`, exactly as the 2002 title compiled the
index of `FILE.FPB` into `SLPS_251.72`, and it uses the same encoding:

    entry = 64-byte-aligned offset | trailing padding in the low six bits

so member *i* starts at `base(v[i])` and is `base(v[i+1]) - base(v[i]) -
pad(v[i])` bytes long, and the last entry of each table is a sentinel equal to
the container's own size.  That sentinel is how this tool finds the tables
without being told where they are: search the executable for a word equal to
the file's length in the ISO, then walk backwards for as long as the bases are
non-decreasing.  Nothing here is hard-coded to an address.

Members are not all one thing.  A member may be a nine-byte codec block, or a
`TIM2` texture stored raw, or an MPEG-2 program stream, or a nested archive of
`(offset, size)` pairs -- see `--census` for the breakdown, which classifies by
reading each member's first bytes rather than by assuming.

    python tools/binfs.py IMAGE.iso --tables
    python tools/binfs.py IMAGE.iso --list DAT.BIN [--limit N]
    python tools/binfs.py IMAGE.iso --census
    python tools/binfs.py IMAGE.iso --extract DAT.BIN INDEX OUT
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso9660 import Image, SECTOR

ALIGN = 64
MASK = ~(ALIGN - 1) & 0xFFFFFFFF

CONTAINERS = ('MOV.BIN', 'DAT.BIN', 'FLD.BIN')
EXE = 'SLPS_254.50'


def base(v):
    return v & MASK


def pad(v):
    return v & (ALIGN - 1)


def find_table(exe, size):
    """(file_offset, entries) of the index whose sentinel is `size`."""
    n = len(exe) // 4
    words = struct.unpack_from('<%dI' % n, exe, 0)
    out = []
    for i, v in enumerate(words):
        if v != size:
            continue
        j = i
        while j > 0 and base(words[j - 1]) <= base(words[j]):
            j -= 1
        if i - j >= 2:                      # a table, not a stray constant
            out.append((j * 4, words[j:i + 1]))
    if not out:
        return None, None
    off, tab = max(out, key=lambda t: len(t[1]))
    # A table's first entry encodes offset 0 plus that member's padding, so at
    # most one leading zero-based word belongs to it; more than one is the
    # alignment slack of whatever sits before the table in .data.
    while len(tab) > 2 and tab[0] == 0 and tab[1] == 0:
        tab = tab[1:]
        off += 4
    return off, tab


class Fs:
    def __init__(self, iso_path):
        self.img = Image(iso_path)
        self.fh = open(iso_path, 'rb')
        self.entries = {e.base: e for e in self.img.walk() if not e.is_dir}
        e = self.entries[EXE]
        self.fh.seek(e.lba * SECTOR)
        self.exe = self.fh.read(e.size)
        self.tables = {}
        for name in CONTAINERS:
            c = self.entries[name]
            off, tab = find_table(self.exe, c.size)
            self.tables[name] = (off, tab)

    def members(self, name):
        """[(index, offset, size, pad)] for one container."""
        _off, tab = self.tables[name]
        out = []
        for i in range(len(tab) - 1):
            o = base(tab[i])
            out.append((i, o, base(tab[i + 1]) - o - pad(tab[i]), pad(tab[i])))
        return out

    def read(self, name, off, n):
        self.fh.seek(self.entries[name].lba * SECTOR + off)
        return self.fh.read(n)


def adpcm(buf):
    """True if every 16-byte frame header in buf is a legal SPU-ADPCM one.

    A PlayStation 2 ADPCM frame is sixteen bytes: a shift/filter byte whose low
    nibble is a shift of at most 12 and whose high nibble is a filter of at most
    4, a flag byte of at most 7, then fourteen bytes of nibbles.  Nothing here
    is a magic number, so the test has to be the whole first block of frames
    rather than a signature -- which is also why it is only applied to members
    that begin with the silent all-zero frame these banks always open with.
    """
    n = len(buf) // 16
    if n < 8:
        return False
    for i in range(n):
        s, f = buf[i * 16], buf[i * 16 + 1]
        if (s & 0x0F) > 12 or (s >> 4) > 4 or f > 7:
            return False
    return True


def kind(head, size):
    """Classify a member from its first bytes.  Never from its position."""
    if len(head) < 9:
        return 'empty' if not head else 'short'
    if head[:4] == b'TIM2':
        return 'TIM2'
    if head[:4] == b'\x00\x00\x01\xba':
        return 'MPEG-PS'
    if head[:4] == b'MSCF':
        return 'MSCF'
    m = head[0]
    packed = struct.unpack_from('<I', head, 1)[0]
    unpacked = struct.unpack_from('<I', head, 5)[0]
    if m in (0, 1, 3) and 9 + packed <= size and unpacked and unpacked < (1 << 26):
        if m == 0:
            return 'block:stored'
        return 'block:%d' % m
    if head[:4] == b'\x00\x00\x00\x00':
        return 'SPU-ADPCM' if adpcm(head) else 'zero-lead'
    return 'raw'


def cmd_tables(fs):
    print('%-10s %10s %10s %12s  %s' % ('CONTAINER', 'INDEX@FILE', 'ENTRIES',
                                        'MEMBERS', 'SENTINEL'))
    for name in CONTAINERS:
        off, tab = fs.tables[name]
        print('%-10s 0x%08X %10d %12d  %d == %s size'
              % (name, off, len(tab), len(tab) - 1, tab[-1], name))
    print()
    print('index entries are 64-byte-aligned offsets with the member\'s')
    print('trailing padding packed into the low six bits')


def cmd_list(fs, name, limit):
    print('%6s %12s %12s %5s  %s' % ('#', 'OFFSET', 'SIZE', 'PAD', 'KIND'))
    for i, o, s, p in fs.members(name)[:limit]:
        h = fs.read(name, o, min(s, 4096)) if s >= 16 else fs.read(name, o, max(s, 0))
        print('%6d %12d %12d %5d  %s' % (i, o, s, p, kind(h, s)))


def cmd_census(fs):
    for name in CONTAINERS:
        ms = fs.members(name)
        counts, bytes_ = {}, {}
        packed_total = unpacked_total = 0
        biggest = (0, -1)
        for i, o, s, p in ms:
            h = fs.read(name, o, min(s, 4096)) if s >= 16 else b''
            k = kind(h, s)
            counts[k] = counts.get(k, 0) + 1
            bytes_[k] = bytes_.get(k, 0) + s
            if k.startswith('block:'):
                pk = struct.unpack_from('<I', h, 1)[0]
                un = struct.unpack_from('<I', h, 5)[0]
                packed_total += pk
                unpacked_total += un
                if pk > biggest[0]:
                    biggest = (pk, i)
        print('=== %s  %d members, %d bytes' % (name, len(ms),
                                                sum(m[2] for m in ms)))
        for k in sorted(counts, key=lambda x: -counts[x]):
            print('    %-14s %7d members %14d bytes' % (k, counts[k], bytes_[k]))
        if packed_total:
            print('    top-level codec blocks: %d packed -> %d unpacked (%.2fx)'
                  % (packed_total, unpacked_total,
                     unpacked_total / float(packed_total)))
            print('    largest packed block:   %d bytes, member %d'
                  % biggest)
        print('    padding declared by the index: %d bytes'
              % sum(m[3] for m in ms))
        print()


def cmd_extract(fs, name, idx, out):
    i, o, s, p = fs.members(name)[idx]
    open(out, 'wb').write(fs.read(name, o, s))
    print('%s member %d: offset %d, %d bytes, pad %d -> %s' % (name, i, o, s, p, out))


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    fs = Fs(argv[1])
    rest = argv[2:]
    if '--tables' in rest:
        cmd_tables(fs)
    elif '--census' in rest:
        cmd_census(fs)
    elif '--list' in rest:
        k = rest.index('--list')
        lim = int(rest[rest.index('--limit') + 1]) if '--limit' in rest else 40
        cmd_list(fs, rest[k + 1], lim)
    elif '--extract' in rest:
        k = rest.index('--extract')
        cmd_extract(fs, rest[k + 1], int(rest[k + 2], 0), rest[k + 3])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
