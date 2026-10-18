# 09 — Leftovers

Reproduce with:

```
python tools/leftovers.py SLPS_255.33
python tools/leftovers.py IMAGE.iso --sweep
python tools/ps2elf.py IRX/KORGUMDI.IRX --iopmod
```

Output: [`reports/leftovers.txt`](../reports/leftovers.txt),
[`reports/magic-sweep.txt`](../reports/magic-sweep.txt).

---

## The devkit is still in the executable

```
0x2BB470   host0:C:\tox\fieldwork\dev\field\
0x2C5A60   HST:host0:R:\
```

`host0:` is the PlayStation 2 devkit's host filesystem — the developer's own PC,
mounted on the debugging station. Two of them survived here, and the first is a
full Windows path.

**`C:\tox\fieldwork\dev\field\`.** So the project root on somebody's machine was
`tox`, the field code lived under `fieldwork\dev\field`, and a build that shipped
still knows where to look if the disc is not there. The corpus has met this
before — *Tales of Rebirth*'s `fatal : '%s' is not found` falls back to `host0:`
— but *Rebirth*'s did not name a directory tree.

`tox` is the project tag, and the disc confirms it a second way. The memory-card
identifier is:

```
BISLPS-25533TOL-S
```

`TOL` — *Tales of Legendia* — and, unlike its predecessor, **the real product
code is compiled in**. *Rebirth* shipped `BISLPS-00000ToRsv%02d`, with the code
left as five zeroes and the number formatted at runtime. This one is a fixed
string with `SLPS-25533` in it. Two builds, eight months apart, one of which
finished the job.

## An index file that disagrees with the index

`AFS/FLIST.DIR`, 95 bytes, is plain text with CRLF line endings:

```
\AFS\SYS_REG.AFS
\AFS\FIELD.AFS
\AFS\SND.AFS
\AFS\MAP.AFS
\AFS\BATTLE.AFS
\AFS\MOVIE.AFS
```

Backslash-separated DOS paths, on a disc whose executable addresses everything as
`cdrom0:\AFS\…`. And the order is not the order in `PTN_INFO.BIN`, the index the
game actually uses, which runs `SYS_REG, MAP, SND, FIELD, BATTLE, MOVIE` —
`FIELD` and `MAP` have swapped places. Two files list the same six archives two
different ways, so at most one of them is read at runtime. `FLIST.DIR` is also
three weeks older than everything else in its directory: 2005-05-28 against
2005-07-21. It is a build-time file list that got mastered with the data.

## A studio MIDI rig, shipped to retail

`IRX/KORGUMDI.IRX` is 29,897 bytes and its `.iopmod` name is
**`KORG USB-MIDI DRIVER`**, version 1.3, `(C)2000-2002 KORG Inc.` It keeps its
identifiers, its error strings, and a device table:

```
Roland UM-1     Roland UM-2     Roland UM-4
YAMAHA UW500    YAMAHA UX96     YAMAHA UX16

[KORG UMD(IOP)] Failed to set MIDI IN thread priority. (iDev=%d)
[KORG UMD(IOP)] Failed to set MIDI OUT thread priority. (iDev=%d)
[KORGUMD IOP] Out of memory for SysEx receive buffer(i).
korgUmdProbe  korgUmdAttach  korgUmdDetach  MidiOutThread  GetEndPoints
```

Six third-party USB MIDI interfaces, named by model. Those are studio boxes; a
retail PlayStation 2 has nothing to connect them to. This is the audio team's
authoring path — the machine that let a composer drive the console's sound
hardware live from a sequencer — loaded from `cdrom0:\IRX\` by a shipped game.

It is not an isolated trace, either. Thirty of `SND.AFS`'s members are **MIDI
sequences** ([07](07-media-and-budget.md)) with names like `BATTLE_BOSS_MIDI`,
`FIELD_MIDI` and `TOGIJO_MIDI`, and `IRX/KORGIVAG.IRX` is `KORG IOP Sound
Driver` 1.1, `(C)2001-2005 KORG Inc.`, which plays them. The USB driver is the
one piece of that chain the player could never use, and it is the one nobody
removed.

`KORGIVAG.IRX` also kept its whole symbol table — `korgivag.c`,
`korgIopVagSetRpcBatchServerThreadId`, `CcProcPhaseReverse`,
`s_aRpcBatchServerReceiveBuf` — and two temporary assembler paths from the
machine that built it:

```
/tmp/cccf1tlc.ilb.stub.s
/tmp/cccf1tlc.ctors.s
```

`/tmp` and `cc` temporaries are GCC's on a Unix host, while the game's own
Emotion Engine code is stamped `MW MIPS C Compiler (2.4.1.01)`. Two toolchains
on two processors from two organisations, both legible on the disc.

## The build stamps the AFS directories do not have

The `AFS` directory format reserves twelve bytes per member for a date and on
all 24,835 members here every one of them is zero ([03](03-containers.md)), so
this disc has no per-asset chronology. What it has instead is CRI's middleware,
which stamps itself, and those stamps are precise to the second.

Every CRI component linked into the Emotion Engine executable:

```
PL2ENC       Ver.1.00    Build:Apr 12 2004 14:40:42
SJ/PS2EE     Ver.6.23    Build:Apr 12 2004 14:40:45
ADXF/PS2EE   Ver.7.14    Build:Apr 12 2004 14:40:47
SKG/PS2EE    Ver.0.64    Build:Apr 12 2004 14:40:49
ADXENC/PS2EE Ver.1.09    Build:Apr 12 2004 14:40:51
ADXT/PS2EE   Ver.9.19    Build:Apr 12 2004 14:40:54
ADXRT        Ver.3010    Build:Apr 12 2004 14:40:55
ADXPS2       Ver.2.52    Build:Apr 12 2004 14:40:56
PS2RNA       Ver 1.26    Build:Apr 12 2004 14:41:08
```

**Twenty-six seconds on the afternoon of 12 April 2004** — one library build,
in order, start to finish. And the I/O processor half, in `IXAUDIO.IRX`:

```
SJRMT   Ver 1.02   Build:Jun 25 2004 16:25:06
SJX     Ver 1.07   Build:Jun 25 2004 16:25:06
PSM     Ver 1.06   Build:Jun 25 2004 16:25:06
RCV     Ver 1.01   Build:Jun 25 2004 16:25:07
ADXI    Ver 9.23rd Build:Jun 25 2004 16:25:08
DTX     Ver.1.07   Build:Jun 25 2004 16:25:08
IOPRNA  Ver 1.27   Build:Jun 25 2004 16:25:08
```

Two seconds on 25 June 2004. So the middleware this game shipped in July 2005
was built in April and June of 2004 and never rebuilt — which is the same
pattern as the nine Sony modules sharing one 2004-05-11 timestamp
([02](02-disc-and-volume.md)), and it puts a floor under how long this project
was in production with its libraries frozen.

`ADXI Ver 9.23rd` is a version string with a typo in it that shipped.

## Three functions that say they are not there

```
PS2RNA_ClearBuf: not implemented
PS2RNA_ClearOverflow: not implemented
PS2RNA_DiscardData: not implemented
```

CRI's own stubs, with their own diagnostic, linked into a retail disc.

## A network stack with nothing to talk to

`INET.IRX` (141,653 bytes), `INETCTL.IRX`, `NETCNF.IRX`, `SMAP.IRX` and
`DEV9.IRX` — 263,645 bytes of PlayStation 2 network code, Ethernet driver and
all — in a single-player role-playing game with no network feature. Beside them
the executable carries Sony's whole debug-connection library:

```
libdbc: bind failed          sceDbcCreateSocket: rpc error
sceDbcSetWorkAddr: rpc error sceDbcGetDeviceStatus: rpc error
sceDbcSendData2: rpc error   sceDbcReceiveData: rpc error
```

`sceDbc*` speaks over the same hardware. The most economical reading is that the
network stack is on the disc because the debug transport was, and the debug
transport is on the disc because nobody took it out. A disc cannot show what a
module was loaded *for*, so this is listed as *Open* in
[99](99-open-questions.md) rather than asserted.

## Smaller things

**Two missing movies.** `MOVIE.AFS` holds `mov_01`, `mov_02`, `mov_04` …
`mov_15`, `mov_17` … `mov_24`. No `mov_03`, no `mov_16`, in a set otherwise
contiguous to twenty-four. Twenty-four slots numbered, twenty-two shipped.

**Twenty copies of one file.** `SND.AFS` carries `DANG00_24K.adx` through
`DANG19_24K.adx`. All twenty are 57,344 bytes, and all twenty are **byte-for-byte
identical to one another** — one two-second stream, stored twenty times, 1.1 MB.

**Two dummies from one source.** `MBS/DUMMY.MBS` and `AFS/DUMMY.AFS` are both
fifty bytes and byte-for-byte identical, in two different directories. Both are a
`CPS` header declaring 50 packed and 128 unpacked followed by a token stream —
so the placeholder for the index table and the placeholder for the archive are
the same compressed file.

**A gap in the spare numbering.** `SND.AFS` has `YOBI00` … `YOBI09` as ADX and
`YOBI20` … `YOBI25` as MIDI, with nothing between 09 and 20. `YOBI` is 予備 —
"spare".

**A debug display flag.** `fld_CDebugDispFlag()` is the one identifier in the
executable that is neither Sony's nor CRI's nor KORG's, and it names the field
code's debug overlay. There is also `srd_debug_geterror = %d`.

**One under-allocation.** `MAP.MBS[547]` says `scr_2evt_04kaii_00f_0hir.mcd.cps`
unpacks to 5,597,888 bytes. Its own `CPS` header says 5,597,952 and the decoder
produces 5,597,952. Sixty-four bytes, one member in 24,835, and the table is the
one that is wrong ([03](03-containers.md)).

---

## What is *not* here

**No cast list.** *Tales of Rebirth*'s executable carried its entire cast
romanised into English — `Lungberg` / `Veigue`, `Rhambling` / `Hilda` — plus
thirty-eight English place names and a hundred battle-effect names, in a game
never published outside Japan. Searching this executable for any of its own
characters' names in Latin script returns **nothing**. Legendia's cast appears on
the disc only as three-letter file prefixes in `SND.AFS` — `SEN`, `SHI`, `WIL`,
`CHL`, `NOR`, `MOS`, `JAY`, `GUR`, `HAR` — which is enough to count their lines
([07](07-media-and-budget.md)) but is not an English name table. Whatever
*Rebirth*'s romanisation was for, this build did not do it.

**And nothing from any other title.** Sweeping all 4,080,664,576 bytes for the
names and project tags of every game in the corpus:

| Pattern | Hits |
|---|---:|
| `Symphonia` / `symphonia` | **0** / **0** |
| `destiny` / `DESTINY` | **0** / **0** |
| `eternia` | **0** |
| `phantasia` | **0** |
| `Venus` | **0** |
| `rebirth` / `REBIRTH` | **0** / **0** |
| `TOD2` / `tod2` | 1 / **0** |
| `TOP2` / `top2` | 2 / 2 |
| `ToR` | 216 |
| `ToL` | 194 |
| `TOL` | 263 |
| `h4m` | 214 |

The non-zero rows are noise, and the arithmetic says so before the inspection
does. A three-byte string appears in 4.08 GB of random data about
`4.08e9 / 2^24 = 243` times; `ToR` got 216, `ToL` 194, `h4m` 214, all inside the
expected range. A four-byte string appears about `4.08e9 / 2^32 = 0.95` times;
`TOD2` got 1 and `TOP2` got 2. Every one of them was then located and read, and
every one sits mid-payload in a high-entropy `CPS` member with random bytes on
both sides. The single `TOL` hit inside the executable is the real one —
`BISLPS-25533TOL-S`, quoted above.

So this is the **second consecutive clean disc**. *Tales of Rebirth* was the
first in the corpus to carry nothing from another title; *Tales of Legendia*
carries nothing either. Before those two, every disc in the series had leaked
something.
