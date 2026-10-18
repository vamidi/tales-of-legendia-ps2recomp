# 04 — The executables

Reproduce with:

```
python tools/ps2elf.py SLPS_255.33 --header --segments --sections
python tools/ps2elf.py IRX/KORGIVAG.IRX --iopmod
python tools/leftovers.py SLPS_255.33
```

Output: [`reports/leftovers.txt`](../reports/leftovers.txt),
[`reports/ring-sites.txt`](../reports/ring-sites.txt).

---

## `SLPS_255.33`

```
type            2 (EXEC)
machine         0x0008  MIPS (EE, R5900)
flags           0x20924000
entry           0x00100008
program headers 2
section headers 8
load span       0x00100000 .. 0x004E4580   (4,081,024 bytes)
file size       2,972,228
```

One `PT_LOAD` of 0x2D5800 file bytes expanding to 0x3E4580 in memory — so about
1.1 MB of `.bss` — and a second, empty, marking the end. There is **no symbol
table**: `.symtab` and `.strtab` are present in the section header table with
size zero. Eight sections, and only three of them carry anything: the single
unnamed `PROGBITS` that is the whole program, `.comment`, and `.reginfo`.

That is a smaller and plainer object than *Rebirth*'s, which kept 94 named
sections and a full overlay map. Nothing here is sectioned by overlay; whatever
this game loads at runtime, it does not describe in its ELF.

### The compiler stamp

`.comment` is 43 bytes and reads:

```
MW MIPS C Compiler (2.4.1.01)\0PlayStation2\0
```

**Metrowerks, the same version string *Tales of Symphonia*'s PlayStation 2 port
carries.** *Tales of Rebirth* carries no `.comment` at all, and *Tales of
Destiny 2* carries none either. So of the four PlayStation 2 builds in this
corpus, two name Metrowerks 2.4.1.01 and two name nothing — and the two that
name it are *Symphonia* 2004 and *Legendia* 2005.

That matters for [06](06-decoder-lineage.md), because it removes the one
explanation that was available in 2004. When *Symphonia*'s decoder shared six
bytes with *Destiny 2*'s, "a change of toolchain" was a live reading. Here the
toolchain is not merely similar, it is stamped identical, and the two builds
share **2,420 contiguous bytes of C runtime** — and twenty-one of decoder.

---

## Seventeen I/O processor files

`IRX/` holds sixteen `.IRX` modules and Sony's `IOPRP300.IMG`
bundle, one of the modules being under `IRX/PAD2/`. Nine of them carry the
same timestamp to the second, 2004-05-11 16:13:53, which is what an SDK drop
looks like when nobody rebuilds it:

| Module | Bytes | `.iopmod` name | Stamp |
|---|---:|---|---|
| `IOPRP300.IMG` | 275,921 | *(not an ELF — a `RESET` bundle)* | 2004-05-11 |
| `INET.IRX` | 141,653 | | 2004-05-11 |
| **`KORGIVAG.IRX`** | 128,390 | **`KORG IOP Sound Driver` 1.1** | **2005-06-06** |
| `MCMAN.IRX` | 95,909 | | 2004-05-11 |
| **`IXAUDIO.IRX`** | 72,336 | `IxAudio` 1.1 | **2005-07-07** |
| `NETCNF.IRX` | 65,909 | | 2004-05-11 |
| `USBD.IRX` | 34,985 | | 2004-05-11 |
| **`KORGUMDI.IRX`** | 29,897 | **`KORG USB-MIDI DRIVER` 1.3** | 2004-05-11 |
| `LIBSD.IRX` | 28,661 | | 2004-05-11 |
| `INETCTL.IRX` | 24,765 | | 2004-05-11 |
| `SMAP.IRX` | 17,017 | | 2004-05-11 |
| `DBCMAN.IRX` | 15,653 | | 2004-05-11 |
| `DEV9.IRX` | 14,301 | | 2004-05-11 |
| `PAD2/DS2U_D.IRX` | 11,725 | | 2004-06-14 |
| `SIO2D.IRX` | 11,289 | | 2004-05-11 |
| `MCSERV.IRX` | 7,417 | | 2004-05-11 |
| `SIO2MAN.IRX` | 6,641 | | 2004-05-11 |

`IOPRP300.IMG` is not an ELF — it begins `RESET\0\0\0` and is Sony's bundle of
boot-time modules, with the usual internal names (`File_Manager`,
`Module_Manager`, `Timer_Manager`, `ROM_file_driver`) and Sony's usual
`Moldule_File` typo intact.

### The sound driver is KORG's

This is the first *Tales* disc in the corpus that does not do its own audio.

```
KORG IOP Sound Driver
%s - version%d.%d rev.%d %s%s%s (C)2001-2005 KORG Inc.
korgivag.c
korgIopVagSetRpcServerThreadId    korgIopVagGetRpcServerThreadId
korgIopVagSetRpcBatchServerThreadId
CcProcPhaseReverse                CcProcReverbDepth
```

The module kept its identifier table, so the driver's source file name, its RPC
entry points and its internal variable names (`s_iRpcServerThreadPriority`,
`s_aRpcBatchServerReceiveBuf`) are all readable, along with two temporary
assembler paths from the machine it was built on — `/tmp/cccf1tlc.ilb.stub.s`
and `/tmp/cccf1tlc.ctors.s`, which are GCC's, not Metrowerks'. So the game's
Emotion Engine code is Metrowerks and its sound driver is GCC, from a third
party, copyright 2001–2005.

The pairing is the point. *Tales of Legendia*'s music was written by a composer
from outside the series' usual staff, and the disc turns out to carry an
outside sound driver to play it with.

### And a USB-MIDI driver, in a retail game

`KORGUMDI.IRX` is `KORG USB-MIDI DRIVER` 1.3, `(C)2000-2002 KORG Inc.`, and it
is not there for the player:

```
[KORG UMD(IOP)] Failed to set MIDI IN thread priority. (iDev=%d)
[KORG UMD(IOP)] Failed to set MIDI OUT thread priority. (iDev=%d)
[KORGUMD IOP] Out of memory for SysEx receive buffer(i).
korgUmdProbe   korgUmdAttach   korgUmdDetach   MidiOutThread
Roland UM-1    Roland UM-2     Roland UM-4
YAMAHA UW500   YAMAHA UX96     YAMAHA UX16
```

It carries a device table naming **six third-party USB MIDI interfaces by
model** — three Roland, three Yamaha. Those are studio boxes. A PlayStation 2
sold at retail has nothing to plug them into and no reason to. This is the
audio team's authoring path, loaded from `cdrom0:\IRX\` by a shipped game,
and nobody took it out. [09](09-leftovers.md).

### Network modules, in a single-player role-playing game

`INET.IRX`, `INETCTL.IRX`, `NETCNF.IRX`, `SMAP.IRX` and `DEV9.IRX` are the
PlayStation 2 network stack — Ethernet driver, TCP/IP, and the configuration
reader — 263,645 bytes of it. *Tales of Legendia* has no network feature. The
executable's own strings include `libdbc: bind failed` and the whole
`sceDbc*` family, which is Sony's debug-connection library and speaks over the
same interface. The most economical reading is that the network stack is there
because the debug transport was, and the debug transport was there because the
build had not had it removed. It is listed as *Open* in
[99](99-open-questions.md), because a disc cannot show what a module was loaded
for.

---

## What the executable links but does not use

Sweeping the printable runs in `SLPS_255.33` returns 922 of eight characters or
more. The middleware is unmistakable:

* **ADX** — about 90 `ADXT_*` and `ADXF_*` error strings, each with CRI's
  numeric code: `E02080804 ADXT_Create: parameter error`
* **Sofdec** — `mwPlyCreateSofdec`, `MWSFD_MakeTblZ16`, `E201301: MWSFD: GetFps
  failed.`, and a rather human `SFD ERROR(%08X): Picture data size is large.
  Please increase MAXBPS of creation parameter.`
* **`cvFs`** — CRI's volume filesystem, thirty-odd entry points'
  worth: `cvFsAddVolumeEx #3:illegal image handle`, `cvFsSetCurVolume`,
  `cvFsGetVolumeInfo`, `cvFsGetFsys64Info`, `cvFsMakeDir`, `cvFsRemoveDir`

The last one is the interesting one, because **there is no `.CVM` file on this
disc** — no `CVMH` header, no `ROFSBLD` stamp, nothing for `cvFsAddVolume` to
add. The API is linked and, as far as the volume shows, unused. *Tales of
Symphonia*'s PlayStation 2 port shipped nine CVM volumes and read them with
`ROFS` on the I/O processor; this build kept the library and put its data in
plain `AFS` archives instead.

Sony's own libraries are stamped in the usual way — `PsIIlibcdvd 3000`,
`PsIIlibdma 3000`, `PsIIlibgraph3000`, `PsIIlibipu 3000`, `PsIIlibkernl3000`,
`PsIIlibmc 3000`, `PsIIlibpad2 3000` — all version 3000, matching the
`IOPRP300` bundle.
