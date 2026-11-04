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

> **Superseded by §6 below.** The "state 2 vs 3" reading was a symptom, not the cause: the stream was not
> being stepped at all.

## 6. Session 66, second pass — the loading loop was never being pumped (fixed)

The requested measurement (`FUN_00197668` vs the ADXF stream at poll time) answered it immediately:

```
[poll-stuck] #269312 i=5 ptid=5 state=2 count=0 progress=0 total=436462 consumed=0
             | sState=2 sDone=0 sBlocks=0 sDevBase=0x700000 sDev=0 sBusy=0 sAct=1 steps=310 gate=0
               slot5=0x00198210 slot4=0x00198280
```

* `FUN_00197668` is just `return handle[+1]`; at the freeze the handle is 2 and the stream is 2 with 0 bytes,
  i.e. a read is armed and nothing will ever complete it.
* **`steps=310` was constant across ~5,000 polls**: the stream step `FUN_0019F298` was not being called at all.
  The gate `DAT_0039E838` is 0 (free), so the pump itself was not running.
* The pump table `0x49C6A0 + slot*0x30` explains why. `FUN_001982a0` (ADXT init) picks the pumps:

  ```
  single-threaded  slot 5 = FUN_00198230 = FUN_00198260 + FUN_00198280 (CRI server) + FUN_00198210 (SJRBF)
  with a worker    slot 5 = FUN_00198210 (SJRBF only), slot 4 = FUN_00198280 (CRI server, on the worker)
  ```

  This title takes the threaded path, and `FUN_001664d0`'s `while (FUN_00196460(i) != 3) FUN_0019a080();`
  busy-waits without ever yielding, so once the worker stops nothing pumps the CRI streams again.

### Fix

`FUN_001b7378` (the pump registrar) is wrapped: when it is asked to register slot 5 with the SJRBF-only
`0x00198210`, `0x00198230` (the library's full main-thread pump) is registered instead. The worker keeps its
own slot 4, and `FUN_00197940`'s re-entrancy guard (`DAT_00393208`) makes concurrent pumping harmless.

Measured: **all six archives load in 74 polls** (previously 268,000+ polls and stuck at index 2/5), the DVCI
tick ends with no active handle, and the guest moves on to its next phase. Screen is still `NOW LOADING`.

### Consequence: the next wall is audio, not DVCI

Immediately after loading the guest calls KORG fn `0x0A` and the IOP never completes it:

```
[IOP/RPC trace:unhandled] sid=0x90000300 rpc=0xa pc=0x118628 ra=0x215910 send=0x4b3800/4 recv=0x4b3800/4
[ps2xIOP:warning] [iop] RPC to 2415919872 timed out      # 2415919872 = 0x90000300, NOT 0x90000200
```

`FUN_002158a8` is the client: `WaitSema`, `sceSifCallRpc(client 0x4B3C00, fn 10, 0x4B3800/4, 0x4B3800/4,
cb 0x2151B8)`. Each call burns the full 2 s IOP deadline, so the port looks frozen afterwards.

**Hex correction (this is the second time it has bitten):** `2415919872 = 0x90000300` (KORG, the server that
*is* registered) and `2415919873 = 0x90000301`. Session 65's conclusion that it was "IxAudio 0x90000200" was
that same bad conversion, so the IxAudio short-circuit added on the IOP side never applied to these calls.

## 7. `TOL_DVCI_SIZE_SHIM` removed

The shim answered `seek(END)` from the host size because the open reply only carried the base. The reply now
carries the byte size at `+0x04`, so the guest computes the sector count itself and the shim (probe, install
branch and `gLastGameFileSize`) is deleted; a comment at the old site says why it must not come back.

`TOL_CRI_WINDOW_FIX` is now *also* redundant - with the pump fixed the six loads complete either way, and with
the shim on the stream's total shows 1 instead of the real 436,462 sectors, so it degrades correct state. It is
left in place (off by default) pending a decision to remove it.

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
