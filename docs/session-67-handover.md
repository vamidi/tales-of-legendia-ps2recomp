# Session 67 handover — implement the LLE CDVD device

Status: the port boots, loads all six AFS archives, renders, and advances frames. It stops on
`NOW LOADING` because the IOP's `cdvdman` imports are unbound in the LLE, so the game's streaming
read driver (`SRD`) reports `Drive Error (sceCdGetError = 0x1)` on every read.

This session found the exact cause at the import layer. The work left is mechanical: bind `cdvdman`
in the LLE and serve sector reads from `data/CD/CDVD.000`.

---

## 1. What was fixed this session (all committed, build green)

| commit | repo | change |
|---|---|---|
| `a3fbcb9` | parent | **Answer sid `0x80000593`** so the pad/mc init step can finish. This was the big one: the EE span on that call ~220x/s with 100 % of its time. |
| `91fd74a` | parent | Auto-detect the disc image (`CDVD.000` / `*.iso` / `*.bin`) beside the ELF when `--iso` is absent. |
| `1cde4bd` | submodule | `Iop::settle()` runs 16k-cycle slices and stops when the in-flight RPC completes (28 ms → 1.6 ms per KORG call). |
| `c3a9d75` | submodule | `readSectors` diagnostics (`TOL_TRACE_CDVD`) + past-EOF zero-fill. **Dead code in this config** (HLE path). |

Instrumentation available (all env-gated, safe to leave on during the next session):

| env var | what it shows |
|---|---|
| `TOL_TRACE_EE_DISPATCH=1` | turns/s, mean/max turn cost, **turn histogram by entry pc** (`[turnbucket]`) |
| `TOL_TRACE_EE_THREADS=1` | once a second: every EE thread's status, wait reason/id, priority, pc |
| `TOL_TRACE_EE_PC=1` | sampled guest pc at dispatch |
| `TOL_TRACE_RPC_STAGE=1` | per SIF call: setup / handleRpc / reply split, with sid + fn |
| `TOL_TRACE_IOP_CALL_TIME=1` | per IOP call: entry → submit+settle → wait-loop laps |
| `TOL_TRACE_IOP_IMPORTS=1` | **unbound imports, by library and ordinal** — this is what named the cdvdman ordinals |
| `TOL_TRACE_CDVD=1` | HLE readSectors diagnostics (not used by the LLE) |
| `TOL_TRACE_SPU2=1` | SPU2 register writes/reads (ADMAS, STATX) |

## 2. The blocker, measured

```
[ps2xIOP] [iop] started cdvd_driver at 105984
[ps2xIOP] [iop] no kernel service for cdvdman #4
[ps2xIOP] [iop] no kernel service for cdvdman #5
[ps2xIOP] [iop] no kernel service for cdvdman #7
[ps2xIOP] [iop] no kernel service for cdvdman #8
[ps2xIOP] [iop] no kernel service for cdvdman #9
[ps2xIOP] [iop] no kernel service for cdvdman #10
[ps2xIOP] [iop] no kernel service for cdvdman #11
[ps2xIOP] [iop] no kernel service for cdvdman #12
```

`grep cdvdman thirdparty/PS2Recomp/ps2xIOP/src/lle/kernel.cpp` → **no bindings at all**. Every
`cdvdman` import falls through to `Iop::unknownImport`, returns nothing, and the guest's SRD turns
that into a drive error. There is no CD-ROM device anywhere in `src/lle/`.

Confirmed separately:
* `data/CD/CDVD.000` is a real ISO9660 image — PVD at LSN 16 reads `\x01CD001\x01` + `PLAYSTATION`.
  Size `1073676288` = `0x3FFF0000` (~1 GiB), which may be **smaller than the retail disc** — handle
  reads past EOF (see §4.6).
* `main.cpp` now prints `[tol] disc image = …\CDVD.000` on every boot, so the path is available.
* The HLE `readSectors` (`src/emulator/imports/iop_cdvd.cpp:654`) is **never called** — this runtime
  runs the IOP in LLE mode. Do not debug it; port it instead.

## 3. How LLE import binding works

Two places, both required:

**a. `thirdparty/PS2Recomp/ps2xIOP/src/lle/kernel_handlers.inc`** — one line per handler:
```c
PS2X_IOP_HANDLER(cdvdRead)
```
This list also feeds `Iop::handlerFor`'s name table, so handler names must be unique.

**b. `kBindings` in `thirdparty/PS2Recomp/ps2xIOP/src/lle/kernel.cpp`** — maps
`{library, ordinal, handler}`:
```cpp
{"cdvdman", 5, "cdvdRead"},
```

`Iop::handlerFor(library, ordinal)` (kernel.cpp:100) resolves an import to a handler index; an
unbound import gets its own `unknownImport` entry labelled `"cdvdman #N"`.

Handler signature is `void Iop::name(CpuContext &c)`, with `arg(c, n)` for `$a0..$a3` and `ret(c, v)`
to set `$v0`. See the existing `registerIntrHandler` / `intrman` handlers in `kernel.cpp` for the shape.

## 4. Implementation plan

### 4.1 Thread a host-file accessor into `Iop` (the only real unknown)

`Iop` (src/lle/iop.h) has `Bus m_bus`, `Cpu m_cpu`, `Spu2 m_spu2`, `EeLink &m_ee` — but no host
file access. `NativeIop` (src/native_iop.cpp) owns `m_impl->host`, which has `openHostFile`,
`readHostFile` and `hostPath(HostPathKind::CdImage)`.

Recommended: add a tiny `<filesystem>`-based accessor to `Iop` (or pass the resolved image path in at
construction/reset) rather than routing through `NativeIop` — the LLE only needs "open this path,
pread this offset, return bytes". A `std::FILE*` held by `Iop`, opened lazily on first `cdvdRead`, is
enough and avoids touching the host abstraction.

Image path source: `PS2Runtime::getIoPaths().cdImage`, already populated by `main.cpp:91fd74a`.
If it is empty, fall back to `paths.cdRoot / "CDVD.000"`.

### 4.2 Handlers to add (standard `cdvdman` order — matches the measured unbound set exactly)

| ordinal | name | behaviour |
|---|---|---|
| 1 | `sceCdInit` | return 1 |
| 2 | `sceCdStandby` | return 1 |
| 3 | `sceCdStop` | return 1 |
| 4 | `sceCdPause` | return 1 |
| 5 | `sceCdRead` | **the real one** — see §4.3 |
| 6 | `sceCdReadIOPm` | like 5, destination is not needed (memset-style) — return 1 |
| 7 | `sceCdSeek` | record LSN, return 1 |
| 8 | `sceCdGetError` | return `lastError` (0 normally) |
| 9 | `sceCdGetToc` | zero the TOC buffer, return 1 |
| 10 | `sceCdSearchFile` | ISO9660 walk — see §4.4 |
| 11 | `sceCdSync` | return 0 (completed) |
| 12 | `sceCdGetDiskType` | return `0x14` (PS2 DVD) |
| 13 | `sceCdDiskReady` | return `2` (complete) |

The measured unbound ordinals were **4,5,7,8,9,10,11,12** — a subset of this table, which is strong
confirmation the mapping is right. Bind all 13 anyway; the module may import more later.

### 4.3 `cdvdRead` — lift from the HLE implementation

Source of truth: `thirdparty/PS2Recomp/ps2xIOP/src/emulator/imports/iop_cdvd.cpp`, function
`readSectors(lsn, sectors, destination)` at ~line 654. It already does:
* `sectors == 0` → success
* range check against IOP RAM
* `pread` from the image at `lsn * 2048`
* fall back to a synthetic ISO from `cdRoot`
* write the buffer into IOP RAM

Port it as `Iop::cdvdRead`, replacing the `memory.writeRam` with `m_bus` writes (IOP RAM base is in
`Bus`; see `iop.cpp` `writeMemory`/`readMemory` for the accessor pattern).

### 4.4 `cdvdSearchFile` — lift from the DQ8 repository

`D:\Personal\C++\DQ8-Recompiled-main\tools\launcher\launcher_iso.h` (~2 KB) is a self-contained
ISO9660 reader: `IsoEntry{path, lba, size, directory}`, `walk(lba, size, prefix, depth)`, PVD parse,
`;1` stripping, `"DIR/FILE.EXT"` paths. Reuse its walk logic for `SearchFile`, which the guest calls
with a path *and a buffer to fill* (struct starts with the path string, then LSN/size at fixed
offsets — match `iop_cdvd.cpp`'s `searchFile` implementation for the exact layout).

### 4.5 Completion semantics

The LLE delivers RPC requests synchronously via `deliverRequests()`; the cdvd handlers are plain
imports called from module code, so they should just do the work and `ret(c, 1)`. If the guest polls
`sceCdSync`/`DiskReady` in a loop, returning "complete" immediately is correct for an emulator.

### 4.6 Past EOF

`CDVD.000` is `0x3FFF0000` bytes = 524,224 sectors. If the retail disc is larger, reads past that
must **zero-fill and succeed**, not fail — the SRD cannot recover from a drive error mid-stream. The
HLE version already has this (`past-eof-zero-filled`, `c3a9d75`); carry it across and log the first
few occurrences so the boundary is visible.

## 5. Verification

Run:
```
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Success criteria, in order:
1. `[cdvd] read lsn=… sectors=… dest=… image=… ok=1` appears (add the same trace, `TOL_TRACE_CDVD`).
2. `SRD: Drive Error` disappears from the log.
3. Screenshots advance past `NOW LOADING` (`TOL_GFX_SCREENSHOT_DIR=<dir>` writes one every 60 frames)
   and a logo/intro appears.
4. `TOL_TRACE_EE_DISPATCH=1` shows the CRI buckets (`0x166794`, `0x1979B0/A4`) declining as loading
   finishes, rather than a single bucket eating the second.

Baseline before this work, for comparison: 3 frames in 60 s, `srd errors = 245`, one dominant turn
bucket.

## 6. Pitfalls learned the hard way

* **`loadcore`/`sysclib`/`sysmem` imports are also unbound** (126 unbound imports total). They return
  nothing and the modules tolerate it. Do not assume "unbound => broken"; only CDVD matters here.
* **`Iop::settle()` budget**: it now runs 16k slices and stops when `m_active` empties. Keep that —
  running the full 2,000,000-cycle budget per RPC cost 28 ms per call.
* **`NativeIop::mutex` is the contention point** for every EE RPC (`probe=5.4 ms`, `rest=0.000`).
  If RPC latency regresses, that lock is where to look; the holder is not `settle()`.
* **The `0x80000593` reply value `1` is an experiment**, not a recovered ABI. The step's terminal
  branch also `printf`s a diagnostic, so `1` lands in an "unexpected value" path that happens to let
  the game continue. If later behaviour looks wrong, revisit the value (the polling set is
  `{0, -1, 0x20}`).
* **Do not run `git submodule update --remote`** — the submodule is ahead of `origin/main`.
* `tools/run_game_agent.bat` produces 0-byte logs under PowerShell; use
  `Start-Process -RedirectStandardOutput/-Error`.
* `Load32` on an unaligned guest address raises a real guest address error; use `Load8`.

## 7. Build

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'
```

## 8. Reference: how the current blocker was found (method worth repeating)

1. `TOL_TRACE_EE_DISPATCH` histogram → 100 % of turns entered at `0x118628` (`sceSifCallRpc`).
2. `TOL_TRACE_RPC_STAGE` → `setup 0.001 / handleRpc 5.4–6.8 / reply 0.12`, `handled=0`, sid
   `0x80000593` fn 4.
3. `[iopstage]` inside `IopSubsystem::handleRpc` → the cost was the `native.server()` *probe*
   (mutex), not the lookup.
4. `TOL_TRACE_PC=0x102D18` → caller `0x1AB490`; disassembly showed the step returns `0` because the
   RPC's return (the reply word) was `0` = "pending".
5. Answering the sid broke the loop: turns 443/s → 38 944/s, frames 1 → 12/72/132.
6. The screen still said NOW LOADING → log showed `SRD: Drive Error` → `TOL_TRACE_IOP_IMPORTS` named
   the unbound `cdvdman` ordinals.
