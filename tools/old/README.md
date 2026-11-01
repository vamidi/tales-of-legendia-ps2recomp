# Tools

Python 3, standard library only. One file per job. Every one of them takes the
disc image (or a file extracted from it) as its first argument and writes to
standard output; the committed output of each is in [`../reports/`](../reports/).

Nothing here needs installing and nothing here writes to the image.

```sh
python tools/iso9660.py IMAGE.iso --pvd
python tools/sector_map.py IMAGE.iso
python tools/afs_census.py IMAGE.iso --headers
python tools/afs_census.py IMAGE.iso --decode          # the slow one
python tools/ring_sites.py SLPS_255.33 --mips --base 0x00100000 --off 0x80 \
       --size 0x2D5800 --imm 4078,4079,4080
python tools/prefix_scan.py SLPS_255.33 0x001F6CA0 872 OTHER.elf [...]
python tools/media_census.py IMAGE.iso
python tools/disc_budget.py IMAGE.iso
python tools/region_profile.py IMAGE.iso --lba 2311 --size 1073676288
python tools/leftovers.py IMAGE.iso --sweep
```

---

## Written for this disc

| File | Job |
|---|---|
| `afs.py` | CRI `AFS` archive reader — extent table, directory, names |
| `afs_census.py` | classify and decode every member of every archive |
| `cps.py` | the `CPS` container and its decompressor, transcribed from `SLPS_255.33` |
| `media_census.py` | ADX / AHX / Sofdec parameters, and the per-prefix cast table |
| `disc_budget.py` | where 4,080,664,576 bytes went, by content rather than by file |
| `region_profile.py` | what is inside a large opaque region, block by block |

`cps.py` is the one to read first if you only read one. It is a transcription of
the routine at `0x001F6CA0`, instruction by instruction, with the differences
from the corpus format named in its docstring rather than left implicit: the
sixteen-byte header, the absent method byte, the absent run escape, and the ring
that is cleared and never preloaded. It is never told a member's unpacked size
while decoding — it stops when the input is exhausted — which is what makes
"4,508 of 4,508 decode to their declared length" a test rather than a tautology.

## Copied from the corpus, unchanged

| File | Source | md5 |
|---|---|---|
| `tales_block.py` | [tales-blockcodec-doc](https://github.com/vs-sr-dev/tales-blockcodec-doc) | `e2dcd6b8dc717b84f67bf8a46568298c` |

Verified identical to the corpus copy and to the copy in
[ps2-talesofrebirth-doc](https://github.com/vs-sr-dev/ps2-talesofrebirth-doc):

```
$ md5sum tools/tales_block.py tales-blockcodec-doc/tales_block.py
e2dcd6b8dc717b84f67bf8a46568298c  tools/tales_block.py
e2dcd6b8dc717b84f67bf8a46568298c  tales-blockcodec-doc/tales_block.py
```

**It finds nothing on this disc.** It is committed anyway. The corpus's rule is
that a decoder census is run with the shared reference implementation and not
with a local rewrite, and that rule does not stop applying when the answer is
zero — a negative is only worth quoting if the instrument that produced it is
the same one that produced the positives elsewhere.
[`../reports/reference-decoder-test.txt`](../reports/reference-decoder-test.txt)
runs it over 13.8 MB of this disc in both dialects and then over *Rebirth* and
*Destiny 2* data in the same invocation, where it returns 15 and 30.

## Copied from `ps2-talesofrebirth-doc`, unchanged

`iso9660.py`, `ps2elf.py`, `dismips.py`, `prefix_scan.py`, `sector_map.py`,
`leftovers.py`, `codec_census.py`, `scpk.py`, `mpeg.py`, `binfs.py`, `xarch.py`.

`leftovers.py`'s `SWEEP` pattern list was extended for this title — `h4m`,
`AFS\0`, `CPS\0`, `ADX`, `@UTF`, `KORG`, `FILE.FPB` and this game's own name
strings were added; nothing was removed, so every pattern the sibling pipelines
searched for is still searched for here and still reported at zero where it is
zero.

`codec_census.py`, `scpk.py` and `binfs.py` are carried unused: this disc has no
`SCPK` bundle and no `.BIN` container, and that they find nothing is part of
[03](../docs/03-containers.md).

## Modified

`ring_sites.py` gained one flag. The corpus's shortcut scans for the immediates
**4078** and **4079**, and that is still the default, so the command the sibling
pipelines ran still behaves identically here. `--imm 4078,4079,4080` widens the
set, because since 2004 the constant has stopped being stable — *Symphonia*'s
PlayStation 2 port clears its ring with 4080 and *Rebirth* with 4079 — and on a
2005 build it is worth asking for all three at once. The immediate is printed for
every hit, so the three never blur together.

That widening is what produced the report in
[`../reports/ring-sites.txt`](../reports/ring-sites.txt): seven sites, one 4078,
one 4079 and five 4080, of which exactly one is the decoder and the other six are
an allocator and five structure-field offsets. All six were disassembled before
being dismissed, which is section 7 of the specification's fourth rule.

## Not used

`xarch.py` is present because it travels with the PlayStation 2 toolkit, and it
is not used for anything in this repository. The corpus records that
opcode-sequence similarity does not discriminate between a real decoder pair and
an unrelated routine, and both sides of this repository's lineage question are
answered by byte equality instead, which is available here because every
executable compared is R5900 or R3000A.
