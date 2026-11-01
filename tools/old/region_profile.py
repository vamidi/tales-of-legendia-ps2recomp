"""What is actually inside a large opaque region, block by block.

Written for `CDVD.000`, a 1,073,676,288-byte file whose name and whose first
kilobyte of 0xFD fill both suggest padding, and which turns out to contain 256
distinct byte values.  A single histogram over a gigabyte hides everything; a
histogram every N megabytes shows the seams.

For each block it prints the dominant byte and how much of the block that byte
covers, plus a coarse entropy in bits per byte.  Padding is one byte at ~100%
and near-zero entropy; compressed or encrypted data is ~8 bits with no
dominant byte; structured game data sits in between.

    python tools/region_profile.py IMAGE --lba N --size N [--block 16777216]
"""

import math
import struct
import sys
from collections import Counter


def profile(fh, base, size, block):
    fh.seek(base)
    pos = 0
    rows = []
    while pos < size:
        n = min(block, size - pos)
        buf = fh.read(n)
        if not buf:
            break
        c = Counter(buf)
        top, topn = c.most_common(1)[0]
        ent = 0.0
        for v in c.values():
            p = v / len(buf)
            ent -= p * math.log2(p)
        rows.append((pos, len(buf), top, 100.0 * topn / len(buf), ent, len(c)))
        pos += len(buf)
    return rows


def main(argv):
    img = argv[1]
    base = int(argv[argv.index('--lba') + 1], 0) * 2048
    size = int(argv[argv.index('--size') + 1], 0)
    block = int(argv[argv.index('--block') + 1], 0) if '--block' in argv \
        else 16 << 20
    fh = open(img, 'rb')
    rows = profile(fh, base, size, block)
    print('%s  LBA %d  %d bytes  %d blocks of %d'
          % (img, base // 2048, size, len(rows), block))
    print()
    print('%14s %10s %6s %8s %7s %6s  %s'
          % ('OFFSET', 'BYTES', 'TOP', 'SHARE', 'ENTROPY', 'VALUES', 'READS AS'))
    for off, n, top, share, ent, nv in rows:
        if share > 99.0:
            what = 'fill'
        elif ent < 1.0:
            what = 'near-fill'
        elif ent > 7.5:
            what = 'compressed / high entropy'
        else:
            what = 'structured data'
        print('%14d %10d   0x%02X %7.2f%% %7.3f %6d  %s'
              % (off, n, top, share, ent, nv, what))
    print()
    kinds = {}
    for off, n, top, share, ent, nv in rows:
        k = ('fill' if share > 99.0 else 'near-fill' if ent < 1.0
             else 'compressed / high entropy' if ent > 7.5
             else 'structured data')
        kinds[k] = kinds.get(k, 0) + n
    tot = sum(kinds.values())
    print('%-28s %14s %8s' % ('SUMMARY', 'BYTES', 'SHARE'))
    for k in sorted(kinds, key=lambda x: -kinds[x]):
        print('%-28s %14d %7.2f%%' % (k, kinds[k], 100.0 * kinds[k] / tot))


if __name__ == '__main__':
    main(sys.argv)
