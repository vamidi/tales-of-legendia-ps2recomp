"""How the 4,080,664,576 bytes were spent, by content rather than by file.

A file listing tells you `MOVIE.AFS` is 890 MB.  That is not the same question
as "how much of this disc is video", because four of the six archives mix
kinds: BATTLE.AFS holds 2,957 compressed members and 2,544 ADX streams, and
SND.AFS holds three kinds at once.  So this walks every member of every
archive, classifies it by its own first sixteen bytes, and adds up bytes per
class.

Members are counted at their *stored* size, because that is what they cost the
disc.  The unpacked total is reported separately and is not part of the budget.

The one judgement call is `CDVD.000`, which is 26.3% of the disc, is stamped a
year before everything else, and is neither an archive nor padding.  It gets
its own line rather than being folded into either.

    python tools/disc_budget.py IMAGE.iso
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from afs_census import ARCHIVES, is_stored, members, sniff

SECTOR = 2048
IMAGE_BYTES = 4080664576

# Everything on the volume that is not one of the six AFS archives.
OTHER = [
    ('CDVD.000', 2311, 1073676288, 'unreferenced build artefact'),
    ('SLPS_255.33', 316, 2972228, 'executable'),
    ('IRX/IOPRP300.IMG', 1866, 275921, 'I/O processor'),
    ('IRX/INET.IRX', 1783, 141653, 'I/O processor'),
    ('IRX/KORGIVAG.IRX', 2037, 128390, 'I/O processor'),
    ('IRX/MCMAN.IRX', 2129, 95909, 'I/O processor'),
    ('IRX/IXAUDIO.IRX', 2001, 72336, 'I/O processor'),
    ('IRX/NETCNF.IRX', 2180, 65909, 'I/O processor'),
    ('IRX/USBD.IRX', 2238, 34985, 'I/O processor'),
    ('IRX/KORGUMDI.IRX', 2100, 29897, 'I/O processor'),
    ('IRX/LIBSD.IRX', 2115, 28661, 'I/O processor'),
    ('IRX/INETCTL.IRX', 1853, 24765, 'I/O processor'),
    ('IRX/SMAP.IRX', 2229, 17017, 'I/O processor'),
    ('IRX/DBCMAN.IRX', 1768, 15653, 'I/O processor'),
    ('IRX/DEV9.IRX', 1776, 14301, 'I/O processor'),
    ('IRX/PAD2/DS2U_D.IRX', 2213, 11725, 'I/O processor'),
    ('IRX/SIO2D.IRX', 2219, 11289, 'I/O processor'),
    ('IRX/MCSERV.IRX', 2176, 7417, 'I/O processor'),
    ('IRX/SIO2MAN.IRX', 2225, 6641, 'I/O processor'),
    ('MBS/SND.MBS', 2277, 65044, 'index'),
    ('MBS/BATTLE.MBS', 2259, 22004, 'index'),
    ('MBS/FIELD.MBS', 2270, 7324, 'index'),
    ('MBS/MAP.MBS', 2274, 3160, 'index'),
    ('MBS/SYS_REG.MBS', 2309, 1720, 'index'),
    ('MBS/MOVIE.MBS', 2276, 88, 'index'),
    ('MBS/DUMMY.MBS', 2310, 50, 'index'),
    ('AFS/FLIST.DIR', 2258, 95, 'index'),
    ('AFS/PTN_INFO.NAM', 2257, 75, 'index'),
    ('AFS/PTN_INFO.BIN', 2256, 64, 'index'),
    ('AFS/DUMMY.AFS', 526567, 50, 'index'),
    ('SYSTEM.CNF', 315, 57, 'boot'),
]

CLASS = {
    'MPEG program stream': 'video (MPEG-2)',
    'ADX': 'audio (CRI ADX)',
    'CPS (compressed)': 'compressed game data',
    'CPS (stored, raw payload)': 'stored game data',
}


def main(argv):
    fh = open(argv[1], 'rb')
    per = {}
    unpacked = 0
    rows = []
    for name, lba, asize in ARCHIVES:
        base, ents = members(fh, lba)
        kinds = {}
        for off, msize in ents:
            if msize == 0:
                continue
            fh.seek(base + off)
            head = fh.read(16)
            k = sniff(head)
            if k.startswith('CPS'):
                k = 'CPS (stored, raw payload)' if is_stored(head) \
                    else 'CPS (compressed)'
                p, u, _r = struct.unpack('<III', head[4:16])
                unpacked += u
            cl = CLASS.get(k, 'other / unidentified')
            kinds[cl] = kinds.get(cl, 0) + msize
            per[cl] = per.get(cl, 0) + msize
        # the archive's own overhead: extent table, directory, sector padding
        used = sum(s for _o, s in ents)
        over = asize - used
        per['archive overhead'] = per.get('archive overhead', 0) + over
        rows.append((name, asize, kinds, over))

    for name, lba, sz, what in OTHER:
        cl = {'unreferenced build artefact': 'CDVD.000 (see docs/06)',
              'executable': 'executable',
              'I/O processor': 'I/O processor modules',
              'index': 'index and boot files',
              'boot': 'index and boot files'}[what]
        per[cl] = per.get(cl, 0) + sz

    print('%-28s %14s %8s' % ('ARCHIVE', 'BYTES', 'SHARE'))
    for name, asize, kinds, over in rows:
        print('%-28s %14d %7.2f%%' % (name, asize, 100.0 * asize / IMAGE_BYTES))
        for k in sorted(kinds, key=lambda x: -kinds[x]):
            print('    %-24s %14d %7.2f%%'
                  % (k, kinds[k], 100.0 * kinds[k] / IMAGE_BYTES))
        if over:
            print('    %-24s %14d %7.2f%%'
                  % ('(archive overhead)', over, 100.0 * over / IMAGE_BYTES))
    print()
    print('%-28s %14s %8s' % ('BY CONTENT', 'BYTES', 'SHARE'))
    tot = 0
    for k in sorted(per, key=lambda x: -per[x]):
        print('%-28s %14d %7.2f%%' % (k, per[k], 100.0 * per[k] / IMAGE_BYTES))
        tot += per[k]
    slack = IMAGE_BYTES - tot
    print('%-28s %14d %7.2f%%' % ('slack (unclaimed sectors)', slack,
                                  100.0 * slack / IMAGE_BYTES))
    print('%-28s %14d %7.2f%%' % ('TOTAL', tot + slack, 100.0))
    print()
    av = per.get('video (MPEG-2)', 0) + per.get('audio (CRI ADX)', 0)
    print('video + audio            %14d %7.2f%% of the disc'
          % (av, 100.0 * av / IMAGE_BYTES))
    print('  Rebirth 2004 (video + voice)              67.20%% of 4.51 GB')
    print('  Tales of Tactics 2004 (all media)         63.30%% of 369 KB')
    print()
    print('compressed game data unpacks to %d bytes' % unpacked)


if __name__ == '__main__':
    main(sys.argv)
