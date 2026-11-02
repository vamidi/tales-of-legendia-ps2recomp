# Session 63 handover — Tales of Legendia native port

**Goal unchanged:** native PC port of *Tales of Legendia* (SLUS_212.01) via PS2Recomp. Runtime is a fixed asset from
`vamidi/PS2Recomp`; bend it to this game.

---

## 1. Where the port stands

| milestone | state |
|---|---|
| Recompile + launch | done — clang-cl + SDL3, 13,520 units, 0 decode failures, 0 dispatch errors |
| Guest reaches its own startup | done — `FUN_00165ad0 game-startup` runs |
| **Guest gets past heap init** | **done this session** (was the blocker) |
| Guest installs its syscall handlers | done — `FUN_0011e070` now returns; ~32 syscalls flow |
| **SIF RPC answered natively** | **done this session** — bind/call no longer block |
| **IOP image + nine IRX modules load from `cdrom0:`** | **done this session** — sound library runs on the native IOP |
| Graphics bring-up (VIF/DMAC reset) | reached |
| Guest creates threads | not yet — `CreateThread` (0x20) not reached |
| **File I/O** | **done this session** — DVCI indexes all six AFS archives |
| **Graphics** | **renders** — `draws=201 passes=23 gif packets=81`, the game's "NOW LOADING" screen |
| **IOPRP boot image** | **booted** — 8 of its 16 modules started (IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV, LOADFILE, ROMDRV, EESYNC) |
| **Audio init** | **completes** — `SJX_Init` and `ps2rna_init_psj` succeed (16 SJUNI remotes) |
| **ioman init** | **answered** — `sceSifInitIoman`'s fn 0xFF succeeds natively |
| **IOP->EE SIF commands** | **delivered** — packet into the guest's own libsif buffer + its receive handler |

**Current blocker, stated precisely:** the loading sequence now **runs**. `FUN_001664d0` opens
`PTN_INFO.BIN`/`PTN_INFO.NAM` through ioman, lseeks, reads, requests its six `.AFS` loads, and the game goes on to
open individual AFS files:

```
[fileio] ioman open 'cdrom0:\AFS\PTN_INFO.BIN;1' -> ...\data\CD\AFS\PTN_INFO.BIN ok (fd 3)
[fileio] ioman open 'cdrom0:\AFS\PTN_INFO.NAM;1' -> ...\data\CD\AFS\PTN_INFO.NAM ok (fd 4)
[pc] enter 0x00119E70  x4      (lseek)
[pc] enter 0x0011A0B0  x2      (read)
[pc] enter 0x00196058          (load request)
[Deci2Call:kputs] DVCI: File cache was not hit. "\\AFS\SYS_REG.AFS;1"
```

Two bugs had gated it (both measured, not guessed):

1. the ioman init reply's first word is a **version**: `FUN_00119770()` memcmps it against `"3000"` and `"...."`
   and only lets a file call through when it matches one, so answering 0 made every open fail before the RPC;
2. the request's **result pointer is at +0x04**, not +0x08 (words are `sema, resultPtr, 4, fd, ...`); writing the
   fd to +0x08 put it at address 4 and the caller read garbage and bailed.

ioman open/close/read/lseek are implemented synchronously in `tolSifCallRpc`: the wrappers are asynchronous by
design (RPC status, then SIF command 0x80000011 fills the record and signals the semaphore), so the handler does the
I/O, writes the result where the caller reads it, and signals that semaphore.

Next: the run now spends its time in the loading work (no `[gfx]` frames are reaching the counter in 45 s), so the
next thing to measure is what `DVCI: File cache was not hit` costs and whether the six `.AFS` loads complete.

Measured chain from one run (`TOL_TRACE_RPC=1`):

```
[rpc] ixaudio fn=0x2   -> reply=0x00000001   (DTX create)
[rpc] ixaudio fn=0x422 -> reply=0x00000002   (SJUNI remote create)  x16 with fn 0x400
PS2RNA: sceSifAllocIopHeap(2256) ret=0x000008d0
[rpc] ixaudio fn=0x40A -> reply=0x00000000   (audio init done)
[rpc] bind client=0x003E1F40 sid=0x80000001
[rpc] ioman init -> ok                       (fn 0xFF answered natively)
```

Where the sound gap is: `sifRegisterRpc` is called exactly twice per run and both are KORG's (`0x90000300`,
`0x90000301`). IxAudio never calls any sifcmd function, so `0x90000200`'s provider is inside the `IOPRP300.IMG` the
game reboots into — a **compressed boot image** (its romdir sizes do not match the data layout), i.e. running it is a
subsystem of its own.

(`sceSifInitCmd`'s handshake was unblocked this session by seeding SIF_REG_MSCOM=1; see section 2c.)

(`sceSifInitCmd`'s handshake was unblocked this session by seeding SIF_REG_MSCOM=1; see section 2c.)

## 2. What was fixed (three defects, in order)

### 2a. `InitMainThread` (syscall 0x3C) returned the bottom of the stack area

`crt0` at `0x1001C4` does `li v1,0x3c; syscall; move sp,v0` with `a1 = -1` and `a2 = 0x100000`.
`ps2_syscalls::SetupThread` returned `PS2_RAM_SIZE - 0x100000 = 0x01F00000` (the *bottom*) as `$sp` unless
`asyncCallbackStackTop() <= initialStack`, which is false in the normal case (pool top starts at `PS2_RAM_SIZE`).
Only `stack_size == 0` callers (DQ8) got `PS2_RAM_SIZE`.

With `sp = 0x01F00000` the main thread's frames sat inside the guest's own heap (`0x4E9B80..0x1F00000`), and the heap
init that follows immediately (`0x100230`: `memset(0x4E9B80, 0, 0x1A16480)`) zeroed them - including
`FUN_00100230`'s saved `$ra`. The thread then returned to pc 0 and went dormant, which is why nothing ever created a
thread, a timer or a draw.

### 2b. The Timer-2 handler ran on the guest stack

After 2a the guest's `$sp` was right but the first interrupt invocation got `$sp = 0x1FFFFF0` (the first pool stack,
carved from `PS2_RAM_SIZE`), because `reserveGuestStackFromAsyncPool(0x1F00000)` refuses when the guest stack bottom
equals the pool floor. Its frame zeroed the same saved `$ra` a second time.

Fix (both in `ps2xRuntime/src/lib/Kernel/Syscalls/System.cpp`, submodule):

- `$sp = PS2_RAM_SIZE` (the end of RDRAM, as the kernel does for `stack == -1`);
- the top `kInvocationStackReserve = 0x40000` (256 KiB = 16 x 16 KiB stacks) belongs to the invocation pool;
- the guest stack bottom handed to `setupCurrentThread()` is `max(initialStack, 0x1FC0000)`, so the pool is
  `[0x1F00000, 0x1FC0000)` and the guest stack is `[0x1FC0000, 0x2000000)` (256 KiB).

Verified: `[tick] #1 sp=0x01FBFFF0`, guest `sp=0x02000000`, heap-init frame survives, `FUN_0011e070` returns.

### 2c. `sceSifInitCmd` reported as already initialised

`SIF.cpp::seedDefaultSifRegsLocked()` now seeds `kSifRegMsCom` (`0x80000002`) to **1**. A title with its own statically
linked libsif (ToL: `sceSifInit` at `0x117540`, `sceSifInitCmd` at `0x117CA0`) polls that register, sends a handshake
and then spins on guest flag `0x003DEF40`, which only its own DMA-completion bookkeeping could set. The runtime's SIF
DMA path completes synchronously and there is no IOP behind the register file, so "the channel is already up" is the
honest native state. The runtime's HLE `sceSifInitCmd` stub is only reached by games that *import* libsif.

Effect: the guest leaves the spin and reaches `sceSifBindRpc` (see section 1).

## 3. Assignment answers (session 62's list)

1. **Tick loop state block named.** `FUN_0011F650` is the EE **Timer 2** interrupt handler, registered by
   `AddIntcHandler(0xb, 0x11F650, 0, 0)` in `FUN_0011f0b0` at `0x11F150`. State = `0x00388CF8`
   (`s5 = 0x00390000` from `lui $s5,0x39` at `0x11F690`, minus `0x7308`); active list head = `0x00388D10`
   (state+0x18). **`$s5` is 0 on entry** - the handover's "log s5 on entry" prints 0, by construction.
2. **Queue dumped: always empty.** `count=0`, `free=0x003E3700` (untouched), `active=0`, `current=0xFFFFFFFF` on
   every tick, so startup never enqueues anything - the bug was upstream, exactly as the decision table said.
3. **The missing `CreateThread` is downstream of this.** The game loop (`0x1072B8`, `j 0x1072B8` at `0x100210`) is
   never reached because the main thread died in heap init. That is now fixed; the thread lives and the guest is
   inside libsif instead.
4. **Map holes** `0x232310`, `0x2324A8`, `0x2324BC` are still not hit (0 missing-function errors).

## 4. Next step, concretely

The DVCI/file layer is done. The guest now indexes its six AFS archives and moves on to the **sound library**:

```
[Deci2Call:kputs] DVCI: "\AFS\SYS_REG.AFS" found.   (x6)
[Deci2Call:kputs] DVCI: Total 6 files.
[IOP/RPC trace:unhandled] sid=0x90000200 rpc=0x2 ... sendBytes=[00 00 00 00 80 6A 3B 00 00 09 00 00 80 08 00 00]
[Deci2Call:kputs] DTX_Create: can't create DTX of server
[Deci2Call:kputs] E0100302: SJX_Init can't create DTX
```

KORG and IxAudio do start on the native IOP (`[iop] started KORG IOP Sound Driver`, `[iop] started IxAudio`), so
the next step is inside the native IOP. Traced with `TOL_TRACE_IOP_RPC=1`: the rejection is
**`native.server(sid, ...)` returning false**, not the guest read - `NativeIop::call` is never reached. Only
`0x90000300`/`0x90000301` (KORG) are ever registered; `2415919872` is `0x90000300`, not `0x90000200` (earlier hex
conversion error).

With `TOL_TRACE_IOP_THREADS=1`, LIBSD and IxAudio both finish their entries and create **and start** their worker
threads (ids 4,5 and 8,9,12), but neither registers `0x90000200`, and ~170 ms of extra IOP time does not change it.
So the provider's init never reaches `sceSifRegisterRpc`.

### The ioman hypothesis was tested and disproven

`ioman` open/close/read/lseek/write are now implemented (`EeLink::hostFiles()` -> `IopHost`, so an IOP module
opening `cdrom0:\...` resolves like the EE side does) and the `no kernel service for ioman` warnings are gone - but
LIBSD **never calls them** (`TOL_TRACE_IOP_FILEIO=1` logs nothing), so its init is not stopping at a file open.

What that run did expose is a real stall: with `TOL_TRACE_IOP_IMPORTS=1`, IxAudio polled `vblank #5` in a tight loop
from `0x4826C` and a constant 0 meant it never advanced. `vblank #5` now returns a 60 Hz counter off the IOP clock,
and the loop is gone (no unbound-import calls at all any more). `sysclib #19` is imported but never called, so it is
not the blocker either.

### Static analysis of IXAUDIO.IRX / KORGIVAG.IRX

`IXAUDIO.IRX` is a stripped ELF32 (one PT_LOAD, `p_vaddr 0`, entry `0x2674`) containing a complete, statically
initialised server record for the sid at link vaddr **`0xD180`**:

```
+0x00 = 0x0000C420   (server function)      +0x14 = 0x90000200  (sid)
+0x10 = 0x0000C9E0   (second handler)       +0x1C = 1
+0x20 = 0x30         (size)                 +0x2C = 0
```

referenced from a table of `{pointer, 2}` pairs at `0x117B8`. So the module is built to register `0x90000200`; the
record simply never reaches `sceSifRegisterRpc`.

Measured with `TOL_TRACE_IOP_PC=1` (per-thread syscall trace, `sceSifRegisterRpc` is handler index 74): **exactly two
calls in a whole run**, and both are KORG's (`0x90000300`, `0x90000301`). `sifSetRpcQueue`/`sifRpcLoop` are called
twice as well. LIBSD's threads 4/5 park on event flags (normal driver idle); IxAudio's thread 9 parks on **semaphore
10** and threads 8/12 never reach another syscall - so IxAudio's init blocks before the registration.

The provider is not a module we forgot to load: `0x90000200` does not occur anywhere in `IOPRP300.IMG` (nor do
`0x90000300`/`0x90000301`, `SDRDRV`, `LIBSD`, `KORG`, `IXAUDIO`).

Also fixed on the way: the loadfile RPC was dropping the module **arguments** (request is
`{argLen, 0, path[0xFC], args[]}`); they now reach the loader, which visibly changed IxAudio's load base
(`0x45C00` -> `0xADB00`).

Next step: find who signals semaphore 10 (trace semaphore create/signal/wait with owners), since that is what
IxAudio's init thread is waiting for before it registers `0x90000200`.

### Sync-object trace: the wait is healthy, and the provider is the IOPRP image
`TOL_TRACE_IOP_SEMA=1` traces every `create`/`wait`/`wait-block`/`wait-ok`/`set`/`clear`/`delete` on semaphores and
event flags with the calling thread and pc. The earlier "semaphore 10" reading was my own off-by-one: the `Wait` enum
is `None, Sleep, Delay, Semaphore, EventFlag, Mailbox, Rpc`, so kind 4 is **EventFlag**, not Semaphore.

Event flag 10 is not a deadlock - it is a healthy self-signalled tick loop in thread 9:

```
evflag 10 create     by thread 9 at 0x144FC
evflag 10 clear      by thread 9 at 0x11994
evflag 10 wait-block by thread 9 at 0x11B84
evflag 10 set        by thread 9 at 0x1295C  waiters=1
... repeats forever
```

Counting sifcmd calls over a whole run: `sifCheckInit` 2, `sifRegisterRpc` 2, `sifSetRpcQueue` 2, `sifRpcLoop` 2 -
i.e. **only KORG's two servers**. IxAudio never calls any of them, so `0x90000200` is not registered by anything the
runtime runs.

The provider has to be the IOP image the game reboots into. `IOPRP300.IMG` is a romdir container with 18 modules
(SYSMEM, LOADCORE, SIFCMD, SIFMAN, THREADMAN, IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV, LOADFILE, TIMEMANI, ROMDRV,
EESYNC, SYSCLIB, STDIO), but its entries are a **compressed boot image**: the recorded sizes do not match the data
layout (the slices are not ELF and their sum exceeds the file), so running it is a decompression + loader subsystem,
not a tweak. That is the real gap behind the sound failure.

### What that means for the port (measured)

The sound failure is **not fatal**. With the module arguments now passed and `vblank #5` ticking, the guest walks
past `SJX_Init` and renders:

```
[gfx] 52.5 fps over 600 frames | transfers=10 (80 KiB) draws=201 passes=23 gif packets=81
      presents composed=588 | ee-dispatches=3121 (5/frame)
```

`TOL_GFX_SCREENSHOT_DIR=<dir>` writes `frame_NNNNNN.ppm`; the captured frame is the game's **"NOW LOADING"** screen
(512x448, off-white text on black). So the port produces real game output, and only audio initialisation is blocked.

Next workstream, in priority order:

1. **Graphics fidelity** - 201 draws for a static loading screen; check `TOL_GFX_TRACE_BATCHES`/`TOL_GFX_STATS_EVERY`
   and whether the screen ever advances (it is the first thing the game shows).
2. **Sound**: run `IOPRP300.IMG` (decompression + module loader), which is also what the game's own `sceSifLoadModule`
   reboot path expects.

## 4b. What was implemented this session (all measured)

| layer | where | effect |
|---|---|---|
| `sceSifBindRpc` / `sceSifCallRpc` (guest 0x118448 / 0x118628) | `src/runtime/tol_overrides.cpp` | replaced by `ps2_syscalls::SifBindRpc` / `SifCallRpc`; no more semaphore wait |
| load-module SIF command (cid 0x80000003) | `ps2xRuntime/.../Stubs/SIF.cpp` + `ps2_iop_transport.h` | opens the image through the IOP-side file layer, sets boot-status bit 0x40000 on the next read |
| loadfile RPC (sid 0x80000006) | `src/runtime/tol_overrides.cpp` | `0xFF` -> `"3000"`; module loads served by the runtime's loader |
| game file open (sid 0x80000597, fn 0) | `src/runtime/tol_overrides.cpp` | path -> `translatePs2Path` -> `fopen`; reply must be `1`, handle goes in the request |
| game file read (sid 0x80000595, fn 1) | `src/runtime/tol_overrides.cpp` | bytes go straight into the caller's buffer; also clears the busy flag and releases the I/O semaphore the read leaves held |
| ToL IOP module list | `src/runtime/main.cpp` | `LIBSD.IRX`, `KORGIVAG.IRX`, `IXAUDIO.IRX` instead of DQ8's sound set |
| IOP ioman file services | `ps2xIOP/src/lle/kernel.cpp` + `iop.h` + `native_iop.cpp` | open/close/read/lseek/write through `IopHost` (bound; LIBSD never calls them) |
| IOP `vblank #5` | `ps2xIOP/src/lle/kernel.cpp` | 60 Hz counter off the IOP clock; removes IxAudio's tight poll loop |
| IOPRP boot image | `ps2xRuntime/src/lib/ps2_iop_host.cpp` + `native_iop.cpp` | parses the romdir container, starts the 6 modules the LLE kernel does not emulate |
| IxAudio server (sid 0x90000200) | `src/runtime/tol_overrides.cpp` | fn `0x2` (DTX), `0x400`/`0x422` (SJX/remote) return handles, so `SJX_Init` completes |
| IOP import tracing | `ps2xIOP/src/lle/kernel.cpp` | `TOL_TRACE_IOP_IMPORTS` names each unbound import and prints a0/a1/a2 |

Result: `DVCI: Total 6 files.` - all six AFS archives indexed - then the sound library's `SJX_Init` (see section 1).

Result: first `cdrom0:` open (`IRX\IOPRP300.IMG`, 275,921 bytes), then all nine IRX modules, then the graphics
bring-up, then the DVCI/SJX failure above.

## 5. Tooling

```
# build (runtime change + link, ~1 min)
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'

# run from the repo root so cdRoot is data\CD
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Probes (all env-gated, all in `src/runtime/tol_overrides.cpp` unless noted):

| env | what it prints |
|---|---|
| `TOL_TRACE_BOOT=1` | enter/exit of 14 boot points (`main.cpp`) |
| `TOL_TRACE_TICK=1` | tick-dispatcher state block + active list (new) |
| `TOL_TRACE_PC=0xADDR[,0xADDR]` | enter/exit of up to 16 functions with pc/ra/sp + 8 words at `$sp` (new) |
| `TOL_TRACE_RPC=1` | every SIF bind/call with sid, function, buffers, and the loadfile result (new) |
| `TOL_TRACE_IOP_RPC=1` | which rejection applies to an IOP RPC: no native server / call declined / no route (new) |
| `TOL_TRACE_IOP_THREADS=1` | IOP thread create/start (new) |
| `TOL_TRACE_IOP_IMPORTS=1` | every call to an unbound IOP import, named, with a0/a1/a2 (new) |
| `TOL_TRACE_IOP_FILEIO=1` | `ioman` opens performed by IOP modules (new) |
| `TOL_TRACE_PARK=1` | every `pc == 0` dormancy with the last 16 top-level dispatch pcs (`EeScheduler.cpp`) |
| `TOL_TRACE_SYSCALL=1` | first 150 syscalls with args, result, call site |
| `TOL_TRACE_DISPATCH=1` | first 4000 inter-function transfers |
| `TOL_TRACE_CKPT=1` | checkpoint-declined dispatches |
| `TOL_GFX_STATS_EVERY=N` | per-N-frame `[gfx]` counters |

Note: the guest's `syscall` instructions that the recompiler recognises as SDK stubs are emitted as **direct**
`ps2_stubs::*` calls, so they never appear in `TOL_TRACE_SYSCALL`. `sceSifSetDma` is one of them.

## 6. Land mines

- Never read a hardware address from memory: `0x10001000` is Timer 2, and `AddIntcHandler` cause `0xb` is Timer 2 too.
- `$s5`/`$gp` are not valid at entry to many guest functions - take addresses from the disassembly.
- A preempting checkpoint inside a *called* function unwinds the C++ stack and resumes as a fresh top-level dispatch
  of `ctx->pc`. That is only safe while the guest frame is intact, so anything that writes on the guest stack
  converts a preemption into a lost return address.
- `tol.exe` must not be running when linking.
- `thirdparty/PS2Recomp` carries local commits (now `9b82cef` + this session's fix); pushing still hangs on the network.
