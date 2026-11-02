# Session 62 handover — Tales of Legendia native port

**Goal:** native PC port of *Tales of Legendia* (SLUS_212.01) via PS2Recomp. Not an emulator, not an
emulator-fidelity project. The runtime is a fixed asset from `vamidi/PS2Recomp`; bend it to this one game.

---

## 1. Where the port actually stands

| milestone | state |
|---|---|
| Recompile + launch | done — clang-cl + SDL3, 13,520 units, 0 decode failures, 0 dispatch errors |
| Guest reaches its own startup | done — `FUN_00165ad0 game-startup` runs |
| Guest creates threads | **never** — syscall `0x20 CreateThread` is called zero times |
| File I/O | none — the guest never asks (verified with `TOL_TRACE_FILEIO` at both host choke points) |
| Graphics | none — `transfers=0 draws=0 passes=0 presents=0` |

**Current blocker, stated precisely:** after `SetupThread`/`SetupHeap` the guest idles in a timer-tick
dispatcher (`FUN_0011f650`, `0x11F650..0x11F8B4`), servicing ~16 ticks/second, and never creates the threads
that would drive the game. The question is **what should enqueue work into that tick loop and why the queue
is empty** — not "which hardware handshake is stuck".

## 2. Proven facts (each measured, re-checkable)

- `ra=0` parks are **normal**: `EeScheduler::startThread` and `dispatchSyscallOverride` set `$ra=0` by design;
  "[ee-park] … invocations=1" is a guest syscall handler completing, and the scheduler pops its invocation.
- `0x10001000 / 0x10001010 / 0x10001020` are **EE Timer 2** (`RCNT2_COUNT / MODE / TARGET`), *not* SIF.
  Confirmed against PCSX2 `Hw.h`. The guest clears Timer 2's overflow bit (`0x800`, write-1-to-clear) and
  counts overflows as a time base — the `0x0F82` / `0x0782` / `0x0B82` writes are exactly that.
- Timer emulation is correct: config mask `0x3FF`, status `0xC00`, CUE = bit 7, and
  `kEeTimerClockHz = { 147456000, 9216000, 576000, 15734 }`; `advanceEeTimers` is pumped from
  `EeScheduler.cpp:712`.
- Vblank pacing is already right: `kVBlankPeriod = 16667 µs`, and events require **both** cycle and host
  deadlines (`EeScheduler.cpp:2421`). The old project's "vblank 200× too slow" bug is **not** present.
- The guest does **no SIF DMA** (zero writes to `0x1000C000+`).
- The guest does **not** read `0x394378`, `0x394370`, `0x394380` or `0x38CCF8` during boot (0 hits, 32- and
  64-bit probes). The old project's staged-init handshake is **not** this stage's blocker.

## 3. Retracted (do not resurrect)

- "The guest is stuck on a **SIF handshake**" — wrong; those are timer registers.
- The `0x11F400` native override that followed from it — it wrote invented values into Timer 2, corrupting
  time. Withdrawn in `fc7db17`; `src/runtime/tol_overrides.cpp` now installs nothing and explains why.
- "Merged vs raw map explains the stall" — both parked (196 vs 246 per 15 s). Map boundaries are a
  correctness matter, not this blocker.

## 4. The assignment (in order)

1. **Name the tick loop's state block.** `FUN_0011f650` computes `state = s5 - 0x7308` at `0x11f694` and reads
   the queue head with `lw $s0, 0x18(state)` at `0x11f698`. Log `s5` once on entry to `0x11F650` (env-gated,
   one line) to get the address — a guess at the value was already wrong once.
2. **Dump the queue** at that address + `0x18` while the tick runs. Decide:
   - queue always empty → startup never enqueues; the bug is upstream, in what should have started work;
   - queue non-empty but never drains → a completion never arrives, and the node contents name which service.
3. **Then chase the missing `CreateThread`.** With the queue understood, the startup path that should create
   threads becomes traceable by its syscall (`0x20`) rather than by guesswork.
4. **Latent, do not forget:** `0x232310`, `0x2324A8`, `0x2324BC` are **holes in `config/map.csv`** — the exact
   addresses where the old tree lost thread 1. They are not hit yet (0 missing-function errors). When they are,
   the failing PC gives the extent; add rows then, not before.

## 5. Tooling you will want immediately

```
# build (13,520-unit corpus; ~2-3 min for a runtime change + 138 MB link)
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'

# run from the disc tree so cdRoot is data\CD (NOT data\ — that is the much smaller mistake made earlier)
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Probes: `TOL_TRACE_BOOT`, `TOL_TRACE_PARK` (prints the dispatch ring), `TOL_TRACE_SYSCALL` (args, result, call
site), `TOL_TRACE_DISPATCH`, `TOL_TRACE_FILEIO`, `TOL_TRACE_SIF`, `TOL_TRACE_READ_ADDR=0xADDR` (32- and 64-bit),
`TOL_GFX_STATS_EVERY`.

Address lookup: `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\map_lookup.ps1 0x11F650`
(script execution is disabled on this machine, so the `-Bypass` form is mandatory; `-Map data\map.csv` for the
raw export).

Corpus: regenerate with the standalone recompiler if the map changes —
`build\recomp\ps2xRecomp\ps2_recomp.exe data\config_fixed.toml` (merged map, 13,519 rows) or
`data\config_orig.toml` (raw 53,221 rows — a full rebuild of that takes >15 min, avoid).

## 6. Land mines

- **Never** interpret a hardware address from memory: `0x10001000` cost a whole workstream. Check `Hw.h` first.
- A probe that covers the wrong width reports zero: 32-bit-only instrumentation missed `ld` loads.
- `tol.exe` must not be running when linking (kill with `Get-Process tol | Stop-Process -Force`).
- The merged map (`config/map.csv`) is the fast corpus; the raw one is a control, not a baseline.
- `vamidi/PS2Recomp` has **four local commits not pushed** (`e023f7a`, `45188bd`, `db0452f`, `9b82cef`);
  two push attempts hung on the network.
