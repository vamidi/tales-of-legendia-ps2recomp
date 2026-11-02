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

With `TOL_TRACE_IOP_THREADS=1`, LIBSD and IxAudio both finish their entries and create worker threads (ids 4,5 and
8,9,12), but neither registers `0x90000200`. Giving the IOP up to ~170 ms of extra time (8 x 1024 frames, driven by
the EE call because it is the only clock) does not produce the registration, so the provider is not merely late -
its init never gets there.

Note: `libdbc: Module version mismatch [libdbc.a = 3.10, dbcman.irx = 3.20]` is the game's own warning and is not
fatal - execution continues past it.
