"""What the audio and video on this disc actually are, and who they belong to.

Three CRI formats ship here:

  Sofdec  an MPEG-2 program stream with CRI's own multiplex, named `.sfd`
  ADX     CRI's ADPCM -- music, ambience and most of the voice
  AHX     CRI's MPEG-2 layer II voice codec, named `.AHX` and wrapped in a
          four-byte `TLPS` chunk that is not CRI's

Classification is by magic, never by name.  The AFS directory truncates every
member name to 32 bytes, so `SND_COMMON_GLOBAL_FLOOR00.mcd.cps` arrives with
its extension cut off and a census keyed on the extension silently misfiles
thousands of members.  The magic is never truncated.  The name is used only
for its prefix.

The ADX header is read for its real parameters rather than assumed -- block
size, bit depth, channel count, sample rate and total sample count are all in
the first sixteen bytes -- so the playing time of every ADX on the disc is
arithmetic rather than an estimate.

The last table is the interesting one.  `SND.AFS`'s members are named by a
short prefix and the prefixes are the party: SEN, SHI, WIL, CHL, NOR, MOS, JAY
and the rest.  Counting members and seconds per prefix turns a pile of audio
into a cast list with line counts.

    python tools/media_census.py IMAGE.iso
    python tools/media_census.py IMAGE.iso --voice     # the cast table only
"""

import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afs_census import members

SECTOR = 2048
ARCHIVES = [
    ('SND.AFS', 1688249),
    ('BATTLE.AFS', 1365382),
    ('MOVIE.AFS', 526568),
]

WHO = {
    'SEN': 'Senel', 'SHI': 'Shirley', 'WIL': 'Will', 'CHL': 'Chloe',
    'NOR': 'Norma', 'MOS': 'Moses', 'JAY': 'Jay', 'GUR': 'Grune',
    'HAR': 'Harriet', 'SCH': 'Schwartz', 'STE': 'Stella', 'VAC': 'Vaclav',
    'SND': 'music / ambience', 'VO': 'battle voice', 'SE': 'sound effect',
    'MUS': 'music', 'MOVIE': 'movie audio',
}

FPS = {1: '23.976', 2: '24', 3: '25', 4: '29.97', 5: '30', 6: '50',
       7: '59.94', 8: '60'}


def names_of(fh, base, count):
    fh.seek(base + 8 + count * 8)
    d_off, d_size = struct.unpack('<II', fh.read(8))
    if not d_off or d_size < count * 48:
        return [''] * count
    fh.seek(base + d_off)
    blk = fh.read(count * 48)
    return [blk[i * 48:i * 48 + 32].split(b'\x00')[0].decode('ascii', 'replace')
            for i in range(count)]


def adx_header(h):
    """(channels, rate, samples, blocksize, bitdepth) or None."""
    if h[:2] != b'\x80\x00':
        return None
    blocksize, bitdepth, chans = h[5], h[6], h[7]
    rate = struct.unpack_from('>I', h, 8)[0]
    samples = struct.unpack_from('>I', h, 12)[0]
    if not (1 <= chans <= 8) or not (4000 <= rate <= 96000):
        return None
    return chans, rate, samples, blocksize, bitdepth


def mpeg_seq(buf):
    i = buf.find(b'\x00\x00\x01\xb3')
    if i < 0 or i + 12 > len(buf):
        return None
    b = buf[i + 4:i + 12]
    return ((b[0] << 4) | (b[1] >> 4),
            ((b[1] & 0x0F) << 8) | b[2],
            b[3] >> 4, b[3] & 0x0F,
            (b[4] << 10) | (b[5] << 2) | (b[6] >> 6))


def main(argv):
    fh = open(argv[1], 'rb')
    voice_only = '--voice' in argv
    kinds = collections.Counter()
    kbytes = collections.Counter()
    rates = collections.Counter()
    chans = collections.Counter()
    per = collections.defaultdict(lambda: [0, 0, 0, 0.0])  # adx, ahx, bytes, sec
    total_sec = 0.0
    per_archive = collections.defaultdict(collections.Counter)

    for aname, lba in ARCHIVES:
        base, ents = members(fh, lba)
        nm = names_of(fh, base, len(ents))
        for i, (off, size) in enumerate(ents):
            if size == 0:
                continue
            fh.seek(base + off)
            head = fh.read(32)
            n = nm[i]
            p = n.split('_')[0] if n else '(unnamed)'
            if head[:4] == b'\x00\x00\x01\xba':
                k = 'Sofdec (MPEG-2 program stream)'
            elif head[:4] == b'TLPS':
                k = 'AHX (CRI voice, TLPS-wrapped)'
                per[p][1] += 1
                per[p][2] += size
            elif head[:4] == b'CPS\x00':
                k = 'CPS (not audio -- counted elsewhere)'
            else:
                a = adx_header(head)
                if a:
                    ch, rate, smp, _bs, _bd = a
                    sec = smp / float(rate)
                    total_sec += sec
                    rates[rate] += 1
                    chans[ch] += 1
                    k = 'ADX (CRI ADPCM)'
                    per[p][0] += 1
                    per[p][2] += size
                    per[p][3] += sec
                else:
                    k = 'unidentified'
            kinds[k] += 1
            kbytes[k] += size
            per_archive[aname][k] += size

    if not voice_only:
        print('%-34s %8s %14s' % ('KIND', 'MEMBERS', 'BYTES'))
        for k in sorted(kinds, key=lambda x: -kbytes[x]):
            print('%-34s %8d %14d' % (k, kinds[k], kbytes[k]))
        print()
        for a in ARCHIVES:
            print('%-14s %s' % (a[0], '  '.join(
                '%s=%d' % (k.split(' ')[0], v)
                for k, v in per_archive[a[0]].most_common())))
        print()
        print('ADX sample rates : %s'
              % ', '.join('%d Hz x%d' % (r, n) for r, n in sorted(rates.items())))
        print('ADX channels     : %s'
              % ', '.join('%d ch x%d' % (c, n) for c, n in sorted(chans.items())))
        print('ADX playing time : %.1f seconds = %.2f hours'
              % (total_sec, total_sec / 3600.0))
        print()
        print('MOVIE.AFS sequence headers')
        base, ents = members(fh, 526568)
        nm = names_of(fh, base, len(ents))
        print('%-14s %12s %10s %8s %14s'
              % ('NAME', 'BYTES', 'SIZE', 'FPS', 'BIT RATE'))
        for i, (off, size) in enumerate(ents):
            fh.seek(base + off)
            s = mpeg_seq(fh.read(1 << 16))
            if s:
                w, h, _ar, fr, br = s
                print('%-14s %12d %10s %8s %14s'
                      % (nm[i], size, '%dx%d' % (w, h), FPS.get(fr, '?'),
                         '%d bit/s' % (br * 400)))
            else:
                print('%-14s %12d %10s' % (nm[i], size, '(no seq header)'))
        print()

    print('Per name prefix.  ADX members carry a header and therefore a')
    print('playing time; AHX members do not, so they count but do not time.')
    print()
    print('%-12s %7s %7s %14s %11s  %s'
          % ('PREFIX', 'ADX', 'AHX', 'BYTES', 'ADX SEC', 'READS AS'))
    for p in sorted(per, key=lambda x: -per[x][2]):
        a, x, b, s = per[p]
        if a + x < 2 and b < 1 << 20:
            continue
        print('%-12s %7d %7d %14d %11.1f  %s'
              % (p, a, x, b, s, WHO.get(p, '')))


if __name__ == '__main__':
    main(sys.argv)
