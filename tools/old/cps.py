"""The `CPS` container and its decompressor, as implemented by SLPS_255.33.

This is *not* the Tales block codec's nine-byte block.  It is the same engine
in a different envelope, and the difference is the point of this file, so both
halves are spelled out rather than assumed.

The envelope -- sixteen bytes, where the corpus format has nine:

    +0x00  "CPS\0"
    +0x04  u32   packed size, counting this header
    +0x08  u32   unpacked size
    +0x0C  u32   zero on every member of this disc
    +0x10  the token stream begins

There is no method byte.  The corpus format dispatches on one (0 stored,
1 LZSS, 3 LZSS + run escape) and every build from 1995 to 2004 reads it; this
one has nowhere to put it and the routine that consumes the stream takes only
(source, destination, length).  One path, always.

The engine, transcribed from the routine at VA 0x001F6CA0 instruction by
instruction:

  * a 4,096-byte ring, masked with 0x0FFF, held at 0x004A0AE0
  * the cursor starts at 4,078 -- RING - 18, the corpus's own constant
  * the ring is cleared to 4,078 zero bytes by an inline loop unrolled by
    eight, which is the 1997-2003 shape, and **nothing is preloaded into it**;
    the synthetic (i, 0x00) / (i, 0xFF) pairs every PlayStation-family build
    since 1997 writes before reading a token are not written here
  * control bits LSB first, 1 = literal, the register refilled as
    `byte | 0xFF00` so that eight bits are spent before the next refill
  * a match token is two bytes: the reference's low eight bits in the first,
    its high four in the *high* nibble of the second, and the length in the
    low nibble as `n + 3` -- range 3 to 18
  * no run escape.  There is no `+19` anywhere in the routine and no third
    token length, so the corpus's method-3 path has no counterpart here
  * the loop is driven by the *packed* size and stops when the input is
    exhausted, exactly as section 1 of the specification describes

`unpack` returns what the machine produces.  It does not consult the unpacked
size while decoding -- that field is only ever compared against the result,
which is what makes "N of N members decode to their declared length" a test
rather than a tautology.

    python tools/cps.py FILE.cps -o out.bin
    python tools/cps.py IMAGE.iso --at 0x1000 --census ...   (see afs_census.py)
"""

import struct
import sys

RING = 4096
MASK = RING - 1
CURSOR0 = RING - 18          # 4078
HDR = 16
MAGIC = b'CPS\x00'


def header(buf, off=0):
    """(packed, unpacked, reserved) or None if there is no CPS here."""
    if buf[off:off + 4] != MAGIC:
        return None
    packed, unpacked, resv = struct.unpack_from('<III', buf, off + 4)
    return packed, unpacked, resv


def unpack(buf, off=0, limit=None):
    """Decode the stream at `off`.  `limit` bounds the input, not the output."""
    h = header(buf, off)
    if h is None:
        raise ValueError('no CPS magic at 0x%X' % off)
    packed, unpacked, _resv = h
    end = off + (limit if limit is not None else packed)
    if end > len(buf):
        end = len(buf)

    ring = bytearray(RING)          # zeroed, and left that way -- no preload
    cur = CURSOR0
    ctrl = 0
    src = off + HDR
    out = bytearray()

    while src < end:
        nxt = ctrl >> 1
        if nxt & 0x0100:
            ctrl = nxt
        else:
            ctrl = buf[src] | 0xFF00
            src += 1
            if src >= end:
                break
        if ctrl & 1:                                  # literal
            b = buf[src]
            src += 1
            ring[cur] = b
            out.append(b)
            cur = (cur + 1) & MASK
        else:                                         # match
            if src + 1 >= end:
                break
            first = buf[src]
            second = buf[src + 1]
            src += 2
            ref = first | ((second & 0xF0) << 4)
            n = (second & 0x0F) + 3
            for k in range(n):
                b = ring[(ref + k) & MASK]
                ring[cur] = b
                out.append(b)
                cur = (cur + 1) & MASK
    return bytes(out)


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    buf = open(argv[1], 'rb').read()
    off = int(argv[argv.index('--at') + 1], 0) if '--at' in argv else 0
    h = header(buf, off)
    if h is None:
        raise SystemExit('no CPS magic at 0x%X' % off)
    packed, unpacked, resv = h
    out = unpack(buf, off)
    print('packed      %d' % packed)
    print('unpacked    %d  (declared)' % unpacked)
    print('reserved    %d' % resv)
    print('produced    %d  %s' % (len(out),
                                  'EXACT' if len(out) == unpacked else 'MISMATCH'))
    if unpacked:
        print('ratio       %.2f%%' % (100.0 * packed / unpacked))
    if '-o' in argv:
        open(argv[argv.index('-o') + 1], 'wb').write(out)
        print('wrote       %s' % argv[argv.index('-o') + 1])


if __name__ == '__main__':
    main(sys.argv)
