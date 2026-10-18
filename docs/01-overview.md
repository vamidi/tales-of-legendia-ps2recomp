# 01 — Overview

*Tales of Legendia*, PlayStation 2, **SLPS-25533**, `VER = 1.01`, NTSC. One
single-layer DVD of **1,992,512 sectors and 4,080,664,576 bytes**, whose volume
descriptor is stamped **2005-07-23 13:38:19 +9**.

Everything below was produced by running the tools in [`tools/`](../tools/) on
an image supplied separately. Their output is in [`reports/`](../reports/).

---

## The disc in one screen

| | |
|---|---|
| Product code | **SLPS-25533**, from `SYSTEM.CNF`, not from the file name |
| Media | one **single-layer** DVD — 1,992,512 sectors, **4,080,664,576 bytes** |
| Volume | ISO 9660 + UDF bridge; publisher `NAMCO LTD.`, **volume identifier blank** |
| Volume stamped | **2005-07-23 13:38:19 +9** — 8 months after *Rebirth*, 7 after *Tactics* |
| File system | **37 files, 4 directories** — where *Rebirth* had seven files and none |
| Executable | `SLPS_255.33`, R5900, 2,972,228 bytes, `.comment` = `MW MIPS C Compiler (2.4.1.01)` |
| I/O processor | **sixteen `.IRX` modules** plus Sony's `IOPRP300.IMG` bundle, including **KORG**'s two |
| Containers | six **CRI `AFS`** archives, 24,835 members, indexed by parallel `MBS` tables |
| Compressed member | **`CPS`** — a *sixteen*-byte header, where the corpus format has nine |
| Block codec | **yes**, on the Emotion Engine only — **4,508 of 4,508 members decode** |
| Ring-clear constant | **4078** — the 1997–2003 shape, not 2004's 4080 or 4079 |
| Decoder vs its neighbours | **21 bytes** shared with *Symphonia* PS2, against **2,420** of C runtime |
| `MSCF` / `ROFSBLD` / `SCPK` / `THEIRSCE` | **zero** real occurrences in 4.08 GB |
| Cross-title data | **none** — second consecutive clean disc |
| Disc used | **99.47%**, no interior gap — but 26.31% of it is one unreferenced file |

---

## Six answers

**1 — The codec is here, and it came from before the fork.** The constant scan
returns one `4078` in `SLPS_255.33` and it is the decoder: an inline zero loop
unrolled by eight, ring base `0x004A0AE0`, mask 4095, cursor at `RING − 18`.
That is the shape every build from 1997 to 2003 has. It is *not* Symphonia
2004's quadword `bzero` with **4080**, and it is *not* Rebirth 2004's library
`memset` with **4079** out of a factored `ring_init`. A fourth PlayStation 2
build in thirty-two months clears the same array a fourth way — by going back to
the first one. [→ 05](05-block-codec.md)

**2 — And the source is nobody's cousin.** 872 bytes of Legendia's decoder,
searched for at any alignment in whole executables, returns **20 bytes** against
*Rebirth*, **21** against *Symphonia* PS2, **16** against *Destiny 2*. The number
that makes those numbers mean something is the third column: `VENUS.ELF`, the
corpus's own negative control — an executable that provably contains no decoder
at all — scores **20**. Legendia's decoder resembles its siblings' exactly as
much as a file with no decoder in it does. And byte equality was available:
872 bytes of Legendia's *own C runtime*, same needle length, same tool, matches
*Symphonia* for the whole needle, and widening it shows the real run is **2,420
contiguous bytes**. Two executables that agree for 2,420 bytes of library code
agree for 21 bytes of decoder. [→ 06](06-decoder-lineage.md)

**3 — The format moved. The engine did not.** This is the first disc in the
corpus where those two facts separate. The nine-byte block header — `u8` method,
`u32` packed, `u32` unpacked — is **not on this disc**, and the unmodified
reference decoder finds **zero blocks** in 13.8 MB of samples while finding 15
and 30 in the control files. What is here instead is `CPS`: four magic bytes,
packed, unpacked, and a reserved word that is zero every time. Sixteen bytes,
and **no method byte at all**. Inside that envelope the machine is the corpus's
to the instruction: 4,096-byte ring, `& 0x0FFF`, control bits LSB first with
`1` = literal, refill as `byte | 0xFF00`, a two-byte match token with the
reference's high four bits in the *high* nibble of the second byte, and
`length = nibble + 3`. Two things are gone: the **synthetic preload** — the ring
is cleared to 4,078 zeros and nothing is written into it — and the **run
escape**, there being no `+19` anywhere in the routine. [→ 05](05-block-codec.md)

**4 — CRI came back, all of it.** *Rebirth*, eight months earlier, had zero CRI
middleware and no `CVMH`, no `ROFSBLD`, no `SAMPLE_GAME_TITLE`. This disc
indexes everything through six **CRI `AFS`** archives, streams its music and
voice as **ADX** and **AHX**, plays its twenty-two movies as **Sofdec**, and
links CRI's `cvFs*` volume API — `cvFsAddVolumeEx`, `cvFsSetCurVolume`,
`cvFsGetVolumeInfo` — although not one `.CVM` file is on the disc. So 2004 was
not a direction. Symphonia's CVMs were not an artefact of its GameCube port
either. Two studios' worth of habits were running side by side.
[→ 03](03-containers.md)

**5 — A gigabyte that is not padding, and is not referenced.** `CDVD.000` is
1,073,676,288 bytes, **26.31% of the disc**, stamped **2004-08-11** where every
other file is stamped 2005. Its first kilobyte is `0xFD` fill and its last 608
MB are a two-valued pattern, which is what made it look like mastering padding.
It is not. Between those two it holds **1,430 `CPS` members on 2,048-byte
boundaries**, 350.3 MB packed, and **every one of them decodes exactly** —
683.1 MB out. It also contains code stamped `MW MIPS` and a `TLPKv1+50` tag. No
index on the disc points at it. It is an earlier build's asset set, left in
place. [→ 08](08-cdvd000.md)

**6 — The soundtrack is 136,763 bytes, and that is why the budget breaks.**
*Rebirth* spent 67.2% of 4.5 GB on video and voice; *Tales of Tactics* spent
63.3% of its 369 KB on media. Legendia spends **37.70%**, or **51.54%** with
`CDVD.000` excluded from the denominator as the dead weight it is. A game known
for its score allocates proportionally *less* of its disc to sound than its
predecessor did — because a substantial part of that score is **thirty MIDI
sequences totalling 136,763 bytes**, played by the KORG driver out of 25.8 MB of
sample banks, where *Rebirth* streamed 1.30 GB of SPU-ADPCM. What *is* streamed
here is mostly voice: 399.8 MB of it, against 222.3 MB of music and ambience,
and **13.97 hours** of ADX playing time by the headers' own sample counts. A
large soundtrack and a small audio budget are the same fact seen from two sides.
[→ 07](07-media-and-budget.md)

### And the archaeology

A devkit path left in the executable, `host0:C:\tox\fieldwork\dev\field\`, which
names the project root **`tox`**; a second, `HST:host0:R:\`; the memory-card
string `BISLPS-25533TOL-S` with **the real product code compiled in**, where
*Rebirth* shipped `BISLPS-00000ToRsv%02d` with the code left as zeroes; a
`FLIST.DIR` on the disc holding six DOS paths in plain CRLF text; **network
modules** — `INET.IRX`, `SMAP.IRX`, `NETCNF.IRX`, `DEV9.IRX` — in a
single-player role-playing game; a **KORG** sound driver on the I/O processor,
in a series that had shipped Sony's; and two gaps in the movie numbering, `mov_03`
and `mov_16`, in a set that runs to 24. [→ 09](09-leftovers.md)

---

## Where to go

| | |
|---|---|
| [02 — The disc and the volume](02-disc-and-volume.md) | single layer from the volume, 37 files, the sector accounting |
| [03 — Containers](03-containers.md) | `AFS`, the `MBS` tables, the `CPS` envelope, and CRI's return |
| [04 — The executables](04-executable.md) | the ELF, the compiler stamp, the I/O processor side |
| [05 — The block codec on this disc](05-block-codec.md) | the census, the engine, and the two things that are missing |
| [06 — Decoder lineage](06-decoder-lineage.md) | **the headline**: the measurement and its controls |
| [07 — Media and the disc budget](07-media-and-budget.md) | video, ADX, AHX, and where the 4.08 GB went |
| [08 — `CDVD.000`](08-cdvd000.md) | the gigabyte that is not padding |
| [09 — Leftovers](09-leftovers.md) | the archaeology, in full |
| [99 — Open questions](99-open-questions.md) | with the measurement beside each one |
