# 99 — Open questions

Each with the measurement that produced it, so that anyone who disagrees knows
what to re-run. Nothing here is softened; where the answer is "the disc cannot
say", that is the entry.

---

**1 — Why does the 2005 build clear the ring the 1997 way?**

Four PlayStation 2 builds in thirty-two months clear the same 4,096-byte array
four ways: an inline byte loop with **4078** (1997–2003 and this disc), a
bespoke quadword `bzero` with **4080** (*Symphonia* 2004), a library `memset`
with **4079** from a factored `ring_init` (*Rebirth* 2004), and then the inline
loop with **4078** again. Legendia does not continue the divergence; it returns
to the original constant while sharing **21 bytes** with its nearest neighbour
against **2,420** of C runtime ([06](06-decoder-lineage.md)).

Three readings fit and the disc chooses between none of them: an older copy of
the source was picked up; the routine was rewritten from a description; or both.
The second has one piece of positive evidence — this decoder is a *resumable
state machine* with five `gp`-relative globals, structurally unlike every
predecessor, which is what you write if you need streaming decompression and have
the algorithm rather than the file. That is suggestive and it is not proof.
*Consistent*, not *Verified*.

**2 — Why is the synthetic preload gone?**

Every PlayStation-family build since 1997 fills its ring with 3,840 bytes of
`(i, 0x00)` and `(i, 0xFF)` pairs before reading a token, and the corpus measured
that guess paying off: on *Eternia*, all 20,085 blocks read the preload at least
once. `ring_init` here clears 4,078 bytes and returns.

Which means **the packer that produced this disc's 4,508 members also stopped
emitting references into the preload**, or the members would not decode — and
they do, 4,508 of 4,508. So the change was made on both sides. Whether the
preload was dropped because the new data did not benefit from it, or because
whoever rewrote the decoder did not know it was there and the packer was
configured to match, is not answerable from an image. It is the sharpest
remaining question about the packer, which remains invisible.

**3 — Was the run escape removed, or never carried?**

There is no `+19` in the 872 bytes and no third token length, so the corpus's
method 3 has no counterpart here. On *Rebirth* the run escape was the
overwhelming default — 93.5% of top-level members. On this disc the concept does
not exist. Since there is also no method byte, it is not possible to tell from
the disc whether the packer was told not to use it or whether this decoder simply
never implemented it. Both produce the same bytes.

**4 — Was `CDVD.000` included on purpose?**

1,073,676,288 bytes, 26.31% of the disc, stamped 2004-08-11 where every other
game file is stamped 2005, holding **1,430 `CPS` members that all decode
exactly**, referenced by no index on the disc ([08](08-cdvd000.md)).

For deliberate: it sits in the innermost sector range, immediately after the
executable and before every archive, and a DVD reads its outer tracks faster.
A gigabyte parked there pushes all 1.46 GB of live archives outward. Against:
a deliberate pad would not normally be a year-old build with real data in it,
and would not normally be named `.000`.

The disc cannot distinguish "layout decision" from "build-tree accident". What it
does settle is that the file is not padding, which is what its name, its `0xFD`
prologue and its 608 MB two-valued tail all suggest before it is read.

**5 — What was *Symphonia*'s 686 MB gap?**

Still open, and now with a third data point that does not close it.
*Symphonia* PS2 left 16.12% of its disc empty with 686 MB in one interior hole;
*Rebirth* left 0.478% with no interior gap, which ruled out "studio layout
policy"; Legendia leaves 0.529%, also with no interior gap, and agrees with
*Rebirth*. But Legendia achieves the same practical effect as Symphonia's hole
by a different mechanism — a quarter of the inner disc occupied by something the
game never reads. Three discs, three layouts, and neither of the two that push
their data outward can be shown to have meant it.

**6 — Why is there a network stack, and a USB MIDI driver?**

263,645 bytes of `INET`/`INETCTL`/`NETCNF`/`SMAP`/`DEV9` in a single-player
game, beside Sony's complete `sceDbc*` debug-connection library which uses the
same hardware; and `KORGUMDI.IRX`, a `KORG USB-MIDI DRIVER` naming six
third-party studio interfaces by model ([09](09-leftovers.md)).

The economical reading is that both are development infrastructure that shipped:
the network stack because the debug transport needed it, the MIDI driver because
the composer's authoring rig needed it. A disc shows what was loaded, never what
it was loaded for, so this is *Open*.

**7 — Why does the executable link CRI's `cvFs` volume API with no volume to
mount?**

Thirty-odd `cvFs*` entry points' worth of error strings are in `SLPS_255.33` —
`cvFsAddVolumeEx`, `cvFsSetCurVolume`, `cvFsGetVolumeInfo`, `cvFsMakeDir` — and
there is **no `.CVM` file on the disc**, no `CVMH` header and no `ROFSBLD` stamp
([03](03-containers.md), [04](04-executable.md)). Either the API came in as part
of a library object that could not be split, or the project used CVM volumes at
some point and moved to plain `AFS`. `CDVD.000` is a year-old asset set and
contains no `CVMH` either, so it does not answer this.

**8 — Why do the `AFS` directories carry no dates?**

The format reserves twelve bytes per member for one and **all 24,835 members
have twelve zero bytes**. Every sibling pipeline got its best material from
per-asset timestamps; this disc has none, and no amount of re-reading will
produce them. What partly substitutes is CRI's middleware build stamps, which
are precise to the second and put the library build at 2004-04-12 14:40:42 to
14:41:08 ([09](09-leftovers.md)) — but that dates the middleware, not the assets.

**9 — What is `TLPS`, and what is `TLPK`?**

Every one of the 2,739 AHX voice members begins with a four-byte `TLPS` chunk
that is not CRI's, and a `CPS` member inside `CDVD.000` contains the ASCII tag
`TLPKv1+50`. `TL` matches the `TOL` of `BISLPS-25533TOL-S`. Neither wrapper has
been parsed here; both are noted because a shared two-letter prefix across a
voice format and a package format is the kind of thing that turns out to matter.

**10 — Which member does `MAP.MBS` under-allocate, and does it matter?**

`MAP.MBS[547]` declares 5,597,888 bytes for `scr_2evt_04kaii_00f_0hir.mcd.cps`;
the `CPS` header declares 5,597,952 and the decoder produces 5,597,952. One
member in 24,835, sixty-four bytes short, in the table the game sizes its
allocations from ([03](03-containers.md)). Whether the game reads that table for
this member, or whether the sixty-four bytes land somewhere harmless, is not
determinable without running it. Recorded because a single outlier in a table
that is otherwise exactly right on 24,834 entries is more likely to be real than
to be a misreading of the format.

**11 — How does the game know a member is stored?**

3,353 members have `packed − 16 == unpacked` and a raw payload, including **every
one of `BATTLE.AFS`'s 2,957 data members**. There is no method byte in the `CPS`
header and the reserved word at `+0x0C` is zero on all of them. The arithmetic
test is the only one the header supports and it is presumably what the game does,
but the loader code that makes the decision has not been located in
`SLPS_255.33`. *Consistent*, not *Verified*.

**12 — Was Legendia developed by a team outside the Wolf Team line?**

This is the premise the disc was opened to test and it is the one thing here that
the disc cannot answer, because a disc carries code and data, not an
organisational chart. What the disc *does* show is that whoever wrote this
decoder did not have the file that *Rebirth*'s and *Symphonia*'s authors had —
21 bytes against 2,420 of shared runtime — and that they had the algorithm
exactly, down to the `+3`, the `| 0xFF00` refill and the cursor at `RING − 18`.
That is a statement about code inheritance and it is stated as one in
[06](06-decoder-lineage.md). Attributing it to a named team is outside what this
method can support.
