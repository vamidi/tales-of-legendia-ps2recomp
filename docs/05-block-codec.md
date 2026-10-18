# 05 — The block codec on this disc

Reproduce with:

```
python tools/ring_sites.py SLPS_255.33 --mips --base 0x00100000 --off 0x80 \
       --size 0x2D5800 --imm 4078,4079,4080
python tools/dismips.py SLPS_255.33 --va 0x001F6CA0 40
python tools/afs_census.py IMAGE.iso --headers
python tools/afs_census.py IMAGE.iso --decode
```

Output: [`reports/ring-sites.txt`](../reports/ring-sites.txt),
[`reports/decoder-listing.txt`](../reports/decoder-listing.txt),
[`reports/afs-census.txt`](../reports/afs-census.txt),
[`reports/cps-decode.txt`](../reports/cps-decode.txt),
[`reports/reference-decoder-test.txt`](../reports/reference-decoder-test.txt).

---

## The shortcut, run with a third constant

Section 7 of the specification says: scan the executable for the immediates
**4078** and **4079**, because they are `RING − 18` and `RING − 17`, they are
the packer's rather than the programmer's, and nothing else in a game has a
reason to load 4,078. Since 2004 that has needed widening — *Symphonia*'s
PlayStation 2 port clears its ring with **4080** and *Rebirth* with **4079** —
so all three were asked for at once, and the immediate is printed for every hit
so they never blur together.

Across 742,912 instruction words of `SLPS_255.33`, seven sites:

```
ADDRESS      WORD       FORM        IMM  ROUTINE
0x00107B90   0x66100FEF daddiu     4079  0x00107B34 (+23 words)
0x001F6CE4   0x24030FEE addiu      4078  0x001F6C98 (+19 words)
0x00267214   0x24B30FF0 addiu      4080  0x002671CC (+18 words)
0x0026760C   0x26040FF0 addiu      4080  0x002675CC (+16 words)
0x00267674   0x24A50FF0 addiu      4080  0x00267658 (+7 words)
0x0026769C   0x24840FF0 addiu      4080  0x00267658 (+17 words)
0x00267990   0x26440FF0 addiu      4080  0x002678EC (+41 words)
```

Section 7 also says to read every near miss rather than dismiss it, so all seven
were disassembled.

**The five `4080` sites are field offsets, not lengths.** Every one has the form
`addiu rX, rBASE, 4080` where `rBASE` is a live pointer loaded from a structure
— `lw a1, 7280(s2)` two instructions earlier — and two of them are the source
and destination arguments of the same 72-byte copy. Nothing near them clears an
array, and the neighbouring constants are 4040, 4248 and 72. Not the decoder.

**The `4079` site is an allocator.** `daddiu s0, s0, 4079` is followed by
`daddiu s0, s0, -1` and `slti v0, s0, 4096`: page rounding. Not the decoder.

**The `4078` site is the decoder**, and it took one disassembly to say so:

```
0x001F6CD4  addiu  v1, zero, 4095       ; the ring mask, 0x0FFF
0x001F6CD8  sw     zero, -30824(gp)
0x001F6CDC  sw     v1,   -30840(gp)
0x001F6CE0  daddu  a2, zero, zero
0x001F6CE4  addiu  v1, zero, 4078       ; RING - 18
0x001F6CEC  sw     v1,   -30844(gp)     ; ...stored as the initial cursor
0x001F6CF0  addiu  a0, a0, 2784         ; ring base 0x004A0AE0
0x001F6CF4  sb     zero, 0(a0)
0x001F6CF8  addiu  a2, a2, 8
0x001F6CFC  sb     zero, 1(a0)
0x001F6D00  sltiu  v1, a2, 4070
0x001F6D04  sb     zero, 2(a0)          ; ...unrolled by eight
   ...
0x001F6D1C  bne    v1, zero, 0x001F6CF4
```

That is an **inline byte loop unrolled by eight, bounded by 4078** — the shape
every build from 1997 to 2003 has. Counting it out: 509 iterations of eight
cover ring[0..4071], and the tail after the loop stores six more, ring[4072..4077].
**4,078 bytes cleared, exactly.**

### Which makes four builds and four ways to clear one array

| Build | Year | Mechanism | Constant |
|---|---|---|---|
| *Destiny*, *Eternia*, *Destiny 2*, *Symphonia* GC | 1997–2003 | inline byte loop | **4078** |
| *Symphonia*, PlayStation 2 | 2004 | bespoke quadword `bzero` | **4080** |
| *Rebirth* | 2004 | library `memset`, from a factored `ring_init` | **4079** |
| ***Legendia*** | **2005** | **inline byte loop** | **4078** |

The corpus reads three builds in thirty months clearing one array three ways as
evidence that by 2004 there was no longer a single copy of the source. A fourth
build eight months later does not contradict that; it sharpens it. Legendia does
not use either 2004 mechanism. It uses the one from before them both — and
[06](06-decoder-lineage.md) shows it did not get there by copying anybody.

### And it is not on the I/O processor

All sixteen `.IRX` modules and the `IOPRP300.IMG` bundle were scanned. **Zero
`4078`. Zero `4079`.** Two `4080` hits, both innocent: `KORGIVAG.IRX` pairs 4080
with 4092 and masks both with `0xFFCF`, and `LIBSD.IRX` stores 4080 and 4092 as
halfwords into the SPU register block at `0xBF90xxxx`. Both are hardware
register values.

So decompression runs on the Emotion Engine only — as in *Symphonia* 2004, and
unlike *Rebirth* 2004 and *Destiny 2* 2002, which each put a second copy on the
IOP. The corpus noted that where CRI's `ROFS` ran on the I/O processor the codec
left it, and where the game read its own containers the codec stayed. Legendia
is a third data point and it points the same way: CRI middleware is back, the
I/O processor is running CRI's and KORG's code, and the codec is not there.

---

## The engine is the corpus's, to the instruction

The routine at `0x001F6DF0` is a state machine over `(source, destination,
length)` with its state in five `gp`-relative globals, so it can be re-entered
on the next buffer. Every fingerprint section 3 of the specification lists is
present:

| Fingerprint | Where | Instruction |
|---|---|---|
| ring size and mask | `0x001F6F18` | `and a3, t1, a3` with `t1` = 4095 |
| ring base | `0x001F6F00` | `lui t0, 0x004A` / `addiu t0, t0, 2784` |
| initial cursor `RING − 18` | `0x001F6CE4` | `addiu v1, zero, 4078` |
| control refill | `0x001F6FCC` | **`ori a3, v1, 0xFF00`** |
| control bits LSB first, `1` = literal | `0x001F6E44` | `andi v1, v1, 0x0001` |
| reference's high bits in the **high** nibble | `0x001F6EC4` | `andi t2, t0, 0x00F0` |
| ...shifted into place | `0x001F6ED0` | `sll t2, t2, 4` |
| length in the low nibble | `0x001F6ECC` | `andi t0, t0, 0x000F` |
| **the `+3`** | `0x001F6ED4` | `addiu t0, t0, 3` |
| driven by the packed size | `0x001F6E1C` | `sltu at, a0, v0`, `v0` = source end |

Reference is twelve bits — low eight from the first token byte, high four from
the second byte's high nibble — length is `nibble + 3`, range 3 to 18, and the
copy writes each byte into the ring as it emits it. That is the 1997 dialect
with nothing changed, on the fifth machine it has run on.

### Two things are missing, and they are the interesting half

**There is no run escape.** The corpus's method 3 adds a second token shape:
short runs of `n + 3` and long runs of `b0 + 19`, range 19 to 274. There is no
`19` anywhere in these 872 bytes and no third token length. One path, always.

**There is no synthetic preload.** Since 1997 every PlayStation-family build has
filled its ring, before reading a single token, with 3,840 bytes of synthetic
`(i, 0x00)` and `(i, 0xFF)` pairs — a standing guess that the data will be 4bpp
tile rows and `0xFF`-padded tables, so the packer can back-reference them for
free. The corpus measured that guess paying off on *Eternia*: every one of
20,085 blocks read the preload at least once.

`ring_init` here clears 4,078 bytes and returns. There are no 256-iteration
pattern loops after the zero loop; the next instruction is `ld ra, 0(sp)`. **The
dictionary starts empty.**

That is not a conclusion drawn from an absence, because section 7 warns
explicitly that a wrong dictionary still produces the right length — a
back-reference copies the same number of bytes whatever it copies. The test has
to be semantic, and it is: the first member of `SYS_REG.AFS` decodes under the
empty ring to 1,625,216 bytes beginning

```
05 00 00 00  80 cc 18 00  00 00 00 00  80 00 00 00  c0 f2 05 00
   count 5,  0x18cc80 bytes, then five plausible offsets
```

which is a parseable table whose declared length matches `MAP.MBS`'s entry for
it. A garbage dictionary does not produce a valid index.

---

## The format moved

The corpus's block is **nine** bytes and its first is a method byte. This disc's
envelope is **sixteen** and has none:

```
corpus            this disc
+0  u8  method    +0x00  "CPS\0"
+1  u32 packed    +0x04  u32 packed (including the header)
+5  u32 unpacked  +0x08  u32 unpacked
                  +0x0C  u32 zero, on every member
+9  stream        +0x10  stream
```

So the unmodified reference decoder cannot find anything here, and it does not.
`tales_block.py`, copied from the corpus with md5
`e2dcd6b8dc717b84f67bf8a46568298c` and not edited, over 13,824,860 bytes of this
disc in both dialects:

| Target | Bytes | psx | snes |
|---|---:|---:|---:|
| `SYS_REG.AFS` member 0, as shipped | 822,424 | **0** | 0 |
| the same member, decompressed | 1,625,216 | **0** | 0 |
| `BATTLE.AFS` from its first stored member | 2,097,152 | **0** | 0 |
| `MAP.AFS`, first 2 MB | 2,097,152 | **0** | 0 |
| `SYS_REG.AFS`, whole archive | 2,113,536 | **0** | 0 |
| `FIELD.AFS`, first 2 MB | 2,097,152 | **0** | 0 |
| `SLPS_255.33`, whole executable | 2,972,228 | **0** | 0 |
| **TOTAL** | **13,824,860** | **0** | **0** |

A zero is worth nothing without a control, so the identical call was made on
discs where the format is known to be present:

| Target | Bytes | psx |
|---|---:|---:|
| *Rebirth* 2004, `DAT.BIN` first 1 MB | 1,048,576 | **15** |
| *Destiny 2* 2002, `FILE.FPB` first 2 MB | 2,097,152 | **30** |
| *Rebirth* 2004, `FLD.BIN` first 1 MB | 1,048,576 | 0 |

The tool runs. It finds nothing on this disc because there is nothing of its
shape on this disc.

---

## The census

Every `CPS` member of every archive, decoded by `tools/cps.py` and checked
against its own declared unpacked length. The decoder is never told that length
— it stops when the input runs out, which is what section 1 says drives the
compressed paths — so the comparison is a test rather than a tautology.

| Archive | compressed | exact | stored |
|---|---:|---:|---:|
| `SYS_REG.AFS` | 430 | **430** | 0 |
| `FIELD.AFS` | 1,823 | **1,823** | 8 |
| `MAP.AFS` | 790 | **790** | 0 |
| `BATTLE.AFS` | 0 | — | 2,957 |
| `SND.AFS` | 35 | **35** | 388 |
| `MOVIE.AFS` | 0 | — | 0 |
| **in the archives** | **3,078** | **3,078** | **3,353** |
| `CDVD.000` ([08](08-cdvd000.md)) | 1,430 | **1,430** | 0 |
| **TOTAL** | **4,508** | **4,508** | **3,353** |

```
in the archives   826,535,905 packed -> 1,415,546,432 unpacked   58.39%
CDVD.000          350,345,144 packed ->   683,107,520 unpacked   51.29%
combined        1,176,881,049 packed -> 2,098,653,952 unpacked   56.08%
```

**Zero mismatches, zero exceptions, zero members that expand.**

### The packer's habits on this disc

Each of these is a line in the corpus's running list of what the packer does,
and the packer remains the only thing in this lineage that has never been seen
directly.

* **Nothing expands.** 0 of 4,508 have packed ≥ unpacked. That has held on every
  disc.
* **The stored path is back, and it is bigger than ever.** *Rebirth* had zero
  method-0 blocks in 2,851 and the corpus recorded the stored path as gone.
  Legendia stores **3,353 members, 614.5 MB, 15.06% of the disc** — and it does
  it without a method byte, by making `packed − 16 == unpacked`. Where it is used
  is categorical rather than incidental: **every one of `BATTLE.AFS`'s 2,957
  data members is stored**, and almost nothing else is. Battle data is not
  compressed at all, on a disc whose map data compresses to 58%.
* **The smallest block is 56 bytes packed** and produces 128 — so the packer is
  still compressing payloads the 2000 version would have stored, as *Rebirth*'s
  did.
* **The largest is 3,864,151 packed**, producing 6,162,880, in
  `MAP.AFS / scr_1twn_00mati_61i_0hir.mcd.cps`. The ~30 KB ceiling that held to
  2002 and broke in 2003 has stayed broken for a third disc, and this one is
  nearly four times the GameCube's 1,007,213 and *Rebirth*'s 1,015,400.
* **The per-archive ratio varies and the dispatcher does not care**, exactly as
  on the 2000 and 2004 discs: `SYS_REG` compresses to 41.6%, `FIELD` to 51.7%,
  `MAP` to 58.7%, `SND`'s handful to 80.9%.
