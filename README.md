# Tales of Legendia PS2recomp

A recompilation of the Tales of Legendia game on the PS2

### What is this project?



### Requirements



### How to use it




Do not publish `data`, `out` or `tools\\.build`. They may contain user inputs,
extracted game files or generated derivatives.


*Status as of **Session 51** (2026-10-30). If `PS2_PROJECT_STATE.md` has a higher session number, this table is stale.*

| Milestone | Status | Note |
|-----------|--------|------|
| Boot chain — SIF/RPC, IOP modules, all six MBS archives mount | ✅ Working | Syscalls and stubs implemented; no I/O stall |
| First rendered frame ("NOW LOADING") | ✅ Verified | Confirmed from a framebuffer dump, not just a counter |
| ADXF asset streaming | ⚠️ Flaky | ~10% of runs still deadlock in `ADXF_GetPtStat`; residual cause unknown |
| Scene rendering after the load transition | ❌ Blocked | Geometry is submitted but degenerate at the GIF level (zero-height sprites) → black screen |
| Build speed | ⚠️ Slow | Tree is an MSVC **Debug** build; `clang-cl + Ninja + Release` is far faster |
| Overall | 🚧 In progress | Evidence, history and next steps: [`PS2_PROJECT_STATE.md`](PS2_PROJECT_STATE.md) |

### Legal notice

This project is an interoperability-oriented source project and is not legal
advice. It is not affiliated with or endorsed by Namco, Sony, or any other rightsholder. 
All trademarks and copyrighted game material belong to their respective owners.
This repository and its Releases must not contain or redistribute game ISOs, encrypted or ELF files, extracted game files, game audio, video, textures, models, fonts, save data, savestates, generated AOT source,
objects or executables incorporating game-derived code. Users must provide
their own lawfully obtained inputs and create private derivatives locally.
Owning a copy does not automatically grant redistribution rights.
The original launcher, preparation and update code in this directory is
licensed under the included MIT License. The included PS2Recomp snapshot is a
project-specific modified version distributed under its MIT License; its
license and notices must remain intact.
The generated `out` directory is private and must not be uploaded without a
separate rights review. See `LEGAL.md` for the canonical scope.


---

## Documents

| | |
|---|---|
| [01 — Overview](docs/01-overview.md) | the disc in one screen, the six answers |
| [02 — The disc and the volume](docs/02-disc-and-volume.md) | single layer from the volume, 37 files, sector accounting |
| [03 — Containers](docs/03-containers.md) | `AFS`, the `MBS` tables, the `CPS` envelope, CRI's return |
| [04 — The executables](docs/04-executable.md) | the ELF, the compiler stamp, the I/O processor side |
| [05 — The block codec on this disc](docs/05-block-codec.md) | the census, the engine, and the two things missing from it |
| [06 — Decoder lineage](docs/06-decoder-lineage.md) | **the headline**: the measurement and its controls |
| [07 — Media and the disc budget](docs/07-media-and-budget.md) | video, ADX, AHX, MIDI, and where 4.08 GB went |
| [08 — `CDVD.000`](docs/08-cdvd000.md) | the gigabyte that is not padding |
| [09 — Leftovers](docs/09-leftovers.md) | the archaeology, in full |
| [99 — Open questions](docs/99-open-questions.md) | twelve, each with its measurement |

## Reports

Committed output of every tool run: [`reports/`](reports/) — volume descriptors
and file listing, the sector map, the `AFS` census and the full `CPS` decode
census, the ring-site scan across all eighteen executables, the decoder
disassembly, the prefix scans with all four controls, the reference decoder's
negative test with its positive control, the whole-image signature sweep, the
`CDVD.000` profile and census, the disc budget, the media census, and the
executable's string sweep.

## Tools

Python 3, standard library only, one file per job: [`tools/`](tools/) — see
[`tools/README.md`](tools/README.md).

`tales_block.py` is copied from
[tales-blockcodec-doc](https://github.com/vs-sr-dev/tales-blockcodec-doc)
without a single edit — md5 **`e2dcd6b8dc717b84f67bf8a46568298c`**, identical to
the corpus copy and to *Tales of Rebirth*'s. It is committed here even though it
finds nothing on this disc, because the test it fails is a result and a test only
counts if the instrument is the same one that succeeded elsewhere.
`iso9660.py`, `ps2elf.py`, `dismips.py`, `prefix_scan.py` and `sector_map.py` are
copied from
[ps2-talesofrebirth-doc](https://github.com/vs-sr-dev/ps2-talesofrebirth-doc)
unchanged; `ring_sites.py` gained one flag, `--imm`, and its default is still the
corpus's 4078/4079.

## Related

* [tales-blockcodec-doc](https://github.com/vs-sr-dev/tales-blockcodec-doc) — the
  shared codec specification and lineage, across nine builds and five platforms
* [ps2-talesofrebirth-doc](https://github.com/vs-sr-dev/ps2-talesofrebirth-doc) — 2004, eight months earlier
* [keitai-talesoftactics-doc](https://github.com/vs-sr-dev/keitai-talesoftactics-doc) — 2004 i-appli, the negative
* [gc-talesofsymphonia-doc](https://github.com/vs-sr-dev/gc-talesofsymphonia-doc) — 2003 GameCube + 2004 PlayStation 2
* [ps2-talesofdestiny2-doc](https://github.com/vs-sr-dev/ps2-talesofdestiny2-doc) — 2002, `FILE.FPB` and `SCPK`
* [ps1-talesofeternia-doc](https://github.com/vs-sr-dev/ps1-talesofeternia-doc) — 2000
* [ps1-talesofdestiny-doc](https://github.com/vs-sr-dev/ps1-talesofdestiny-doc) — 1997
* [snes-talesofphantasia-doc](https://github.com/vs-sr-dev/snes-talesofphantasia-doc) — 1995 + GBA 2003
* [android-talesofcrestoria-doc](https://github.com/vs-sr-dev/android-talesofcrestoria-doc) — 2020

## Licence

Tools under [MIT](LICENSE). Documentation and reports under
[CC BY 4.0](LICENSE-DOCS).

*Tales of Legendia* is a trademark of BANDAI NAMCO Entertainment. This project is
unaffiliated with and unendorsed by Bandai Namco, Namco Tales Studio, CRI
Middleware, KORG or Sony Interactive Entertainment.

