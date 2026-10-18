"""Read the MPEG-2 sequence header of every movie in `MOV.BIN`.

`MOV.BIN` has no header of its own.  It is 1.73 GB of MPEG-2 program stream laid
end to end, and the only thing that says where one movie stops and the next
starts is the index compiled into the executable -- which `binfs.py` reads.  So
this tool does not search for movie boundaries; it takes the ones the game
declares and checks them, which is a stronger test than a search would be: if
the index were wrong, the declared offset would not land on a pack header.

Every figure printed here comes out of the bitstream rather than out of a file
name or a guess.  Width and height are the twelve-bit fields of the sequence
header, the frame rate is its four-bit code, and the bit rate is the eighteen-bit
field times 400.  Duration is the file's length divided by that bit rate, so it
is the *muxed* duration including audio and padding and is an upper bound on the
video's own running time.

    python tools/mpeg.py IMAGE.iso
    python tools/mpeg.py IMAGE.iso --packs N     # decode the first N pack headers
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from binfs import Fs

PACK = b'\x00\x00\x01\xba'
SEQ = b'\x00\x00\x01\xb3'
GOP = b'\x00\x00\x01\xb8'

RATE = {1: '23.976', 2: '24', 3: '25', 4: '29.97', 5: '30',
        6: '50',7: '59.94', 8: '60'}
ASPECT = {1: 'square', 2: '4:3', 3: '16:9', 4: '2.21:1'}


def sequence(buf):
    """(w, h, aspect, rate, bitrate) from the first sequence header in buf."""
    j = buf.find(SEQ)
    if j < 0:
        return None
    h = buf[j + 4:j + 12]
    if len(h) < 8:
        return None
    w = (h[0] << 4) | (h[1] >> 4)
    ht = ((h[1] & 0x0F) << 8) | h[2]
    aspect = h[3] >> 4
    rate = h[3] & 0x0F
    bitrate = ((h[4] << 10) | (h[5] << 2) | (h[6] >> 6)) * 400
    return w, ht, aspect, rate, bitrate


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    fs = Fs(argv[1])
    print('%3s %13s %13s %10s %5s %8s %11s %9s'
          % ('#', 'OFFSET', 'SIZE', 'PICTURE', 'ASP', 'FPS', 'BIT/S', 'MUX SECS'))
    total = 0.0
    for i, o, s, _p in fs.members('MOV.BIN'):
        if s == 0:
            print('%3d %13d %13d   -- empty index slot --' % (i, o, s))
            continue
        buf = fs.read('MOV.BIN', o, 1 << 16)
        if not buf.startswith(PACK):
            print('%3d %13d %13d   NOT a pack header: %s'
                  % (i, o, s, buf[:4].hex()))
            continue
        q = sequence(buf)
        if not q:
            print('%3d %13d %13d   pack header, no sequence header in 64 KB'
                  % (i, o, s))
            continue
        w, ht, asp, rate, br = q
        secs = s * 8.0 / br if br else 0
        total += secs
        print('%3d %13d %13d %10s %5s %8s %11d %9.1f'
              % (i, o, s, '%dx%d' % (w, ht), ASPECT.get(asp, asp),
                 RATE.get(rate, '?'), br, secs))
    print()
    print('%d movies, %.1f seconds muxed (%.1f minutes) at the declared bit rate'
          % (sum(1 for m in fs.members('MOV.BIN') if m[2]), total, total / 60.0))


if __name__ == '__main__':
    main(sys.argv)
