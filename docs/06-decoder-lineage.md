# 06 — Decoder lineage

Reproduce with:

```
python tools/prefix_scan.py SLPS_255.33 0x001F6CA0 872 SLPS_254.50 SLPS_254.00 \
       SLPS_251.72 SLPS_030.50 SLPS_011.00 VENUS.ELF
python tools/prefix_scan.py SLPS_255.33 0x0010B6D0 872 SLPS_254.50 SLPS_254.00 ...
python tools/prefix_scan.py SLPS_254.50 0x001BFC34 932 SLPS_255.33 SLPS_254.00 SLPS_251.72
```

Output: [`reports/decoder-prefix.txt`](../reports/decoder-prefix.txt).

---

## The question this disc was opened to answer

The corpus's boundary statement, after eight builds, is that the format belonged
to **the *Tales* console C codebase**, and that the boundary is one of
inheritance rather than choice: *Venus & Braves* shipped on the same disc as
*Destiny 2* without the codec because it was a different codebase, and *Tales of
Tactics* shipped in the same month as *Rebirth* without it because a Java
application had nothing to inherit.

*Tales of Legendia* was developed inside Namco Tales Studio by a team that did
not come down the Wolf Team line. If that is so, it is the first time the codec
meets a group inside the same studio, working on the same series, for the same
console generation, that is not descended from the code's authors. Two outcomes
were both worth having: the codec present, and the boundary is the codebase as a
shared artefact inside the studio; the codec absent, and the boundary runs
*through* Namco Tales Studio.

The answer is neither, and it is more interesting than either.

**The codec is here. The source is not.**

---

## The measurement

Take 872 contiguous bytes of Legendia's decoder — `ring_init` at `0x001F6CA0`
through the end of the state machine at `0x001F7004` — and find the longest run
of those bytes appearing **anywhere** in another whole executable, at any
alignment, without telling the tool where to look.

| Legendia's **decoder** against | CPU | Longest identical run |
|---|---|---:|
| *Symphonia*, PlayStation 2, 2004 | R5900 | **21 bytes** |
| *Rebirth*, PlayStation 2, 2004 | R5900 | **20 bytes** |
| *Destiny 2*, PlayStation 2, 2002 | R5900 | 16 bytes |
| *Eternia*, PlayStation, 2000 | R3000A | 11 bytes |
| *Destiny*, PlayStation, 1997 | R3000A | 11 bytes |
| ***Venus & Braves*, PlayStation 2, 2003** | **R5900** | **20 bytes** |

The last row is the one that settles it, and it cost nothing. `VENUS.ELF` is the
corpus's own negative control: 933,840 instruction words with no `4078`
immediate anywhere, no zero loop, no ×8 pattern fill, **no decoder at all**.
Legendia's decoder resembles *Rebirth*'s exactly as much as it resembles an
executable that does not contain a decoder. Twenty bytes is the noise floor of
two MIPS routines sharing `nop` padding, an `addiu sp, sp, -N` prologue and a
`jr ra` — which is what the corpus said the floor was, measured three pipelines
ago, at six to eight bytes for shorter needles.

## The control that makes it a measurement

A negative byte result is worth much more if you can show byte equality was
available at all. Section 7 says to take a needle *of the same length* from the
builds' shared C library and run the identical measurement.

872 bytes of **Legendia's own C runtime**, from `0x0010B6D0` in the same file,
same tool, same targets:

| Legendia's **C runtime** against | Longest identical run |
|---|---:|
| *Symphonia*, PlayStation 2, 2004 | **872 — the entire needle** |
| *Destiny 2*, PlayStation 2, 2002 | 313 bytes |
| *Venus & Braves*, PlayStation 2, 2003 | 313 bytes |
| *Rebirth*, PlayStation 2, 2004 | 276 bytes |
| *Eternia*, PlayStation, 2000 | 17 bytes |
| *Destiny*, PlayStation, 1997 | 17 bytes |

The needle saturated against *Symphonia*, so it was widened until the answer
stopped growing — 4,096 bytes, 16,384, 65,536, all returning the same number:

> **2,420 contiguous identical bytes** between `SLPS_255.33` and `SLPS_254.00`.

Both carry `.comment` reading `MW MIPS C Compiler (2.4.1.01)`. Two executables
built with the same stamped compiler, eleven months apart, at the same studio,
agree for **2,420 bytes** of library code — and for **21 bytes** of decoder.

The ratio is 115 to 1. There is no toolchain explanation left to make.

## The instrument reproduces the corpus's published numbers

The controls above are only worth as much as the tool, so the tool was made to
reproduce results it did not produce. `tales-blockcodec-doc` reports that
*Rebirth*'s 932-byte C-runtime needle matches *Symphonia* 2004 for **276** bytes
and *Destiny 2* 2002 for **288**. Run here:

| *Rebirth*'s 932-byte C-runtime needle | This run |
|---|---:|
| vs *Symphonia*, PlayStation 2, 2004 | **276** ✓ |
| vs *Destiny 2*, PlayStation 2, 2002 | **288** ✓ |
| vs ***Legendia*, PlayStation 2, 2005** | **276** |

Both published figures reproduce to the byte. And the third row is new: **four
PlayStation 2 *Tales* titles across thirty-five months link the same C runtime
objects byte for byte.**

---

## What this means

The corpus's 2004 finding was that by then there was no longer one copy of the
decoder's source: three builds in thirty months clearing the same array three
ways with three constants through three mechanisms, while the on-disc format did
not move a bit.

Legendia is the fourth build and it does not extend that series by adding a
fourth mechanism. It **repeats the first one** — the inline byte loop bounded by
4078, unrolled by eight, that 1997, 2000, 2002 and the 2003 GameCube build all
use — while sharing no bytes with any of them.

Three readings are available and the disc does not choose between them:

1. **The source was taken from an older copy.** A 1997-vintage `ring_init` still
   existing somewhere in the studio in 2005 and being picked up by a new team is
   consistent with everything measured: the constant is the old one, and the
   compiled bytes differ because eight years and a different compiler lie between.
2. **The decoder was rewritten from a description.** The 872 bytes are not
   structured like any predecessor. Every earlier build is a straight-line
   routine that takes a block and returns its contents; this one is a *resumable
   state machine* with its cursor, mask, control register, saved token byte and
   a `0..3` state word in five `gp`-relative globals, so it can be handed a
   partial buffer and called again. Nothing in the corpus does that. Someone who
   had the algorithm and needed streaming decompression would write this; someone
   who had the file would not restructure it this way.
3. **Both** — an old copy consulted and then rewritten for a streaming loader.

What the disc *does* settle is what was actually being asked. The format did not
travel with the code. Every fingerprint of the format is intact — the ring, the
mask, the cursor at `RING − 18`, the `| 0xFF00` refill, the twelve-bit
reference with its high nibble in the second byte, the `+3` — and **4,508 of
4,508 members decode**. The *code* shares twenty-one bytes with its nearest
neighbour, which is what a file with no decoder in it scores.

And the envelope around the format *did* move, for the first time in ten years:
nine bytes became sixteen, the method byte was dropped, the run escape went with
it, and the synthetic preload the packer had been able to rely on since 1997 is
no longer written. So the boundary statement needs a new clause. It is not only
that the codec followed the codebase. It is that by 2005 **the codebase could
hand a team the algorithm without handing it the code, and the format survived
the transfer while the container around it did not.**

The one thing that still has not forked is the packer. Something produced
1,176,881,049 bytes of tokens that a decoder written to the 1997 description
reads without a branch, for a container that did not exist in 1997, on a disc
whose decoder shares nothing with any predecessor. Nine years and five machines
in, the packer is still invisible and still the only fixed point.
