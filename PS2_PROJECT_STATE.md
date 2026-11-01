# Tales of Legendia — native PC port (state)

> Fresh start, 2026-11-01. The previous 59-session history is archived at
> `docs/state-archive/PS2_PROJECT_STATE_2026-11-01_sessions01-59.md`.
> Everything below described the **new** tree only.

## 1. What this project is

A **native PC port** of *Tales of Legendia* (PS2, `SLUS_212.01`) produced by static recompilation.
The game's MIPS is translated to C++ ahead of time and compiled into a normal Windows executable; there is no
interpreter loop for the EE CPU. This is **not** a PS2 renderer reimplementation: the goal is the game drawing into a
normal window.

Starting point is the `DQ8-Recompiled` skeleton (Sinan-Karakaya), adapted to Tales of Legendia.

## 2. Tree

| Path | Contents | Owner |
|---|---|---|
| `src/runtime` | entry point (`main.cpp`), overlay dispatch, ToL-specific integration | ours |
| `src/gfx` | GS state, VRAM, transfers, trace replay, SDL-GPU renderer (`backends/sdlgpu`) | ours |
| `src/ui` | in-game menu on F1: display, sound, controls, QoL | ours |
| `data` | game inputs (`SLUS_212.01`, ISO, TOML) + the generated corpus in `data/output` | local, gitignored |
| `thirdparty/PS2Recomp` | submodule: analyzer, recompiler, `ps2_runtime` | pinned |
| `thirdparty/imgui` | submodule: backs the F1 overlay | pinned |
| `thirdparty/simde` | submodule: SSE intrinsics on non-x86 hosts | pinned |

Pinned commits (`git submodule status`):

```
e5636981  thirdparty/PS2Recomp   main
391f714c  thirdparty/imgui       master
65c15e6b  thirdparty/simde       master
```

## 3. Where the generated code is imported (this was the open question)

The generated corpus is **not** included by hand anywhere. It is picked up through one CMake variable:

- `DQ8_GENERATED_DIR` (root `CMakeLists.txt:86`, `CACHE PATH`) — default
  `build/generated/SLUS_212.07`; the ToL value must become our corpus directory.
- `src/runtime/CMakeLists.txt:29` — `file(GLOB DQ8_GENERATED_SOURCES CONFIGURE_DEPENDS "${DQ8_GENERATED_DIR}/*.cpp")`
  builds the static library `dq8_generated` from every `.cpp` there, and refuses to configure if it finds none.
- `src/runtime/CMakeLists.txt:64-70` and `:118` — the same directory goes on the include path, which is how
  `ps2_recompiled_functions.h`, `ps2_recompiled_stubs.h`, `ps2_syscalls.h` and `ps2_stubs.h` resolve (PCH list at
  `:74-77`). `main.cpp` includes `ps2_recompiled_functions.h` to name individual functions.
- The runtime side (`ps2_runtime` target + `ps2_runtime.h`, `ps2_syscalls.h`, `ps2_stubs.h`, `ps2_runtime_macros.h`)
  comes from `thirdparty/PS2Recomp/ps2xRuntime` — all four headers are present in the pinned commit.

**The corpus already exists**: `data/output` holds 53,558 `.cpp` + `ps2_recompiled_functions.h` (4.6 MB) +
`ps2_recompiled_stubs.h` + `register_functions.cpp` (20 MB), generated earlier from this same ELF by
`vamidi/PS2Recomp`. Each unit is the familiar shape:

```cpp
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
void AddDmacHandler_0x1143f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) { ... }
```

## 4. What is missing from the pasted skeleton

Resolved during setup:

| Item | Resolution |
|---|---|
| `cmake/*.cmake` | pasted; `cmake/Dq8Icon.cmake` now skips when `res/icon` is absent, which is what configure used to fail on |
| `tools/runtime/render_cadence.h`, `gs_fan_probe.h` | copied from the DQ8 reference so `main.cpp` compiles verbatim; both are env-gated and are DQ8-specific diagnostics, never run by default |
| `tools/runtime/CMakeLists.txt` (its test executables) | deliberately not added — DQ8-only harness |
| `res/`, `config/`, `setup.py` | only needed for regeneration or the macOS bundle; not used on this path |

## 4b. Runtime lineage: vamidi/PS2Recomp + the DQ8 reference's ps2xRuntime

`vamidi/PS2Recomp` is the pinned submodule, but the DQ8 skeleton is written against a runtime that has
features upstream lacks. Rather than rewrite `main.cpp` to the older API, `ps2xRuntime` was **synced from the
local DQ8 reference** (`D:\Personal\C++\DQ8-Recompiled-main\thirdparty\PS2Recomp`): 317 files, 78 new —
native IOP bridge (`ps2_native_iop`: real IRX modules with SPU2 mixed to the host device), threaded GS
backend, EE fiber, host pacing, and the VU1 compile pipeline (`ps2_vu1_fmac/region/program/cache/capture`).

Preserved through the sync: the ToL corpus in `ps2xRuntime/src/runner` (53,558 files) and our extra files
(`includes/ps2_recompiled_*.h`, `ps2_vfs`, `ps2_rom_device`, `gs_texture_page_cache`). The
`RecompiledFunction` ABI is identical on both sides (`void(*)(uint8_t*, R5900Context*, PS2Runtime*)`), so
**no regeneration is required**.

The sync is committed *inside* the submodule (`b314e1f`) and the parent records that gitlink. To make it
permanent it must be pushed to `vamidi/PS2Recomp`, otherwise `git submodule update` restores the old runtime.

Only one line of `main.cpp` differs from the reference: the `--trace-printf` path referenced DQ8's
`FUN_00119508_0x119508`, which does not exist in ToL's corpus, so that block now reports "no guest printf
wrapper is mapped for this title yet".

## 5. Build plan (adapted from the DQ8 wiki)

```powershell
# 1. recompiler (standalone, from the submodule)
cmake -S thirdparty/PS2Recomp -B build/ps2recomp-standalone -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF
cmake --build build/ps2recomp-standalone --target ps2_recomp

# 2. generate (only if the existing corpus is unusable)
<ps2_recomp> data/config.toml

# 3. the port
cmake -S . -B build/game -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DDQ8_LINK_GENERATED=ON -DDQ8_GENERATED_DIR=<corpus> -DDQ8_GFX_ENABLE_SDLGPU=ON
cmake --build build/game --target dq8
```

## 6. Status

| Step | State |
|---|---|
| commit reset + submodules | done (`580cc0d`, `8b4786f`) |
| state archived + rewritten | done (`2bb3c88`) |
| runtime synced from reference | done, submodule commit `b314e1f` |
| configure | **passes** — `linking 53558 generated translation units` |
| `ps2_runtime` (incl. native IOP / threaded GS / VU1 pipeline) | **builds clean** |
| `dq8_gfx_gs` (our GS/gfx layer) | **builds clean** |
| `main.cpp` | **builds clean** |
| `dq8_generated` (53,558 corpus TUs) + final link | **compiling** — no code errors; run with `-j 6` |

**Build note (this machine):** `-j 12` fails with `C1002 compiler is out of heap space` / `C3859 Failed to create
virtual memory for PCH` / `C1076 internal heap limit` — each `cl.exe` holds the PCH, which includes the 4.6 MB
`ps2_recompiled_functions.h`. 64 GB RAM / 16 cores needs `-j 6`. The objects already produced are kept; ninja resumes.

## 7. Open decisions

## 7. Working rules for this tree

- `thirdparty/*` are submodules: never edit them, fix locally in `src/`.
- Generated `.cpp` are build inputs, not source: they live under `data/` (gitignored) or `build/`.
- `data/*` is gitignored (game files); only `data/README.txt` is tracked.
- Keep the corpus flags (`-fno-strict-aliasing -fwrapv`, `-msse4.1`) — they preserve guest semantics.

## 8. Build toolchain (decided 2026-11-01)

**Build the corpus with clang-cl, not MSVC.** `cl.exe` dies with `C1002` (out of heap, pass 2) on four single
functions of 8.7-9 MB / ~226k lines (`FUN_00256bc8`, `FUN_00256d58`, `FUN_00256cc0`, `FUN_002578b8`); DQ8's corpus
caps at 0.8 MB so it never hits this. The `/Od` + no-PCH workaround in `src/runtime/CMakeLists.txt` applies only when
the compiler is MSVC and did not save those four.

```powershell
cmd /c '"...\VC\Auxiliary\Build\vcvars64.bat" && cmake -S . -B build/clang -G Ninja `
      -DCMAKE_BUILD_TYPE=Release -DDQ8_LINK_GENERATED=ON `
      -DCMAKE_C_COMPILER="...\VC\Tools\Llvm\x64\bin\clang-cl.exe" `
      -DCMAKE_CXX_COMPILER="...\VC\Tools\Llvm\x64\bin\clang-cl.exe"'
cmd /c '"...\vcvars64.bat" && ninja -C build/clang -j 12 dq8'
```

Configure: `Clang 22.1.3`, `linking 53558 generated translation units`, `using lld` for the corpus link (lld matters:
ld.exe would struggle with 53k objects). clang-cl on Windows uses MSVC's STL and linker, so the DQ8 wiki's libc++/LLVM-23
caveat does not apply. Job count: 12 is fine for clang (~1 GB/job); MSVC needed 6 and still failed on the giant units.

---

## Session 60 (2026-11-01) — RESTART on the PS2Recomp toolchain; the black screen was a bad function map

### What the project is (framing that was wrong before)
PS2Recomp is a **static recompilation toolchain for native PC ports**, not an emulator: `ps2_analyzer` finds function
boundaries -> `ps2_recomp` emits C++ -> `ps2xRuntime` stands in for the PS2. A port = generated corpus + runtime +
a little per-game glue. Fix surfaces in order: **TOML (stub/skip/patch) -> game override -> runtime C++ -> regenerate**.
Deep guest stepping is the last resort, not the first.

### Tree
Fresh start from the DQ8-Recompiled skeleton: `src/runtime` (entry/overlays), `src/gfx` (GS + SDL-GPU renderer),
`src/ui` (F1 menu), `thirdparty/{PS2Recomp,imgui,simde}`, `config/` (function map), `data/` (gitignored: ELF, ISO, corpora).

### Build (works)
- **clang-cl 22.1.3 + lld** (`build/tol2`). MSVC's `cl.exe` CANNOT compile this corpus: four units are 8.7-9 MB /
  ~226k-line single functions and die with `C1002` regardless of `-j`. `-j 12` is safe with clang.
- **SDL3 3.4.18** dev package extracted to `out/sdl3` (gitignored), configured with `-DSDL3_DIR=.../cmake`.
  `tol: SDL GPU backend available (--gs=sdlgpu)`. `SDL3.dll` must be copied beside each new `tol.exe`.
- Corpus build with MSVC also needs `-j 6`; with clang use 12.
- `runtime: drop /Qspectre-` (submodule `eb2d5a3`): CMake reports `MSVC=true` for clang-cl, so clang-cl parsed it as a filename.

### Runtime lineage
`vamidi/PS2Recomp` was kept, but `ps2xRuntime` was **synced from the local DQ8 reference** (`b314e1f`, 78 new files:
native IOP + SPU2, threaded GS backend, EE fiber, host pacing, VU1 pipeline). Corpus + `RecompiledFunction` ABI
(`void(*)(uint8_t*,R5900Context*,PS2Runtime*)`) preserved. Pushed to `vamidi/PS2Recomp`.
Corpus shims needed afterwards, each verified against git history rather than guessed:
`SET_GPR_ZE32` -> alias of `SET_GPR_U32` (8,152 files), `before/afterVu0Access` (1,520 files, no-ops),
`eeTransferPending()` (old runtime forwarded to `EeScheduler::transferPending()`; no equivalent -> false).

### Rename
`DQ8 -> tol` everywhere: project `ToLRecomp`, target `tol`, `TOL_*` CMake vars/macros/namespaces/env vars,
`cmake/Tol*.cmake`. 0 references remain. Env switches are now `TOL_GFX_*`, `TOL_GS_*`, `TOL_TRACE_*` (28 of them) —
commands copied from the DQ8 wiki using `DQ8_*` silently do nothing.

### ROOT CAUSE of the black screen: the function map, not the runtime
`data/map.csv` (Ghidra export) treated **every secondary entry point as a function**: 40,253 of 53,221 rows were
`entry_*`, median row 32 bytes. The recompiler emitted each fragment as a standalone function ending in
`ctx->pc = <next instruction>`; resuming into one ran the enclosing routine's epilogue with `$ra == 0` -> `jr $ra` to
pc 0 -> EE thread dormant. That was the whole boot failure, and it happened inside `libc-init`.

**Corrected map** (`config/map.csv`, commit `0637e19`, 20,754 rows): fold an `entry_*` row into the previous row
**only when contiguous** (`entry.Start == previous.End`).
- 32,467 folded; 7,786 left standalone.
- First attempt folded everything -> 12,968 rows -> `FUN_003889d8` grew to ~300 KB and the recompiler reported 5,347
  `unhandled-opcode` errors decoding data as code. **A fold must never cross a gap.**
- Regenerated corpus: 20,699 recompiled, 0 decode failures (was 53,558 units, 9 MB monsters). `data/output_fixed`.

### Verified after the fix (TOL_TRACE_BOOT / TOL_TRACE_PARK)
- **No `[ee-park]` at all** — the `ra == 0` dormancy is gone; `crt0` and `libc-init` both return normally.
- `dispatchGuestBranch` DOES set `$ra` (`enter FUN_0011ea58 ra=0x001001F0` = crt0's return). No corpus-wide jal bug.
- `FUN_0011ea58`'s frame is balanced: `addiu sp,-0x10` / `sd $ra,0(sp)` / `ld $ra,0(sp)` / `addiu sp,+0x10` in the
  delay slot of its tail `j 0x120770`. Exonerated.
- Entry confirmed from the ELF header: `e_entry = 0x00100008 = "entry"` (528 B). `kDefaultEntryPoint` was DQ8's
  `0x0010008C` (+0x84); corrected, `--entry=` still overrides.

### CURRENT BLOCKER (next session starts here)
crt0 makes three JALs: `0x11EA58` (init chain), `0x114960` (FlushCache), `0x165AD0` (**main**, verified present:
384 B, table slot 104114). Only the first runs; it exits with
`pc=0x0011E900 ra=0x0011E9E4 sp=0x01EFFF70` (entry had `ra=0x001001F0 sp=0x01F00000`), so JALs 2 and 3 never execute
and the window stays black (60 fps presenter, `gif packets=0 draws=0 presents=0`).

Probes on `0x11E070` and `0x120770` (the last two calls in the init chain) were **never entered** — the derailment
happens earlier in `FUN_0011ea58`. `ra=0x11E9E4` is a return address from the `0x11E9xx` cluster, i.e. a slot the
current frame never wrote.

**Next:** add the remaining init-chain callees to `kBootPoints` (`0x11E808`, `0x11E948`, `0x11EF38`, `0x11F0B0`,
`0x11F2F0`, `0x1154B8`, `0x11EB80`) and find the first one that exits with a foreign `$ra`; then check that function's
map row for an overlap (`entry_0010012c` nesting inside `entry` is one known overlap in this map).

### Probes and build dirs
| item | where |
|---|---|
| `TOL_TRACE_BOOT=1` | `src/runtime/main.cpp` (`kBootPoints` + `bootTraceCall`), prints enter/exit with pc/ra/sp |
| `TOL_TRACE_PARK=1` | submodule `Kernel/EeScheduler.cpp`, logs every `pc == 0` dormancy with the producing function |
| `TOL_GFX_SHOW_FPS` / `TOL_GFX_STATS_EVERY` | per-frame `[gfx]` counter line (transfers/draws/presents/ee-dispatches) |
| working build | `build/tol2` (corrected corpus, SDL3) |
| old corpus | still at `thirdparty/PS2Recomp/ps2xRuntime/src/runner` for A/B |

### Session 60 update — the corrupting frame is FUN_0011e948

Boot trace with all 13 init-chain points wrapped (enter/exit with pc/ra/sp):

```
[boot] enter crt0            ra=0x00000000 sp=0x01FFFFF0
[boot] enter FUN_0011ea58    ra=0x001001F0 sp=0x01F00000   <- crt0's return, correct
[boot] enter FUN_0011e808    ra=0x0011EA68 sp=0x01EFFFF0
[boot] exit  FUN_0011e808    pc=0x0011EA68 ra=0x0011EA68   <- clean
[boot] enter FUN_0011e948    ra=0x0011EA70 sp=0x01EFFFF0   <- return address correct
[boot] exit  FUN_0011e948    pc=0x0011E900 ra=0x0011E9E4 sp=0x01EFFF70   <- CORRUPT
```

`FUN_0011e948` is the frame that introduces it:
- entered with `ra = 0x0011EA70` (the instruction after its `jal` in `FUN_0011ea58`) - correct;
- exits with `ra = 0x0011E9E4` and `pc = 0x0011E900`, `sp` down by 0x80;
- `0x11E9E4` lies BEFORE `0x11E948`, so it cannot be a return address of any call made inside it: it is a stale
  stack slot the epilogue restored -> **prologue/epilogue offset mismatch in this function**, not a runtime or map bug;
- it never unwinds to `0x0011EA70`, so `FUN_0011ea58`'s remaining calls (0x11EF38, 0x11F0B0, 0x11F2F0, 0x1154B8,
  0x11EB80, 0x11E070, 0x120770) and crt0's `FlushCache`/`main` JALs never run. Window stays black.

**Next:** compare `FUN_0011e948`'s `addiu sp,-N` / `sd $ra,off` with its `ld $ra,off` / `addiu sp,+M` in
`data/output_fixed/FUN_0011e948_*.cpp`. If they disagree the bug is in the recompiler's prologue/epilogue emission
(fix belongs in PS2Recomp, benefits every game); if they agree, the row's End is short and the body falls through
into the next row - then the map is at fault (see the known `entry_0010012c` nested inside `entry`).

### Session 60 update 2 — the dispatch failure is NOT a missing map entry

Corrected the containment rule (drop a contained row only when it is an `entry_*`; never drop `FUN_*`/named/`thunk_*`).
My first version deleted real functions, including `FUN_0011e900`, which is exactly the target that fails.

Final map: **13,519 rows** (32,467 labels folded contiguously, 7,235 duplicate labels dropped). Regenerated corpus:
13,519 functions, 13,464 recompiled, 0 decode failures, 13,520 .cpp. `0x11E900` **is** in the table
(`register_functions.cpp` has `// 0x11e900`), and the neighbourhood is complete:
`FUN_0011e900 (16) / FUN_0011e910 (52) / FUN_0011e948 (252)`.

**But the boot trace is byte-identical:**

```
[boot] enter FUN_0011e948 pc=0x0011E948 ra=0x0011EA70 sp=0x01EFFFF0
[boot] exit  FUN_0011e948 pc=0x0011E900 ra=0x0011E9E4 sp=0x01EFFF70
```

So `dispatchGuestBranch(0x11E900, ...)` still returns **false** even though the slot exists. The call site is:

```c
SET_GPR_U32(ctx, 31, 0x11E9E4u);
ctx->pc = 0x11E900u;
if (!runtime->dispatchGuestBranch(rdram, ctx, 0x11E900u, 0x11E9DCu, 0x11E9E4u, DirectCall, "JAL")) return;
```

`pc == 0x11E900 && ra == 0x11E9E4` on exit is therefore just "the dispatch declined and the function returned",
not a corrupted frame. **The frame handling was never wrong.**

**Next:** read `PS2Runtime::dispatchGuestBranch` and enumerate every path that returns false (missing target,
`ctx->pc == 0` after the call, stop requested, reentry rejection, ...), then log which one fires for this call.
That is a runtime question now, and the map is out of the picture.
