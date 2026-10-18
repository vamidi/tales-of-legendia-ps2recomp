# 03 — Containers

Reproduce with:

```
python tools/afs.py IMAGE.iso --lba 1981234 --size 2113536 --list
python tools/afs_census.py IMAGE.iso --headers
python tools/leftovers.py IMAGE.iso --sweep
```

Output: [`reports/afs-census.txt`](../reports/afs-census.txt),
[`reports/afs-sysreg-list.txt`](../reports/afs-sysreg-list.txt),
[`reports/magic-sweep.txt`](../reports/magic-sweep.txt).

---

## Four titles, four mechanisms

The corpus has now seen four different answers to "how does this disc find a
file", from one studio inside three years:

| Build | Mechanism |
|---|---|
| *Destiny 2*, 2002 | `FILE.FPB` — 64-byte-aligned offsets, padding packed into the low six bits |
| *Symphonia* PS2, 2004 | nine CRI **`CVM`** volumes, read by `ROFS` on the I/O processor |
| *Rebirth*, 2004 | three headerless `.BIN` blobs, indexed by `u32` tables in `.data`, `FILE.FPB`'s encoding |
| ***Legendia*, 2005** | six CRI **`AFS`** archives, indexed by parallel `MBS` tables |

Legendia is the fourth. It is not *Rebirth*'s and it is not *Destiny 2*'s: there
is **no `FILE.FPB`** anywhere in 4.08 GB, **no `SCPK` bundle**, and no `u32`
extent table in `.data` with padding in the low six bits. The offsets here are
plain, and the archive carries its own directory.

---

## `AFS`, and what it is

`AFS` is CRI Middleware's plain archive — the simplest thing in the catalogue
that also supplies ADX, Sofdec and `ROFS`. It has no compression of its own:

```
+0x00  "AFS\0"
+0x04  u32   member count
+0x08  count x (u32 offset, u32 size)
+...   u32 offset, u32 size            the directory block's own extent
...
directory block: count x 48 bytes
  +0x00  char[32]  name
  +0x20  u16 year, month, day, hour, minute, second
  +0x2C  u32
```

Everything little-endian, and every member starts on a 2,048-byte boundary
because the archive is meant to be read a sector at a time off a DVD.

Six of them:

| Archive | LBA | Bytes | Members |
|---|---:|---:|---:|
| `SYS_REG.AFS` | 1,981,234 | 2,113,536 | 430 |
| `FIELD.AFS` | 1,348,178 | 35,233,792 | 1,831 |
| `MAP.AFS` | 961,346 | 792,231,936 | 790 |
| `BATTLE.AFS` | 1,365,382 | 661,231,616 | 5,501 |
| `SND.AFS` | 1,688,249 | 600,033,280 | 16,261 |
| `MOVIE.AFS` | 526,568 | 890,425,344 | 22 |
| | | **2,981,269,504** | **24,835** |

**The directories carry names and no dates.** Every one of the twelve date bytes
on all 24,835 members is zero. The names are excellent and are used throughout
this repository — `field_grd0c_x00y00.mcd.cps`,
`scr_1twn_00mati_61i_0hir.mcd.cps`, `menu_pic_item_765kg_hammer.mcd.cps`,
`SND_ENVIRONMENTAL_SOUND_BEACH.mcd.cps` — but there is no per-asset chronology
on this disc, which is what the sibling pipelines got their best material from.
[99](99-open-questions.md) records that rather than working around it.

The 32-byte name field truncates. `SND_COMMON_GLOBAL_FLOOR00.mcd.cps` arrives as
`SND_COMMON_GLOBAL_FLOOR00.mcd.cp`, so the extension is not reliable and nothing
in this repository classifies a member by it — every census keys on the magic,
which is never truncated. Getting that wrong the first time misfiled 13,000
members, and the corrected numbers are in [07](07-media-and-budget.md).

---

## The `MBS` tables, and what they are for

Beside the archives, in their own directory, sit seven small files:

```
MBS/SYS_REG.MBS     1720 = 430 x 4
MBS/MAP.MBS         3160 = 790 x 4
MBS/FIELD.MBS       7324 = 1831 x 4
MBS/BATTLE.MBS     22004 = 5501 x 4
MBS/SND.MBS        65044 = 16261 x 4
MBS/MOVIE.MBS         88 = 22 x 4
MBS/DUMMY.MBS         50
```

Every one is **exactly four bytes per member of the archive it is named after**.
The contents are not offsets — the `AFS` header already has those. They are
sizes, and specifically:

> **`MBS[i]` is the number of bytes member *i* occupies once it is in memory.**

For a compressed member that is the `CPS` header's unpacked size. For anything
else — ADX, AHX, Sofdec, a stored `CPS` — it is the member's stored size. Which
means the game can size every allocation before it opens anything, from a table
totalling 97 KB, without seeking into a 792 MB archive to read a header.

That is a claim worth checking rather than asserting, so it was checked on all
24,835 members:

| Archive | members | `MBS[i]` correct |
|---|---:|---:|
| `SYS_REG.AFS` | 430 | 430 |
| `FIELD.AFS` | 1,831 | 1,831 |
| `MAP.AFS` | 790 | **789** |
| `BATTLE.AFS` | 5,501 | 5,501 |
| `SND.AFS` | 16,261 | 16,261 |
| `MOVIE.AFS` | 22 | 22 |
| | **24,835** | **24,834** |

**One member disagrees.** `MAP.AFS[547]`, `scr_2evt_04kaii_00f_0hir.mcd.cps`:
its `CPS` header declares 5,597,952 unpacked bytes, the decoder produces exactly
5,597,952, and `MAP.MBS[547]` says **5,597,888** — sixty-four bytes short. The
table is the one that is wrong, and it is wrong in the direction that
under-allocates. One in 24,835. See [99](99-open-questions.md).

---

## The `CPS` envelope

Members that are compressed carry a sixteen-byte header:

```
+0x00  "CPS\0"
+0x04  u32   packed size, including this header
+0x08  u32   unpacked size
+0x0C  u32   zero on every member of this disc
+0x10  the token stream
```

The corpus's format has **nine** bytes and its first is a method byte — `0`
stored, `1` LZSS, `3` LZSS with a run escape — which every build from 1995 to
2004 reads. **There is no method byte here.** The routine that consumes the
stream takes `(source, destination, length)` and has one path. What that means
for the codec is [05](05-block-codec.md); what it means for the container is
that a stored member has to be recognised some other way, and the only test the
header supports is arithmetic:

```
packed - 16 == unpacked   ->  the payload is raw
```

On 3,353 members that holds to the byte, and it has to be how the game decides,
because nothing else in the sixteen bytes distinguishes them. 2,957 of those are
the whole of `BATTLE.AFS`'s compressed-member population — every model and
texture the battle system loads is stored raw — and 388 are in `SND.AFS`.

| Kind | Members | Packed | Unpacked |
|---|---:|---:|---:|
| `CPS`, compressed | **3,078** | 826,535,905 | 1,415,546,432 |
| `CPS`, stored | **3,353** | 614,523,984 | — |
| ADX | 15,643 | 552,009,757 | — |
| AHX (`TLPS`-wrapped) | 2,739 | 70,160,470 | — |
| Sofdec | 22 | 890,421,248 | — |

---

## Three index files, and one that disagrees with the others

`AFS/` holds three small files beside the archives.

**`PTN_INFO.BIN`**, 64 bytes, magic `EX2B`:

```
EX2B, count 6, then six pairs of (member count, index):
   (430, 0)  (790, 1)  (16261, 2)  (1831, 3)  (5501, 4)  (22, 5)
```

**`PTN_INFO.NAM`**, 75 bytes, magic `MSGE`: six string offsets and six names, in
the same order — `SYS_REG`, `MAP`, `SND`, `FIELD`, `BATTLE`, `MOVIE`. Together
the two files are the archive table: which archive is index *n*, what it is
called, and how many members it has, so the loader can validate a header before
trusting it.

**`FLIST.DIR`**, 95 bytes, is not that. It is plain text with CRLF line endings:

```
\AFS\SYS_REG.AFS
\AFS\FIELD.AFS
\AFS\SND.AFS
\AFS\MAP.AFS
\AFS\BATTLE.AFS
\AFS\MOVIE.AFS
```

Backslashes, so these are DOS or Windows paths, not the `cdrom0:\` the executable
actually uses. And **the order is not `PTN_INFO`'s** — `FIELD` and `MAP` have
swapped places. Two files on one disc list the same six archives in two
different orders, which means at most one of them is what the game reads. It is
also three weeks older than everything else in `AFS/` (2005-05-28 against
2005-07-21). It reads as a build-time file list that got mastered along with the
data. [09](09-leftovers.md).

---

## CRI came back

*Tales of Rebirth*, eight months earlier, was the first clean disc: zero `MSCF`,
zero `CVMH`, zero `ROFSBLD`, zero `SAMPLE_GAME_TITLE`, no CRI middleware of any
kind. The reading offered at the time was that *Symphonia*'s nine CVM volumes
were an artefact of its GameCube port and the studio's own habit was the 2002
one.

This disc does not support that reading. Sweeping all 4,080,664,576 bytes:

| Pattern | Hits | What they are |
|---|---:|---|
| `ROFSBLD` | **0** | — |
| `SAMPLE_GAME_TITLE` | **0** | — |
| `PUBLISHER_NAME` | **0** | — |
| `SCPK` | **0** | — |
| `THEIRSCE` | **0** | — |
| `VAGp` | **0** | — |
| `FILE.FPB` | **0** | — |
| `MSCF` | 1 | inside `MAP.AFS` payload — see below |
| `CVMH` | 1 | inside `MAP.AFS` payload |
| `@UTF` | 1 | inside `MAP.AFS` payload |
| `TIM2` | 3 | inside `MAP.AFS` payload |
| `AFS\0` | 39 | the six archive headers and 33 in payload |
| `CPS\0` | **7,868** | the real ones, plus payload noise |
| `ADX` | 3,145 | the text, not the magic |
| `KORG` | 35 | the two I/O processor modules and their strings |

**The four-byte patterns with a single hit are noise, and the arithmetic says
so before the inspection does.** A specific four-byte string appears in
4,080,664,576 random bytes about `4.08e9 / 2^32 = 0.95` times. `MSCF` got 1,
`CVMH` got 1, `@UTF` got 1, `TIM2` got 3. Every one of them was then looked at,
and every one sits in the middle of a high-entropy `CPS` payload with random
bytes on both sides — `...4942423841465344ff454d5343464f4c31ff...` for the
`MSCF`. None is aligned, none has a plausible header after it, and none is at a
member boundary. The same test disposes of the three-byte patterns: `h4m` got
214 hits against an expected `4.08e9 / 2^24 = 243`.

So the negative holds — no cabinet, no CVM volume, no `ROFS` — but the positive
is much larger than *Rebirth*'s was. This disc runs **ADX** for its music,
**AHX** for its voice, **Sofdec** for its movies, **AFS** for its containers,
and it links CRI's volume filesystem API as well: `cvFsAddVolumeEx`,
`cvFsSetCurVolume`, `cvFsGetVolumeInfo`, `cvFsGetFsys64Info`, thirty-odd entry
points' worth of error strings sitting in `SLPS_255.33` for a `.CVM` file that
is not on the disc. [04](04-executable.md).

Two builds, eight months apart, from one studio: one with no CRI at all and one
with four of its products and a fifth linked unused. Whatever the 2004
divergence was, it was not the company adopting or dropping a middleware stack.
