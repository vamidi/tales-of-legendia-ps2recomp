# Session 66 handover — Tales of Legendia native port

**Goal unchanged:** native PC port of *Tales of Legendia* (SLUS_212.01) via PS2Recomp. Not an emulator.
Build: `build/tol2` (clang-cl + Ninja), run `build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu`.

---

## 1. What was fixed this session (one root cause, two symptoms)

The game's **own file server (sid `0x80000597` / `0x80000595`) is SECTOR-addressed, not handle-addressed.**

* `open` returns a **pair in the 300-byte request buffer itself**: word at `+0x00` = a base, word at
  `+0x04` = the file size in **bytes**. `FUN_00101a60` copies `response+0x00..0x07` into the caller's first
  8-byte word, so the caller's struct is `{base, size}`.
* `read` (fn 1) takes an **absolute sector number**, not a handle. The whole chain computes it:

  ```
  FUN_001AD850 (CRI device read):  offset = entry[+0x20] + entry[+0x0C]      // base + position
                                  -> FUN_001aaf80(request, offset, blocks, buffer, mode)
  FUN_001ab4f8 (SJRBF request):   FUN_001029a8(request[+8], request[+0xC], request[+0x10], desc)
  ```

  `request[+8]` is that `base + position`, so a read names sector `base + pos`.

Our old server handed out consecutive small handles `1,2,3,…`. Consequence:

* block 0 of an archive read the right file (position 0),
* block 1 resolved to the **next file's** handle (2 -> 3 = FIELD.AFS), block 2 to SND, block 3 to a handle that
  did not exist;
* an unknown first word makes `FUN_001029a8` return 0, `FUN_001ab4f8` turns request state 1 into **9 (error)**,
  and the DVCI completion poll `FUN_001ad220` latches the device entry to **state 3**, which is
  `FUN_0019EF58`'s discard branch. The ADXF stream then re-arms forever and the poll (`FUN_00196460`) never
  sees handle state 3 again - the "loading never advances" wall of sessions 64/65.

### The two code changes (both in `src/runtime/tol_overrides.cpp`)

1. **Open reply carries `{base, size}`.** `openGameFile` now assigns `base = (slot + 1) * 0x100000` (stride must
   exceed the largest archive in sectors: MOVIE.AFS = 436,462) and the RPC handler writes the byte size to
   `sendBuffer + 0x04`.
2. **Reads resolve an absolute sector.** `gameFileRead` finds the slot whose `[base, base + size)` contains the
   first word, then reads from `(sector - base) * 2048`. The old sequential `gGameFileOffset` is gone.

Diagnostic logging was added/wired behind the existing switches: `[cri] read` (device read slot + state
transition + entry fields), `[cri] gameread` (the game's read client + its return value + caller `ra`),
`[fileio] gamefile read ... REJECTED`, and the `[adxf]` dump now prints `+2C/+30/+34/+5C` and the device
state. The `[dvci] window #n` line is gated behind `TOL_TRACE_DVCI` (it used to print unconditionally).

### Measured effect (no env switches at all)

```
DVCI: "\AFS\SYS_REG.AFS" found.   ... "\AFS\MOVIE.AFS" found.   DVCI: Total 6 files.
[] gameread handle=1 blocks=2 ... -> 1
[] read pos=0/894     (SYS_REG)   -> state 1->2, ret 1
[] read pos=0/386911  (MAP)       -> state 1->2   pos 0..3, 5, 6 ...
[] gameread handle=0x40001D -> 1   (base 0x400000 = slot 3 = SND, position 29)
```

* the 6 archives parse instead of being served from each other's data;
* `progress` (DVCI entry parse) went 255 -> **6143** (24 sectors of SND's 6143-entry table);
* the loading loop advanced from load index 0 to **2** (to **3** with `TOL_CRI_WINDOW_FIX=1`);
* **zero** `E0xxxxxx` error prints (the `Illigal format(not AFS)` class is gone);
* screen still `NOW LOADING` (2 frames captured, 512x448).

`TOL_DVCI_SIZE_SHIM` is now **obsolete** (the size comes from the open reply); it still works but should not be
needed. `TOL_CRI_WINDOW_FIX` still buys one extra archive.

## 2. Where it stops now

Load index 2 (SND) / 3 with the window shim. The DVCI handle keeps consuming sectors (`consumed` 25, 29, ...)
but `progress` freezes, i.e. the poll is not parsing: it returns the **stream/handle state 2** instead of 3.
The credit tick `FUN_001973A8` -> `FUN_001972E0` does run and reports `handleState=3 streamState=1`, so the
handle is credited, but the ADXF stream settles to **state 1 (idle)** with `+34=0x800 +5C=1` while the DVCI
handle still expects more data. Same class as session 65 follow-up 3 (synchronous completion erases the
state-3 window) but now with correct data, so the next probe should compare `FUN_00197668(handle)` against
the stream state at the exact moment the poll runs.

## 3. Land mines confirmed this session

* **The CRI device vtable lives in `.data` at `0x003A6A98`** and points at addresses Ghidra never disassembles
  (`0x1AD5A0` = device open, `0x1AD360`). Read the pointer table with `pcsx2`/`memory_read`, then disassemble
  with `tools/disasm_guest.py` (capstone, MIPS64 - plain MIPS32 stops at the first `sd`).
* `FUN_001ab4f8` is the SJRBF request machine: state 1 -> `FUN_001029a8`; a return other than 1 sets **state 9**;
  state 2 -> `FUN_001ab3e8` decides 3 (done) or 9 (error).
* `FUN_001ad220` is the DVCI device completion poll (`FUN_001ab120(request)` status): 3 -> device state 1 and
  `pos += blocks`; **9 -> device state 3 (error)**, which is what froze the stream.
* `FUN_001ad2e8` is the per-tick walk of the 40-entry 0x48-stride device table at `0x003A6B00` and **is** called.
* Do not assume the first word of a read is a handle; this game's server pair is positional.

## 4. New tooling

| tool | purpose |
|---|---|
| `tools/disasm_guest.py <elf> <start> [end\|--count N]` | capstone MIPS64 disassembly of any VA range, for code Ghidra skips (pointer-only entries) |

## 5. Next step, in order

1. Probe `FUN_00197668` (handle state read) and, at the same instant, the ADXF stream `+1`, to see why the poll
   observes 2 when the tick's credit observed 3. The freeze (progress 6143 / 1023) is the only thing between
   here and the loading screen advancing.
2. If it is the synchronous-completion race again, prefer a runtime-ordering fix over a probe shim: the file
   read must not complete before the DVCI handle has latched its request (the guest's own wait path already
   pumps correctly for block 0).
3. After the six loads finish, watch for the first non-loader graphics (logos) and re-check the IxAudio RPC
   timeouts (they are IOP-side and were quiet this session).
