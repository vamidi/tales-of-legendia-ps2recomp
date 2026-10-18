# 02 — The disc and the volume

Reproduce with:

```
python tools/iso9660.py IMAGE.iso --pvd
python tools/iso9660.py IMAGE.iso
python tools/iso9660.py IMAGE.iso --csv
python tools/sector_map.py IMAGE.iso
```

Output: [`reports/iso-volume.txt`](../reports/iso-volume.txt),
[`reports/iso-tree.txt`](../reports/iso-tree.txt),
[`reports/iso-files.csv`](../reports/iso-files.csv),
[`reports/sector-map.txt`](../reports/sector-map.txt).

---

## Single layer, from the volume rather than from the file size

The image is 4,080,664,576 bytes. That is under the 4.7 GB a DVD-5 holds, so
the file size *suggests* one layer. The file size is not evidence — an image can
be truncated, padded, or unpacked from an archive that reported something else,
and on this corpus that has already cost a wrong estimate once. The volume
descriptor is evidence:

```
LBA 16     type 1    primary
  system id      PLAYSTATION
  volume id
  volume space   1992512 sectors (4080664576 bytes)
  block size     2048
  path table     58 bytes at LBA 257 (L) / 259 (M)
  publisher      NAMCO LTD.
  application    PLAYSTATION
  created        2005072313381900$
LBA 17     type 255  terminator
LBA 18     BEA01 / NSR02 / TEA01     UDF bridge
```

**`volume space` is 1,992,512 sectors.** A single-layer DVD holds 2,298,496
sectors of 2,048 bytes. 1,992,512 fits inside one layer with 305,984 sectors to
spare, so the disc is single layer — and the file size agrees only because
`1,992,512 × 2,048 = 4,080,664,576` to the byte. The image is exactly the volume
the disc declares, with no tail and no truncation.

Four details are worth stopping on.

**The volume identifier is blank.** Thirty-two spaces. Not `TALES_OF_LEGENDIA`,
not `SLPS_25533`. *Tales of Rebirth* did the same thing eight months earlier and
*Tales of Symphonia*'s nine CVM volumes did the opposite, shipping CRI's untouched
default `SAMPLE_GAME_TITLE`. Two studios, two ways of not filling in a field.

**The path table is 58 bytes**, where *Rebirth*'s was ten. Ten bytes is one
record and one record is the root. 58 bytes is five: the root and four
subdirectories. This is the first *Tales* disc in the corpus with a directory
tree rather than a flat root.

**The creation stamp carries a `$` offset byte**, decimal 36 — GMT plus
thirty-six quarter-hours, **UTC+9**, Japan. So `2005-07-23 13:38:19` is local
Tokyo time, and every one of the 37 files carries the same offset.

**There is a UDF bridge.** `BEA01`, `NSR02` and `TEA01` sit at LBA 18–20. Neither
*Rebirth* nor *Destiny 2* carries one.

### Where this disc sits in time

| Build | Volume stamped | Distance from Legendia |
|---|---|---|
| *Tales of Symphonia*, PlayStation 2 | 2004-08-17 | 11 months, 6 days earlier |
| *Tales of Rebirth* | 2004-11-17 | **8 months, 6 days earlier** |
| *Tales of Tactics*, i-appli | 2004-12-16 | **7 months, 7 days earlier** |
| ***Tales of Legendia*** | **2005-07-23** | — |

The gap that matters for the codec is the second one. *Rebirth* is the build
that proved the decoder source had forked per title; Legendia is eight months
downstream of it, on the same console, from the same studio.

---

## Thirty-seven files, four directories

Where *Rebirth* had seven files and no directories at all, this disc has a tree:

```
/SYSTEM.CNF                57
/SLPS_255.33          2972228
/CDVD.000          1073676288
/IRX/                          17 modules,   982469 bytes
/IRX/PAD2/                      1 module,     11725 bytes
/MBS/                           7 tables,     99390 bytes
/AFS/                           6 archives + 4 small index files
```

`SYSTEM.CNF` is 57 bytes and says the whole of it:

```
BOOT2 = cdrom0:\SLPS_255.33;1
VER = 1.01
VMODE = NTSC
```

So the product code is **SLPS-25533** and the version is **1.01** — read from
the boot configuration, not inferred from the file name, and not from the
`Tales of Legendia (Japan, Asia)` the archive was labelled with.

### The file stamps are a build chronology

All 37 carry real dates, and they separate cleanly into three groups:

| Group | Range | What it is |
|---|---|---|
| Sony SDK modules | **2004-05-11 16:13:53** (nine files, identical to the second) | shipped as received |
| one pad driver | 2004-06-14, one sound module 2005-06-06, one 2005-07-07 | middleware |
| **`CDVD.000`** | **2004-08-11 17:08:31** | a year adrift — see [08](08-cdvd000.md) |
| the game | **2005-05-28 → 2005-07-22** | the actual build |

The nine Sony modules sharing one timestamp to the second is what an SDK drop
looks like when nobody rebuilds it. The game's own files run:

| File | Stamp |
|---|---|
| `AFS/FLIST.DIR` | 2005-05-28 00:27:26 |
| `AFS/PTN_INFO.BIN`, `.NAM` | 2005-07-21 19:02:55 |
| `AFS/SYS_REG.AFS` | 2005-07-21 19:03:13 |
| `AFS/MAP.AFS` | 2005-07-21 19:06:00 |
| `AFS/SND.AFS` | 2005-07-21 19:18:36 |
| `AFS/FIELD.AFS` | 2005-07-21 19:19:09 |
| `AFS/BATTLE.AFS` | 2005-07-21 19:28:20 |
| `AFS/MOVIE.AFS` | 2005-07-21 19:29:40 |
| `MBS/*.MBS` | 2005-07-18 20:12:23 → 20:29:56 |
| `SLPS_255.33` | 2005-07-22 14:13:44 |
| `SYSTEM.CNF` | 2005-07-22 16:15:11 |
| the four directories | 2005-07-22 18:49:04 → 18:58:42 |
| *volume* | *2005-07-23 13:38:19* |

That is a coherent mastering session and not a set of stamps normalised by a
tool: the six archives were written in a 27-minute run on the evening of
2005-07-21, the index tables three days before them, the executable the next
afternoon, and the volume the morning after that.

**The one thing this disc does not have is a per-member chronology.** The CRI
`AFS` directory format reserves twelve bytes per member for a date, and on all
six archives here **every one of those bytes is zero**. 24,835 members, no
dates. The names survive and are excellent; the times do not. That is the
opposite of what the sibling discs gave, and it is recorded in
[99](99-open-questions.md) rather than smoothed over.

---

## Sector accounting

```
FIRST     LAST      SECTORS   WHAT
0         15        16        <system area>
16        17        2         volume descriptors
18        256       239       UDF bridge and its slack
257/259   —         2         path tables
262-265   —         4         the four directory records
315       315       1         SYSTEM.CNF
316       1767      1452      SLPS_255.33
1768      2255      488       the eighteen IRX modules
2256      2310      55        the index files and the MBS tables
2311      526566    524256    CDVD.000
526567    1982265   1455699   the six AFS archives
1982266   1992511   10246     all zero
```

**Slack is 10,537 sectors, 0.5288% of the disc**, and all but 291 sectors of it
is the single run of zeros after the last archive. There is **no interior gap**.

This is the third data point on a question the corpus has been carrying since
2004. *Tales of Symphonia*'s PlayStation 2 disc left 16.12% of itself empty,
686 MB of it in one interior hole, and that was published as unproven —
plausibly head separation, plausibly nothing. *Rebirth* left 0.478%, all after
the last file, and said that whatever Symphonia's hole was, it was not a studio
layout policy. Legendia leaves 0.529%, all after the last file, and agrees.

But it does something the other two did not, and it is worth stating precisely
because it is easy to mistake for the same thing. **`CDVD.000` occupies sectors
2311 to 526566 — 26.31% of the disc, in the innermost region, immediately after
the executable and before every archive.** A DVD reads its outer tracks faster
than its inner ones. Putting a gigabyte of unreferenced data at the start of the
disc pushes all 1.46 GB of real archives outward. Whether that was the intent or
whether the file is simply where the build system left it is not answerable from
the image, and both readings are in [08](08-cdvd000.md) and
[99](99-open-questions.md). What is measurable is that the effect is the same as
Symphonia's interior hole and the mechanism is not: Symphonia left sectors
unclaimed, and this disc declared a file.
