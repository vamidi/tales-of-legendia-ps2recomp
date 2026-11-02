# Session 64 handover — Tales of Legendia native port

**Goal unchanged:** native PC port of *Tales of Legendia* (SLUS_212.01) via PS2Recomp. Not an emulator. The runtime
(`thirdparty/PS2Recomp`, `ps2xRuntime` + `ps2xIOP`) is a fixed asset; bend it to this game.

---

## 1. Where the port stands

| milestone | state |
|---|---|
| Recompile + launch | done — clang-cl + SDL3, 13,520 units, 0 decode failures, 0 dispatch errors |
| Guest reaches its own startup | done — `FUN_00165ad0 game-startup` runs |
| Guest gets past heap init | done — syscall 0x3C stack fix (section 3.1) |
| SIF RPC answered | done — the game's own libsif bind/call replaced by the runtime's HLE |
| IOP image + IRX modules load from `cdrom0:` | done — IOPRP boot image (section 3.8) + 9 modules |
| File I/O | **done** — DVCI indexes all six AFS archives; ioman open/read work |
| Audio init | **completes** — `SJX_Init` + `ps2rna_init_psj` succeed (section 3.9) |
| Graphics | **renders** — 201 draws, 81 GIF packets, the game's "NOW LOADING" screen (512x448) |
| **Guest threads** | **not yet — `CreateThread` (0x20) is never called; the port has 1 thread** |
| Loading screen advances | no — see section 2 |

## 2. Current blocker, stated precisely

The port renders "NOW LOADING" and stops there. The loading sequence is `FUN_001664d0` (found with
`TOL_TRACE_SPIN=1`, which put the guest at `0x1666B4`, one of its `for(;;)` error paths):

```c
lVar3 = FUN_00119a68("cdrom0:\AFS\PTN_INFO.BIN;1", 1);   // ioman open, fn 0
lVar4 = FUN_00119a68("cdrom0:\AFS\PTN_INFO.NAM;1", 1);
if (lVar3 < 0 || lVar4 < 0) for (;;) {}
FUN_00119e70(fd, 0, 2); FUN_00119e70(fd, 0, 0);           // lseek: size, rewind
malloc(size); FUN_0011a0b0(fd, buf, size);               // read, fn 2
FUN_00119cf8(fd);                                        // close
... parse PTN_INFO ...
for (i = 0; i < 6; ++i) {
    FUN_00196058(i, "<CDV:\AFS\...>.AFS", 0, out);       // request a load
    while (FUN_00196460(i) != 3) FUN_0019a080();         // wait for state 3
}
```

All of the file I/O in that sequence now **works** (section 3.11): the two `PTN_INFO` files open, four lseeks and two reads
run, the six load requests are issued, and the game goes on to open individual AFS files
(`DVCI: File cache was not hit. "\AFS\SYS_REG.AFS;1"`).

**What is still missing is threads.** Measured against PCSX2 (section 4): after loading, the real game runs **eight EE
threads**, nearly all blocked in kernel waits. This port runs one. `FUN_00196460(i)` reports the progress of the
game's *own* asynchronous loader, which on hardware is serviced by worker threads — with a single thread that state
can never reach 3, so the loading screen cannot advance no matter how correct the file layer is.

## 3. What was fixed this session (all measured)

Every item below was verified in a run; the commits are listed at the end.

1. **`InitMainThread` (syscall 0x3C) returned the bottom of the stack area.** ToL's crt0 passes `stack = -1,
   size = 1 MB`, so it got `0x01F00000` — the bottom — putting the main thread's frames *inside its own heap*
   (`0x4E9B80..0x1F00000`), which the next `memset` zeroed, including a saved `$ra`. The thread then returned to pc 0
   and went dormant: the whole boot failure. Now it returns the end of RDRAM and reserves the top 256 KiB for the
   invocation stacks.
2. **SIF command channel reported as initialised** (`SIF_REG_MSCOM = 1`), so the game's `sceSifInitCmd` stops
   spinning on a flag no IOP would set.
3. **Native SIF bind/call.** The game carries libsif inside the ELF (`sceSifBindRpc` 0x118448, `sceSifCallRpc`
   0x118628); both waited on semaphores only an IOP reply would signal. They are replaced by
   `ps2_syscalls::SifBindRpc` / `SifCallRpc`.
4. **SIF load-module command** (cid `0x80000003`, path at packet `+0x18`): reads the image through the IOP-side file
   layer and sets boot-status bit `0x40000` on the next read. First `cdrom0:` open = `IRX\IOPRP300.IMG` (275,921 B).
5. **Loadfile RPC (sid `0x80000006`)**: `0xFF` returns `"3000"`; module loads are served by the runtime's loader,
   **with their arguments** (the request is `{argLen, 0, path[0xFC], args[]}`; dropping the args loaded IxAudio
   unconfigured — its base moved `0x45C00 → 0xADB00` once fixed).
6. **The game's own file server**: sid `0x80000597` fn 0 (open; reply must be `1`, handle goes in the request) and
   sid `0x80000595` fn 1 (read; bytes into the caller's buffer, plus the I/O semaphore `0x385228` that the read path
   leaves held). Result: `DVCI: Total 6 files.`
7. **IOP kernel**: `ioman` file services implemented on `IopHost` (`EeLink::hostFiles()`), `vblank #5` returns a
   60 Hz counter (IxAudio was polling a constant 0 in a tight loop), and `TOL_TRACE_IOP_*` probes.
8. **IOPRP300.IMG boot.** The container is a romdir table followed by **16 module ELFs**; the table's first three
   entries are the image's own, and the recorded sizes are *not* the layout, so modules are found by ELF magic and
   sized from their program headers. Modules the LLE kernel emulates are skipped; **IOMAN and FILEIO are loaded for
   real** (they are servers, not kernel primitives). 8 modules start: IOMAN, MODLOAD, FILEIO, CDVDMAN, CDVDFSV,
   LOADFILE, ROMDRV, EESYNC.
9. **IxAudio server (sid `0x90000200`)**, answered natively because no module registers it: fn `0x2` (DTX create),
   `0x400` (SJX), `0x422` (SJUNI remote) return fresh handles. `SJX_Init` and `ps2rna_init_psj` now complete
   (16 remotes, IOP heap allocated). Audio is not produced.
10. **ioman init (fn `0xFF`)** answered with the version string **`"3000"`** — the game memcmps it against `"3000"`
    and `"...."` (`FUN_00119770`) and refuses every file call otherwise.
11. **ioman open/close/read/lseek** implemented synchronously: the wrappers are asynchronous by design (RPC status,
    then SIF command `0x80000011` fills the request record and signals the semaphore), so the handler does the I/O,
    writes the result where the caller reads it, and signals that semaphore. **Request layout: `{sema, resultPtr, 4,
    fd, ...}` — the result pointer is at `+0x04`, not `+0x08`** (that mistake wrote the fd to address 4 and made the
    caller bail).
12. **IOP→EE SIF command delivery** (`deliverSifCommand`): writes a packet into the guest's own libsif receive buffer
    (`0x3DECC0`) and runs its DMAC receive handler (`0x117AA8`). Verified delivered; the loading gate turned out to be
    elsewhere.

## 4. PCSX2 as the reference (ground truth)

`pcsx2_connect` (DebugServer 21512 + Pine 28011) works and the game is loaded. Its post-loading steady state:

```
TID 0: PC=0x00081fc0 status=1 waitType=0    <- EE kernel park loop (nop sled + b 0x80081FC0), GPRs cleared
TID 1: PC=0x00114608 status=4 waitType=1    <- syscall stub: jr ra; nop; li v1,0x33; syscall
TID 2: PC=0x00114728 status=4 waitType=2
TID 3: PC=0x00198c90 status=8 waitType=0
TID 4..7: PC=0x00114608 status=4 waitType=1
```

Eight threads, mostly in kernel waits, CPU idling. Useful A/B recipes: `pcsx2_get_threads` (thread PCs),
`pcsx2_disassemble`, `pcsx2_read_registers`, `pcsx2_set_breakpoint`, `pcsx2_read_memory`. Note `get_modules` hung once
and `get_threads` once returned an error — retry, do not assume the connection is dead.

## 5. Assignment for the next session, in order

1. **Find the guest's first `CreateThread` (syscall 0x20).** Trace `TOL_TRACE_SYSCALL` around the loading sequence and
   compare with PCSX2 (breakpoint `0x1664D0`, then inspect the thread list). If the guest never reaches it, find what
   the loading sequence needs before it can start its workers.
2. **Exercise the runtime's `CreateThread`.** It has never been called; verify it creates a scheduler thread, starts
   it at the requested entry, and that `EeScheduler` runs it. This is untested code in this port.
3. **Then re-check the loading loop**: `FUN_00196460(i)` should reach 3 and the six `.AFS` loads complete; watch for
   `[fileio]` lines for `SYS_REG/FIELD/SND/MAP/BATTLE/MOVIE.AFS` and for the loading screen advancing.
4. Latent, do not forget: `0x232310`, `0x2324A8`, `0x2324BC` are holes in `config/map.csv`; 0 missing-function errors
   so far.

## 6. Tooling

```
# build (13,520-unit corpus; ~1 min for a runtime change + link)
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'

# run from the repo root so cdRoot is data\CD
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Probes (all env-gated):

| env | what it prints |
|---|---|
| `TOL_TRACE_SPIN=1` | guest pc sampled at checkpoint (backward-branch) points — **the tool that finds a within-function spin** |
| `TOL_TRACE_PC=0xADDR[,0xADDR]` | enter/exit of up to 16 functions with pc/ra/sp + 8 words at `$sp` |
| `TOL_TRACE_RPC=1` | every SIF bind/call with sid, function, buffers; plus the per-service results |
| `TOL_TRACE_FILEIO=1` | every file open at all three layers (`[fileio] iop open`, `[fileio] open`, `[fileio] gamefile open`, `[fileio] ioman open`) and `[ioprp]` boot lines |
| `TOL_TRACE_IOP_RPC=1` | which rejection applies to an IOP RPC |
| `TOL_TRACE_IOP_THREADS=1` | IOP thread create/start |
| `TOL_TRACE_IOP_IMPORTS=1` | every call to an unbound IOP import, named, with a0/a1/a2 |
| `TOL_TRACE_IOP_SEMA=1` | semaphore/event-flag create/wait/set/clear with thread + pc |
| `TOL_TRACE_PARK=1` | every `pc == 0` dormancy with the last 16 top-level dispatch pcs |
| `TOL_TRACE_SYSCALL=1` | first 150 numeric syscalls |
| `TOL_TRACE_DISPATCH=1` | first 4000 inter-function transfers |
| `TOL_GFX_STATS_EVERY=N` | per-N-frame `[gfx]` counters (draws/passes/gif packets/ee-dispatches) |
| `TOL_GFX_SCREENSHOT_DIR=DIR` | writes `frame_NNNNNN.ppm` (512x448) |

Address lookup: `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\map_lookup.ps1 0x1664D0`
(`-Bypass` is mandatory; script execution is disabled on this machine).

IRX analysis: `python` + `capstone` are installed. Parse an IRX as ELF32 (one PT_LOAD, `p_vaddr 0`) and search for
`lui reg,0x9000` + `ori/addiu` pairs — a raw 32-bit search misses `li` immediates.

## 7. Land mines

- **`$s5` / `$gp` are not valid at entry to many guest functions** — `FUN_0011F650` loads `$s5` itself. Take
  addresses from the disassembly, never from an entry register.
- **`Wait` enum order is `None, Sleep, Delay, Semaphore, EventFlag, Mailbox, Rpc`** — reading kind 4 as "Semaphore"
  sent a whole investigation the wrong way. Kind 4 is EventFlag, 6 is Rpc.
- **IOP handler indices are not library ordinals.** `handlerFor` maps (library, ordinal) onto an index into
  `kernel_handlers.inc`; `sifRegisterRpc` is index 74, `sifCallRpc` 80, `sifBindRpc` 79.
- **`g_sifCmdHandlers` is only ever written.** A game with its own libsif needs `deliverSifCommand`; the HLE table is
  not its path.
- **A `for(;;)` at 0x1666B4 is an error path, not a wait loop.** Both error paths in `FUN_001664d0` look identical in
  a spin trace; the disassembly distinguishes them.
- **ioman request layout: `{sema, resultPtr, 4, fd, ...}`** — result pointer at `+0x04`.
- **The IOPRP romdir sizes are not the module layout**; find ELFs by magic and size them from program headers.
- **Do not skip IOMAN/FILEIO in the boot image** — they are servers.
- The `libdbc: Module version mismatch [libdbc.a = 3.10, dbcman.irx = 3.20]` line is the game's own warning, not fatal.
- `tol.exe` must not be running when linking. The merged map (`config/map.csv`) is the fast corpus; the raw one is a
  control, not a baseline.
- `vamidi/PS2Recomp` carries local commits (now through `14faba7`); pushing has hung on the network before.

## 8. Commits this session (newest first)

Submodule `thirdparty/PS2Recomp`:

```
14faba7 runtime: TOL_TRACE_SPIN samples the guest pc at checkpoint points
3e7cc2d ioprp: boot IOMAN and FILEIO for real
17ec6b2 iop: boot IOPRP images and add a byte-based module loader
e31de0a iop: trace semaphores and event flags (TOL_TRACE_IOP_SEMA)
f15c7fb iop: trace syscalls and thread waits; log the registering thread
44e2420 iop: implement the ioman file services and give vblank a tick
02fc8e1 iop: log thread start under TOL_TRACE_IOP_THREADS
e7db01f iop: trace RPC dispatch and thread creation; give a missing sid IOP time
a9411d5 sif: serve the load-module command and report the load as finished
ff85001 runtime: report the SIF command channel as already initialised
e460718 runtime: InitMainThread returns the end of RDRAM (the heap-init fix)
```

Parent:

```
e03a765 docs: PCSX2 A/B - the real game runs eight threads after loading
6d3882b docs: loading sequence runs - ioman open/lseek/read/load all reached
13bd9cd ioman: fix the result-pointer offset and the version reply; loading advances
e73769f ioman: implement open/close/read/lseek synchronously and fix the init version
a2d560a docs: the loading sequence found - FUN_001664d0 waits on ioman file I/O
c1ce7f2 docs: notification channel implemented and verified; the loading gate is elsewhere
d0db05a sif: deliver IOP->EE commands into the guest's own libsif receive path
a808c36 ioman: answer sceSifInitIoman (sid 0x80000001 fn 0xFF)
fa4b852 ixaudio: answer the DTX/SJX create calls; docs: IOPRP boot + audio init complete
88ec48d docs: the port renders - game shows its NOW LOADING screen
3e57838 docs: sync-object trace - the wait is healthy, the provider is the IOPRP image
62d95c3 fileio: pass module arguments to the loader; docs: static analysis of 0x90000200
c9b9b34 docs: ioman tested and disproven; vblank #5 was a real IOP stall
a58dd9f docs: sound sid 0x90000200 has no provider
07197df docs: trace result for sid 0x90000200 - no server, not a guest-read failure
19e73be docs: file layer resolved, sound DTX is the next blocker
3d185b4 fileio: complete the game's file I/O; DVCI indexes all six AFS archives
b944262 fileio: serve the game's own file-open RPC (sid 0x80000597)
24a0636 docs: where the first cdrom0: open comes from
f1fdf80 sif: native bind/call + loadfile RPC; the guest loads its IOP modules
187e2b3 sif: report the command channel as initialised
fa34ffa runtime: tick-loop and per-PC probes; InitMainThread sp fix unblocks heap init
```

## 9. What was retracted this session (do not resurrect)

- "`$s5` on entry to `FUN_0011F650` names the tick state block" — `$s5` is **0** there; the function loads it itself.
- "`2415919872 = 0x90000200`" — it is `0x90000300`. That bad hex conversion sent a whole sound investigation after the
  wrong sid.
- "IxAudio's init thread waits on semaphore 10" — kind 4 of `Wait` is **EventFlag**; event flag 10 is a healthy
  self-signalled tick loop, not a deadlock.
- "The loading gate is an IOP→EE notification" — the channel was implemented and verified; delivering the ioman ready
  command changes nothing, because the guest is spinning in its own loading loop.
- "The `for(;;)` at `0x1666B4` is the load-status wait loop" — it is one of the sequence's *error* paths.
- "The IOPRP image contains the sound server" — it does not; the sid appears nowhere in it.

