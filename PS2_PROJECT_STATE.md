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

- `TOL_GENERATED_DIR` (root `CMakeLists.txt:86`, `CACHE PATH`) — default
  `build/generated/SLUS_212.07`; the ToL value must become our corpus directory.
- `src/runtime/CMakeLists.txt:29` — `file(GLOB TOL_GENERATED_SOURCES CONFIGURE_DEPENDS "${TOL_GENERATED_DIR}/*.cpp")`
  builds the static library `tol_generated` from every `.cpp` there, and refuses to configure if it finds none.
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
      -DTOL_LINK_GENERATED=ON -DTOL_GENERATED_DIR=<corpus> -DTOL_GFX_ENABLE_SDLGPU=ON
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
| `tol_gfx_gs` (our GS/gfx layer) | **builds clean** |
| `main.cpp` | **builds clean** |
| `tol_generated` (53,558 corpus TUs) + final link | **compiling** — no code errors; run with `-j 6` |

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
      -DCMAKE_BUILD_TYPE=Release -DTOL_LINK_GENERATED=ON `
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
debug knobs are `TOL_*` (renamed from the inherited DQ8 names); a command written for the DQ8 wiki using `DQ8_*` now does nothing.

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

---

## Session 60 (cont.): the real defect — computed jumps into mid-function

After the map fix, the boot no longer stalls on `0x11E900`. The next run (boot paths are interleaving-sensitive, so
successive runs are not identical) produced:

```
Error: No exact recompiled function for guest PC 0x13f660 ... codeRegion=yes
Error: No exact recompiled function for guest PC 0x13f6b0 ...
```

Diagnosis (all verified, not guessed):

- Both addresses **are** inside a real function: `FUN_0013f4b0`, `0x0013F4B0..0x0013F7C0` (784 bytes). The map is right.
- `FUN_0013f4b0` **does** emit resume labels for 20+ internal addresses (`case 0x13f4ecu: ... case 0x13f634u:`),
  and each of those has a table slot.
- `0x13F660` and `0x13F6B0` are **not** among them: `Select-String "case 0x13f660u"` = 0, `"// 0x13f660"` in
  `register_functions.cpp` = 0.

So the guest reached a mid-function address the recompiler never enumerated — a **computed branch** (`jalr` through a
function pointer or a jump table). The map is not at fault; the *entry-point enumeration* is incomplete by construction.

### Why the earlier attempt failed

`function_emitter.cpp` builds its own local `resumeTargets` from `analysisResult.resumeEntryPoints`, which drives only
the `switch (ctx->pc) { case ...: goto label_...; }` and the `label_` emission. The **function table** is a separate
pipeline: `function_table_emitter.cpp:124` iterates `cg.m_resumeEntryTargetsByOwner`, which is populated in
`ps2_recompiler.cpp:1860` from `analysisResult.resumeEntryPoints` / `indirectFallbackEntryPoints`.

Adding every instruction to `resumeTargets` in the emitter therefore produced labels and cases but **no table slots** —
`lookupFunction()` consults the table first, so dispatch still failed. (That patch has been reverted.)

### Option A implemented (the correct site)

`ps2_recompiler.cpp`, in `discoverAdditionalEntryPoints()`, where the decoded instruction list is in scope:

```cpp
            // Option A: a computed jump (jalr through a function pointer or a jump table) can enter a function at
            // ANY decoded instruction, and the guest has no notion of an "entry point". Register a resume slot for
            // every instruction so such a jump resumes mid-function instead of failing to dispatch. The generated
            // table is dense per 4 bytes, so this fills existing slots rather than growing the table.
            for (const Instruction &everyInst : instructions)
            {
                ownerTargets.push_back(everyInst.address);
            }
```

This feeds `m_resumeEntryTargetsByOwner` -> `setResumeEntryTargets` -> `cg.m_resumeEntryTargetsByOwner`, which is
consumed by **both** the table emitter and `function_emitter.cpp:60-62` (so labels and cases are emitted for the same
addresses automatically). Table base/end and slot count are unchanged; only the number of initialised slots grows.

Cost: bigger `register_functions.cpp` and larger TUs (a `case` + `label_` per instruction). Acceptable; clang-cl has
already proven it handles the large units.

### Verification contract

Regeneration must show, before any boot result is trusted:

- `reg163f660=1` and `reg163f6b0=1` in `data/output_fixed/register_functions.cpp`
- `corpus_cpp=13520`, 0 decode failures
- `tol.exe` relinked *after* the corpus (previous run was invalidated by testing a 21:46 binary against a 22:07 corpus)

Then the boot run must show `No exact recompiled function` count = 0.

### Still uncommitted

- `TOL_TRACE_CKPT` (checkpoint-decline counter) and `TOL_TRACE_FINDADDR` (FindAddress args/result) in the submodule
- `0x11E900` boot point in `src/runtime/main.cpp`
- `ps2_recompiler.cpp` option-A change + `build/recomp`

---

## Session 61: path audit + diagnostics hygiene

### 1. The map-lookup caveat is fixed

`tools/map_lookup.ps1` (new) resolves an address to its covering row. Two bugs were behind the blank output
reported earlier:

- **The original one-liner indexed a single object**: `Select-Object -First 1` returns one `PSCustomObject`, so
  `$r[1].Name` is `$null`. It must be `$r.Name`.
- **Then my first script version stayed silent**: (a) `[Convert]::ToInt64("0x00100008", 16)` rejects the `0x`
  prefix that `map.csv` uses (must strip it), and (b) the `begin`/`process` block form did not run the `process`
  block when invoked with `-File`, exiting 0 with no output. Rewritten as a plain script body.

Verified: `powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\map_lookup.ps1 0x13F660` ->
`FUN_0013f4b0 ... (interior +0x1B0)`; comma lists and `-All` work too.

Answers it produced for addresses that previously printed blanks:

| address | owner | relation |
|---|---|---|
| `0x109144` | `FUN_00109108` (192 B) | interior +0x3C — where `game-startup` returned to |
| `0x1450A0` | `FUN_001450a0` (656 B) | **start** |
| `0x145650` | `FUN_00145640` (168 B) | interior +0x10 — the render edge |
| `0x11E8C0` | `FUN_0011e878` (136 B) | interior +0x48 — the `ee-park` source |
| `0x165AD0` | `FUN_00165ad0` (384 B) | **start** — game startup |

### 2. ELF paths: the reference layout differs, and ours was pointing at the wrong directory

The runtime derives `cdRoot` from the ELF's own directory (`PS2Runtime::configureIoPathsFromElf`,
`ps2_runtime.cpp:1091`: `hostRoot = cdRoot = elfDirectory`, `mcRoot = elfDirectory/mc0`). It is therefore the
*launch path* that decides where `cdrom0:` resolves.

- **DQ8 reference**: ELF at `config\SLUS_212.07`, no disc tree or ISO in the repo -> `cdRoot = config\`
  (contains nothing), so DQ8 only works with `--iso=` or `--cd-root=`. Its `main.cpp` is otherwise identical
  to ours.
- **This repo**: two byte-identical copies of `SLUS_212.01` (SHA-256 `03E18C8B...C9B6`) - one at `data\` and one
  at `data\CD\`. The **real disc tree is `data\CD`**: `AFS\{BATTLE,MAP,FIELD}.AFS`, `IRX\*.IRX`, `MBS\`,
  `CDVD.000` (1.0 GB), `SYSTEM.CNF`.

So launching `data\SLUS_212.01` sets `cdRoot = data\` and every `cdrom0:` read misses the tree. The correct
invocation is:

```
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

which yields `[tol] cd root = ...\data\CD` (now printed unconditionally; `main.cpp` also got its stale
`SLUS_212.07` usage strings fixed).

**Caveat: today the two layouts behave identically** (both: 0 dispatch errors, boot reaches `game-startup`,
`draws=0`) because the guest performs **no file I/O at all** yet (0 IRX/cdrom/VFS lines in either run). The path
defect is latent but real and will bite the moment the guest opens its first IRX or AFS file.

### 3. Side effect of option A, found and fixed: render-cadence probe

`tools/runtime/render_cadence.h` required every registered slot in a hardcoded window `0x145080..0x145658` to
resolve to one function. `FUN_001450a0` starts at `0x1450a0`, inside that window, so the check was always wrong;
it only went unnoticed because `hasFunction(0x145080)` used to be false (that address is interior to
`FUN_00145060`). With dense per-instruction slots the probe finally ran, found the neighbour, and disabled itself:
`[render-cadence] unavailable: unexpected interior function at 001450a0`.

Fixed by deriving the extent from the table instead of trusting a hardcoded window: walk outward from `kEntry`
while the slot still resolves to the same function (bounded by +/-32 KB). `kEntry` corrected to `0x145650`, the
documented render edge, inside `FUN_00145640`. Result:

```
[render-cadence] enabled: completed guest edge 145650->1439b8; wrapped=42 entries over 00145640..001456e4
```

### Current blocker (unchanged, next to chase)

No graphics traffic at all: `transfers=0 draws=0 passes=0 presents=0`, and `TOL_TRACE_PARK=1` shows ~17
thread-returns/second with `ra=0x0` and a fresh `sp=0x01FFFFF0` from `0x11E8C0` (`FUN_0011e878` interior) and
`0x11EEF0` — thread entries whose bodies return immediately. Until the guest gets past that it never opens a
file, so the cd-root fix cannot be validated end to end yet.

---

## Session 61 (cont.): does tol.exe perform file I/O? **No — it never reaches it**

The earlier "0 IRX/cdrom/VFS lines" was weak evidence, because the syscall and stub layers only log *failures*.
So both host-FS choke points were instrumented behind `TOL_TRACE_FILEIO=1`:

- `PS2Vfs::open` (`ps2xRuntime/src/lib/ps2_vfs.cpp`) - the guest `fileio` path; logs guest path, resolved host
  path, rom0 reads and every failure mode.
- `PS2IopHostAdapter::openHostFile` (`ps2xRuntime/src/lib/ps2_iop_host.cpp`) - the IOP-side opener used for
  `cdrom0:`, `host:`, `mc0:`; logs the resolved host path, size and handle.

Result over a 15 s run launched as `data\CD\SLUS_212.01`:

```
count of [fileio] lines: 0
probe strings found in tol.exe: 2   <- instrumentation is linked, so 0 means "never called"
```

So **tol.exe performs no file I/O at all** — not a single open attempt, successful or failed, at either layer.
The ELF itself is read by `std::ifstream` in `loadELF` (not through these paths), and that is the only file the
process touches.

Consequence: the cd-root fix cannot be validated end to end yet, and it is not the current blocker. The guest
stops earlier, in the thread churn (`~17 thread-returns/s, ra=0x0, sp=0x01FFFFF0` from `0x11E8C0` interior of
`FUN_0011e878`). Until the guest starts asking for an IRX or AFS file, both `data\` and `data\CD\` launch paths
remain observationally identical.

Interpretation detail: because no IRX is ever loaded, the failure is **not** "the IOP module files are missing" -
the guest never gets far enough to request them.

---

## Session 63 (2026-11-02) — the tick queue is empty because the main thread died in heap init; fixed

### 1. Assignment step 1-2 answered: the tick loop is the Timer-2 interrupt handler, and its queue is empty

`FUN_0011F650` is registered with `AddIntcHandler(0xb, 0x11F650, 0, 0)` in `FUN_0011f0b0` (0x11F150) - cause 0xb is
EE **Timer 2**, not SIF. Its state block is `s5 - 0x7308` with `s5 = 0x00390000` (`lui $s5, 0x39` at 0x11F690), so
state = `0x00388CF8` and the active list head is `0x00388D10` (state+0x18). `$s5` is **0 on entry** - the function
loads it itself, so the handover's "log s5 on entry" always prints 0; take the address from the code, not the register.

Layout (from `FUN_0011F650` / `FUN_0011F590` insert / `FUN_0011F618` remove):

| offset | meaning |
|---|---|
| +0x00 | Timer-2 overflow counter (high half of the time base) |
| +0x10 | node count |
| +0x14 | free-list head |
| +0x18 | **active-list head** |
| +0x1C | currently running handle (0xFFFFFFFF = idle) |

Node = 0x40 bytes, pool at `0x3E3700` (128 nodes), `+0x00` next, `+0x08` id, `+0x0C` flags (bit 1 = periodic),
`+0x10` interval, `+0x18` last, `+0x20` start, `+0x28` callback, `+0x30` arg.

**Measured with `TOL_TRACE_TICK=1`: `count=0`, `free=0x003E3700` (untouched), `active=0x00000000` on every tick.**
So startup never enqueues: the queue is empty because the code that would create timers never ran. The timer
subsystem itself is fine (handler registered, Timer 2 ticking at ~16 Hz).

### 2. Root cause: `InitMainThread` (syscall 0x3C) returned the BOTTOM of the stack area

`crt0` at 0x1001C4 does `li v1,0x3c; syscall; move sp,v0` with `a1 = -1` (stack at end of RDRAM) and
`a2 = 0x100000`. `ps2_syscalls::SetupThread` computed `initialStack = PS2_RAM_SIZE - 0x100000 = 0x01F00000`
(the *bottom*) and returned that as `$sp` whenever `asyncCallbackStackTop() <= initialStack` was false - which is the
normal state, since the invocation pool top starts at `PS2_RAM_SIZE`. Only a caller passing `stack_size == 0`
(DQ8) got `PS2_RAM_SIZE`.

With `sp = 0x01F00000` the main thread's frames sat **inside the guest's own heap** (`0x4E9B80..0x1F00000`), and the
heap init that immediately follows (`0x100230`: `memset(0x4E9B80, 0, 0x1A16480)`) zeroed them. Evidence chain:

```
[pc] enter 0x00131BE0 ra=0x0010024C sp=0x01FFFF80
[pc]   frame ... +30=00165C74          <- FUN_00100230 saved $ra at sp+0x30
[pc] exit  0x00101390 pc=0x00109144    <- memset preempted by a checkpoint (returns mid-loop)
[pc] enter 0x00100270 ... sp=0x01FFFF80
[pc]   frame ... +30=00165C74          <- resume: frame still intact, jr $ra -> 0x165C74  (correct)
```
versus the same resume when the frame was below `0x1F00000`: `+30=00000000` -> `pc=0` -> thread dormant.

### 3. Second defect, exposed by the first fix: the Timer-2 handler ran on the guest stack

`[tick] #1 sp=0x01FFFFF0` - the interrupt invocation got the *first* pool stack (`PS2_RAM_SIZE - 0x10`), because
`reserveGuestStackFromAsyncPool(0x1F00000)` refuses (`guestStackBase > floor` is false when both are `0x1F00000`),
so the pool stayed at the top of RDRAM, on top of the guest stack. Its frame zeroed `FUN_00100230`'s saved `$ra`
again. Fixed by splitting the top of RDRAM in `SetupThread`:

- `kInvocationStackReserve = 0x40000` (256 KiB = 16 x 16 KiB invocation stacks) taken off the top,
- guest `$sp = PS2_RAM_SIZE`, guest stack bottom = `0x1FC0000` (so 256 KiB of guest stack),
- `setupCurrentThread()` is called with that bottom, so the pool is `[0x1F00000, 0x1FC0000)`.

Verified: `[tick] #1 sp=0x01FBFFF0` (pool), guest `sp=0x02000000`, heap init frame survives.

### 4. What the guest does now (measured)

- Syscalls start flowing: `0x3c -> v0=0x2000000`, `0x3d -> v0=0x4e9b80`, `0x74` (SetSyscall) x ~30,
  `0x5b` (GetEntryAddress) probes. `FUN_0011e070` (the syscall-handler installer) now **returns**.
- The guest runs real init code: `0x165cb4 -> 0x11e7a0 -> 0x117aa8 -> 0x1174f8 -> 0x117cf8`.
- It then reaches `FUN_00117ca0` = **`sceSifInitCmd`** and stops in its wait loop
  (`0x117DF8: jal 0x1174f8; beq v0,zero,0x117DF8`, i.e. spin on guest flag `0x003DEF40`).

### 5. Current blocker: the SIF command channel has no IOP to answer it
`sceSifInitCmd` reads `sceSifGetReg(0x80000002)` (`kSifRegMsCom`, seeded to 0), sends a 16-byte SIF command packet
(`sceSifSetDma` -> `ps2_stubs::sceSifSetDma`, which copies EE->native IOP and fires DMAC cause 5) and waits for the
guest flag `0x3DEF40`. Nothing sets it: `g_sifCmdHandlers` in `SIF.cpp` is only stored, never invoked, and the
audio-only `ps2_native_iop` has no `sifcmd`. The richer `ps2xIOP` LLE emulator + plugin profiles exist but are not
wired up for this title (main.cpp only calls `ps2_native_iop::setModules` with DQ8's sound module list).

So the next workstream is the IOP/SIF command channel: either answer the `sceSifInitCmd` handshake natively, or wire
up the `IopSubsystem` with a ToL profile. This is *real* SIF traffic - the earlier "the guest does no SIF DMA"
finding is superseded.

### 6. Probes added this session (`src/runtime/tol_overrides.cpp`)

- `TOL_TRACE_TICK=1` - dumps the tick dispatcher's state block and active list (first 6 ticks, then every 64th).
- `TOL_TRACE_PC=0xADDR[,0xADDR...]` - wraps up to 16 guest functions and prints enter/exit with pc/ra/sp plus the
  eight words at `$sp`, which is what made the lost `$ra` visible.

### 7. Land mines reconfirmed

- `$s5` is not valid at entry to `FUN_0011F650`; the state address must come from the code.
- Preemption at a backward edge inside a *called* function unwinds the C++ stack; the resume is a fresh top-level
  dispatch of `ctx->pc`. That works only while the guest frame is intact - so anything that scribbles on the guest
  stack turns a preemption into a lost return address.
- The tick rate drops from ~16/s to ~4/s while the guest spins in the SIF wait loop, so a "slower" tick log is a
  symptom of the spin, not of timer emulation.

### 8. `sceSifInitCmd` unblocked; the blocker moved one layer down to `sceSifBindRpc`
`SIF.cpp::seedDefaultSifRegsLocked()` now seeds `kSifRegMsCom` (0x80000002) to **1** instead of 0. Rationale: a title
with its own statically linked libsif (ToL carries one - `sceSifInit` at 0x117540, `sceSifInitCmd` at 0x117CA0) polls
that register, sends a handshake command and then spins on guest flag `0x003DEF40`, which only its own DMA-completion
bookkeeping could set. The runtime's SIF DMA path completes synchronously and there is no IOP behind the register file,
so "the command channel is already up" is the honest native state. (The runtime's HLE `sceSifInitCmd` stub exists but
is only reached by games that *import* libsif, not by this one.)

Measured effect: the guest leaves the spin and walks further into libsif -

```
0x166298 -> 0x11c990 -> 0x118448 (sceSifBindRpc) -> 0x117e68 -> 0x1184fc -> sceSifSetDma -> FlushCache(0x114720)
```

and then stops: `FUN_00118448` is **`sceSifBindRpc`** - it creates a semaphore, sends bind command `0x80000009` with
`sceSifSendCmd` and `WaitSema()`s for the server's reply. `FUN_0011c990` retries that bind forever. Nothing in the
runtime answers it (`g_sifCmdHandlers` is never invoked), so the main thread blocks in the scheduler and the EE runs
only the Timer-2 handler: `ee-dispatches=44` per 300 frames, `transfers=0 draws=0`.

**Next step:** implement the IOP side of the RPC bind - either dispatch `g_sifCmdHandlers` when a command completes
(so the guest's own handler signals the semaphore), or answer `sceSifBindRpc` natively. Then watch for
`sceSifLoadModule` (0x71), the first `cdrom0:` open (`TOL_TRACE_FILEIO=1`) and `CreateThread` (0x20).

---

## Session 63 (cont.) — SIF RPC answered; the guest loads its IOP modules and reaches its first data files

### 9. Native bind/call: the guest's own libsif is replaced at its two blocking entry points

The game carries libsif inside the ELF: `sceSifBindRpc` at `0x00118448`, `sceSifCallRpc` at `0x00118628`. Both send a
SIF command and `WaitSema()` for a reply only an IOP would send. `src/runtime/tol_overrides.cpp` now installs the
runtime's HLE implementations (`ps2_syscalls::SifBindRpc` / `SifCallRpc`) in their table slots. Argument conventions
are the SDK's, so every caller above them - the game's loadfile and iopheap layers - is unchanged, and the calls
complete synchronously instead of waiting for a reply that cannot arrive. `TOL_TRACE_RPC=1` logs bind/call.

Side effect worth knowing: the guest's `sceSifBindRpc` also allocated its own client record and created the
semaphore; the HLE bind instead writes the client struct at the runtime's offsets and records
`g_rpc_clients[clientPtr].sid`. It happens that `client+0x24` - which the game reads as "bound" - is where the HLE
bind stores `server`, so the guest's `while (!bound) retry-bind` loop terminates on the first pass.

### 10. The load-module SIF command is served, and the first `cdrom0:` open happens

`sceSifLoadModule` (`FUN_0011dc40`) does not use RPC: it builds a packet `{size, dest, cid=0x80000003, ...}` with the
module path at `+0x18`, DMA's it to the IOP and then polls `sceSifGetReg(4)` for bit `0x40000`. `ps2_stubs::sceSifSetDma`
now recognises that command, opens the image through the **IOP-side** file layer
(`PS2IopTransport::readHostFile` -> `PS2IopHostAdapter::translateGuestPath`/`openHostFile`, the same choke point
`TOL_TRACE_FILEIO` reports on), loads it onto the native IOP when that IOP claims it, and sets the done bit on the
next boot-status read (after the EE rewrote that register, as the IOP's asynchronous completion does; the bit stays
set because the guest polls it again later).

The game prefixes the image with the ROM loader that consumes it - `rom0:UDNL cdrom0:\IRX\IOPRP300.IMG;1` (a
constant in its `.data`, never rewritten) - so the part after the first space is the file.

**First `cdrom0:` open, measured:**

```
[fileio] iop open '...\data\CD\IRX\IOPRP300.IMG' -> ok, 275921 bytes
[sif] load-module 'rom0:UDNL cdrom0:\IRX\IOPRP300.IMG;1' -> 'cdrom0:\IRX\IOPRP300.IMG;1' read (275921 bytes)
```

### 11. The loadfile RPC (sid 0x80000006) is served, and the nine IRX modules load

After the IOPRP, the guest binds `0x80000006` (loadfile) and drives its module loader over RPC. `tolSifCallRpc` now
answers that sid: `0xFF` returns the version string `"3000"` (the game compares it against `"3000"` and `"...."` and
only proceeds on a match - that is what `FUN_0011cfe0` checks), anything else is a module load whose request is
`{argLen, 0, path[0xFC], args[]}` at the send buffer, served by the runtime's own loader (`ps2_syscalls::SifLoadModule`
with the registers saved and restored).

Measured: all nine modules load and the sound library really starts on the native IOP.

```
SIO2MAN.IRX -> 1, SIO2D.IRX -> 2, MCMAN.IRX -> 3, MCSERV.IRX -> 4, DBCMAN.IRX -> 5,
PAD2\DS2U_D.IRX -> 6, LIBSD.IRX -> 7, KORGIVAG.IRX -> 8, IXAUDIO.IRX -> 9
[fileio] iop open '...\data\CD\IRX\LIBSD.IRX' -> ok, 28661 bytes
[ps2xIOP] [iop] started Sound_Device_Library at 65536
INFO: STREAM: Initialized successfully (48000 Hz, 16 bit, Stereo)
```

### 12. The game's file layer, now working

The guest runs the graphics bring-up (`FUN_001385d0`: VIF0/VIF1 reset, DMAC reset) and then its data-file
initialisation, which used to fail on its own file system. Both services are now implemented in
`src/runtime/tol_overrides.cpp`:

- **open** - sid `0x80000597`, fn 0, client `0x3DBA80` (`FUN_00101a60`): a 300-byte request with the path at
  `+0x24`, and a 4-byte reply that must be **1** on success (the handle goes into the request's first word;
  `FUN_001ae288` tests the reply against exactly 1).
- **read** - sid `0x80000595`, fn 0xE (state) and fn 1 (read), client `0x3863D0` (`FUN_001029a8`): a 24-byte
  request `{handle, blocks, buffer, info[3], desc, countOut}`. The bytes are written straight into the caller's
  buffer, the count-out word is filled, the busy flag `0x385234` is cleared, and the I/O semaphore `0x385228` -
  which the read path leaves held for its completion - is released.

Measured result:

```
[fileio] gamefile open '\\AFS\FLIST.DIR;1' -> ...\data\CD\AFS\FLIST.DIR ok, 95 bytes (handle 1)
[fileio] gamefile read handle 1 95/95 bytes at 0x0049A440
[fileio] gamefile open '\\AFS\SYS_REG.AFS;1' -> ... ok, 1830912 bytes   (and FIELD/SND/MAP/BATTLE/MOVIE)
[Deci2Call:kputs] DVCI: Total 6 files.
```

The path question is settled: `translatePs2Path` already maps `\AFS\X.AFS;1` to `cdRoot + AFS/X.AFS` with the ISO
version suffix stripped, so no per-file mapping is needed.

### 13. Next blocker: the sound library's DTX

With the file layer working, `SJX_Init` is what stops the guest:

```
[IOP/RPC trace:unhandled] sid=0x90000200 rpc=0x2 ... sendBytes=[00 00 00 00 80 6A 3B 00 00 09 00 00 80 08 00 00]
[Deci2Call:kputs] DTX_Create: can't create DTX of server
[Deci2Call:kputs] E0100302: SJX_Init can't create DTX
```

KORG and IxAudio now run on the native IOP (`[iop] started KORG IOP Sound Driver`, `[iop] started IxAudio`).
Traced with `TOL_TRACE_IOP_RPC=1` (in `IopSubsystem::handleRpc`), the rejection is **`native.server(sid, ...)`
returning false** - `NativeIop::call` is never reached, so `host.readGuest(...)` is not involved:

```
[iop-rpc] sid=0x90000200 fn=0x2 -> NO native server and no route (unhandled)
```

Only two servers are ever registered: `0x90000300` and `0x90000301` (KORG). (An earlier note here claimed
`2415919872 = 0x90000200`; that was a bad hex conversion - it is `0x90000300`.)

With `TOL_TRACE_IOP_THREADS=1`, LIBSD and IxAudio both finish their entries and create and start their worker threads
(ids 4,5 and 8,9,12), but neither registers `0x90000200`, and ~170 ms of extra IOP time does not change it - so the
provider's init never reaches `sceSifRegisterRpc`.

Tested and disproven: the `ioman` file services. They are now implemented (`EeLink::hostFiles()` -> `IopHost`, so an
IOP module opening `cdrom0:\...` resolves like the EE side), and the `no kernel service for ioman` warnings are gone,
but LIBSD never calls them (`TOL_TRACE_IOP_FILEIO=1` logs nothing).

Found and fixed on the way: `TOL_TRACE_IOP_IMPORTS=1` showed IxAudio polling `vblank #5` in a tight loop from
`0x4826C`, where a constant 0 meant it never advanced. `vblank #5` now returns a 60 Hz counter off the IOP clock and
the loop is gone (no unbound-import calls at all any more). `sysclib #19` is imported but never called.

Still open: which module registers `0x90000200`. Next step is to disassemble `IXAUDIO.IRX` / `KORGIVAG.IRX` (both
contain the constant) around their `sceSifRegisterRpc` call.

Note: `libdbc: Module version mismatch [libdbc.a = 3.10, dbcman.irx = 3.20]` is the game's own warning and is not
fatal - execution continues past it.

---

## Session 63/64 summary (2026-11-02) — from "black screen" to "NOW LOADING" rendering

**Read `docs/session-64-handover.md` first; it is the current state.** Short version:

- Boot now runs: crt0 -> libc init -> main -> game init -> IOPRP boot image -> 9 IRX modules -> DVCI indexes all six
  AFS archives -> audio init completes -> ioman init -> the loading sequence's file I/O works.
- **The port renders**: 201 draws / 81 GIF packets, the game's own "NOW LOADING" screen (512x448), captured with
  `TOL_GFX_SCREENSHOT_DIR`.
- **Current blocker: threads.** The guest never calls `CreateThread` (0x20); the port runs one EE thread. PCSX2 (same
  game) runs **eight** after loading, so the game's asynchronous loader (`FUN_00196058` request /
  `FUN_00196460` status poll inside `FUN_001664d0`) can never report state 3. That is why the loading screen does not
  advance even though every file operation under it succeeds.
- Biggest single fix: `InitMainThread` (syscall 0x3C) returned the *bottom* of the stack area, putting the main
  thread's frames inside its own heap; the next `memset` zeroed a saved `$ra` and the thread died. It now returns the
  end of RDRAM and reserves the top 256 KiB for invocation stacks.
- New probes worth knowing: `TOL_TRACE_SPIN` (finds a within-function spin by sampling the guest pc at backward
  branches), `TOL_TRACE_RPC`, `TOL_TRACE_FILEIO`, `TOL_TRACE_IOP_RPC/THREADS/IMPORTS/SEMA`.
- PCSX2 MCP is connected and is the reference: `pcsx2_get_threads`, `pcsx2_disassemble`, `pcsx2_read_registers`,
  `pcsx2_set_breakpoint`.

---

## Session 65 (2026-11-03) — the loading stall is `ADXF_GetPtStat` failing the AFS header check

**Read `docs/session-65-handover.md`; it is the current state.** Short version:

- **Resolved to a single wrong input**: the CRI file device holds the file size in **2048-byte sectors** at
  `handle+0x08` (position at `+0x0C`); `seek(END)`+`tell` feed `FUN_0019f298`, which stores the count into the ADXF
  stream's `stream+0x14` and the byte window into `stream+0x10 = count << 11`. That count was 0, so `FUN_00196db0`
  saw "0 sectors remaining" and marked the partition DONE **without issuing a read**.
- **Supplying it works**: with `TOL_DVCI_SIZE_SHIM=1` (wraps the device seek `FUN_001AD780`) the port really loads —
  `[fileio] gamefile read handle 8 2048/2048 bytes at 0x00391140`, the 430-entry `SYS_REG.AFS` extent table starts
  parsing (DVCI progress 0 → 255, one block's worth of 8-byte entries), the poll returns 2 instead of garbage and
  returns *normally* to the loading loop, and `E0040701: Illigal format(not AFS)` disappears.
- **Remaining gap**: the *second* block read never completes. Block 0 completed only because `FUN_00196db0`'s
  blocking wait (`FUN_0019eec8` → `FUN_00197940`) pumped it inline; later reads have no wait and depend on the SJRBF
  pump `FUN_001b0d00` walking the device table at `0x003A7FB0`, which is all zero because `FUN_001b0870` (the only
  slot registrar) is never called for this stream.
- Device chain verified at runtime: `reader 0x39EE48` → file object `0x00499DC0` → device `0x003A6A98`
  (open `0x1AD5A0`, seek `0x1AD780`, tell `0x1AD818`, read `0x1AD850`), handle `0x003A75F8`. All slots populated.
- **Threads are not the axis — they already work.** `CreateThread` is a TOML stub (`CreateThread_0x1144e0.cpp`), so it
  is a direct C++ call, not a syscall (an earlier "zero thread syscalls" reading in this session was measuring the
  wrong thing). The middleware's own init (`FUN_001997a8` → `FUN_00199428`) creates a worker thread (entry
  `FUN_00198d50`, ends in `ExitDeleteThread()`); it runs and pumps the CRI slots 17× in 8 s. **No `CreateThread` or
  `EeScheduler` extension is needed**, and the threaded pump registration is correct for this title.
- **With the size shim the read chain works**: stream state 1 → 2 → 3, `+24=0x00391140` (buffer armed),
  `+28=0x800`, zero errors. **The remaining blocker is DVCI request retirement**: the stream reaches 3 but the DVCI
  handle stays at state 2 and `handle[+0x14]` (consumed) never advances, so the poll keeps returning 2, the 430-entry
  parse freezes at 255 (one block) and the stream re-reads block 1 forever.
- Land mine: a probe that does `Load32` on an unaligned guest address raises a real guest address-error exception and
  vectors the guest to the kernel at `0x80000080` (not recompiled), which looks exactly like a game bug. Use `Load8`.

### Active Runner Command

```
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Run it from the repo root (the runtime derives `cdRoot` from the ELF's own directory, so the ELF must be the one under
`data\CD`). `tools\run_game_agent.bat <seconds> <logname>` wraps that with a timeout and split stdout/stderr logs.

---

## Session 66 (2026-11-04) — the loading stall was the file server's address model

**Read `docs/session-66-handover.md`; it is the current state.** Short version:

- **Root cause**: the game's own file server (sid `0x80000597` open / `0x80000595` read) is **sector-addressed**.
  `open` replies `{base, sizeInBytes}` inside the 300-byte request buffer (`+0x00` base, `+0x04` size), and a read
  names an **absolute sector**: `FUN_001AD850` computes `entry[+0x20] + entry[+0x0C]` (base + position) and
  `FUN_001ab4f8` passes that to `FUN_001029a8`. Our old consecutive handles `1,2,3…` made `base + pos` walk into
  the *next file's* handle, so block 1 of an archive was served from another archive and block 3 named a handle
  that did not exist -> `FUN_001ab4f8` state 9 -> `FUN_001ad220` device state 3 -> `FUN_0019EF58` discard branch ->
  the ADXF stream never completes -> the loading poll never returns 3.
- **Fixed** in `src/runtime/tol_overrides.cpp`: `openGameFile` hands out `base = (slot + 1) * 0x100000` (stride
  larger than MOVIE.AFS's 436,462 sectors) and the RPC writes the byte size at `sendBuffer + 0x04`; `gameFileRead`
  resolves the absolute sector to `(slot, sector - base)`.
- **Measured**: all six archives `found`, `DVCI: Total 6 files.`, extent parsing to **6143** entries, loading loop
  advanced from index 0 to **2** (3 with `TOL_CRI_WINDOW_FIX=1`), **zero** `E0xxxxxx` error prints, no unknown
  handle. Screen still `NOW LOADING`.
- `TOL_DVCI_SIZE_SHIM` is obsolete (real sizes now arrive through the open reply).
- **Remaining blocker**: the DVCI handle keeps consuming but the poll sees stream state 2 while the credit tick
  sees 3; the ADXF stream falls back to idle (`state 1`, `+34=0x800`, `+5C=1`). Same family as session 65's
  synchronous-completion race, now with correct data.
- New tool: `tools/disasm_guest.py` (capstone MIPS64) for the code Ghidra skips - the CRI device vtable at
  `0x003A6A98` points at `0x1AD5A0` (open) which is not in Ghidra's function list.

### Session 66, second pass — the loading loop was never pumped (fixed)

- **Traced `FUN_00197668` against the ADXF stream at poll time.** It is just `return handle[+1]`; at the freeze
  handle=2, stream=2, `sDone=0`, device idle. The decisive field was the new `steps=` counter: the stream step
  `FUN_0019F298` had been called **310 times and then never again** across 268k polls, with the ABXR gate
  `DAT_0039E838 = 0` (free). So the poll was not racing anything - nobody was pumping.
- **Cause**: `FUN_001982a0` registers slot 5 = `FUN_00198210` (SJRBF only) when a worker thread exists, and
  puts the CRI server pump `FUN_00198280` on slot 4 (the worker). `FUN_001664d0` busy-waits on
  `FUN_0019a080()` without yielding, so when the worker stops, the CRI streams stop with it.
- **Fix**: wrap the pump registrar `FUN_001b7378`; slot 5 registered with the SJRBF-only `0x00198210` gets
  `0x00198230`, the library's own full main-thread pump (`0x198260 + CRI server + SJRBF`). The worker keeps
  slot 4 and `FUN_00197940` is re-entrancy guarded.
- **Measured**: all six archives load in **74 polls** (was 268k+ and stuck), tick ends with no active handle,
  guest advances to its next phase; still 0 `E0xxxxxx`, 0 missing functions.
- **Next wall**: KORG `sid 0x90000300 fn 0x0A` (client `FUN_002158a8`, 4-byte in/out buffer at `0x4B3800`)
  never completes on the IOP, so every call burns the 2 s deadline. Note `2415919872 = 0x90000300`, not
  `0x90000200` - session 65's "IxAudio" conclusion was that hex mistake, which is why the IOP-side IxAudio
  short-circuit never matched these calls.
- **`TOL_DVCI_SIZE_SHIM` deleted** (the open reply carries the size now). **`TOL_CRI_WINDOW_FIX` also deleted**:
  with the pump fixed all six loads complete either way (74 polls), and the shim only overwrote the correct
  stream total (1 instead of 436,462 sectors).
- **Audio wall narrowed**: KORG `0x90000300 fn 0x0A` is `FUN_002158a8(0x33)` from `FUN_001eeda0` (sound init).
  In `KORGIVAG.IRX` it registers handler `0xB5B0` (jump table at `0xDCCC`, fn `0x0A` -> `0xA070` ->
  `0x6960`, the SPU2/voice config). The call has `hasServer=1` but the handler shows no import activity, so the
  LLE IOP queues it and never invokes it - next thing to check is `sceSifRegisterRpc`'s stored record and who
  calls it.

---

## Session 68 (2026-11-04) - the LLE cdvdman handlers, and why the IOP modules never ran

**Result: the CDVD drive error is gone (`SRD: Drive Error` 245 -> 0). The LLE now serves `sceCdRead` from the
detected `CDVD.000`; `[cdvd] read lsn=16 ... pvd=014344303031` is the verified ISO9660 PVD.**

### 1. Two foundational LLE defects had to be fixed first

- **IOPRP module slices dropped every relocation table.** `PS2IopHostAdapter` sized each module from its
  `PT_LOAD` segment only, so `IRX` section headers and `SHT_REL` tables (which sit after the load image) were
  cut off. `parseIrx` then saw no relocations, loaded every module unrelocated, and each module's init thread
  jumped to a module offset instead of `base + offset` (the `pc=0x0001000C word=0x0001001C` fault on every
  module). Fix in `ps2xRuntime/src/lib/ps2_iop_host.cpp`: extend the slice through `e_shoff + e_shnum * e_shentsize`
  and every section's `sh_offset + sh_size`. cdvdfsv/`cdvd_ee_driver` now initialise and register all six RPC
  servers (sids `0x80000592/59a/593/59c/597/595`).
- **`registerLibraryEntries` registered zero functions.** The real export table starts with reserved zero
  slots (ordinals 0..3), followed by the function list and a zero terminator. The handler stopped at the first
  zero, so no module exports ever linked and every import fell back to the kernel. It now keeps leading zeros as
  reserved slots and stops at the first zero *after* a real function.

### 2. The LLE cdvdman device

`ps2xIOP/src/lle/kernel.cpp` + `kernel_handlers.inc` now implement the real cdvdman exports (ordinals verified
against the `CDVDMAN` module in `IOPRP300.IMG` and the CDVDMAN interface docs):

| ordinals | behaviour |
|---|---|
| `#4` init, `#5` standby, `#7` seek, `#15` stop, `#38` pause | success, clear the error |
| `#6` `sceCdRead`, `#114` `_sceCdRE` | 2048-byte sectors from `IoPaths::cdImage`, into IOP RAM; partial/past-EOF reads zero-fill and still succeed |
| `#8` `sceCdGetError` | last error (0 or `0x30`) |
| `#9` `sceCdGetToc` | zero the 1024-byte buffer |
| `#10` `sceCdSearchFile`, `#84` `sceCdLayerSearchFile` | live ISO9660 walk of the image (PVD at LSN 16) |
| `#11` sync, `#13` disk ready, `#28` status, `#12` disk type | completed / `0x14` PS2 DVD |
| `#14` tray, `#16/#17` pos conversions, `#22` iLink id, `#24/#37` callback, `#39` break, `#50` `_sceCdSC`, `#75` mmode, `#83` dual info | synchronous equivalents |

`linkImports` resolves `cdvdman` to the kernel deliberately (the real module drives Mechacon registers the LLE
does not emulate) and also resolves `ioman` to the kernel so IOP `cdrom0:` opens map to the extracted disc tree.
`tol_overrides.cpp` no longer answers sid `0x80000593` natively: that sid is the game's CDVD service, and the
native reply was masking cdvdfsv (session 67 had labelled it "pad").

### 3. Verification

```
TOL_CDVD_SELFTEST=1 TOL_TRACE_CDVD=1 build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
[cdvd] image '...\data\CD\CDVD.000' -> 1073676288 bytes
[cdvd] read lsn=16 sectors=1 dest=0x00010000 image=1073676288 bytes ok=1 reason=image
[cdvd] selftest read lsn=16 sectors=1 -> ret=1 pvd=014344303031     # \x01CD001
[cdvd] search '\SYSTEM.CNF;1' -> lsn=690 size=57
SRD: Drive Error = 0,  "no kernel service for cdvdman" = 0
```

`TOL_CDVD_SELFTEST=1` is the new env switch; it runs the two handlers on the first module load. The guest has
not issued a raw read in a normal run because the AFS path still goes through the EE gamefile RPC (sids
`0x80000597/595`, served in `tol_overrides.cpp`); the handler itself is proven by the selftest.

### 4. Remaining wall (not CDVD)

The screen still stops at `NOW LOADING`. With CDVD fixed, the visible activity is the KORG RPC (`sid
0x90000300 fn 0x0A`) repeating every 2-9 ms; round trips now complete (they burned the 2 s deadline before the
relocation fix). That audio/stream wall is the next session's work; it is no longer a disc error.

### Session 68, continuation - async SIF RPC callbacks must be deferred

`SifCallRpc` invoked the caller's `endFunction` inline (`eeScheduler().invokeCurrent(RpcCallback)`) before returning
to the guest. For the KORG sound sequence that turned `FUN_002158A8`'s completion semaphore into a busy spin:
`WaitSema` at entry always found the semaphore already signalled, so the main thread re-sent `sid 0x90000300 fn
0x0A` ~485 times/s and starved the sound thread (`tid=6`, Ready at `0x218908`).

Fix: in `ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp`, queue the RPC end function with
`eeScheduler().queueInvocation(...)` instead of invoking it inline. The scheduler runs it once the caller has
actually yielded (the caller blocks in `WaitSema`), which is the hardware order.

Measured effect (15 s run):

| metric | before | after |
|---|---|---|
| EE turns/s | 485 (99% at `sceSifCallRpc`) | 189, mean 0.36 ms |
| screenshots | 1 (frame 12) | frame 12, 72, ... 492 (~33 fps) |
| `SRD: Drive Error` | 0 | 0 |
| AFS/DVCI | 6 archives found | 6 archives found |

The main thread then goes dormant at `0x139690` and the next blocker appears:
`E0100401: can't create PS2RNA of IOP`, repeating. `FUN_001b3a20` asks IxAudio
(`sid 0x90000200`, client `0x3A66F0`) via `FUN_001acfb0(8, ...)` and gets 0, so CRI's PS2RNA
creation fails. That is the next workstream; it is not an RPC-timing problem (`FUN_001acfb0` passes
`endFunction = 0`, so it is unaffected by the deferral).

### Session 68, continuation 2 - E0100401 (CRI PS2RNA) cleared

`FUN_001b3a20` creates the CRI PS2RNA object: it allocates IOP heap blocks, then asks IxAudio
(`sid 0x90000200`, client `0x3A66F0`) with `FUN_001acfb0(8, ...)` -> function **0x408** and uses the
reply as the object handle. A zero reply prints `E0100401: can't create PS2RNA of IOP`.

The IxAudio native handler only returned handles for `{0x002, 0x400, 0x422}`, so 0x408 got 0. Routing
IxAudio to the real IOP server now that it registers was tried: it clears E0100401, but the real
`IXAUDIO.IRX` DTX create (fn 0x002) also returns 0 in this environment, which brings back
`E0100302: SJX_Init can't create DTX`. The working form is the native handle reply for both lifecycle
calls: `{0x002, 0x400, 0x408, 0x422}` all return a fresh nonzero handle.

Verification (20 s):

```text
E0100401 = 0, E0100302 = 0, SRD: Drive Error = 0
screenshots through frame 792 (~40 fps)
```

### Next stall

The loading screen still stops at `NOW LOADING`. The main EE thread ends up **Dormant** at
`0x139690` (the return of GS setup `FUN_00139620`, caller `FUN_0027A1F0`), while the CRI worker
threads (tid 3/4, entries `0x198D50`/`0x198E58`) sit in their sleep loops at `0x198E14`/`0x198EF8`
and tid 5 is suspended. No further AFS reads are issued. So the next workstream is the CRI/DVCI
loading pump: find which thread is supposed to drive the next read after the DVCI table and why the
main thread is no longer in the `FUN_001664d0` poll loop.

### Session 68, continuation 3 - the CRI/DVCI pump is working; the main loop exits

`FUN_001664d0` loads the six AFS archives sequentially:

```c
for (i = 0; i < 6; ++i) {
    FUN_00196058(i, name, 0, buf);          // start
    while (FUN_00196460(i) != 3)           // poll
        FUN_0019a080();                     // pump (slot 5 -> FUN_00198230)
}
```

`TOL_TRACE_DVCI=1` shows it running to the end: poll #73/#74 are `i=5` (`MOVIE.AFS`, total
436462 sectors), and #74 reports `state=3` with `sDone=2048 sBlocks=1` (`sDevBase=0x700000`,
`devState=1`). The earlier loads processed their extent blocks (`i=0..4`, progress 255..3327).
So the pump/registrar fix from session 66 is still correct; the six extent loads complete.

The stall is immediately after: the main loop `FUN_00165AD0` (an infinite `do { ... } while(true)`)
is entered once with `ra=0x00100210 sp=0x02000000`, and later the main thread is **Dormant** at
`0x00139690` (a `jr $ra`, `ra=0x0027A210`, inside GS setup `FUN_00139620` called from
`FUN_0027A1F0`). `FUN_00165AD0` should never return, so this is a lost/changed control flow in the
EE scheduler or a preemption resume, not the CRI pump. The `[pc]` probe also shows `FUN_00165AD0`
"exiting" to `0x00109144` (interior of `memset`), another sign the return path is being diverted.

Next workstream: trace the preemption at the `jr $ra` in the render path (`0x139690` ->
`0x27A210`) and why `FUN_00165AD0`'s loop ends. The CRI/DVCI pump itself needs no change.

### Session 68, continuation 4 - trace toolbox results (pump complete, game loop alive)

Trace evidence gathered with the existing probes:

| trace | what it showed |
|---|---|
| `TOL_TRACE_DVCI=1` | six AFS extent loads complete: `#73/#74 i=5` (`MOVIE.AFS`, 436462 sectors) reaches `state=3`, `sDone=2048 sBlocks=1`; `i=0..4` advanced progress 255..3327 |
| `TOL_TRACE_PARK=1` | 649 parks, almost all `invocations=1` with `ra=0` and `sp=0x1fbfff0` - normal RPC-invocation returns, not a dead main thread |
| `TOL_TRACE_PC=0x165AD0` | main loop entered once (`ra=0x100210 sp=0x02000000`); its "exit" to `0x109144` is a call target, not a return |
| `TOL_TRACE_EE_DISPATCH=1` | 1386 turns/s on tid=1; buckets `0x165E48`, `0x166794`, `0x1AB5CC` (13%), `0x1A152C`, `0x1979B0` - the frame/CRI loop is alive |
| `TOL_TRACE_PC=0x165E70/0x166010/0x166120` | no hits, but these addresses are folded into larger map rows (`0x165E48` is the real dispatch entry), so the probe is a false negative |

Conclusion: the CRI/DVCI pump is not the blocker and the EE main loop is running at full rate. The game simply never leaves the
`NOW LOADING` state. The next trace target is the loading-state transition itself: `FUN_00166120` (event queue: codes 0x25,
0x24, 0x23, 0x21, 0) and `FUN_00166010`'s callback wait at `0x1660A0`, plus the CRI/ADXF stream that should read the actual
game data after the six extent tables. Use `TOL_TRACE_EE_DISPATCH` buckets (not single-address probes) for these addresses.

### Session 68, continuation 5 - 0x1660A0 / FUN_00166120 are downstream of a blocked CRI update

30 s trace with `TOL_TRACE_PC=0x1660A0,0x166120,0x166010,0x165E70`:

- `0x1660A0` (callback), `0x166120` (event queue) and `0x166010` (waiter): **0 hits**.
- `0x165E70` (frame update): 1 hit, `ra=0x165B14`, exits to `0x19BD20` and never returns.

`0x19BD20` is interior to `FUN_001978f0` (the CRI/ADXF library, 32 KB). The main loop is therefore stuck in
the CRI/ADXF update on its first frame; `FUN_00166010`'s wait on the `0x1660A0` GS/VU1 callback and
`FUN_00166120`'s event queue are never reached.

Causal chain: main loop -> `FUN_00165E70` -> CRI/ADXF (`0x19BD20`, also the `0x1AB5CC` 13% and
`0x1A152C` buckets) -> never returns -> `FUN_00166010`/`0x1660A0` never run -> no state transition out of
`NOW LOADING`.

Next trace target: the containing CRI function `FUN_001978f0` (`0x1978f0`) and the ADXF stream state at
`0x19BD20`; single-address probes on interior addresses are unreliable, so wrap the containing row start.

### Session 68, continuation 6 - the frame update is not stuck; the CRI data load is throughput-limited

The `0x19BD20` target was a false lead: it is a tiny setter (`jr ra; sw a1,0x9c(a0)`), and the
`TOL_TRACE_PC` "exit" only records the first call handoff, not the function's later control flow.

Measured the CRI read path instead:

```text
[fileio] gamefile read ... 2048/2048 bytes   (one block per RPC)
reads=69 in 8 s, bytes=139359  ->  8.6 reads/s, ~17 KB/s
[cri] read handle=0x003A75F8 blocks=1 buf=0x00391140 state=0->2 ret=1 | pos=0/894 ...
```

Each ADXF stream read submits **1 sector** and takes ~116 ms to complete; the CRI pump is called
~68 times/s (`0x166794` bucket), so each block needs ~8 pump cycles. The six extent tables complete,
but the remaining data load is orders of magnitude slower than the real DVD (a 2 KB sector is ~1 ms
on hardware). `TOL_CRI_IO_LATENCY=1` (the one-poll deferral shim) did **not** change the rate
(69 reads / 17 KB/s), so the limiter is the per-block handshake/stream window, not status timing.

Next target: the ADXF stream read window. The stream carries `+28=2048` bytes of buffer and
`+2C=512` (sector size); `FUN_00196058`'s request size is turned into blocks by `param_8 >> 11`.
The read is always `blocks=1`, so raising the request/window (or making the stream issue a multi-block
read) is the way to lift 17 KB/s. Also check the `stream[0x49]` completion flag: the step reads only
when `state==2 && +49==1`, and the per-block handshake is what costs the eight pump cycles.

### Session 68, continuation 7 - CRI read block count traced to the device tell/seek

`FUN_0019EF58` computes the read block count as:

```c
iVar2 = tell(handle);                                  // position in sectors
seek(handle, 0, stream[0x18], &uStack40);
iVar6 = stream[0x30] - iVar2;
iVar5 = stream[0x14] - iVar2;
if ((tellBytes >> 11) < iVar6) iVar6 = tellBytes >> 11;
if (iVar6 < iVar5) iVar5 = iVar6;
iVar4 = stream[0x2c];                                  // 0x200 (512)
if (iVar5 < stream[0x2c]) iVar4 = iVar5;
read(handle, iVar4, buffer);
```

Device slots:

- `FUN_001AD780` (seek) returns the clamped **position in sectors** (894 for SEEK_END, 0/1/2 for seeks).
- `FUN_001AD818` (tell) returns `handle+0x0C`, the position in sectors.

The room is `stream[0x14]=894` sectors and the max is `stream[0x2c]=512`, yet the read is
`blocks=1`. The limiting value is the tell/seek output treated as bytes (`>> 11`), i.e. one
2048-byte buffer. The stream's buffer/window (`+0x18=0x800`, `+0x28=0x800`) is one sector.

The per-read cost is also the issue: the gamefile read wrapper `caseD_0` (`0x1AB5C8`) runs ~68 times/s
at ~5.9 ms each (`0x1AB5CC` bucket 402 ms/s = 13%). A 2 KB host read is microseconds, so the
5.9 ms is the RPC/state-machine round trip, not the file I/O.

Next: measure `gameFileRead` host time vs RPC overhead, and/or raise the ADXF stream buffer so the
read step issues `blocks > 1`. The `0x1660A0` callback remains downstream; it is not the cause.

### Session 68, continuation 8 - gameFileRead measured: host I/O is ~10 us, not the bottleneck

Added `TOL_TRACE_FILEIO_TIME=1` to `gameFileRead` and replaced the per-byte `Store8` copy with a
bulk `memcpy` into EE RAM (plus a reused `thread_local` buffer instead of a fresh vector per 2 KiB
block).

Measured (8 s):

```text
[fileio-time] sector=0x200000 bytes=2048 read=20.6us copy=0.2us total=22.3us
[fileio-time] sector=0x500001 bytes=2048 read=2.4us  copy=0.1us total=2.6us
reads=69, bytes=139359, ~17 KB/s   (unchanged)
```

So the entire HLE read handler is 2-39 us. The ~5.9 ms per CRI read is in the **caller/state
machine**, not the file I/O. Optimizing the host read cannot raise the throughput; the limiter is the
CRI/ADXF handshake (~8 pump steps per 2 KiB block) and/or the main-thread frame scheduling.

The bulk copy is kept (it removes 2048 `Store8` calls per block); the timing switch is diagnostic.
Next target: the CRI read caller `caseD_0` (`0x1AB5C8`) and the ~8-step handshake between
`state=2` and `state=3`, or raising the ADXF stream buffer so a read covers several sectors at once.

### Session 68, continuation 9 - multi-block window experiment (negative result)

Tried raising the ADXF stream window to 8 blocks (`TOL_CRI_RAISE_WINDOW=1`): patched the stream's
`+0x24` buffer to a 16 KiB `guestMalloc`, and `+0x18`/`+0x28` to `0x4000`. Measured result:

```text
[cri] window raised: buffer 0x00000000 -> 0x01EF0000, size 0x4000
[cri] read handle=0x003A75F8 blocks=1 buf=0x00391140 state=0->2 ...
reads=69, bytes=139359, ~17 KB/s   (unchanged)
```

The read still used `buf=0x00391140` and `blocks=1`; the patched stream's `+0x24` was `0`, so this is
not the buffer the DVCI read path uses. The actual 2 KiB buffer is the DVCI read buffer `0x391140`, and
the block count is pinned by the seek/tell output, not the stream window. The experiment was reverted.

Kept from the measurement pass: the bulk `memcpy` copy and `TOL_TRACE_FILEIO_TIME`. Next target is the
CRI handshake/scheduler latency (the `caseD_0` 5.9 ms wall time with a 10 us handler), or patching the
actual DVCI request (`blocks`) rather than the stream window.

### Session 68, continuation 10 - CRI caller traced; completion poll is not blocking

Traced `FUN_001029A8` (game read client) and the CRI state machine `FUN_001AB4F8`:

- `FUN_001029A8` calls `FUN_00101d80(4)` (PollSema on the I/O sema, then `FUN_00101F70(1)` busy check) and
  issues the mode=1 read RPC.
- `FUN_00118828` returned **0** in every sampled call, `FUN_00101F70` returned 0, and the
  `FUN_00115758` DelayThread wrapper was **never entered**. So the completion poll is not sleeping.
- Added `clearGuestRpcBusy` to clear `client[0]->header[0x10]` bit 0 after the game-data RPC (the guest's
  own libsif busy bit, which the native override previously left set). Re-measured: still 69 reads / 8 s,
  ~17 KB/s, so it was not the limiter (but it is the correct completion state).

The 5.9 ms `caseD_0` bucket is therefore **wall time/preemption**, not a blocking call inside the handler:
the read handler is 2-39 us and the completion checks return immediately. The rate is set by the CRI
state machine's step count per 2 KiB block and the main-thread scheduling, not by one wait.

Next: measure CPU time vs wall time for `caseD_0`/`FUN_001AB4F8`, or count `FUN_0019F298` steps per
completed block to see whether the state machine itself is the 8-step cost.

### Session 68, continuation 11 - CRI step CPU/wall + steps per read

Added `[adxf-time]` (inside the ADXF step probe) using `GetThreadTimes` + `steady_clock`:

```text
[adxf-time] steps=100  wall=11.5ms cpu=0.0ms  cpu/wall=0.000 reads=22 steps/read=4.55
[adxf-time] steps=300  wall=30.6ms cpu=78.1ms cpu/wall=2.556 reads=68 steps/read=4.41
[adxf-time] steps=400  wall=33.8ms cpu=78.1ms cpu/wall=2.311 reads=69 steps/read=5.80
[adxf-time] steps=1500 wall=58.2ms cpu=78.1ms cpu/wall=1.343 reads=69 steps/read=21.74
```

Interpretation:

- During the extent-table loads it is **~4.4 ADXF steps per completed 2 KiB read**.
- After 69 reads (the six extent tables) the step pump keeps running to 1500+ steps, but
  `gGameFileReadCount` stops at 69: **no further reads are requested**.
- `cpu/wall > 1` is cooperative-fiber preemption: the step yields, the same OS thread runs other
  guest turns, and `GetThreadTimes` counts their CPU too. The step's own CPU cost is small; it is not
  CPU-bound.

So the CRI pump is not the throughput limiter after the extent tables: the game simply stops issuing
data reads. The next blocker is again the loading-state transition (`FUN_00166010` -> `0x1660A0`),
which is why the data-load phase is never entered.

### Session 69 - loading-state transition unblocked: missing function-table slot 0x139690

The frame update (`FUN_00165E70`) was previously entered once and never returned. TOL_TRACE_PC on
its callees showed it stopped inside `FUN_001EDC20` -> sound init -> `FUN_0027A140` -> `FUN_0027a1f0`,
which ends with `j 0x139690`. `0x139690` is a shared clamp-setter epilogue
(`jr ra; sw a0, -0x7b44(gp)`). Ghidra folded it into its callers, so the dense function table has
null slots at indices 0x139684..0x13969c (`0x139690` included). The DirectJump therefore reported a
missing target (silently - only the first missing target is logged), and the runtime spun forever on
the missing-PC stub. That was the real reason the first frame never finished.

Fix (src/runtime/tol_overrides.cpp): re-express the two instructions natively with
`replaceFunction(0x139690, ...)`. With that, the frame update completes its whole callee list and the
main loop reaches `FUN_00166010` and the callback `0x1660A0` for the first time.

Second fix in the same file: `0x1F8150` is the GS/VU1 worker loop
`do { FUN_001f8000(); FUN_0015231c(); } while (true)`. On hardware it is a separate thread; run
inline it never returns to `FUN_00166010`. The override replaces every slot in
`[0x1F8150,0x1F8170)` (the loop resumes at interior 0x1F8168) and runs one iteration per dispatch,
returning to `$ra`.

New blocker (GS/VIF1 transfer bookkeeping): `FUN_00166010` waits on a stack flag (`iStack_4` at
`sp+0x2C`) that `0x1660A0` sets only after its `while (FUN_001f8480(handle) != 0)` loop exits. The
transfer record lives in the guest table at `0x4A72C0` (0x2c-byte records); `FUN_001f82f0` submits a
packet via `FUN_001f8be0`, and `FUN_001f8dd0` reads `record[2]` as the status. With TOL_TRACE_GS the
flag pointer is correct (`0x01FFFFCC`) but stays 0: the record is never marked complete. The callback
runs, spins on the busy status, and the wait loop never sees the flag. Next: make the VIF1/GS packet
submission (`FUN_001f8be0`/the 0x4A72C0 records) complete, or emulate the VIF1/GS completion so the
record status goes to 0.

Diagnostics added: `TOL_TRACE_GS` logs worker steps and the flag pointer/value. `tools/find_missing_targets.py`
scans the corpus for direct dispatch targets absent from the function table (found 0x139690; most of
the other 500 are out-of-range mis-disassembled data in `entry_003a3198` and friends).

### Session 70 - memory-card prompt is BIOS-only; HLE mcserv version fixed

Asked about the "Checking memory card" screen shown before the logos. Findings:

- The prompt text ("Checking memory card (8MB)", "Do not remove", "MEMORY CARD") is **nowhere on
  the disc** (scanned SLUS_212.01, all IRX, and the full 1 GB CDVD.000, ASCII and UTF-16). It is the
  **BIOS** memory-card manager screen, so a direct-ELF port never runs it. The game's own strings are
  only `libmc: too old release of mcserv.irx` / `mcman.irx` and `SRV_CMEMCARD::*` (in-game save UI).
- The port does render: at 3 s the window is black, at 6 s the game's own `NOW LOADING` is drawn
  (text comes from a game asset, not the runtime). It then stays frozen there because
  `FUN_00166010` still waits on the GS/VIF1 transfer record at `0x4A72C0`.
- Real bug found in `ps2xIOP/src/modules/mcserv.cpp`: the HLE reported `kMcservVersion = 0x0205`
  and `kMcmanVersion = 0x0206`. The game's libmc `FUN_001257f8` requires `mcserv >= 0x20a` and
  `mcman >= 0x20e` (disassembly: `slti v0, mcman, 0x20e; beq v0, zero, success`), so it aborted
  with "too old release of mcserv.irx" and skipped the memory-card path. Changed to
  `0x020A`/`0x020E`; "too old release" is gone and `[mc] Init -> 0` succeeds.
- New warning after the fix: `libdbc: Module version mismatch [libdbc.a = 3.10, dbcman.irx = 3.20]`
  (controller/DB manager). Non-fatal so far but the next version-check to look at.
- The memory-card prompt itself is BIOS; showing it would mean emulating the BIOS memory-card
  manager (or drawing a native prompt), not fixing the game. The game still needs the GS/VIF1
  transfer to complete to leave `NOW LOADING`.

### Session 71 - GS/VIF1 loading gate fixed

The `FUN_00166010` wait was finally traced to its real status source:

- `FUN_00166010` waits on `iStack_4` (sp+0x2C). `0x1660A0` sets it after
  `while (FUN_001f8480(handle) != 0)`. `FUN_001f8480` reads `record[2]` from the VIF1 transfer
  table at `0x4A72C0` (0x2C-byte records). `FUN_001f82f0` builds a packet into it; `FUN_001f8be0`
  queues it. The runtime never retires the record (VIF1 DMA packet not executed to completion), so
  the status stays 2 and the callback spins.
- The callback dispatches to **thunk `0x1F7310`**, whose generated body calls
  `FUN_001f8480_0x1f8480` **directly**, bypassing the function table. Replacing only `0x1F8480` had
  no effect; replacing **`0x1F7310`** (and `0x1F8480`) with a "status = 0" stub makes the callback
  set the flag. With that, `FUN_00166010` returns and the main loop reaches `FUN_00166120` for the
  first time; the load/render loop (`FUN_0019a080` CRI pump) then runs.
- This is unconditional now (host GS backend consumes packets synchronously); the comment in
  `tol_overrides.cpp` explains why the thunk slot is the one that must be patched.
- Also fixed in `iop_cdvd.cpp`: `sceCdStatus` (ordinal 28) returned `kCdvdStatusPause` (0x0A)
  unconditionally; now returns ready (1). `signalCommandComplete()` now creates the CD interrupt
  event flag on demand instead of silently dropping the completion when the flag did not exist yet.
- The screen is still `NOW LOADING` because the **DVCI/CRI data load** is now the limiter again:
  `[cri] gameread` stops around 60-69 and `[dvci] load=5 (MOVIE.AFS) poll -> 0 progress=22`. That is
  the pre-existing read-stop blocker, separate from the GS/VIF1 gate.
- The only missing-target left is the harmless `jr ra` no-op at `0x231E80`.

### Session 72 - MOVIE.AFS DVCI read stop fixed

`FUN_001664D0` loads the six AFS extent tables with
`while (FUN_00196460(i) != 3) FUN_0019a080();`. For MOVIE.AFS (i=5) the poll never
returned 3, so the loop hung at i=5.

Trace (`TOL_TRACE_DVCI`): for i=5 the stream reported state 2 then 0, and the poll
parsed all 22 header entries (`progress` 0 -> 22) but **fell through** to the
next-read path (`FUN_001970e8`) instead of taking the `handle[8] <= progress` ->
`FUN_001963a8()` -> `return 3` branch. The recompiled flow only reaches the
completion path for larger tables; the 22-entry MOVIE.AFS table takes the
`iVar11 >= DAT_003910b4 << 9` fall-through.

Fix (`src/runtime/tol_overrides.cpp`, `dvciCompletionFix`, unconditional): wrap
`0x196460`; if the active load's request-table `handle[8]` (entry count) is nonzero
and `DAT_003910a4` (progress) has reached it, perform the `FUN_001963a8` cleanup
(`DAT_003910a8=0`, `DAT_003910ac=-1`, `DAT_003910a4=0`, `DAT_003910b4=0`) and set
`v0=3`. Verified with `[dvci-done]`:

```text
[dvci-done] idx=0 count=430  progress=430
[dvci-done] idx=1 count=790  progress=790
[dvci-done] idx=2 count=7931 progress=7931
[dvci-done] idx=3 count=1831 progress=1831
[dvci-done] idx=4 count=5501 progress=5501
[dvci-done] idx=5 count=22   progress=22
```

The extent-table loop now exits. Remaining: the screen still shows `NOW LOADING`
because after the tables the **scene** stream stops requesting reads
(`[adxf-time] reads=70` while `steps` keeps climbing). That is the next blocker,
separate from the DVCI table completion.

### Session 73 - scene stream stop traced to MOVIE.AFS / IPU stub

Traced the post-DVCI stream stop with `TOL_TRACE_DVCI` (`[adxf] step-in/out`
dump of stream `0x39EE48`):

```text
[adxf] step #34 done ... 46=1 49=1 dev=003A75F8 devState=1
[adxf] step-in  state=1 | 45=0 46=0 47=0 48=0 49=0 ... dev=00000000 devState=0
```

After step #34 the device closes (`dev=0`), `stream[0x46]`/`[0x49]` go to 0, and
the stream stays `state=1` with `45=0` (no client read request). `FUN_0019F298`
only calls the read (`FUN_001a9448`) when `stream[0x45] == 1`, so the step is a
no-op from then on. `FUN_001AB4F8` only issues `FUN_001029A8` in state 1, and its
state machine is no longer being driven.

The stream's volume (`totalSectors=323233`, ~662 MB) matches **MOVIE.AFS**, and the
runtime's IPU is a **stub** (`ps2xRuntime/src/lib/Kernel/Stubs/IPU.cpp`): `sceIpuInit`
pokes `IPU_CTRL`/`IPU_CMD`/FIFO and `sceIpuRestartDMA`/`sceIpuStopDMA`/`sceIpuSync`
just return 0. There is no MPEG decode. So the movie/logo player consumes ~386 KB
(`stream[+0x14]=0x5E75F`), waits for the decoder, and never requests the next
block. This matches the user's "before the logos ... the screen render is missing":
the logos are MOVIE.AFS movies, and the IPU never produces frames.

Next step is the IPU/MPEG path (real decode or a controlled skip/blank-frame path),
not the DVCI or DVCI/ADXF plumbing.

### Session 74 - correction: IPU is not touched; scene load is the ADXF/MBS path

Ran with `TOL_TRACE_IPU` (new, logs IPU/VIF/GIF MMIO in `ps2_memory.cpp`). In 15 s
the log contains **no** accesses to `0x10002000`/`0x10002010`/`0x10007010`; only
VIF1/GIF (`0x10003000`, `0x10003C00`, `0x10005000`). So the IPU stub is **not** the
current blocker and the planned blank-frame IPU would not help.

What actually happens after the extent tables (`TOL_TRACE_FILEIO`):

- The 70 `gameFileRead` calls are the AFS extent reads (sid `0x80000595`, client
  `0x3863D0`, HLE `gameFileRead` in `tol_overrides.cpp`); the last is
  `sector 0x700000 (file slot 6 +0)`.
- Then `ioman` sequentially opens/reads the six scene-layout files:
  `\MBS\SYS_REG.MBS` (1720), `MAP.MBS` (3160), `SND.MBS` (31724), `FIELD.MBS`
  (7324), `BATTLE.MBS` (22004), `MOVIE.MBS` (88). All succeed.
- After `MOVIE.MBS` the file I/O stops. The next guest activity is the sound init
  (`0x231e00`/`0x231e80`) and the ADXF stream (`0x39EE48`) stays `state=1`,
  `stream[0x45]=0`.

So the remaining stall is the ADXF consumer: `FUN_0019F710` (ADXF_Read, sets
`stream[0x45]=1`) is only called from `FUN_00197550`, which is driven by the game
loaders (`FUN_00206140`, `FUN_00230AD0`, `FUN_001f8530`). The read completion the
state machine (`FUN_001AB4F8`) waits on is the file-data RPC end function
`0x1016d0` (registered in the table), which clears `DAT_00385234`. Next: check
whether that end function is actually invoked for the client `0x3863D0`, or
whether the game simply stops issuing `ADXF_Read` after `MOVIE.MBS`.

### Session 75 - SIF end function was never invoked (fixed)

Root cause of the read-completion stall: the runtime **replaces** the guest
`sceSifCallRpc` (`0x118628`) with the HLE `tolSifCallRpc` (`tol_overrides.cpp`).
The HLE answers the RPC inline (GameFileRead, loadfile, IxAudio, ...), but it read
only `a0/a1/a3/t0/t1/t2` — not the **8th argument (end function) in `$t3`** or the
**end parameter in the first stack word**. So the guest's `0x1016D0` end function
(libsif packet copy + completion, which clears `DAT_00385234`) was **never called**.
Probing confirmed `0x1016D0` = 0 calls, while the CRI state machine `0x1AB4F8` ran
40× waiting on `FUN_00101f70(1)`.

Fix: `tolSifCallRpc` now wraps `tolSifCallRpcBody`, reads `endFunction =
GPR_U32(ctx,11)` and `endParameter = Load32(sp)`, and after the body queues a
`GuestInvocation` (`RpcCallback`) to run `endFunction` with `a0 = endParameter`
(same deferral as `RPC.cpp`'s reply callback). Verified: `0x1016D0` now runs **40×**.

Remaining: the **ADXF/scene stream** (`0x39EE48`) is a different client. It still
shows `state=1`, `stream[0x45]=0`, `dev=0`, `stream[+0x14]=0x5E75F`, and
`gGameFileReadCount` stays 69. Its read path is `FUN_00197550` -> `FUN_0019F710`
(ADXF_Read) -> `FUN_0019F298` (step) -> `FUN_001ad850`/`FUN_001aaf80` (CRI device
DMA via `FUN_001b6f68`/`FUN_001b6fe0`), not the libcdvd `FUN_001029A8` path. The
client (`FUN_00230AD0`/`FUN_00206140`) only re-reads when `FUN_00197668()==3`, so
the CRI device DMA completion is the next layer to fix.

### Session 76 - ADXF/CRI device: end-function fix landed, device handle is 0

`tolSifCallRpc` now also clears `DAT_00385234` for the libcdvd end function
`0x1016D0`, and `0x1016D0` runs 40x. That did **not** move the ADXF stream:
it still shows `state=1`, `45=0`, `46=0`, `48=0`, `49=0`, `dev=0`, `+14=0x5E75F`,
and `gGameFileReadCount` stays ~60-69.

So the ADXF stall is not the libcdvd pending flag. The read driver is
`FUN_0019EF58`: it looks at `stream[+0x14]` (remaining), `stream[+0x44]/[0x48]`
(cancel), then calls the file object's vtable (`fobj[0] + 0x18`) and
`FUN_001a98e0` / `FUN_001a9878` (vtable +0x20/+0x18). `dev=0` means the CRI device
handle (`*(*(stream+8)+4)`) is zero — the device is not open, so the read cannot
start and the stream never reaches state 3. `FUN_001aae58` returns
`&DAT_003a55d0` only when `DAT_003a55d0 == 0`; if the CRI device was closed or
never opened, it returns 0.

Next: the CRI device open/close (`FUN_001aae58`, `DAT_003a55d0`), the file object
at `stream+8`, and the vtable read methods, not the libcdvd SIF path.

### Session 77 - ADXF file object is 0 (never opened)

Dumped the ADXF stream fields: `stream+4 = 0x003B1998` (the driver object) but
`stream+8 = 0` (the file object), `stream+0xC = 0`, `stream+0x3C = 0`. The stream is
`state=1`, `45=0`, `46=0`, `49=0`, and step-in == step-out (no change).

So the stream is in its "reading" state with **no file object**. The step
`FUN_0019F298` only opens the file (`FUN_001a9448(stream[0x50], stream[0x54], 0)`)
when `stream[0x45] == 1`, and `FUN_0019F710` (ADXF_Read) is the only setter of
`0x45`; it is reached from `FUN_00197550` only when the caller's remaining
(`handle[0xC]`) is > 0xFFFFE and the stream is at state 3. The game loader
`FUN_00230AD0` only re-reads when `FUN_00197668()==3`. That leaves the stream
pinned at state 1 with no file object: the open path
`FUN_001a9448(stream[0x50]=0x1FFFEC0, stream[0x54]=0, 0)` is the next thing to
verify (it is what would set `stream+8` and let the state reach 3).

Forcing `DAT_00385234 = 0` did not change this, confirming the libcdvd path is no
longer the limiter here.

The stream's path is valid: `stream[0x50] = 0x01FFFEC0` contains
`CDV:\AFS\MAP.AFS`. So the ADXF stream is the MAP.AFS, and the open is not failing
on a bad name. It is simply never reached because `stream[0x45]` never becomes 1:
the only setter is `FUN_0019F710`, reached only from `FUN_00197550` when the
caller's remaining exceeds 0xFFFFE and the stream is at state 3. The loader
`FUN_00206140` opens by name, calls `FUN_00197550`, then waits
`while (FUN_00197668(handle) != 3)`. Need to find the initial state-1 transition
(who set `stream[+1] = 1` with `stream+8 = 0`) and the `FUN_001970e8` read that
should drive it to 3.

### Session 78 - ADXF total size is 0 (read computes negative remaining)

The ADXF stream fields are: `+0x0C = 0` (total size) and `+0x14 = 0x5E75F`
(position). `FUN_00196db0` computes `remaining = *(p+0xC) - *(p+0x14)` = a
**negative** value, so the read can never become "nothing left / state 3". The
credit tick `FUN_001972e0` copies stream state/position into the DVCI handle, and
`FUN_0019eda0` sets state 2 (reading) when `+0x14 != 0`, else 3. So with a zero
total and a nonzero position the state machine can only sit at 1/2.

The total (`+0xC`) is normally set by the ADXF open (`FUN_001a9448`) together with
the file object (`+8`). Both are 0, so the open never ran. The open runs in
`FUN_0019F298` only when `stream[0x45] == 1`, set by `FUN_0019F710`, reached from
`FUN_00197550` only when the DVCI handle's `+0x0C > 0xFFFFE`. So the next thing to
verify is the handle `FUN_00206140` opens with `FUN_00196a10` and whether its
`+0x0C` is the MAP.AFS size or 0.

### Session 79 - corrected: those fields are the ADXF stream, not the DVCI handle

The `+0x0C`/`+0x14` dump is the **ADXF stream** at `0x39EE48`, not the DVCI handle
(`lVar3`/`0x390738`). `FUN_00196960` (DVCI open) sets the handle: `+0x10 =
FUN_0019f560(stream)` (total), `+0x0C = FUN_0019f568(stream)` (stream position
`stream[+0x14]`). So the handle `+0x0C` is a position, not a total.

`FUN_00197550` gate: it calls `FUN_0019f710` (ADXF_Read, the only setter of
`stream[0x45]`) **only when the handle position `+0x0C > 0xFFFFE`**. At the stall
the stream position is `0x5E75F` (386 KB < 1 MB), so the ADXF read is never
requested, `stream[0x45]` stays 0, `stream+8` never opens, and the DVCI handle
never reaches state 3. `FUN_00206140` then waits
`while (FUN_00197668(handle) != 3)` forever.

Also confirmed `DAT_00393208` (pump guard) reading 1/5 is **normal** mid-pump, not
stuck; the guard-reset workaround was reverted.

Next: why the block loader's stream position sits at 0x5E75F and never advances
past the 1 MB ADXF gate — i.e. who should consume/advance the prefix before
`FUN_00197550` starts the ADXF read.

### Session 80 - device read "done" mode DAT_003a7782

`FUN_001ad850` stamps `stream[0x44..0x46]` from `DAT_003a7780/81/82` and
`stream[0x47]`... The ADXF step (`FUN_0019F298`) completes only when the read-done
marker (`stream[0x46]`) is 1, and `stream[0x46]` comes from `DAT_003a7782`
(param_3 of `FUN_001aeab0`). The trace shows `sync=0` (`DAT_003a7784`), i.e. async
mode, and the completion path depends on `DAT_003a7782`.

`FUN_001aeab0(set, mode, done, sync)` is the CRI device-config setter; it is called
only from the CRI init `FUN_0019f7b0`/`FUN_0019f9a8`, which `FUN_001663a0` drives
with the `apuStack_20` config (`{&DAT_003c0a28, 0, 0}`). So the done mode is 0 and
the async read never reports done; the stream stays state 1 and the block loader
`FUN_00206140` spins `while (FUN_00197668 != 3)`.

Next: `FUN_0019f7b0`'s `FUN_001aeab0` arguments (the done/sync modes) and the async
device-read completion (`FUN_0019ef58` / `FUN_001ad850` with `DAT_003a7784==0`).

### Session 81 - CORRECTION: the ADXF stream is NOT stuck at state 1; the async read completes

Instrumented `FUN_0019EF58` (the ADXF read/completion step), `FUN_00196DB0` (the per-arm
dispatch), `FUN_00197940` (the pump), `FUN_0019A080` (the wait) and a full 40-slot stream
dump. Results:

* The `state = 1` seen by the `FUN_0019F298` step probe was an **arm-boundary snapshot**:
  `FUN_00196DB0` deliberately resets the stream to state 1 (`FUN_0019EEC8` ->
  `FUN_0019EE60`, which sets `stream[1] = 1` unless a read is in flight) before arming the
  next arm. It is not a deadlock.
* The async device read works end to end: `FUN_0019EF58` goes `st=2 busy=0` -> `busy=1` ->
  `st=3`, `stream[0x34]` reaches `stream[0x5C]`, `stream[0x58]` (blocks done) increments, and
  the credit tick `FUN_001972E0` then lifts the DVCI handle to `h1=3`.
* `[io-wait]` shows steady progress: one 2 KB block per wait, ~1.5 waits/s, with the loop
  walking the six AFS files (`894`, `386911`, `223038`, `17204`, `323233`, ...) and `h14`
  tracking `s58` exactly.
* At wait #60 (~40 s) the `while (FUN_00196460(i) != 3) FUN_0019A080();` loop in
  `FUN_001664D0` **finishes** and `FUN_0019A080` stops being called at all. So the whole
  extent-table load (`FUN_001663A0` -> `FUN_001664D0`) completes.

The arm size is intentionally 1 block: `FUN_00196058` loads `t0 = 0x00391140` / `t1 = 0x800`
(a static 2 KB extent table) and `FUN_001961A8` sets `DAT_003910B4 = 0x800 >> 11 = 1`. That is
fine because these are AFS **extent** entries (a handful per file), not per-sector reads.

Therefore the ADXF/CRI device-read path is healthy and the remaining `NOW LOADING` stall is
**after** `FUN_001663A0` returns into `FUN_00165C90` (the startup/logo routine), not in the
stream state machine. Next: trace what `FUN_00165C90` does after `FUN_001663A0` and why the
main loop never reaches the scene load `FUN_002073A0`/`FUN_00206140` (still 0 calls).

### Session 82 - place after the extent load: FUN_00166120 -> script engine

Startup order at `0x165AE0` (inside `FUN_00165AF0`):

```
FUN_00165C90  startup: CRI init + AFS extent load + logos
FUN_00165E70  frame update
FUN_00166010  GS/VIF1 wait (now exits)
FUN_00166120  script/event dispatcher
FUN_001667E0 / FUN_00166870
0x139D50 ...  render setup
```

`FUN_00166120` is `uVar1 = thunk_FUN_001f88c0(); while (thunk_FUN_001f88f0(uVar1) == 0) { op =
thunk_FUN_001f8920(uVar1); arg = thunk_FUN_001f8960(uVar1); dispatch(op, arg); uVar1 =
thunk_FUN_001f89a0(uVar1); }` followed by `FUN_0027AD50(); FUN_00279C00(); FUN_0027A140();
FUN_002EB320(); FUN_001f7290(1);`.

Control now parks in `FUN_0027AD50` (entered once from `0x166240`) and resumes at `0x28B0C8`.
That address is **not a function**: it is `caseD_0_0x28b0c8.cpp`, i.e. `switch` case 0 of
`FUN_0028B010` (cases at `0x28B0C8/0x28B0DC/0x28B0F0`). The `[pc] exit <fn> pc=<other>` lines in
`TOL_TRACE_PC` are **checkpoint bails**, not returns - do not read them as `$ra` corruption.

Tried a sentinel-`$ra` rework of the `0x1F8150` GS worker override on the theory that the
`pc=0x1F8168 ra=0x151F88` bail meant `$ra` corruption. It regressed: worker steps went 2 -> 1
and `DAT_00407740`'s flag stayed 0 instead of reaching 1. Reverted and re-verified (2 steps,
flag 0 -> 1). The current `0x1F8150` override is correct as written.

Next: `FUN_0027AD50` / the `FUN_0028B010` switch-case resume, and why the dispatcher never
advances past the first script command.

### Session 83 - FIXED: jump dispatch fell through, restarting switch case 0

`FUN_0028B010` ends in a jump-table switch:

```
0x28b0a0  sltiu at, s0, 9
0x28b0a4  beqz  at, 0x28b17c          ; default
0x28b0ac  v0 = table[s0]              ; table at 0x3CA9F0
0x28b0c0  jr    v0
```

Each case is emitted as its own function (`caseD_0_0x28b0c8.cpp` … `ps2_default_0x28b17c.cpp`)
and they share the parent's epilogue: `caseD_0` computes `v0 = clamp(9999, a0)`, then
`b 0x28B180` and returns with `ctx->pc = 0x28B180`.

The parent's `jr v0` was emitted as:

```cpp
if (!runtime->dispatchGuestBranch(..., GuestBranchKind::IndirectJump, "JR")) {
    return;
}
label_fallthrough_0x28b0c0:
ctx->pc = 0x28B0C8u;      // <-- clobbers the case's 0x28B180 continuation
```

A jump never falls through, but codegen did, so every `jr v0` ran `caseD_0` and then reset
`ctx->pc` back to `0x28B0C8` - an endless case-0 loop. That is why `FUN_0027AD50` and
`FUN_00166120` never returned and the startup stopped right there.

Fix (two parts, both applied):

1. **Emitter** - `thirdparty/PS2Recomp/ps2xRecomp/src/lib/control_flow_emitter.cpp`:
   `emitExternalJumpDispatch` (non-call branch) and `emitExternalRegisterJumpDispatch` now emit a
   `return;` after the dispatch, so `ctx->pc` as left by the target body is honoured. Calls
   (`JAL`/`JALR`) still fall through.
2. **Existing corpus** - the same transformation applied mechanically to `data/output_fixed/`:
   **302 files, 1324 jump-dispatch sites** (pattern: `dispatchGuestBranch` + `GuestBranchKind::
   IndirectJump|DirectJump`, followed by `return;` then `}`).

Verified result: `FUN_00166120` now completes, the startup proceeds to `FUN_001667E0` and
`FUN_00166870` (both previously unreachable), and the case-0 spin is gone.

Next blocker: after `FUN_00166870`, control returns to `0x151F88`; the GS worker runs
(`FUN_001F8000` x2, `FUN_0015231C` x4) and then execution parks, so the next call in the startup
sequence, `FUN_00139D50` at `0x165B54`, is still never reached.

### Session 84 - the park is the guest coroutine scheduler (FUN_0015231C)

`0x151F88` is inside `FUN_00151E90`, the DMA packet-list walker, and is exactly the instruction
after a `jal FUN_0015231C`. `FUN_0015231C` is **not** a flush - it is the guest's thread
yield/context switch:

```
iVar2    = iRam003db1a0 * iRam003db1b0;
puVar1   = *(u32 **)(iVar2 + 0x48F590 + iRam003db1c0);   // per-context save slot
puVar1[0x6b] = 1;
puVar1[0..] = v0, v1, a0..a3, s0..s8, sp, ra, f0..f31;    // full register spill
... then jumps to 0x151F88 instead of `jr $ra`
```

`FUN_001F84B0` calls it with `$ra = 0x1F8528` (the instruction after `jal FUN_0015231c`) and
expects to be resumed there. The port has no coroutine scheduler, so that continuation is
discarded and the startup parks inside `FUN_00151E90` with `pc == ra == 0x151F88`. Every one of
the ~30 callers of `0x15231C` has the same problem.

Three approaches were tried and **all reverted** (baseline re-verified: worker steps 2,
`DAT_00407740` flag 0 -> 1, `FUN_00166120`/`FUN_001667E0`/`FUN_00166870` all reached):

| experiment | result |
|---|---|
| worker inner calls use `$ra = savedRa` instead of `0x1F8160`/`0x1F8168` | worker steps 2 -> 1, flag stuck 0 |
| skip the `FUN_0015231C` call in the worker override entirely | worker steps 2 -> 1, flag stuck 0 |
| override `0x15231C` as a no-op "return to $ra" | worse - `FUN_00166120` never reached |

Conclusion: this is not a `$ra` bug to patch locally. Making progress needs a real cooperative
thread model for the guest - save/restore the per-context block at `0x48F590` (index from
`DAT_003db1a0 * DAT_003db1b0 + DAT_003db1c0`, state word `ctx[0x6b]`) and round-robin between
contexts, with `FUN_0015231C` as the yield point. That models what the hardware thread does and
is what `FUN_00166010`'s flag wait and the GS worker were designed around.

### Session 85 - CORRECTION: no host scheduler needed; the second GS worker was the loop

Scoped the "scheduler" properly and the premise in Session 84 was wrong:

* `FUN_001F8180` creates **two** guest threads through `FUN_00151CD0(entry, arg, prio, name)`:
  `0x1F8150` (worker 1, already overridden) and **`0x1F84B0`** (worker 2).
* `FUN_00151CD0` -> `FUN_00151C80` scans the guest's own **64-entry thread table at `0x407160`**
  (stride `0x210`; state `+0x1F0`, id `+0x1F1`, callback `+0x1B0`, callback arg `+0x1C0`,
  `+0x1AC`, `+0x200`) and fills the first free slot via `FUN_001518C0`. Thread lifecycle is
  entirely guest-owned - there is nothing for a host scheduler to own.
* `FUN_0015231C` is **save-only**: it spills the register file into `ctx = table[A*B+C]` at
  `0x48F590` (`A`=`0x3DB1A0`, `B`=`0x3DB1B0`, `C`=`0x3DB1C0`), sets `ctx[0x1AC] = 1`, stores `$ra`
  at `ctx[0x1B4]`, and `jr ra`. No switch, no restore, no scheduler counterpart.
* `FUN_001F84B0` is the actual loop:
  `do { while (queue == 0) FUN_0015231c(); lock; dispatch(queue); unlock; } while (true);`
  i.e. a second infinite worker, same shape as `0x1F8150`.

**Fix applied:** `0x1F84B0` is now overridden to process one work item per dispatch and return to
`$ra` (mirrors `0x1F8150`; logs `[gs] worker2 step #N queue=...` under `TOL_TRACE_GS`).

Also established that the `0x1F8150` override's interior `$ra` values (`0x1F8160` / `0x1F8168`)
are **load-bearing**: they make the guest run its real loop body, and that pass is what raises the
completion flag. Tried three replacements - `$ra = savedRa`, two host-driven iterations, and
skipping the save call - and all left `DAT_00407740` at 0. Original kept.

Verified baseline after the change: worker 1 = 2 steps, worker 2 = 1 step, flag 0 -> 1,
`FUN_00166120` / `FUN_001667E0` / `FUN_00166870` all reached.

Still open: `FUN_00166870` hands off at `0x1F8168` and `FUN_00139D50` at `0x165B54` is never
reached, so the remaining question is the worker-1 re-entry contract (why the guest loops back
into `0x1F8150` instead of returning through `$ra`), not a missing scheduler.

### Session 86 - worker 1 does NOT re-enter; the real spin is the script engine at 0x1ECC40

Instrumented the `0x1F8150` override (entry / guard / after-step / after-save counters, gated by
`TOL_TRACE_GS`). Result over a full run:

```
[gs] worker entry #1 savedRa=0x00151F88 afterStep=0x001F8160 afterSave=0x00151F88
                   exit=0x00151F88 guardHits=0
```

* Exactly **one** entry, **zero** guard hits. Worker 1 never re-enters `0x1F8150`.
* `afterStep = 0x1F8160` and `afterSave = 0x151F88`: the step returns into the loop's
  `jal FUN_0015231c`, and the save returns straight to the callback's caller.
* `0x1F8150`'s body is `addiu sp,-0x10 / sd ra,0(sp) / jal 0x1F8000 / jal 0x1F5231C / b 0x1F8158`,
  so `0x1F8160` and `0x1F8168` are the correct continuations, and `0x1F8168` is also one of the
  slots the override replaced - which is why a checkpoint bail there *looked* like a re-entry.

**Conclusion: the re-entry contract is already correct; nothing to fix there.** The earlier
`pc=0x1F8168` in the `TOL_TRACE_PC` exit line is a checkpoint bail, not a jump.

Also: `eeCheckpointDue()` is time-based, so single-run A/B counts (e.g. `FUN_001667E0` 0 vs 1) are
not reliable evidence on their own - use `TOL_TRACE_SPIN` and longer windows.

**Real remaining blocker, found with `TOL_TRACE_SPIN=1`:** after ~7.8M branches the guest pins at

```
[spin] pc=0x1ECC40 ra=0x1ECD04   (identical for 3,000,000+ branches / ~30 s)
```

`0x1ECC40` is `jal 0x1F7350` inside `FUN_001ECBE0`, the script/event engine's per-object update.
Its loop is

```
0x1ECC38  bne  v0, zero, 0x1ECDA0    ; exit when the script reports finished
0x1ECC40  jal  0x1F7350              ; peek next opcode
0x1ECC4C  bne  v0, 0x1b, 0x1ECCF0    ; only opcode 0x1b is handled here
```

with the "finished" predicate `FUN_001f88f0(handle) = *(u32*)handle <= handle >> 32`, i.e.
`index < count` on a fat pointer (low word = command table base, high word = current index; the
opcode peek is `base + 0x10 + index*0x10`). The script context for `s1` never reaches
`index >= count`, so the dispatcher drains forever and `NOW LOADING` never advances.

Next: why that script context's `count`/`index` never converge - instrument `FUN_001f89a0`
(advance) and `FUN_001f8920` (peek) for the `FUN_001ECBE0` handle, and identify the opcode the
script is parked on.

### Session 87 - FIXED: script dispatcher spun on a NULL command block

Instrumented the script-context thunks (`TOL_TRACE_SCRIPT=1`):

```
[script] status  #1 handle=0000000001523010 base=0x01523010 index=0 count=0          <- empty, finishes
[script] status  #2 handle=0000000000000000 base=0x00000000 index=0 count=1065353216
[script] status  #3 handle=0000000100000000 base=0x00000000 index=1 count=1065353216
...
[script] status  #6200000 handle=005E9ABE00000000 base=0x00000000 index=6199998 count=1065353216
[script] peek    #6200000 -> opcode=0x00000000
```

So the index **was** advancing correctly all along (`advance` produced index+1 every time). The
problem is the **base is 0**: the packed handle is `(index << 32) | 0`, so
`FUN_001f88f0` loads `count` from guest address 0, which holds `0x3F800000` (the float 1.0f) =
**1,065,353,216**. The dispatcher therefore has to walk a billion indices before it can report
"finished" - the permanent `pc=0x1ECC40` spin.

Handle layout (confirmed from MIPS): low 32 = command block pointer, high 32 = command index;
`count = *(u32*)base`; advance = index+1; `status = !(index < count)`.

Also found: the thunk at `0x1F7340` does **not** go through the function table - its generated
body calls `FUN_001f88f0_0x1f88f0(rdram, ctx, runtime)` directly. So overriding `0x1F88F0` never
fires on the dispatcher path; the guard has to be installed on the **thunk `0x1F7340`**, which the
dispatchers do reach via `dispatchGuestBranch`.

**Fix:** `scriptStatusProbe` (installed unconditionally on `0x1F7340`) now treats a null command
block as empty and returns "finished" (v0 = 1) without touching guest state, delegating to the
real predicate for every non-null handle. A null script has no commands, so this is correct
semantics, not a workaround for the dispatcher.

Verified with `TOL_TRACE_SPIN=1` over 60 s: `pc=0x1ECC40` no longer appears at all, and the
sampler keeps moving through real work (`0x27B6E0`, `0x334294`, `0x3436B8`, `0x19BCC8`,
`0x19F4A0`, `0x1AC518` - the ADXF/CRI path) with no sticky PC.

Still open: `FUN_002073A0`/`FUN_00206140` (scene load) not reached yet; the guest is now running
I/O code instead of spinning.

### Session 88 - scene load blocked: resource table (gp-0x7534) never initialised

With the script spin gone, the startup reaches `FUN_00166870` -> `FUN_00164EA0` -> **`FUN_001EC670`**,
which runs five sequential asset-load stages:

```
job = FUN_001EC9A0(stage, resourceId, 1);
while (FUN_0027BFF0(job) == 0) { FUN_00151FF0(0); FUN_0019A080(); FUN_001EE060(); FUN_001392D0(); }
```

`FUN_0027BFF0` is just `*job == 2`. Instrumented with `TOL_TRACE_LOAD=1` (probes on `0x1EC9F0`,
`0x27BFF0`, `0x3710C0`):

```
[load] resource id=1 gp=0x003E2BF0 slot=0x003DB6BC table=0x00000000 -> 0x00000000
[load] resource id=2 ... table=0x00000000 -> 0x00000000
[load] resource id=3 ... table=0x00000000 -> 0x00000000
[load] resource id=4 ... table=0x00000000 entry=0x3F800000 -> 0x0FE00000
[load] start res=3 resource=0x0FE00000 flag=1 -> job=0x004E8A90 state=1
[load] status #2.. job=0x004E8A90 state=0      (never reaches 2)
```

The chain:

1. `FUN_003710C0(id) = *(u32*)(table + 0x10 + id*4) >> 2`, where
   `table = *(gp - 0x7534)`. Note the decompiler's constant is `4294937292` (= -0x7534); the
   similar-looking `4294939820` used by `FUN_002EB320` is a different offset (-0x6B54) and is a
   red herring.
2. That pointer lives at `0x003DB6BC`, which the ELF ships in the **zero-filled tail** of the
   single PT_LOAD (`vaddr=0x00100000 filesz=0x2DAF80 memsz=0x3E9B80`), i.e. `.bss` - so it starts
   as 0 and must be set at runtime.
3. It is written **only** by `FUN_003710A0`, whose only caller is `FUN_0027A990`, which is only
   called as **script opcode 0** from `FUN_00166120`.
4. `FUN_00166120` runs with `a0 = 0x01523010`, and that script block's first word is 0, so
   `FUN_001f88f0` reports "finished" immediately: the boot script has **no commands** and no
   opcode 0 ever executes.

Net effect: stages 0-2 resolve to 0 (skipped), stage 3 gets garbage `0x0FE00000` from address
`0x20`, creates a job that immediately drops to state 0, and the startup blocks forever in
`FUN_001EC670`. `NOW LOADING` stays up.

Next: find why the boot script context at `0x01523010` is empty - where its command block is
supposed to be loaded/populated, and whether `FUN_0027A990` should also run from an earlier
(non-script) init path. Probes for this are already in place: `TOL_TRACE_LOAD=1`.

### Session 89 - boot script is a 1.2 MB transfer that never lands

Traced the empty boot script to its origin. `FUN_00166010` does:

```
size  = FUN_001f72d0(0);            // resource 0 size + 0x10
buf   = FUN_001013e0(0, size);      // the script block at 0x01523010
thr   = FUN_00151cd0(0x1660a0, 0, 1, 0);
FUN_00152030(thr, &flag, buf, 0, 0);
while (flag == 0) { FUN_00151ff0(0); FUN_00151ff0(1); FUN_001392d0(); }
return buf;                          // -> becomes FUN_00166120's a0
```

`FUN_001660A0` (the thread entry) starts the load and waits:

```
uVar2 = FUN_001f72f0(0, buf, 0);            // -> FUN_001f82f0 -> FUN_001f8be0 (queue) -> FUN_001f8ac0
lVar3 = FUN_001f8480(uVar2);
while (lVar3 != 0) { FUN_0015231C(); lVar3 = FUN_001f8480(uVar2); }
*flag = 1;
```

`TOL_TRACE_LOAD` shows the real request:

```
[load] submit #1 type=2 id=0 size=1252176 dest=0x01523010 -> handle=0x00000080
```

So the boot script is a **1,252,176-byte transfer into 0x01523010**. `FUN_001f8be0` only *queues* a
0x2C descriptor at `0x4A72C0 + slot*0x2C` (status byte +2 starts at 2; handle at +8). The queue head
is `*(gp-0x7860)` = `0x003DB390`, and the drainer is **worker 2 (0x1F84B0)**, which dispatches
`0x1F8530` / `0x1F87C0` per descriptor (`0x1F8530` opens the file with the descriptor id via
`FUN_00196b88` and reads it).

Fix applied: this port enters guest thread entries only once, so nothing ever drained that queue.
`ioPumpProbe` (installed on `FUN_0019A080`, which every load wait loop calls each iteration) now
invokes worker 2 whenever the queue head is non-null. Verified:

```
[load] pumping transfer queue head=0x004A72C0 (pump 1)
[load] pumping transfer queue head=0x004A72EC (pump 2)
```

Remaining gap: the handler issues an **asynchronous** DVCI read and relies on the status byte being
cleared by completion, but `FUN_001660A0`'s wait is `while (FUN_001f8480(h) != 0) { FUN_0015231C(); }`
- it only yields, it never pumps I/O. Forcing "done" (the current default) lets the waiter proceed
before the data lands, so the buffer is still zero and `count = 0`. Reporting the real status instead
deadlocks: submit #1 happens and then no pumps and no script context (verified with
`TOL_VIF_STATUS_REAL=1`, which restores hardware behaviour).

So the stub stays default. To actually land the data, the transfer has to be driven to completion
*inside* that yield-only wait - i.e. make `FUN_001f8480` perform/pump the pending transfer
synchronously before reporting, rather than just yielding.

### Session 90 - yield now pumps the DVCI engine; transfer handler hits a stale spinlock

Two more fixes landed, both verified, but the boot script still does not land.

1. **Yield pump.** `FUN_001f8530` waits `while (desc[2] != 3) { ... FUN_0015231C(); }` and
   `FUN_001660A0` waits `while (FUN_001f8480(h) != 0) FUN_0015231C();` - both only yield, so in this
   scheduler-less port the DVCI read could never retire. `yieldPump` (installed on `0x15231C`) now
   runs the original context save and then pumps `FUN_00197940` (the DVCI/ADXF engine), guarded
   against re-entry. Verified: `[load] yield-pump #1..#6`.

2. **Stale transfer spinlock.** `FUN_001f7390` opens the handler with
   `while (iGpffff8780 != 0) FUN_0015231C(); iGpffff8780 = 1;` (lock at `gp-0x7880` =
   `0x003DB370`). A cooperative checkpoint inside the handler leaves it held by the same logical
   thread, so every later acquire spins forever - which is why `0x1F8530` was entered 22x without
   ever reaching `FUN_00196b88`/`FUN_001970e8`. `transferLockProbe` clears a stale lock before
   acquiring (single guest thread, so it can only ever be stale). Verified:
   `[load] clearing stale transfer lock (was 1, clear #1)`.

Remaining blocker: the handler now gets past the lock but is **restarted** rather than resumed.
`0x1F8530` bails at a cooperative checkpoint (`pc=0x001AD2D8`, later `pc=0x0010EE5C`), the bail
unwinds back into the queue pump, and `ioPumpProbe` re-dispatches the handler **from its entry** on
the next iteration - so it redoes the lock/reset and bails again, never progressing to the DVCI
open/read. Still unlanded: `submit #1 id=0 size=1252176 dest=0x01523010`, so
`*(0x01523010) == 0` and the script context stays empty (`count = 0`), the resource table
(`gp-0x7534`) stays null, and `FUN_001EC670` stage 3 keeps failing.

Next: make the queue pump **resume** the in-flight handler instead of re-entering it - i.e. keep
the handler's `ctx->pc` across pumps (only dispatch `0x1F8530` when the descriptor status is still
2/queued, and let the runtime continue an in-progress one), which is the same class of problem as
the earlier checkpoint-resume work.

### Session 91 - transfer handler coroutine: resume captured, but re-entry does not take

Implemented the resume discipline. Worker 2 (`0x1F84B0`) is now a coroutine driver:

* only *starts* a descriptor whose status byte (`slot+2`) is still `2` (queued);
* on a preempted handler it records `gXferResumePc = ctx->pc` plus `gXferSlot`, then returns
  normally to the caller (the handler's guest stack frames stay intact);
* on the next pump, if `gXferSlot` still matches the queue head, it re-enters at
  `gXferResumePc` with `$ra = 0xFFFFFFF0` (kXferSentinel) so a completed handler still returns
  into the override and can run `FUN_001f8da0` (reap) + `FUN_00153190` (unlock).

Verified the capture works:

```
[load] pumping transfer queue head=0x004A72C0 (pump 1..8)
[load] transfer handler preempted, resume at 0x001AD2D8 (slot=0x004A72C0, #1)
```

But it still does not progress: there is exactly **one** preemption and no subsequent
re-entry - `0x1F8530` is entered once, `0x1F96B88` / `0x1F970E8` / `0x1F8DA0` are all **0**, and
`*(0x01523010)` stays 0. So pumps 2..8 run the worker but fall through the resume path (no second
"preempted" log and no reap), which means the `gXferResumePc != 0 && gXferSlot == queue` branch is
not being taken, or `lookupFunction(gXferResumePc)` returns null and the branch silently falls
through to "completed". That is the next thing to instrument (log inside the resume branch itself,
before and after `lookupFunction`).

Boot script still unlanded: `submit #1 id=0 size=1252176 dest=0x01523010`, `count = 0`,
resource table `gp-0x7534` still null, `FUN_001EC670` stage 3 still failing.

## Session 92 (2026-11-07) - boot script lands, audio path bound, and the screen is black

### Committed milestones

| commit | what |
|---|---|
| `cec6b04` | boot-script load fixed: the load stage (`FUN_00166010`) now pumps the CRI device per tick via a queued invocation of `FUN_0019A080`. Boot script lands, resource table `0x003DB6BC` populates, script engine walks (count 5/48, index advancing), 6 asset files load |
| `9a3aa89` | ADX lifecycle completed through the guest's own detach (`FUN_00199988`), refcount 1 -> 0, stream flags set; waiter thread exits and the starved ADX workers run |
| `45f931c` | `sysclib #19` (sprintf) bound in the LLE IOP kernel |
| `77bec89` | `dmacman` bound (4 reset, 5/6 env, 8/9 CHCK/MADR, 14 `sceDmaSend`, 15 `sceDmaSendN`) routed through `Iop::dmaTransfer`; `no kernel service for dmacman` 7 -> 0, real DMA and SPU2 register writes now happen |
| `a42afbe` | SPU DMA completion IRQ traced: lines 36/40 register to `0x49CE8`, delivered, never disabled - the IRQ was not dropped |
| `94399fd` | outer-loop trace + correction: the main thread never exits; `tid 1 Dormant` is the bootstrap record |
| `ca550d1` | one outer iteration traced end to end: load stage -> pipeline (both bail on ordinary checkpoint preemption at worker 1's `0x1F8150`/`0x1F8168`) -> frame loop runs normally, repeatedly |
| `f3f85b9` | GS state recorded (frames presented, picture black) |

### Where it stands

The game boots, loads assets, runs the frame loop - and the picture is black.

```
frame_000012.ppm  max=230  mean=0.7  nonBlack=0.66%   <- NOW LOADING visible
frame_001012.ppm  max=0    mean=0    nonBlack=0%      <- black from here on
```

Screenshots come from `TOL_GFX_SCREENSHOT_DIR` / `TOL_GFX_SCREENSHOT_EVERY` (`gs_frontend.cpp:614`),
which writes the latched host presentation frame (512x448 PPM).

### GS history measurement (`TOL_TRACE_GSHIST=1`)

`GS` keeps a 512-entry `GSDebugHistoryEntry` ring but starts **paused**; the probe unpauses it at the
frame tail (`0x00139A20`) and reports what the guest submits:

```
[gs] f=30 tags=42 regs=60 draws=410 verts=1192 transfers=0 presents=0
     lastPrim=4 y=[2240.0..2272.0] drawFbp=224 range=[0..224] fbp=0<-0 preferred=0
```

Reading it:

- **The guest does draw**: 410 Draw entries, 1192 vertices, ~14 draws per frame, continuously. So this
  is not "nothing is submitted".
- 42 + 60 + 410 = 512 = ring capacity, so these are the last 512 GS events, not a per-window count.
- **`presents=0`**: no Present event is ever recorded.
- Draws target **FBP 224** (0x70000) as well as FBP 0; `y=[2240..2272]` is 12-bit fixed point, i.e.
  140..142 px, plausible on a 448-line buffer.
- `transfers=0`: no texture uploads in that window.

So the open question is the one the graphics skill calls "same packets, wrong picture": the guest draws
into a framebuffer, and what gets presented is black. The presenter builds its request from
`pmode` / `dispfb1` / `display1` (+ preferred source) in `buildPresentationRequestUnlocked`, so the next
step is to compare **the FBP the display registers select** against **the FBP the draws target (224)**.
If the display selects FBP 0 while everything is drawn to 224, the presenter is reading the wrong buffer.

### How to reproduce any of this

- Screenshots: `TOL_GFX_SCREENSHOT_DIR=<dir> TOL_GFX_SCREENSHOT_EVERY=20`.
- GS submission: `TOL_TRACE_GSHIST=1`.
- Boot flow: `TOL_TRACE_BOOT=1`.
- Transfer/CRI: `TOL_TRACE_XFER=1`, `TOL_TRACE_DVCI=1`.
- Audio lifecycle: `TOL_TRACE_ADX=1`; `TOL_NO_STREAM_RELEASE=1` disables the provoked ADX detach for A/B.
- IOP interrupts: `TOL_TRACE_INTR=1`.

### Known workarounds still in place

- ADX is detached by the port after ~4096 spins of the shutdown waiter (`installSpinRelease`), because
  the game never closes the audio session during boot. Documented, default on, `TOL_NO_STREAM_RELEASE=1`
  disables.
- KORG RPC functions 0x0/0x11/0x3C are answered with a zero reply in `ps2xIOP` because those handlers
  never complete.

## Session 93 (2026-11-07) - the renderer is alive; the guest is still loading, at ~1.5 fps

Picked up from Session 92's "frames are black". The measurements below separate three things that
looked like one bug: what the guest submits, what the display selects, and how fast the guest runs.

### 1. The guest submits continuously

`TOL_TRACE_GSHIST=1` unpauses `GSDebugHistory` (it starts paused) and reports at the frame tail
(`0x00139A20`):

```
[gs] f=30 tags=42 regs=60 draws=410 verts=1192 transfers=0 presents=0
     lastPrim=4 y=[2240.0..2272.0] drawFbp=224 range=[0..224] fbp=0<-0 preferred=0
[gs]    bounds px(x/16) x=[112.0..152.0] y=[114.0..142.0] degenerate=0 prim[0..7]=0/0/0/0/372/0/38/0
[gs]    fbp0=38 fbp224=372
```

- 410 Draw entries per 512-entry ring window, 1192 vertices, none degenerate; primitive types 4 (372
  draws) and 6 (38 draws); all geometry inside a 112..152 x 114..142 pixel box.
- Draws target **FBP 224** (0x800E0, i.e. 0x70000) mostly and FBP 0 for the rest.
- `FRAME_1` is rewritten constantly and alternates between FBP 0 and FBP 224: the guest double-buffers.

### 2. The display never leaves FBP 0

`TOL_TRACE_GSPRIV=1` (ps2_memory write64) shows the guest performing **exactly three** privileged
writes for the whole run:

```
[gspriv] PMODE   = 0x0000000000000066   -> EN1=0, EN2=1 (display circuit 2)
[gspriv] DISPFB2 = 0x0000000000001000   -> FBP 0, FBW 8 (512 px), PSM 0 (32-bit)
[gspriv] DISPLAY2= 0x001BF9FF0203227C   -> DW 2559 / MAGH 4 = 512 wide, DH 447 = 448 high
```

`TOL_TRACE_GSPRESENT=1` confirms every presentation is 512x448, HostPixels, `displayFbp=0`,
`sourceFbp=0`. `DISPFB1`/`DISPLAY1` are never written at runtime (init values only: 0x1400 / 640x448),
and `DISPFB2` is never moved to 224.

So the guest draws into 224 and displays 0. That is either this game's own scheme (224 as a work
buffer) or the display swap that should follow is not happening - which cannot be decided until the
guest gets past the loading phase.

### 3. The guest runs at ~1.5 frames per second

- `TOL_VBLANK_PACING_TRACE=1`: only 61 VBlank ticks in 40 s; each VBlank fires 0.5-1.9 s late.
  When the guest goes idle the same trace shows 60 ticks/s at ~2 ms late, so the pacer itself is fine -
  the guest is simply holding the EE for ~0.6 s per frame.
- `TOL_TRACE_GSHIST` shows the sequence number advancing ~12k per 30 outer-loop tail calls (about 400
  GS events per tail call), so work is being submitted; it is the frame clock that is slow.

### 4. The boot script is advancing

`TOL_TRACE_SCRIPT=1` over 60 s:

```
[script] status  #2  handle=...015230A0 index=0  count=48
[script] advance #1 -> index 0 -> 1
...
[script] advance #16 -> index 15 -> 16
```

The resource table handle walks 0 -> 16 of 48 entries. `loader-entry` is `FUN_00164F80`, a thread
entry that never returns, so a single `loader-entry #1` is expected; the dispatch/slot-loop counters
inside it are the real progress signal.

### 5. The two raster backends

- `--gs=sdlgpu` (GPU rasteriser): frames are presented continuously, `frame_000012` has content
  (max=230, 0.66% non-black - the NOW LOADING screen), `frame_001012` and later are fully black.
- `--gs=sw` (software rasteriser, the default): only two frames are produced in 40 s because it is far
  slower, and both contain the same 0.66% non-black NOW LOADING image.
- GS VRAM readback (`TOL_DUMP_VRAM`, 4 MB) is all zero under `sdlgpu`: expected, because that backend
  rasterises on the host GPU and does not fill GS memory. Not evidence of a rendering failure.

These runs are not a same-guest-moment comparison, so "sw shows content, sdlgpu shows black" is not a
conclusion yet - only that a picture exists and that the loading screen is static.

### Probes added

| probe | location | what it does |
|---|---|---|
| `TOL_TRACE_GSHIST` | `src/runtime/tol_overrides.cpp` | unpauses GS debug history, per-window submission counts, draw bounds/prim histogram, per-frame digests, newest sequence and GS tick |
| `TOL_DUMP_VRAM` | same | writes the live 4 MB GS VRAM at the 400th frame tail (gated with `TOL_TRACE_GSHIST`) |
| `TOL_TRACE_GSPRESENT` | `ps2xRuntime/src/lib/gs/gs_frontend.cpp` | logs every 30th presentation: presented/mode, display+source FBP, size, PMODE/SMODE2, DISPFB1/2, DISPLAY1/2, context FBPs |
| `TOL_TRACE_GSREG` | same | logs FRAME_1/2 and privileged GIF register writes |
| `TOL_TRACE_GSPRIV` | `ps2xRuntime/src/lib/ps2_memory.cpp` | logs privileged MMIO writes (PMODE, DISPFB1/2, DISPLAY1/2) when the value changes |

### Where the work goes next

1. **Speed first.** The boot is progressing but at ~1.5 guest fps, so every screen test is expensive.
   Profile one guest frame (EE execution vs GS submit vs raster) before changing anything else; the
   VBlank pacing trace already proves the pacer is not the bottleneck when the guest idles.
2. **Re-check the display after the script reaches 48.** If `DISPFB2` moves to FBP 224 then, the port
   is rendering correctly and the black screen was the loader. If the script finishes and the display
   is still FBP 0 while draws go to 224, the display-swap path is the defect.
3. Only then compare the same guest tick against PCSX2 (`abdiff.py mem` / a GS register read).

### Addendum: the actual blocker is the ADXF pump, not the renderer

Measured after the section above, with `TOL_TRACE_DVCI=1` (the guard probe now logs transitions) and
`TOL_TRACE_EE_PC=1`:

```
[adxf-guard] #2    before=0 after=2 pc=0x001AD2D8 ra=0x001AD334   <- ordinary checkpoint bail
[adxf-guard] #7    before=0 after=0 pc=0x0019ED38 ra=0x0019ED38   <- completed (guard cleared)
...
[adxf-guard] #1375 before=0 after=5 pc=0x00198AB0 ra=0x001B6F20   <- dies here, once
[adxf-pump]  #2500 guard=5 blocked=1125                            <- every later call early-returns
```

`FUN_00197940` is a state machine: `DAT_00393208` = 1,2,3,4,5,6 then 0, and the first line of the
function is `if (DAT_00393208 != 0) return;`. At state 5 it calls `FUN_0019f468`, which walks the 40
ADXF stream slots and services each active one. The bail pc `0x00198AB0` is inside `FUN_00198ab0`:

```c
DAT_00394300 = DAT_00394300 - 1;
if (DAT_00394300 == 0) {
    FUN_00199268(DAT_00394358);
    ChangeThreadPriority(GetThreadId(), DAT_0039bbf0);   // <- the host call leaves here
}
```

So the pump was abandoned at that transfer. What is measured is that the guard stays at 5 forever after
it: no later pump call ever clears it, and the sample stream never shows the thread re-entered at
`0x00198AB0` (bails that do resume appear as dispatch entries - `0x001F8168` is sampled 97 times that
way - while `0x00198AB0` is sampled zero times). Whether the thread is parked mid-call or the resume
point was lost is the open question the fix has to answer.

Consequences, all measured:

- the 17th boot-script entry never loads; `[script] advance` stops at index 16 of 48 (stable over
  minutes, both with and without the ADX spin-release workaround, so it is not that workaround);
- `[load] submit #6 ... size=260368 dest=0x005C64C0 -> handle=0x00000305` is the last request, and
  `[adxf-stall] ... state=1 busy=0 done=0 blocks=0 dev=0 devBase=0x0` shows the slot never moves;
- the main thread then spins at ~1.5 guest fps, which is what made the screen look frozen/black.

Thread sampling (`TOL_TRACE_EE_PC`) confirms the main thread is the one spinning: 7016 of 7052
dispatches are `tid=1`, and after the stall its samples sit in `0x0015fb60`/`0x0015fcb0` (a
set/clear-flag pair that republishes handlers) rather than at `0x00198AB0`.

EE interrupt delivery was also counted (`TOL_TRACE_EE_IRQ`) to rule out an interrupt storm: 1800
IRQs, 1656 of them DMAC cause 1 (VIF1 channel), and no handler is registered for it, so they are
dropped rather than looping. Not the cause.

### What to fix next

The pump must survive the transfer: when the scheduler takes a thread out of guest code at
`ChangeThreadPriority` (or at any checkpoint), the guest thread has to resume at the bail pc so the
state machine finishes. Either the resume context is being overwritten while the pump is parked, or
the transfer is being treated as completion. Reproduce with:

```
TOL_TRACE_DVCI=1 TOL_TRACE_SCRIPT=1 build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

and watch for `[adxf-guard] ... after=5`; the first one is the failure. Then confirm the thread's saved
pc for the next dispatch is `0x00198AB0` (or later) and not the renderer loop.

## Session 94 (2026-11-07) - the checkpoint unwarp loses guest state; the loader waits on two daemons

### What the pump stall actually is

The ADXF pump (`FUN_00197940`) is a guest state machine over `DAT_00393208` (1,2,3,4,5,6,0) and
`FUN_00198ab0` writes through `$s0`, which it loads as `0x393208`. Instrumenting every call shows the
register is correct at every call:

```
[pumpcall] target=0x0019F468 guard=1 sp=... s0=0x00393208 ra=0x0019798C
[pumpcall] target=0x0019F468 guard=5 sp=... s0=0x00393208 ra=0x001979BC
```

The fatal event is a **checkpoint bail** inside the chain, and the resumed chain reaches the pump's
continuation `0x1979BC` with `$s0` no longer `0x393208` (measured 1), so the `sw` that would set the
guard to 6/0 writes to the wrong address and the state machine is pinned forever.

The mechanism: a checkpoint preemption at an inter-function `dispatchGuestBranch` leaves the thread
parked at the callee entry with the host call chain unwound; resuming re-enters the callee and walks
back up through every caller's return label. Callee-saved registers survive that walk only while every
frame's saved copy is intact, and in deep chains they are not.

Isolation: `TOL_NO_EE_CKPT=1` makes `EeScheduler::checkpointDue` always false (syscall preemption still
suspends the fiber). With it the pump never leaks again - `[adxf-pump] #2000 guard=0 blocked=9`
against `blocked=2749` before. `TOL_NO_CALL_CKPT=1` restores the old `dispatchGuestBranch` behaviour
(default now defers a call-boundary checkpoint to the callee's own backward edge).

So the pump guard is a **symptom**, and the boot stall at script entry 16 has a second cause.

### The real stall: two loader tasks never finish

`FUN_00164F80` is the loader thread:

```c
lVar2 = (*task)();
FUN_00152030(FUN_001520c0(), lVar2, 0,0,0);
while (FUN_00165420(lVar2) != 0) {   // FUN_00165420 -> FUN_001521c0 -> FUN_00152160
    render(); FUN_001ee060();        // loading screen + script engine
    ...
}
```

`FUN_00152160` scans 64 objects at `0x407160` (stride `0x210`) for one whose `+0x1F0` byte is set and
whose `+0x200` equals the task pointer; `FUN_001521c0` reports "busy" while `+0x1F0 == 1`.

Traced with `[taskfind]`, the loader polls exactly two of them, and both stay busy for the whole run:

```
[taskfind] #1  arg=0x00407580 ret=0x00407580 slot=2 busy=1 id=0x00407580
[taskfind] #3  arg=0x00407790 ret=0x00407790 slot=3 busy=1 id=0x00407790
```

Their names are in the object (`+0x1D0`):

- slot 2 `0x407580`: `#UNKNOWN`
- slot 3 `0x407790`: **`AppCell daemon`**

So the boot is waiting for the game's own "AppCell daemon" (and an unnamed task) to finish. The script
index is genuinely stuck at 16 of 48 (verified past the probes' 16-line logging cap by raising it to
every 500 calls).

### Also measured

- Script handle struct at `0x015230A0` is live: `count=0x30` (48) plus size/pointer fields. Careful:
  the *data* buffer is not valid before the script lands, so dumps taken early read as zeros.
- The load staging buffer `0x005C64C0` is all zero at ~20 s, and the last resource submitted is #6
  (`id=0x4012E`, 260368 bytes, job state 2). No 7th submit happens.
- Guest speed is ~1.5 frames/s (VBlank tick 61 after 40 s), so the loading screen advances ~1.5x/s.

### Probes added this session

| probe | location | what it shows |
|---|---|---|
| `TOL_TRACE_EE_IRQ` | `EeScheduler.cpp` | interrupt counts by cause (ruled out an IRQ storm) |
| `TOL_TRACE_EE_PC` | `EeScheduler.cpp` | dispatch-entry pc sampling (main thread is awake and looping) |
| `TOL_TRACE_PUMPPC` | `EeScheduler.cpp`, `ps2_runtime.cpp` | pump-chain dispatch entries, resumed `$s0`/`$s1`/`$sp`, stack frames, and every call into `FUN_0019F468`/`FUN_00197940` with the guard |
| `TOL_NO_EE_CKPT` | `EeScheduler.cpp` | disables checkpoint bails (diagnostic A/B) |
| `TOL_NO_CALL_CKPT` | `ps2_runtime.cpp` | restores call-boundary checkpoint declines |
| `[taskfind]` | `tol_overrides.cpp` | names the loader's pending tasks and their busy state |

### Next

1. Find what clears `+0x1F0` on the `AppCell daemon` object (0x407790). Its slot holds three function
   pointers at `+0x1B0` (`0x164F40`, `0x164FCC`, `0x165110`); the completion path is what never runs.
2. In parallel, check whether the daemon waits on an IOP service: the run still reports
   `no kernel service for ioman` (25), `loadcore` (23), `sysmem` (6), `sifcmd` (3), `sysclib` (3),
   `thevent` (2), `thbase` (2), `intrman` (2), `sifman` (1).
3. Treat the 1.5 fps guest speed as a first-class defect: even with a working load path, a
   frame-paced boot at that rate takes tens of minutes.

### Addendum: where the 1.5 fps goes

`TOL_TRACE_EE_DISPATCH=1` (the scheduler's own profiling) over 25 s:

```
[eedispatch] 891/s mean=1.107 ms max=17.525 ms (tid=1) over 901 turns
[turnbucket] pc=0x0027B67C 42 turns   718.6 ms ( 71% of the second)
[turnbucket] pc=0x00349D68 126 turns  157.2 ms ( 16% of the second)
[turnbucket] pc=0x0027B45C 42 turns    40.7 ms (  4%)
[eeturn] tid=1 16.73 ms cycles=76128 start=0x0027B67C end=0x003499A8 ra=0x00349868
```

- `FUN_0027b5a0` (entry `0x27B67C` in the sample) runs 42x/s at ~16.7 ms a pass and alone accounts
  for 71% of the host CPU. It loops `0x180 * 2` slots: `FUN_0027b290(i, ch)` probes a slot and, when
  empty, `FUN_0027b390(i, ch, 1)` programs it through `FUN_00349c00(0x10/0x11/0x12, ...)` and
  `FUN_00349770(0x13, ...)`.
- 16.7 ms for ~768 slot probes means roughly 20 us per probe, far more than the guest code itself;
  each probe is a chain of `dispatchGuestBranch` calls, so inter-function dispatch overhead (table
  lookup + checkpoint accounting) dominates the pass.
- 76128 EE cycles are accounted for a turn that takes 16.7 ms of host time, i.e. the host runs ~64x
  slower than EE cycle time on this path.

Consequence: the loader's `while (task busy) { render; engine; }` loop only gets ~1.5 iterations per
second of guest time, so everything the boot waits on crawls.

### Where to attack next, in order

1. The `AppCell daemon` task (slot 3, `0x407790`): find what clears its `+0x1F0` byte. That is the
   direct blocker for script entry 17.
2. Inter-function dispatch cost: it is the 71% item. A faster `lookupFunction`/`checkpointDue` path
   (inline table index, cheaper cycle accounting) would multiply the whole boot.
3. The missing IOP services (ioman 25, loadcore 23, ...) if the daemon turns out to wait on one.

## Session 95 (2026-11-07) - the AppCell daemon traced to a dangling script-engine object

### What the daemon is

`FUN_00164ea0` creates it:

```c
FUN_00151c80(0x164f40, 0x164f80, 0x165110, 4, 0, 0x3c08a0);   // entry, step, term, prio 4, name
```

The name at `0x3c08a0` is **"AppCell deamon"** (the game's spelling). Its step is `FUN_00164f80`, the
loader loop itself, so it is a permanent daemon: `+0x1F0 == 1` is *expected* for it and is not what the
boot waits on. `FUN_00152160(param)` scans the 64 blocks at `0x407160` (stride `0x210`) matching
`+0x200 == param`; slot 2 (`0x407580`) is a real job and cycles `0/1/3/0` normally, slot 3
(`0x407790`) is the daemon and goes to 1 once and stays.

Forcing the daemon's poll to report idle (`TOL_FORCE_TASK_IDLE=1`) changes nothing, which rules the
daemon out as the gate.

### The actual gate: the script engine's object is released and reused

`FUN_001ee060` is the engine's per-iteration entry:

```c
FUN_001edf80();  FUN_001ed2e0();
uVar1 = FUN_001ed0c0();                       // returns the global at 0x3DB320
(**(code **)(*(int *)uVar1 + 0x1c))(uVar1);   // vtable slot 0x1C = "step"
```

Instrumenting the object gives:

```
[engineobj] #1 obj=0x004E9D70 vt=0x003D4B20 step=0x00232110
[engineobj] #2 obj=0x004E9D70 vt=0x003D26D8 step=0x00000000   <- step is NULL from here on
[enginestep] #1 target=0x00232110
[enginestep] #2.. #N target=0x00000000
```

A second virtual call is also seen jumping to `0x3F800000` (float 1.0) - i.e. non-pointer values are
being read out of a pointer table.

The switch is caught at a call boundary:

```
[objwatch] 0x00000000 -> 0x003D4B20 at source=0x001EDCF0 target=0x00231F30   (engine ctor)
[objwatch] 0x003D4B20 -> 0x003D26D8 at source=0x00171088 target=0x00101420   (after this point)
```

`0x3D26D8 == 0x3D26C0 + 0x18`, and `0x3D26C0` is the vtable of the class built by `FUN_00171020`
(`*puVar3 = &DAT_003d26c0`). The heap around the object shows the game's own debug tags:

```
before: 004E9D68: FFFFFFFF 00000000 003D4B20 00000000
        004E9D88: ...                               45455246 ("FREE")
after:  004E9D70: 003D26D8 00000000
        004E9D78: 00020000 00010001 005AA140
        004E9D88: ...                               4D444E52 ("RNDM")
```

So the block the global `0x3DB320` still points at has been released and reused, and the engine then
calls vtable slot 0x1C of whatever now lives there. That is why the script stops advancing at entry 16
of 48 and why the loader keeps rendering the loading screen: every `FUN_001ee060` call is a no-op.

It is not a checkpoint-bail artefact: with `TOL_NO_EE_CKPT=1` the same switch happens at the same
place, and the script still stops at 16.

### Probes added

| probe | location | what it shows |
|---|---|---|
| `TOL_WATCH_TASK` | `EeScheduler.cpp` | task-block `+0x1F0` transitions per guest turn, and every change of the engine object's word 0 |
| `[taskfind]` extras | `tol_overrides.cpp` | caller `$ra` of each task lookup, plus `TOL_FORCE_TASK_IDLE` to report the daemon block idle |
| `[engine]` / `[engineobj]` / `[enginestep]` | `tol_overrides.cpp`, `ps2_runtime.cpp` | engine globals, object vtable and step slot, and the actual target of the `jalr` at `0x1EE0A4` |
| `[objwatch]` | `ps2_runtime.cpp` | the engine object word and its neighbourhood at the call boundary that changed it |

### Where this leaves the boot

Script index 16 of 48, loading screen up, ~1.5 guest fps, and the script engine stepping into a NULL
pointer every iteration. Next: find who releases the block at `0x4E9D70` while `0x3DB320` still points
at it (the game's heap tags "FREE"/"RNDM" are at payload `+0x18`), and whether the release is the game's
own teardown or a consequence of a guest-state difference upstream.

### Addendum: the block is reused by the game's own allocator

Hooking the allocator (`FUN_00101420` -> `FUN_001013b0` -> `FUN_00100700`) shows the engine block handed
out a second time:

```
[heapalloc] size=0x1C  ret=0x004E9D70 ra=0x001EDCD4   <- engine object (FUN_001edc20)
[heapalloc] size=0x14  ret=0x004E9D70 ra=0x00171070   <- FUN_00171020's sub-object, same block
```

and hooking the free thunk (`0x001013c0`, which is the table entry - hooking `0x001008d0` catches
nothing because the thunk jumps to it directly) shows the only frees in that neighbourhood:

```
[heapfree] ptr=0x004E9BD0 ra=0x001666F8
[heapfree] ptr=0x004E9C30 ra=0x00166700
```

So `0x4E9D70` is never freed, yet the allocator re-issues it. That is allocator free-list corruption
(two frees made from `FUN_001664d0`, a data loader that allocates temp buffers with the same heap),
not a game-side release. It is deterministic and unaffected by `TOL_NO_EE_CKPT`.

Next: dump the heap (e.g. `0x4E9000..0x4EA800`) at the moment before and after `FUN_001664d0` runs and
compare it against PCSX2 at the same guest moment (`abdiff.py mem`); a single wrong opcode in the
allocator would corrupt the free list exactly this way.

## Session 96 (2026-11-07) - the engine block is freed from inside its own step

### PCSX2 reference: blocked

PCSX2 was running (`C:\PCSX2-MCP-v1.0.0\pcsx2-qt.exe`, game loaded) and I saved your state to slot 0
before touching it. The first `read_memory` of `0x0015230A0` timed out, then the tool layer hung again
on `evaluate gp`. I stopped there rather than risk the session. **Slot 0 holds your pre-comparison
state.**

Useful data captured before it hung: at `0x0015230A0` PCSX2 had `70 9d 4e 00 f0 14 62 00 d0 9c 4e 00 ...`
and at `0x003DB320` it read `30 00 00 00 ... 00 05 08 00 ... 40 03 00 00 40 08 00 00`, i.e. the same
`0x30 / 0x00080500 / 0x340 / 0x840` shape our port has in the script handle struct. So PCSX2 at that
moment was in a *different* phase than our stall (its `0x3DB320` is not a heap pointer), which is why a
like-for-like heap diff needs a synchronised guest moment, not just the same address.

### In-port heap forensics (this is the useful result)

The game's allocator is `FUN_00100700` (allocate), `FUN_001008d0` (free), with 0x20-byte headers and
ASCII magics: `0x4D514553` "SEQM" fresh, `0x4D444E52` "RNDM" reused, `0x45455246` "FREE" free.
Header: `+0x00` magic, `+0x04` prev physical, `+0x08` next physical, `+0x10` size, `+0x14/+0x18`
free-list links.

Watching the engine block's header (`0x4E9D50`) at *every* inter-function call gives the exact
ordering:

```
[magicwatch] 0x00000000 -> 0x45455246 at source=0x0010082C target=0x00100B20   (allocator internals)
[magicwatch] 0x45455246 -> 0x4D444E52 at source=0x0010082C target=0x00100B20
[objwatch]   0x00000000 -> 0x003D4B20 at source=0x001EDCF0 target=0x00231F30   (engine ctor)
[magicwatch] 0x4D444E52 -> 0x45455246 at source=0x001EE0AC target=0x001F0320   <- freed
[magicwatch] 0x45455246 -> 0x4D444E52 at source=0x001007A8 target=0x00100B70   <- reallocated
[objwatch]   0x003D4B20 -> 0x003D26D8 at source=0x00171088 target=0x00101420   (new class)
```

`source=0x001EE0AC` is the call inside `FUN_001EE060` that follows the engine's virtual step at
`0x001EE0A4`. Because `dispatchGuestBranch` samples *before* it invokes the target, the free happened
**inside the step** (`FUN_00232110`, vtable slot 0x1C), not in the submit call after it:

```c
void FUN_00232110(int p) {
    (**(code **)(**(int **)(p + 0x0c) + 0x1c))();   // sub-object steps
    (**(code **)(**(int **)(p + 0x10) + 0x1c))();
    (**(code **)(**(int **)(p + 0x18) + 0x1c))();
}
```

The engine object's `+0xC` is `0x4E9DB0` and `+0x10` is `0x5AA140`; one of those sub-steps frees the
engine block. Meanwhile the global `0x3DB320` is set once (0 -> 0x4E9D70) and **never changes again**
for the whole run, so from that point the loader steps a freed, re-used block.

Confirmed independent of the scheduler: identical with `TOL_NO_EE_CKPT=1` (checkpoints off).

### Honest state

Script index 16 of 48, loading screen up, engine step NULL, engine global dangling. The remaining work
is to identify which sub-object step frees the parent and whether the parent pointer should be cleared
there - that is a focused question, but I could not finish it in this session.

### Probes added

`[magicwatch]` (block magic at every call boundary) joins `[objwatch]`, `[heapalloc]`, `[heapfree]`,
`[heapfree2]`, `[taskfind]`, `[engineobj]`, `[enginestep]`; `TOL_WATCH_TASK` drives the watches.
Note the free does **not** go through `0x001013c0`/`0x001008d0` as a table call, which is why hooking
those showed nothing - the sampling watch was the way to catch it.

### Session 97 - the engine object is deleted by its own destructor during the first sub-step

Wrapping the call site in `PS2Runtime::dispatchGuestBranch` (magic read before and after the callee)
attributes the heap change to an exact call:

```
[callwatch] src=0x00231E64 target=0x00101440 kind=1 ... magic 0x4D444E52 -> 0x45455246
[callwatch] src=0x00232140 target=0x00231EA0 kind=3 ...
[callwatch] src=0x001EE0A4 target=0x00232110 kind=3 ...
[heapfree2] a0=0x004E9D70 a1=0x00591FA0 ra=0x00231E6C
```

- `0x00101440` is `thunk_FUN_001008d0` - the allocator's free entry (`FUN_001008d0`).
- `ra=0x00231E6C` is the return address of the `jal 0x00101440` inside the destructor at `0x231E20`:

```
0x231E20  addiu sp,sp,-0x20
0x231E2C  move  s0,a0
0x231E30  beq   s0,zero, 0x231E6C
0x231E38  v0 = 0x3D4B00 ; 0x231E44 sw v0,0(s0)     ; vtable = base
0x231E48  v0 = 0x3D4AE0 ; 0x231E50 sw v0,0(s0)     ; vtable = base-base
0x231E54  v0 = (int16)a1 ; 0x231E5C blez v0, 0x231E6C
0x231E64  jal   0x00101440                          ; free(this)
0x231E68  move  a0,s0
```

So `FUN_00231E20(obj, deleting)` is the object's destructor: it walks the vtable to the base class and,
when `deleting > 0`, frees the block. It is reached only through a vtable slot (its only xref is the
DATA word at `0x3D4B08`, i.e. the destructor slot of the base vtable at `0x3D4B00`), which is why
hooking its entry with `replaceFunction` fires nothing - the call resumes at a label inside it.

Ordering puts the delete **inside the first sub-step** of `FUN_00232110`:

```c
0x232120  a0 = [s0 + 0x0c];  jalr [*a0 + 0x1c]     // sub-object 0x4E9DB0  <- delete happens here
0x232134  a0 = [s0 + 0x10];  jalr [*a0 + 0x1c]     // sub-object 0x5AA140 (this step is a bare jr ra)
0x232148  a0 = [s0 + 0x18];  jalr [*a0 + 0x1c]
```

`[callwatch] src=0x00232140 target=0x00231EA0` is the second (no-op) step, so the free logged just
before it belongs to the first sub-step's subtree.

Consequence, unchanged from Session 96: the global `0x3DB320` still points at the freed block, and the
next iteration steps vtable slot 0x1C of whatever now occupies `0x4E9D70` (NULL).

Next question is now narrow and concrete: what does the first sub-object (`0x4E9DB0`, stored at
engine+0x0C) do that destroys its owner, and is the engine supposed to be on that object's list at all.

## Session 98 - single-snapshot heap diff against PCSX2 (cheap path)

One 768-byte read of PCSX2 at `0x4E9D40` (running, no parallel calls) against the port's dump of the same
range taken at the first `FUN_001EE060` call (`TOL_DUMP_ENGINE=<path>`).

**Confirmed differences:**

| address | port | PCSX2 | meaning |
|---|---|---|---|
| `0x4E9D50` | `46 52 45 45` "FREE" | `52 4E 44 4D` "RNDM" | engine block freed by us, alive there |
| `0x4E9D64` | `00�`/`70 A5 5A 00` | `FF�`/`FF�` | free-list links written on our side only |
| `0x4E9DC4` | `01` | `00` | renderer step guard (`FUN_00230a50` gate) |
| `0x4E9DBC` | `0x4EA090` | `0x53DE20` | renderer buffer[1] pointer |
| `0x4E9DC0` | `0x53DE20` | `0x4EA070` | renderer buffer[2] pointer - **swapped, and one address differs** |
| `0x4E9DE8/EC/F1` | `00 00 00` | `80 02 02` | renderer fields past the 4 sub-allocations |
| `0x4E9EF6�` | `0x447A0000`, `0xBF800000` | `0` | renderer constants only we have |
| `0x4E9F00�` | `0` | `0x43A50000`, `0x3F23D70A` | renderer constants only PCSX2 has |

**Reading it honestly:** the guard difference alone is not conclusive. PCSX2's renderer object has
`+0x08 = 2`, both buffer pointers set and the fields past `+0x34` populated, but `+0x14 = 0` - that is
exactly the state of `FUN_00230810` *part way through its body* (it sets `+0x14 = 1` only after the
`0x18000` allocation). So PCSX2 was sampled mid-constructor and would very likely set the guard to 1 as
well. The two buffer pointers being swapped and pointing at different addresses
(`0x4EA090` vs `0x4EA070`) means the **heap layout already differed before the engine step**, i.e. the
divergence is upstream of the delete, not in the guard.

So the next target is the earliest allocation-order divergence in boot, not the renderer step.

### State

PCSX2 was paused once and resumed; the user's state is in slot 0. The MCP wedges when two reads are
issued in parallel - use one read per call from now on. Port unchanged functionally; `TOL_DUMP_ENGINE`
added for the snapshot.

## Session 99 - PCSX2 reference settled with watchpoints (restart authorised)

PCSX2 was restarted from `data\Tales of Legendia (USA).iso` (its original command line had no arguments).
A window capture confirmed it boots to the **title screen with the Tales of Legendia logo** within
seconds, so it is a valid reference.

**Watchpoint on the engine block header `0x4E9D50` (write/break) across a full boot: 0 hits**, and at the
title screen the magic still reads `RNDM`. PCSX2 allocates the engine block at `0x4E9D70` and **never
frees it**.

**Guard check:** `0x4E9DC4` reads **1** in PCSX2 at the title. (The earlier reading of 0 was stale - a
repeat read on the same instance now returns 1.) So the renderer step guard matches the port and is not
the differentiator either.

### Conclusion

Both emulators have the guard set and both run `FUN_00232110`; only the port deletes the engine, and the
delete comes from the first sub-step (the renderer object at engine+0x0C = `0x4E9DB0`):
`FUN_00230a50` -> `FUN_00230ad0` / `FUN_00230c80` / `FUN_00230d90` / `FUN_002314a0` -> ... ->
deleting destructor `FUN_00231e20(obj, 1)` -> `free(0x4E9D70)` at `ra=0x231E6C`.

So the bug is inside what that renderer step walks, not in the guard and not in the allocator.

Next: compare the list/state the renderer step traverses at the first step (the port deletes the engine
there; PCSX2 does not), i.e. the pointers inside `0x4E9DB0` and whatever `FUN_00230ad0`/`FUN_00230d90`
walk.

## Session 100 - the third sub-object researched

The engine's third sub-object is `*(engine+0x18) = 0x5AA170` (allocated by `FUN_001edc20` as a
`0x3F8` block through `FUN_0032f3a0`).

- Static constructor `FUN_0032f3a0` sets `*obj = 0x3D9168`, then calls `FUN_0032f440`.
- **At runtime the object's vtable is `0x3D50D0`** (read from PCSX2 at `0x5AA170`), not `0x3D9168` -
  the follow-up initialiser replaces it.
- Vtable `0x3D50D0` slot `0x1C` = **`0x002853d0`**, the third step.
- `FUN_002853d0` = `FUN_00285410()` + `FUN_002855e0(p)` + `FUN_002854a0(p)` + `FUN_002857c0(p)`.
- `FUN_002855e0` is a media/stream update: it gates on `FUN_00197668()` (stream state), checks the
  fourcc `0x53504C54` and copies frames through `FUN_00108f50` / `FUN_001a0470`.
- `FUN_002857c0` walks `p+0x08` buffers calling `FUN_00292800` / `FUN_00292810`.

So the engine's three sub-objects are a renderer (`0x4E9DB0`), an unnamed object (`0x5AA140`) and a
media/stream player (`0x5AA170`). Nothing in the third step obviously deletes the engine, and the call
ring still records no third call - the `free` is logged between the second (a `jr $ra` stub) and the
third.

Open: how `FUN_00231E20` is entered at all. Neither `dispatchGuestBranch` (ring/dtorcall) nor the
scheduler dispatch (dtorentry) records a transfer to `0x231E20`, yet its body runs
(`free` at `ra=0x231E6C`).

## Session 101 - cheap steps: a real 2.5x speedup, and the "script" is a MIX container

### Performance fix (committed)

`dispatchGuestBranch` called `std::getenv` **per guest call** (for TOL_TRACE_SCRIPT / TOL_WATCH_TASK /
TOL_TRACE_PUMPPC - instrumentation I added). Hoisted them to `static const bool`, evaluated once.

Measured with the scheduler's own profiler:

| build | dispatches/s | mean turn |
|---|---|---|
| before probes (session 93) | 891 | 1.107 ms |
| with per-call getenv probes | 480-505 | 1.97-2.06 ms |
| **after hoisting** | **1535-1569** | **0.62 ms** |

So the probes alone had halved the emulator, and the fix leaves it ~2.5x faster than even the original
build. VU1 timing probe added too: `m_vu1.execute` is **never called** during the loading phase, so VU1
interpretation is not the hot path.

### The "script" is a MIX container, not a boot script

`FUN_001EE060` -> `FUN_001EDF80` (16-slot loop) -> `FUN_001ECBE0`, which dispatches on the fourcc at the
chunk head:

```
0x7158494d "MIXq"  0x6558494d "MIXe"  0x6358494d "MIXc"  0x7258494d "MIXr"
```

The handle at `0x15230A0` (count 48) is a MIX command stream, and the 16 advances are 16 MIX chunks
processed inside the *first* engine call. After that the stream is never read again, and the presented
frame at 120 s (frame_003612) is pure black (max=0).

Opcode/advance order, all inside engine call #1: indices 0..15 read opcodes
0,1,2,3,4,5,0x0B,0x0C,0x0D,0x0E,0x0F,0x2A,0x10,0x11,0x12 and advance 16 times; reader status stays 0
("ready") on every sampled call.

### Still true

- Refusing the engine free does not unblock the boot (delete is a side effect).
- PCSX2's handle at 0x15230A0 is byte-identical to ours.
- 120 s run: 16 advances, 15 engine calls, 5 screenshots, last frame black.

## Session 102 - audio/MIX traced: the audio side is healthy, the stall is on the EE

30 s boot with `TOL_TRACE_IXAUDIO=1` and `TOL_TRACE_SPU2=1`:

- IxAudio starts (`[iop] started IxAudio at 933632`, `IxAudio Ver 1.0.0.7`).
- Its RPCs are answered in sequence: fn 0x2, 0x400, 0x403, 0x408, 0x40A, 0x40C, 0x422 with increasing
  reply ids (0x12, 0x13, 0x14 ...), so the audio RPC channel is alive.
- SPU2 register writes flow (185 `write` lines) and the module reports its memory layout
  (`ManagementMemory[S]/[E]`, `EffectWork[C0]/[C1]`).
- `STATX` (0x344) is read 37+ times, always `0x0080`, from **IOP pc 0x00049084**. That is AutoDMA:
  `dmaWrite`/`dmaRead` set bit 7 after every block, so the bit is re-set continuously while the driver
  streams. Clearing it on read changes nothing (tested), and the same value comes back. It is the normal
  IxAudio polling loop, not a stall.

So: **audio and MIX are not where the boot stops.** The MIX stream at 0x15230A0 really is read 16 times
and then not again, but the EE loader - not the audio driver - stops asking for more.

Narrowing done this session:

| hypothesis | verdict |
|---|---|
| engine object delete blocks the boot | no (refusing the free changes nothing) |
| per-call `getenv` in dispatchGuestBranch | yes, real - fixed, ~2.5x faster (0.62 ms/turn) |
| VU1 interpretation is the hot path | no (`m_vu1.execute` never called in this phase) |
| SPU2 STATX bit 7 spins | no (IOP-side normal AutoDMA polling) |
| Grandia as an audio reference | not useful for STATX (its runtime is HLE-level, no core regs) |

## Session 103 - MIX buffer checked: complete, not truncated

Dumped 528 KB (`0x1523010..0x15A4010`) during the read burst (peek #8) with `TOL_DUMP_MIX`.

The container header at `0x15230A0`:

```
+0x00 count = 48
+0x08 0x00080500   +0x14 0x340   +0x18 0x840
+0x20 48 entries of 16 bytes { id, offset, size, pad }
```

Entries: ids `1,2,3,4,5,...,0x30,0x31,0x32` (last slot empty), e.g.

```
id=0x01 off=0x0B80 size=0x10     id=0x05 off=0x0FC0 size=0x12D60
id=0x31 off=0x7F800 size=0xC68   id=0x32 off=0x80480 size=0x6C
```

Validation:

- **max end offset = 0x804EC**, and the loaded file is 0x131B50 (1,252,176 bytes) - every chunk lies
  inside the buffer, **no entry is truncated**.
- Data verified present at the far end: the window at 0x15A3410..0x15A3810 is 453/1024 non-zero, and
  the last chunk at 0x15A3490 reads `48 FF 02 D8 1D 20 23 FF ...`.
- `0x80500` in the header matches the table's max end (the bank's total size).

So the 16-of-48 stop is **not** missing data. The reader simply stops asking.

Note: a full scan of the 528 KB that covers every chunk finds **zero "MIX" fourccs**, so the
MIXq/MIXe/MIXc/MIXr dispatch in FUN_001ECBE0 belongs to a different object (the slot dispatcher), not to
this bank. The bank is the boot-script/data container, and it is complete in memory.

## Session 104 - the 16-slot loop located; the producer stops refilling it

`FUN_001EDF80` (the "slot loop") iterates **16 slots at `0x4A5840`, stride `0x24`** (9 words), not the
object I had been reading. Per slot:

```c
if (slot[0] == 1) {                 // 1 = pending, 2 = dispatched
    if (ready(slot[1], slot[2])) {
        slot[0] = 2;
        FUN_001ecbe0(slot);         // the MIXq/MIXe/MIXc/MIXr dispatcher
        FUN_001ec5b0(slot);
    }
}
```

Live trace (11 slot-loop calls in 25 s, script still 16):

```
#1  [0] 00000000 ...                                  (empty)
#2  [0] 00000001 00000000 00000101 00000000 005C64C0  (pending, cmd 0x101)
#3  [0] 00000002 ...                                  (dispatched)
#4  [0] 00000001 ... 00000182 ... 005C64C0            (pending, cmd 0x182)
#5  [0] 00000002 ...                                  (dispatched)
```

So only slot 0 is ever used, it cycles `1 -> 2` once per call, and **the producer stops writing
`slot[0] = 1` after 16 commands**. Nothing downstream refuses them: the bank is complete (session 103),
the reader reports ready, audio is healthy (session 102).

Next cheap step: watch `0x4A5840` (slot[0]) and log the guest pc that writes `1` - that names the
producer and shows why it stops after 16.

## Session 105 - the boot loader chain identified (FUN_001EC670)

Watching `slot[0]` at `0x4A5840` names the producer: every `0 -> 1` transition comes from a turn starting
in `FUN_001EC670` (`0x1EC6DC`, `0x1EC6E4`, `0x1EC74C`, ...), with `ra=0x151F88` (it is pumped by
`FUN_00151ff0(0)`), then `1 -> 2` dispatches it and `2 -> 0` clears it. Exactly **16** such cycles.

`FUN_001EC670(param)` is the boot loader's sequential chain:

```c
FUN_0027bf20(0); FUN_0027c2e0();
lVar2 = FUN_001ec9a0(0, 1, 1);
if (param_1 && lVar2) while (FUN_0027bff0(lVar2) == 0) { FUN_00151ff0(0); FUN_0019a080(); FUN_001ee060(); FUN_001392d0(); }
... (1,2,1) (2,3,1) (3,4,1) ...
FUN_0027bf20(1);
lVar2 = FUN_001ec9f0(8, 0x4012e, 1);
if (param_1 && lVar2) { while (FUN_0027bff0(lVar2) == 0) { ...pump... } thunk_FUN_0027c360(lVar2); }
FUN_0027bf20(0);
return;
```

So the six `[load] submit` entries seen in the logs are exactly this chain (0->1, 1->2, 2->3, 3->4, then
resource 8 = `0x4012E`), each pumped by `FUN_00151ff0(0)` + `FUN_0019a080` (IO) + `FUN_001ee060`
(engine) + `FUN_001392d0` (load tick).

Each of those pump loops is where the 16 slot cycles come from, and the load probe showed every job's
`FUN_0027bff0` returning 2 (done), so the loader chain itself completes.

Next cheap step: after `FUN_001EC670` returns, the loader (`FUN_00164F80`) pops the next task from
`0x497010` / `puRam003db1f8`. Dumping that list at the stall shows whether the next task is queued at all
- i.e. whether the boot script is supposed to queue it and does not.

## Session 106 - the loader's task queue is empty at the stall

Dumping the loader state at engine calls 2-4 (`TOL_DUMP_LOADER`):

```
[loader] #2 globals 0x3DB000: 00000001 00000001 00000000 00000000 00000001 00000001 00000000 ...
[loader] #2 table   0x497010: 00000000 00000000 00000000 ... (all zero)
[loader] #3 ... identical
[loader] #4 ... identical
```

`FUN_00164F80` pops the next task from the table at `0x497010` (count at the gp-relative `0x3DB014`,
current-task pointer at `0x3DB018`). **The table is entirely zero and the globals never change**, so the
loader is running its fallback task (`LAB_0016C700`) - there is no queued work.

That lines up with everything else measured:

- the six loads of `FUN_001EC670` completed (each `FUN_0027bff0` returned 2),
- the script/data bank is complete in memory (48/48 chunks, session 103),
- the engine's sub-objects are zeroed after the first call, so the 16-slot loop has nothing to consume,
- audio and the IOP are healthy (session 102).

So the stall is now pinned to one question: **which guest path is supposed to enqueue the next loader
task, and why does it not run?** Nothing downstream refuses work; nobody asks.

Also fixed a self-inflicted problem: a careless replace had copied the loader dump into ten probes; it is
back to exactly one, in the engine probe, outside the renderer check.

## Session 107 - there is no enqueue to trace; the real scheduler is the 64-task table

Watching the loader's queue count (`0x3DB1F4`) showed exactly **one** transition:

```
[queuewatch] count 4294967295 -> 0 turn start=0x00100008   (initial)
```

It is **0 from reset and never changes**. `0x497010` (the function queue `FUN_00164F80` pops from) is
never written - the loader always takes the empty-queue path and runs `&LAB_0016C700`, which is a
trampoline `j 0x00204900` -> `FUN_00204900` -> `FUN_002045f0`: allocate a 0x18 object, set vtable
`0x3D4770`, then `FUN_00151db0(6)`.

So my session-106 wording ("nothing is queued") was a misread: there is no enqueue path in use at all.
The real scheduler is `FUN_00151db0(prio)` / `FUN_00151ff0(prio)`, which walk **64 task blocks at
`0x407160`, stride `0x210`**:

- `+0x1F0` byte: active
- `+0x1F1` byte: priority
- `+0x1D0`: name (inline ASCII)
- `+0x200`: task id

Live at the stall (engine calls 2-4, identical):

```
[tasks] [0]=CCDVD_MCD_MANAGER::Deam(p0) [1]=CCDVD_MCD_MANAGER::Deam(p0) [3]=AppCell deamon(p4)
```

Only three tasks exist, and the loader loop runs `FUN_00151ff0(7)`, `(9)`, `(8)` - **priorities with no
registered tasks** - which is why the engine has nothing to do.

Next cheap question: which tasks should occupy priorities 7/8/9, and did they exist earlier and exit?

## Session 108 - full 64-task trace from boot: priorities 7/8/9 never exist

`TOL_TRACE_TASKS=1` logs the active task set at `0x407160` whenever it changes (index, inline name,
priority). 618 changes in 25 s. Timeline:

```
[tasks] turn=0x00100008 active=0:
[tasks] turn=0x00165B14 active=2: [0]CCDVD_MCD_MANAGER::Deam(p0) [1]CCDVD_MCD_MANAGER::Deam(p0)
[tasks] turn=0x001F8150 active=3: ... [2]#UNKNOWN(p0)
[tasks] turn=0x0015201C active=3: ... [2]#UNKNOWN(p10)
[tasks] turn=0x001F8168 active=4: ... [2]#UNKNOWN(p10) [3]AppCell deamon(p4)
   ... then a steady cycle of [2]#UNKNOWN appearing as p4 and p10 ...
[tasks] last: active=3: [0]CCDVD(p0) [1]CCDVD(p0) [3]AppCell(p4)
```

**Priorities observed across the whole run: p0 (1235), p4 (818), p10 (206). Priorities 7, 8 and 9 never
appear at all.** So the loader's `FUN_00151ff0(7)`, `(9)`, `(8)` calls are generic scheduler pumps that
are vacuous in this game - they are not evidence of a missing task. That hypothesis is dead.

Distinct tasks seen: `CCDVD_MCD_MANAGER::Deam` (slots 0 and 1), `#UNKNOWN` (slot 2), `AppCell deamon`
(slot 3). The only activity at the stall is slot 2 flipping between priority 4 and 10, which is the
loader's own job being reprioritised as it is dispatched.

So the task landscape is stable and sane; the stall is not a missing task registration.

## Session 109 - slot 2 does NOT keep the loader busy

Hooked the loader's poll (`FUN_00165420` -> `FUN_001521c0` -> `FUN_00152160`) and logged every change of its
(job, result) plus the task lookups:

```
[poll] #1 job=0x005C6490 busy=0          <- the only poll of the whole 25 s run
[poll] (no further lines: job/result never change, %500 never reached)
```

So `FUN_00165420` is called **once**, the job is `0x005C6490`, and it reports **not busy (0)**. The loader's
inner `while (poll != 0) { render; engine; }` therefore exits immediately - **slot 2 is not holding the
loader**.

The `[taskfind]` traffic (43 hits) comes from other callers, named by `ra`:

- `ra=0x15205C` - inside `FUN_00152030` (bind params to a task block)
- `ra=0x151D00` - inside `FUN_00151cf0` (the per-task runner)
- `ra=0x1521D0` - inside `FUN_001521c0` itself (the poll wrapper), one hit with `ret=0` (block not found)

and those lookups do see slot 2 (`0x407580`) and slot 3 (`0x407790`) with `busy=1` - but they are
parameter/runner traffic, not the loader's wait.

Conclusion: the loader is not spinning on slot 2 or on the daemon block. Its single job completed, and the
loader loop is not what is blocking the boot.

## Session 110 - the loader's frame loop never runs at all

Hooked every call in the loader's inner loop and logged them in order:

```
[loop] ff0(0) x108   [loop] ff0(1) x98   [loop] ff0(4) x94
[loop] e3b0: 0       [loop] d230: 0
```

- `FUN_00151ff0` is called only with priorities **0, 1 and 4** - never 7, 9 or 8.
- `FUN_0016e3b0` and `FUN_0016d230` are **never called**.

The loader's loop is:

```c
lVar4 = FUN_00165420(lVar2);          // poll: returned 0 (session 109)
while (lVar4 != 0) {
    FUN_0015231c();  FUN_00151ff0(7); FUN_00151ff0(9); FUN_00151ff0(8);
    do { FUN_00151ff0(6); } while (...);
    FUN_0016e3b0();  FUN_001ee060();   FUN_0016d230();
    lVar4 = FUN_00165420(lVar2);
}
```

With the poll returning 0, **the entire body is skipped**. The game never enters its per-frame
render/engine loop after the asset chain finishes - which is exactly why the presented frames are black
(session 92/103) and why the engine is only ever called from `FUN_001EC670`'s load pump.

So the stall is upstream of everything we have been chasing: the job handed to the loader (`0x5C6490`)
reports "not busy" immediately instead of staying busy for the lifetime of the main loop.

Note also the context: the AppCell daemon was created with
`FUN_00151c80(0x164f40, 0x164f80, 0x165110, prio 4, 0, "AppCell deamon")` - its *step* is
`FUN_00164f80`, the loader loop itself, and `FUN_00151ff0(4)` is what runs it. The `ff0(4)` traffic is
the daemon being stepped.

Next: find what sets the state of job `0x5C6490` (the poll `FUN_0027bff0` reads it) and why it is
already "done" on the first poll.

## Session 111 - the loader's job key is slot 2, and slot 2 is idle at the first poll

Dumping the job object and searching the task table for its key:

```
[poll] job=0x005C6490 busy=0
[poll]   job words: 00407580 00000000 00000040 41504144 003D4770 00000003 (key=0x00407580)
[poll]   blocks with +0x200 == key: [2]#UNKNOWN active=0 prio=10  ret=0x00000000
```

So:

- The job's first word is **`0x00407580`** = the block address of **slot 2 (`#UNKNOWN`)**.
- Slot 2 exists and its `+0x200` matches the key exactly.
- But its `+0x1F0` active byte is **0** at that moment, so `FUN_001521c0` returns 0 and the loader's
  `while` body (task priority 7/9/8/6 runs, `FUN_0016e3b0`, `FUN_001ee060`, `FUN_0016d230`) is skipped.

The job object itself is a task object: vtable `0x3D4770` (the same one `FUN_002045f0` assigns), field
`+0x08 = 0x40`, `+0x0C = "DAPA"` fourcc, `+0x14 = 3`.

So the loader's frame loop is gated on slot 2 being active, and at the first poll slot 2 is idle. That is
the last link: **the game never renders a frame because the task it waits on was already idle when it
polled.**

Session 108's trace of the same block shows slot 2 does become active (`active` flips, priority 4 <-> 10,
~618 changes in 25 s) - but the loader only polls once, so it never observes that activity.

Refinement (measured, not inferred): the poll probe now also emits a `repeat` line every 200 calls. None appeared, and
the only state line is `#1`, so `FUN_00165420` is called **exactly once** in 25 s - the loader never reaches the poll
a second time, even though `FUN_00151ff0(4)` (the AppCell daemon step, `FUN_00164f80` itself) fires 94 times. So the
loader re-enters its body and returns before the poll on almost every entry; the single poll it does make reports idle.
