# Tales of Legendia — native PC port (state)

> Fresh start, 2026-11-01. The previous 59-session history is archived at
> `docs/state-archive/PS2_PROJECT_STATE_2026-11-01_sessions01-59.md`.
> Everything below described the **new** tree only.

---

## CURRENT STATUS - updated 2026-11-25 (session 201; READ THIS FIRST, then jump to *Session 201* at the bottom)

> **Session 201 (latest) - the mode-1 tail is NEVER entered: the fade thread is starved after 64 yields.
> The IPU is therefore unexercised, but it is implemented, wired and self-tested.** Traced with new probes:
> `[unitwait] resume` at `0x2043F0` fires exactly **64** times in every run (90/116/146/150/180 s) and then
> stops; `TAIL-ENTRY` at `0x204400` is **0**, `[tail] RETURN` is **0**, `[scene] 285E80` is never called from
> the tail and `[fade] #2` never arrives. The loop body is innocent (`0x1520C0` 64 in / 64 out, `0x1520F0`
> 64 in / 64 out, `0x153010` never called). The thread is TCB **0x4079A0** (prio 4): after the 64th yield it
> is still `state=1` (ready) and still linked (`head 0x407160 -> 0x407370 -> 0x407790 -> 0x407580 ->
> 0x4079A0 -> 0`), not deleted and not blocked - the guest run loop simply stops dispatching it, so the port
> idles (guest work 0.3 % of wall, paceWait 80-91 %). IPU: **0** register writes/reads and **0** DMA starts
> in 150 s (only `DVCI: "\AFS\MOVIE.AFS" found` from the boot AFS index), but `TOL_IPU_SELFTEST=1` proves the
> decoder answers the guest contract (`cmd=0x00010001 busy=0 bp=1 commands=1`). Next single measurement:
> whether two TCBs resolve to the same context block (`0x49F590 + *(0x3EB1A0) + *(0x3EB1C0)`). Details:
> *Session 201*.

> **Session 197 (latest) - the boot sequence is 3 logo CARDS (2D) then an MPEG-2 opening movie + video title.**
> Reference boot captured frame by frame (`logs/refseq2/montage.png`): NOW LOADING (5-11 s) -> **namco** (13.6 s)
> -> **moth Productions** (18 s) -> **Sofdec + IBM** (22.5 s) -> **opening movie** (28-39 s) -> **TALES OF
> LEGENDIA title** (40.6 s onwards). The three "units" (`FUN_00204240` modes 1/2/3) are the three logo cards; the
> title is at ~40 s, not ~21 s. The Sofdec/IBM card proves it is streamed video, and the reference title pauses in
> `FUN_00129258` reading `IPU_CMD`/`IPU_BP`/`IPU_TOP`. `AFS\MOVIE.AFS` is 22 standard MPEG-2 program streams. The
> game imports no `sceMpeg*`/`sceIpu*`, so it drives the IPU hardware directly (`entry_0012dec4` = the IPU/DMA
> driver; `FUN_00129258` issues `IPU_CMD = 0x30000000` (VDEC) and consumes the hardware's per-command macroblock
> count and `IPU_TOP`). The port initialises the IPU at 51.7 s and then never feeds or decodes a single frame
> (`transfers=0 tex builds=0 gsw=0 gif=5`). Two INDEPENDENT gates, both measured: **(A) the three logo cards
> are 2D LZSS images + fades, not video** - the IPU is idle at 13-22 s, so the namco/moth/Sofdec cards are the
> original session-158 "no textured draws" defect and are the cheapest next win; **(B) the opening movie and the
> title background are IPU video** (`FUN_00128408` starts at ~27.8 s at ~20 pictures/s). A complete PS2 IPU with
> real MPEG-2 VLC tables already exists on this machine
> (`D:\Personal\C++\grandia-ps2recomp\runtime\src\hw\ipu.cpp` + `mpeg_tables.h`) and its integration surface is
> mapped. Staged plan, register contract and correction: *Session 197* (197.3, 197.7, 197.9).

> **Session 158 - PCSX2 is finally a usable reference, and the port matches it at the scene start.**
> The MCP wrapper is broken for memory/disassembly (lagged reads, `hexdump` dead) - use `tools/ps2dbg.py` and
> `tools/pcsx2_trace.py`, which talk to the DebugServer's real protocol (newline JSON over TCP 21512). Reference
> boot: unit1 @6 s -> **scene start @11 s** (`FUN_00285E80(a0=0x5AA170, a1=4, a2=0xE0)`) -> unit2 -> unit3 ->
> title @~21 s. The port issues the **same calls with the same arguments**, and the scene heap object matches the
> emulator field for field (four idle entities - which are *not* the title). Two real port defects remain:
> the loader units take ~50 s each instead of ~5 s (the fade yield costs ~0.66 s), and the scene page (280)
> receives **only untextured black** draws - zero textured draws target it, so the copy to page 0 carries black.
> Next: dump `0x5AA000..0x5AD000` in PCSX2 at the `0x285E80` breakpoint and diff it against the port's
> `logs/ramdump/port_scene.bin`. Full detail in *Session 158* at the bottom.

> **Session 156 (2026-11-20) - the presenter now scans out what the GS draws.** `src/gfx/backends/sdlgpu/sdlgpu_backend.cpp`
> retargets the read circuits from DISPFB2 onto a drawn page before any `readCircuit`, and honours DISPFB2's own page
> (including **FBP 0**, a real VRAM page this title copies into) when that page has GPU content. Measured: the loading
> screen is now read from the drawn page (`[prep-out] displayFbp=224 sourceFbp=224 512x448`, 41/41 captures 512x448
> non-black, `[badpc]` 0). GS private-register trace: **PMODE = 0x66 (EN1 off, EN2 on)**, DISPFB1 untouched at the runtime
> default and must be ignored, DISPFB2 = 0x1000 (FBP 0 / FBW 8 / CT32), DISPLAY2 = 0x001BF9FF0203227C. The guest's own
> display path is: draw into FRAME 224/280, then copy it into page 0 with 20 vertical 64x448 sprite strips
> (`tme/abe/fst=1`, `tbp0=7168`, RGBA 128/128/128/128, alpha 0x64/0x40) and scan out page 0. With
> `TOL_FIBER_PREEMPT=1` the guest now runs 99.6 % busy (`mainIters` +13/s) and the screen goes black at 640x448
> **because the guest draws 112,380 black fills itself** (`rgba=0,0,0,0`, `tme=0` into page 224, then copies them to
> page 0) - presentation is faithful, the missing content is guest-side. Details in *Session 156 - RESULT* at the bottom.

> **Session 154 (2026-11-18) - read the Session 154 section at the bottom before acting.** It traces what leaves pc = 0x1B4458 pending (the normal checkpoint/resume path), shows the row frame is intact except its $ra word because that word is the shared $ra slot of every 0x20/0x40-byte frame at that stack depth, and fixes the epilogue per-address (validate $ra with hasFunction, otherwise substitute 0x1A1548 and repair). [badpc] is widened to any non-dispatchable pc, which exposed that the same class still kills the thread one level up (pc = 0x0049C790): a whole activation frame chain can be recycled before the pending return point is resumed, so the durable fix belongs in the resume path (section 154.5).

**Session 152 (latest) — `0x3059B0`, the AppCell child step.** `entry_003059a0` (0x3059A0..0x3059D0) is
emitted with **no `switch (ctx->pc)` at all** (it starts `ctx->pc = 0x3059a0` and runs
`lw t9,(a0); lw t9,0x28(t9); jr t9`), so the phase machine's every-step call to `child->vtable[0x10]`
(= `0x3059B0`) executed `vtable[0x28]` instead of the real thunk `li a2,1; lw t9,0x18(t9); jr t9`. Fixed with
`childStepEntryFix` in `src/runtime/tol_overrides.cpp`. Measured effect: `[leafstep]` (the tree leaf's step
`0x301B00`) goes **0 → 1 call**, the leaf now receives `a1 = 0x63DA00` exactly as PCSX2 does, `app+4` becomes
`0x63DA00` (was permanently 0), and the app advances from `outer=1 / phase=3` to `outer=4 / phase=0`. The
earlier `[childstep] ret=0x63EB00 "success"` reading was the wrong method's return. No regression: 15/15
game-visible classes identical to the pre-fix baseline.

**Session 152 (earlier) — `0x125A78`, the CRI op-channel SIF end function.** The op-channel stall was an
**interior-entry defect**, not a lost continuation. `entry_00125a64` (0x125A64-0x125B48) emits
`switch (ctx->pc)` cases for its own branch targets only (`0x125ac4/0x125b08/0x125b18`); the CRI op
channel's **SIF end function `0x125A78`** is dispatched as an RPC completion inside that row, so the dispatch
restarted the row at `0x125A64` — the tail of another function (`ld ra,0x10(sp) / ... / jr $ra`) — and with the
invocation's `$ra = 0` it left `pc = 0` and parked the thread. The end function's whole body is
`lui v0,0x39 / lw a0,-0x72c8(v0) / j 0x114710`, i.e. `SignalSema(*(0x388D38))`, so the completion semaphore was
never signalled: `FUN_00233FC0` spun `poll(0x126248) -> yield(0x15231C)` forever and `FUN_00126248` never
dequeued. Fixed per-address in `src/runtime/tol_overrides.cpp` (`criEndFunctionFix`, installed by
`installCriEndFunctionFix`).

Measured effect (60 s): `[g248]` went from `ret=0/0xFFFFFFFF` forever to **`ret=1`, twice**; the dequeue
succeeds and the main thread leaves the op-channel spin for the per-frame loop. **The boot still does not reach
the title screen**: the app job now parks at `0x3661DC` (a0=1.0f, `[order] yield ra=0x003661DC`), and one
pre-existing `jalr`-to-null with `a0=0xFFFFFF81` (-127) at `0x301EB8` remains (present before the fix, 1 per run).

**Goal:** boot *Tales of Legendia* to its title screen and render it in a normal window.

**Where it stands:** the game boots, initialises the IOP, loads its nine IRX modules, indexes all six AFS
archives, runs its own file layer, completes the boot-script asset chain and streams real scene data
(largest observed reads: 544,768 / 522,240 / 389,120 / 262,144 bytes). The AppCell loader daemon runs, its
per-frame loop runs (`[poll] busy=1`, `[prio4]` steering it), the frame loop's first iteration completes, and
the script-engine object survives. The screen still shows only the early `NOW LOADING` frame, and everything
now waits on the loader's job chain (`0x63D850 -> 0x63D890 -> 0x4079A0` reports `busy=1` forever) and on the
MIX chunk producer that stops after 16 of 48 (see *Current blocker*).

**Systemic defect (the one thing to fix properly):** the corpus was generated from a Ghidra map whose rows
sometimes merge several small routines. The emitted prologue `switch (ctx->pc)` only contains the row's internal
branch/call targets, so **dispatching at an interior entry point restarts the row at its start** and runs the
wrong code (often the previous function's epilogue, returning a garbage `$ra`). This has caused every
main-thread death so far. Fixed per-address so far, in `src/runtime/tol_overrides.cpp`:

| address | what it really is | override |
|---|---|---|
| `0x139690` | shared clamp-setter epilogue (`jr ra; sw a0,-0x7b44(gp)`), missing table slots | `replaceFunction` lambda |
| `0x1F8150` / `0x1F84B0` | GS worker 1 / worker 2 loops (legacy pumps only, off by default) | worker overrides |
| `0x165220` | loader job thread's `step` fragment (`sp-=0x10; sd ra; jal 0x165400`) | `taskStepEntryFix` |
| `0x231E80/90/A0` | three empty destructors (`jr $ra`) merged into the deleting-dtor row | `returnToRa` |
| `0x205F80` | accessor `jr $ra; lbu $v0,8($a0)` merged into the previous epilogue row | `readByteOffset8Fix` |
| `0x205F90` | virtual thunk `lw t9,(a0); lw t9,0x1C(t9); jr t9` | `vtableOffset1CThunkFix` |
| `0x205FA0` | release-child/clear-fields function (converges on `case 0x205fcc`) | `objectResetFix` |
| `0x1B18F0..0x1B195C` | side-effect-free 39-entry table scan (its only callee `0x1B18E8` is `jr $ra`); resumed at the loop head with a stale `$ra`, so the epilogue returned to pc 1 | `returnToRa` on every slot |

**Durable fix (not yet done):** regenerate the corpus with the emitter producing a `case` for **every**
instruction in a row (`Option A` in `ps2xRecomp/src/lib/ps2_recompiler.cpp`), then rebuild. That retires all of
the address overrides above and the whole class of bugs. Verify on regeneration: dense `case` lists for
`entry_00205f68`, `entry_00165210`, `entry_00231e54`, and `0x165220`/`0x205F80` present as cases.

**Current blocker (re-stated after session 152):** the op-channel livelock is gone, so the session-121
description below is now history. The boot advances to the AppCell loader's **app job**, which parks at
`0x3661DC` (`[order] yield ra=0x003661DC a0=0x3F800000`, i.e. a 1.0f timeout argument) waiting on the
sub-object `0x63E3D0`'s byte `+8`. Its dequeue dependency (`FUN_002E0820(0, …)` / job table
`0x4E9140 + index*0x18`) now *does* return 1 - twice, measured - so the remaining gate is whatever must set
`0x63E3D0+8` afterwards. A second, pre-existing defect is visible in the same window:
`[guest-branch:missing-target] kind=IndirectCall source=0x301EB8 target=0x0 a0=0xFFFFFF81` (-127) — a
`jalr` through a null handler on an error-report path (present before the fix, 1 per run).

**Historical (session 121, superseded):** the boot waited on the loader's job chain: slot 4's task (app job,
step `0x366090`) sat in a wait loop at `0x3661DC` on `0x63E3D0`'s byte `+8`, and that object never left outer
phase 1 / inner phase 1 because `FUN_002E0820(0, ...)` kept returning 0 - it reads the game's 26-entry job
table at `0x4E9140 + index*0x18` and bit 1 of entry 0's status byte was never cleared. The reason it was never
cleared was the interior-entry defect at `0x125A78` fixed in session 152 (see CURRENT STATUS above).

**Also fixed in session 120:** the tree could not be built at all (the 11/13 reconfigure dropped
`CMAKE_CXX_FLAGS` and clang-cl 22 defaults to exceptions OFF, so every `throw`/`try` in the submodule failed).
`build/tol2` is now configured with `-DCMAKE_CXX_FLAGS=/EHsc`; see *Build / run* below.

**Rendering (deferred until the boot finishes):** `DISPFB2` stays `FBP 0` while the guest draws to `FBP 224`
(`ctxFbp=[224,0]`), so the presenter reads a buffer that is not the draw target. `frame_000012` is the only
non-black frame (`NOW LOADING`, 0.59 % non-black); every later frame is `max=0`. Not the current gate.

**Build / run (this machine):**

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Run from the repo root: the runtime derives `cdRoot` from the ELF's directory (`data\CD`). `-j 12` is safe with
clang-cl; MSVC cannot compile the corpus (four 8.7-9 MB single functions, `C1002`). If `build/tol2` is ever
reconfigured from scratch, `-DCMAKE_CXX_FLAGS=/EHsc` is **mandatory** (clang-cl 22 defaults to C++ exceptions
off and the submodule's `ps2xIOP` sources throw - session 120).

**Build-system caveats (learned 2026-11-10):**

- **Never edit `ps2xRuntime/include/*.h`** unless you accept a full rebuild of all ~13.5k generated TUs (~10 min)
  and a PCH invalidation. If you must: rebuild `src/runtime/CMakeFiles/tol_generated.dir/cmake_pch.cxx.obj`, then
  backdate that `.pch` and `.obj` so the corpus objects stay valid (clang-cl rejects a PCH whose recorded input
  mtime differs).
- `ninja -C build/tol2` currently re-runs CMake on every invocation (~5 min) because a globbed directory is newer
  than `build.ninja`. It still builds correctly afterwards.

**Probe index (all opt-in, no effect when unset):** `TOL_TRACE_PARK` (parks, `[retbad]`, `[badpc]`, `[path]`,
`[loop1910]`, `[y1910]`), `TOL_TRACE_EE_THREADS` (thread/sema/event-flag table once a second),
`TOL_TRACE_EE_DISPATCH` / `TOL_TRACE_EE_PC` (per-second turn buckets / dispatch pc samples), `TOL_STALL_DUMP`
(run-queue + `[runq2]`, `[pumpbody]`, `[loaderspot]`, job-step return probes), `TOL_TRACE_FILEIO`,
`TOL_TRACE_DVCI`, `TOL_TRACE_GSHIST`, `TOL_TRACE_GSPRIV`, `TOL_TRACE_LOAD`, `TOL_TRACE_SCRIPT`,
`TOL_TRACE_TASKSTEP`, `TOL_PUMP_PRIO4` (experimental I/O-wait pump; injecting pumps from anywhere else corrupts
guest state — measured), `TOL_WATCH_TASK`, `TOL_WATCH_SLOT2`, `TOL_TRACE_PREEMPT` (in-place preemption counter),
`TOL_NO_FIBER_PREEMPT` (opt-out: restores the old unwinding preemption; in-place is the default since session 159).

**Confirmed NOT the problem:** no skipped functions (`skip = []` in `data/config_fixed.toml`, 0 `TODO_NAMED` in
the corpus); the 55 `stubs` are the libkernel import wrappers and all resolve to real handlers; no missing
dispatch targets; the ADXF pump is healthy (`guard=0 blocked=0`); the audio/IOP side is up.

**Working rules:** generated `.cpp` are build inputs (never edit); `thirdparty/*` is a submodule (commit the
probe/fix inside it, then the parent gitlink); game files under `data/` are gitignored; per-address fixes go in
`src/runtime/tol_overrides.cpp`; probes are env-gated and hoisted out of hot paths (`getenv` per guest call once
cost 2.5x).

---

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
| `data` | game inputs (`SLUS_212.01`, `CD\` disc tree, TOML) + the generated corpus in `data/output_fixed` | local, gitignored |
| `thirdparty/PS2Recomp` | submodule: analyzer, recompiler, `ps2_runtime` | pinned |
| `thirdparty/imgui` | submodule: backs the F1 overlay | pinned |
| `thirdparty/simde` | submodule: SSE intrinsics on non-x86 hosts | pinned |

Pinned commits (`git submodule status`, 2026-11-11):

```
d4b1bce8  thirdparty/PS2Recomp   main  (ToL runtime + probe commits, not yet pushed)
391f714c  thirdparty/imgui       master
65c15e6b  thirdparty/simde       master
```

`thirdparty/PS2Recomp` carries local commits (runtime fixes and probes) on top of upstream; `git submodule
update` would lose them until they are pushed to `vamidi/PS2Recomp`.

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

## 6. Status (historical — 2026-11-01 skeleton build; see CURRENT STATUS at the top for today)

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

## Session 112 - write-watch on slot 2: the clear is 0x151F18 and it always runs just before the poll

`TOL_WATCH_SLOT2` hooks `ps2TraceGuestWrite` and logs every store touching `0x407770` (slot 2 block
`0x407580` + `0x1F0`). Only six writers exist, and the byte has a strict lifecycle:

```
pc=0x00100160 WRITE128 0->0    zero-init
pc=0x00109154 WRITE128 0->0    zero-init
pc=0x00151840 WRITE8   0->0    FUN_001517e0, clear (no-op here)
pc=0x00151AC4 WRITE8   0->1    FUN_001518c0, mark ready (sb $a3 = 1)
pc=0x00151D24 WRITE8   1->3    FUN_00151cf0, runner takes it
pc=0x00151F18 WRITE8   3->0    FUN_00151e90, pump clears after the body returns
```

A 60 s run shows **486 full cycles** (`0->1` x486, `1->3` x486, `3->0` x485) and the poll counter
passes 400. Every poll is immediately preceded by the clear:

```
[slot2] pc=0x00151F18 ... old=3 new=0
[poll] #1 job=0x005C6490 busy=0
[slot2] pc=0x00151F18 ... old=3 new=0
[poll] #200 repeat ... slot2active=0
```

`FUN_00165420` returns 1 only while the byte is exactly 1, and state 1 exists only between `0x151AC4`
and `0x151D24` - both inside the create/run chain. By the time the loader polls, the job has been
created, run and cleared within the same call chain, so the poll sees 0 on every one of the 400+
iterations and the frame loop is skipped every time.

**Correction to sessions 109/111:** the poll is not called once. It is called 400+ times in 60 s; the
earlier 25 s runs ended before the loader reached that phase (16 script advances, no poll at all).

Next: compare this byte in PCSX2 at the loader poll. State 1 there means our runtime completes the job
synchronously where hardware keeps it queued; state 0 means the behaviour is correct and the stall is
elsewhere.

## Session 113 - direct PCSX2 comparison: the job thread is alive there, finished here

Read PCSX2's memory directly (paused, no state disturbed; Pine is down so no savestate was possible):

```
0x3DB1F4 (job queue count) = 0          -> PCSX2 uses the same fallback job
0x497050 (job record)      = 0x0016C700 -> LAB_0016C700, the fallback entry
0x407770 (slot 2 state)    = 01 04 00 01 -> state 1, priority 4, +1F3 = 1
0x407740 (slot 2 +0x1C0)   = 0x0063D850 -> job object pointer
0x407750 (slot 2 +0x1D0)   = "#UNKNOWN"
0x407730 (slot 2 +0x1B0)   = 0x00165210, 0x002047D0, 0x00165250
```

So in PCSX2 slot 2 is the **same thread** (entry `0x165210`, priority 4) and it is **alive in state 1**, with
its resume PC parked at `0x002047D0` - inside `FUN_00204690`, the job's step method (vtable+0x0C), which is
the real game job (`FUN_00204690` loops: `while (poll(child)) yield();`).

In the port:

- `[create] entry=0x00165210 prio=4 -> block=0x00407580` - the same thread is created.
- `[jobcfg]` (`FUN_00204630`, vtable+0x08) runs 20x - the trivial entry body.
- `[jobstep]` (`FUN_00204690`, vtable+0x0C) **never runs**.
- `[thunk]` (`FUN_00165400`, the vtable+0x0C dispatcher reached from the step wrapper `0x165220`) **never runs**.
- Slot 2's state cycles `0->1->3->0` 486 times in 60 s and the loader poll returns 0 on every one of 400+
  iterations, so its frame loop never executes.

The vtable is correct at the moment the step would be dispatched (`vt=0x003D4770, m8=0x00204630,
mC=0x00204690`, verified by probe), and the resume proxy `FUN_00152200` does run - but `0x165400`
(which would `jr` to `mC`) is never reached, so the step is never entered.

Next probe: read the *resumed block's* `+0x1B4` inside `FUN_00152200` (its block pointer is `$t0` at
`0x152310`, so a mid-function probe there, not a function-entry probe) to see which address the resume
actually jumps to for the job block.

## Session 114 - NEW SESSION BASELINE (start here)

**Goal:** get the port past the loading screen to the logos and the title.

**Where it stops today:** the AppCell loader (`FUN_00164f80`) never enters its per-frame loop, because the
job thread it creates is already finished when the loader polls it. Everything downstream (rendering,
script, audio) is idle as a consequence, not as a cause.

### The scheduler, as decompiled (this is the model everything below uses)

| address | role |
|---|---|
| `FUN_001518c0` | **thread create** - fills a 64-slot block (`0x407160 + i*0x210`), state `+0x1F0` = 1, priority `+0x1F1`, entry `+0x1B0`, step `+0x1B4`, exit `+0x1B8` |
| `FUN_00151cf0` | **finish** - calls `+0x1B8`, sets state 3, demotes priority to 10 |
| `FUN_00151e90` | **pump** - runs state-1 tasks of one priority, clears state-3 tasks to 0 at `0x151F18` |
| `FUN_00152200` | **resume** - restores registers and jumps to `block+0x1B4` (the saved coroutine PC) |
| `FUN_001517c0` / `FUN_00151780` | block list iteration / tail-append insert |
| `FUN_001521c0` | `return block && block->state == 1` - the loader poll primitive |

Task triplet for the loader's job: entry `0x165210` -> vtable+8, step `0x165220` -> vtable+0x0C,
exit `0x165250` -> vtable+0x10/+0x14. Job object vtable `0x3D4770`:
`+0x08 = FUN_00204630` (trivial config), `+0x0C = FUN_00204690` (**the real game job**, loops
`while (poll(child)) yield()`), `+0x2C = FUN_002055b0` (app main loop).

### The divergence (verified, not inferred)

PCSX2, read directly while paused (no state disturbed):

```
0x3DB1F4 queue count = 0          -> fallback job, same as the port
0x497050 job record  = 0x0016C700 -> LAB_0016C700
0x407770 slot 2      = 01 04 00 01 -> state 1, priority 4, +0x1F3 = 1
0x407740 slot 2+0x1C0 = 0x0063D850 -> job object pointer
0x407730 slot 2+0x1B0 = 0x00165210, 0x002047D0, 0x00165250
```

So on hardware the job thread is **alive in state 1**, parked at resume PC `0x002047D0` inside
`FUN_00204690`. That is what makes `FUN_00165420` (poll) return 1 and the loader frame loop run.

In the port: the same thread is created (`[create] entry=0x00165210 prio=4 -> 0x00407580`), its trivial
entry body runs (`[jobcfg]`), and then it is finished - `[jobstep]` (`FUN_00204690`) and `[thunk]`
(`FUN_00165400`, the vtable+0x0C dispatcher) **never run at all**. Slot 2 cycles `0->1->3->0` 486 times
in 60 s; the poll returns 0 on every one of 400+ iterations, so the loader's frame loop is skipped every
time.

The vtable is correct at the decisive instant (`vt=0x003D4770, m8=0x00204630, mC=0x00204690`) and the
resume proxy `FUN_00152200` does execute - but `0x165400` is never reached, so the step is never entered.

### Next step (single probe, not yet run)

Read the **resumed block's** `+0x1B4` inside `FUN_00152200`. The block pointer is in `$t0` at
`0x152310` (`lw $t1, 0x1B4($t0)`), so this must be a **mid-function** probe at `0x152310`, not a
function-entry probe - entry probes at `FUN_00152200` sample a stale block (slot 2 shows `+0x1B0 = 0`
while other priorities are being resumed) and give the wrong answer.

That tells us which address the job's resume actually jumps to, and therefore why `0x165400` /
`FUN_00204690` are never entered.

### Ruled out (do not re-run these)

| hypothesis | measured result |
|---|---|
| loader waits on slot 2 / slot 3 | no - single poll returns 0; the busy flags belong to other call paths |
| priorities 7 / 8 / 9 missing | never exist in this game (only 0, 4, 10) - the `ff0(7/9/8)` calls are vacuous by design |
| job queue `0x497010` unfilled | correctly empty; PCSX2 uses the same fallback `0x16C700` |
| engine object delete | not the gate; PCSX2 never frees `0x4E9D50` either |
| audio / MIX / SPU2 | healthy; the STATX `0x80` polling is IOP AutoDMA |
| VU1 | never called in this phase |
| script bank truncation | 48/48 chunks intact |
| dispatch overhead | fixed - per-call `getenv` hoisted (1535-1569 dispatches/s, 0.62 ms mean) |

### Build / run

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

Note: touching `ps2xRuntime/include/ps2_runtime.h` rebuilds all ~13.5k generated TUs; touching
`src/runtime/tol_overrides.cpp` rebuilds in seconds.

### Instrumentation already in the tree

`TOL_TRACE_SCRIPT=1` installs the probe table in `installScriptThreadProbes` and enables:

`[tasks]` 64-slot task table on change � `[slot2]` writes to `0x407770` � `[ptr]` writes to `0x407740`
(job pointer, set `0x152064` / cleared `0x151B78`) � `[poll]` loader poll � `[run]` `FUN_00151cf0` for
slot 2 � `[create]` `FUN_001518c0` � `[jobcfg]`/`[jobstep]` `0x204630`/`0x204690` � `[thunk]`
`0x165400` � `[resume]` `FUN_00152200` � `[loop]` `FUN_00151ff0` with caller � `[engine]` `FUN_001ee060`
� `[script]` reader advances/status.

`TOL_WATCH_SLOT2=1` enables the two write-watches in `ps2TraceGuestWrite`.

`TOL_NO_EE_CKPT=1` disables checkpoint bails (they lose callee-saved registers in deep chains);
`TOL_REFUSE_ENGINE_FREE=1`, `TOL_NO_STREAM_RELEASE=1` exist but the first is not the gate and the second is
a documented workaround that is on by default.

### External reference

`vs-sr-dev/ps2-talesoflegendia-doc` documents the **JP build (SLPS_255.33)** and covers disc/volume, AFS +
CPS containers, the block codec, media budget and `CDVD.000` - **nothing on the runtime scheduler**, so it
does not shortcut this work. Two facts worth keeping: the ELF has a single flat `PT_LOAD` with **no
overlay map** ("whatever this game loads at runtime, it does not describe in its ELF"), and `CDVD.000`
carries Metrowerks-compiled code plus a `TLPKv1+50` tag. Its `tools/prefix_scan.py` is a cross-binary
routine fingerprint (longest identical byte run at any alignment, with a control) - the right instrument
for transferring a routine identified in the JP docs into our US ELF, not for debugging behaviour.

## Session 115 (2026-11-08/09) - the loader frame loop and the engine object: two codegen gaps fixed

### 1. Loader job never stepped (0x165220)

Session 114's probe question was answered with the existing `TOL_TRACE_GTHREADS` probe: after the job thread is created its
TCB `+0x1B4` (resume pc) is **0x165220**, and it stays there for the whole run (`turn #33/#40/#600/#2000/...`).
`0x165220` is the `step` fragment of map row `entry_00165210` (0x165210..0x165250). The function table maps every 4-byte
slot of that row to `entry_00165210`, but the **emitted prologue switch only has cases for the row's call continuations
(0x165230 / 0x165238)**. A dispatch at 0x165220 therefore hit `default`, restarted the body at 0x165210 - the row's
*entry* tail - and called vtable+0x08 (config) instead of vtable+0x0C (step). Measured before the fix:
`[jobcfg]` ran, `[jobstep]`/`[thunk]` never, `[poll] ... busy=0` forever, so `FUN_00164f80`'s per-frame loop was skipped.

Fix: `taskStepEntryFix` in `src/runtime/tol_overrides.cpp`, installed on slot 0x165220. It re-expresses the three
prologue instructions natively (`sp -= 0x10; sd ra; a0 = *a0`) and dispatches 0x165400 (vtable+0x0C thunk), then
continues at 0x165230 (which has a case).

Verified: `[taskstep] #1 param=0x00407740 obj=0x005C6490`, `[thunk] #1`, `[jobstep] enter #1 pc=0x00204690`,
`[poll] #1 job=... busy=1`. The loader's frame loop now runs (`[loop] ff0(0/1/4)` + `[engine]` repeatedly).

### 2. The engine object was freed by an interior entry point (0x231E80/90/A0)

The Session 95-100 "the engine block at 0x4E9D70 is freed by its own sub-step" mystery is solved. The map row
`entry_00231e54` (0x231E54..0x231EB0) is a **merge of several tiny routines**:

```
0x231E54  delete tail of the 0x231E20 base destructor: if (deleting) free($s0)
0x231E80  jr $ra        <- empty destructor, own vtable slot
0x231E90  jr $ra        <- empty destructor, own vtable slot
0x231EA0  jr $ra        <- empty destructor, own vtable slot (obj 0x5AA140)
0x231EB0  FUN_00231eb0  set vtable 0x3D4B20; return this
```

The emitted switch only has `case 0x231e6c`; dispatching at 0x231E80/0x231E90/0x231EA0 hit `default` and restarted the
row at **0x231E54**, i.e. ran the *deleting* tail with whatever `$s0` the caller had - which was the engine object.
`[dtorrange]` traces before the fix: `target=0x231E80 a0=0x5AA140` then `src=0x231E64 ... a0=0x004E9D70 free`.
This is why the block's magic went `RNDM -> FREE` while `0x3DB320` still pointed at it, and why a fresh 0x14-byte
allocation reused it (Session 95-98). PCSX2 never frees it (Session 99 watchpoint) because on hardware those slots
execute `jr $ra`, not a delete.

Fix: `returnToRa` in `src/runtime/tol_overrides.cpp`, installed on slots 0x231E80 / 0x231E90 / 0x231EA0.

Verified (`TOL_WATCH_TASK=1`): `heapfree2 = 0`, magic stays `4D444E52` (RNDM), engine word stays `003D4B20`, and the
engine now reports real substeps:

```
[engine] substep0 obj=0x004E9DB0 vt=0x003D4AC0 step1C=0x00230A50   (renderer)
[engine] substep1 obj=0x005AA140 vt=0x003D4B00 step1C=0x00231EA0
[engine] substep2 obj=0x005AA170 vt=0x003D50D0 step1C=0x002853D0   (media/stream)
[engine] g334=0x004E9C50 renderer +8=00000002 +C=0x004EA090 +10=0x0053DE20 +14=1
```

That renderer state matches PCSX2's (Session 98): guard 1, both buffer pointers set - it used to be all zero.

### 3. What the guest does now

Past the 16-chunk MIX bank the boot streams real data instead of 2 KB extent reads:

```
[fileio] gamefile read sector 0x200002 544768 bytes
[fileio] gamefile read sector 0x40001F 522240 bytes
[fileio] gamefile read sector 0x40015E 389120 bytes
[fileio] gamefile read sector 0x604C37 262144 bytes
```

75 reads over 200 s, frames presented at ~50/s, `[spin]` keeps moving (no sticky pc). The screen is still black after the
early `NOW LOADING` frame (`frame_000012` nonblack 0.587%, every later frame max=0).

### 4. New blocker

After the last read (`slots 5 +19511`, 256 KB) no further `gamefile read` is issued, while the spin sampler ends in the
ADXF/CRI path (`0x1A9A28`, `0x1AD320`, `0x1B1910`). Next: the ADXF guard/pump counters for this phase
(`TOL_TRACE_DVCI`) and whether the loader is waiting on the job chain (`0x5C6490 -> 0x5C64D0 -> 0x63D850 -> 0x63D890 ...`).

### 5. Root cause of both fixes (worth fixing in the recompiler)

Both defects are the same codegen gap: **a map row can be entered at an interior address that has no switch case**, and
the emitted default restarts the row at its start. `Option A` in `ps2_recompiler.cpp` (register every decoded instruction)
is supposed to make the switch dense, but this corpus (generated 2026-11-02 18:18) is not dense for these rows
(`FUN_00204690`: 156 cases over ~13k instructions). The general fix is to regenerate with a recompiler build that really
emits Option A, which also makes the 0x165220 and 0x231E80/90/A0 overrides unnecessary. Until then the three tiny
overrides are the faithful per-slot behaviour.

### Probes added

| probe | where | what |
|---|---|---|
| `[dtorrange]` / `[dtorrange2]` | `ps2_runtime.cpp` `dispatchGuestBranch` | every transfer in/out of 0x231E00..0x231EC0 (the 2nd is after the callee) |
| `[dtorrrun]` / `[dtorsched]` | `EeScheduler.cpp` | scheduler-side dispatch into that range (proves the fiber-resume path) |
| `TOL_TRACE_TASKSTEP` | `tol_overrides.cpp` | logs the 0x165220 step fix |

### Build / run (unchanged)

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

### 6. New blocker, measured (session 115 close)

`TOL_TRACE_DVCI=1` for 60 s after the fixes:

```
[adxf-pump]  #2500 guard=0 blocked=0          <- the guard is NOT wedged (session 93 has not come back)
[adxf-stall] step #29407 unchanged x28000 | state=1 busy=0 45=0 48=0 49=0 buf=0x0 done=0 blocks=0
             win=1048575 dev=0 devBase=0x0    <- the ADXF stream is idle: no client read request
```

So the pump runs freely; the *consumer* has stopped asking (stream[0x45] = 0, the only setter is `FUN_0019F710`
ADXF_Read). The last data request was `slots 5 +19511`, 262144 bytes into 0x0067D200. `TOL_TRACE_GSHIST` installs and
unpauses but never reports, because the frame-tail address it samples (`0x00139A20`) is never reached - the guest is
still inside the loader's per-frame loop (`FUN_00164F80`), not the game's main render loop (`FUN_00165AD0`).

Next probes (not yet run): the job chain at that moment (`[create]`/`[jobstep]`/`[thunk]`/`[poll]` with
`TOL_TRACE_SCRIPT=1`, raising their print caps), and which job step should call `FUN_00197550`/`FUN_0019F710` next.

Display measurement after all fixes (unchanged): `DISPFB2 = 0x1000` (FBP 0), `ctxFbp=[224,0]`, every presentation is
512x448 from FBP 0 -> black. The early `NOW LOADING` frame (frame_000012, 0.587% non-black) is the only picture so far.

## Session 116 (2026-11-09) - frame loop measured; the AppCell daemon is starved and the run loop parks the main thread

### 1. What runs now (measured)

`TOL_TRACE_PC` on the startup chain: the main loop's first iteration completes its whole body -

```
enter 0x165AD0 (main)          -> 0x165C90 startup
enter 0x165C90                 -> exits to memset (ordinary checkpoint bail)
enter 0x165E70 (frame update)  -> 0x19BD20
enter 0x166010 (GS/VIF wait)   -> 0x1F8150 (worker)
enter 0x166120 (script)        -> 0x27AA08
enter 0x1667E0                 -> returns v0=1
enter 0x166870                 -> 0x1F8168
enter 0x139D50 (render setup)  x10
```

and `TOL_TRACE_LOAD` shows the asset chain **completing**: submits #1..#6 (`size=1252176`, 530512, 71760, 90192,
387344, 260368) all reach job state 2 (`[load] status ... state=2`). So the boot is past the asset chain.

### 2. The AppCell loader daemon only runs once

`[loaderspot]` (new probe on the loader row entry_00164f8c) shows:

```
[loaderspot] row-start #1 tid=1 ra=0x00151F88 ...     <- entered once
(poll / after-task / no-task: 0 hits)
```

and `[prio4]` (new probe on FUN_00151FF0(4)) shows the pump is called **once** with slot 3 ready:

```
[prio4] enter
[prio4] #1 slot3 state=1 prio=4 started=1 yield=0 resume=0x00164F80 entry=0x00164F40
```

The daemon creates the fallback job (`[create] entry=0x00165210 -> block=0x00407580`), binds it (`FUN_00152030`),
yields at `0x164FC4`, and is never resumed. Reason: the frame loop's three pumps are at `0x165B90/98/A0`
(prio 0, 1, 4) but the *whole iteration is consumed by the startup asset chain* (`FUN_001EC670`), whose pump loops call
`ff0(0)`/`ff0(1)` only - measured `[loop] ff0(0) x14, ff0(1) x4, ff0(4) x1` with the `ff0(0/1)` calls coming from
`0x166070/78` (FUN_00166010) and `0x1EC6D4/73C/7A4/80C/880` (FUN_001EC670), and only **one** call with
`ra=0x165B98/BA0/BA8` (the main loop).

So the daemon (which owns the loader task queue) is parked for the whole asset phase; the job chain it spawned runs once.

### 3. The job chain and where it stops

```
[thunk] #1 a0=0x0063D850 -> step 0x00204690  (runs, exits to 0x151F88 = yield)
[poll]  #1 job=0x0063D890 busy=1 ra=0x002046F0   (parent polls child, child ready)
[thunk] #2 a0=0x0063D890
[pc] enter 0x00366090 #1 a0=0x0063D890 ... exit pc=0x001C03DC
```

The child job's step is **0x00366090** (`FUN_00366090`, 572 B; present in the table), which calls `0x1C03DC` and then
never returns to the loader. After that the EE is *alive but cycling*: `TOL_TRACE_EE_PC` at the end is dominated by
`tid=1` in the microkernel run loop (`0x151F88`) and the CRI pump (`0x1A152C/0x1A36A0/0x1AD2C4/0x19ED1C/0x198270`),
plus `tid=3/tid=4` (CRI workers at `0x198E0C/0x198EF8`). `TOL_TRACE_DVCI` shows the ADXF stream idle
(`state=1 45=0 dev=0`) with the pump healthy (`guard=0 blocked=0`), and no further `gamefile read`.

`FUN_001516B0` (called right after the prio-4 pump in the main loop, `0x165BB0`) is **never** called
(new `[framebody]` probe, 0 hits), which says the main thread never gets back out of the pump/run-loop to the rest of
the frame body.

### 4. Conclusion / next

Two coupled defects remain:

1. **Daemon starvation**: the asset chain pumps only priorities 0/1, so the AppCell loader daemon (prio 4) cannot run
   while it loads. Either the chain must pump 4 too (it is the loader's own task queue), or the daemon must be pumped
   from the I/O wait (`FUN_0019A080`) as well.
2. **Run-loop park**: the daemon's job chain parks the main thread inside the microkernel run loop (`0x151F88`) after
   `thunk #2`; the child job's step `0x366090` runs (calls `0x1C03DC`) but the loader never resumes at its poll.

Next probes: `TOL_TRACE_PC=0x1C03DC` and the callees under it to see where `FUN_00366090` parks; and whether pumping
priority 4 from `FUN_0019A080` (guarded) lets the daemon's poll finally run.

### Probes added this session (all `TOL_TRACE_SCRIPT`)

| probe | what |
|---|---|
| `[loaderspot]` | loader row entry_00164f8c at row-start / after-task / poll / no-task |
| `[prio4]` (enter + TCB dump) | every FUN_00151FF0(4) call with slot 3 state/prio/resume/entry |
| `[framebody]` | FUN_001516B0 call rate (main-loop body completion) |
| `[poll]` | now prints caller `$ra` and the first 50 calls (the old key suppressed identical results from another caller) |

## Session 117 (2026-11-10) - daemon run-queue state measured; injecting prio-4 pumps is unsafe

### 1. The daemon is NOT lost: it is ready, linked, and counted

`TOL_STALL_DUMP=1` (new) samples the guest run queue both at dispatch entries (`0x151F88`/`0x151E90`) and once
per green-thread enter (`FUN_00152200`). Last dump of a 90 s run:

```
[runq] #5 pc=0x00151F88 tid=1 ready[0..7]=2,0,0,0,1,0,0,0 cur=0x00407160 A=1 B=464 C=448
[runq]   slot= 0 tcb=0x00407160 state=1 prio=0 started=1 resume=0x001F8168 next=0x00000000 prev=0x00407370
[runq]   slot= 1 tcb=0x00407370 state=1 prio=0 started=1 resume=0x001F8528 next=0x00407160 prev=0x00407580
[runq]   slot= 2 tcb=0x00407580 state=3 prio=10 started=1 resume=0x001660EC next=0x00407370 prev=0x00407790
[runq]   slot= 3 tcb=0x00407790 state=1 prio=4 started=0 resume=0x00164F80 next=0x00407580 prev=0x00000000
```

So while the main thread is in the run loop:

- the AppCell daemon (slot 3, `0x407790`) is **state=1, prio=4, `ready[4]=1`**, and is the **head** of the doubly
  linked run queue (`prev=0`, `next=0x407580`); the full list is `0x407790 -> 0x407580 -> 0x407370 -> 0x407160`;
- `started=0` is not a gate: the run loop only uses `+0x1F3` for the first-dispatch branch (`0x151F68` ->
  `0x151FB0`), not for selection;
- the scheduler's current-context pointer (`0x48F920`, `A*B+C` = 1*464+448) points at slot 0.

### 2. The pump simply stops being called

`[pumpbody]` (new, also under `TOL_STALL_DUMP`) wraps `FUN_00151E90` and times it:

```
[pumpbody] #1 prio=0 us=1635 ra=0x00151F88 pc=0x001F8150
[pumpbody] #2 .. #12 prio=0 us=98..4678 ra=0x00151F88 pc=0x001F8168
```

**12 calls in 90 s, all priority 0**, each returning with `pc=0x1F8168` (worker 1's resume). The run loop is not
stuck - it returns normally - the guest just never asks for another priority (and never for priority 4).
Green-thread enters also stop at ~10 turns (`[runq2] turns=10`); the daemon is never among the entered TCBs in the
baseline (`0x407160`, `0x407370`, `0x407580` only).

Tail of the run (`TOL_TRACE_EE_DISPATCH`): tid=4 (a CRI worker) runs at 1447 turns/s cycling
`0x198EF8 / 0x1A152C / 0x198E14 / 0x166490 / 0x1A36A0 / 0x1B76F8`; the main thread is not in the top buckets
(parked), and the read count is stationary at **75** (last: `slot 5 +19511`, 262144 bytes at `0x0067D200`).

### 3. Negative result: injecting prio-4 pumps corrupts guest state

Two variants were tried and both are unsafe:

- **mirror**: after any `FUN_00151FF0(prio)` also run `FUN_00151FF0(4)`;
- **ADXF entry**: pump priority 4 at the entry of `FUN_00197940`, with a full `R5900Context` snapshot/restore.

The ADXF variant did schedule the daemon (`turn #12 tcb=0x00407790 resume=0x00164F80`) and ran ~270 pumps, then the
guest died with **`Unhandled SPECIAL instruction: 0x1 at 0x393234 raw=0x00000001`** (executing data). Cause: the pump
switches contexts through `FUN_00152200`, so the interrupted thread's registers are replaced by the scheduled thread's;
restoring them afterwards leaves the guest scheduler mid-switch. Same family as the session 84-94 experiments.
Both variants were **removed**; only the opt-in, safe I/O-wait pump (`ioWaitPrio4Probe`, `TOL_PUMP_PRIO4=1`) remains,
and it is unusable after the asset chain because `FUN_0019A080` is no longer called (70 pumps in that phase, then none).

### 4. Conclusion / next

The daemon starvation is real but is a **symptom**: the queue and counters are correct, and the run loop works when
called. What is missing is the caller: after the asset chain's wait loops finish, the guest stops pumping any
priority and parks the main thread while a CRI worker loops. So the next question is *what the main thread waits on in
the post-asset phase* (`tid=1` parked, `tid=4` looping at 0x198EF8/0x1A152C), and whether that wait is a completion
from the pending read/stream (stream `0x39EE48` idle: `state=1 45=0 dev=0`). If the main thread is waiting for the
AppCell loader, then the fix is to get the guest's own frame loop to its prio-4 pump, not to inject pumps.

### Probes added

| probe | where | what |
|---|---|---|
| `[runq]` / `[runq2]` | `EeScheduler.cpp` + `tol_overrides.cpp` | full TCB table (state/prio/started/yield/resume/entry/links) + `ready[0..7]`, throttled 2 s, at dispatch and at every green-thread enter |
| `[pumpbody]` | `tol_overrides.cpp` | `FUN_00151E90` entry/return with prio, elapsed us, and the pc it leaves |
| `[prio4]` extras | `tol_overrides.cpp` | slot-3 links and ready counters on every `FUN_00151FF0(4)` |

## Session 118 (2026-11-10) - the parked main thread: it is Dormant (no wait object), killed by bogus returns from merged rows

### 1. Identification (TOL_TRACE_EE_THREADS / TOL_TRACE_PARK / new [retbad] [badpc])

`TOL_TRACE_EE_THREADS=1` last dump, with no probe poking the guest:

```
[eethreads] cycle=... running=-1 threads=7 sema=14 evf=0
  tid=  1 Dormant   wait=-    prio= 24/  0 pc=0x00000000 ra=0x00000000 sp=0x00423530 entry=0x00100008
  tid=  2 Suspended prio=  8/119 pc=0x00198C60
  tid=  3 Ready     prio= 16/119 pc=0x00198E14 entry=0x00198D50
  tid=  4 Ready     prio= 18/119 pc=0x00198EF8 entry=0x00198E58
  tid=  5 WaitSusp  wait=sleep entry=0x00198F38
  tid=  6 Waiting   wait=sleep pc=0x00118D50 entry=0x00218908
  (all 14 semaphores: waiters=0 ; no event flags)
```

So the main thread is **not blocked on anything**: no sema waiter, no event flag, no `Waiting` state. It is
`Dormant` with `pc = 0` (later `1`). It was Running at `pc=0x001AD2D8` (CRI read step) a few seconds earlier.

`[ee-park]` named the producing chain:

```
[ee-park] thread=1 ... returned-to-pc0 from=0x366168 ra=0x0 sp=0x423530 invocations=0
  recent=0x366168, 0x20D370, 0x366138, 0x365568, 0x1C0878, 0x1C03DC, 0x366090, 0x165220, 0x151FC8, ...
```

i.e. the loader job step (`0x366090` -> caseD_0 `0x1C03DC` -> epilogue `0x1C0878` -> `0x365568` -> `0x366138` ->
`0x20D370` -> `0x366168`) returned with `$ra == 0`, and `FUN_00151E90`'s scheduler branch does `makeDormant()`.

### 2. Root cause: three more merged-row interior entries (map row entry_00205f68)

The new `[retbad]` probe (in `dispatchGuestBranch`, logs any callee that leaves pc < 0x100000) named the culprit:

```
[retbad] callee=0x00205F80 from=0x00366178 kind=3 pc=0x00000000 ra=0x00000000 a0=0x0063E3D0
```

`0x205F80` is a **real one-instruction accessor** (`jr $ra ; lbu $v0,8($a0)`) but the map merged it into the
**previous function's epilogue row** `entry_00205f68` (body starts `move v0,s1; ld ra,0x20(sp); ...; jr ra;
addiu sp,+0x30`). Its table slot exists, the emitted switch has no case, so the dispatch restarted the row at
`0x205F68`, ran the previous function's epilogue on the job's stack, loaded a garbage `$ra` and returned to 0.

The same row swallowed three routines. All are now re-expressed natively in `tol_overrides.cpp`:

| address | real code | fix |
|---|---|---|
| `0x205F80` | `jr $ra ; lbu $v0,8($a0)` | `readByteOffset8Fix` |
| `0x205F90` | `lw $t9,($a0); lw $t9,0x1C($t9); jr $t9` (virtual thunk) | `vtableOffset1CThunkFix` |
| `0x205FA0` | small release-child/clear-fields function (prologue + vtable+0x38 call + clear at 0x205FCC) | `objectResetFix`, converges on the existing `case 0x205fcc` |

After these, the job-chain park is **gone** (`[retbad]` count 0) and the main thread gets further: it now runs the
CRI/ADXF code (`recent` chain `0x1B1910 <- 0x1664A0 <- 0x199FD4 <- 0x199148 <- 0x198E0C`) before stopping.

### 3. Next instance (different context)

`[badpc] thread=1 pc=0x00000001 from=0x001B1910 ra=0x00000001 sp=0x01FFFF40` - the stop is now on the **async
invocation stack** (sp = 0x01FFFF40, top of RDRAM), inside `FUN_001b18f0` (a 0x27-iteration loop). `0x1B1910` *is*
in that function's switch, so this is not the same missing-case bug; a `jr $ra` returned `ra == 1` on an
invocation context. That is the next thing to identify (an invocation's `$ra` should be the resume pc).

### 4. Systemic conclusion

Every one of the main-thread deaths so far is the same codegen gap: **a map row can be entered at an interior
address with no switch case, and the default restarts the row at its start** (previous sessions: 0x165220,
0x231E80/90/A0; this session: 0x205F80, 0x205F90, 0x205FA0). Targeted overrides advance the boot one entry at a
time. The durable fix is to regenerate the corpus with the emitter really producing a case for every instruction in
the row (Option A), which would retire all of these overrides.

### Probes added

| probe | where | what |
|---|---|---|
| `[retbad]` | `ps2_runtime.cpp` `dispatchGuestBranch` | names a callee that leaves pc < 0x100000 (0, 1, ...) |
| `[badpc]` | `EeScheduler.cpp` run loop | same, for plain generated `jr $ra` that bypasses dispatch, with the dispatch ring |
| `[jobepilogue]` / `[jobwraptail]` | `tol_overrides.cpp` | resume points of the job step epilogue and the step wrapper tail |

## Session 119 (2026-11-10) - FUN_001B18F0 returns to pc 1 because its epilogue runs on a foreign frame

### 1. The call path (measured, TOL_TRACE_PARK `[path]`)

```
[path] target=0x001B18F0 from=0x001A9A3C kind=3 ret=0x001A9A44 ra=0x001A9A44 sp=0x01PPP...
```

Every call into `FUN_001B18F0` comes from `0x1A9A3C` (inside `FUN_001a9a08`) with the correct return
address `0x1A9A44`, on several stacks (`0x397B30`, `0x1FFFDxx`, ...). Its loop calls `0x1B18E8`, which is an
empty `jr $ra` stub. So nothing in the function itself is wrong.

### 2. The bad dispatch (`[loop1910]`, new probe)

```
[loop1910] #1 tid=1 ra=0x001A9A44 sp=0x01FFFF20 frameRa=0x00000001 s0=0x003ACF40 s1=0x00000002
```

The scheduler dispatches `0x1B1910` (the loop head, near its end: `s1=2` of 0x27) with the **correct `$ra`
register**, but on a frame at `sp=0x1FFFF20` whose saved-return slot `[sp+0x10] = 0x1FFFF30` contains **1**.
The epilogue does `ld ra,0x10(sp); jr ra` -> pc 1 -> the scheduler's `[badpc]` makes thread 1 Dormant.

### 3. Why the frame is foreign (two independent proofs)

- **The prologue never ran on it.** No `[path]` entry ever entered `0x1B18F0` with caller `sp=0x1FFFF40`,
  which is the only sp that would make its frame `0x1FFFF20`.
- **Nothing wrote the slot.** A temporary write-watch on `0x1FFFF30` (added to the inline guest-write tracer,
  `TOL_WATCH_STACKRA`) recorded **0 writes in an 80 s run**. The `1` is leftover stack memory, not a value any
  guest function stored this session.

So the `$ra` register is fine; the *frame* is not. The epilogue is executed by a dispatch that jumps into the
middle of the row (`0x1B1910`) without the matching prologue, and it loads a stale word.

### 4. Where that dispatch comes from

- `0x15231C` (the microkernel yield) never saved a context with pc `0x1B1910` (`[y1910]` probe: 0 hits).
- `dispatchGuestBranch` never targets `0x1B1910` (`[path]`: 0 hits) and there is no `jal 0x1B1910` anywhere.
- The `[badpc]` ring shows the previous dispatch was `0x1664A0` — a *resume* inside `FUN_00166490`, whose own
  prologue is at `0x166490` and whose body is `FUN_0019A068(0); sync; ei; return 0`. Its `jr $ra` is the most
  likely producer of pc `0x1B1910` (a return through a stale `$ra`), i.e. the same "row resumed mid-function"
  family. That is the next thing to instrument (log the scheduler's saved pc at every checkpoint bail whose
  pc is `0x1B1910`, and the `jr ra` that lands there).

### 5. Build-system note (learned the hard way)

Editing `ps2xRuntime/include/ps2_runtime.h` invalidates the PCH and rebuilds all **13.5k** generated TUs
(~10 min). After such an edit: rebuild `src/runtime/CMakeFiles/tol_generated.dir/cmake_pch.cxx.obj`, then
backdate the `.pch`/`.obj` so the corpus objects stay valid. Also, `ninja -C build/tol2` now re-runs CMake on
every invocation (a globbed directory is newer than `build.ninja`), which costs ~5 min; a real build still
proceeds correctly after it.

### Probes added

| probe | env | what |
|---|---|---|
| `[path]` | `TOL_TRACE_PARK` | call sites + expected return for 0x166490 / 0x1B18F0 / 0x1B18E8 / 0x1B1910 |
| `[loop1910]` | `TOL_TRACE_PARK` | scheduler dispatch at 0x1B1910 with ra/sp/frameRa/s0/s1 |
| `[y1910]` | `TOL_TRACE_PARK` + `TOL_STALL_DUMP` | microkernel saves whose pc is 0x1B1910 (block, saved sp/s0/s1) |
| `TOL_WATCH_STACKRA` | (removed) | write-watch on 0x1FFFF30; proved 0 writes |

## Session 120 (2026-11-17) - build repaired; the 0x1B1910 death removed; the loader frame loop now runs

### 0. The build was broken before anything else could be measured

`build/tol2` had been reconfigured on 11/13 00:11 (Rider) with `CMAKE_CXX_FLAGS` **empty**, and clang-cl 22
defaults to `/EHs-c-` (C++ exceptions OFF). Every `throw`/`try` in the submodule failed with
`error: cannot use 'throw' with exceptions disabled`, so the tree could not be relinked at all (tol.exe was
stale from 11/10 23:59). Verified the default with a one-line test: `clang-cl -std:c++20 -MD -c t.cpp` where
`t.cpp` does `throw 1;` -> error; adding `/EHsc` compiles.

Fix: configure with `-DCMAKE_CXX_FLAGS=/EHsc`. This rebuilds **all 13,669 edges** (~35 min at `-j 12`), which
is unavoidable because the 11/13 reconfigure had already dirtied the whole graph. After that
`ninja -C build/tol2 -j 12 tol` is incremental again (`no work to do`).

### 1. The 0x1B1910 death: mechanism measured, then removed

Two new probes (both `TOL_TRACE_PARK`):

| probe | where | what |
|---|---|---|
| `[parkbail]` | `ps2_runtime.cpp` `eeCheckpointDue` | every checkpoint bail whose `ctx->pc` is in `0x1B18F0..0x1B1960`, with `sp/ra/s0/s1` |
| dispatch ring | `EeScheduler.cpp` `run()` | `tid/pc/sp/invocations` of the last 32 dispatches, printed by `[loop1910]` and `[badpc]` |

Measured (100 s run):

```
[parkbail] #1 pc=0x001B1910 ra=0x001A9A44 sp=0x01FFFF20 s0=0x003ACF40 s1=0x00000002
[badpc]   -1 tid=1 pc=0x00000001 sp=0x01FFFF40 inv=0
[badpc]   -2 tid=1 pc=0x001B1910 sp=0x01FFFF20 inv=0     <- resume at the loop head
[badpc]   -3 tid=1 pc=0x00000000 sp=0x01FBFFF0 inv=1     <- invocation return, same thread
[badpc]   -4 tid=1 pc=0x001664A0 sp=0x01FBFFE0 inv=1
[loop1910] #1 ... sp=0x01FFFF20 frameRa=0x00000001 s0=0x003ACF40 s1=0x00000002
           frame=00499F10/0000001F/00000001/00000036
```

The frame dump settles it: `sp+0 = 0x00499F10` and `sp+8 = 0x0000001F` are the caller's `$s0`/`$s1` from
`FUN_001a9a08` at iteration 0 of its own `jalr` loop, so this **is** `FUN_001b18f0`'s own frame (the prologue
ran and `s0`/`s1` advanced to 37 of 39 while the loop ran) - but its saved `$ra` slot holds **1**, i.e. the
`$ra` register was already 1 when the prologue saved it. The caller always sets `$ra = 0x1A9A44` before the
dispatch (`[path]` shows 120 clean entries), so the stale value came from the *unwound resume* of the thread,
not from the call. `FUN_001b18f0` is a 39-entry scan (stride 0x38 at `0x3AC728`) whose only callee `0x1B18E8`
is an empty `jr $ra` stub - the row has no effect beyond reading that table.

Fix (`tol_overrides.cpp`): `returnToRa` on every slot of `0x1B18F0..0x1B1960`, the faithful re-expression of
a side-effect-free scan.

Result (120 s): **`[badpc]` = 0**, the main thread stays alive (`tid=1 Running pc=0x00151F88 sp=0x00421540`),
and the AppCell loader's per-frame loop finally runs - `[poll]` 1625 times, all `busy=1`, `[prio4]` #813 with
`resume=0x00164FE8`. All six asset loads submit and reach job state 2. The script advances 16 of 48, which is
the same script wall sessions 103-106 hit, but with the loader loop alive instead of skipped.

### 2. Negative result: fiber preemption (kept, opt-in, default off)

The right long-term fix for this whole family (the unwarp re-dispatches mid-function addresses with whatever
registers the guest left) is to preempt by **suspending the guest fiber** instead of unwinding, so the C++ call
chain and every callee-saved register survive - the mechanism syscalls already use. Implemented in
`EeScheduler::checkpointDue` behind `TOL_FIBER_PREEMPT=1` (preempt in place, return false so the generated
caller just continues), plus alternation between a parked thread and the invocation carrier in `run()`.

Measured with it enabled: the boot stalls in libc init,
`tid=1 Ready prio=119/0 pc=0x00109144 sp=0x01FFFF40`, 2 threads / 2 semaphores, while the Timer-2 handler
(`0x11F650`) runs forever on the carrier (`recent` = 16x `0x11F650`). Preferring the carrier starves the guest;
preferring the thread starves interrupts; strict alternation still stalls. So it is off by default and the
default stays the historical unwinding behaviour. It is **not** discarded: the missing piece is a way to nest
an invocation on a thread whose fiber is parked mid-call (a second host stack for that thread).

### 3. Reading the park log correctly

`[ee-park] ... invocations=1 sp=0x1fbfff0` is the **normal invocation-return path** (the handler returns to
pc 0, the scheduler pops the invocation and continues) - not a dead thread. `thread=-1` in those lines is the
invocation carrier thread (`acquireInvocationThread`, negative ids). Only a park with `invocations=0` makes a
thread dormant.

### 4. Next

The boot now sits in the loader's frame loop with the job chain reporting `busy=1` forever
(`0x63D850 -> 0x63D890 -> 0x4079A0`), the script writer stops at 16 of 48 MIX chunks (sessions 103-106), and
`tid=1` cycles the microkernel run loop `0x151F88`. The next question is the same one session 106 left open,
now with a live loader: which guest path is supposed to enqueue the next loader task / feed slot 0 at
`0x4A5840`.

### 5. Build / run (unchanged command, exceptions flag now in the cache)

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >NUL 2>&1 && ninja -C build/tol2 -j 12 tol'
build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu
```

New probes: `TOL_TRACE_PREEMPT` (in-place preemption counter), `TOL_FIBER_PREEMPT` (the opt-in experiment).

## Session 121 (2026-11-17) - the loader job chain traced to one status bit

Question: the AppCell loader's job chain reports `busy=1` forever. Traced end to end; the runtime is not
involved - the guest task is genuinely waiting.

### The chain (all measured)

1. `FUN_00164F80` (loader) calls `FUN_00165420(job=0x63D850)` -> `FUN_00152160(*(u32*)0x63D850 = 0x407580)`,
   which scans the 64 TCB blocks at `0x407160` for `+0x200 == key` and reports busy while `+0x1F0 != 0`.
   Slot 2 is `state=1` -> busy. 811 polls from `ra=0x0016503C`.
2. Slot 2's step is `FUN_00204690`. At `0x2046DC` it takes `s2 = FUN_003662E0()` = job object `0x63D890`
   (whose first word is TCB `0x4079A0`, slot 4) and sits in `do { yield(); } while (FUN_00165420(0x63D890))`
   at `0x2047C8..0x2047B8`. Slot 4 is `state=1` -> busy. 811 polls from `ra=0x002047D8`.
3. Slot 4 is the app job task (`entry=0x165210`, step `0x366090`). `TOL_STALL_DUMP` shows its guest resume pc
   is **`0x3661DC` in all 14 dumps over 60 s**, while the priority-4 pump runs 813 times: it is resumed, runs,
   and yields again from the same instruction.
4. `0x3661DC` is inside `while (*(u8*)(s1 + 8) != 0) { global->vt[8](); s2->vt[0x1C](s2); s1->vt[0x38](s1);
   global->vt[0x0C](); yield(); }` (the yield is at `0x3661D4`, so the resume lands right after it).
5. `s1` = object `0x63E3D0`, built by `FUN_003654B0` (0x220 bytes, vtable **`0x3DA320`**, outer phase `+0x70`,
   next `+0x72`, inner phase `+0x74`, child `+0x18`, job handle `+0x21C` from `FUN_001C0390(0, ..., 3, 0, 1)`).
   Its wait byte is read through vtable `+0x0C` = **`0x205F80`** (`jr $ra; lbu $v0,8($a0)`) - one of the three
   merged-row interior entries fixed in session 118. `vt[0x38]` = `FUN_00365630` is the per-iteration update.
6. Live `[jobobj]` sampling (`TOL_TRACE_SCRIPT`, probe on `0x365630`) is completely frozen over 65 s:

   ```
   [jobobj] #1  obj=0x0063E3D0 vt=0x003DA320 state=1 phase=1 next=65535 inner=0 child=0x0063E610 job=0x0063E800 ret=0 | +78=0 +7C=0 +84=0
   [jobobj] #20 obj=0x0063E3D0 vt=0x003DA320 state=1 phase=1 next=65535 inner=1 child=0x0063E610 job=0x0063E800 ret=0 | +78=0 +7C=0 +84=0
   ```

   So the wait byte `+8` is 1 forever, the outer phase stays 1 with nothing queued (`+0x72 = -1`), and
   `FUN_00365630` returns 0 ("not done") on every call, so it never advances `+0x70` to `+0x72`.
7. `FUN_00365630` phase 1 calls **`FUN_00365700`** (a second state machine on `+0x74`). Its inner phase 1 does
   `*(u32*)(this+0x80) = -1; v0 = FUN_002E1EF0(0, this+0x80); if (v0 == 0) return 0;` and only then
   `+0x74 += 1` and starts the child (`FUN_00307770(child, 1.0f)`).
8. `0x2E1EF0` is a tail-jump to **`FUN_002E0820(index, out)`**, which reads a 26-entry table at
   **`0x4E9140 + index*0x18`** and returns 0 when **bit 1 of the entry's status byte is clear**. With `index 0`
   it is always 0, so the phase never advances.

### Therefore

`busy=1` is not a deadlock and not a poll bug: the TCBs correctly describe a guest task that has not finished.
The whole boot is waiting on **bit 1 of the status byte of job-table entry 0 at `0x4E9140`**, which nothing
sets. The same 0x18-stride table (26 entries) is what `FUN_001C0390`'s factory indexes, so this is the game's
own job/worker table, and the missing piece is the producer that completes slot 0.

### Probe added

| probe | where | what |
|---|---|---|
| `[jobobj]` | `tol_overrides.cpp`, wrapper on `0x365630` | the app job's sub-object: vtable, wait byte `+8`, outer phase `+0x70`/next `+0x72`, inner `+0x74`, child `+0x18`, job `+0x21C`, the update's return value, and `+0x78/+0x7C/+0x84` |

### Next

Find who writes `*(u8*)0x4E9140` bit 1 (and what `FUN_002E0820`'s `out` word is consumed as), i.e. which
subsystem completes job slot 0. If nothing ever does, the next question is which queued job should have been
dispatched into that table - the same "nobody asks" shape as session 106, now narrowed to one table entry.

## Session 122 (2026-11-17) - the job table has no writers at all; the setter is a known function the port never calls

> **Partly wrong, corrected in session 123.** Two claims below are mine and false: the "zero writers"
> measurement (watched the wrong layer, so it measured nothing) and the treatment of `0x2E1D30` as a callable
> setter (it is an interior row tail; nothing calls it because there is nothing to call). Corrections are
> inline below and the replacement evidence is in session 123.

### Port side: zero writers (measured, with a control)

`TOL_WATCH_JOBTABLE=1` (new, in `PS2Runtime::Store8/16/32/64/128`) logs every store overlapping
`0x4E9140..0x4E93B0` with pc, op, addr, size, value, entry+offset and the bytes. Over a 55 s boot:

```
[jobtable] lines: 0
```

Positive control in the same run (pre-existing watch on the TCB status byte):

```
[slot2] pc=0x00151AC4 WRITE8   addr=0x00407770 size=1 old=0 new=1     (7 hits)
[ptr]   pc=0x00109144 WRITE128 addr=0x00407740 size=16                 (6 hits)
```

So the store path is live and the job table genuinely has **no CPU-store writer**.

> **Correction (session 123) - this measurement is invalid, and the conclusion with it.** `PS2Runtime::Store8/16/32/64`
> is bypassed for ordinary RAM: generated code writes RAM through the `WRITE8/16/32/64/128` macros in
> `ps2_runtime_macros.h`, which go straight to `FAST_WRITE*` and only call `ps2TraceGuestWrite`. A watch inside
> `StoreN` therefore sees *special* addresses only, and `[jobtable] lines: 0` measured nothing at all. The
> pre-existing `[slot2]`/`[ptr]` watches kept working only because they live in `ps2TraceGuestWrite`. Correct
> layer for a guest-data write watch is that header path or a game-function override - session 123 uses the
> override and immediately sees the writer.

One-shot dump at the first wait (new, inside `jobObjProbe`), diffed against PCSX2 at the title:

| offset | port | PCSX2 |
|---|---|---|
| +0x00 entry0 status | `00000000` | `00000000` |
| +0x04 | `0000FFFF` | `FF0E0000` |
| +0x08 | `00000000` | `00000002` |
| +0x14 | `00623100` | `00623100` |

The status byte is 0 in **both**, so the table is transient state and "bit 1" is a symptom: PCSX2 sets it during
load and clears it again. Note the port's table is not all zero (`+0x04`, `+0x14`), so something other than a
CPU store also reached it (ELF image, DMA/IOP, or the bulk `memcpy` in `gameFileRead`).

### Static xref: the whole job-table API is a 20-function cluster

Every reference to the table appears as `lui 0x4F; addiu -0x6EC0` (0x4F0000-0x6EC0 = 0x4E9140). Twenty
functions reference it; the ones around the reader form the API:
`0x2E07D0, 0x2E0820(read), 0x2E0880, 0x2E0950, 0x2E09E0, 0x2E0A70, 0x2E1040, 0x2E1700, 0x2E1A40, 0x2E1D30`.
`FUN_002E1D30` is a **writer**: it computes `s2 = 0x4E9140 + index*0x18` (3*index*8) and
`s1 = entry+6`, reads the status byte, masks with `-3`, and stores both `status (s2+0)` and `entry+6 (s1+0)` -
exactly the two fields `FUN_002E0820` consumes.

### Dynamic: the port runs almost none of that API

`TOL_TRACE_PC` on the ten functions over 50 s: only two entries, once each, both with **index 0**:

```
[pc] enter 0x002E07D0 a0=0x00000000 a1=0x00000001 a2=0x0000013F a3=0xFFFFFFE7
[pc] enter 0x002E09E0 a0=0x00407D70 a1=0 a2=0 a3=0
```

`0x2E1D30` (and `0x2E1A40`, `0x2E1700`, `0x2E1040`, `0x2E0A70`, `0x2E0950`, `0x2E0880`) are **never entered**, so
the status bit is never set because the *setter is never called* - not because a store went missing.

> **Correction (session 123) - the second wrong claim.** `0x2E1A40`/`0x2E1D30` are not entry points that
> something fails to call; they are interior rows of one body whose real entry is `0x2E15C0`, and the reason no
> caller exists is that there is nothing there to call (no `jal`, no pointer in the ELF image, no `lui/addiu`
> materialisation for either address in the whole 53k-function corpus). Saying "the setter is never called"
> understated the problem. More importantly, session 123 found the actual bit-1 writer in a *different* member of
> the same family: `0x2E07D0` is a test-and-set of bit 1, and the port does enter it.

### Probe caveat found

`0x2E0820` also shows no dispatch entry, because `0x2E1EF0` is a **tail-jump thunk** (`j FUN_002E0820`) whose
generated body calls `FUN_002e0820_0x2e0820` directly in C++, bypassing the function table - the same trap as
the `0x1F7340` thunk in session 87. Dispatch-based probes are blind to it; hook the thunk if it must be observed.

### PCSX2 side (armed, not yet caught)

Watchpoint armed on `0x4E9140..0x4E9158` with `onchange` (the wide write watch had been parked on a BIOS
zero-fill loop: `s0=0x4E9140, t9=0x27, sq v0,(s0)` at `0x8000E3C8`, which clears exactly 39x16 = 624 = 26x0x18
bytes - a decoy, it can only clear). A reset drops debugger state (watchpoints disappear), so the order must be
reset -> arm -> resume.

## Session 123 (2026-11-17) - the measured producer is a *clear*; the bit-1 writer is a test-and-set the port already reaches

### The instrument was on the wrong layer (corrects session 122)

`PS2Runtime::Store8/16/32/64` is **bypassed for ordinary RAM**: generated code writes RAM through the
`WRITE8/16/32/64/128` macros in `ps2_runtime_macros.h`, which go straight to `FAST_WRITE*` and only call
`ps2TraceGuestWrite`. `TOL_WATCH_JOBTABLE` therefore saw special addresses only, and `[jobtable] lines: 0`
measured nothing. A guest-data write watch belongs in `ps2TraceGuestWrite` or (no PCH cost) in a
`replaceFunction` override. Replaced with three overrides in `tol_overrides.cpp`:

| override | what it logs |
|---|---|
| `0x2DEF60` | `a0` (entry), status byte before -> after, `ra`, change counter |
| `0x2E1D30` | `a0`, entry-0 status, `ra` (tests whether the row tail runs at all) |
| `0x2E07D0` | `index`, status byte before -> after, bit 1, return value |

### Measured: the producer captured in PCSX2 writes zero (the clear-not-set result)

```
[jobpoke] #1 entry=0x004E9140 slot=0 status 0 -> 0 ra=0x002DEEE4 changes=0
[jobtail]  0 calls
```

`FUN_002DEF60` runs **exactly once**, on slot 0, and writes `0`. Its masks are built from `$zero`:
`and a1, v1, -2` clears bit 0, `or v1, v1, $zero` sets nothing, `sb v1, 0(a2)`. So it **initialises/clears
bit 0** and is structurally incapable of setting bit 1. The chain captured from PCSX2 last session
(`0x165E70` -> `0x2DEED0` -> `0x2DEF60`, `sb` at `0x2DEF98`) is an *init*, not the set - which is why matching
it exactly in the port changed nothing, and why PCSX2 also reads `00` at the title. **There is no missing store
here to find; that call site was the wrong one.**

### The `0x2E1D30` hunt was chasing a row tail

The rows are contiguous and each one falls into the next:

```
0x2E1040-0x2E15C0   0x2E15C0-0x2E1700   0x2E1700-0x2E1920   0x2E1920-0x2E1A40
0x2E1A40-0x2E1D30   0x2E1D30-0x2E1EA0   0x2E1EA0-0x2E1EF0   thunk 0x2E1EF0-0x2E1F00
```

- `FUN_002E1920` ends with `ctx->pc = 0x2E1A40u;` (lines 649, 671)
- `FUN_002E1A40` ends with `ctx->pc = 0x2e1d30u;`
- `FUN_002E1700` ends with `ctx->pc = 0x2E1920u;`

So this is **one body split into five rows**; the real entry is `0x2E15C0`, called only from `FUN_002E0A70`
(0x2E0C28, 0x2E0F78) and `FUN_002E1040` (0x2E1184, 0x2E14D4) - and **neither of those has a `jal` caller
either**. Nothing calls `0x2E1A40`/`0x2E1D30` by any mechanism: no `jal`, no pointer in the ELF file image, no
`lui/addiu` materialisation.

Port: `TOL_TRACE_PC` on `0x2E1700, 0x2E1920, 0x2E1A40, 0x2E1D30, 0x2E1EA0, 0x2E1F00` over 50 s -> **zero enters**.

Not the module's step either: the stuck object's vtable `0x3DA320` methods live at `0x2E93xx`/`0x3656xx`, and its
child's `0x3D7630` is all `0x30xxxx`. Neither contains the chain. The module step is `FUN_00365630`, phase 1
`FUN_00365700`, which calls the *reader* through the thunk `0x2E1EF0 -> 0x2E0820`.

### The actual bit-1 producer: `0x2E07D0` is a test-and-set

Decompiled by hand from `FUN_002e07d0_0x2e07d0.cpp`:

```c
a2 = 0x4E9140 + index*0x18;          // (a0*2 + a0) << 3
a1 = *(u8*)a2;                        // status byte
v0 = (a1 >> 1) & 1;                   // dsll32 a1,30  /  dsrl32 v0,31
if (v0 == 0) {
    a1 = (a1 & ~2) | 2;               // daddiu a0,0,2 ; and v1,a1,-3 ; or v1,v1,a0
    *(u8*)a2 = a1;                    // sb v1, 0(a2)  at 0x2E0814
    return 1;
}
return 0;                             // already set
```

This is the bit the whole boot waits on, and it is a **leaf that the family's veneer calls**: every wrapper in
`0x2E1F00, 0x2E1F80, 0x2E2000, 0x2E2090, 0x2E2120, 0x2E21B0, 0x2E2240` does `jal 0x2E07D0` as its first call
(grep for `0x2E07D0u,` -> 7 hits, one per wrapper). That is why every earlier xref search came up empty: the
producer is not reachable by name from the job code, it is a service routine behind a veneer.

The port **does** enter it - session 122 recorded `[pc] enter 0x002E07D0 a0=0x00000000` - so by this code the
port must leave `0x02` at `0x4E9140`. The job-table dump still reads `00`, which is the contradiction the new
`[jobref]` probe measures.

### Side notes (both corrected as requested)

1. **`Option A` / dense-corpus is not implicated here.** These rows really are split, but the split is
   *traversable*: each boundary carries an explicit `ctx->pc = <next row>` fall-through, so a body that is
   entered runs through all five rows. The harmful variant from sessions 85-90 was a row entered from outside;
   this one is never entered at all, which no amount of row-merging would fix.
2. **`[jobtail] 0 calls` is expected, not a defect.** `0x2E1D30` is a row tail, unreachable by design. It stays
   installed as a cheap assertion: if it ever fires, the row map changed.

### Follow-up in the same session: the write watch named the writers, and the blocker is *ordering*

Two probes were added after the first write-up, and both changed the conclusion.

**1. Bit 1 is not the gate - falsified by measurement.** `[jobref]` on `0x2E07D0` showed the port
test-and-sets entry 0 correctly (`status 00 -> 02 bit1=1 ret=1 ra=0x002E1F98`), and `[jobobj]` correlates the
byte with the phase per frame:

```
[jobobj] #1 ... phase=1 inner=0 ret=0 jt0=00 bit1=0     <- parks
          [jobref] sets bit 1
[jobobj] #2+ ... phase=1 inner=1 ret=0 jt0=02 bit1=1    <- bit set, still stuck forever
```

So "the boot waits on bit 1" was the wrong reading. Reading `FUN_002E0820` properly gives the real semantics:

```c
index -> a2 = 0x4E9140 + index*0x18
if (bit1(*(u8*)a2) == 0) { *(u32*)a1 = *(int16*)(0x4E9146 + index); return 1; }   // deliver payload
return 0;                                                                          // bit1 set -> no payload
```

**bit 1 means *claimed/consumed*.** The payload is the `u16` at `entry+6`, handed to `&this+0x80` - the very
field `FUN_002E1D30` writes, which closes the loop with session 121.

**2. The write watch named the writers.** New `TOL_WATCH_JOBTABLE_RAM` in `ps2TraceGuestWrite`
(`ps2_runtime.h`) - the only layer that sees RAM stores. 87 writes to `0x4E9140..0x4E93B0` in 40 s, and
`entry0+6` has exactly three writers:

| pc | writer | note |
|---|---|---|
| `0x002DEFC4` | `FUN_002DEF60` | entry initialiser, writes 0 |
| `0x002E0A38` | row `0x2E09E0-0x2E0A70` | the submit API - **it does run** (`a0=0x00407D70`) |
| `0x002DF7BC` | `FUN_002DF6D0` | delay-slot store, target `0x4E9146+s0` |

**3. The timeline is inverted.**

```
177  0x2DEFC4  FUN_002DEF60 init      entry0+6 := 0
230  jobobj #1 CONSUMER PARKS         status 00, entry0+6 = 0000
232  0x2E0814  test-and-set           bit 1 := 1     <- claim, from FUN_002e1f80 (ra=0x2E1F98)
235  0x2E0A38  submit                 entry0+6 written  <- payload arrives
242  0x2DF6D0  ...                    (writes 0x9D2286, unrelated)
246  0x2DF7BC  ...                    entry0+6 written
```

The port **claims the slot before the payload is submitted**, and the reader refuses to deliver once bit 1 is
set - permanently. In PCSX2 the order is submit -> read (bit1 clear, payload `0xFF0E`) -> claim, which is why
the title dump shows `00` in the status byte *and* `0xFF0E` at `+6`.

Both calls are inside `FUN_00365700`: the claim at `0x365838` (`jal 0x2E1F80`, preceded by `sw $zero,0x78(s0)`)
and the read at `0x3659B4` (`jal 0x2E1EF0`). Same function, claim path taken first, so the divergence is which
phase branch runs and in what order relative to the `0x2E09E0` submit - not a missing function.

**Probe-design lesson (repeat of an old one):** `[jobslot6]` read `s0` at function entry and therefore computed
`0x4E9146 + 0x4E9140 = 0x009D2286`, a nonsense address. `s0` is set *inside* `FUN_002DF6D0`, so an entry-time
probe cannot validate that store. Only the header-level write watch could.

### Next

Two candidate fixes, and the measurement points at the second:

1. The claim at `0x365838` fires one phase too early - find its branch test and compare with the read path at
   `0x3659B4`.
2. The `0x2E09E0` submit is executed *after* `FUN_00365700` has already claimed; in the guest the submit belongs
   to the frame before. Fix the ordering (who services the AppCell daemon TCB `0x407790` first), not the table.

Either way the target state is checkable in one run: `entry0+6 != 0` while `bit1 == 0` at the moment the
consumer checks, i.e. `[jobobj] jt0=00` together with a nonzero `+6`.

## Session 124 (2026-11-17) - TCB/context ordering trace: the daemon runs first and is *not* the submitter; the dispatcher is

New probe: `TOL_TRACE_ORDER` (`orderEvent` + wrappers in `tol_overrides.cpp`) gives one total order across all
probes with a sequence number, and guest `sp` as the context discriminator (each guest thread has its own
stack). Instrumented: `0x165E70` (frame), `0x164F40` (TCB slot 3 entry, AppCell daemon), `0x165210` (generic job
entry), `0x2E09E0` (submit row), `0x2E1EF0` (reader thunk), plus `claim` emitted from `jobTableRefProbe`.

### Measured order (single init pass; the frame counter never advances past 1)

```
#1  f1 frame    sp=0x01FFFFD0  ra=0x00165B14                      <- one-shot init call, not a loop
#2  f1 daemon   sp=0x01FFFF80  ra=0x00151FC8  a0=0x00407950      <- TCB slot 3 (0x407790), job obj slot3+0x1C0
#3  f1 appjob   sp=0x01FFFF80  ra=0x00151FC8  a0=0x00407740      <- slot 2
#4  f1 appjob   sp=0x01FFFF80  ra=0x00151FC8  a0=0x00407B60      <- slot 4
#5  f1 claim    sp=0x00423450  ra=0x002E1F98                      <- phase machine, app-job stack
#6  f1 submit   sp=0x00417550  ra=0x00151F88  a0=0x00407D70      <- different dispatch site, slot 5 job obj
#7+ f1 read     sp=0x00423480  ra=0x003659BC  a0=0, a1=0x0063E450 <- repeats forever, identical args
```

`[tcb] f1 slot3 state=0 prio=0 | slot4 state=0 prio=0` - so at this pass the entries are dispatched through
`FUN_00151E90` (`0x151FC8`) with the TCB blocks, and `state` only becomes 1 afterwards (as seen in earlier
sessions).

### What the daemon does and does not do

- TCB slot 3 (`0x407790`, entry `0x164F40`) **does run**, once, *before* the app job, with `a0 = 0x00407950`
  (= slot3 + `0x1C0`, the per-slot job object, same layout as `[slot2]`/`[ptr]` probes).
- It is **not** the submitter. The submit walks a different context (`sp=0x00417550`) with
  `a0 = 0x00407D70` = **slot 5** + `0x1C0`. So slot 5 is the thread that writes the table.
- The claim and all reads come from one context (`sp=0x004234xx`), i.e. the app-job phase machine, which runs
  before slot 5 is dispatched.

### `0x2E09E0` is an interior row (again), and that explains the ra

`FUN_002E0950` ends with `ctx->pc = 0x2E09E0u;` (lines 227, 249), so `0x2E09E0` is only the *next row* of the
chain `0x2E0880 -> 0x2E08D0 -> 0x2E0950 -> 0x2E09E0 -> 0x2E0A70`. This is the third time in two sessions that an
"API function with no callers" turned out to be a row boundary (cf. `0x2E1A40`/`0x2E1D30`).

Consequence: `ra=0x00151F88` at the `0x2E09E0` boundary belongs to whoever entered the **chain head**, not to the
row - and it does not match either dispatcher `jal` cleanly (`0x151F80: jal func_152200` would give
`ra=0x151F84`; `0x151F88: jal func_15253C` would give `ra=0x151F8C`). So the submit is reached through one of
those two dispatcher paths, via an intermediate that preserves `ra`, and the exact one is still open.

### Answer to "why does submission happen after the claim"

Because the two live in different dispatch services of the **same** `FUN_00151E90` pass, and the port takes them
in the wrong order:

1. the thread dispatch (`0x151FC8`) runs slots 2/3/4 to their blocking points - slot 4's phase machine claims
   (`0x2E0814`) and then parks at `0x3661DC` in `while (*(u8*)(s1+8))`;
2. only afterwards does the dispatcher's other path (`0x151F80`/`0x151F88` -> ... -> row `0x2E09E0`) run the
   slot-5 submit, writing `entry0+6` at `0x2E0A38`.

Since the claimant parks immediately after claiming, the payload lands after the only context that could consume
it has stopped running. Nothing re-checks: that is the deadlock, and it is an ordering defect, not a missing
function or a missing store.

### Next

1. Identify which dispatcher path reaches the `0x2E0880` chain head (`0x152200` vs `0x15253C`) and whether slot 5
   is meant to be dispatched before slot 4 (TCB priority/ring position). `state=0/prio=0` at the pass suggests
   the ring is walked in slot order, while the guest expects the submitter earlier - check the guest's TCB
   priorities once `state` is 1, and the ring order in `EeScheduler.cpp`.
2. Cheaper intermediate check: log `FUN_00151E90`'s own pc progression (`0x151F80`, `0x151F88`, `0x151FC8`) with
   `orderEvent` to see the loop shape and confirm which service runs first.

## Session 125 (2026-11-17) - dispatcher progression: service A (`0x152200`) runs the submitter, by tail-jump, after the TCB dispatch round

Loaded the `ps2-recomp` skill for this one; two of its prohibitions shaped the method - **no header edits** (the
earlier `ps2_runtime.h` watch cost a 13,560-file rebuild) and **never touch `runner/*.cpp`**. The three PCs are
logged by wrapping the functions called *at* them, which is header-free and lives in the override file.

### The pump loop, from the disassembly

```
0x151F78  jal func_15243C     <- iteration top          (wrapped as "iter")
0x151F80  jal func_152200     <- service A              (wrapped as "dispA", ra := 0x151F88)
0x151F88  jal func_15253C     <- service B              (wrapped as "dispB", ra := 0x151F90)
0x151FC0  jalr $v0            <- dispatch one TCB entry, delay slot 0x151FC4: a0 = s0 + 0x1C0 (ra := 0x151FC8)
0x151FC8  b   0x151F78        <- loop back edge (only a branch, so an iteration is observed via its top marker)
```

`jal` sets `ra = pc + 8`, which makes the three paths distinguishable purely from `ra` at every event.

### Measured progression

```
#1  frame   sp=0x01FFFFD0  ra=0x00165B14
#2  iter    sp=0x01FFFF50  ra=0x00151F80  a0=1 a1=1
#3  dispA   sp=0x01FFFF30  ra=0x00151F88  a0=1 a1=1        <- service A, nothing to do yet
#4  dispB   sp=0x00411480  ra=0x00151F90                   <- service B, on a THREAD stack
#5  daemon  sp=0x01FFFF80  ra=0x00151FC8  a0=0x00407950    <- TCB dispatch, slot 3
#6  appjob  sp=0x01FFFF80  ra=0x00151FC8  a0=0x00407740    <- slot 2
#7  appjob  sp=0x01FFFF80  ra=0x00151FC8  a0=0x00407B60    <- slot 4
#8  claim   sp=0x00423450  ra=0x002E1F98                   <- inside slot 4's run
#9  submit  sp=0x00417550  ra=0x00151F88  a0=0x00407D70    <- SERVICE A's ra, slot 5 context
#10 read    sp=0x00423480  ra=0x003659BC                   <- and then polls forever
#11 iter    sp=0x01FFFF80  ra=0x00151F80                   <- next outer iteration
```

### Confirmed: which path runs the submitter

**Service A - `0x152200`, called at `0x151F80`.** The submit event carries `ra=0x00151F88`, and the only
instruction that produces that value is the `jal` at `0x151F80` (MIPS `ra = pc + 8`). The TCB dispatch at
`0x151FC0` produces `ra=0x00151FC8`, which is exactly what the daemon and both app jobs show - so the submit is
*not* a TCB entry dispatched by the `jalr`.

Two further facts pin the mechanism:

- `FUN_00152200` contains **no `jal` at all** (grep for `dispatchGuestBranch` on a direct target: 0 matches). It
  only calls through function pointers, so it is a **work-queue drainer**, and it reaches the
  `0x2E0880 -> 0x2E08D0 -> 0x2E0950 -> 0x2E09E0` chain by **tail-jump** (`jr`, not `jalr`), which is why the
  chain's `ra` is still the pump's `0x151F88` rather than something inside service A.
- `dispB` is entered with **thread** stacks (`0x00411480`, `0x00423520`, `0x004172D0`), while `dispA` always runs
  on the init stack (`0x01FFFF50`/`0x01FFFF80`). So the pump is **re-entered recursively from inside a guest
  thread** - the guest's yield re-enters `FUN_00151E90` on the yielding thread's own stack.

### Why the submission lands after the claim (mechanism, not mystery)

The pump's per-iteration order is A -> B -> dispatch, and service A is the one that runs the submitter. In this
pass the work dequeued by A is a "run TCB" item that recursively re-enters the pump, which dispatches slots
3/2/4 (`ra=0x151FC8`); slot 4's phase machine claims entry 0 and parks. Only when that nested run unwinds does
service A's own tail-jumped work item execute - the submit - at `#9`. So the inversion is **queue order inside
service A**, not a wrong `jal` order and not a missing store.

Also measured: the pump spins with `a0=1,a1=1` for hundreds of iterations before dispatching anything (that is
why the trace now suppresses spin after 10 lines and caps `read` at 12 - otherwise 600 events were burned before
the first TCB ran).

### What this means for the port (not an emulator)

PCSX2 runs this same guest code without deadlocking, so the guest's queue order is correct and the port must not
"fix" the order. What differs is that in the guest the parked thread is **resumable**: it parks in
`while (*(u8*)(s1+8))` at `0x3661DC`, the yield re-enters the pump on its own stack (observed at `#4`/`#17`), and
after the submit it is re-dispatched and re-checks - now finding `entry0+6 != 0` with bit 1 still clear.

So the defect is in the **port's guest-thread scheduling**: after the claim the thread stops being re-dispatched,
the nested pump never unwinds back into it, and the payload submitted at `#9` is never observed. That is the same
shape as the session-120 note that the main thread never yields to the worker, and it belongs in the runtime's
guest-thread/yield support - not in the recompiled code and not in a table poke.

### Next

1. **Resume path:** instrument the parked thread. It is a TCB whose `state` is 1 and whose update is called from
   `0x151FC0`; check whether the runtime ever re-enters it after `#9`. If not, that is the bug.
2. If it is re-entered and still fails, the re-check reads a stale copy (the `&this+0x80` payload written by the
   earlier read at `#10`), and the fix is in that buffer, not in the job table.
3. Do **not** reorder the queue or pre-seed `entry0+6` - both would make the port diverge from the guest on a path
   PCSX2 proves correct.

## Session 126 (2026-11-17) - resume hypothesis FALSIFIED: the thread is scheduled, it spin-waits in the phase machine and never reaches its yield

Probes: `yield` on `0x15231C` (the park loop's yield at `0x3661D4`, `ra := 0x3661DC`) and `park` on `0x366090`
(`FUN_00366090`, the app-job step). The park loop does not spin on a word - it calls the scheduler:

```
0x3661CC  jalr $t9
0x3661D4  jal  func_15231C     <- yield, same 0x152xxx family as service A/B and the loop top
0x3661DC  ...                  <- the wait site seen in earlier sessions
```

### Measured (85 s, TOL_TRACE_ORDER)

```
dispA 268 | read 12 (capped) | appjob 2 | claim 1 | submit 1 | frame 1 | daemon 1 | iter 1 | park 1 | YIELD 0
```

- `yield = 0`: `0x15231C` is **never entered**, so the cooperative yield is never executed.
- `park = 1`: `FUN_00366090` runs once.
- `read` repeats with a constant context (`sp=0x00423480`, `ra=0x003659BC`), and `dispA` runs 268 times.

**Conclusion: the app-job thread is not parked-and-forgotten.** Both it and the pump are being scheduled over and
over; the thread simply spins in the phase machine's read path, never reaching the yield at `0x3661D4`. The
session-125 recommendation ("resume the parked thread") was therefore wrong and is retracted here - the
scheduler is already round-robining both contexts.

### What this leaves

The thread is alive, the submit happened (`#11`), and the read still returns 0 every iteration because **bit 1 is
set**. So the fairness/resume layer is not the defect; the defect is that the phase machine takes the **claim
branch before the read branch** in the same pass, and once claimed, the reader's contract
(`if (bit1) return 0;`) can never deliver - no matter how many times the thread is rescheduled.

That is now the single question to settle against the emulator, and it is a state question, not a scheduling one:
at the analogous instant, what are `obj+0x70` (phase), `obj+0x74` (inner), `obj+0x78`, `obj+0x7C` in PCSX2, and
which branch does that imply? Port values from `[jobobj]` at the park are `phase=1 next=0xFFFF inner=1
+78=0 +7C=0 +84=0`.

### Emulator check (deferred - PCSX2 restarted this session)

PCSX2 was found paused at `0x002DEF98` (the `sb` of the entry initialiser, i.e. a stale breakpoint on the
initialiser, not the interesting site) and its MCP connection wedged; user restarted it. On reconnect:

1. **Do not** re-arm `0x2DEF98` - it fires during entry init and only parks the VM.
2. Arm `0x3661DC` (hit count) to confirm the guest *does* loop there and yield, and note `a0`/`s1`.
3. At that stop, read `0x4E9140` (status + `entry+6`) and the object `0x63E3D0` fields `+0x70/+0x74/+0x78/+0x7C`,
   then compare with the port's `[jobobj]` line at its park. The branch difference will show whether the port's
   phase/flags diverge before the claim or the claim itself is mis-ordered.

## Session 127 (2026-11-17) - ROOT CAUSE: the claim is a *reserve*; the release (`0x2E1D30`) never runs, so bit 1 is set and never cleared

### The phase machine does not claim - measured

`FUN_00365700` at `0x365818..0x365838`:

```
0x365818  a0 = *(s0+0x18)        // child object 0x63E610
0x36581C  t9 = *(a0)             // child vtable 0x3D7630
0x365820  t9 = *(t9+0x10)        // -> FUN_003059B0
0x365824  jalr $t9  (a1 = *(s0+4))
0x36582C  bnel $v0,$zero,0x365B9C   // nonzero => success path, skip the claim
0x365834  a0 = 0 ; 0x365838 jal 0x2E1F80 ; delay *(s0+0x78) = 0
```

New probe `[childstep]` on `0x003059B0`:

```
[childstep] #1 child=0x0063E610 a1=0x0063DA00 ret=0x0063EB00 ra=0x0036582C
```

`ret` is **nonzero** (a pointer), so the `bnel` takes `0x365B9C` and `0x365838` is **never reached**. The phase
machine is not the claimant. `0x2E1F80` has exactly two call sites - `0x365838` (dead here) and **`0x321334`** in
`FUN_003211C0` - so the claim comes from `FUN_003211C0`.

### Corrected role of each piece

| role | code | effect on job-table entry 0 |
|---|---|---|
| reserve | `FUN_003211C0` (`0x321334`) -> `0x2E1F80` -> `0x2E07D0` | **sets bit 1** (`sb` at `0x2E0814`) |
| fill | row `0x2E09E0` (`0x2E08D0/0x2E0950` fall-through), write at `0x2E0A38` | writes `entry0+6` |
| consume | `FUN_002E0820` via thunk `0x2E1EF0`, called at `0x3659B4` | returns payload **only while bit 1 is clear** |
| **release** | `0x2E1D30`: `status & ~2` (mask `-3`) + `entry+6` | **clears bit 1** - **never runs in the port** |

So bit 1 means "reserved"; the reader is right to answer 0 while it is set. The port reserves and fills, but the
release - `0x2E1D30`, the tail of the `0x2E15C0` body identified in sessions 122-123 as having no callers - never
executes. That is the deadlock, and it re-validates those sessions' target from the other direction: the row we
could not reach is the *release*, not a setter.

### Consequence for the fix

Nothing is wrong with dispatch order, the queue, the yield, or the job table. The guest must run the `0x2E15C0`
body (the release) after the job completes, and the port never does. `0x2E15C0`'s only callers are `0x2E0A70`
(x2) and `0x2E1040` (x2); neither has a `jal` caller, so the entry into that chain is indirect/tail - the same
row-boundary pattern. Next: instrument `FUN_003211C0` (the reserver) to see whether it also *should* be the
releaser's driver, and watch the status byte after the submit to confirm nothing ever clears bit 1.

### Emulator status this session

PCSX2 wedged at process level: DebugServer returned a parse error and Pine timed out on port 28011, and the
reconnect still reported `PC=0x002DEF98, paused=true` - i.e. the paused save state was restored again. Needs a
hard process kill and a fresh boot without the debugger attached. Port-side evidence above did not depend on it.

## Session 128 (2026-11-17) - FUN_003211C0 is never entered; the claim is on BOTH paths (session-127 reading corrected); the release is still the only missing piece

### FUN_003211C0: instrumented, never runs

Probes `resc` (`0x3211C0`), `subm` (`0x2E0880`), `ent0` (`0x1C8660`), `clm2` (`0x2E1F80`, to capture the outer ra):

```
[resc]  0 lines        <- FUN_003211C0 is NEVER entered
[subm]  never          <- 0x2E0880 never entered
[ent0]  never          <- 0x1C8660 never entered
[clm2]  #10 ra=0x00365840   <- the reserve comes from the PHASE MACHINE (jal at 0x365838)
```

Statically it could not have reached the release anyway: its complete direct call graph is
`0x153120, 0x324C10, 0x3240D0, 0x324700, 0x281F50, 0x1F31E0, 0x1F32C0, 0x1F3370, 0x3076F0, 0x307710, 0x307760,
0x307770, 0x3076E0, 0x1EC540, 0x1EE2A0, 0x2E1F80 (reserve), 0x2E1EF0 (read), 0x2E0880 (submit), 0x1C8660,
0x324B50, 0x300000` - and **no `0x2E0A70`, `0x2E1040` or `0x2E15C0`**, the only callers of the release.

### No interior re-entry (resume-at-delay-slot theory falsified)

`[pm]` on `0x365700`: 25+ entries, **every one** `entrypc=0x00365700 ra=0x0036567C`. So the emitted special case
`label_365830: if (ctx->pc == 0x365830) { delay; goto label_365834; }` is never exercised, and the phase machine
is not being re-entered at an interior pc.

### Why the child step returned nonzero yet the claim still ran

`0x365838` is reachable from **two** directions, and the second one is the success path:

```
0x36582C  bnel $v0,$zero,0x365B9C      // success
0x365B9C  ... jalr  (ra = 0x365BA8)
0x365BA8  b   0x365834                 // unconditional, .+4+(-0xDE<<2) -> 0x365834
0x365BAC    delay: *(s0+4) = 0
0x365834  a0 = 0
0x365838  jal 0x2E1F80                 // claim
```

Failure falls into `0x365834`; success loops back to it after clearing `*(s0+4)`. **Both paths claim.** So the
claim is normal guest behaviour on every iteration, and session 127's "claim is the failing-step fallback" is
wrong and corrected here. `FUN_003059B0` returning `0x63EB00` is consistent with what follows.

### What is actually left

The claim sets bit 1 every iteration (test-and-set, so only the first one matters), the submit fills
`entry0+6`, and the reader correctly refuses while bit 1 is set. The guest must therefore clear bit 1 between
iterations - the release (`0x2E1D30`, mask `-3`) - and in the port nothing ever does:

```
guest loop:  claim(bit1=1) -> fill(entry+6) -> read -> release(bit1=0) -> read succeeds
port loop:   claim(bit1=1) -> fill(entry+6) -> read(0) -> read(0) -> read(0) ...
```

Note also that `0x2E0880` was never entered, so the write at `0x2E0A38` comes from a work item the pump
tail-jumps into **directly at `0x2E09E0`**, not from the chain head. The release is presumably another such work
item (`0x2E0A70` or `0x2E15C0`) that is never dequeued.

### Next

1. Probe `0x2E0A70` and `0x2E15C0` entries (expected 0) to confirm the release work item is never dequeued.
2. Instrument `FUN_00152200` (service A, the queue drainer) to log the function pointer it dequeues - that names
   the work item that *is* dequeued (`0x2E09E0`) and shows whether a release item is ever queued behind it.
3. Do not "fix" this by clearing bit 1 or writing `entry0+6` from an override until the guest's own release path
   is proven unreachable for a port-specific reason (e.g. a queue entry the runtime drops).

## Session 129 (2026-11-17) - ANSWER: the release job is NEVER QUEUED (not dropped); the submit is queued once

Two probes: `relA` (`0x2E0A70`), `relB` (`0x2E15C0`), plus `TOL_TRACE_INDIRECT` in `PS2Runtime::dispatchGuestBranch`
(`ps2_runtime.cpp`, env-gated, filtered to `sourcePc` in `0x152200..0x152700`).

### Release heads: both dead

```
relA = 0   (0x2E0A70 never entered)
relB = 0   (0x2E15C0 never entered)
```

### The pump's work list (81 dispatches, all from one tail-jump site)

```
[ind] from=0x00152314 -> target   kind=2   ra=0x00151F88   (ra unchanged => `jr`, not `jalr`)
```

15 distinct targets, complete set:

| count | target | note |
|---|---|---|
| 19 | `0x1F8528` | transfer-queue worker |
| 18 | `0x1F8168` | transfer-queue worker |
| 11 | `0x3661DC` | **the app-job park PC - re-dispatched 11x** |
| 10 | `0x164FE8` | AppCell loader |
| 6 | `0x1F86B8` | transfer worker |
| 4 | `0x2047D0` | the other park site (session 121) |
| 3 | `0x233FEC` | |
| 2 | `0x165220` | AppCell job |
| 2 | `0x1660EC` | frame region |
| 1 | `0x1F8150`, `0x164FCC`, `0x1660A0`, `0x1F84B0` | |
| 1 | **`0x2E09E0`** | **the submit - queued exactly once** |
| 1 | `0x164F80` | loader per-frame loop |
| - | **no `0x2E0A70`, no `0x2E15C0`** | **the release is never queued** |

### Conclusion

The release is **never queued**, not dropped: the queue is drained completely (81 dispatches, every item printed),
the submit appears in it once, and neither release head ever does. `0x3661DC` appearing 11 times also settles the
resume question from the other side - the parked thread is re-dispatched at its wait PC repeatedly.

So the job completes, the submit is queued and run, and the step that would enqueue the release never executes.

### Next

1. Find the **enqueue** counterpart of `0x152314` (whose `jr` drains the list) and instrument it, then identify which
   producer should push a release item. `0x15231C` (the yield) and `0x15243C` (loop top) are the likely siblings.
2. Compare against the guest: the release enqueue is almost certainly tied to the submit completing - if the port
   never observes the completion (e.g. a status word the guest sets from an interrupt or the SPU/DMA path), the
   enqueue is skipped. That is where a game override would belong, not in the job table.

## Session 130 (2026-11-17) - ROOT CAUSE FOUND: the submit thread is suspended at `jal 0x2DF590` and never resumed, so the release at `0x2E0A58` never runs

### Correction to session 129: `0x152200` is a context restore, not a queue drainer

```
0x152200  lui $at,0x3E ; lw $t1,-0x4E60(at)   -> index     @0x3DB1A0
0x152208  lui $at,0x3E ; lw $t2,-0x4E50(at)   -> stride    @0x3DB1B0
0x152214  lui $t0,0x49 ; addiu $t0,-0xA70      -> base 0x48F590
0x152220  lw $t3,-0x4E40(at)                  -> third term @0x3DB1C0
0x15222C  lw $t0,0($t0)                        -> context block
0x152230  sw $ra,0x1A8($t0) ; sw $zero,0x1AC($t0)
0x152238+ lq $v0..$fp from 0($t0)              -> restore every GPR
0x152314  jr                                    -> resume that thread
```

So the 15 `[ind]` targets are **thread context PCs** (the TCB save area at `0x48F590`), not queued work items:
`0x3661DC` is slot 4's suspended PC, the `0x1F8xxx` are worker threads, `0x164F80/CC/E8` the loader, and
`0x2E09E0` is the submit running as a resumed thread context. A static pointer scan for any of these returns zero
hits (they are all built into `.bss` at runtime), so only the dynamic trace could name them.

### The release is inside the submit row - and it is skipped

The submit row releases its own slot immediately after filling it:

```
0x2E0A30  sb  $v1,0($s0)        status
0x2E0A34  jal func_2DF590
0x2E0A38    delay: sh $v0,0($s1)   entry+6
0x2E0A3C  sh  $v0,0($s1)
0x2E0A4C  addiu $v1,$zero,-3
0x2E0A50  and  $v1,$a1,$v1      status & ~2   <- clears bit 1
0x2E0A58  sb  $v1,0($s0)        <- THE RELEASE
0x2E0A68  jr  $ra
```

Measured writes to `entry 0` from this row:

```
pc=0x002E0A30  WRITE8  addr=0x004E9140      (1)
pc=0x002E0A38  WRITE16 addr=0x004E9146      (1)   <- delay slot, runs before the call
pc=0x002E0A3C  (0)   pc=0x002E0A50  (0)   pc=0x002E0A58  (0)
```

Everything after the call is straight-line, so the only way it is skipped is that control never resumes at
`0x2E0A3C`.

### The suspension, measured

```
[sub2] #1 enter a0=0x00000000 ra=0x002E0A3C
[sub2] #1 ret  v0=0x00000001 nextpc=0x00233FC0   (caller expects 0x002E0A3C)
```

`FUN_002DF590` is called with the correct `ra`, returns `v0=1`, and leaves `ctx->pc = 0x233FC0`. That address is
in the same function as `0x233FEC`, which appears in the scheduled-thread set - so this is a **context
switch/yield inside the call**, not a corrupted `jr ra`. The submit thread is suspended at `0x2E0A3C` and is
never resumed, so `0x2E0A58` never clears bit 1.

### The complete causal chain (final)

```
submit thread runs (as a context at 0x2E09E0)
  -> fills entry+6                                (0x2E0A38, observed)
  -> calls func_2DF590                            (0x2E0A34, observed)
       -> switches context to 0x233FC0            (observed: nextpc=0x233FC0)
  -> submit thread NEVER resumed at 0x2E0A3C      (0x2E0A3C/0x2E0A50/0x2E0A58 = 0 writes)
  -> release (status & ~2) never executes
  -> bit 1 stays set forever
  -> FUN_002E0820 returns 0 by contract ("claimed")
  -> phase machine waits forever; resume, ordering and the job table were never the problem
```

This supersedes sessions 125-128's recommendations: the thread *is* rescheduled (0x3661DC dispatched 11x), the
claim is legitimate, and the missing step was always this one suspended call.

### Next

1. Confirm the suspended context is never re-entered: probe `0x2E0A3C` (wrap the row and log `ctx->pc` at entry) or
   check whether `0x2E0A3C` ever appears in the scheduled-context set. Expect zero - that closes the loop.
2. Fix location: the runtime's context save/switch must record `0x2E0A3C` as the suspend PC for this thread and
   re-schedule it. That is guest-thread support code (`EeScheduler` / context switch), not `runner/*.cpp`, not the
   job table, and not a data poke.

## Session 131 (2026-11-17) - both follow-ups executed: continuation is NEVER recorded in scheduler state (stack only)

### Option 1 - confirmed: the suspended continuation is never scheduled

The complete scheduled-context set (15 PCs, from `TOL_TRACE_INDIRECT`):

```
0x164F80 0x164FCC 0x164FE8 0x165220 0x1660A0 0x1660EC 0x1F8150 0x1F8168
0x1F84B0 0x1F8528 0x1F86B8 0x2047D0 0x233FEC 0x2E09E0 0x3661DC
```

`0x2E0A3C`: **0 occurrences**. `0x2DF5DC`: 0. `0x233E50`: 0. So the resume point of the submit thread never
becomes a schedulable context.

### Option 2 - the save never happened, proven by scan

After the switch (`nextpc=0x233FC0`), a scan of the TCB blocks (`0x407000..0x408000`), the context table
(`0x48F000..0x490000`) and the thread's stack (`0x417000..0x418000`):

```
needle 0x002E0A3C recorded 1 time(s), first at 0x00417510  <- STACK FRAME
needle 0x002DF5DC recorded 1 time(s), first at 0x004174C0  <- STACK FRAME
```

Both are **only** ordinary stack frames (prologue `ra` spills). Neither appears in the context table or any TCB
block, so **no scheduler structure ever captured this thread's continuation**. `0x2DF590` -> `0x233E50` switches
away, and the thread is left with its resume point existing solely on its own stack, which nothing re-enters.

### Fix specification (now fully determined)

- **Where:** the guest's context-save path reached through `func_2DF590 -> func_233E50` (the `0x233xxx` scheduler
  module that also owns the scheduled PC `0x233FEC`), writing into the save area at `0x48F590` keyed by
  `0x3DB1A0` (index) / `0x3DB1B0` (stride) / `0x3DB1C0` (offset). `0x152200` restores from the same table and
  saves its own `ra` at `block+0x1A8`.
- **What:** before switching away, the outgoing thread's resume PC (`0x2DF5DC`, and transitively `0x2E0A3C`) must
  be written into its context block so the scheduler can select it again.
- **Not:** `runner/*.cpp`, the job table, bit 1, `entry0+6`, or a data poke - all previously ruled out by
  measurement.

Because the table is guest RAM written by guest code, the defect is most likely a guest-side divergence inside
that scheduler call (for example a primitive it uses returning a different result), not a missing runtime feature
to bolt on. The next micro-step is to dump the context block for the submit thread (locate it by its saved
`sp` ~ `0x417550`) and see which field should hold the resume PC and what value it actually contains.

## Session 132 (2026-11-17) - emulator corroborates: guest steady state is "filled AND released"; port never reaches it

PCSX2 back up and running this time (`paused=false`). Breakpoint armed at `0x2E0A34` (the submit row's
`jal 0x2DF590`). Job table at the loader stage, read via Pine:

```
004e9140  00 00 00 00 00 00 0e ff 02 00 00 00 00 00 00 00 00 00 00 00 00 31 62 00
```

| field | PCSX2 (guest) | port at its stall |
|---|---|---|
| `entry0` status byte | **`0x00`** (bit 1 **clear**) | **`0x02`** (bit 1 **set**) |
| `entry0+6` (the payload the reader returns) | **`0xFF0E`** | **`0x0000`** |
| `entry0+8` | `2` | `0` |

So the guest's steady state is **filled and released** - the exact state the port never reaches - and it matches
the session-122 reference dump byte for byte.

### What this confirms

The guest does clear bit 1 (the release runs), and `entry0+6` holds a real payload. The port sets the bit, writes a
zero payload, and never releases. Combined with session 131 (the submit thread's continuation `0x2E0A3C` exists
only as a stack frame and is never scheduled), the chain is closed end to end.

### Caveat on the context table read

`0x48F590` reads were **inconsistent**: one read returned bytes byte-identical to the job table
(`00 00 00 00 00 00 0e ff…`), which cannot be right for a context save area, so that path returns wrong data
intermittently. The DebugServer side also fails parse on roughly half of calls (`list_breakpoints`,
`read_registers` both errored with `reading 'length'` / argument type errors). Until that stabilises, the
context-block layout must be established from the port's own dump, not from PCSX2.

### Next

1. Port-side: dump `0x48F590` (`0x400` bytes) at the `[sub2]` event so the layout can be read from a source we
   trust, and locate the submit thread's block by its saved `sp` (`~0x417550`).
2. Then implement the save: write the resume PC into that block before the switch, and verify `0x2E0A58` writes
   (i.e. the release runs) - the single check that proves the deadlock is gone.

## Session 133 (2026-11-17) - `0x48F590` dumped from the port: the TCB is registered, the continuation is not

One-shot dump of `0x48F590..0x48F990` at the `[sub2]` switch (our own memory, since PCSX2's read of this range was
inconsistent).

### Result

```
0048F590 .. 0048F760 : all zero
0048F770: 000001D0 00000000 00000000 00000000
0048F790: 00000001
0048F7A0: 01FFFEE0            <- a stack pointer (pump/init range)
0048F7B0: 00008000
0048F7C0: 00407BB0            <- TCB slot 5 = the SUBMIT thread
0048F7D0: 00000001
...
0048F920: 00407BB0            <- the same TCB referenced again
0048F860+: 3E4CCCCD 3F800000 4B800000 ...   <- float constants: the tail of the range is unrelated data
```

Two conclusions:

1. **`sp_like = 0`** - no `0x417xxx` value exists anywhere in the range, so the submit thread has **no saved
   register block**. Its continuation is not stored.
2. Its **TCB is registered**: `0x407BB0` appears twice (`0x48F7C0`, `0x48F920`). The scheduler knows the thread;
   it simply holds no resume state for it.

The table's real extent is ~`0x2D0` bytes; past that the range is ordinary float constant data, so a full
`0x400` dump includes noise (worth remembering for any future comparison).

### Mechanism note

`FUN_00233E50` (the function that performs the switch, called from `0x2DF590` at `0x2DF5D4`) contains **no
`syscall` instruction** - only the `ps2_syscalls.h` include. So the context switch is guest-implemented, and the
missing save belongs to that guest path rather than to a runtime syscall stub.

### Where the fix must go (unchanged, now with the layout evidence)

The outgoing thread's resume PC (`0x2DF5DC`, and transitively `0x2E0A3C`) must be written into its context block
before the switch, and that block must then be selectable. Today: TCB present, block absent. Acceptance test is a
single line: `0x2E0A58` produces a write and status returns to `0x00`.

## Session 134 (2026-11-17) - TCB layout found (resume PC at `+0x1B4`), and the one-shot poke proves the field is not the whole story

### The layout, from four live TCBs at the switch point

```
slot2 tcb=0x407580  sp@+0xE0=0x004154F0  ra@+0x1A8=0x00151F88  +0x1AC=1  state@+0x1F0=1
slot3 tcb=0x407790  sp@+0xE0=0x00413530  ra@+0x1A8=0x00151F88  +0x1AC=1  state@+0x1F0=1
slot4 tcb=0x4079A0  sp@+0xE0=0x00423500  ra@+0x1A8=0x00151F88  +0x1AC=1  state@+0x1F0=1
                    +0x1B4 = 0x003661DC   <- its PARKED pc: a real suspension point
slot5 tcb=0x407BB0  sp@+0xE0=0x00417550  ra@+0x1A8=0x00151F88  +0x1AC=0  state@+0x1F0=1
                    +0x1B4 = 0x002E09E0   <- its ENTRY pc, never advanced
```

So **`TCB+0x1B4` holds the thread's resume PC**, `+0xE0` the saved sp, `+0x1A8` the saved ra, `+0x1F0` state, and
`+0x1AC` is a per-thread flag that is `1` on every working thread and `0` on slot 5. Working slot 4 shows a
genuine suspension PC; slot 5 still shows the entry it was created with.

### The experiment (opt-in `TOL_TRY_RESUME`, one-shot)

```
[sub2] #1 enter a0=0x00000000 ra=0x002E0A3C
[try] slot5: tcb+0x1B4 := 0x002E0A3C, tcb+0x1AC := 1
[sub2] #1 ret  v0=0x00000001 nextpc=0x00233FC0
```

Result: `0x2E0A3C` 0 writes, `0x2E0A50` 0, `0x2E0A58` 0. **Writing the PC and the flag is not sufficient** - slot 5
is never selected even with a correct continuation recorded.

### What that rules in

The field value is a symptom. Slot 5 is not in the set the scheduler picks from. Candidates, in order of
likelihood:

1. **Ready-list membership.** The `0x48F770`-style record (which at `+0x50` held the TCB pointer `0x00407BB0`) is
   the scheduling record; slot 5's entry likely lacks the runnable marker or is not linked.
2. **Register block emptiness.** If the scheduler restores all GPRs from the TCB on selection, a block that was
   never filled would zero every register - so a correct resume PC alone would still not produce a sane run.
3. The switch path (`0x2DF590 -> 0x233E50`) removes the thread instead of parking it.

### Next

Dump the scheduling record area (`0x48F740..0x48F940`, the two records that reference TCBs) side by side with a
working thread's record, and identify the field that marks a thread runnable/linked. Then the fix writes both that
membership field and `+0x1B4`, which is the pair the experiment shows is required.

## Session 135 (2026-11-17) - RUNNABLE FIELD FOUND: `TCB+0x1F0` bit `0x400` (and the `+0x1FC` link); setting it unblocks the next stage

### The field

`0x48Fxxx` holds no per-thread records (only slot 5 appears there), so the threads are tracked by a **list
embedded in the TCBs**. TCB tail for all four threads at the switch point:

```
offset:     +1F0       +1F8       +1FC       +200      +204       +208
slot2     01000401  00407790   004079A0   00407580  00413560   00415550
slot3     01000401  00407370   00407580   00407790  00411560   00413550
slot4     01000401  00407580   00407BB0   004079A0  00421560   00423550
slot5     01000101  004079A0   00000000   00407BB0  00415560   00417550
```

- **`+0x1F0` is the flags word.** Working threads: `0x01000401`. Slot 5: `0x01000101` - the difference is **bit
  `0x400`**. This is the runnable/ready bit.
- **`+0x1FC` is the next-link**, and it is **null** on slot 5 (unlinked; it is the tail of the chain).
- `+0x204/+0x208` are stack-range words (`0x00413560/0x00415550` style); `+0x1E0` on slot 5 is ASCII
  (`"...LoadInfoThread"`), i.e. a name field, not state.

**Instrumentation lesson:** the long-standing `state@+0x1F0` probe read a **u8**, which returns `0x01` for both
values and hid this for several sessions. The field must be read as u32.

### Experiment: writing the field changes behaviour

With `TOL_TRY_RESUME`, slot 5 gets `+0x1B4 := 0x002E0A3C`, `+0x1AC := 1`, `+0x1F0 := 0x01000401`,
`+0x1FC := 0x004079A0`:

```
[jobtabram] pc=0x002DF788 WRITE16 addr=0x004E9142 off=0x2
[jobtabram] pc=0x002DF798 WRITE16 addr=0x004E9144 off=0x4
[jobtabram] pc=0x002DF7B4 WRITE16 addr=0x004E9140 off=0x0
[jobtabram] pc=0x002DF7BC WRITE16 addr=0x004E9146 off=0x6   <- entry+6
```

`FUN_002DF6D0` is called from `FUN_002DF590` at `0x2DF624`, so **`0x2DF590` now continues past the switch point**
instead of abandoning its caller - a behaviour change caused solely by the flag writes. `0x2E0A3C`/`0x2E0A58`
still did not execute inside the 75 s window (the run was still inside that call chain when killed).

### Conclusion

The membership field is `+0x1F0` bit `0x400` (plus a non-null `+0x1FC`). The earlier "poke insufficient" result
was incomplete rather than wrong: PC-only and flag-only were each insufficient, and PC+flag+link together do
change the scheduler's behaviour. What remains is completing the resume (the register area and stack) and giving
the run enough time, with the acceptance test unchanged: `0x2E0A58` writes and the status byte returns to `0x00`.

## Session 136 (2026-11-17) - CORRECTION from `HANDOFF_LOADER.md`: there is no runnable bit; `0x400` is the PRIORITY byte

The project docs already contained this subsystem, and they correct two of my readings.

### `HANDOFF_LOADER.md:1052-1054` (authoritative, hardware-derived)

```
TCB: base 0x407160, stride 0x210; state +0x1F0, priority +0x1F1, entry +0x1B0,
     entry arg +0x1C0, started flag +0x1F3, +0x1AC, +0x200.
Context: base 0x48F590, size 0x1D0; +0x1AC state, +0x1B4 $ra, +0x1A0 $sa.
```

So the word I dumped at `+0x1F0` is `state | priority<<8 | started<<24`, and my session-135 conclusion was wrong:

| slot | dumped `+0x1F0` | state | **priority** | started |
|---|---|---|---|---|
| 2, 3, 4 | `0x01000401` | 1 | **4** | 1 |
| 5 | `0x01000101` | 1 | **1** | 1 |

- There is **no runnable bit `0x400`** - that bit is simply priority 4 vs 1.
- Slot 5 is a normal **priority-1** thread; its flags were never wrong.
- Priority coverage is fine: the frame loop pumps `FUN_00151ff0(0)`, `(1)`, `(4)` (`HANDOFF_LOADER.md:715`),
  so priority 1 is serviced.

**Consequence:** the `TOL_TRY_RESUME` write `+0x1F0 := 0x01000401` changed slot 5's *priority*, i.e. it fabricated
guest state - precisely what `HANDOFF_LOADER.md:56-60` (framing error #2) warns about. The observed behaviour
change is real but it means "moved to another run queue", not "made runnable". That experiment must not be read as
evidence for a flag fix, and it should not ship.

### What the docs already establish about the real mechanism

`HANDOFF_LOADER.md:84-90` (hardware-verified run loop):

```
0x151F78 jal FUN_0015243C    ; save
0x151F80 jal FUN_00152200    ; switch in
0x151F88 jal FUN_0015253C    ; <- the yield lands here
0x151F90 lw  v0, 0x1AC(s0)   ; still alive?
0x151F94 bnez v0, 0x151EE8   ;   yes -> next thread
0x151F9C jal FUN_00151CF0    ;   no  -> DELETE the thread
```

`+0x1AC` is the **alive** flag: `1` = yielded and alive, `0` = the entry returned -> the thread is deleted.
`FUN_00152200` sets it to `0` on switch-in, and the **yield `FUN_0015231C` sets it back to 1**. Our port shows
`yield (0x15231C) = 0`, so the thread that was switched in never yields again and stays `0`.

That is consistent with everything measured: slot 5 is switched out inside `jal 0x2DF590` (nextpc lands at
`0x233FC0`), `+0x1AC` never returns to 1, and `+0x1B4` stays at the entry PC because the save
(`FUN_0015231C`, save-only per `HANDOFF_LOADER.md:1055`) never runs.

### Also corrected: D1/D2 were already refuted by the docs

`HANDOFF_LOADER.md:651-698` shows the invocation queue drains and the timer is not latched (`mode=0x382`,
`EQUF/OVFF` clear), concluding D1/D2 are "refuted hypotheses, not defects". So interrupt delivery should not be
re-investigated from scratch.

### Corrected next target

`FUN_0015231C` (the yield) is the function that must run and does not. Read it and the `0x233E50` call it is
reached through, and establish why the switch-out path (`0x2DF590 -> 0x233E50`) abandons its caller instead of
performing the save+yield sequence. The acceptance test is unchanged: `0x2E0A58` writes and the status byte
returns to `0x00`.

## Session 137 (2026-11-17) - the yield chain is intact on paper; the "yield = 0" probe is the suspect

`FUN_00233E50` (`0x233E50..0x233FC0`) is the yield wrapper and contains exactly the two sites we measured:

```
0x233EAC  jal func_15231C        <- THE YIELD  (ra = 0x233EB4)
0x233EEC  jal func_233FC0        <- ra = 0x233EF4
```

`[sub2]` recorded `nextpc = 0x233FC0` after `0x2DF590` returned, which is the *target* of the `0x233EEC` call -
i.e. execution passed **`0x233EAC`**, the yield. So the call is being made even though the `yield` probe reported
zero entries.

That makes the probe, not the guest, the suspect, and the docs already name the mechanism
(`HANDOFF_LOADER.md:642-645`, D5): `replaceFunction(pc)` only sees calls routed through `dispatchGuestBranch`,
while many internal calls are emitted as direct C++ (`FUN_xxx_0xNNN(...)`). A zero count must always be
cross-checked against the corpus.

### Next

1. Verify the install: log `runtime->lookupFunction(0x0015231Cu)` at startup (non-null?) and have the wrapper print
   unconditionally (no tag-based suppression) so a missed call cannot be disguised as a filtered one.
2. Read `+0x1AC` immediately after `0x233EAC` returns: if it becomes 1, the yield works and the alive flag is fine;
   if it stays 0, the yield's write is being skipped.
3. Keep the acceptance test unchanged: `0x2E0A58` writes and status returns to `0x00`.

## Session 138 (2026-11-17) - yield verified working; the submit path SKIPS it because `FUN_00126368` returns 0

### Both checks done

```
[install] lookupFunction(0x15231C) = ok        <- wrapper IS installed
yield_lines = 60                                <- the yield IS called (earlier "yield = 0" was an artefact)
A58 = 0                                         <- the release still does not run
```

Alive-flag behaviour across the call (`+0x1AC` of the current TCB, from `0x3DB198`):

```
[yield] #1 ra=0x001F8168  alive_before=0 -> alive_after=0  v0=0x00000000
[yield] #3 ra=0x001660EC  alive_before=0 -> alive_after=1  v0=0x00000002
[yield] #5 ra=0x001F86B8  alive_before=1 -> alive_after=1  v0=0x00000002
```

So the yield mechanism is healthy and **does** set `+0x1AC = 1` whenever it returns nonzero. The alive flag was
never the defect.

### The actual gap: the submit path never reaches the yield

Every observed call site is a worker loop: `ra = 0x1F8168, 0x1F8528, 0x1660EC, 0x1F86B8`. **No `ra = 0x233EB4`**,
i.e. `0x233EAC` is never executed. The guard explains why:

```
0x233E9C  jal func_126368       (a1=0, a2=s5, a3=s4, t0=s3)
0x233EA4  beqz $v0, +0x28       ; v0 == 0 -> skip the yield entirely (target 0x233ED0)
0x233EAC  jal func_15231C       ; the yield, only when v0 != 0
```

Since the yield is skipped on this path, `FUN_00126368` returned **0**. Everything follows from that: no save, no
`+0x1AC := 1`, `+0x1B4` stays at the entry PC, the run loop sees a non-yielded thread, and the caller of
`0x2DF590` is abandoned.

Supporting detail: the `0x126xxx` cluster also wrote job-table fields earlier
(`pc=0x00126328/0x00126340/0x00126358` -> `entry0+8/+0xC/+0x10`), so `FUN_00126368` is job-related - its condition
is plausibly "is the job still pending?", which is still true here.

### Next

Probe `FUN_00126368` (`0x126368`): log its arguments and its **return value**, plus the same arguments the caller
passes (`a2=$s5, a3=$s4, t0=$s3`). That single number is the gate: if it is 0 in the port and nonzero in the guest,
the divergence is inside it or in the job state it reads - not in the scheduler, the yield, or the TCB.

## Session 139 (2026-11-17) - the gate returns 0 = SUCCESS; the yield skip is by design, so that line is a dead end

Probe on `0x126368`:

```
[gate] #1 a0=0x00000000 a1=0x00000000 a2=0x004E9148 a3=0x004E914C t0=0x004E9150 t1=0x002E09E0 ret=0x00000000 ra=0x00233EA4
```

Called **once**, from `0x233EA4` (the caller's `beqz`), with `a2/a3/t0` = `entry0 + 8 / + 0xC / + 0x10` (the fields
the `0x126xxx` cluster writes) and `t1 = 0x2E09E0` (the submit entry PC).

### Complete return map of `FUN_00126368` (0x126368..0x1264E8)

| site | value | meaning |
|---|---|---|
| `0x1263B8` | `-100` (`0xFF9C`) | guard: `*(0x3E5D64) == 0` |
| `0x1263D4` | `-200` (`0xFF38`) | `FUN_00114730` returned `< 0` |
| `0x12640C`, `0x126428`, `0x126498` | `+1` | three distinct case paths |
| `0x1264A8` | `-91` (`0xFFA5`) | further failure case |
| **`0x1264B4`** | **`0`** (`daddu v0,zero,zero`) | **success / fall-through** |

The observed `ret = 0` means it passed both guards and fell through the success tail. It also stores its arguments
into the structure at `0x3E5DC0` (`0x1263E4: *(v1+4) = a0`, `0x1263E8: *(v1+8) = a1`, both 0 here).

### Consequence: this is not the bug

`0x233E50` calls the yield only when `FUN_00126368` returns **nonzero**, i.e. only on **failure**. A `0` result
means "proceed normally" and the yield is *supposed* to be skipped. So:

- the earlier framing ("the submit path skips the yield because the gate says no") was backwards - `0` is success;
- the yield is an error-retry path, not the normal suspension mechanism for this call;
- sessions 137-138's conclusion that the missing yield explains the abandonment is therefore **withdrawn**.

### Corrected understanding of the stall

`0x2DF590` calls `0x233E50`, which succeeds, skips the yield, and calls `0x233FC0` - and that is where control goes
(`nextpc = 0x233FC0`). So the thread is suspended inside a **normal, successful** call, and something later is
supposed to resume it at `0x2E0A3C`. The open question is therefore who resumes a thread that was never yielded
from: the run loop only re-enters threads it switched out, so the resume must come from the `0x233FC0` path or from
a queue entry.

### Next (needs the emulator for the first time in this thread)

Compare in PCSX2: break at `0x233EA4` and read `v0` (expect 0 as well - if so, the skip is confirmed normal), and
break at `0x2E0A3C` to see whether the guest *does* resume the submit row there. If the guest reaches `0x2E0A3C`
while the port never does, the divergence is in whatever `0x233FC0` leaves behind, not in the gate.

## Session 140 (2026-11-17) - emulator blocked; both breakpoints armed and waiting

Armed both comparison points (both accepted):

```
0x00233EA4  breakpoint set   <- the gate's beqz; read v0 here
0x002E0A3C  breakpoint set   <- does the guest resume the submit row here?
```

The VM never reaches either: it sits at **`0x00081FC0`** (a BIOS address, i.e. not in game code), `continue` does
not advance it, and repeated reads disagree (`0x81FC0` from `evaluate`/`pause`, `0x001271C8` from `read_registers`).
So the instance is wedged the same way as earlier in the session and needs a GUI reset/reboot of the disc.

**Resume procedure (breakpoints survive the reset in this setup, but re-arm if not):**
1. Reset/reboot the disc in the GUI and let it return to the loader.
2. It should stop at `0x233EA4` first -> read `v0` (expected `0`, which would confirm the yield skip is normal).
3. Then, if the guest resumes the submit row, it stops at `0x2E0A3C` - which answers the second question by
   itself. If it stops at `0x2E0A3C` but the port never does, the divergence is downstream of `0x233FC0`.

Port-side status is unchanged and independent: the gate returns 0 = success, so the yield is an error-retry path
(session 139), and the remaining question is who resumes a thread suspended inside a successful call.

## Session 141 (2026-11-17) - EMULATOR CONFIRMS THE DIVERGENCE: guest reaches `0x2E0A3C`, port never does

Both comparison points taken in PCSX2 at the loader.

### 1. The gate: `v0 = 0` in the guest too

```
Paused at PC=0x00233EA4   ("v0" = 0x0)
```

So `FUN_00126368` returns **0 = success** on hardware as well, the yield is skipped there too, and the yield really
is an error-retry path. Session 139's correction is confirmed against the emulator; sessions 137-138 stay
withdrawn.

One extra observation: the guest hits `0x233EA4` **repeatedly** (it stopped there twice in a row, `v0 = 0` both
times), whereas the port hit the gate **once**. So the guest retries this path in a loop; the port does not.

### 2. The resume point: the guest DOES reach `0x2E0A3C`

With the gate breakpoint removed (it was masking the second one):

```
Breakpoint 0x002E0A3C
Paused at PC=0x002E0A3C
```

So on hardware the submit row **is** resumed after `jal 0x2DF590`, and therefore executes `0x2E0A50`
(`status & ~2`) and `0x2E0A58` (the release). This matches the hardware steady state measured earlier
(`status = 0x00`, `entry+6 = 0xFF0E`).

### The divergence, stated exactly

| | gate `v0` | gate hit count | reaches `0x2E0A3C` | release `0x2E0A58` |
|---|---|---|---|---|
| PCSX2 (guest) | `0` | repeated (retry loop) | **YES** | runs (status -> `0x00`) |
| port | `0` | **once** | **NO** | never |

Everything upstream of the call is identical - same gate, same success value, same skipped yield. The difference is
purely that the guest **resumes the caller at `0x2E0A3C`** and the port abandons it. And the guest does it in a
loop, which is why the retry count differs too.

### Next

The resume is what to fix, and the guest shows it is normal: after `0x2DF590 -> 0x233E50 -> 0x233FC0` returns, the
submit row continues at `0x2E0A3C`. Instrument `0x233FC0` (the last call in `0x233E50`) to see what it does in the
port and whether it returns; the port's `nextpc = 0x233FC0` at that moment says control was left *inside* that call
rather than unwinding to `0x2E0A3C`. Acceptance test unchanged: `0x2E0A58` writes and status returns to `0x00`.

## Session 142 (2026-11-17) - final link refuted: `0x233FC0` returns correctly

```
[fc0] #1 enter a0=0x003E5D40 a1=0x00000001 ra=0x00233EF4
[fc0] #1 RETURNED v0=0x00000000 nextpc=0x00233EF4
[fc0] #2 enter a0=0x003E5D40 a1=0x0000000D ra=0x00233CA8
[fc0] #2 RETURNED v0=0x00000000 nextpc=0x00151F88
```

- Call #1 carries `ra = 0x233EF4`, exactly the `jal` at `0x233EEC`, and **returns to that address** - so `0x233E50`
  does continue past its last call, and `0x233FC0` is **not** where control is lost. The earlier reading of
  `nextpc = 0x233FC0` as "abandoned inside that call" was an artefact of the runtime's inline dispatch, not a
  defect.
- Call #2 is reached from `ra = 0x233CA8` (a different caller in `0x233Cxx`) and returns to **`0x151F88`**, the run
  loop post-switch site - so *on that path* a context switch happens rather than a plain return.

Still `0x2E0A3C = 0` and `0x2E0A58 = 0`, so the break lies between `0x233E50`'s continuation and the submit row's
resume.

### Next

Instrument `0x2DF5DC` (the instruction in `0x2DF590` immediately after its `jal 0x233E50` at `0x2DF5D4`): if the
port never executes it, the loss is one frame up, in `0x2DF590`'s own continuation rather than in `0x233FC0`.

## Session 143 (2026-11-17) - FOUND IT: `0x233FC0` is a poll-and-yield loop, and the poll returns nonzero so the yield never runs

### `FUN_00233FC0` (0x233FC0..0x234010)

```
0x233FC8  sw   $zero, 0x1C($sp)      ; local flag = 0
0x233FCC  a0 = 1 ; a1 = 0
0x233FD4  jal  func_126248           ; (1, 0, &flag)
0x233FDC  bnez $v0, +0x18            ; nonzero -> SKIP the yield, leave the loop
0x233FE4  jal  func_15231C           ; THE YIELD (the save that writes ctx[0x1B4] = $ra)
0x233FEC  b    -0x20                 ; -> back to 0x233FD0, poll again
```

So it is a **poll-until-ready, yielding each iteration** loop. `FUN_00126248` is called as `(1, 0, &flag)` - a
thread/status query - and the yield only happens while it returns 0.

### Measured: the yield from here never executes

```
yield calls: 30   ra values: 0x1F8168 x14, 0x1F8528 x8, 0x1660EC x2, 0x1F86B8 x6
ra = 0x233FEC : 0        <- the call at 0x233FE4 is never reached
```

`FUN_00233FC0` itself is entered twice and returns through `0x233EF4` correctly, but it always takes the
`bnez $v0` exit at `0x233FDC`. Therefore **`FUN_00126248` returns nonzero in the port on the first poll**, so the
loop never yields, the save never runs, `ctx[0x1B4]` is never updated, and the thread is parked with a stale resume
PC and `+0x1AC = 0` - exactly the state measured on slot 5.

### Why this closes the chain

```
0x2DF590 -> 0x233E50 -> 0x233FC0 -> poll FUN_00126248
                                       port: nonzero  -> exit loop, no save, no yield
                                       guest: 0        -> yield (save ctx[0x1B4]) -> poll again
```

Every earlier layer is now verified correct against the emulator (gate `v0 = 0`, plain returns, thread re-dispatched
11x). The single remaining difference is **the value `FUN_00126248` returns**, and it gates the only save in the
path.

### Next

Instrument `0x126248` exactly as `0x126368` was: log `a0/a1/a2` and the return value, then compare against PCSX2 at
`0x233FDC` (break there and read `v0`). If the guest returns 0 and the port returns nonzero, the fix is inside
`FUN_00126248` (or the thread state it queries) - a small, well-bounded function rather than the scheduler.

## Session 144 (2026-11-17) - `FUN_00126248` is behaving correctly (returns 1 = done); the loss is after `0x233EF4`

```
[g248] #1 a0=0 a1=0 a2=0x00000000  global388D30=0x00000000  ret=0xFFFFFFFF
[g248] #2 a0=1 a1=0 a2=0x0041745C  global388D30=0x00000001  ret=0x00000001
[g248] #3 a0=1 a1=0 a2=0x004172EC  global388D30=0x0000000D  ret=0x00000000
[g248] #4..   same as #3, ret=0
```

- `#1` is an early init call before `*(0x388D30)` is set, hence the documented `-1` path (`0x126284`).
- `#2` is the **submit path** (`a2 = 0x41745C`, the `&flag` local of `0x233FC0`): it returns **1 = completed**.
- `#3+` are other paths (`a2 = 0x4172EC`): they return **0**, so they yield and loop as designed.

So the poll is answering "done", the yield is *correctly* skipped, and `0x233FC0` returns normally into `0x233E50`
at `0x233EF4` (session 142). The loss is therefore **after `0x233EF4`**, inside `FUN_00233E50`'s continuation -
not in the poll and not in the yield.

### Next

Trace `FUN_00233E50` past `0x233EF4`: instrument the calls it makes after that point and find which one fails to
return to `0x2DF5DC`. Acceptance test unchanged: `0x2E0A58` writes, status returns to `0x00`.

## Session 145 (2026-11-17) - `0x233E50` has no calls after `0x233EF4`; its exit leaves `pc = 0x233FC0`, and the blind spot is interior dispatch

Static: after `0x233EF4` `FUN_00233E50` contains **no `jal`** at all - only `b` branches and `jr $ra` at
`0x233FB8`. So "the first call that fails to return" does not exist in that range; the only way to fail is for the
function never to reach its own `jr`.

Measured with a probe on the row start (`0x233E50`):

```
[e50] #1 enter_ra=0x002DF5DC exit_pc=0x00233FC0 v0=0x00000001   (expected exit 0x002DF5DC)
```

- Entered with the correct caller continuation `ra = 0x2DF5DC`.
- Leaves `pc = 0x233FC0`, i.e. the C++ returned at the inline dispatch of the `0x233EEC` call. So completion
  requires the runtime to **re-dispatch at `0x233EF4`** after `0x233FC0` returns.
- `0x233EF4` is a valid resume label (`case 0x233ef4u: goto label_233ef4;`, generated file line 63), so the
  emitter models it correctly.

**Instrumentation blind spot:** the probe is installed at the row start `0x233E50`, and `replaceFunction` patches a
single table entry, so an interior dispatch to `0x233EF4` would bypass it. `[e50] = 1` therefore does **not** prove
the continuation never runs - it only proves the row start was entered once.

### Next

Install the same probe at **`0x233EF4`** (4-byte table granularity, so this is safe and does not disturb the row
start) and log entry + `ctx->pc` after `0x233FB8`'s `jr`. That distinguishes the last two possibilities:

1. `0x233EF4` is never dispatched -> the runtime drops the continuation after the `0x233FC0` return;
2. it is dispatched but never reaches `0x233FB8` -> the branch at `0x233EF8` (`beql $v0,-5`) diverts, and `$v0`
   (`1` here) is the value to compare against hardware.

Acceptance test unchanged: `0x2E0A58` writes, status returns to `0x00`.

## Session 146 (2026-11-17) - `0x233EF4` IS resumed and returns to `0x2DF5DC`; the chain is clean one frame further

```
[install] lookupFunction(0x233EF4) = ok
[e50] #1 enter_ra=0x002DF5DC exit_pc=0x00233FC0 v0=0x00000001
[ef4] #1 RESUME entry_pc=0x00233EF4 ra=0x00233EF4 v0=0x00000000 v1=0x00000000
[ef4] #1 exit_pc=0x002DF5DC
```

- The interior continuation **is dispatched** (so the runtime does not drop it after the `0x233FC0` return).
- It resumes with `v0 = 0` (the value `0x233FC0` returned), so `beql $v0,-5` at `0x233EF8` does not divert.
- It ends at `0x233FB8`'s `jr` with **`exit_pc = 0x2DF5DC`**, i.e. `0x233E50` returns correctly to its caller.

So `0x233E50` and everything below it are verified end to end. The abandonment is now **one frame up**: `0x2DF590`
resumes at `0x2DF5DC` and must then return to the submit row at `0x2E0A3C`, which never happens.

### Next

Probe **`0x2DF5DC`** (interior continuation of `FUN_002DF590`, immediately after its `jal 0x233E50` at `0x2DF5D4`)
with the same pattern - entry + exit pc. Possible outcomes:

1. never dispatched -> the continuation is dropped at this level;
2. dispatched and exits at `0x2E0A3C` -> then the submit row *is* resumed and the fault is in what follows it;
3. dispatched but exits elsewhere -> the first genuinely diverging return, and the fix socket.

Acceptance test unchanged: `0x2E0A58` writes, status returns to `0x00`.

## Session 147 (2026-11-17) - FIRST BROKEN RETURN: `0x2DF5DC` resumes but exits at `0x116A6C`, not `0x2E0A3C`

```
[install] lookupFunction(0x2DF5DC) = ok
[d5dc] #1 RESUME pc=0x002DF5DC ra=0x002DF5DC v0=0x00000000
[d5dc] #1 exit_pc=0x00116A6C (expect 0x002E0A3C)
```

Outcome **3** from the plan: the continuation is dispatched (so the runtime does not drop it), but this invocation of
`FUN_002DF590` ends by returning to **`0x116A6C`** instead of the submit row's continuation `0x2E0A3C`.

Six layers below are verified correct against hardware, so this is the first place the port genuinely diverges:
the `ra` planted by the submit row's `jal` at `0x2E0A34` (`0x2E0A3C`) is not the `ra` this call returns with.

Two candidate explanations:

1. **`ra` is clobbered** inside the `0x2DF590 -> 0x233E50 -> 0x233FC0` chain (the chain restores/fiddles registers
   and something writes `$ra`), so the return goes to whatever was last in it.
2. **this resumption belongs to a different invocation** - `0x2DF590` has other callers, and `0x116A6C` is
   plausibly the return address of a call site at `0x116A64`. In that case the submit row's own call is still
   pending and the two invocations are being conflated.

Per the agreed rule this is **not** the pivot condition (it did not return to `0x2E0A3C`), so the next step stays
on this seam - but it is now a fix socket rather than another open question.

### Next

1. Static: check who calls `0x2DF590` (look for a `jal`/dispatch at `0x116A64`) to decide between the two candidate
   explanations above.
2. Dynamic: log `$ra` at entry of `0x2DF590` for **each** invocation (it is called from `0x2E0A34` with `0x2E0A3C`);
   if two invocations exist with different `ra`, they are being conflated and the fix is to keep them distinct.
3. Acceptance test unchanged: `0x2E0A58` writes, status returns to `0x00`.

## Session 148 (2026-11-17) - conflation theory dead: one caller only, and `0x116A6C` is a ROW BOUNDARY - `$ra` was overwritten

Both cheap checks:

```
dispatch callers of 0x2DF590 : exactly ONE - 0x2E0A34, continuation 0x2E0A3C   (the submit row)
0x116A6C                     : END of row caseD_45_0x116a08 (0x116a08..0x116a6c)
```

This kills explanation 2 (two invocations conflated): there is a single dispatched caller, and `[sub2] #1 enter
ra=0x002E0A3C` agrees - one entry, correct `ra`.

It confirms explanation 1 instead, and sharpens it: **`0x116A6C` is a row boundary, not a call site.** No `jal` can
have planted it as a return address, so by the time `FUN_002DF590` executed its final `jr $ra`, `$ra` held a value
that came from somewhere other than a matching call - i.e. the register was **overwritten inside the chain**.

The chain in question is `0x2DF590 -> 0x233E50 -> 0x233FC0`, and every one of those layers was verified to return
correctly in isolation (sessions 142-146). The difference now is that we know the *final* return of the outer
function uses a corrupted `$ra`, which is only possible if something in between rewrote the register - a saved-state
restore (the `0x152200` family restores the whole GPR file from a context block) or a register spill/refill that
picks up another context's value.

### Next

Log `$ra` at the exact instruction where `FUN_002DF590` performs its final `jr` (interior address, 4-byte table
granularity as used for `0x233EF4`/`0x2DF5DC`), and compare it with the `0x2E0A3C` that was present at entry. That
pinpoints the overwrite to a specific sub-call, and the fix becomes "preserve `$ra` across that call" rather than
anything in the scheduler, the gate, or the job table.

## Session 149 (2026-11-17) - the corruption is at/before `0x2DF5DC`, not in the later calls

`FUN_002DF590`'s final `jr $ra` is at **`0x2DF6C8`** - reached by fall-through, so it cannot be hooked directly. The
last call before it is `0x2DF0B0` at `0x2DF6A8`, and only `b`/`nop` separate them, so the `$ra` present when that
call returns is the value the `jr` uses. Probe on it:

```
[d0b0] #1 enter ra=0x002DF08C  exit ra=0x002DF08C
[d0b0] #2 enter ra=0x002DF72C  exit ra=0x002DF72C
[d5dc] #1 RESUME pc=0x002DF5DC ra=0x002DF5DC v0=0x00000000
[d5dc] #1 exit_pc=0x00116A6C
```

**No call carries `ra = 0x2DF6B0`**, which is what a call from `0x2DF6A8` would produce. So on this path the last
call is never reached at all: after resuming at `0x2DF5DC` the function goes straight to the `jr` with `$ra` already
wrong (`0x116A6C`, a row end from `caseD_45_0x116a08`).

Therefore the overwrite happens **at or before `0x2DF5DC`** - i.e. during the `0x233E50` inline dispatch, not in the
later calls. Consistent with the entry/continuation picture:

- submit row sets `ra = 0x2E0A3C` and calls `0x2DF590` (0x2E0A34);
- `0x2DF590` saves that `ra` on its frame and calls `0x233E50`, which replaces `$ra` with `0x2DF5DC`;
- the final `jr` is supposed to use the **restored** `ra` (0x2E0A3C) but instead uses `0x116A6C`.

So the suspect is the **save/restore of `ra` across the `0x233E50` call** (the prologue `sd $ra` plus the matching
`ld $ra` before the `jr`), not any deeper function.

### Next

Find and check that `ld $ra, N($sp)` in `FUN_002DF590` (the epilogue restore): log its value against the frame slot,
or compare the `sp` used by the prologue `sd` with the `sp` used by the epilogue `ld`. A mismatched `sp` (stack
switched inside the call, which we know can happen) would load a foreign value - exactly `0x116A6C`.

## Session 150 (2026-11-17) - stack-mismatch theory REFUTED: `sp` is correct at the continuation

Static: the store and load are the **same slot**, with a symmetric frame:

```
0x2DF590  addiu sp, sp, -0x50
0x2DF598  sd    ra, 0x40(sp)      ; store  -> slot = sp+0x40
0x2DF6B4  ld    ra, 0x40(sp)      ; load   <- same slot
0x2DF6C8  jr    ra
0x2DF6CC    addiu sp, sp, 0x50    ; delay slot, symmetric
```

Dynamic (sp logged on both sides):

```
[sub2] #1 enter  sp=0x00417520    ; at 0x2DF590 entry
[d5dc] #1 RESUME sp=0x004174D0    ; at 0x2DF5DC, after the 0x233E50 call
```

Delta = `0x50`, exactly the frame size, so the continuation runs with the **correct** post-prologue `sp`. There is no
double-decrement, no lost frame, and the prologue store / epilogue load address the same slot
(`0x4174D0 + 0x40 = 0x417510`, which is exactly where the earlier needle scan found `0x2E0A3C`).

**So the wrong `$ra` at the `jr` is not a stack-pointer bug.** Either the slot at `0x417510` is overwritten between
store and load, or the resumed path does not execute the epilogue `ld` at all.

### Next

Log the **content of the slot** `0x417510` at three points - at `0x2DF590` entry (just after the store), at the
`0x2DF5DC` resume, and after the call returns - to see whether it still holds `0x2E0A3C` at the end. That
distinguishes "slot overwritten" from "epilogue not executed", which are different fixes:

- overwritten -> the writer of `0x417510` is the culprit (find it with a range watch on the submit thread's stack);
- epilogue skipped -> the resumed path branches past `0x2DF6B4`, and the branch condition at `0x2DF5DC` is the fix.

## Session 151 (2026-11-17) - slot is CORRECT at all points: the exit is a different control transfer, not the epilogue

```
[sub2] #1 enter   ra=0x002E0A3C  sp=0x00417520
[sub2] #1 ret     nextpc=0x00233FC0  slot[0x417510]=0x002E0A3C
[d5dc] #1 RESUME  ra=0x002DF5DC  sp=0x004174D0  slot[0x417510]=0x002E0A3C
[d5dc] #1 exit_pc=0x00116A6C
```

The saved-RA slot holds the correct `0x2E0A3C` at **every** sampled point, and `sp` at the continuation is correct
(`entry - 0x50`). So:

- the slot is **not** overwritten (theory 1 refuted);
- an executed `ld $ra,0x40(sp)` at `0x2DF6B4` would load `0x2E0A3C` and the `jr` at `0x2DF6C8` would go there;
- yet control leaves at `0x116A6C`.

Conclusion: on this path the row does **not** execute its own epilogue - the transfer to `0x116A6C` comes from a
different instruction in `FUN_002DF590`'s row (an indirect transfer / tail jump), not from the `jr $ra` at
`0x2DF6C8`. The row contains exactly one `jr`, so the exit must be an indirect branch.

### Next

Log the **control transfer** out of the row rather than guessing: widen the existing `TOL_TRACE_INDIRECT` filter in
`PS2Runtime::dispatchGuestBranch` to also cover `sourcePc` in `0x2DF590..0x2DF6D0`, and print `targetPc` plus
`kind`. The transfer that produces `0x116A6C` will name its own instruction, which is the fix socket - and it will
also show whether that transfer is legitimate guest code or a runtime redirect.

---

# SESSION CLOSE 2026-11-17 (sessions 123-151) - READ THIS BEFORE PROBING ANYTHING

## One-line status

The loader stalls because job-table entry 0 stays **reserved** (bit 1 set) with **no payload** (`entry+6 = 0`);
the guest clears it, the port never does. Everything downstream of that is verified; the single open question is
one indirect control transfer.

## The blocker, stated exactly

| | PCSX2 (guest) | port |
|---|---|---|
| `entry0` status byte | `0x00` (bit 1 clear) | `0x02` (bit 1 set) |
| `entry0+6` payload | `0xFF0E` | `0x0000` |
| reaches `0x2E0A3C` (submit row resume) | **yes** | **no** |
| `0x2E0A58` (the release `sb`) | runs | never runs |

Reader contract (`FUN_002E0820`): `if (bit1) return 0; else *out = *(s16*)(0x4E9146+idx), return 1;` - so bit 1 =
"claimed", and the port is stuck in the claimed state with an empty payload.

## Verified correct (do NOT re-investigate)

Established by measurement in the port, and cross-checked against PCSX2 where possible:

- **The job table writers**: `FUN_002DEF60` (init/clear, bit 0 only), row `0x2E09E0` (fills `entry+6` at `0x2E0A38`),
  `FUN_002E07D0` (test-and-set of bit 1 at `0x2E0814`).
- **The gate** `FUN_00126368` returns **0 = success** (tail `0x1264B4`) in the port *and* `v0=0` in PCSX2. The
  yield it guards is an **error-retry path**, so skipping it is correct.
- **The poll** `FUN_00126248` returns `1` ("done") on the submit path (`a2=0x41745C`) and `0` elsewhere - correct
  discrimination; `-1` only before `*(0x388D30)` is initialised.
- **The yield** `FUN_0015231C` runs (30 calls, `ra` = worker loops) and sets `+0x1AC=1` when it returns nonzero.
- **The switch** `FUN_00152200` (context restore from `0x48F590`, index/stride `0x3DB1A0/0x3DB1B0/0x3DB1C0`).
- **Interior continuations ARE dispatched**: `0x233EF4` resumes and returns to `0x2DF5DC`; `0x2DF5DC` resumes.
- **The TCB layout** (see below) and `sp`/saved-RA integrity: `sp` at the continuation is exactly `entry-0x50`, and
  the saved-RA slot `0x417510` holds `0x2E0A3C` at store, resume and return.
- **D1/D2** (timer latch, IRQ ack ordering) were already refuted in `HANDOFF_LOADER.md:651-698` - not defects.

## Refuted by measurement (the recurring lesson)

Each of these was a confident conclusion that a measurement killed - in order: the `StoreN` write watch (blind to
RAM stores), "the claim is the failing-step branch" (both paths claim), "bit `0x400` is a runnable flag" (it is the
**priority** byte), "the yield is missing" (gate returns success), "`0x233FC0` never returns" (it returns),
"dispatch mismatched/`$ra` clobbered by a stack mismatch" (sp and slot are both correct).

**Rule: distrust the instrument before the guest.** A zero count, a `nextpc`, or a u8 read of a u32 field has
misled this project four times.

## The one remaining question

`FUN_002DF590` (row `0x2DF590..0x2DF6D0`) is entered correctly (`ra=0x2E0A3C`), resumes at `0x2DF5DC` with correct
`sp` and a correct saved RA, and yet control leaves at **`0x116A6C`** - the tail of row `caseD_45_0x116a08`.
The row has exactly **one** `jr` (`0x2DF6C8`, preceded by `ld $ra,0x40(sp)` at `0x2DF6B4`), which would return to
`0x2E0A3C`. So the exit is **an indirect transfer inside the row**, not the epilogue.

## Next probe (exact, cheap)

Widen the `TOL_TRACE_INDIRECT` filter in `PS2Runtime::dispatchGuestBranch` (`ps2_runtime.cpp`, already env-gated)
to cover `sourcePc ∈ [0x2DF590, 0x2DF6D0)` and log `targetPc` + `kind`. That names the instruction producing
`0x116A6C` and decides between guest code and a runtime redirect. Acceptance test unchanged: `0x2E0A58` writes and
the status byte returns to `0x00`.

## Reference facts (hard-won, reuse them)

```
TCB base 0x407160, stride 0x210, 64 entries
  +0x1A8 saved ra    +0x1AC alive flag (1 = yielded/alive, 0 = returned -> thread DELETED)
  +0x1B0 entry       +0x1B4 resume PC      +0x1C0 entry arg
  +0x1F0 state(u8) | priority(u8 @+0x1F1) | started(u8 @+0x1F3)
  +0x1F8 / +0x1FC / +0x200   list links / self ;  +0x204/+0x208 stack range
Context array 0x48F590, entry size 0x1D0 (index/stride/off at 0x3DB1A0/0x3DB1B0/0x3DB1C0)
Current-thread pointer 0x3DB198
Job table 0x4E9140, stride 0x18, 26 entries; reader reads 0x4E9146 (= entry0+6)
Run loop FUN_00151E90: 0x151F78 save / 0x151F80 switch-in / 0x151F88 yield lands / 0x151F90 alive check
                      0x151F94 bnez alive -> next thread ; 0x151F9C DELETE thread
Submit row 0x2E09E0: 0x2E0A30 status / 0x2E0A34 jal 0x2DF590 / 0x2E0A38 entry+6 /
                      0x2E0A50 and ~2 / 0x2E0A58 RELEASE / 0x2E0A68 jr
Chain 0x2DF590 -> 0x233E50 -> 0x233FC0(poll 0x126248; yield 0x15231C only when poll != 0)
```

## Instrumentation inventory (all opt-in, all in `src/runtime/tol_overrides.cpp` unless noted)

| env var | what it does |
|---|---|
| `TOL_TRACE_ORDER` | `[order]`/`[sub2]`/`[e50]`/`[ef4]`/`[d5dc]`/`[d0b0]`/`[gate]`/`[g248]`/`[fc0]`/`[yield]`/`[tcb]` chains |
| `TOL_WATCH_JOBTABLE_RAM` | range watch on `0x4E9140..0x4E93B0` (in `ps2_runtime.h` - header edit, cost a full rebuild) |
| `TOL_TRACE_INDIRECT` | indirect transfers with `sourcePc ∈ [0x152200,0x152700)` (in `ps2_runtime.cpp`) |
| `TOL_TRY_RESUME` | diagnostic only: pokes `+0x1AC/+0x1B4/+0x1F0/+0x1FC` on slot 5. **Not a fix** - it changes priority and fabricates guest state |
| `TOL_TRACE_PC` / `TOL_TRACE_SCRIPT` / `TOL_WATCH_SLOT2` | pre-existing |

## Documentation already covering this area (read before re-deriving)

- `HANDOFF_LOADER.md` - TCB/context layout, run loop, alive check, D1-D5, "host scheduler is the framing error".
- `docs/ps2-static-recompilation-overview.md:161` - `EeScheduler` scope.
- `.opencode/skills/ps2-recomp/resources/db-syscalls.md` - thread syscall table.
- `.opencode/skills/ps2-recomp/resources/09-ps2tek.md:4767` - canonical `struct TCB`.

---

# Session 152 (2026-11-17) - the exit at 0x116A6C was printf's jump table; the real bug is an interior entry at 0x125A78

This session answered session 151's "one remaining question", refuted its conclusion, and fixed the actual
defect. Kept in full because four separate conclusions along the way were wrong before measurement killed them.

## 1. `0x116A6C` is legitimate guest code - the row never "exited" corruptly

`TOL_TRACE_INDIRECT` was widened (armed-window recorder, see §5) and produced the complete transfer list of the
`0x2DF590` call after its `0x2DF5DC` resume:

```
[0]  0x002DF624 -> 0x002DF6D0 CALL   ra=0x002DF62C sp=0x004174D0
[1]  0x002DF718 -> 0x001312D0 CALL   ra=0x002DF720 sp=0x00417340
...
[11] 0x001172FC -> 0x001164C8 CALL   ra=0x00117304 sp=0x004171A0
[12] 0x0011657C -> 0x00116A6C JR     ra=0x00117304 sp=0x00417080
```

`0x11657C` is `jr $a0` over the **printf/sprintf format-parser jump table** (`lui v0,0x3c / addiu v0,-0x1270`
= table at `0x3BED90`, indexed by `format_char - 0x20`, 0x59 entries); `0x116A6C` is another case body of the
same function, not a row boundary. So:

- the exit was **an indirect jump inside printf**, i.e. normal guest code - not a runtime redirect;
- session 151's premise ("the row does not execute its own epilogue, so that is the bug") was **wrong**: the row
  took its documented "start the job" path, `s2 == 0 && (entry[0] & 1) == 0` -> `jal 0x2DF6D0(index)`
  (`0x2DF604` `beql s2,zero,0x2DF614` -> `andi v1,v1,1` -> `beq` not taken -> `0x2DF624`);
- `0x2DF6D0` is **not** an error printer: it builds a path from `"%s/%s%02d"` (`0x3CB578`/`0x3CB560`) and calls
  `0x2DF0B0`, and it really ran (`[d0b0] #2 enter ra=0x002DF72C`, exit ra intact).
- `[d5dc] exit_pc` is useless as a "did the row finish" signal: dispatch sets `ctx->pc` then unwinds, so it
  reports the *last* transfer of the whole nested chain, not a return.

## 2. Where it actually stops: a poll/yield livelock, faithfully executed

After the loader ran, the guest entered `FUN_00233FC0` - a **poll-then-yield loop**:

```
0x233FCC loop: li a0,1 ; move a1,zero ; jal 0x126248 (dequeue) ; addiu a2,sp,0x1c
0x233FDC       bne v0,zero,0x233FF8   ; done -> return *(sp+0x1c)
0x233FE4       jal 0x15231C           ; yield
0x233FEC       b 0x233FD0             ; retry
```

Over 40 s the thread only alternates `0x233FEC` <-> `0x151F88` (the run loop's yield landing) with the same
`sp = 0x4172D0`, and `0x2E0A*` is never dispatched, `relA`/`relB` never run. That is a genuine guest wait, not
a lost continuation - the dequeue simply never succeeds.

`FUN_00126248` is a queue pop: `if (*(0x388D30) == 0) return -1; if (PollSema(0x388D38) < 0) return 0;`
`*(0x388D30) = 0; *out = *(0x3E7300); SignalSema(0x388D34);`. `0x388D38` is created in `FUN_001257f8`
(`s_sceMc_sema_timer`) and is signalled **only** by the RPC completion of the op submitter (`FUN_001260d0` and
six siblings: fill `0x3E5DC0`, `sceSifCallRpc(client, fn, …, end)`; on success store the fn code into
`0x388D30`). Measured: `[g248] a0=1 … global388D30=0x0000000D ret=0x00000000` - fn code stored, semaphore
never signalled.

## 3. Root cause: the RPC end function is an interior address of `entry_00125a64`

The end function is `0x125A78` (measured in `$t3`: `[rpc] call client=0x003E5D40 sid=0x80000400 fn=0xD …
t3=0x00125A78`), and its whole body is:

```
0x125A78 lui v0,0x39
0x125A7C j   0x114710          ; iSignalSema
0x125A80 lw  a0,-0x72c8(v0)    ; a0 = *(0x388D38)
```

`data/output_fixed/entry_00125a64_0x125a64.cpp` (row `0x125A64..0x125B48`) has

```cpp
switch (ctx->pc) {
    case 0x125ac4u: goto label_125ac4;
    case 0x125b08u: goto label_125b08;
    case 0x125b18u: goto label_125b18;
    default: break;
}
```

**`0x125a78` has no case.** The table slot exists (Option A dense table, `register_functions.cpp:676394`), but
dispatching there runs the row from `0x125A64` - the tail of a *different* function:
`ld ra,0x10(sp); addiu v0,1; ld s0,0(sp); jr ra; addiu sp,+0x20`. The RPC invocation sets `$ra = 0`, so that
`jr` leaves `pc = 0` and the thread is parked. Measured before the fix:

```
[criend] #1 ENTER a0=0x00000000 ra=0x00000000 sp=0x01FBFFF0 sema338D38=0x00000037
[criend] #1 exit pc=0x00000000
```

So `SignalSema(0x37)` never ran. This is exactly the systemic defect in CURRENT STATUS ("dispatching at an
interior entry point restarts the row"), and it is the first case where it was the *whole* boot gate.

## 4. Fix (per-address, in `src/runtime/tol_overrides.cpp`)

`criEndFunctionFix`, installed by `installCriEndFunctionFix` (`runtime.replaceFunction(0x00125A78u, …)`):

```cpp
const uint32_t semaphoreId = runtime->Load32(rdram, ctx, 0x00388D38u);
SET_GPR_U32(ctx, 4, semaphoreId);
ctx->pc = 0x00114710u;          // == the row's own `j 0x114710` (iSignalSema)
```

Verified: `[g248]` goes from `ret=0xFFFFFFFF`/`0` forever to `ret=0x00000001` (twice per boot), the dequeue
succeeds, and the main thread leaves the op-channel spin.

## 5. Instrumentation added (both env-gated, no effect when unset)

| where | what |
|---|---|
| `ps2_runtime.cpp` (`dispatchGuestBranch`) | armed-window recorder `g_tolOrderArm` / `g_tolOrderSeen` / `g_tolOrder{Src,Tgt,Ra,Sp,Kind}[256]` - records every inter-function transfer made inside one guest call, including kind and `$ra`. This is the only way to see fall-through and intra-row gotos. |
| `tol_overrides.cpp` (`orderD5DCProbe`) | arms the window, prints the transfer sequence + job-table status/payload at the exit. |
| `tol_overrides.cpp` (`tolSifCallRpc`) | `[rpc] endfn t3/sp0/spC/sp10/mapped` and `t3=` on the call line; call-log cap 256 -> 4000 (the CRI submit was hidden past the cap and that is what made "is the end function passed?" unanswerable). |

## 6. No-regression check (clean A/B, same env, 60 s each)

`TOL_TRACE_ORDER=1` only, pre-fix `logs/e152b.log` vs post-fix `logs/e158.log`: **16/16 behaviour classes
identical, 0 changed** (12 IOP modules started, 6 AFS `found` + `Total 6 files`, 3 `PS2RNA: sceSifAllocIopHeap`,
`libdbc` warning, identical `no kernel service` set, identical `[fileio]`, SIF install, 1 pre-existing
`missing-target`, 0 `No exact recompiled function`, 0 exceptions, `[render-cadence] enabled`, 0 drive/CRI
errors, process alive at 60 s).

Forward movement is confined to the stall itself: `read` 12x`ret=0` (spin) -> 2x`ret=1`; `dispA` 135 -> 4;
`yield` from `0x3661DC` 0 -> 247.

## 7b. Continuation (session 152) - the app/phase gate chain, measured, and the PCSX2 cross-check

After the `0x125A78` fix the boot sits in `FUN_00366090`'s run loop:

```c
while (app->vtable[0x0C]()) { manager[8](); partner->[0x1C](); app->vtable[0x38](app); manager[0xC](); yield(); }
```

`app` = `0x63E3D0`, and **the heap addresses are identical in PCSX2 and the port**, so any address can be
compared directly between the two. `vtable[0x0C]` = `0x205F80` = `lbu v0,8(a0)` ("busy"), so the loop can only
end when `app+8` is cleared. `vtable[0x38]` = `0x365630` = the step, which calls `FUN_002E9720` and then one of
`FUN_00365700` (state 1) / `0x365CD0` (state 3) / `0x365FD0` (state 4) according to `app+0x70`.

Measured with the `[appobj]` probe (`TOL_TRACE_ORDER`), steady state over 45 s:

```
obj=0x0063E3D0 +4=0 busy(+8)=1 outer(+70)=1 next(+72)=0xFFFF phase(+74)=3 cur(+14)=1 cnt(+64)=1
res(+80)=0xFFFFFF0D child=0x0063E610 ch4=0 ch8=0 jt0=[0x00000001 pay=0xFF0D]
```

- **phase 3** is the fade/hand-off stage and its first test is `if (!*(int*)(param_1+4)) return 0;` - so the boot
  now waits on `app+4`.
- `app+4` is rewritten every step at `0x2E977C sw v0,4(s0)`: `obj+4 = acquire(obj)`, where acquire is secondary
  vtable `0x3DA35C` + 0x14 = `FUN_002E9EB0`. It walks the child array at `obj+0x18`, cursor `obj+0x14`, count
  `obj+0x64`, and returns the first child that is **not busy** *and* whose `vtable[0x14]` returns nonzero.
  `child->vtable[0x14]` = `0x205F90` thunk -> `0x205FA0` = our `objectResetFix`, whose real body was verified
  instruction-for-instruction in PCSX2:
  `v0 = child->+4; child->+4 = 0; child->+8 = 0;` - i.e. **the acquire consumes the child and clears its busy
  flag**.
- With `child+4 == 0` the walk returns 0 but still advances `cur` to `cnt`, which is exactly the measured
  `cur=1 cnt=1` - so the port executes the guest logic faithfully here; this is not a mis-dispatch.

PCSX2 cross-check (paused at `0x2E0A3C`, the submit continuation the port still never reaches):
`app 0x63E3D0` = `+4=0, +8=1, +0x18=0x63E610, +0x70=1, +0x72=0xFFFF, +0x74=1`;
`child 0x63E610` = `+4=0, +8=0x01`; job table `0x4E9140` = `word0=2 (bit 1 set), payload=0xFFFF`.
Two write watchpoints (`0x63E3D4`, then `0x63E614`) both resolved to `sw v0,4(s0)` sites and both wrote **0**, so
a zero `+4` is a normal transient. The real divergence is the busy byte:

| | port | PCSX2 |
|---|---|---|
| `child 0x63E610 +8` | **0** (idle) | **0x01** (busy) |
| consequence | the walk consumes an already-idle child that has no successor | the walk skips a child that is still working |

So the remaining defect is **below** the app/phase machinery: in the recursive task tree
(`child+0x58` list, count 1, same shape - see `0x3058B8 sw v0,4(s0)`) a task finishes and clears its busy flag
without ever producing the successor object its parent acquires. Everything above it - app run loop, step
dispatch, phase machine, child walk and `objectResetFix` - is verified correct.

### Next probe (exact)

Dump the task tree at the park: level 0 = `app+0x18[i]` (count `app+0x64`), level 1 = `child+0x58[i]` (count 1
per `0x305898`'s `a2=1`), level 2 likewise, printing each node's `+0`, `+4`, `+8`. The first node whose `+8` is
0 while its parent still waits is the leaf that never reports a successor; its step is the fix site.

## 7c. Continuation - the task tree walked to its leaf (all values match PCSX2)

The `[tree]` probe walks level 0's list (`app+0x18`, count `app+0x64`) and then each deeper level through the
embedded sub-object (`node+0x58`, count 1, per `0x305898`). Result - the tree is only two levels deep:

```
[tree] L1 node=0x0063E610 vtbl=0x003D7630 +4=0 busy(+8)=0 state(+38)=0 b4D=0
[tree] L2 node=0x0063E740 vtbl=0x003D7140 +4=0 busy(+8)=0 state(+38)=1 b4D=0..1
```

`0x63E740` is the leaf. Its class: ctor `0x300E50` (sets vtable `0x3D7140`, `+0xC = 0x3D7168`), step
`0x301B00` (vtable `+0x10`), secondary vtable `0x3D7168` (`+0x08 0x301D10`, `+0x0C/0x10/0x14 =
0x238740/0x2387E0/0x2389B0`, `+0x18 0x301C00`, `+0x1C 0x300EC0`). Its fields look like a **streaming read
task**: `+0x24 = 0xB10158` (an 11.5 MB file offset), `+0x2C = 0x63EB50` (buffer), `+0x50 = 7`.

The step's whole decision:

```c
if ((node[0x38] - 2) < 2) {            // state == 2 or 3  -> ready
    FUN_002eca00((*(u8*)(node+0x4E) == 0 ? 3 : 1) | 0x3c);
    FUN_00326aa0(node[0x1D], 1);
} else {                               // anything else -> not ready
    FUN_002eca00(3);
    *(u8*)(node+0x4D) = 1;
    parent->vtable[0x14]();            // 0x205FA0: clears parent +4 and +8
}
node->vtable[0x18](node, parent, 1);
```

Measured: `state(+38) = 1` for the whole run - it never reaches 2, so **every step takes the "not ready"
branch, which is what resets the parent's `+4`/`+8`**. That single fact explains the `app+4 == 0`,
`child+8 == 0` observation of §7b.

**PCSX2 comparison - every value is identical.** The same node exists at the same address with the same vtable,
and at the equivalent point it reads `+0x38 = 1`, `+8 = 0`, `+4 = 0`. So the port is *not* in a wrong state; it
is stuck in a transition that the reference makes. Letting PCSX2 run ~600M cycles completed the loader
(the object memory was then recycled - `0x63E740` now holds stream data `0x012FEF2F…`), i.e. state 1 → 2 does
happen on hardware.

The port's file layer is demonstrably healthy in the same window: 119 `[fileio]` lines, reads up to
544,768 / 522,240 / 389,120 / 262,144 bytes, `gamefile read sector 0x…` into guest buffers. So this is not
"reads are broken"; it is the leaf's own state transition.

### Next probe (exact, two options)

1. **Dynamic (preferred)**: restore a loading-screen savestate in PCSX2, set a write watchpoint on the leaf's
   state word `0x63E778` (`leaf+0x38`), and continue. The write that stores `2` names the exact function that
   must run - then check whether the port ever enters it. (My attempt missed the window because PCSX2 had
   already left the loading screen by then; a savestate makes it repeatable.)
2. **Static**: decompile the leaf's own work methods - secondary vtable `+0x18 = 0x301C00`,
   `+0x1C = 0x300EC0`, `+0x08 = 0x301D10`, and `0x326AA0` (called on the ready path) - and find the store of
   `2` into `+0x38`; its caller is the code the port is missing.

## 7d. Session 152 - second fix: the child step `0x3059B0` was an interior entry with NO switch case

Found by following option 1 (reference-driven) as far as it went, then checking the generated row directly.

```
entry_003059a0 (0x3059A0..0x3059D0) generated body:

    ctx->pc = 0x3059a0u;          // <-- no switch (ctx->pc) prologue at all
label_3059a0:
    lw t9,(a0); lw t9,0x28(t9); jr t9        // tail-jumps to vtable[0x28]
    ...
label_3059b0:                                // only reachable from the 0x3059AC delay-slot resume
    lw t9,(a0); li a2,1; lw t9,0x18(t9); jr t9
```

The phase machine calls `child->vtable[0x10]` = **`0x3059B0`** every step (0x365824). Dispatching there ran the
row from `0x3059A0`, i.e. **`vtable[0x28]` instead of `vtable[0x18]` with `a2 = 1`**. So the AppCell child step
never did its real work, the task tree never published a successor, and the app stayed in phase 3.

Fix: `childStepEntryFix` (installed by `installChildStepEntryFix`) re-expresses the four instructions:
`a2 = 1; t9 = obj->vtable[0x18]; pc = t9;` and tail-jumps with the caller's `$ra` preserved.

Measured (45 s, `TOL_TRACE_ORDER=1`):

| signal | before | after |
|---|---|---|
| `[leafstep]` (tree leaf step `0x301B00`) | **0 calls** | **1 call, `a1 = 0x63DA00`** |
| `app+4` (acquired successor) | always `0` | **`0x63DA00`** |
| `app outer(+0x70) / phase(+0x74)` | `1 / 3` forever | **`4 / 0`** (advanced) |
| `child 0x63E610 +4 / +8` | `0 / 0` | **`0x63DA00 / 1`** (= PCSX2 at `0x2E0A3C`) |

No regression: 15/15 game-visible classes identical to the pre-fix baseline (kputs set, 12 IOP modules, 6 AFS
archives, `no kernel service` set, 1 pre-existing `missing-target`, 0 `No exact recompiled function`,
0 exceptions, render-cadence enabled, 0 drive/CRI errors).

### Note on the earlier instrumentation

`[childstep] ret=0x0063EB00 "success"` (used in earlier sessions to argue the child step was healthy) was the
return value of **`vtable[0x28]`**, the wrong method. A probe can only report what the row actually executed.

### Next

The app is now in **outer state 4** (`FUN_00365FD0`), which is the next stage after the progress/fade machine.
The kernel meanwhile spins in run-loop service B (`[order] dispB a0=0x004079A0 a1=0x00000004`). Continue from
`FUN_00365FD0` (state 4) and check whether it needs the same interior-entry treatment - and, as always,
regenerate with a `case` per instruction (Option A) to retire this whole class; `entry_003059a0` shows a row
can even lack the prologue `switch` entirely.

## 7e. Session 152 - state 4 completes; "service B" is the switch-in, and the loop is idle-slow, not spinning hot

**`FUN_00365FD0` (outer state 4) is healthy** - `[state4]` shows a two-call hand-off per cycle:

```
#1 ENTER outer=4 phase=0 next=0xFFFF   ->  EXIT v0=0 phase=1        (not done)
#2 ENTER outer=4 phase=1 next=0xFFFF   ->  EXIT v0=1 phase=1 next=0 (done)
```

4 calls over 40 s = **2 complete cycles**. On completion it calls `app->vtable[0x20](app, 0)`, and
`0x20D3C0` is literally `sb $a1, 8($a0); jr $ra` - so it **clears `busy(+8)`**, which is what lets
`FUN_00366090`'s `while (app->vtable[0x0C]())` loop finish. `0x365630` then sets `outer = next = 0`, i.e. a
state with no handler (the machine is idle until something re-arms `+0x72`).

**`FUN_0015253C` ("service B") is not a dispatcher** - it is the switch-in accessor:

```c
idx = *(0x3DB1A0) * *(0x3DB1B0);
HI/LO = *(0x48F710 + idx) / *(0x48F720 + idx);
return *(u64*)(0x48F590 + idx);        // context slot
```

It takes **no arguments**, so the `a0=0x004079A0 a1=0x00000004` in the `[order] dispB` line are stale
registers, not parameters. The run loop calls A (`0x152200`) then B (`0x15253C`) once per iteration.

**Why it looks like a spin, measured:** `dispB = 73` in 45 s (~1.6 iterations/s) with ~60 EE dispatches/s.
That is *idle-slow*, not a hot loop. The only runnable thread is slot 4:

```
[tcb] slot4 tcb=0x004079A0 +0x1B4 = 0x003661DC  +0x1AC=1 (alive)  state=1 (ready)
```

i.e. the AppCell app-job thread, parked exactly at its loop's `jal 0x15231C` (0x3661D4). Every iteration it
yields, the run loop re-selects it, and the loop turns once more. Nothing else is runnable to make its wait
complete, so the kernel round-trips through service B without the game advancing.

**Instrument failure worth remembering:** an attempt to print the wall-clock delta between samples with
`std::chrono` inside the probe produced nonsense (`obj=0x00000000`, a constant `+6546384 ms`, and field values
shifted by one word) - i.e. `$s1` was 0 at those entries. That build was reverted; the `[appobj]` probe is back
to the trustworthy version and reproduces `obj=0x63E3D0`. Lesson: a probe that suddenly reports an impossible
object pointer is broken, not a discovery.

### Next

Measure the app loop's cadence **without** `std::chrono` in the probe (e.g. count Timer-2 ticks, or a host
counter read at runtime level) to decide whether the loader is genuinely slow (a few iterations/second) or the
samples are just sparse. If it is genuinely slow, the guest's own `yield(1.0f)` at `0x3661D4` is the suspect -
the same family as the earlier "tick rate drops" observations.

### Measured cadence (runtime counters, no clock)

`[appobj]` now prints the host's own atomics (`g_eeEnterGuestCount`, `g_eeRunLoopIterations`,
`g_eeProcessPendingEventsCount`) instead of a timestamp:

```
#1  enter=25329 loops=25704 events=25704   outer=1 phase=1
#10 enter=26622 loops=27007 events=27007   outer=4 phase=0
```

- over 40 s: Δenter ≈ 1293, Δloops = Δevents ≈ 1303 → **~32 scheduler turns/s**;
- `enter ≈ loops` exactly → **no invocation backlog**; every turn dispatches one guest entry;
- between two app-loop iterations: **~130 turns**, i.e. the app thread yields only ~10 times in 40 s.

So the app loop is *not* the spin: its step runs for seconds of host time, and ~130 scheduler turns
(mostly Timer-2 ticks and their handlers) happen inside it. Note `enter` counts **scheduler** entries only -
nested `dispatchGuestBranch` calls run inside one entry - so it cannot be read as "guest instructions/s".

Guest time base is healthy: `[tick]` logs the first 6 Timer-2 dispatches then every 64th, and 13 lines in 40 s
means ≈450 ticks ≈ 11 Hz (nominal ~16 Hz).

### The loader is now actually streaming

`[xfer] tick-pump` in the same run (device `0x0039EE48`):

```
#1 st=2 done=0    win=266      #2 st=3 done=255  win=255      #3 done=25   win=25
#4 done=39     #5 done=190   #6 done=128   #7 done=17   #8 done=17
```

Before these fixes the same line was stuck at `chunk=0 win=266`. Seven distinct transfer windows and hundreds
of chunks have now completed - the DVCI/ADXF pipeline is moving data, and the app loop's "spin" is its normal
poll-while-loading, not a deadlock.

### Nested work per turn, and the real end state

`g_tolGuestTransfers` (new, incremented in `dispatchGuestBranch`) counts every inter-function transfer, nesting
included:

```
#1  enter=25329 loops=25704 xfer=5995597
#10 enter=26622 loops=27007 xfer=6029484
```

- **Δxfer ≈ 33,887 over 40 s (847/s)** against Δloops 1,303 → **~26 transfers per scheduler turn**;
- per app-loop iteration: ~130 turns and ~5,000 transfers (~38/turn);
- `enter == loops == events` → every turn handles exactly one event and one guest entry: no backlog.

**The 32 turns/s is host pacing, not a spin.** `EeScheduler` pulls in `EeHostPacing.h` and paces guest time to
wall time at VBlank boundaries (`pacingNow`, `m_pacingSpeed`, `TOL_VBLANK_PACING_TRACE`). So the loop runs at
frame rate by design.

**GS backend is irrelevant to this**: `--gs=sw` and `--gs=sdlgpu` produce *byte-identical* counters
(`#1 xfer=5995597`, `#10 xfer=6029484`), because the samples are taken at guest events and the emulation is
deterministic. Do not read those numbers as wall-clock rates.

**The app loop does not stall - it finishes.** `[appobj]` stops at #10 because outer state 4 cleared `busy(+8)`,
so `while (app->vtable[0x0C]())` exits. The EE continues:

```
[order] #100 f1 appjob  a0=0x00407B60            <- slot 5 starts again
[order] #101..#108 f1 iter                        <- run loop top only, no dispA/dispB
[xfer] adx-detach after 4096 spins: refcount 1 -> 0, stream flag 0x394378 = 1
```

**Instrument-interpretation error to avoid repeating:** the counters only print *at appobj samples*, so "the
counters froze" meant "no more app-loop yields", not "the EE stopped". Check the counter's print site before
concluding anything about a stall.

### Current end state (60 s, TOL_TRACE_ORDER + TOL_TRACE_PARK)

```
[tcb] slot2 0x00407580 alive=1 state=1   slot3 0x00407790 alive=1 state=1
[tcb] slot4 0x004079A0 alive=1 parked 0x003661DC   slot5 0x00407BB0 alive=0
[ee-park] thread=2 entry=0x198c60 returned-to-pc0 from=0x166490 ra=0   (repeating)
```

Thread 2 (entry `0x198C60`, a CRI worker) is re-invoked and **dies immediately** - every park is
`from=0x166490 ra=0x0`, i.e. it executes a `jr $ra` with `$ra == 0` right after `0x166490`. That is the next
defect to chase: `0x166490` is the "async-invocation return path around the CRI poll helper" already noted in
`TOL_TRACE_PARK`'s `[path]` probe, so the invocation is being handed a context whose `$ra` was never set.

### Session 152 - the ADX detach handshake, and why the port only ever runs thread 1

The stall after the loader is a handshake deadlock, and every step of it is now measured:

```c
FUN_00199E30 (on the loader thread):              worker (thread entry 0x198C60, id 2, prio 119):
  while (*(0x394380) == 0) {                        if (*(0x394378) != 0) goto done;
      *(0x394378) = 1;                              loop: step(); if (*(0x394378) == 0) goto loop;
      ChangeThreadPriority(t,1);                  done:
      WakeupThread(t);                              *(0x394380) = 1;   // releases the waiter
      FUN_00199200(t);   // ResumeThread if 8/C     ExitThread();
  }                                                 *(0x394378) = *(0x394380) = t = 0;
```

`t` = `*(0x394358)` = **2**. Measured facts:

| evidence | value |
|---|---|
| `[cthread] #2` | `entry=0x198C60 prio=119 stack=0x3943F0 -> id=2` (only 6 threads created - no id wraparound) |
| `[sthread] id=2` | `status=5` = `EeThreadStatus::Dormant`, so `StartThread` accepts it |
| corpus | `FUN_00198c60_0x198c60.cpp` exists, slot present at `register_functions.cpp` - entry is recompiled |
| `[adxdet] #1` | `tid=2 wait(394378)=1 done(394380)=0` - the flag is set, the worker has not answered |
| `[eepc]` (every run, all session) | **`tid=1` only** |

So `FUN_00199E30` sets the flag and wakes thread 2, but **thread 2 never executes** - and the runtime hides
that with `[xfer] adx-detach after 4096 spins`, which writes `0x394378`/the refcount itself.

The reason is scheduling, not thread creation: the port dispatches thread 1 exclusively. Thread 2 is Ready at
priority 119 while thread 1 keeps running (it never blocks, because the waits it performs are served
synchronously by the HLE). Under strict priority nothing at 119 can ever run, so the handshake cannot complete.

**Fix direction (port-level, not emulator-level):** make thread scheduling honour what the guest asks for - a
`WakeupThread`/`ChangeThreadPriority` on an HLE'd primitive must make the woken thread run (reschedule/yield),
instead of returning success while thread 1 continues. That is the guest's actual contract, and it is what the
`4096 spins` hack is standing in for. Do **not** keep the hack once the wake actually schedules.

### Probes added (submodule, both env-gated)

| var | where | output |
|---|---|---|
| `TOL_TRACE_THREADS` | `Thread.cpp` `CreateThread`/`StartThread` | `[cthread] entry/prio/stack -> id`, `[sthread] id/status/entry/arg` |
| `TOL_TRACE_ORDER` | `tol_overrides.cpp` `adxDetachProbe` | `[adxdet] tid/394378/394380/TCB entry+pc` + a one-shot thread-table scan |

### Session 152 - thread identity: the deadlock was manufactured by our own workaround (FIXED)

The `0xDEADBEF0` in `[stop] ... ra=0xDEADBEF0` is **our sentinel**, from `spinReleaseProbe` in
`tol_overrides.cpp`. The old code did this after 4096 spins of `0x198C48`:

```cpp
PS2Runtime::RecompiledFunction detach = runtime->lookupFunction(0x00199988u);
SET_GPR_U32(ctx, 31, 0xDEADBEF0u);
detach(rdram, ctx, runtime);          // runs FUN_00199988 -> ... -> FUN_00199E30 INLINE
```

`FUN_00199988` is "stop all streams" (`0x1999A8` mark flags, `0x1999B0`/`0x1999B8`/`0x1999C0` steps, then
`0x1999C8 jal 0x199E30`), reached through `FUN_0019A2B0` (no xref - called via a pointer). Called inline from
the spin probe it runs on the **waiter's own thread**, and its final detach handshake waits for
`DAT_00394380` - the flag that same thread sets only *after* leaving its spin loop. Hence:

- `[adxdet] curThread=2 ra=0x001999D0 sp=0x00394BD0` (sp at the top of the waiter's own stack),
- `WakeupThread(2) -> -406` = `KE_ILLEGAL_THID`, because `id == m_currentThreadId` (the guest asks thread 2 to
  wake itself).

So the HLE wake path was never at fault; the workaround created the deadlock it was reporting.

**Fix (`spinReleaseProbe`):** apply only the *state* the short-circuited audio close would have applied -
`0x3942C4 = 0` and the six stream flags `0x394378/388/398/3A8/3B8/3C8 = 1` - and **stop hijacking guest code**.
The waiter then sees its flag, sets `0x394380 = 1` and exits through its own `ExitDeleteThread` path.

Verified (60 s): `[adxdet]` = 0 calls, `[stop]` = 0 calls, the new line
`stream release applied after 4096 spins (refcount 0, flags set)` appears, and the regression check is
**14/14 behaviour classes identical**.

### Rendering status - the port *is* drawing

`TOL_GFX_STATS_EVERY=60` on a 120 s run, after the release:

```
[gfx] 1.3 fps over 60 frames | transfers=324 (42189 KiB, 157 forced flushes) draws=4219 passes=499
      resolves=176 (39318 Kpx) refreshes=3 | gif packets=17582 | presents composed=48
[gfx] 60.0 fps ... draws=0 transfers=0 passes=0 ... presents composed=108..348    (idle)
```

So the GS workload is real (4,219 draws / 17,582 GIF packets / 176 resolves / 42 MB of transfers) and frames
are composed and presented; afterwards the scene goes idle at 60 fps with zero draws - i.e. the game is waiting,
not failing to render.

**Open item:** `TOL_GFX_SCREENSHOT_DIR` produced no files in a 110 s run, and no screenshot line appears in
either log, although `gsScreenshotEnabled()` reads the variable. Next: check the latch/writer path in
`sdlgpu_backend.cpp` (who honours `gsScreenshotEnabled()` and whether the composed-present path reaches it), so
the actual frame can be captured and compared with PCSX2.

### Session 152 - screenshots work again, and the black screen is pinned to DISPFB

`writePng()` existed but **had no caller and was in no CMake target** (orphaned in the ToL adaptation), so
`TOL_GFX_SCREENSHOT_DIR` only kept composed pixels alive and produced no files. Fixed: added
`gsdump/gs_frame_image.cpp` to `tol_gfx_gs` and wrote a PNG every 30 composed frames from the composed-present
path in `sdlgpu_backend.cpp`. 40 frames captured, 512x448.

Measured frames (`tools/png_probe.py`, a filter-0 RGBA decoder + luminance map):

| frame | non-black | content |
|---|---|---|
| `frame_000030` (~0.5 s) | **0.66 %** | greys 16/32/48/96/160 - the "NOW LOADING" text |
| `frame_000060`, 090, 150, 300, 1200 | **0.00 %** | fully black |

So the port renders the loading text once and then presents black, even though the same run reports
`draws=4219 gif packets=17582` - the drawing happens, the presentation reads the wrong buffer.

Every captured frame reports `displayFbp=0 sourceFbp=0 preferred=0`, and the port's GS privileged state is

```
[gspriv] PMODE     = 0x66
[gspriv] DISPFB2   = 0x00001000  (fbp=0 fbw=8)
[gspriv] DISPLAY2  = 0x001BF9FF0203227C
```

while the guest's draw target is FBP 224 (`ctxFbp=[224,0]`). **The display base never moves off page 0**, so
the presenter shows an empty page. That is the black screen, and it is independent of the loader stall.

Note on the guest side: **no file in the corpus writes `0x12000070`/`0x120000A0` as an immediate address** -
the GS environment is programmed through computed register addresses, so the flip must be traced on the runtime
side (the handler that records `DISPFB2`), not by grepping the corpus.

**PCSX2 A/B limitation (measured):** the debug server returns **zeros** for GS privileged registers
(`0x12000070`+ read back all zero), so a register-level comparison of DISPFB/DISPLAY is not possible through
the MCP. Comparing the picture needs either a PCSX2 GS dump (`.gs`, whose embedded screenshot
`gs_dump_file`/`gs_replay` can already extract) or a savestate-level VRAM read.

### Next

1. Trace the runtime's GS privileged **write** path for `DISPFB1/DISPFB2`: log `base` and `value` on every
   write. If the guest writes fbp=224 and the runtime drops or mis-decodes it, that is a renderer fix; if the
   guest never writes it, the flip is upstream (init/ordering) and belongs with the loader work.
2. Independently: `sourceFbp` is 0 even though the draw target is 224 - check whether the presenter should be
   falling back to the current FRAME_1 target when DISPFB has not been reprogrammed, which is the port-side
   question rather than an emulation one.

### Session 152 - WHY NOTHING RENDERS: the presenter is stuck on the local-memory path

Draw/read-side probe (`TOL_TRACE_GSFRAME=1`):

```
[gsdraw] #3 surf base=0 psm=0 fbw=8 512x448 scale=1 undefined=1 needsUpload=1 dirty=0 written=[0,0..65,448]
[gsdraw] #4 surf base=0 psm=0 fbw=8 512x448 scale=1 undefined=0 needsUpload=0 dirty=1 written=[0,0..65,448]
[gsread] #1 sourceBase=0 psm=0 surface=yes undefined=0 needsUpload=1 scale=1/1 forceMem=0
[gsread] #2..#8  ... needsUpload=1        <- on every present
```

Three conclusions, in order of how much they overturn earlier assumptions:

1. **The draws target `base 0`, not 224.** `context.frame.fbp` is 0 at draw time. The `ctxFbp=[224,224]` in `[gspresent]` comes from
   `request.contextFrames`, which is a *different* (preferred/stale) source, not the live draw target. So the earlier
   reading "draws to 224, displays 0" was **wrong**: draw target and DISPFB agree on page 0.
2. **Forcing the presenter to page 224 changes nothing** (frames stay black apart from the one "NOW LOADING" frame at
   0.5 s). The presenter was never the problem.
3. **The present path never uses the GPU texture.** `readCircuit` downloads the texture only when
   `surface->needsUpload.empty()`; the probe shows `needsUpload=1` on *every* present, so it always takes the
   local-memory fallback. GPU draws write the texture, not local memory, so the presented image is black.

`needsUpload` is non-empty by construction and is re-merged constantly:
`GsTargetCache::createColorSurface` sets `needsUpload = wholeRegion()` (`sdlgpu_targets.cpp:262`), and
`invalidate()` merges every local-memory write back into it (`:409 surface.needsUpload.merge(written)`, also `:386`,
`:593`). It is only ever *cleared* by the VRAM -> texture upload path (`:768-822`, `:861`, `:960`), which is evidently
not draining this surface.

So the defect is: **the VRAM -> texture upload never drains `needsUpload` for the surface being presented, so the display
path permanently prefers the empty local-memory copy. The renderer is fine; the upload bookkeeping is not.**

### Next probe (exact)

Instrument the drain path (`GsTargetCache` upload: `prepareColorView` / the `needsUpload` handling at
`sdlgpu_targets.cpp:768-822`) with the same `TOL_TRACE_GSFRAME` gate:
- is the upload called for the presented surface at all (count calls per surface base)?
- what region does it take (`upload.x0..y1`) and what does it clear?
- does `applyCpuPatches` / `resolveSurface` run for it?

A surface whose `needsUpload` is never cleared after a successful upload is the bug; if the upload is never invoked for
this surface, the bug is in the caller that decides which surfaces need an upload.

### Session 152 - GsTargetCache probe: a stale shadow surface was hiding the display (FIXED)

`TOL_TRACE_GSFRAME` on `refresh`/`invalidate`/`readCircuit` produced:

```
[gsrefresh] #1 ENTER base=0 psm=0 needsUpload=[]      <- clean when refresh runs (82 calls)
[gsinval]   #1 pages=2 preserveOwned=0               <- only 13 in 45 s, not per frame
[gsread]  #1  sourceBase=0 psm=0 surface=yes 640x896 fbw=10 undefined=0 needsUpload=1
```

So the presenter was reading the **wrong surface object**: two surfaces share `(base 0, psm 0)` - a boot-time
clear created a **640x896 / fbw=10** view, and the real display surface is **512x448 / fbw=8**.
`findColorSurface(base, psm)` returned the *first* match, i.e. the stale 640x896 view, and its `needsUpload` is
permanently dirty because the other view's draws re-merge it (`sdlgpu_targets.cpp:367-370`). That pinned the
present path to the local-memory fallback on every frame.

**Fix:** `findColorSurface` takes an optional `bufferWidth` and prefers the surface whose FBW matches the
caller's display setup (`readCircuit` passes `setup.fbw`). Verified:

```
[gsread] #1 sourceBase=0 surface=yes 512x448 fbw=8 undefined=0 needsUpload=0    <- correct surface, GPU path
```

### ...and the remaining black frame is NOT the present path

The draw probe also showed the target **moves**: early draws go to `base=0`, later ones to
`base=7168` (= `224 << 5`, page 224) with small written regions (65x34, e.g. `[447,351..512,385]`), while DISPFB
keeps displaying page 0. Forcing the presenter to page 224 *with* the selection fix gives:

```
[gsread] sourceBase=7168 surface=yes 512x448 fbw=8 undefined=0 needsUpload=0     <- right surface, GPU path
frames 60/300/900: 100 % black
```

So the primitives reach the GPU with non-empty bounds but **produce no visible pixels**. "Nothing renders" is
now narrowed to the draw contents (texture/CLUT sampling or blend factors), not to page selection, surface
selection or the readback path - all three of which are fixed or eliminated.

### Next probe (exact)

For a sampled draw batch, print what the primitives actually are: primitive type, TCC/TFX/ABE blend factors,
texture `tbp/tbw/psm` (and whether that VRAM region is non-empty), the vertex colours, and the scissor. That
separates "sprites sample an empty texture -> black modulate" from "blend factors make them invisible".

### Session 152 - primitive-content probe: the composite reads a black page 224

`TOL_TRACE_GSPRIM=1` on `submit()`:

```
[gsprim] #3 type=6 iip=0 tme=1 abe=1 tcc=1 tfx=0 A=0 B=1 C=2 D=1 FIX=64
         tex tbp0=7168 tbw=8 psm=0 tw=10 th=9   texRead=1 texel=0x00000000/0x00000000
         v0=(1792.0,1824.0) rgba=128,128,128,128 uv=8,8 scissor=[0,0..511,447] frameFbp=0 psm=0
         ofx=28672 ofy=29184 adj=(0.0,0.0)
```

- primitives are **sprites** (`type=6`), textured (`tme=1`), alpha-blended (`abe=1`), `tcc=1`, `tfx=0`
  (modulate) - a normal HUD/logo composite;
- **coordinates are correct**: `ofx=28672` -> `28672/16 = 1792`, so `adj=(0.0,0.0)` / `(64.0,0.0)` - on-screen.
  (An earlier "12.4 fixed-point not decoded" theory was **wrong** - measured and refuted.)
- scissor correct (`0,0..511,447`);
- **the sampled texture is page 224 (`tbp0=7168`) and is black** (`0x00000000`) while the sprite draws into
  `frameFbp=0` at modulate with a 128-grey vertex colour: black x grey = black.

So the composite itself works; it copies **page 224 -> page 0**, and the source is empty. "Nothing renders" is
therefore a **producer** problem for page 224, not a rasteriser/blend/coordinate problem.

Corroboration from the draw probe: draws targeting `base=7168` do happen (`[gsdraw] #50000 surf base=7168`),
but the composite sprites appear earlier in the stream - so the ordering (fill 224, then composite) may be
reversed on this path.

### Next probe (exact)

Per frame, count draws into `base 7168` (page 224) and the composite sprites that sample `tbp0=7168`, in order.
If composites precede the fills, it is ordering/timing on the present path; if page 224 is filled and still
black, its primitives are failing (then repeat this probe filtered to `frameFbp=224`).

### Session 152 - the black-fill loop: the guest only ever clears (42,716 draws, 5 textured)

`TOL_TRACE_GSORDER` with per-target tallies:

```
fills=42716  comps=6084  fillTex=5  fillRGB=5
[gsorder] #48200 FILL224 target=224 tbp0=7168 tme=0 tcc=1 tfx=0 rgba=0,0,0,0 adj=(192,224) written=[191,223..257,257]
```

- **42,716** draws into page 224, of which only **5** are textured (`tme=1`) and only **5** have any non-zero
  RGB - the rest are `tme=0, rgba=0,0,0,0` **black clears** in a 64x32 tile grid (8 x 14 = 112 tiles = one full
  512x448 clear), i.e. ~380 full-screen clears;
- **6,084** composite blits copy that page into the display page (64-px columns, modulate, 128-grey);
- ordering: the first 18 page-224 events are composites with `fills=0` (the boot frame blits an empty buffer),
  after which every frame is clear-then-blit.

So the port is **not** mis-rendering: the guest is issuing a per-frame black clear and nothing else. There is no
scene/content draw to display. The "black-fill loop" is the **loading screen's clear**, and it ends only when the
loader finishes and the game switches to its scene renderer - the same progression stall isolated earlier
(the app job reaches outer state 4 and clears `busy`, then the kernel idles and no content is ever drawn).

Conclusion for the graphics work: the renderer is exonerated. The three real defects found along the way were
`0x125A78` (CRI end function interior entry), `0x3059B0` (AppCell child step interior entry) and
`findColorSurface` (stale 640x896 view of page 0 pinning the present path to local memory) - all fixed and
verified. The screen stays black because **nothing is being drawn**, and what should end that is the loader
completing, not a GS change.

### Next

Return to progression: after the app job finishes (outer state 4 -> busy cleared), what is supposed to issue the
next job / start the scene renderer? The kernel idles at that point, and no content draw is ever submitted.

### Session 152 - what should start the next scene job: priority 0 was illegal (FIXED), but it is not the gate

Traced the one concrete anomaly in the creation log: the **first** `CreateThread` failed.

```
[cthread] #1 entry=0x001153E0 prio=0 stack=0x003DE020 size=1024 -> id=-403
```

`-403` = `KE_ILLEGAL_PRIORITY`. `FUN_001154b8` creates a thread named **"SceKerneltopThread"** with
`initial_priority = 0` (`uStack_7c = 0`) running `FUN_001153E0`, which is a **deferred thread-command dispatcher**:
it waits on semaphore `0x3DE420` and applies ring-buffer commands - `0 -> WakeupThread`,
`1 -> RotateThreadReadyQueue`, `2 -> SuspendThread`, else an assert.

The port rejected priority 0:

```cpp
if (params.priority < 1 || params.priority >= kPriorityCount) return KE_ILLEGAL_PRIORITY;   // createThread
if (priority < 1 || ...)                                     return KE_ILLEGAL_PRIORITY;   // changePriority
if (priority < 0 || ...)                                     return KE_ILLEGAL_PRIORITY;   // rotateReadyQueue (already correct)
```

On the EE kernel priority **0 is legal** (it is the *top* priority - hence the thread's name), so both checks were
wrong and inconsistent with `rotateReadyQueue`. **Fixed**: both now accept 0.

Verified: `[cthread] #1 ... prio=0 -> id=2` (was `-403`), i.e. "SceKerneltopThread" now exists.

**But it is not the scene gate**: the draw tally is *identical* with the fix
(`fills=42716 comps=6084 fillTex=5 fillRGB=5`), so the guest still issues only clears. The deferred-op dispatcher
was being created incorrectly all along - a real defect, now fixed - but the missing scene job has another cause.

### Where the "no new scene job" question stands

- The app job is **one-shot**: `FUN_00366090` creates its objects, runs `while (app->vtable[0x0C]())`, and after
  the state-4 hand-off (`vtable[0x20]` = `0x20D3C0` = `sb a1,8(a0)`) clears `busy`, exits and tears down.
- `0x365630`'s switch has handlers only for outer states 4, 3 and 1; **outer 0 has none**, so once the hand-off
  sets `outer = 0` the step is inert.
- Nothing re-arms it: no second `CreateThread` for `0x366090` appears, and no draw with content is ever submitted
  (`fillTex = 5` out of 42,716 draws).

So the next thing to identify is the guest code that *re-creates or re-arms* the app job after a load unit, i.e.
the caller of `CreateThread(0x366090)` (or the writer of `app+0x70`/`+0x72`), and what it waits on. That is the
actual scene-job starter.

### Session 152 - identified: the scene job is a vtable method, and the loader step stops being driven

Correcting the earlier model (the app job is **not** a thread, so there is no second `CreateThread` to find):

```
0x165210  j 0x1653F0                       ; loader thread body (guest TCB entry)
0x165220  jal 0x165400 ; jal 0x15231C      ; the STEP: run one app job, then yield, v0 = 1
0x165400  (**(code**)(*(int*)(obj+0x10) + 0xc))()   ; -> vtable[0x0C] of the secondary vtable
secondary vtable 0x3DA380: slots 0x366080, 0x366090, 0x3662D0, 0x366310
                          -> 0x366090 = FUN_00366090 = the AppCell app-job body
```

Static confirmation: the literal `0x00366090` appears exactly once in the ELF, at data address `0x3DA38C`
(= `0x3DA380 + 0x0C`), and `0x3DA380` is installed as `obj+0x10` by the constructors `FUN_00366040` /
`FUN_00366310`. So the "scene job" is a **method invoked from the loader step**, not a thread entry.

**Measured** (`[loaderstep]`, 60 s run):

```
[loaderstep] #1 a0=0x0063D850 v0=0
[loaderstep] #2 a0=0x0063D890 v0=0
[loaderstep] #3 a0=0x0063D890 v0=0
[loaderstep] #4 a0=0x0063D890 v0=0      <- then never called again
```

The step runs the whole `FUN_00366090` body (which itself creates the 0x220-byte app object 0x63E3D0 and loops
until `busy` clears - that is where the observed state 1 -> 3 -> 4 cycles happen) and is then **not driven
again**. So the re-arm point is not the app object and not a thread creation: it is the **guest microkernel
continuing to schedule the loader thread** (TCB `+0x1B4` = the step entry). After the app loop's final exit that
thread is no longer dispatched, and nothing submits another unit.

Note the `[tcb]` dump agrees: slot 4's `+0x1B4` sits at `0x003661DC` (the app loop's yield) rather than at the
step entry `0x165220`, i.e. the thread is parked inside the job, not back at its step.

### Next

Trace the guest microkernel's hand-off of that thread after `FUN_00366090` returns: its TCB status/`+0x1B4`
update and the run-loop selection. That is where the loader thread stops being dispatched - and therefore where
the scene job should have been re-armed.

### Session 152 - TCB + dispatch trace: the loader thread IS queued and IS resumed; it parks itself

Switch-in protocol (`FUN_00152200`, called at `0x151F80`):

```c
tcb = *(0x3DB1A0 * 0x3DB1B0 + 0x48F590 + 0x3DB1C0);   // context slot -> TCB
*(tcb + 0x1A8) = return_address;   // saved $ra
*(tcb + 0x1AC) = 0;                // alive flag cleared: "this thread is now running"
restore HI/LO from tcb+0x180 / +0x190
(**(code**)(tcb + 0x1B4))(a0..a3, s0..s3, gp, tcb, ...)   // call the RESUME pc
```

`FUN_0015243C` (`0x151F78`) is the matching register save into `0x48F590 + idx`. Neither function picks the
thread - they only read the selector triple - so the *observable* choice is which TCB gets `+0x1AC = 0`.

**Ready queue at priority 4 (`[select]`, mid-run):**

```
tcb4=0x004079A0 state=1 prio=4 started=1 next=0x00407580 prev=0x00000000  pc(+1B4)=0x002043F0   <- HEAD (loader)
tcb2=0x0040758  state=1 prio=4 started=1 next=0x00407790 prev=0x004079A0  pc(+1B4)=0x002047F8
tcb3=0x00407790 state=1 prio=4 started=1 next=0x00407370 prev=0x00407580  pc(+1B4)=0x00164FE8
```

So the loader thread (0x4079A0) is **ready, started, and the head** of the priority-4 run queue, and its resume
pc *changes* over the run (0x3661DC early, 0x2043F0 later) - i.e. **it is being resumed**, not skipped.
`0x1AC` is 1 while parked and 0 while inside the switch-in, exactly as the protocol above requires.

**Conclusion:** the thread is not starved by dispatch selection. It parks *inside the app loop*, because the loop
paces itself with `jal 0x15231C` (yield, `a0 = 1.0f`) at `0x3661D4` and its exit condition is `busy(+8)`, which
only the state-4 hand-off clears. The loader step `0x165400` therefore only runs when the loop unwinds back to it
(4 times), not per frame.

### Next

Measure what the yield at `0x15231C` actually waits on and how often it returns (its cadence is the loop's
cadence), and separately why the app loop re-enters instead of exiting after the two state-4 completions already
observed - i.e. who re-arms `busy(+8)` after `vtable[0x20]` clears it.

### Session 152 - the yield waits on nothing, and `busy` is set exactly once

**(1) `0x15231C` is not a wait - it is the cooperative save.** `FUN_0015231C` does:

```c
ctx = *(0x3DB1A0 * 0x3DB1B0 + 0x48F590 + 0x3DB1C0);   // current thread's context
ctx[0x6B] = 1;                                        // +0x1AC = 1  -> "yielded, alive"
... saves the whole register file (gpr/vf/HI/LO/sa) ...
ctx[0x6D] = return_address;                           // +0x1B4 = resume pc
return;                                               // straight back into the run loop
```

No semaphore, no timer, no condition. So the app loop's cadence is exactly "how often the scheduler picks this
thread again", and the thread is the head of the prio-4 ready queue (see previous entry) - i.e. nothing in the
yield is slowing it down.

**(2) `busy(+8)` is written exactly once in each direction, by two different callers** (`[busy]`, 45 s):

```
[busy] attach20D370 app a1=0x0063DA00 a2=0x00000001 -> busy=1  ra=0x00366168   // inside FUN_00366090 (startup attach)
[busy] set20D3C0   app a1=0x00000000               -> busy=0  ra=0x0036600C   // inside FUN_00365FD0 (state 4 -> vtable[0x20])
```

So:

- the only `busy = 1` comes from the app job's **own startup** (`vtable[0x18]` on the app with the partner
  0x63DA00 and mode 1), not from an external re-arm;
- the only `busy = 0` comes from the **state-4 hand-off** (`FUN_00365FD0 -> vtable[0x20]`, ra 0x36600C);
- **nothing sets it again.** The app job is strictly one-shot: it starts itself, runs until state 4 completes,
  clears its own flag, and returns.

That closes the question: the "black-fill loop" is not a loop at the app level at all - one unit runs, hands off,
and the pipeline stops. Although the loader step `0x165400` is called 4 times, only the **first** call reaches
the app job (`0x366090`); the later calls target a different loader object (`a0=0x63D890` vs `0x63D850`) whose
`vtable[0x0C]` is not the app-job entry, so no second unit is ever started.

### Next

Compare the two loader objects' secondary vtables (`0x63D850` and `0x63D890` at +0x10) and their `[0x0C]` slots:
the second/third/fourth `0x165400` calls dispatch somewhere that does not run the app job. That is where the next
unit is being dropped.

### Session 152 - later units are NOT dropped: they are the scene fade, and it is starved

Comparing the two loader objects' secondary vtables at each `0x165400` call:

| call | obj | `vt(+10)` | `vt[8]` | **`vt[C]`** | `vt[10]` | `vt[14]` | obj+4 | obj+8 |
|---|---|---|---|---|---|---|---|---|
| #1 | `0x63D850` | `0x3D4770` | `0x204630` | `0x204690` | `0x2048F0` | `0x204930` | 0 | 64 |
| #2 | `0x63D890` | **`0x3DA380`** | `0x366080` | **`0x366090`** (app job) | `0x3662D0` | `0x366310` | 1 | 0 |
| #3 | `0x63D890` | **`0x3D4750`** | `0x204200` | **`0x204240`** | `0x2045A0` | `0x204140` | 1 | 0 |
| #4 | `0x63D890` | `0x3D4750` | same | `0x204240` | same | same | 1 | 0 |

So object `0x63D890`'s `+0x10` changes from the **app-job vtable `0x3DA380`** to the **loader vtable `0x3D4750`**
between the second and third call - i.e. the object is re-created as the next stage's class (normal object reuse;
the addresses are identical to PCSX2's, so this is guest logic, not aliasing).

`0x204240` (`FUN_00204240`) is a **scene fade-in / hold / fade-out**:

```c
while (thunk_FUN_001f8480(uVar4) != 0) FUN_0015231c();          // wait, yielding
(*obj+0x24)->vtable[0x0C](..., 0xFFFFFFFFFFFFFF00, 0xE0);       // black overlay object
obj+0x20+0x50/54/58 = 1.0f;  obj+0x20+0x5C = 0.0f;             // alpha
do { f += 0.04; *(float*)(*(int*)(obj+0x20)+0x5C) = f;  FUN_0015231c(); } while (f < 1.0);   // FADE IN
do { ... FUN_0015231c(); i++; } while (i < 0x5A);                                        // HOLD <= 90
do { f -= 0.04; *(float*)(*(int*)(obj+0x20)+0x5C) = f;  FUN_0015231c(); } while (0 < f);      // FADE OUT
```

That is the `tme=0, rgba=0,0,0,0` full-screen black quad measured earlier, with alpha driven by `+0x5C`.

**All loops are bounded - about 115 yields per fade - so the fade always completes.** But the measured yield
cadence is ~0.2 per second (the thread is prio 4 and the higher-priority GS workers `0x407160` / `0x407370`
take the turns), so **one fade needs ~10 minutes of wall time**. The screen is black because we never get far
enough into the fade-out to see content.

So the chain is now fully explained end to end: app job one-shot -> loader hands over to the fade -> the fade is
correct but starved -> the visible result is a black screen for minutes.

### Next

The remaining defect is scheduling fairness/rate for prio-4 threads: ~32 scheduler turns/s but only ~0.2 picks/s
for this thread while the GS workers monopolise turns. Either the GS workers are burning turns they should yield
(the `0x1F8150` / `0x1F84B0` legacy pumps are already flagged as "off by default" suspects), or the prio-4 queue
is not being rotated on every `0x15231C` yield (`ctx[0x6B] = 1` then next-thread).

### Session 152 - candidate tests: the scheduler is fair; the fade never brightens the screen

**Candidate (a) "GS workers monopolise turns" - REFUTED.** Per-TCB pick tally at the switch-in:

```
[picks] after 200:  0x407160=47  0x407370=47  0x407580=37  0x407790=34  0x4079A0=33  0x407BB0=2
[picks] after 800:  0x407160=167 0x407370=167 0x407580=157 0x407790=154 0x4079A0=153 0x407BB0=2
```

Round-robin is fair: the loader thread (`0x4079A0`) receives 153/800 = **19 %** of turns. Nothing monopolises.

**Candidate (b) "prio-4 queue not rotated on yield" - REFUTED.** All three prio-4 threads
(`0x407580`, `0x407790`, `0x4079A0`) receive near-identical shares (157/154/153), i.e. the `ctx[0x6B] = 1` yield
does rotate the queue every time.

**The fade is not slow - it never brightens the screen.** 153 captured frames over 200 s:

```
frame_000060 .. frame_009060 : nonblack = 0.00 %  meanLum = 0.0     (every sampled frame)
max over all 153 frames      : 0.00 %
```

So either `FUN_00204240` holds at alpha = 1 (fade-out never runs), or the fade-out completes and reveals
**nothing** - which matches the draw tally (`fillTex = 5` of 42,716): no content is drawn behind the overlay.
The emulator-side picture was not verified visually: PCSX2's debug server exposes no screenshot and returns
zeros for GS privileged registers, so the fade is confirmed from the code and the overlay draws, not from an image.

**Where it stands:** scheduling and the fade mechanism are both exonerated. The game fades to black on a
transition and the destination scene never draws. That is the same conclusion as the draw tally - the guest
issues only clears - and the remaining work is the content path (what should draw after the fade), not timing.

### Session 152 - fade probe built (works), and the real blocker: an invocation loop of 0x166490

**Fade probe (fixed and measuring).** `FUN_00204240` is wrapped, its wait condition `FUN_001F8480` is wrapped, and
the alpha `*(overlay+0x5C)` is sampled every 200 switch-ins. Result:

```
[fade] #1 ENTER obj=0x0063D890 mode(+14)=1  a(+18)=0x014E29E0
[fade] #2 ENTER obj=0x0063D890 mode(+14)=2
[picks] 200: overlay=0x0063D970 alpha=0.600
[picks] 400: alpha=1.000     <- fade-in complete
[picks] 600: alpha=1.000     <- hold
[picks] 800: alpha=0.200     <- fade-out running
```

So the fade is real, completes its ramp, and is **not** the defect. `[fadewait]` never fires, so the pre-fade
wait (`FUN_001f8480`) is not called on this path (the probe found no slot for that thunk) - worth re-checking,
but the alpha ramp proves the fade body runs.

**The screen stays black because nothing is drawn behind the overlay** (draw tally: 5 textured of 42,716).

**The real blocker, newly measured:** with `TOL_TRACE_PARK=1` the EE is saturated by an **invocation loop**:

```
[ee-park] thread=-1 entry=0x0 returned-to-pc0 from=0x166490 ra=0x0 sp=0x1fbbff0 invocations=1
          recent=0x166490, 0x166490, 0x11f650, 0x166490, 0x166490, ...
count = 5,720 in 120 s (~48/s)
```

`thread=-1` is an **invocation carrier**, `0x166490` is the tiny `jal 0x19A068; sync; ei; jr $ra` wrapper, and the
Timer-2 handler `0x11f650` is interleaved. Meanwhile the loader TCB (`0x4079A0`) is parked at `pc=0x204380`, i.e.
inside the fade, at prio 4 / state 1 / started 1.

So: the guest's turns are consumed by repeated invocation callbacks, the loader thread never returns far enough to
start the next stage, and the scene never draws. (The earlier "scheduler stopped at 800 switch-ins" reading was a
sampling artifact - `[picks]` counts switch-ins only, not invocation entries.)

### Next probe (exact)

Log every `queueInvocation` (or `invokeCurrent`) with the invocation's pc, kind and the host call site, rate
limited. That names what re-queues `0x166490` at ~48/s - the Timer-2 handler path or the CRI pump - and that is
what has to stop for the scene to run.

### Session 152 (cont.) - the invocation mix is interrupt-driven: GS IRQ (cause 2) + Timer-2

`TOL_TRACE_INVOC` tallies every queued invocation by pc and kind:

```
[invoc] total=1000  kind0(Interrupt)=867  kind3(RpcCallback)=133
        top: 0x00166490=436   0x0011F650=64 (Timer-2)   0x00125A78=2   0x00126310=2
```

`GuestInvocationKind`: `0 = Interrupt`, `1 = Alarm`, `2 = GsCallback`, `3 = RpcCallback`, ... So **87 % of all
queued invocations are interrupts**, and the top handler is `0x166490`.

Registration found statically: `FUN_001663A0` calls `FUN_00113b50(0x166490)`, which is a wrapper around

```c
uVar3 = AddIntcHandler(2, param_1, -1);   // EE IRQ cause 2
FUN_00115228(2);                          // enable it
```

**EE IRQ cause 2 is the GS interrupt**, and `0x166490` is its (tiny) handler: `jal 0x19A068; sync; ei; jr $ra`.
So the guest's turns are dominated by GS-interrupt dispatch plus the Timer-2 handler, and the loader thread is
parked inside the fade (`pc=0x204380`, prio 4, state 1, started 1).

Together with the fade probe this completes the picture of "why nothing renders":

1. the fade is correct and completes (alpha 0.6 -> 1.0 -> 0.2),
2. nothing is drawn behind it (5 textured draws of 42,716 - the scene never draws),
3. the EE spends ~87 % of its invocations on GS/Timer-2 interrupts while the loader thread sits inside the fade.

So the guest is not deadlocked - it is *interrupt-bound* with no scene content, and each scheduler turn is
expensive enough (~0.7 s per loader-thread yield) that the fade alone takes ~90 s.

### Next probe (exact)

Two candidates, both now sharply defined:
1. **GS interrupt rate**: the port raises cause 2 ~7x/s while the guest is only clearing/fading. Log the raise site
   in the runtime's GS/intc path (`signalInterrupt`/`raiseInterrupt` callers) with a per-source tally; a
   per-packet raise where hardware raises per-DMA-completion would explain both the rate and the heavy turns.
2. **Cost per turn**: with interrupts tallied, time (or count guest dispatches inside) one GS-interrupt
   invocation versus one ordinary turn - if the handler's `jal 0x19A068` path is expensive, the interrupt rate is
   what makes the whole guest crawl.

### Session 152 - measured: VBlank runs at 12.4 Hz (not 60), interrupts are cheap, the guest is idle-waiting

**Interrupt rate** (`TOL_TRACE_EE_IRQ`, 60 s) and the raise sites, which correct the cause mapping:

```c
case EeEventType::VBlankStart: dispatchIrq(false, 2u);      // cause 2 = VBLANK START
case EeEventType::VBlankEnd:   dispatchIrq(false, 3u);      // cause 3 = VBLANK END
                               dispatchIrq(false, 9u + timer);  // Timer N -> 11 = Timer 2
```

```
[eeirq] total=1800  c1=188  c2=743  c3=737  c5=23  c11=109
```

| cause | meaning | rate |
|---|---|---|
| 2 | VBlank start | **12.4 /s** |
| 3 | VBlank end | 12.3 /s |
| 11 | Timer 2 | 1.8 /s |
| 1, 5 | other | 3.1 /s, 0.4 /s |

`constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);` - i.e. the runtime *asks* for 60 Hz and
delivers **12.4 Hz**, a 4.8x shortfall. (Earlier notes calling cause 2 "GS" were wrong; it is VBlank start.)

**Cost per interrupt** (`[gscost]`, wrapping the cause-2 handler `0x166490` and measuring `g_tolGuestTransfers`):

```
isr_calls=600  isr_transfers=1399  avg=2.33  total_transfers=6180721
```

**2.33 guest transfers per interrupt** versus **~26 per ordinary switch-in** - the handler is cheap and is *not*
the cost. What matters is the throughput: between ISR 200 and ISR 600 (~33 s) the entire guest advanced
**977 transfers ≈ 30/s**. So the guest is not computing - it is **waiting**, paced by VBlank at 12.4 Hz.

So the fade (and the whole boot) is slow for one reason: **the guest only gets ~12 frames per second**, and each
frame's guest work is trivial. The bottleneck is the VBlank/pacing rate, not interrupt cost, not scheduling
fairness, not the renderer.

### Next probe (exact)

`EeScheduler`'s VBlank boundary emitter: why does it fire at 12.4 Hz when `kVBlankPeriod` is 16667 us? Dump
`ee_host_pacing::VBlankBoundary` (there is already a `TOL_VBLANK_PACING_TRACE` around `traceVBlankPacing`) and
check whether the boundary is being rebased/late every frame (the trace prints `starts=`, `rebases=`,
`max_late_us=`, `next_host_us=`). A boundary that rebases every frame, or a `next_host_us` far in the past,
would explain a fixed ~5x shortfall.

### Session 152 - pacing measured: guest clock is right, host wall is 4.8x worse; the guest spins in a queue

`TOL_VBLANK_PACING_TRACE`:

```
[VBlank pacing] tick=1  cycle=4915299   host_us=54440889440  starts=1 rebases=1 max_late_us=320340
[VBlank pacing] tick=41 cycle=201527259 host_us=54468728117  starts=3 rebases=3 max_late_us=948612
```

- `Δcycle` = 4.9 M per tick -> at 294.9 MHz that is **16.67 ms of guest time: the guest's own frame clock is a
  correct 60 Hz**;
- `Δhost` = 27.8 s over 41 ticks = **~680 ms of wall time per guest frame** (~40x slower than real time);
- `rebases == starts` on every line - **every boundary rebases instead of catching up**, with `max_late_us` ~0.95 s.
  So the pacer has given up on catching up; it is a symptom of the host being 40x behind, not the cause.

`TOL_GFX_STATS_EVERY=120` splits the wall time:

| | loading phase | idle phase |
|---|---|---|
| presented fps | 2.5 | 60.0 |
| **GS backend share of wall** | **4 %** | 26 % |
| ee-dispatches / frame | 390 | 1 |
| sched loop | 1010 /s | 337 /s |

So the port is **not render-bound**: the whole GS path is 4 % of wall time. The scheduler loop turns ~1000
guest entries per second, yet only ~12 VBlanks/s arrive.

`TOL_TRACE_EE_PC` shows where those entries go - one guest cluster on tid 1:

```
0x349C9C x104  0x349DD8 x101  0x349D68 x99  0x349D48 x91  0x3499A8 x36
0x27B45C x36   0x3499D0 x32   0x349A38 x32   0x27B488 x32  0x27B4E0 x31
```

`0x3498xx-0x349Dxx` is `FUN_00349C00` and its neighbours: a **queue walk that dispatches items matching a
predicate** (`0x3341A0` init, `0x3343E0` next, `0x334400` item, `0x3344E0`, `thunk_FUN_00334530` process).

So the guest burns its ~1000 entries/s walking that queue while the frame (and therefore VBlank progress) barely
advances. The 12.4 Hz is a *consequence* of this spin, not a separate defect.

### Next probe (exact)

Identify the queue `FUN_00349C00` walks and why it never drains: log its `param_1` (type), the item count returned
by `FUN_003343E0`, and how many items hit `thunk_FUN_00334530` per call. A queue that is always non-empty and
never matches the predicate is a permanent spin - and that would be the thing to fix rather than the pacer.

### Session 152 - the queue walk is an empty-queue poll: nothing is enqueued (8 matches ever)

Instrumented the walk (`0x349C00`), its iterator (`0x3343E0`) and its match callback (`0x334530`) behind
`TOL_TRACE_QUEUE`:

```
[queue] walk #1 type=1 (yields=10 matches=8)
[queue] yields=500  ret=0x00000001 matches=8  type=16 walks=231
[queue] yields=5000 ret=0x00000001 matches=8  type=16 walks=2481
```

| metric | value | meaning |
|---|---|---|
| walks | 2,481 / 40 s = **62/s** | the guest polls 18 queues at ~60 Hz |
| items yielded | 5,000 (~2 per walk) | each queue holds 1-2 entries |
| **predicate matches** | **8, all at the very start** | nothing work-worthy was ever queued again |
| `FUN_003343E0` return | **1** on every sample | "no more items" - the walk exits immediately |

So this is **not a spin**: it is a normal 60 Hz poll of empty queues. The stall is that **nothing is enqueued** -
the producers stopped after the first 8 items. That matches the rest of the chain measured today: one app job ran
and never re-armed (`busy` written once each way), the fade completed and revealed nothing, and the loader stage
never invoked the app job again.

### Answer to "do we need a native fade override?" - NO

There is no reason to override the fade and several against:

1. **It works.** Measured: alpha 0.6 -> 1.0 (hold) -> 0.2, two invocations (modes 1 and 2), driven by the guest's
   own `FUN_00204240`. A native replacement would substitute fabricated behaviour for correct guest logic - the
   port rule broken for no gain.
2. **It is not the blocker.** The screen is black because the guest draws nothing behind the overlay
   (5 textured draws of 42,716), not because the fade misbehaves. Overriding the fade would hide the symptom and
   leave the actual defect (empty queues / no scene submitted) unfixed.
3. **Its timing is guest state.** The ramp is 0.04 per iteration against the guest's own frame counter; a native
   fade would have to guess the same rate and would drift from what the game expects.

So: keep the guest's fade, fix the producer side. The next probe is the enqueue path - who submits to these
queues and why they stop after 8 entries.

### Session 152 - ROOT CAUSE of the stalled load: the mode byte at gp+0x4A86D9 is zero

Instrumented the walk's caller and its argument chain:

```
[queue] walk #1   type=1  a1=0x00000001 a2=0x00000000
[queue] walk #200 type=18 a1=0x00000001 a2=0x00000000
[queue] table=0x01562FA0 active=230 hist= t1=61 t2=62 t3=8 t4=91 t5=8 t6=3
[queue] type2 states: s0=1 s1=5 s2=5 s3=5 s4=5 s5=5 s6=4 s7=4 s8=3 ... s16=1 s17=1 s18=1 ...
[queue] modeByte gp=0x003E2BF0 [0x0088B2C9]=0x00  which0=[0,0,0]  which1=[0,0,0]
```

The chain, verified end to end:

1. `FUN_0027B390` (the 60 Hz poller) calls the walk three times:
   `FUN_00349C00(0x10, FUN_0027B870(0,0), FUN_0027B870(1,0))` and the same for `0x11`/`0x12`.
2. `FUN_0027B870(which, idx)` decodes **one byte at `gp + 0x4A86D9`** as six 2-bit fields
   (idx0: bits 0/1, idx1: bits 2/3, idx2: bits 4/5). `a2` is the `which=1` bit of each pair -> bits **1, 3, 5**.
3. `FUN_00349C00`'s switch processes types 16/17/18 **only if `a1 == 1 && a2 == 1`**.
4. The walk *does* find candidates - the 233-slot table (`*(0x3DB660)`, stride 0x18, type nibble at +0x24, code at
   +0x25) holds **230 active slots**, and exactly one type-2 entry in each of states **16, 17 and 18**.
5. But the mode byte is **0x00**, so `a2 == 0` always, the `&&` fails, and those three entries are found and
   **skipped on every one of the 2,481 walks**. `matches` stays at 8 (the boot-time ones) forever.

So the loading is not stalled by a spin, an empty queue, the scheduler, the fade or the renderer: **it is gated on a
single mode byte, `gp+0x4A86D9`, which nothing ever sets.** The three waiting entries (states 16/17/18) can only
advance when bits 1/3/5 of that byte are set.

### Next probe (exact)

Find the writer of `gp+0x4A86D9` - the scene-mode setter that should run when a scene starts. Its bits 0/2/4 (the
`which0` fields) are also clear, so the whole "mode" is uninitialised, which points at an init call that did not
happen rather than a per-field bug. Watch the byte (range watch on a 4-byte window around it) and log the PC that
writes it; if nothing ever writes it, the init path is the missing piece.

### Session 152 - REFUTED by the reference: the mode byte is 0x14 on hardware too

Corrected the address first: `FUN_0027B870` reads `*(u8*)(index * 0x12D8 + 0x4A86D9)` where
`index = *(gp - 0x7844)`; the index is 0, so the byte is the **static** address `0x004A86D9` (not the
`gp + 0x4A86D9` I first printed). Watched over 30 s:

```
[queue] mode byte idx=0x00000000 addr=0x004A86D9 val=0x14 changes=1 which1=[0,0,0]
```

`0x14` = `0001 0100` -> idx0 which0/which1 = 0/0, idx1 = 1/0, idx2 = 1/0. The `which1` bits (1, 3, 5) - exactly
what `a2` reads - are 0, and `changes=1` means **nothing writes the byte in the port**.

**PCSX2 reads the same bytes:**

```
PCSX2 [0x4A86D9] = 0x14
```

So the guest *legitimately* leaves `which1` clear: the walk's `a1 == 1 && a2 == 1` gate for types 16/17/18 is
simply not enabled at this point on hardware either. The earlier "root cause = uninitialised mode byte" note is
**withdrawn**; the 8 matches are normal and the 60 Hz poll is normal.

What remains true from this line of work: the 233-slot table is **populated** (230/233, types 1-5, one type-2 entry
each in states 16/17/18), the walk runs at a normal 60 Hz, and `matches` stays at 8 *by design*.

### Where the investigation turns next

The measured anomaly that is still unexplained is the fade's **first wait**:

```c
uVar4 = FUN_001f72f0(obj+0x14, obj+0x18, 0);
while (thunk_FUN_001f8480(uVar4) != 0) FUN_0015231c();     // <- before anything is drawn
```

The loader TCB (`0x4079A0`) was parked at `pc=0x204380`, i.e. **inside `FUN_00204240`**, and the loader step
`0x165400` is invoked only 4 times - consistent with the thread sitting in that wait loop rather than returning to
its step. My probe on `0x1F8480` never fired, which means that address is not the function behind
`thunk_FUN_001f8480` (the thunk targets something else). Naming the real target and probing its return value is
the next concrete step: if it never returns 0, the fade never draws, the thread never returns, and the pipeline
never re-arms - which would tie every observation together without touching the renderer or the guest's fade.

### Session 152 - the fade's wait DOES fire and DOES terminate (hypothesis refuted)

My probe had been installed on the *target* `0x1F8480`; the fade calls the trampoline **`0x1F7310`** (`j 0x1F8480`),
so it never fired. Retargeted:

```
[fadewait] #1  a0=0x00000080 ret=0x00000002
[fadewait] #2  a0=0x00000080 ret=0x00000001
[fadewait] #3  a0=0x00000080 ret=0x00000000     <- the wait ends
[fadewait] #4  a0=0x00000101 ret=0x00000001
[fadewait] #5  a0=0x00000101 ret=0x00000000
...                                            (12 waits in 40 s)
[fadewait] #12 a0=0x00000305 ret=0x00000001
```

So `FUN_001f8480` (-> `*(u8*)(obj+2)`, or 0xFF when null) counts down **2 -> 1 -> 0** and the loop exits every
time, and successive objects (`0x80`, `0x101`, `0x182`, `0x284`, `0x305`) are awaited in turn. **The fade's wait is
healthy and is not the blocker** - another hypothesis refuted by measurement rather than by argument.

### Where that leaves the chain (all measured, each link verified)

- 233-slot resource table populated (230/233), polled at a normal 60 Hz;
- the walk's `a2` gate is closed on hardware too (mode byte `0x4A86D9 = 0x14` in both port and PCSX2);
- the fade runs, its wait terminates, its alpha ramps 0.6 -> 1.0 -> 0.2;
- the app job is one-shot (`busy` written once each way, ra 0x366168 / 0x36600C);
- the loader step `0x165400` is invoked only **4** times in the whole run;
- the guest draws no content (5 textured draws of 42,716).

So the one thing still unexplained is precisely the **5th** loader-step invocation that never happens after the
second fade returns. The fade returns normally, so the next question is what happens to the loader thread at that
moment: whether it exits (and nothing re-creates it) or stays parked in a state nobody re-arms.

### Next probe (exact)

Watch the loader TCB (`0x4079A0`) across the moment the last fade returns: `+0x1AC` (alive), `+0x1F0` (state),
`+0x1B4` (resume pc), plus `+0x1A8`, sampled once per second; and log whether `0x165400` is ever entered after it.
If the thread's alive flag drops to 0, the loader thread ends after the fade and the missing piece is whatever
should re-create it; if it stays alive with a pc outside the step, the re-arm is a guest state nobody sets.

### Session 152 - the loader thread parks in the fade-in loop at 0x204380; the step never runs a 5th time

Sampled TCB `0x4079A0` (`[tcb4]`, every 20th switch-in) together with the loader-step and fade counters:

```
[tcb4] sel=20  alive=0 state=0 pc=0x00000000 loaderSteps=0 fades=0
[tcb4] sel=40  alive=1 state=1 pc=0x003661DC loaderSteps=2 fades=0     <- app loop yield
[tcb4] sel=100 alive=1 state=1 pc=0x003662AC loaderSteps=2 fades=0
[tcb4] sel=820 alive=1 state=1 pc=0x00204568 loaderSteps=3 fades=1
[tcb4] sel=840 alive=0 state=0 pc=0x00165238 loaderSteps=3 fades=1     <- mid switch-in (alive cleared while running)
[tcb4] sel=860 alive=1 state=1 pc=0x00204380 loaderSteps=4 fades=2
[tcb4] sel=880..900 alive=1 state=1 pc=0x00204380 loaderSteps=4 fades=2   <- frozen
```

So the thread is **not dead**: `+0x1AC` = 1 (yielded/alive), `+0x1F0` = 1 (ready), and it is dispatched normally at
prio 4. It simply stays at `pc = 0x00204380` for the rest of the run, and `loaderSteps` never reaches 5.

`0x204380` disassembles to the **fade-in loop condition**:

```asm
0x204378  jal 0x0015231C        ; yield
0x204380  lui v0,0x3F80         ; f0 = 1.0f
0x20438C  c.lt.s f20, f0        ; f20 < 1.0 ?
0x204390  bc1t 0x0020433C       ; loob back into the fade-in body
```

So the second fade's **fade-in never converges**: `f20` (the alpha accumulator at +0x5C's source register) does
not reach 1.0, the loop never exits, the fade never returns, the loader step is never re-entered, no further unit
is submitted, and the screen stays black.

That is the concrete end of the chain - and note it is *not* the fade's design (the first fade completed,
alpha 0.6 -> 1.0 -> 0.2). The suspicion is that `f20` does not survive the `0x15231C` yield/resume for this
second invocation: the switch-in (`0x152200`) restores only HI/LO and calls `TCB+0x1B4`; the full GPR/FPU context
is saved by `0x15243C` into `0x48F590+idx`, so a resume that does not go through the context-restore trampoline
would leave `f20` holding another thread's value.

### Next probe (exact)

Sample `f20` (from the live `R5900Context`) at `0x204380` on each hit, together with `+0x5C` of the overlay object:
- if `f20` is constant or does not advance by 0.04 per iteration -> the accumulator is not surviving the yield,
  and the fix is in the resume path (context restore), not in the fade;
- if `f20` advances past 1.0 but the branch still loops -> `c.lt.s` is mis-evaluating, which is an FPU issue.

### Session 152 - f20 is fine: the fade advances 0.04 -> 1.00 across yields; the thread is slow, not stuck

Probe on `0x204380` (the fade-in loop condition), sampling the live FPU register `ctx->f[20]` and the overlay's
stored alpha:

```
[fadef20] #1  f20=0.0400 alpha(+5C)=0.0400 overlay=0x0063D970
[fadef20] #2  f20=0.0800 alpha(+5C)=0.0800
...                                            (+0.04 per hit)
[fadef20] #25 f20=1.0000 alpha(+5C)=1.0000     <- fade-in completes
[fadef20] #26 f20=0.0400 alpha(+5C)=0.0400     <- the NEXT fade starts
[fadef20] #27 f20=0.0800 ... #30 f20=0.2000
```

Three hypotheses die here:

- **`f20` survives the `0x15231C` yield** - it advances by exactly 0.04 per iteration and reaches 1.0;
- **`c.lt.s` evaluates correctly** - the loop always exits at 1.0 (so the row's dense case for `0x204380` is fine
  too: `case 0x204380u` exists, checked statically);
- **the thread is not parked forever** - after the first fade-in completes, the game starts the *next* fade
  (`fades` went 1 -> 2), which is why the TCB was repeatedly sampled at `0x204380` at the end of a 60 s run.

**So there is no deadlock**: the fade needs ~140 scheduler turns (25 in + up to 90 hold + 25 out) and the game
performs two fades, ~280 turns. At the measured rate that is minutes of wall time, which looks identical to being
stuck over any run length tried so far.

That collapses the whole investigation to one number: **the cost of a scheduler turn** (~0.7 s, ~40x slower than
the guest's own 60 Hz frame). Everything downstream - the fade taking minutes, "no scene job", the black screen -
is that one rate.

### Next probe (exact)

Account for the ~0.7 s per turn directly: the GS backend is only 4 % of wall (measured), so instrument the three
other consumers on the turn path - (a) the interrupt/invocation dispatch (`dispatchIrq` -> `queueInvocation` ->
`enterGuest`), (b) the VBlank boundary emitter, (c) the presenter's per-frame work - with a shared counter of
cumulative microseconds each. Whichever dominates is the thing to fix; the guest logic, the fade and the renderer
are all behaving correctly.

### Session 152 - turn cost accounted: 49 % guest / 49.9 % pacing sleep / 1.1 % presenter

`TOL_TURN_COST` timers (cumulative microseconds) in `EeScheduler.cpp` (`enterGuest`, `ee_host_pacing::waitUntil`,
`m_eventCv.wait`) and `ps2_runtime.cpp` (`m_framePump`), reported every 200 presents. After a full 60 s run:

```
[turncost] presents=28400 guestEntries=48964 paceWaits=3810 |
           guest=29020.7ms(49.0%)  paceWait=29527.5ms(49.9%)  presenter=674.4ms(1.1%)
           total=59222.6ms  per-present=2.09ms
```

| consumer | share of wall | per iteration |
|---|---|---|
| (a) guest execution (`enterGuest`) | **49.0 %** | 1.02 ms |
| (b) VBlank pacing wait (`ee_host_pacing::waitUntil`) | **49.9 %** | 1.04 ms |
| (c) presenter (`m_framePump`) | **1.1 %** | 0.024 ms |

So:

- the **presenter is 1 %** - the renderer, the GS backend and the fade are conclusively not the cost;
- the loop runs **~473 iterations/s at 2.09 ms each**, so the "~0.7 s per turn" figure from the TCB sampling was
  an artifact of sampling every 20th switch-in (each sample spans many iterations) - not a real per-turn cost;
- wall time is split **evenly between executing guest code and deliberately sleeping in the VBlank pacer**. The
  port is doing exactly what pacing asks: run ~1 ms, sleep ~1 ms.

Therefore there is no "slow turn" left to fix. What remains true is that the guest does almost no *useful* work
per frame (draws: clears only), so game progress is slow in game terms even though the emulator is
running at its intended rate. That is the loader pipeline, not a performance defect.

### Conclusion of this line of work

Every consumer on the turn path is now measured, and the two biggest (guest 49 %, pacer 49.9 %) are both behaving
as designed; the presenter at 1 % rules the renderer out for good. If more throughput is wanted it must come from
the guest doing real work per frame (the loading pipeline), not from the scheduler, the pacer or the renderer.

### Session 152 - the post-fade scene start DOES run (all three calls), and still nothing draws

Instrumented the fade's mode==1 tail (`TOL_TRACE_SCENE`):

```
232190 count 11  first: a0=0x004E9D70 a1=0x3F3AE148(0.7297f) ret=0x005AA170   <- engine object
285e80 count 1   first: a0=0x005AA170 a1=0x00000004           ret=0x00000000   <- scene/view object
2321b0 count 2   first: a0=0x004E9D70 a1=0x00000001           ret=0x00000001   <- success
```

This matches the source exactly: `FUN_00285E80(FUN_00232190(engine, 0.7297f), 4)`, with the pointer returned by
`232190` (`0x5AA170`) passed straight in, and `FUN_002321B0(engine, 1)` returning 1.

An earlier run showed `285e80` missing; that was a run without the full flag set - with `TOL_TRACE_SCENE` +
`TOL_TRACE_ORDER` it fires once, early. **Corrected**: the scene start is not skipped.

So the chain is now verified *through* the scene start:

```
app job one-shot -> loader hands over -> fade (works) -> scene start: 232190 -> 285E80 -> 2321B0 (all run, success)
                                                                          |
                                                                          v
                                                        ...and no content is ever drawn
```

**The remaining defect is after the scene start**: the scene object (`0x5AA170`, built from the engine at
`0x4E9D70`) is created and reported ready, but nothing then draws it - the per-frame scene render/pump never
produces a single textured primitive (5 textured draws of 42,716 for the whole run).

### Next probe (exact)

Follow the scene object: read its vtable at `0x5AA170`, identify its step/draw slot, and probe that slot's call
count. Then either the pump is never called (a scheduling/registration gap) or it is called and draws nothing
(inside the scene renderer) - and the same instrument decides it in one run.

### Session 152 - ANSWER: the scene object is never pumped (vt+0x38 never called)

Read the scene object handed to `FUN_00285E80`:

```
[scenestep] scene obj=0x005AA170 vtable=0x003D50D0
  +8=0x285150 +C=0x231DF0 +10=0x1ED5E0 +14=0x285280 +18=0x285330
  +1C=0x2853D0 +28=0x2895F0 +38=0x292520          (the rest null)
[scenestep] installed vt+10 = 0x001ED5E0
[scenestep] installed vt+38 = 0x00292520
```

Call counts over 45 s:

| slot | function | calls |
|---|---|---|
| `vt+0x10` | `0x1ED5E0` | **10** (a0 alternates `0x4E9DB0` engine / `0x5AA170` scene) |
| `vt+0x38` | `0x292520` | **0** |

**So the scene object exists, is reachable, and is queried through `+0x10`, but its step/render entry at `+0x38`
is never called.** Per the decision rule this is the first branch: **the pump/registration is the fix, on the port
side** - not inside the scene renderer, which is never given a chance to run.

That is consistent with everything measured above it: the loader, the app job, the fade and the scene start all
complete correctly, and then the scene sits idle because nothing drives it each frame. It also explains the final
rendering fact directly - 5 textured draws of 42,716 - since a scene that is never stepped cannot submit
geometry.

### Next probe (exact)

Find what should call the scene's `+0x38`: locate every site that walks scene objects and invokes that vtable slot
(xref the slot value `0x3D50D0 + 0x38 = 0x3D5108`, and trace the engine's per-frame update - the same object that
`FUN_00232190` returns is presumably registered somewhere). Either the registration never happens (guest state) or
the pump loop runs but skips this object - and the same instrument decides it in one run.

### Session 152 - engine update traced end-to-end; the scene IS pumped, and its entries sit at state 0

**The engine's per-frame update, fully traced:**

```
FUN_00164F80   frame loop
  while (taskIsBusy) {
    FUN_0015231c(); dispatch channel 7; 9; 8;  do { flag=0; dispatch 6 } while (flag)
    FUN_0016e3b0();
    FUN_001ee060();                    <-- engine tick
    FUN_0016d230();
  }
    |  obj = FUN_001ed0c0()  = 0x4E9D70
    v
  (*obj->vt[0x1C])(obj) = FUN_00232110
      (*engine[+0x0C]->vt[0x1C])()   0x4E9DB0  vt 0x3D4AC0 -> 0x230A50
      (*engine[+0x10]->vt[0x1C])()   0x5AA140  vt 0x3D4B00 -> 0x231EA0
      (*engine[+0x18]->vt[0x1C])()   0x5AA170  vt 0x3D50D0 -> 0x2853D0   <- the SCENE
```

`FUN_00151ff0(id)` is not a sleep: it is `FUN_00151e90(id)`, which walks the ready queue and switches to
every thread whose `+0x1F1` equals `id` - i.e. "run everyone waiting on channel `id`".

**Tick rate measured (`[tick]`, wall clock):** ~24.4 s with *no* tick at all after the first one (boot/loader
window), then a steady **~9 Hz** (92-125 ms), `task=0x00497050` from about t=27.6 s. The scene is first stepped at
**t ~ 36 s**, right after `FUN_00285E80`, at that ~9 Hz - fast enough for its own 9-step cooldown.

**Xref of the +0x38 slot (`0x3D50D0+0x38 = 0x3D5108` -> `0x292520`):** zero static references (vtable-only), and
the function is the **destructor** (`*this = &DAT_003d5100`, frees +0x04/+0x0C/+0x10/+0x18, optional self-free).
It is correctly never called while the scene lives. It is **not** the pump.

**Correction to the previous session's answer:** the pump slot is `+0x1C`, not `+0x38`, and the scene **is**
pumped. With per-object counting (the earlier probe logged only its first ten calls, all belonging to another
object that shares the entry):

```
[scenestep] vt+1C SCENE #1 (total 1) a0=0x005AA170
[scenestep] vt+1C SCENE #2..#5       a0=0x005AA170
```

**What the scene step does** (`0x2853D0` -> `0x285410`, `0x2855E0`, `0x2854A0`, `0x2857C0`) is a per-entry
streaming state machine, not a renderer:

| function | role |
|---|---|
| `FUN_002855E0` | per entry, only if `FUN_00197668(entry[0x2C]) == 3`: state 5 -> read chunk, accept when magic == `0x53504C54` ("TLPS"); state 6 -> size |
| `FUN_002854A0` | per entry, gated on `scene[0x1C] == 0` (cooldown 8): state 1/2/3/4 -> `0x2928F0/292950/292990/292A60` |
| `FUN_002857C0` | per entry: `FUN_00292800(entry[0xC])` (an empty stub) and `FUN_00292810(entry)` |
| `FUN_00292810` | takes **one** argument (`lwc1 f1,0x140(a0)`, `lw a0,0x30(a0)`); drives the drawable `entry[0x30]` |

**Measured (75-90 s runs):**

```
[scenestream] steps=100  tlpsAccept=27  commit810=396  commit800=396
```

- the I/O gate is **open**: handle `0x390738` reports status `3` in 17/20 samples (2 transiently),
- **27 TLPS chunks** were read off the disc and accepted - real data, real file layer,
- **396** per-entry commits ran.

**Where it stops:** all four scene entries are alive and wired, but idle:

```
entry=0x005AA1B0 drawObj(+30)=0x003933C8 file(+34)=0x003B1C18 state(+14C)=0 scale(+140)=0.500
entry=0x005C7E80 drawObj(+30)=0x0039348C file(+34)=0x003B1CD8 state(+14C)=0 scale(+140)=0.500
entry=0x005E5B50 drawObj(+30)=0x00393550 file(+34)=0x003B1D98 state(+14C)=0 scale(+140)=0.500
entry=0x00603820 drawObj(+30)=0x00393614 file(+34)=0x003B1E58 state(+14C)=0 scale(+140)=0.500
```

`state(+0x14C) = 0` is "idle": `FUN_002854A0` only acts on **1..4**, `FUN_002855E0` only on **5..6**. Nothing ever
kicks an entry out of 0, so no entry ever enters the load pipeline through this path - the four draw objects are
never fed, and the GS only ever sees clears.

Refuted this session: "the scene is never pumped" (wrong slot + throttled log), "scene master alpha is 0"
(`FUN_00292810`'s first parameter is an object pointer, not a float - the Ghidra prototype is wrong).

### Next probe (exact)

Find the writer of `entry+0x14C` that sets it to `1` (the load kick) and why it never runs for these four
entries: probe the scene setup methods (`0x285150`, `0x285280`, `0x285330`, `0x2853D0`'s callees) for stores to
`+0x14C`, and check whether the kick is gated on `FUN_001a1460(entry[0x30])` or on the entry being "visible"
(a camera/frustum test) - a scene that is never marked visible would leave every entry at 0.

### Session 152 - the writer of entry+0x14C: a complete table, and every load request is dead

Every write to `entry+0x14C` in the scene cluster, with its caller and what it was measured doing:

| state | writer | who calls it | measured |
|---|---|---|---|
| **1** | `FUN_002928b0` (`FUN_0010b7e8(entry+0x40)` then `1`) | `FUN_002311b0` | **0 calls** |
| **2** | `FUN_00292930` (stores a2/a3 at +0x38/+0x3C, then `2`) | `FUN_002311b0`, `FUN_00285bc0` | **0 calls** |
| **4** | `FUN_00292a40` (same, then `4`) | `FUN_00285c90` | **0 calls** |
| 5 | `FUN_00292990` / `FUN_00292a60` (open the stream, size > 0) | handlers of state 3 / 4 | n/a - states never reached |
| 6 | `FUN_002855e0` (chunk read) | handler of state 5 | n/a |
| 7 | `FUN_002928f0` / `FUN_00292950` | handlers of state 1 / 2 | n/a |
| 0 | `FUN_00292610` (reset: state 0, close stream, clear model) | `FUN_002854a0` when the visibility test passes | **14 calls, on entries 0x4EA090 / 0x53DE20 only** |

**The kick chain, statically:**

```
engine child +0x0C (0x4E9DB0, vtable 0x3D4AC0) -> vt[+0x1C] = FUN_00230a50
   if (*(char*)(entity+0x14) == 1) {
       FUN_00230ad0(entity)                 <-- gate:
          if ((entity[0x18] || entity[0x1C]) && entity[0x160] == 1) {
             if ((entity[0x158]==2 || entity[0x158]==3) && FUN_00197668()==3 && entity[0x28]==1) {
                 FUN_00231be0(...); FUN_002311b0(entity); entity[0x28]=0;   <-- the only kick
```

```
scene vt[+0xFC] (0x3D51CC) = FUN_0029ce60 -> FUN_00285bc0 -> FUN_00292930  (state 2)
FUN_0027d0f0 -> FUN_001ed680 -> FUN_00285c90 -> FUN_00292a40              (state 4)
scene vt[+0x28] (0x2895f0) - no references at all, never dispatched
```

**Measured (90 s, `TOL_TRACE_SCENE`):**

```
[scenetest] entstep #1..#10  entity=0x004E9DB0  mode(+14)=1  RUNS
                            f158=0 f160=0 pending(+28)=0 src(+18)=0x00000000 src(+1C)=0x00000000
                            entry(+0C)=0x004EA090
[scenetest] vis #1..#500     obj=0x003933C8/...  status(+1)=0  => keep
counts: entstep=10  kick=0  sceneReq2=0  sceneReq4=0  sceneVt28=0
```

**So the answer to "why do the entries sit at 0": no load request is ever issued.**

- The entity step `FUN_00230a50` *does* run (10x) and its mode gate `entity[+0x14] == 1` *does* pass.
- But the one entity being stepped is **unconfigured**: `entity+0x18 = entity+0x1C = 0` (no data source) and
  `entity+0x160 = 0` (mode 0, where the kick needs 1, and the commit branch needs 2).
- `FUN_00230ad0` therefore fails its very first test and returns without touching anything.
- The scene's own requesters are never reached either: `FUN_00285bc0` (state 2) is only reachable through scene
  vtable **+0xFC**, and `FUN_00285c90` (state 4) only through `FUN_0027d0f0 -> FUN_001ed680`; both are 0 calls.
  The scene's `+0x28` slot (`0x2895f0`) has no references at all.

Neither the visibility/camera test (`FUN_001a1460`: `drawObj[+1] == 5`) nor the reset path is responsible - it
reads `0` ("keep") on all four draw objects and only ever fires on two other entries.

The four scene entries are therefore healthy but never handed work: each has a draw object (`+0x30`), a file
(`+0x34`) and a 0.5 scale (`+0x140`), and stays at state 0 forever.

### Next probe (exact)

Find who is supposed to write `entity+0x18` / `entity+0x1C` (the data source) and `entity+0x158` / `entity+0x160`
(mode/variant) on the stepped entity, and why that never happens: trace `FUN_0027d0f0` (the state-4 requester's
root) and the scene's `+0xFC` slot `FUN_0029ce60`, and check whether the scene definition that should configure
the entity is the same data that the 27 accepted TLPS chunks carry - i.e. whether the definition arrives and is
dropped, or never arrives at all.

### Session 152 - who should populate the entity fields, and what the "TLPS" accepts really are

**Two and only two triggers can start a model load, and both are dead:**

```
1) script bytecode opcode 0x18        FUN_0027d0f0 (the script VM)
     case 0x18:
        scene = FUN_00232190(FUN_001ed0c0())      <- the 232190 call we saw 100+ times
        if (FUN_00285b60(scene, id))              <- find entry by id
            FUN_001ed680(0, id) -> FUN_00285c90 -> FUN_00292a40   (entry state 4)

2) map streamer                      FUN_0029ce60 (scene vtable +0xFC = 0x3D51CC)
     FUN_00285840(scene, ...) then FUN_00285bc0(scene, id, handle, 1) -> FUN_00292930 (state 2)
```

**Measured: `scriptVM (FUN_0027d0f0) = 0`, `OPCODE18 (FUN_00285b60) = 0`, `MAP-STREAM (FUN_0029ce60) = 0`.**

`FUN_0027d0f0` has **zero static references** in the whole image. Searching the ELF for its pointer finds exactly one store,
at **vaddr `0x003D503C`**, inside a class-method registry whose entries are four pointers on a `0x20` stride:

```
0x3D4FB8: 0x00278490 0x00278510 0x00278590 0x002786F0
0x3D4FD8: 0x00278A80 0x00278BB0 0x00278C10 0x00278C20
0x3D4FF8: 0x002791F0 0x00279250 0x00279260 0x00279270
0x3D5018: 0x002792F0 0x00279370 0x002793F0 0x00279580
0x3D5038: 0x0027C5F0 0x0027D0F0 0x0027D0D0 0x00000000      <- the script VM is slot 1
0x3D5058: 0x0027C5A0 ...
```

So the script VM is a class's *step(dt, self)* method — the same shape as `FUN_0029ce60(float, int)` — reached only through
that registry, and no dispatcher ever reaches it. Its own first line is a delta gate (`if (param_1 < 1.0) return;`).

**The entity fields themselves** (`+0x18`/`+0x1C` source, `+0x158` variant, `+0x160` mode) are written by the load path
(`FUN_00231be0(...)` inside `FUN_00230ad0`) and committed by `FUN_002311b0`; `FUN_002311b0` also sets the pending byte
`entity+0x28 = 1` for `+0x160 == 1`. Since neither trigger runs, nothing ever writes them: measured `+0x18 = +0x1C = 0`,
`+0x158 = +0x160 = 0`.

**And the accepted "TLPS" chunks are not scene data — my earlier reading was wrong.** `FUN_00108f50` is a generic
string/param handler; its actual traffic is configuration and archive paths:

```
magic='thpr'  head=74687072693D3430                -> "thpr i=40"
magic='vag='  head=7661673D31303234006B65793D313032  -> "vag=1024", "key=102"
magic='MFS'   / magic='CDV'
magic='\AFS'  head=5C4146535C5359535F5245472E414653 -> "\AFS\SYS_REG.AFS"
magic='\AFS'  head=5C4146535C4649454C442E4146530A  -> "\AFS\FIELD.AFS"
```

So the engine reaches the point of *naming* `FIELD.AFS`, but **no scene-definition / geometry chunk is ever accepted**.
The genuine chunk accept in `FUN_002855e0` (entry state 5, magic `"TLPS"` = `0x53504C54`) can never run, because state 5
is only reachable from state 3/4, which the two dead triggers above are the only way to reach.

Conclusion for the black screen: the scene is fully built, pumped, and its four entries are healthy - but the
**game logic that asks for models (script VM opcode 0x18) and the map streamer are never dispatched**, so no entry is
ever loaded and the GS only ever receives clears.

### Next probe (exact)

Find who is supposed to dispatch the class registry at `0x3D5038` (i.e. instantiate that class and call slot 1 with a
delta) - and whether the map streamer `FUN_0029ce60`'s object (whatever holds vtable `0x3D50D0`'s `+0xFC`) is ever
created. Both are "who calls the thing that was never called" questions now, with exact addresses to instrument:
the registry entry `0x3D5038`, the instantiation sites that store it into an object, and the object that owns
`0x3D51CC`.

### Session 152 - the registry instrumented: neither class is ever constructed, and the field-load loop is never dispatched

**Corrected addresses for the registry at `0x3D5038`.** It is not a flat registry: `0x3D5030` is a **vtable** (two leading
nulls, destructor at `+0x08`, the same shape as the scene's `0x3D50D0`). So:

```
vtable 0x3D5030 = { +0x00: 0, +0x04: 0, +0x08: FUN_0027c5f0 (dtor), +0x0C: FUN_0027d0f0 (the script VM) }
ctor            = FUN_0027c3f0   (the only other site that builds 0x3D5030)
```

**Instantiation chain, all of it dead:**

```
FUN_00168810   field-load loop; ZERO static refs; lives at vtable slot 0x3D258C (base 0x3D2580, +0x0C)
  -> FUN_00168460 -> FUN_00166a10 -> FUN_0016cfe0 -> FUN_0027c3f0 (script-actor ctor, vptr = 0x3D5030)
  (FUN_0016cfe0 is also called from FUN_0020c610, itself only reachable from 0x208bfc)
```

Measured (90 s, `TOL_TRACE_SCENE`):

```
actorCtor (FUN_0027c3f0)        = 0     <- the script-actor class is NEVER constructed
actorCtorCaller (FUN_0016cfe0)  = 0     <- its only caller never runs
mapStep (FUN_0029cc60)          = 0
scriptVM (FUN_0027d0f0)         = 0
OPCODE18 (FUN_00285b60)         = 0
```

**Correction to the previous session's "scene vtable +0xFC".** `0x3D51CC` is *not* the scene's slot. Decoding the table
gives base **`0x3D51A0`** (two nulls, dtor at `+0x08`), so `0x3D51CC` is slot **`+0x2C`** of a *different* class:

```
vtable 0x3D51A0: +08=0x29CAF0  +0C=0x29CB80  +10=0x29CBC0  +14=0x29CC40  +18=0x29CC50
                 +1C=0x29CC60 (step)  +2C=0x29CE60 (the map/field streamer)
```

Confirmed independently: the ELF stores `0x0029CC60` at `0x3D51BC` = `0x3D51A0 + 0x1C`. The scene's own `+0x2C`
(and `+0xFC`) are **zero**; `0x3D50D0` only runs to about `+0x3C`.

So "whether any object ever gets the `+0xFC` slot populated" has a precise answer: the slot does not exist on the scene
at all, and the class that does own the streamer (`vtable 0x3D51A0`) is never created or stepped - `0x29CC60` = 0 and
`0x29CE60` = 0.

**Full dead chain for the black screen:**

```
scene built, pumped each tick, 4 entries healthy
  -> no model-load request: script VM class (vtable 0x3D5030) never constructed
  -> no map/field stream: class (vtable 0x3D51A0) never constructed or stepped
  -> the field-load loop FUN_00168810 that would drive both is itself never dispatched
     (it is vtable-only at 0x3D258C, exactly like the VM and the streamer)
  -> entries stay at state 0, GS receives clears only
```

**Port framing (not emulation):** every one of these is *guest* code that should run. The defect is not a missing
emulator feature but a **missing guest dispatch**: the objects carrying vtables `0x3D2580` (field-load loop),
`0x3D5030` (script actor) and `0x3D51A0` (map/field) are never created, so their methods never run. The port fix will
be either (a) find the guest task/app-queue step that should construct them - note the app job was previously measured
one-shot, busy set once and never re-armed, which fits a dropped task hand-off - or (b) a faithful port-side override
that performs the same construction at the same point in guest state, which is legitimate only once we know the guest
call site's contract.

### Next probe (exact)

Find the object that owns vtable `0x3D2580` (the field-load loop `FUN_00168810` at `+0x0C`): search the ELF for pointer
stores of `0x003D2580`, identify the constructor, and instrument it plus its callers. If that object is also never
created, walk one more level; if it *is* created, then the field-load loop is being skipped by a guard inside the
dispatcher and that guard is the fix site.

### Session 152 - ROOT CAUSE: the scene dispatcher (manager 0x3DB31C) is created once, then destroyed, and never recreated

The whole "nothing renders" symptom reduces to **one global that ends up null**: the object pointer at
**`0x3DB31C`** (gp-relative, `gp - 0x78D4`; the corpus shows it as `READ32(ADD32(GPR_U32(ctx,28), 4294936364))`).
It is the dispatcher that every subsystem beats through `(*mgr->vt[0x08])()` and `(*mgr->vt[0x0C])()`.

**Measured at runtime:**

```
[scenetest] clearManager by FUN_001ea800 at t=0ms      (global 0 -> 0, harmless)
[scenetest] mgrFactory #1 a0=0x00000000 a1=0x00000000
[scenetest] mgrCtor #1 this=0x0063E290
[scenetest] mgrFactory #1 -> global 0x3DB31C = 0x0063E290      <- created
[scenetest] clearManager by FUN_001ea8b0 at t=17657ms (global now 0x0063E290)   <- DESTROYED
[tick] #150 ... mgr=0x00000000                                  <- null for the rest of the run
```

**Why:** `FUN_002055b0` is the game's scene-execution driver, and its own body brackets the two events:

```c
FUN_002055b0(scene) {
    FUN_001ea850(0, uStack_c);          // create the manager (the single mgrFactory call)
    ... main loop: (*mgr->vt[8])(); FUN_00204ed0(...); FUN_00169f90(); (*mgr->vt[0xC])(); yield ...
    FUN_001ea8b0();                     // destroy the manager at completion (t = 17.657 s)
}
```

`FUN_002055b0` has zero static references; its pointer is stored at **`0x3D479C` = `0x3D4750 + 0x4C`**, i.e. slot
`+0x4C` of the **loader object's vtable** (`0x63D890`, the same object whose `vt[0x0C]` is the fade `0x204240`).

So the loader drives the scene once (0 - 17.6 s), tears the manager down, and **is never invoked again**. From that
moment `0x3DB31C` is null, so:

- the app loop's `(*mgr->vt[8])()` / `(*mgr->vt[0xC])()` have nothing to beat,
- every `vt+0x0C` method goes silent - the script VM (`0x27d0f0`), the field-load loop (`0x168810`), the field init
  (`0x169370`), measured 0 calls each (this is the "+0x1C dispatched, +0x0C never dispatched" pattern),
- no model-load request is ever issued, entries stay at state 0, and the GS receives clears only.

That also explains the earlier **24 s window with no engine ticks**: it brackets this teardown at 17.66 s.

**Walked chain, for the record (each level vtable-only, each measured 0):**

```
FUN_00168810 (field-load loop)        <- vtable 0x3D2580 +0x0C   (derived)
FUN_00169370 (field init/registration)<- vtable 0x3D25E0 +0x0C   (base)
FUN_0027C3F0 (script-actor ctor)      <- vtable 0x3D5030, ctor called from FUN_0016cfe0 <- {FUN_00166a10, FUN_0020c610}
FUN_0029CE60 (map/field streamer)     <- vtable 0x3D51A0 +0x2C   (NOT the scene's +0xFC; the scene vtable ends ~+0x3C)
all of them depend on mgr = 0x3DB31C
```

**Port framing (not emulation).** This is guest behaviour, not a missing emulator feature: the guest object that owns
`vt+0x4C` (`0x3D4750`, the loader at `0x63D890`) still exists after the fade and the scene start; the port simply never
re-enters its scene driver. The fix is to restore the guest call sequence - re-invoke the loader's `+0x4C` scene driver
after the scene start, exactly as the guest would - not to emulate anything new and not to fake a renderer.

### Next probe (exact)

Probe `0x002055b0` (call count and return time) plus every dispatch of the loader's `+0x4C`, and identify the guest state
machine step that should re-enter it after `FUN_00285E80`'s scene start: the loader object `0x63D890` keeps vtable
`0x3D4750`, so read its state fields after the scene start and find the guard/branch that ends the sequence instead of
looping back to `+0x4C`. That branch is the fix site.

### Session 152 - CORRECTION: the driver is FUN_00366090 (not FUN_002055b0); vt+0x0C is dispatched only 4 times total

**My previous attribution was wrong.** `FUN_002055b0` (stored at `0x3D479C`) **never runs** - probed directly, zero entries.
The real scene driver, found by capturing `$ra` at the manager factory's call site, is:

```
[scenetest] site 366090 #1 ra=0x00165230 a0=0x0063D890 mgr=0x00000000
```

- **`FUN_00366090`** is the driver: it calls both the manager **create** (`FUN_001ea850`) and the **destroy**
  (`FUN_001ea8b0`), and it appears in both caller lists.
- **`a0 = 0x0063D890` is the loader object** (vtable `0x3D4750`) - so the loader is what it drives.
- `ra = 0x165230` -> the `jal` at `0x165228`, which is `jal 0x165400`; this code uses **tail jumps** (`j`, see
  `0x165210`), which preserve `ra`, so the driver was entered through a tail jump from the loader step.

**The dispatch chain, decompiled:**

```c
FUN_00165400(obj)   { (*(obj + 0x10))->vt[0x0C](); }    // generic "call vt+0x0C" dispatcher
FUN_00165220(obj)   { FUN_00165400(*(obj)); FUN_0015231c(); return 1; }   // step one object + yield
FUN_00165150(p, m)  { node = FUN_00164e40(...); *node = p; link node into list at piGpffff8608; }  // list append
```

So the loader's **`vt+0x0C` = `0x204240` = the fade**, and the whole scene phase is driven by repeatedly stepping objects
through `FUN_00165220` -> `FUN_00165400` -> `(*obj[0x10])->vt[0x0C]()`.

**Measured: that dispatcher runs exactly 4 times in the whole session, the last at t = 10.156 s.**

```
[scenetest] vt0C #1 t=0ms      obj=0x0063D850 vptr(+10)=0x003D4770
[scenetest] vt0C #2 t=0ms      obj=0x0063D890 vptr(+10)=0x003DA380
[scenetest] vt0C #3 t=1390ms   obj=0x0063D890 vptr(+10)=0x003D4750
[scenetest] vt0C #4 t=10156ms  obj=0x0063D890 vptr(+10)=0x003D4750
total = 4
```

`vptr(+10)=0x3DA380 -> 0x3D4750` is exactly the loader's recorded vtable switch, and `0x3D4750 + 0x0C = 0x204240` (the
fade). After t = 10.156 s **nothing dispatches `vt+0x0C` again**, so the loader is never stepped after the scene start
(which happens at ~36 s), the scene driver is never re-entered, the manager stays null from ~17.6-28.6 s onward, and no
script/map/field code (all `vt+0x0C` methods) ever runs.

**The guard is therefore not a branch inside the driver** - it is that the *object-step loop* (the code that calls
`FUN_00165220` on each list node) stops after 4 iterations. That loop is the fix site.

### Next probe (exact)

Instrument `FUN_00165220` (`0x165220`) - the "step one object + yield" wrapper - logging `*(a0)` and `ra` at every call,
and instrument the list head `piGpffff8608` (`gp - 0x79F8` = `0x3DB1F8`... verify: `0xFFFF8608` signed = `-0x79F8`, so
`gp - 0x79F8`). Then the loop that calls `FUN_00165220` will be identified by its `ra`, and the guard that ends it
(fewer than 4 objects in the list, or an early exit) will be visible in one run. That loop is where the port fix belongs:
the loader object still exists with vtable `0x3D4750`, so it must be kept in that step list after the scene start -
exactly what the guest does, with no emulation involved.

### Session 152 - VERIFIED MODEL: the loader is stepped once per thread activation, 4 activations in 60 s

Method correction first (three earlier claims were wrong: "never pumped", "27 TLPS chunks", "FUN_002055b0 is the
driver"). The fix applied here: every probe logs install status, call site (`ra`), receiver identity, and the live
globals; and no candidate is described as the driver until it has actually been probed.

**Verified dispatch trace (target now computed from `vptr`, not `*(vptr)`):**

```
VT0C #1 t=0ms      obj=0x63D850 vptr(+10)=0x3D4770 target(+0C)=0x204690
VT0C #2 t=0ms      obj=0x63D890 vptr(+10)=0x3DA380 target(+0C)=0x366090   <- scene driver (pre-fade table)
VT0C #3 t=1359ms   obj=0x63D890 vptr(+10)=0x3D4750 target(+0C)=0x204240   <- the fade   (post-fade table)
VT0C #4 t=10218ms  obj=0x63D890 vptr(+10)=0x3D4750 target(+0C)=0x204240   <- the fade
tasks=0x497050 (constant)   mgr=0x00000000 at all four
```

This settles the contradiction from the previous entry: `0x366090` really is `vt+0x0C` of `0x3DA380`, and the fade
really is `vt+0x0C` of `0x3D4750`. It also corrects an inference: `mgr = 0` at **all four** dispatches including
t = 10.2 s, so the manager is created *later* than the driver's entry.

**And the dispatcher's caller is the scheduler, not an object loop:**

```
STEPWRAP #1 t=0ms      a0=0x00407740 *a0=0x63D850  ra=0x00151FC8
STEPWRAP #2 t=0ms      a0=0x00407B60 *a0=0x63D890  ra=0x00151FC8
STEPWRAP #3 t=1391ms   a0=0x00407B60 *a0=0x63D890  ra=0x00151FC8
STEPWRAP #4 t=10204ms  a0=0x00407B60 *a0=0x63D890  ra=0x00151FC8
```

`ra=0x151FC8` is inside **`FUN_00151e90`** (the thread dispatcher), which invokes
`(*callback)(TCB + 0x1C0)` **once per activation**, latched by the byte at `TCB+499`:

```c
if (*(char *)(iVar3 + 0x1F0) == 1 && *(byte *)(iVar3 + 0x1F1) == channel) {
    if (*(char *)(iVar3 + 499) == 0) { *(char*)(iVar3 + 499) = 1; (*cb)(iVar3 + 0x1C0); }
    FUN_0015243c(); FUN_00152200(); FUN_0015253c();
}
```

`a0 = 0x407B60 = TCB + 0x1C0` → **TCB = 0x4079A0, the loader TCB** already recorded. `*(TCB+0x1C0) = 0x63D890`.

**So the loader object is stepped exactly once per activation of its thread, and that thread was activated 4 times in
60 seconds.** That is the guard - a *scheduling* fact, not a branch inside any scene, fade or driver code. Everything
downstream (script VM, field load, field init, map stream, model load) is silent because the object that would drive it
is stepped 4 times and then not at all.

**What is certain vs still inferred:**

| certain | still inferred |
|---|---|
| loader stepped 4x via `FUN_00151e90` -> `(*cb)(TCB+0x1C0)` -> `vt+0x0C` | *why* the loader thread is activated only 4x |
| `vt+0x0C` = `0x366090` (pre-fade) then `0x204240` (post-fade) | whether the guest expects more activations or the port drops wake-ups |
| manager created once, destroyed once, null thereafter | which subsystem should recreate it |
| task list head `0x3DB1F8` constant at `0x497050` | whether that is the correct task for this phase |

### Next probe (exact)

Instrument the loader TCB `0x4079A0`: log `+0x1F0` (state), `+0x1F1` (wait channel), `+499` (activate latch),
`+0x1AC` (alive), `+0x200` and `+0x1B0` on every `FUN_00151e90` iteration that touches it, plus a timestamp. That
answers "why only 4 activations" directly: either the thread sits in a wait state whose channel is never signalled
(port-side wake-up gap), or it is descheduled by the priority/selector globals `0x3DB1A0/B0/C0`. **That is the fix site
and it is a scheduler question, so the resolution belongs in the runtime's scheduler, not in an override that fakes
scene behaviour.**

### Session 152 - ANSWER: the loader activates 4x because its thread is only *ready* 4x, not because dispatch is rare

Probed the scheduler path in the runtime (PCSX2 reads are unusable - `0x63D890` returned all-zero, same debug-server
failure as the GS registers, so all measurement is done in the port).

**(a) Which channels are actually served.** `FUN_00151ff0(id)` forwards to the dispatcher only when the enable table at
`0x48F570` allows it:

```
[enable] FIRST ask id=0 -> enabled=3
[enable] FIRST ask id=1 -> enabled=0
[enable] FIRST ask id=4 -> enabled=1     <- the loader's channel: ENABLED
[enable] FIRST ask id=7 -> enabled=0     <- frame loop asks 7: DISABLED
[enable] FIRST ask id=9 -> enabled=0     <- DISABLED
[enable] FIRST ask id=8 -> enabled=0     <- DISABLED
[enable] FIRST ask id=6 -> enabled=0     <- DISABLED
```

**(b) Who requests what, and from where.**

```
[req] FIRST id=0 enabled=3  ra=0x166070  t=0ms
[req] FIRST id=1 enabled=0  ra=0x166078  t=32ms
[req] FIRST id=4 enabled=1  ra=0x165BA8  t=27578ms
[req] FIRST id=7 enabled=0  ra=0x164FF0  t=27766ms   } the 4 dispatches inside
[req] FIRST id=9 enabled=0  ra=0x164FF8  t=27766ms   } FUN_00164f80 are all
[req] FIRST id=8 enabled=0  ra=0x165000  t=27766ms   } no-ops forever
[req] FIRST id=6 enabled=0  ra=0x16500C  t=27766ms   }
```

**(c) The real main loop** is `FUN_00165ad0`, and its inner loop dispatches exactly the enabled channels:

```c
do {
    FUN_00152b70(); FUN_00151ff0(0); FUN_00151ff0(1); FUN_00151ff0(4);
    FUN_001516b0(); FUN_001f96c0(); FUN_0019a080(); FUN_00153780();
    FUN_00164d40(); FUN_00139a20();
    lVar2 = FUN_00153180();
} while (lVar2 == 0);
```

```
[mainloop] iter #2 t=62ms dt=62ms  mgr=0x0063E290
[mainloop] iter #3 t=187ms dt=125ms
[mainloop] iter #4..#12  dt≈47-125 ms   pred increments 1:1
[mainloop] exitPred #1..#12  ret=0 -> "continue loop"  (never exits)
```

So the main loop iterates at **~110 ms (~9 Hz)** and calls `dispatch(4)` **every iteration** - about 540 times in 60 s.

**Therefore the activation count is NOT a dispatch-rate problem.** The loader activates 4 times because on the other
~536 dispatches its TCB is not in the ready state (`+0x1F0 != 1`) and `FUN_00151e90` skips it:

```
[tcb] t=27593ms state=1 chan=4   -> activated
[tcb] t=28750ms state=0 chan=10  -> skipped
[tcb] t=28797ms state=1 chan=4   -> activated
[tcb] t=37468ms state=0 chan=10  -> skipped
[tcb] t=37531ms state=1 chan=4   -> activated
```

`cb(+0x1B0) = 0x165210` and `alive(+0x1AC) = 1` throughout, so the TCB is correctly wired; only its **state** gates it.

### Open discrepancy (not explained - do not build on it)

`mgr` at `0x3DB31C` reads `0x63E290` from mainloop iteration #2 (t = 62 ms), but reads `0` at `VT0C #3` (t = 1359 ms) and
`VT0C #4` (t = 10218 ms). The only three functions that write that global (`FUN_001ea800`, `FUN_001ea810`,
`FUN_001ea8b0`) were all probed and fired only at t=0 and t≈17-28 s. Something zeroes it in between, or one of the
reads is not measuring what it appears to. This must be resolved before using the manager's lifetime as evidence.

### Next probe (exact)

Find who sets the loader TCB's state byte `0x4079A0+0x1F0` to `1` (ready) and to `0` - that is the wake-up path and the
actual gate. Instrument `FUN_00151710` and the list insert/remove helpers (`FUN_001517c0`, `FUN_00151cf0`,
`FUN_0015243c`) for the loader TCB specifically, and log every write with `ra`. The port fix belongs there (a missing
or wrong wake-up for that thread), not in scene code and not in an override that fabricates rendering.

### Session 152 - TCB state machine traced: the gate is `alive(+0x1AC)`, not readiness starvation

Every write site for the state byte `+0x1F0` in the whole guest (offset 496, from the recompiled corpus):

| writer | role |
|---|---|
| `FUN_001517e0` | scheduler init - zeroes all TCBs and the channel counter table |
| `FUN_00151cf0` | **thread exit**: state=3, park chan=10, decrement `0x48F570[chan]`, call `+0x1B8` |
| `FUN_00151e90` | reap: state 3 -> remove from list -> state 0 |
| `FUN_0035ddc0` | unrelated object (vtable `0x3D9FA0`), different struct |

**Correction:** `0x48F570` is a **waiter reference count**, not an enable table (`FUN_00151cf0` decrements it on exit). So
channels 6/7/8/9 reading 0 simply means nobody waits on them - normal, not a bug. That retires the previous entry's
"disabled channels" framing.

**Measured state machine (tags are the bracket that observed each change):**

```
t=0      tick             state=0 chan=0  alive=0
t=17047  516b0.pre        state=1 chan=4  alive=1 resume=0x3661DC   ready - driver stage (FUN_00366090)
t=17891  51cf0.pre        state=1 chan=4  alive=0                   stage finished: alive cleared
t=17891  516b0.pre        state=3 chan=10                           terminated by FUN_00151cf0
t=17922  151e90[4]        state=0 chan=10                           reaped inside dispatch(4)
t=17937  516b0.pre        state=1 chan=4  alive=1 resume=0x204284   RE-ARMED - fade continuation
t=24109  51cf0.pre        state=1 chan=4  alive=0                   second stage finished
t=24125  151e90[4]        state=0 chan=10                           reaped
t=24141  516b0.pre        state=1 chan=4  alive=1 resume=0x204284   RE-ARMED again
```

`ra=0x00151FA4` for every `FUN_00151cf0` call is the dispatcher's own line:

```c
if (*(int *)(iVar3 + 0x1AC) == 0) { FUN_00151cf0(*(undefined4 *)(iVar3 + 0x200)); }
```

**So the answer to "why is it not made ready":** it *is* made ready - 4 times - and each time it runs a stage until the
**`alive` byte at `+0x1AC` is cleared**, at which point the dispatcher terminates it (state 3, channel 10, waiter count
decremented) and the next pass reaps it to state 0. Re-arming sets `state=1, chan=4, alive=1` **and a new resume PC**,
so each arm resumes guest code at a yield point (`0x3661DC` driver -> `0x204284` fade -> `0x204284` fade).

It stops after the 4th because **`alive` is left at 0 and nothing arms it a fifth time**. Dispatch is not the
constraint: the main loop `FUN_00165ad0` calls `dispatch(4)` every iteration (~540x per 60 s in the measured 110 ms
loop).

**Open point (proven, not guessed):** the arming writes (`state=1, chan=4, alive=1` plus the waiter-count increment)
exist in neither the guest writers found above nor the runtime (`EeScheduler.cpp` only *reads* `+0x1AC/+0x1F0/+0x1F1/
+0x1F3`). Greps for `+0x1F0`/`+0x1F1`/`0x48F570` find no arming site, and the arming lands in a 15 ms window during
which **another EE thread is running** (the dispatching thread's post-bracket shows no change), so the write comes from
a *different thread* through an address form the greps did not match.

### Next probe (exact)

Identify the arming thread and site empirically, not statically: bracket the remaining main-loop calls in order
(`FUN_00152b70`, `dispatch(0)`, `dispatch(1)`, `dispatch(4)`, `FUN_001516b0`, `FUN_001f96c0`, `FUN_0019a080`,
`FUN_00153780`, `FUN_00164d40`, `FUN_00139a20`) with `logLoaderTcb`, and record the **current thread id** in each tag
(from the scheduler's current-TCB global). The first tag whose name is not the dispatching thread names the arming
thread; bracketing that thread's own calls then lands the exact write. Fix site: whatever should arm the loader a 5th
time (the guest's continuation after the fade) - port-side, in the scheduler/guest-continuation path, with no emulation
and no fabricated rendering.

### Session 152 - arming found empirically; three hypotheses refuted by measurement

**Correction to the plan:** the arming thread is not a separate thread. `cur` (the scheduler's current-TCB, read from
`0x48F750 + depth*0x1D0` with `depth` at `0x3DB1A0`) shows `cur=0x004079A0` (the loader) only inside its own exit path;
every arming happens at `depth=0, cur=0`, i.e. **on the dispatching side**, before `FUN_001516b0` is entered.

**The arming writes are in the context save/restore primitives**, bracketed directly:

```
15253c.post t=29672  latch=0 alive=0 resume=0x165220 | cur=0x00407580
15243c.save t=29672  latch=1 alive=0 resume=0x165220 | cur=0x004079A0   <- FUN_0015243c sets latch(+0x1F3)=1
15253c.post t=29687  latch=1 alive=1 resume=0x3661DC | cur=0x004079A0   <- FUN_0015253c sets alive(+0x1AC)=1 + resume(+0x1B4)
```

So: **arm = context restore; step = the callback in `FUN_00151e90`, fired while `latch == 0`** (one step per restore).

**Three hypotheses were tested and REFUTED by measurement:**

| hypothesis | measurement | verdict |
|---|---|---|
| waiter count at `0x48F578` (channel 4) fell to 0, so `FUN_00151ff0(4)` short-circuits | `wait4 = 3` throughout (dips to 2 on exit, returns to 3) | **refuted** |
| the reap (`FUN_00151710`) unlinked the TCB so the walk never reaches it | final state `prev=0x407580 next=0 head=0x407160 tail=0x4079A0` | **refuted** |
| half-link: predecessor's `next` doesn't point back | `predNext=0x004079A0`, `pred=0x407580`, `link-ok` | **refuted** |

**The state at the end is fully eligible:** `state=1, chan=4, alive=1, latch=1, resume=0x204284`, linked as the tail, with
the predecessor pointing back and the channel count non-zero.

**What actually gates it - the dispatch loop collapses.** `FUN_00165ad0` (the loop that dispatches channels 0/1/4):

```
iter #40  t=2265ms  dt=31ms   ~32 Hz
iter #80  t=3843ms  dt=31ms
iter #120 t=5437ms  dt=31ms
iter #160 t=7015ms  dt=47ms
(<20 further iterations in the remaining 53 s -> ~0.36 Hz)
```

So after ~7 s the loader is offered a few dozen dispatch opportunities in a minute instead of ~1900, and it is stepped
3-4 times. `FUN_00153180` (the loop predicate) always returns 0, so the loop never exits - it just slows by ~90x.

### Next probe (exact)

Instrument the per-call cost inside `FUN_00165ad0`'s inner loop (timestamps around `FUN_00152b70`, the three
dispatches, `FUN_001516b0`, `FUN_001f96c0`, `FUN_0019a080`, `FUN_00153780`, `FUN_00164d40`, `FUN_00139a20`,
`FUN_00153180`) and print the slowest call per iteration. The loop cost grows ~90x in the first 7 s; the single call
whose cost grows is the fix site. This is a *specific measured regression*, not general slowness - and it is what
starves the loader, not any scene, scheduler or TCB logic.

### Session 152 - per-call costing of FUN_00165ad0: NO call slows; the time is outside the loop body

Every call in the inner loop was bracketed with `QueryPerformanceCounter`, accumulating per iteration
(`iterUs` = 52b70 entry -> 153180 entry; `sum` = total across all brackets; `max` = slowest single call):

```
[mainloop] exitPred #70  iterUs=56099  sum=9516  max=8948us@139a20
[mainloop] exitPred #110 iterUs=55931  sum=10923 max=10335us@139a20
[mainloop] exitPred #160 iterUs=51089  sum=8475  max=7952us@139a20
[mainloop] exitPred #164 iterUs=755890 sum=11093 max=10286us@139a20
[mainloop] exitPred #170 iterUs=85873  sum=10071 max=9511us@139a20
```

Bracketed sites (all verified present in the function table, so all installed): `0x152b70`, `0x151ff0`
(all three channel dispatches), `0x1516b0`, `0x1f96c0`, `0x19a080`, `0x153780`, `0x164d40`, `0x139a20`, and the
predicate `0x153180` cost.

**Findings:**

- The whole loop body costs **~9.5-11 ms per iteration and does not grow** across 60 s.
- `FUN_00139a20` is the single largest call at **~9-10 ms, constant** - it is not the regression.
- A normal iteration measures **~56 ms**, so **~46 ms (83%) is outside every bracketed call**.
- The 756 ms spike accounts only 11 ms in brackets, so **~745 ms (98%) is outside the loop body**.

Because the loop's calls are adjacent `jal`s with no guest code between them, that unaccounted wall-clock is time when
this thread is not executing its own body: **interrupt handlers (VBlank etc.) and other guest threads**.

**So the earlier framing is corrected again:** the loader is starved because the *wall-clock* iteration rate of
`FUN_00165ad0` degrades ~90x, but the degradation is **not** in any call the loop makes. The loop's own work is
constant and cheap (~10 ms); what grows is the time stolen from it.

### Next probe (exact)

Attribute the unaccounted wall-clock. Reuse the existing IRQ instrumentation (`TOL_TRACE_EE_IRQ`) and add:
(1) per-IRQ-handler entry/exit timing with the cause (2 = VBlank start, 3 = VBlank end, 9+T = timer),
(2) a per-thread CPU-time total keyed by current TCB, printed every ~2 s, and
(3) the count and total cost of VBlank handler runs per second.
The one whose share rises while `FUN_00165ad0`'s iteration time falls is the thief. Only after that is known can the
loader's starvation be fixed at its actual cause - and it is still a port-side scheduler/IRQ question, not scene code.

### Session 152 - RETRACTION: the "3.6x VBlank deficit" was my arithmetic error; pacer and IRQ path are correct

Added a permanent diagnostic, `TOL_PACING_RATIO` in `EeScheduler.cpp`: it counts VBlank pacing events
(`traceVBlankPacing`, called once per VBlank) against `dispatchIrq` calls (and per-cause start/end/timer) and prints
the rates once per second.

```
[ratio] vblankTicks/s=1  raises/s=3   vblankStart/s=1  vblankEnd/s=1  timer/s=0    (boot, ~22 s)
[ratio] vblankTicks/s=0  raises/s=24  ...                                        (boot)
[ratio] vblankTicks/s=60 raises/s=130 vblankStart/s=60 vblankEnd/s=60 timer/s=8    (steady state)
[ratio] vblankTicks/s=59 raises/s=127 vblankStart/s=59 vblankEnd/s=59 timer/s=7
[ratio] vblankTicks/s=60 raises/s=128 vblankStart/s=60 vblankEnd/s=60 timer/s=8
```

**In steady state VBlank pacing and VBlank IRQ raises are both exactly 60/s.** There is no deficit.

**Retraction:** the previous entry's "650 raises vs ~2340 expected = 3.6x deficit" was wrong. `c2` in the `[eeirq]`
trace is a **cumulative** counter (`sByCause[]` is never reset), and it was accumulated across the ~22 s boot phase where
VBlank legitimately runs at 1-5/s. Dividing that boot-contaminated total by the full run time produced a fictitious
deficit. The frame-rate path in the port is correct and is **not** the cause of anything.

**Also retired:** the "83-98% unaccounted time" from the per-call costing was a methodology artifact - `g_iterStartQpc`,
`g_iterSumUs` and `g_maxUs` were single globals in a cooperative multi-threaded guest (another thread's entry overwrites
the iteration start), and the figure compared **wall-clock latency** against **CPU cost inside `jal`s**, which need not
be equal. The trustworthy instrument is the project's own `TOL_TURN_COST`.

**What `TOL_TURN_COST` actually says** (steady state, same run):

```
presents=18400 guest=29177.4ms(66.4%) paceWait=14423.2ms(32.8%) presenter=318.1ms(0.7%)
guest 29170.2 -> 29177.4 ms across 600 presents: FLAT
```

Guest CPU total is flat while `paceWait` grows, and `guestEntries` advance only ~58 per 600 presents at ~0.04 ms each.
So the guest is **idle by its own logic** - sitting in a wait, not being starved and not running slowly.

**Where that leaves the investigation:** the renderer, pacer, VBlank/IRQ path, scheduler fairness, GS backend and
presenter are all measured correct. The game runs, receives 60 VBlank/s, and waits. The only established defect remains
the one from earlier entries: the model/scene load is never requested (the loader thread is stepped once per activation,
clears `alive(+0x1AC)`, and is never re-armed), so the scene streams no geometry and the GS receives clears only.

### Next probe (exact)

Follow the guest's wait: identify what the main loop actually blocks on during its ~24 ms idle per present - read the
waiting thread's `+0x1F1` channel and `+0x200` payload at the moment `paceWait` is entered, and map that channel to its
signaller. If it waits on VBlank (correct) then the guest is simply spending its frame budget doing nothing, and the
defect is upstream in guest logic; if it waits on something that never arrives, that signaller is the fix site.

### Session 152 - the wait mapped: the signaller is VBlankStart (correct), and the guest has TWO regimes

Instrumented both scheduler wait points (`TOL_WAIT_MAP`):

1. the event-processing loop's `pacingWaitUntil` (line ~3251), which names the earliest due deadline's event type;
2. **`EeScheduler::waitForEvent()`** - the actual per-frame wait - which blocks on the earliest entry of
   `m_deadlines`, or on an Alarm timer if that is sooner.

**The signaller is `EeEventType::VBlankStart`**, rescheduled every `kVBlankPeriodCycles` (lines 304-306 / 3330-3332).
VBlank arrives at **60/s** (measured with `TOL_PACING_RATIO`: `vblankStart/s=60`). So per the decision rule: **the guest
waits on VBlank, which arrives correctly - the defect is upstream in guest logic, not a never-arriving signaller.**

**But the run-to-run behaviour is not one steady state.** Two back-to-back runs of the same binary:

| run | paceWaits | guest | paceWait | reading |
|---|---|---|---|---|
| A (`TOL_PACING_RATIO`+`TOL_TRACE_SCENE`) | 1871 | 29177 ms (66.4%) | 14423 ms (32.8%) | guest finishes early and **sleeps** on VBlank |
| B (`TOL_WAIT_MAP`) | **0** | 37544 ms (**98.9%**) | **0 ms (0.0%)** | guest never waits - it **saturates CPU** |

`waitForEvent()` returned immediately in run B (events always pending), so no wait existed to map and the `[waitmap]`
line correctly printed nothing.

**Consequence:** the "the guest is idle" conclusion from the previous entry is only true in regime A. The guest
alternates between idling on VBlank and saturating CPU, so **a single run's turn-cost split does not describe a steady
state** and must not be used as the basis for a fix. Any claim of the form "X% of time is spent in Y" needs the regime
stated, or needs both regimes measured in one run.

### Status

Measured correct: renderer, GS backend, presenter, pacer cadence (60/s), VBlank raise path (60/s start+end), scheduler
fairness, TCB arming/linking/waiter-count, fade, scene start, and the frame-loop's own per-call cost (~10 ms constant).
Not correct: **no model/scene load is ever requested** (loader stepped once per activation, `alive` cleared, never
re-armed; `FUN_002311b0` never called; no `TLPS` geometry chunk accepted).

### Next probe (exact)

Distinguish the two regimes in one run: log the turn-cost split **together with** a marker of which regime the guest is in
(e.g. the loader TCB's `state(+0x1F0)` and the main loop's iteration counter at each sample). Then report only samples
from one regime. That removes the run-to-run instability that has twice led this investigation to a wrong conclusion.

### Session 152 - unified regime line built; single-regime result: the guest is CPU-bound spinning, loader is READY

Built the requested instrument. `TOL_TURN_COST` now prints, on one line: the cumulative split, the **per-window DELTA**
(so a phase change cannot hide inside cumulative numbers), and the **regime markers** - loader TCB `state/chan/alive`
plus `mainIters` and its increment. The markers are cross-TU globals (`tol_state::` in tol_overrides.cpp, declared
`extern` in ps2_runtime.cpp) and are updated unconditionally, so the line is never blank.

**Single-regime result (4 consecutive samples, identical):**

```
[turncost] presents=19000 guest=37679.4ms(98.6%) paceWait=0.0ms(0.0%) presenter=525.7ms(1.4%)
          | DELTA guest=99.8% pace=0.0% pres=0.2%
          | loader state=1 chan=4 alive=1   mainIters=48(+7)
```

- **guest 99.6-99.8 % of every window** - the guest is CPU-bound, not idle (this is regime B).
- **paceWaits = 0** - the scheduler never waits for VBlank, so no host wait exists to blame.
- **`loader state=1 chan=4 alive=1` throughout** - the loader TCB is *ready and alive*. It is **not** being starved by
  the scheduler, the ready list, the waiter count, or its own arming. All earlier hypotheses about the TCB are dead.
- **`mainIters` +7 per 200 presents (~17.5 iterations/s)** - the frame loop barely advances.

**Why: the guest spins.** Added `TOL_GUEST_HOT`, which samples the guest PC at every `enterGuest`:

```
[guesthot] distinct=41  top: 0x0015FC10=264  0x0015FD80=262  0x0015FB7C=231  0x0015FCF0=230  0x001A35E4=102  0x001A36A0=102
```

Per 2000-entry window those four `0x15FB7C..0x15FD80` PCs are **~49 % of all guest entries**, and `distinct=41` means
the guest is confined to a tiny code region. They are interior labels of **`FUN_0015fb60`**, which installs its own
interior code labels into `param_1+0x1240..0x124C` via `PTR_LAB_00389210/40/70/80` - i.e. a self-referential dispatch
table, and the engine calls those labels repeatedly. Earlier windows showed the same shape in
`0x349C9C/0x349D48/0x349D68/0x349DD8` (~55 %), inside `FUN_00349c00`, the queue walker/poller already recorded.

**Conclusion (single regime, consistent across all samples):** the runtime is not the bottleneck and the loader is not
being starved. A **guest spin consumes ~99 % of the CPU**, which throttles `dispatch(4)` to ~17.5/s, so a loader that is
ready and alive gets stepped only a handful of times. This is guest logic - consistent with the session's earliest
measurement that the work queues are empty (233 queues, 8 matches ever).

### Next probe (exact)

Identify what drives the `FUN_0015fb60` label table continuously and what those label bodies poll: decompile the
`PTR_LAB_00389210/40/70/80` targets (they are interior labels of `FUN_0015fb60`), and instrument the caller that invokes
them at ~2000/s. Then find the producer that should fill the queue they poll - that producer is the fix site, and it is
guest logic, not runtime, not rendering.

### Session 152 - RETRACTION: the "guest spin" was a misreading of what guestEntries counts

Decoded the four tables (they install labels in `0x15C7E0-0x15CF40`):

| table | entries |
|---|---|
| `0x389210` | `15C7E0 15C7E0 15C7E0 15C7F0 15C830 15C8C0` (x2 modes) |
| `0x389240` | `15C7E0 x6 15C9B0 15C9F0 15CA80 15C9B0 15C9F0 15CA80` |
| `0x389270` | `15CB70 15CBB0 15CC00 15CC50` |
| `0x389280` | `15CCB0 15CD70 15CE70 15CF40` |

**The hot PCs are not polls.** Disassembly of the hot address shows it is a loop back-edge:

```
0x15FC10  addiu a2, a2, 0x1
0x15FC14  slti  a3, a2, 0x8
0x15FC18  bne   a3, zero, 0x0015FB7C      <- branch back to the hot PC
```

`0x15FB7C` is the top of `FUN_0015fb60`'s own `do { switch (uVar1) ... } while (uVar1 < 8)` loop.

**And `enterGuest` samples every dispatched guest branch, not every function call** - backward branches go through
`dispatchGuestBranch`. So `guestEntries` counts *loop iterations*, and:

```
2000 entries/s  ~=  8 iterations x ~4 calls/frame x 60 Hz  =  1920/s
```

which is exactly what a normal 60 Hz game looks like. `FUN_0015fb60` has **16 call sites**
(`FUN_00153780`, `FUN_001ebb60`, `FUN_0015d8f0`, `FUN_001c9410` x4, `FUN_0023bf10`, `FUN_002dd560`, `FUN_0032e7a0`,
`FUN_00330e20`, ...) and **one of them, `FUN_00153780`, is called once per `FUN_00165ad0` inner-loop iteration.**

**Retraction:** the previous entry's conclusion - "a guest spin consumes ~99% CPU and starves the frame loop" - is
withdrawn. It was based on treating branch-entries as function entries. `guest=99%` in the turn-cost line means only
"the CPU is executing guest code", which is expected for a recompiled game. Nothing here is a poll loop.

**What is actually still unexplained:**

1. `mainIters` advances only ~17.5/s (measured `+7` per 200 presents, four consecutive identical samples), where a
   60 Hz frame loop should be ~60/s. Ratio vs VBlank: **60 / 17.5 = 3.43**.
2. The two regimes remain unreconciled: run A `paceWaits=1871` (guest sleeps on VBlank), run B `paceWaits=0`
   (never sleeps). Same binary, back-to-back.
3. The established functional defect is unchanged: no model/scene load is ever requested.

### Next probe (exact)

Measure **one `FUN_00165ad0` iteration's wait composition**, not the aggregate: for each iteration, record how many
VBlank boundaries elapse inside it (VBlank counter delta between `52b70` entry and `153180` entry). The ratio 60/17.5 =
3.43 predicts **~3.4 VBlanks consumed per iteration**; if that holds, the loop is deliberately waiting on 3-4 VBlanks
per pass and the question becomes "which call inside the iteration waits on multiple VBlanks" - traceable by bracketing
the VBlank counter at each call. If instead iterations are spread evenly over single VBlanks, the loop is being sampled
rarely and `mainIters` is under-counted. This distinguishes a real wait from a measurement artifact before any further
conclusion is drawn.

### Session 152 - VBlank deltas per iteration: the 3.43 hypothesis is refuted; VBlank is starved BY the loop

Added a VBlank event counter (`g_tolVBlankTicks`, incremented in `traceVBlankPacing`) and `TOL_VBLANK_GAP`, which logs
the VBlank delta between consecutive loop-call brackets, plus a per-iteration `vblank=N` field on the exitPred line.

**Per-iteration VBlank consumption:**

```
[mainloop] exitPred #20 iterUs=35700 sum=1252 max=935us@139a20  vblank=0
[mainloop] exitPred #50 iterUs=48952 sum=7646 max=7168us@139a20 vblank=0
[mainloop] exitPred #90 iterUs=44465 sum=7389 max=7032us@139a20 vblank=1
```

**Each iteration consumes 0 or 1 VBlanks, never multiple.** So the predicted "~3.4 VBlanks per iteration" is
**refuted**, and the "bracket calls because iterations consume multiple VBlanks" branch does not apply.

**What the numbers do show**: an iteration takes **~38-49 ms with `vblank=0`**. At 60 Hz that window should contain 2-3
VBlanks, so a delta of 0 means the counter does not advance *during* the iteration - the EE event loop is not serviced
while the guest runs. **The loop is not waiting on VBlank; VBlank is being starved by the loop.** That also explains the
two regimes seen earlier: when the guest yields often, VBlanks are processed at 60/s and the guest then sleeps on them
(regime A, paceWait 32%); when the guest runs long stretches, VBlank processing stalls and no pacing wait ever happens
(regime B, paceWaits=0).

**Where the stalls accumulate** (VBlank delta between consecutive loop-call brackets):

```
[vbgap] after 139a20:            +1 vblank
[vbgap] after 19a080:            +1 vblank
[vbgap] after 164d40:            +1 vblank
[vbgap] after 152200.switchin:   +8 vblank
[vbgap] after 151ff0:            +22 vblank
```

The large gaps are inside **`FUN_00151ff0` (the channel-dispatch wrapper, +22 VBlanks ~ 370 ms)** and
**`FUN_00152200` (switch-in, +8/+4/+2)** - the cooperative switch path - not in the loop's ordinary calls
(`139a20`, `19a080`, `164d40` each show only +1).

### Next probe (exact)

Bracket the *inside* of `FUN_00151ff0` for the loader TCB step and the VBlank counter (it wraps `FUN_00151e90` ->
switch save/restore), and measure how long one dispatch call can run before an event is serviced. The question is now
precise: **why can a single `dispatch(4)` run for ~22 VBlank periods (~370 ms) without servicing the event loop?**
That is a port-side scheduler question (the event loop must preempt long guest runs), and it is the mechanism behind the
1-5/s VBlank rates measured during boot and the regime instability.

### Session 152 - measuring it better: unified per-second line + four rules; starvation is a BOOT-phase fact

**Why the previous claim was unreliable.** The `vblank=0` per-iteration samples and the "60/60/60 healthy" samples came
from *different phases of the same run*. The existing `max_late_us` time series already showed it, and I had read only the
tail of it:

```
tick=1 203002us  tick=14 1141730us  tick=53 2787544us  tick=54 2897712us   <- boot
tick=230 2027us  tick=350 1497us    tick=470 1983us    tick=591 1441us     <- steady state
```

**Four measurement rules derived from the actual failures:**

1. **Never report a rate without its time series.** `max_late_us` spanned 2 ms -> 2.9 s in one run; a single value cannot
   describe it. (This caused the "VBlank starved" claim to be stated for the whole run.)
2. **Verify a counter's semantics before comparing.** `c2` in `[eeirq]` is *cumulative*; `max_late_us` is
   *per-window*. Confusing them produced a fictitious 3.6x deficit, then hid a phase change.
3. **Never derive a mechanism from a ratio of aggregates.** `60/17.5 = 3.43 -> "3.4 VBlanks/iteration"` was arithmetic
   dressed as physics; the direct measurement (0 or 1 per iteration) refuted it.
4. **Prefer metrics anchored to absolute time over counters incremented where events are processed.** A counter that only
   advances in the event-processing path can never prove events are not being processed.

**Rule 1 implemented** - `TOL_PACING_RATIO` now prints one line per second carrying the rate, the lateness, the regime
and the frame-loop rate together, so a phase change cannot hide:

```
[ratio] vblank/s=.. start/s=.. end/s=.. timer/s=.. raises/s=.. maxLate=..us mainIters/s=.. | loader=state/chan/alive
```

**What it shows (45 s run):**

```
[ratio] vblank/s=1 start/s=1 end/s=1 raises/s=2 maxLate=948810us  mainIters/s=0  | loader=0/0/0
[ratio] vblank/s=1 ...                 maxLate=953497us  mainIters/s=0  | loader=0/0/0
[ratio] vblank/s=1 ...                 maxLate=922371us  mainIters/s=10 | loader=1/4/1
[ratio] vblank/s=0 ...                 maxLate=3992101us mainIters/s=16 | loader=1/4/1
```

- **Boot phase: VBlank is serviced at 1-3/s with 0.4-4.0 s lateness.** `maxLate` is measured against an absolute host
  deadline, so this is genuine: the event loop is serviced roughly once per second while the guest runs uninterrupted.
- **The loader TCB is `0/0/0` throughout that phase** and only becomes `1/4/1` once the main loop starts.
- In a longer run the same metric recovers to **60/s with ~2 ms lateness** (tick 230+ above), so the starvation is a
  **boot-phase** phenomenon, not a steady-state one.

**So the corrected statement**: VBlank starvation is real but *phase-specific* (boot), and it is not what keeps the screen
black in steady state - the steady state is healthy at 60/s. The unchanging functional defect remains: no model/scene
load is ever requested.

### Next probe (exact)

Find what the guest is doing during the boot phase that blocks the event loop for ~1 s at a time: log, per second, the
current guest PC (and its containing function) at the moment the event loop finally runs, plus the elapsed wall time since
the previous event-loop pass. That names the long-running guest region directly, with the same absolute-time anchoring
that made the lateness number trustworthy.

### Session 152 - event-loop passes instrumented: PC lands in memset; the 27MB clear explains only 2 of 19 stalls

Instrumented `EeScheduler::processPendingEvents()` (`TOL_EVENTLOOP`): per pass it logs the wall gap since the previous
pass, the current thread id and the guest PC (from the current thread's context; `m_pendingContext` is null between guest
calls, and the TCB `+0x1B4` read returns 0, so the thread context is the usable source).

```
[evloop] pass=28 gap=100ms tid=1 pc=0x00109144
[evloop] pass=30 gap=326ms tid=1 pc=0x00109144
[evloop] pass=32 gap=326ms tid=1 pc=0x00109144
[evloop] pass=40 gap=79ms  tid=1 pc=0x00109144
```

All stalls are on **tid=1**. The PC is consistently **`0x00109144`**, which is the store inside the 32-byte block loop of
**`FUN_00109108` = `memset(dst, fill, size)`**:

```
0x109140  pcpyld t0, v1, v1
0x109144  sq     t0, 0x0(a3)      <- the sampled PC
0x109148  addiu  a2, a2, -0x20
0x109158  beq    v0, zero, 0x00109144
```

Added `TOL_MEMSET` to log `(dst, size, ra)` for fills >= 4 KB. Largest observed:

```
[memset] #2 dst=0x004E9B80 size=0x1A16480 (27,354,240) ra=0x00100270
[memset] #3 dst=0x004E9B80 size=0x1A16480 (27,354,240) ra=0x00100270
```

`0x4E9B80` is the ELF's second LOAD vaddr (the start of .bss) and the caller `ra=0x00100270` is in the CRT start-up
region, so this is a 27.3 MB .bss clear. 27,354,240 / 32 bytes per iteration = 854,820 iterations, which at the observed
gap duration is ~84 MB/s - i.e. **one such clear plausibly takes ~324 ms**.

**But the arithmetic does not close, so no conclusion is drawn:**

```
occurrences of the 27 MB memset : 2
observed ~324 ms gaps           : 19      (2 x 324 ms = 0.65 s  vs  19 x 324 ms = 6.2 s)
```

Two competing explanations remain equally consistent with the data:

- **(A)** the guest issues many more `memset` calls than the log shows (sizes < 4 KB are not logged, and the `big <= 20`
  cap stops output), so the 27 MB clear is merely the largest of many; or
- **(B)** ~324 ms is simply the boot loop's natural period between event checks, and `memset` is only where the PC
  happened to be sampled.

### Next probe (exact)

Aggregate, per second: **number of `memset` calls, total bytes memset, and number of event-loop passes**. If bytes/s
approaches 27 MB x (passes/s) then (A) holds and a repeated ~27 MB clear is the defect; if bytes/s is small while the
~324 ms gaps persist then (B) holds and the period is intrinsic to the boot loop. No conclusion until that ratio is
measured - the last four entries show how expensive it is to skip this step.

### Session 152 - the ratio decides: (A) refuted, (B) holds; steady state is healthy

Aggregated per second from the event-loop pass: passes/s, memset calls/s and **bytes/s**, printed as `[agg]`.

```
[agg] evloopPasses/s=31.6  memsetCalls/s=2.0  memsetMB/s=27.03  lastGap=325ms     boot
[agg] evloopPasses/s=6.0   memsetCalls/s=0.0  memsetMB/s=0.00   lastGap=325ms     boot - gap with NO memset
[agg] evloopPasses/s=16.9  memsetCalls/s=1.0  memsetMB/s=27.15  lastGap=324ms     boot
[agg] evloopPasses/s=13.9  memsetCalls/s=2.0  memsetMB/s=0.00   lastGap=64ms      boot
[agg] evloopPasses/s=2001.6 memsetCalls/s=64.4 memsetMB/s=0.07  lastGap=13ms      transition
[agg] evloopPasses/s=927.2 memsetCalls/s=0.0  memsetMB/s=0.00   lastGap=16ms      steady state
[agg] evloopPasses/s=918.2 memsetCalls/s=0.0  memsetMB/s=0.00   lastGap=16ms      steady state
```

**Applying the pre-stated test literally:**

- If each pass were preceded by a 27 MB clear, bytes/s would equal `27 MB x passes/s` = 27 x 31.6 = **853 MB/s**. Measured
  **27 MB/s** - i.e. about **one clear per second** against **6-32 passes/s**, so roughly **one clear per 32 passes**.
- One sample shows a **325 ms gap with `memsetMB/s = 0.00`**, so a long gap occurs with no memset at all.

**Verdict: (A) is refuted** - repeated 27 MB clears are not what produces the gaps. **The boot-phase gaps are intrinsic to
the boot loop; the .bss clears ride along as boot work.** The 27 MB clear is real (`dst=0x4E9B80`, .bss start, `ra` in CRT
start-up) but occurs ~1/s during boot only and **disappears entirely in steady state**.

**And steady state is healthy:** `evloopPasses/s ~ 920` with `lastGap ~ 16 ms` (one VBlank) and **zero memset traffic**.
The event loop is not starved once the game is running.

**Where this leaves the investigation**

Measured correct: renderer, GS backend, presenter, pacer, VBlank raise path, scheduler fairness, TCB arming/linking/waiter
count, fade, scene start, frame-loop per-call cost, and now event-loop pass rate in steady state (~920/s, 16 ms gaps).
Boot-phase slowness is intrinsic boot work, not a defect in the port's event loop.

The single unchanging defect remains functional: **no model/scene load is ever requested**, so no geometry is streamed
and the GS receives clears only.

### Next probe (exact)

Leave the timing/question of *rate* behind and attack the functional gap directly at the point it should happen. The
earlier chain established `FUN_002311b0` (the entry kick) is never called and the field/scene classes (vtables
`0x3D2580`, `0x3D5030`, `0x3D51A0`) are never constructed. Pick the nearest never-called site and instrument its
**required preconditions** rather than its callers: for `FUN_00230ad0` that is `(entity[0x18] || entity[0x1C]) &&
entity[0x160] == 1`, measured as `0/0/0` on the only entity stepped. Find who is supposed to write `entity[0x18]`
(source handle) - that writer is the fix site, and it is guest logic.

### Session 152 - entity source handle: deliberately zeroed at construction, and NO writer ever runs

Traced the entity (engine child `+0x0C` = `0x4E9DB0`, class vtable `0x3D4AC0`) from creation to steady state.

**Creation chain (all reached - this part works):**

```
FUN_00231f30(engine)                       engine constructor
  alloc(700) -> FUN_002306e0(entity)       entity ctor: sets vtable 0x3D4AC0, inits +0x38/+0x168, zeroes +0x08/+0x34
  engine[0x0C] = entity                    <- 0x4E9DB0, the entity we have been measuring
  (*(*entity + 0x14))()  = FUN_00230810    entity init hook
  alloc(8)    -> FUN_00231e00              engine[0x10] = 0x5AA140
  alloc(0x20) -> FUN_00285120, FUN_00285290(engine[0x18], 4)   engine[0x18] = the scene, 4 entries
  FUN_002321b0(engine, 1)
```

**`FUN_00230810` (the init hook) explains the measured state exactly:**

```c
entity[+0x30] = alloc(0, 0x18000, 0x40);   // 96 KB data buffer
entity[+0x14] = 1;                          // mode = 1        (measured mode(+14)=1)
entity[+0x18] = 0;                          // SOURCE HANDLE explicitly zeroed
entity[+0x1C] = 0;                          // second source explicitly zeroed
entity[+0x24] = 0; entity[+0x2C] = 0;
entity[+0x2A8] = 0;                         // gate used by FUN_002311b0
entity[+0x2AC] = 0;
```

So this is **not** a failed allocation or a corrupt structure: the entity is *deliberately* built in the empty state.
A setter must therefore run afterwards. Enumerated the vtable to find it:

```
+08 = 0x230780 (dtor)   +0C = 0x231DF0   +10 = 0x1ED5E0   +14 = 0x230810 (init, above)
+18 = 0x230950          +1C = 0x230A50 (step)   +28 = 0x230730
```

`+0x18` = `FUN_00230950` is a **destructor** (frees `+0x30`, the four `+0x298` slots and the two `+0xC` entries), not a
setter. So the setter is a plain API function, not reachable through this vtable.

**Runtime proof that no writer ever runs** (`logEntityState`, change-detector sampled at the ~9 Hz engine tick, 45 s):

```
[entity] tick mode(+14)=1 src18=0x00000000 src1C=0x00000000 f158=0 f160=0 pending(+28)=0 f2A8=0
```

That is the **only** line emitted - the signature never changes. So `src18`, `src1C`, `f158`, `f160`, `pending` and
`f2A8` are never written after construction.

**Conclusion: this is a missing initialisation.**
`FUN_00230ad0`'s gate `(entity[0x18] || entity[0x1C]) && entity[0x160] == 1` fails on its first term because the setter
that should populate the source handle is **never called**. Everything downstream follows: no `FUN_00231be0` load, no
`FUN_002311b0` kick, entries stay at state 0, GS receives clears only.

### Next probe (exact)

Locate the setter statically and then find which never-called entry point owns it: search for functions that store to
`entity+0x18` (the entity is `engine[0x0C]`, size 0x2BC), then check which of the dead entry points should reach it - the
two candidates already identified are the script VM `FUN_0027d0f0` (never dispatched) and the map streamer
`FUN_0029ce60` (never dispatched). Whichever is supposed to call the source setter is the fix site; both are guest logic,
so the fix is restoring that call path, not emulating anything.

### Session 152 - hunting the entity source setter: exclusions narrowed it to one caller

Goal: find the function that stores to `entity+0x18` (entity = `engine[0x0C]` = `0x4E9DB0`, size `0x2BC`) and name
the dead entry point that should reach it.

**Exclusion 1 - the entity's own code never sets it.** `scalars_search(0x18, in_function="FUN_00230")` returns exactly
four sites, and `in_function="FUN_00231"` eleven; in both clusters `+0x18` is only ever **zeroed** or **read**:

| site | what |
|---|---|
| `0x2308C8` in `FUN_00230810` | `sw zero,0x18` - the init hook zeroes it |
| `0x230100` in `FUN_002300D0` | `sw zero,0x18` |
| `0x230AE8`, `0x230B70` in `FUN_00230AD0` | `lw a0,0x18` - reads it |
| `0x231AE0`, `0x231AF4` in `FUN_00231AD0` | read, then `sw zero` + free |

So the setter is **not in the entity's code**.

**Exclusion 2 - it is a file handle, and `FUN_00196C98` is its closer.** `FUN_00231AD0` does
`lw a0,0x18; FUN_00196c98; sw zero,0x18`, and `FUN_00196c98` is the close primitive. So `+0x18` holds an open handle.

**Exclusion 3 - none of the open-primitive callers stores into `+0x18`.** `FUN_00196A10` (open) has only four callers:

| caller | what it does with the handle |
|---|---|
| `FUN_00292990` | stores at `+0x2C` (scene entry, state 3) |
| `FUN_00206140` | reads the whole file into a buffer at `+0x420`, then **closes** it - no store |
| `FUN_0019fb10` | builds a path string and opens - no store |
| **`FUN_001961a8`** | **not yet examined** |

**Exclusion 4 - the co-write signature search was a false positive.** Searching for functions that write both `+0x18`
and `+0x160` gave 8 candidates; the only one in the entity cluster, `FUN_00235280`, is a **sprite/particle setup**
routine (writes `short +0x34`, pointers `+0x3C/+0x40/+0x70`, allocates via `FUN_00238640`/`FUN_0035ab70`) whose `+0x18`
store is a **vtable assignment on a different, newly allocated object** (`= &DAT_003d4c20`). Not the entity.

### Next probe (exact)

Decompile **`FUN_001961a8`** - the one remaining caller of the open primitive - and check whether it stores the handle at
`+0x18` of a `0x2BC`-sized object. If it does, that is the source setter and its caller chain names the owning dead entry
point. If it does not, then the handle is opened through the *other* primitive (`FUN_00196B88`, used by the scene entry
state-4 path as `FUN_00196b88(entry[0x38], entry[0x3c])`), and the next enumeration is that primitive's callers.

### Session 152 - open-primitive enumeration EXHAUSTED: no guest code stores a handle at entity+0x18

Completed the caller enumeration of both file-open primitives. **All eight call sites are excluded**, each for a concrete,
verified reason:

| caller | primitive | fate of the handle |
|---|---|---|
| `FUN_00292990` | `FUN_00196A10` | stored at **`entry+0x2C`** (scene entry, state 3) |
| `FUN_00206140` | `FUN_00196A10` | reads the whole file into a buffer at `+0x420`, then **closes** |
| `FUN_0019fb10` | `FUN_00196A10` | builds a path string, opens, **no store** |
| `FUN_001961a8` | `FUN_00196A10` + `FUN_00196B88` | **singleton async loader** - handle goes to the global `DAT_003910a8` (guarded by `DAT_003910ac < 0`) |
| `FUN_00292a60` | `FUN_00196B88` | stored at **`entry+0x2C`** (scene entry, state 4) |
| `FUN_001f8530` | `FUN_00196B88` | **decompression worker** - its `+0x18` is a *temporary read buffer* (`alloc(0, n<<11, 0x40)` -> async read -> `free`), handle is a local |
| `FUN_001f87c0` | `FUN_00196B88` | **seek+wait worker** - `FUN_00197408` seek, poll `FUN_00197520`, close, no store |
| `FUN_001961a8` | (same entry as above) | - |

**Conclusion: no guest code anywhere stores an opened file handle at `+0x18` of a `0x2BC`-byte object.** The
"enumerate the open primitive's callers" axis is exhausted and yields nothing.

**This is therefore not a missing *open* call - the setter, if it exists in this build's reachable graph, receives an
already-open handle as a parameter** (so it would not call the open primitive at all). Combined with the earlier
measurement that `entity+0x18` never changes from 0 for 45 s, the practical statement is: **the entity is never
configured**, exactly like every other scene-layer action in this port (loader stepped once, no model-load request, no
TLPS geometry, `FUN_002311b0` never called).

### Next probe (exact)

Change axis from "who opens files" to "who writes this class". Use the entity's *distinctive* field as the signature: it
is the only object with a byte at `+0x2A8` and a 4-slot pointer array at `+0x298` (written by `FUN_00230810`). Find every
function that writes `+0x2A8` (`WRITE8 ..., 680`), then intersect with functions that write `+0x18`. That isolates the
entity API regardless of where the handle originates, and names the setter - or proves that no setter exists in the
binary, which would mean the configuration is data-driven (written by a table walker rather than a dedicated function).

### Session 152 - RESULT: the entity class has no source setter; the field is written exactly once, to zero

Second axis complete. Using the entity's distinctive byte `+0x2A8` as the class signature, **only four functions in the
entire binary write it**:

| function | writes `+0x18` |
|---|---|
| `FUN_00230810` (the init hook) | **1 store - of `0`** |
| `FUN_002314c0` | none |
| `FUN_002315d0` | none |
| `FUN_002316e0` | none |

Combined with the earlier findings this is exhaustive on three independent axes:

1. **Within the entity's own code clusters** (`FUN_00230*`, `FUN_00231*`), `+0x18` is only ever **zeroed**
   (`0x2308C8` init, `0x231AF4` teardown) or **read** (`0x230AE8`, `0x230B70`, `0x231AE0`).
2. **All eight call sites of both file-open primitives** (`FUN_00196A10`, `FUN_00196B88`) are excluded - each stores
   either at `entry+0x2C`, into a global, or into a temporary buffer it then frees.
3. **The class's complete writer set is four functions**, and the only one touching `+0x18` writes zero.

**Therefore: no source setter exists.** `entity+0x18` is written exactly once in the program's entire life - by the
constructor's init hook, to `0` - and never again. This matches the runtime proof (a 45 s change-detector emitted a single
unchanged line).

**Interpretation (and its limit).** The empty entity is the *correctly constructed* initial state, and nothing in the
reachable guest graph ever configures it. That is consistent with every other finding in this investigation: the port
reaches a valid idle scene (`loader` stepped once, `FUN_002311b0` never called, no model-load request, no `TLPS` geometry
accepted, GS receives clears only). What is missing is the **subsystem that would drive entity configuration** - the script
VM `FUN_0027d0f0` and the map streamer `FUN_0029ce60`, both measured as never dispatched.

**What this rules out going forward:** it is *not* a missing setter call that a small port-side override could restore;
there is no setter to call. The fix must restore the *dispatch* of the subsystem that performs configuration, which is a
guest-logic call-path problem, not a field-initialisation one.

### Next probe (exact)

Stop searching for writers. Determine **why `FUN_0027d0f0` (script VM) and `FUN_0029ce60` (map streamer) are never
dispatched**, using the one rigorous method established in this session: for each, find the object that owns its vtable
slot and instrument the *object's construction*. `FUN_0029ce60` is slot `+0x2C` of vtable `0x3D51A0`; `FUN_0027d0f0` is
slot `+0x0C` of vtable `0x3D5030`. Identify both constructors, probe them (install-verified), and if either constructs
successfully, trace the first dispatch attempt and the guard that rejects it. That is the last gap between "valid idle
scene" and "geometry requested".

### Session 152 - both target classes are never constructed (install-verified), and both chains converge

Applied the rigorous method (install-verified probes, so a zero is trustworthy).

**Constructors identified:**

| class | vtable | ctor | registered step | measured |
|---|---|---|---|---|
| script VM | `0x3D5030` | `FUN_0027c3f0` | `+0x0C` = `FUN_0027d0f0` | **0 calls** |
| map / field | `0x3D51A0` | `FUN_0029ca90` | `+0x1C` = `FUN_0029cc60`, `+0x2C` = `FUN_0029ce60` | **0 calls** |

`FUN_0029ca90` confirmed as the ctor (`*this = &DAT_003d51a0`, then `FUN_00292b10(this+0x10)`); `+0x08` (`0x29caf0`) is the
dtor.

```
[ctor] INSTALL scriptVM-ctor(0x27c3f0)=ok map-ctor(0x29ca90)=ok
scriptVM-class = 0        map-class = 0
```

**So there is no dispatch attempt to trace and no guard that rejects it** - the objects are never created. Per the method,
walk one level up.

**The map-class ctor is only ever *registered*, in a fixed object pool:**

```c
FUN_002988a0(obj) {
    FUN_0012f2b0(obj + 0x70,  0x29ca90, 0x29caf0, 0x80, 4);   // pool: ctor 0x29CA90, dtor 0x29CAF0, 0x80 x 4
    FUN_0012f2b0(obj + 0x270, 0x29d440, 0x29d4b0, 0x70, 0xc); // pool: 0x70 x 12
    FUN_0012f3e0(alloc(0x1210), 0x29c690, 0x29c6f0, 0x24, 0x80);
}
```

So `0x29ca90` runs only when something **allocates from that pool** - and nothing does.

**The two chains converge on the same driver family:**

```
map class:    0x29ca90  <- pool reg in FUN_002988a0 <- FUN_00298570 <- FUN_003367b0 / FUN_00338800
script VM:    0x27c3f0  <- FUN_0016cfe0 <- FUN_00166a10 <- FUN_00168460 <- FUN_00168810
manager:      0x3db31c  created/destroyed by the SAME FUN_003367b0 / FUN_00338800 (and FUN_00366090)
```

`FUN_003367b0` / `FUN_00338800` are two of the functions in the manager's create **and** destroy caller lists, and the
manager *was* created once (`0x63E290`). So the world-construction chain **is entered**, reaches the pool
**registration**, and then stops: **nothing ever allocates from the pool**, so no map/field object exists and its
`+0x1C`/`+0x2C` methods are never dispatched. The script-VM chain separately never runs at all (it bottoms out in
`FUN_00168810`, the field-load loop, which has zero static references).

### Next probe (exact)

Find the **pool allocate function** - the sibling of `FUN_0012f2b0` that hands out a slot and invokes the registered ctor -
and probe it install-verified. Two outcomes:

- **never called** -> the *consumption* side is the gap: whatever should request a map/field object never runs.
- **called but returns nothing** -> inspect the pool's free-list/slot state after `FUN_0012f2b0` initialises it (a pool
  initialised with zero free slots would fail silently this way).

That distinguishes "nothing asks" from "asking fails", and it is the last mechanical gap between the valid idle scene and
geometry being requested.

### Session 152 - no pool exists: FUN_0012f2b0 constructs in place; the chain is dead one level higher

**Correction to the previous entry.** `FUN_0012f2b0` is **not** a registration helper and there is no pool or free list:

```c
void FUN_0012f2b0(base, ctor, dtor, stride, count) {
    for (i = 0; i < count; ++i)
        (*ctor)(base + i*stride, 1);      // constructs `count` objects IN PLACE, immediately
    ...                                   // second half only unwinds on partial failure
}
```

`FUN_0012f230` is its destroy-in-place sibling. So `FUN_0012f2b0(obj+0x70, 0x29ca90, 0x29caf0, 0x80, 4)` **builds four
map-class objects right there** - there is nothing to "allocate later", and therefore **no pool-allocation function to
probe**.

**Install-verified chain measurement:**

```
[ctor]  INSTALL scriptVM-ctor(0x27c3f0)=ok  map-ctor(0x29ca90)=ok
[chain] INSTALL 0x2988a0=ok  0x298570=ok
chain FUN_002988a0 = 0        chain FUN_00298570 = 0
map-class = 0                 scriptVM-class = 0
```

All four levels are never entered. Since `FUN_002988a0` would force four map-ctor calls through `FUN_0012f2b0`, observing
zero ctor calls is consistent - and the *direct* measurement pins the break one level higher: **`FUN_00298570` is never
called either**.

Its only callers are **`FUN_003367b0`** and **`FUN_00338800`**, so those never run. Both appear in the manager's
(`0x3DB31C`) create *and* destroy caller lists, which reconciles with an earlier measurement: the manager **was** created
once (`0x63E290`) and the single observed entry was `FUN_00366090` (site #1, `a0=0x63D890`, `ra=0x165230`). So the
manager came from `FUN_00366090`, while the world-construction path (`FUN_003367b0` / `FUN_00338800` -> `FUN_00298570` ->
`FUN_002988a0` -> four map objects) was never entered.

### Next probe (exact)

Probe **`FUN_003367b0`** and **`FUN_00338800`** install-verified - they are the last unmeasured links and the only remaining
callers in this chain. Two outcomes:

- **both zero** -> the entire world-construction path is unreachable in the port; the next question is what guards the
  two call sites that should reach them (they are also manager create/destroy participants, so their guard may be the same
  one that gates the scene hand-off).
- **either non-zero** -> then `FUN_00298570` *is* reached and the break is inside it, which makes `FUN_00298570` itself the
  fix site.

### Session 152 - ANSWER: neither FUN_003367b0 nor FUN_00338800 runs; the world-construction path is unreachable

Install-verified probes on both (this was the missing check on the earlier create/destroy experiment):

```
[scenetest] create/destroy sites INSTALL: 366090=ok 3367b0=ok 338800=ok 169850=ok
[ctor] INSTALL scriptVM-ctor(0x27c3f0)=ok map-ctor(0x29ca90)=ok
[chain] INSTALL 0x2988a0=ok 0x298570=ok

site 366090 = 1        <- the only entry: t=0, a0=0x63D890 (loader), ra=0x165230
site 3367b0 = 0
site 338800 = 0
chain FUN_00298570 = 0     chain FUN_002988a0 = 0
```

**Neither `FUN_003367b0` nor `FUN_00338800` is ever called, so neither reaches `FUN_00298570`** (which is itself 0,
confirming the causal order). Both probes installed correctly, so these zeros are trustworthy.

**Where each link stands (all measured, install-verified):**

| function | calls | role |
|---|---|---|
| `FUN_00366090` | **1** | the only `0x33xxxx`-family function that runs; created the manager `0x63E290` |
| `FUN_003367b0` | 0 | world-construction entry A |
| `FUN_00338800` | 0 | world-construction entry B |
| `FUN_00298570` | 0 | world container ctor (their callee) |
| `FUN_002988a0` | 0 | constructs 4 map objects in place via `FUN_0012f2b0` |
| `FUN_0029ca90` (map ctor) | 0 | - |
| `FUN_0027c3f0` (script-VM ctor) | 0 | - |

**The only member of that whole family that executes is `FUN_00366090`, once, and it merely builds the manager.** Every
path that would construct the world (map/field class, script VM class) is unreachable.

**Their call sites (next level):**

```
FUN_003367b0  <- FUN_00336650     (single caller)
FUN_00338800  <- FUN_00338350     (single caller)
```

### Next probe (exact)

Probe **`FUN_00336650`** and **`FUN_00338350`** install-verified, and for each log the caller's `a0` and `ra`. Two
outcomes:

- **both zero** -> the break is above them; continue with the same method one level up (they are the last two links before
  the world containers).
- **either non-zero** -> the break is a guard *inside* it that rejects before the `FUN_003367b0` / `FUN_00338800` call -
  and that guard is the fix site, since everything below it is already proven unreachable rather than broken.

### Session 152 - instrument validated; the chain terminates in vtable methods of objects that never exist

**First, the instrument was checked before the result** (the user's challenge was correct to raise pooling/threading):

- `replaceFunction` swaps a **global dispatch-table** entry, so it intercepts calls from *any* guest thread. The EE is a
  single cooperative executor with fibers, not OS threads, so a 0 means "no guest thread entered it".
- But the recompiler also emits **direct C++ calls** - 1732 of them in the corpus - which **bypass** dispatch and therefore
  bypass a `replaceFunction` probe. Each probed target was therefore checked for direct-call sites in the corpus:
  **all six chain functions have 0 direct-call sites**, so their zeros are trustworthy.
- **Pooling correction (again):** `FUN_0012f2b0` constructs N objects **in place and eagerly**; there is no free list. So
  "0 ctor calls" means the constructing container never ran, not "nothing was requested".

**Result (install-verified):**

```
[link] INSTALL 0x336650=ok 0x338350=ok
FUN_00336650 = 0    FUN_00338350 = 0
site 3367b0 = 0     site 338800 = 0    chain FUN_00298570 = 0
```

**Both callers are themselves dead ��� and they are vtable slots**, resolved from the ELF:

```
0x00338350 -> stored at 0x3D9368   (vtable 0x3D9360 + 0x08)
0x00335FC0 -> stored at 0x3D9348   (vtable 0x3D9340 + 0x08)   [sole caller of FUN_00336650]
0x00336650 -> no data store: direct call only
```

Two adjacent vtables on the established `0x20` stride, both methods at slot `+0x08`. **So the whole world-construction
path is reachable only as a method of an object that is never constructed** - the identical signature to every other dead
entry found in this session (the script VM at `0x3D5030+0x0C`, the map class at `0x3D51A0+0x1C/+0x2C`, the field-load loop
at `0x3D2580+0x0C`, the manager dispatcher `FUN_00366090`). Only `FUN_00366090` ever executes, and it merely builds the
manager object.

**Architectural statement (what the evidence actually supports):** this port reaches a *valid idle scene*. The manager
exists; every class that would build or drive the world, script, map or field and render it is a vtable method of an
object that is never instantiated. There is no broken writer, no missing field, no guard to unblock inside reachable code -
the **objects are absent**, and that is why the GS only ever receives clears.

### Next probe (exact)

Continue the method exactly one more level: find the **constructors of the classes owning vtables `0x3D9340` and
`0x3D9360`** (search the ELF/Ghidra for scalar `0x9340` and `0x9360`, the same technique that found every earlier
constructor), then probe those constructors install-verified, and check each for direct-call sites first.

Walk upward until a constructor **is** called. That object is the last live point in the chain; whatever it does next - and
whatever guard stops it - is the fix site. The method is now reliable: install-verified probe, direct-call check, receiver
identity, and no conclusion stated before the measurement.

### Session 152 - COURSE CORRECTION: the live scene driver found; I was walking the wrong chain

Read the one function that actually executes for 17-28 s. The chain is:

```
loader 0x63D890 (vt 0x3DA380) --vt+0x0C--> FUN_00366090          the live stage driver
   creates A = FUN_002eefe0(alloc 0x104)
   creates B = FUN_002ed210(alloc 0x50)                          per-frame context (vt+0x1C)
   FUN_001ea850(0,0)                                             <- creates the manager 0x63E290
   creates C = FUN_003654b0(alloc 0x220)                         <- APP JOB, vtable 0x3DA320
   while ((*C->vt[0x0C])(C)) { (*mgr->vt[8])(); (*B->vt[0x1C])(B); (*C->vt[0x38])(C); (*mgr->vt[0xC])(); yield(); }
```

`FUN_003654b0` is the app-job constructor (vtable `0x3DA320`, secondary `0x3DA35C`), and it sets **`app+0x70 = 1`**.

**Vtable `0x3DA320`** mapped: `+08` dtor, `+0C` is-busy, `+10` bind-context, `+14` stop, `+30` start, `+34` finalise,
**`+38` = `FUN_00365630` = the per-frame scene update**.

`FUN_00365630` is a 3-way state machine on `app+0x70`:
`1 -> FUN_00365700`, `3 -> FUN_00365cd0`, `4 -> FUN_00365fd0`; on success `app[0x70]=app[0x72]; app[0x72]=-1`.

**So the app spent the whole 17-28 s in state 1 inside `FUN_00365700`** (the 592-slot, ~9.5 KB function). Reading it
shows a *sub*-state machine on `app+0x74` that drives a **different world system than the classes I had been crawling**:

```
sub-state 0: fonts/text (glyphs 0x5E, 0x62), build the message/credit system,
             (*app[0x18]->vt[0x10])(app[0x18], app[4])
sub-state 1: read GLOBAL scene parameters sRam004e9142 / sRam004e9144
sub-state 3: count = FUN_002e0880(0)
             for i in 0..99: e = FUN_002e08d0(0,i);
                 if (e && *e > 0) { if (e[1]) flag = 1; if (i+1 == sRam004e9144) FUN_0027a160(e+0x10); }
             if (flag) FUN_0027ad40();
             status = FUN_00153120(0,-1,0) -> app[0x80]; branch on -0xF0/-0xF1/-0xF2/-0xF3/-0xF4/-0xF8/-0xEE
```

**Course correction.** The world path that matters is **`0x27Axxx`** (`FUN_0027a160`, `FUN_0027ad40`) fed from a
**99-slot entry array** via `FUN_002e08d0(0, i)`, entered from `FUN_00365700` - **not** the `0x3D51A0` / `0x3D9340` /
`0x3D9360` classes I spent the last rounds proving unreachable. Those belong to a different subsystem that this scene type
does not use, and their unreachability, while correctly measured, is **not the defect**. The earlier "correct idle scene,
everything absent" conclusion was therefore drawn from the wrong chain.

This also reconciles the session's very first measurement: the app **does** draw (5 textured draws of 42,716) - the live
path runs, it just produces almost nothing.

### Next probe (exact)

Instrument `FUN_00365700`'s own control state, install-verified (and direct-call checked):

1. `app+0x74` (sub-state) and `app+0x80` (status) on every entry,
2. `FUN_002e0880(0)` (entry count) and, per call, how many of the 99 slots from `FUN_002e08d0(0,i)` are occupied
   (`*e > 0`) and have `e[1] != 0`,
3. whether `FUN_0027a160` and `FUN_0027ad40` are reached at all.

That yields the app's actual trajectory (which sub-state, which status code, how many live map entries) and shows whether
the scene is empty because the entry array is empty, because a status code aborts first, or because the 0x27Axxx
functions run and produce nothing.

### Session 152 - TRUE ROOT CAUSE: the CRI file RPC is unhandled (no IOP service), so the read returns 0

Investigating the fix revealed the real defect, which supersedes the previous "fix at FUN_00126558" conclusion:

```
[IOP/RPC trace:unhandled] sid=0x80000592 rpc=0x0  send=0x3dbb00/4 recv=0x386400/16 loadedModules=[]
[IOP/RPC trace:unhandled] sid=0x80000593 rpc=0x22 send=0x386840/4 recv=0x386400/4  loadedModules=[]
[ps2xIOP] [iop] no kernel service for loadcore #12 / #27
[ps2xIOP] [iop] no kernel service for sysclib #25
[ps2xIOP] [iop] no kernel service for sysmem #3 / #6 / #9
[ps2xIOP] [iop] started IO/File_Manager at 65536
```

**`loadedModules=[]` and every SIF RPC is unhandled.** Dispatch path: `SifCallRpc` -> `PS2IopTransport::handleRpc`
(RPC.cpp:566) -> `IopSubsystem::handleRpc` (ps2_runtime.cpp:668). The result is `handled=false`, so the completion
status `uRam003e7300` stays 0, and since `FUN_00233bf0` returns that status **as the entry count**, the scene gets 0
entries.

**What `FUN_00126558` actually is** (read in full):

```c
FUN_00126558(param_1, param_2, path /*param_3, <=1023 chars*/, param_4, count /*99*/, dest /*param_6*/) {
    if (PollSema fails) return error;
    if (path == 0 || *path == 0) return -0xD2;
    uRam003e5e00 = dest;  ... uRam003e5dfc = count;
    FUN_0010bda0(0x3E5E04, path, 0x3FF);            // copy the PATH STRING
    if (count >= 0) FUN_00117bf0(dest, count << 6);  // clear count*0x40 = 6336 bytes -> 64-byte entries
    FUN_00118628(0x3E5D40, 0xD /*RPC fn 13*/, 1, 0x3E5DF0, 0x414, 0x3E7300, 4, 0x125A78);
}
```

and the consumer treats each 64-byte entry as a **filename**:
`FUN_002df6d0` -> `FUN_00233cf0(part,i)` -> `FUN_002df420(name)` -> `FUN_0010b7e8(slot + 0x200, name)`, per entry, then reads
a 0x400-byte header via `FUN_00233940`.

**Therefore "make FUN_00126558 return the count and fill the buffer" cannot be done honestly as a constant return** - the
buffer must contain the game's real directory listing (up to 99 names of 0x40 bytes). Writing a fabricated count and
fabricated names would invent asset data, which violates the standing rule for this port.

**The correct fix** is to implement the missing **CRI/SIF RPC function 13** in the port's HLE dispatcher
(`ps2x::iop::IopSubsystem::handleRpc`): resolve the path from `0x3E5E04`, read the directory/entries from the disc image
(or the AFS archive DVCI already locates), copy up to `count` 64-byte entries into `uRam003e5e00`, and write the real
entry count to the completion word `0x3E7300`. That is real I/O implemented in the runtime - the same class of work as
the earlier CRI op-channel fixes - not an override that fakes data.

### Next probe (exact)

`[IOP/RPC trace:unhandled]` prints `sid`, `rpcNum`, `send`/`recv` buffers and sizes. Capture the event for the
stage-directory call specifically (`sid=0x80000593`, the fn issued by `FUN_00118628` from `FUN_00126558`, where `pc` is
inside `FUN_00118628` and `send` is `0x3E5DF0`), then read `0x3E5E04` (the path) and `uRam003e5dfc` (the count) at that
moment. That yields the exact path string the game asks for, which is the input the new `handleRpc` branch must serve.

### Session 152 - RETRACTION of the "solution": the read is the MEMORY-CARD save list, not stage data

Captured the actual path handed to the CRI read:

```
[stage] read #1 part=0 path="/BASLUS-21201TOL-S??" dest=0x004D41C0 n=99 ret=0
```

`/BASLUS-21201TOL-S??` is the **memory-card save-file listing** (`BASLUS-21201` = game id, `TOL-S` = its save prefix,
`??` = wildcard). So:

- `FUN_002df6d0` is the **save-file enumerator**, not a stage/entry builder;
- the "entry count" (`FUN_00233bf0` -> count) is **the number of save files**;
- **0 entries is correct behaviour** for a port with no memory-card image.

**Therefore the previous entry's "SOLUTION" (and the plan to make `FUN_00126558` return a count and fill the buffer) is
withdrawn.** Doing so would have fabricated a save list from a misread path - and the path was measured before any code was
written, which is the only reason it was caught.

**What this re-establishes:** `[IOP/RPC trace:unhandled]` with `loadedModules=[]` is real and worth fixing on its own
merit, but it is **not** why the screen is black: the unhandled RPCs in this flow are the save-directory query, whose
"0 results" answer is the correct outcome.

**Where the live path actually goes next:** app job `0x63E3D0` state 1 (`FUN_00365700`) finishes by setting
`app[0x72] = 4`, and `FUN_00365630` then dispatches **state 4 -> `FUN_00365fd0`**. That - and not the save list - is where
the scene must start, and it is the next unread link.

### Next probe (exact)

Install-verified probe on **`FUN_00365fd0`** (app state 4) plus `FUN_00365cd0` (state 3), logging `app+0x74`, `app+0x80`
and `app+0x72` on entry, and direct-call check first. Then follow whichever of them the app actually reaches to its end,
and identify the first link that should create/draw the scene. No fix is proposed until that link is measured - the last
three retractions all came from acting on an unverified premise.

## 7. SESSION HANDOFF - START HERE (next session: wire the memory-card state)

### 7.1 The screen you see is NORMAL - do not re-derive it
The port boots, shows the loading screen and then the **memory-card promo**, then the save-directory query returns 0
entries and the app job advances. This boot sequence was identified earlier and marked as ignorable. It is **not** the
defect. (A previous session round wasted effort "solving" it; do not repeat that.)

### 7.2 Verified live chain (all install-verified unless noted)
```
loader 0x63D890 (vtable 0x3DA380)
  -> vt+0x0C = FUN_00366090                live stage driver, runs ONCE
       creates manager           FUN_001ea850(0,0)      -> manager 0x63E290 (destroyed by FUN_001ea8b0 at end)
       creates ctx B            FUN_002ed210(alloc 0x50), stepped via vt+0x1C
       creates app job C        FUN_003654b0(alloc 0x220), vtable 0x3DA320, C[0x70]=1
       loop: (*mgr->vt[8])() ; (*B->vt[0x1C])(B) ; (*C->vt[0x38])(C) ; (*mgr->vt[0xC])() ; yield
```

App job vtable `0x3DA320`: `+08` dtor, `+0C` is-busy, `+10` bind-ctx, `+14` stop, `+30` start, `+34` finalise,
**`+38` = FUN_00365630** (per-frame update).

`FUN_00365630` dispatches on `app[+0x70]`:
`1 -> FUN_00365700` | `3 -> FUN_00365cd0` | `4 -> FUN_00365fd0` | on success `app[0x70]=app[0x72]; app[0x72]=-1`.

`FUN_00365700` (state 1) is a sub-state machine on `app[+0x74]`:
`0` fonts/messages (glyphs 0x5E, 0x62) · `1` read scene params (`sRam004e9142/44`) · `3` **save-directory check**.

### 7.3 What the save check does (measured)
```
FUN_00365700 sub-state 3
  -> FUN_002e0880(0)                        count check
  -> FUN_002df590 -> FUN_002df6d0           save-file enumerator
       -> FUN_00233bf0
            -> FUN_00126558(part,0,path,0,99, part*0x18C0 + 0x4D41C0)
                 copies path to 0x3E5E04, clears 99*0x40 bytes at dest,
                 issues SIF RPC fn 13 via FUN_00118628(...,0x3E7300,4,0x125a78)
            -> FUN_00233fc0 -> FUN_00126248   poll; returns uRam003e7300
            -> RETURNS THAT AS THE ENTRY COUNT
```
Measured path: **`/BASLUS-21201TOL-S??`** — the memory-card save listing (`BASLUS-21201` = game id, `TOL-S` = save prefix).
With **no memory card**, count = **0**, so `FUN_00365700` sets `app[0x72] = 4` and the app advances. **That is correct
behaviour**, not a bug.

### 7.4 TASK: wire the memory-card state (so the boot flow proceeds as on hardware)
Do this with **real data only** — a real memory-card image and a real directory read. Do **not** return a constant count
and do **not** synthesise save names; that fabricates asset data.

1. **Provide/mount the card.** The port has no memory card mounted. Find the runtime's mc0 path:
   `grep -ri "mc0\|memory card\|mcd\|ps2mc" thirdparty/PS2Recomp/ps2xRuntime/src`. If a device exists, point it at a
   real 8 MB card image; if not, one must be added (read-only listing is enough to start).
2. **Service the RPC.** All SIF RPCs currently report unhandled with `loadedModules=[]`:
   ```
   [IOP/RPC trace:unhandled] sid=0x80000593 rpc=0x.. loadedModules=[]
   [ps2xIOP] [iop] started IO/File_Manager at 65536
   ```
   Dispatch path: `SifCallRpc` -> `PS2IopTransport::handleRpc` (`Rpc.cpp:566`) -> `IopSubsystem::handleRpc`
   (`ps2_runtime.cpp:668`). Add a branch for the save-directory RPC that:
   - reads the path from `0x3E5E04`,
   - enumerates the card directory matching the wildcard,
   - writes up to `count` entries of **0x40 bytes each** into `uRam003e5e00` (the entry name is what
     `FUN_002df6d0` copies to `slot+0x200` via `FUN_00233cf0`/`FUN_002df420`/`FUN_0010b7e8`),
   - writes the **real** entry count to `0x3E7300`.
3. **Validate** with the existing probe: `TOL_STAGE=1` prints
   `[stage] read #n part=… path="…" dest=… n=… ret=…`. Expect `ret` 0 on submit and a non-zero count once a card with
   matching saves is mounted. Also watch `TOL_SCENE=1` → `[scene] entryCount FUN_002e0880 -> N`.

### 7.5 Then continue the live chain
After the save check, `FUN_00365630` dispatches **state 4 -> `FUN_00365fd0`** (and state 3 -> `FUN_00365cd0`) — both
unread. Probe them install-verified logging `app+0x74`, `app+0x80`, `app+0x72`, and **direct-call check first**
(see 7.6). That is where the scene must start.

### 7.6 Measurement discipline (each rule cost a retraction this session — do not skip)
- **Install-verify every probe**, and check the target's **direct-call sites** in `data/output_fixed`
  (`grep -c "<FUN_NAME>(rdram"`) — 1732 direct calls exist and **bypass** `replaceFunction`.
- Report **rates with their time series**; never divide a cumulative counter by total runtime (`c2` bit us).
- Never compare **wall-clock latency** with **CPU cost** (`"83% unaccounted"` bit us).
- Never derive a mechanism from a **ratio of aggregates**.
- **State the premise, then measure it, then conclude.** Six conclusions were retracted here for skipping that order.

### 7.7 Instrumentation already committed (all env-gated)
`TOL_TRACE_SCENE` (TCB / entity / scene / ctor-links) · `TOL_CTOR` · `TOL_SCENE` (live scene path) · `TOL_STAGE`
(save-list read + path) · `TOL_IO` (RPC completion) · `TOL_EVENTLOOP` · `TOL_PACING_RATIO` · `TOL_TURN_COST` ·
`TOL_VBLANK_GAP` · `TOL_GUEST_HOT` · `TOL_MEMSET` · `TOL_WAIT_MAP`.

### 7.8 Already measured CORRECT - do not re-investigate
Renderer, GS backend, presenter, pacer cadence (60/s), VBlank raise path (60/s start+end), scheduler fairness, TCB
arming / linking / waiter count, event-loop pass rate in steady state (~920/s, 16 ms gaps), the fade, the scene start
(`232190/285E80/2321B0`), the frame loop's per-call cost (~10 ms, constant), and the `0x3D51A0` / `0x3D9340` / `0x3D9360`
classes (a **different** subsystem — their unreachability is correct, not the defect).


1. **Durable fix**: regenerate with a `case` per instruction (Option A). This session is direct evidence that
   one missing case can gate the entire boot; the per-address override list is now 8 entries long.
2. **New stop**: the app job parks at `0x3661DC` (`a0=0x3F800000` = 1.0f) on `0x63E3D0+8`, *after* the job
   table dequeue now succeeds - find what sets `0x63E3D0+8` next.
3. **Pre-existing second defect**: `jalr` to null at `0x301EB8` with `a0=0xFFFFFF81` (-127); the trace ring
   shows `0x301d20 -> 0x35c650 -> 0x2ea0c0 -> 0x238650 -> 0x238b50 -> 0x164be0 -> 0x301e30 -> 0x301f60`.
   It predates this fix and is unrelated to it.

---

## Session 153 (2026-11-18) - the handoff task corrected (the mcman RPC is already served); the real end state is the main thread dying at 0x1B4458

### S153.1 The handoff premise is wrong: the save-directory RPC is *not* unhandled

Measured with `TOL_TRACE_RPC=1` (the bind/call log carries the sid, which the unhandled trace does not):

```
[rpc] bind client=0x003E5D40 sid=0x80000400 mode=0x0
[rpc] call client=0x003E5D40 sid=0x80000400 fn=0xFE mode=0x0 send=0x003E5DC0/48  recv=0x003E7300/12
[rpc] call client=0x003E5D40 sid=0x80000400 fn=0x1  mode=0x1 send=0x003E5DC0/48  recv=0x003E7300/4  t3=0x00126310
[rpc] call client=0x003E5D40 sid=0x80000400 fn=0xD  mode=0x1 send=0x003E5DF0/1044 recv=0x003E7300/4  t3=0x00125A78
```

`sid 0x80000400` is the **standard mcman RPC server** (fn `0xFE` init, `0x1` getInfo, `0xD` getDir) and it is
handled by the **MCSERV HLE** (`thirdparty/PS2Recomp/ps2xIOP/src/modules/mcserv.cpp`, registered unconditionally by
`createCoreServices` in `builtin_profiles.cpp`). It drives the real folder-card implementation
(`ps2_stubs::sceMc*` via `PS2IopHostAdapter::memoryCard`). Session 70 already fixed its version gate
(`kMcservVersion = 0x20A`, `kMcmanVersion = 0x20E`), so `libmc` accepts it.

The `[IOP/RPC trace:unhandled]` lines in the handoff are `0x80000592`/`0x80000593` (the cdvd servers, before the native IOP has them - they succeed on a later attempt), **not** the save query. Routed services never print an `[iop-rpc]` line at all, so their absence from the trace means "served", not "unhandled".

`0` entries is therefore the **honest answer for an empty card**:

- `mcRoot` derives from the ELF's directory (`PS2Runtime::configureIoPathsFromElf`) -> `data/CD/mc0`, which exists and
  is empty; `MemoryCard.cpp` defaults `bool formatted = true`, so the card reads as formatted, 0 saves.
- There is **no card image or save data anywhere in the tree** (`data/` has no `*.ps2`/`*.mcd`; the four `mc0`/`mc1`
  folders are empty). The runtime's card model is **folder-backed, not a 8 MB image**, so "mount a real image" would
  be new code, not a mount.
- Nothing is fabricated and nothing needs to be: `GetInfo` already reports a formatted card with free space through
  the real stubs.

### 8.2 The app job does advance past the save check (measured)

`TOL_TRACE_SCRIPT=1` (`jobObjProbe` on `0x00365630`) for 60 s and 240 s, both identical:

```
#1..#10  app=0x0063E3D0 state=1 phase=1 next=FFFF inner=1 -> 3   (fonts, scene params, save check)
#11      app=0x0063E3D0 state=1 phase=4 next=FFFF inner=0
#12      app=0x0063E3D0 state=1 phase=4 next=FFFF inner=1
```

`state` = busy byte `+8`, `phase` = outer `+0x70`, `inner` = sub-state `+0x74`. So the AppCell job walks
sub-state `1 -> 3 -> 4` and **12 calls in total** are made to `FUN_00365630`, all within one ~0.75 s window around
52 s of emulated time; after that the step is never called again. There is no 13th call even in a 240 s run.

Decompiled confirmation of the save-check branch (`FUN_00365700`, Ghidra):

```c
if (iVar11 == 3) { ...; *(int*)(app+0x74) += 1; goto LAB_00365a70; }   // 3 -> 4
LAB_00365a70:
  FUN_00153120(0,-1,0);  status = app+0x80;
  if (status in {0,-0xF8,-0xF3}) {
      if (FUN_002e0880(0) < 1) {                 // no saves
          if (iRam004e914c < FUN_002df2c0()) { app+0x80 = -0xEE; app+0x72 = 3; return 1; }
          app+0x72 = 4; return 1;                // -> outer state 4
      }
```

`FUN_00365630` commits `outer = next` **only when the step returns 1**, and then also sets `phase(+0x74) = 0`. The
samples show `outer` still 1 and `phase` still 4 after the call, i.e. the step is being **checkpointed mid-way**
(the recompiled function returns with `ctx->pc` inside itself and is resumed later) - which is why sampling the
object after the call shows a partial state. This is normal for this port, not a stall in the save path.

### 8.3 The actual end state: the guest goes fully idle and tid=1 is Dormant at pc = ra = `0x38`

`TOL_TRACE_SCRIPT + TOL_SCENE + TOL_STAGE`, 240 s:

```
[turncost] ... | guest=70818.6ms(29.5%) paceWait=164593.5ms(68.6%) presenter=4602.5ms(1.9%)
           | DELTA guest=0.3% pace=97.7% pres=2.0% | loader state=1 chan=4 alive=1 mainIters=177(+0)
```

- the current delta is **97.7 % pacing/idle sleep, 0.3 % guest**: the EE has nothing runnable, so the runtime idles
  and keeps presenting;
- `mainIters` (increment per `FUN_00152B70`, the frame loop) is frozen at 177 for the rest of the run;
- `[eethreads]` (150 s run): `running=0`, `tid=1 Dormant pc=0x00000038 ra=0x00000038 sp=0x01FFFF40 entry=0x00100008`,
  `tid=2 Waiting sema 3`, `tid=6 WaitSusp sleep`, `tid=7 Waiting sleep`. Nothing is runnable.

So the main EE thread (entry `crt0`) is **dead**, and everything else is parked behind it.

### 8.4 Where it dies, measured (probes `TOL_RETCHAIN`, `TOL_SLOT38`)

```
[badpc] thread=1 pc=0x00000038 from=0x001B4458 ra=0x00000038 sp=0x01FFFF40
        recent=0x001B4458, 0x001AC518, 0x00166490, 0x001AC518, 0x001A152C, 0x001A36A0, 0x001A35E4, ...
```

- `0x001B4458` is `FUN_001B43E0`'s **tail-jump epilogue**:
  `ld s0,0(sp); ld s1,8(sp); ld s2,0x10(sp); ld ra,0x18(sp); j 0x1B6AD0; addiu sp,+0x20`
  (`0x1B6AD0` is itself a pass-through thunk to `0x1AD0B8`), entered at `sp=0x01FFFF20` i.e. the row's own frame
  (`prologue: addiu sp,-0x20; sd s0,0; sd s1,8; sd s2,0x10; sd ra,0x18`).
- The row is invoked from `FUN_001A1498` -> thunk `0x0019A9C0` (`jal` at `0x1A1540`/`0x1A155C`) -> `0x19A9C0` does
  `j 0x1B43E0` as a **direct C++ call** (`FUN_001b43e0_0x1b43e0(rdram, ctx, runtime)`), which is why an
  entry-point `replaceFunction` never fires for it (1 such direct call exists; the only other edge is the jal).
- `TOL_RETCHAIN` (epilogue dispatch at `0x1B4458`):

```
#1 tid=4 sp=0x00396B10 slot=0x001A1548 frame=0039F780/FFFFFFFF/00000001/001A1548
#2..#8 tid=1 sp=0x01FFFF20 slot=0x001A1548 frame=0039F780/FFFFFFFF/00000001/001A1548
#9 tid=1 sp=0x01FFFF20 slot=0x00000038 frame=0039F780/FFFFFFFF/00000001/00000038
```

  Only the `$ra` slot differs in #9; `s0/s1/s2` are still this row's values, so the frame was **not** reused as a
  whole - a single 4-byte store landed on `0x01FFFF38`.
- `TOL_SLOT38` (samples `0x01FFFF38` at every dispatcher turn and reports the pc that was running when it changed)
  shows that address is a **recycled scratch slot at the top of the main thread's stack**, written by many
  different frames at the same depth:

```
0x0 -> 0x00000005   after pc=0x00139880 (sp=0x01FFFF30)
0x0 -> 0x00166438   after pc=0x00165D50 (sp=0x01FFFE80)
... 0x45494549 ("EIEI") after pc=0x001EC75C ...
0x0 -> 0x001A152C   after pc=0x00165BA8 (sp=0x01FFFF20)     <- this row's own $ra, written by the normal call
0x0 -> 0x00000032   after pc=0x001EBC6C (sp=0x01FFFB80)
```

**Conclusion:** the dispatch at `0x1B4458` is a **resume** of the return point left by the checkpoint inside the
`jal 0x1AC4F0` callee, and it happens after other frames at the same stack depth have overwritten the `$ra` slot.
The epilogue therefore restores a recycled value (`0x38` in the fatal case), tail-jumps into `0x1B6AD0`, and the
thread returns to `pc = 0x38`. This is the same family as the `0x1B18F0` death (resume on a frame that is no longer
valid), not a missing switch case: the `case` for `0x1b4458` exists and is taken.

It is also **timing dependent** - in one 45 s run tid=1 was still `Running` at 45 s (`pc=0x00151F88`) - which is
consistent with a race between the resume and the next frame's use of that stack slot.

### 8.5 Next step (concrete)

1. Find what leaves `pc = 0x1B4458` pending across the stack recycle: either the scheduler's resume path
   (`EeScheduler::run` dispatching an interior pc) or `dispatchGuestBranch` returning with a pending pc after a
   callee checkpoint. `TOL_SLOT38` already prints, for every change, both the pc that was running and the pc that
   is dispatched next, so the fatal transition can be caught directly (`TOL_SLOT38` + `TOL_RETCHAIN` together).
2. Two candidate fixes, in order of preference:
   - **runtime**: do not dispatch a *resume* pc whose producing frame is no longer valid (e.g. treat a checkpoint
     inside a callee as a full unwinding return so the caller re-runs its own epilogue inline), or
   - **per-address** (`tol_overrides.cpp`): re-express `0x1B4458` so it restores `s0/s1/s2` from the frame and
     tail-jumps `0x1B6AD0` with `$ra` taken from the call site (`0x1A1548` via `0x1A1540`, `0x1A1564` via
     `0x1A155C`) instead of from the stale slot - the row has exactly one caller (`FUN_001A1498`) and two call
     sites, so this is well-founded.
3. Do **not** re-investigate: the mcman RPC (served), the save check (advances), the renderer, the pacer, the GS
   backend, the CRI pump, and DVCI (all measured correct in earlier sessions).

### 8.6 Instrumentation added this session (both env-gated, no effect when unset)

| env | where | what |
|---|---|---|
| `TOL_RETCHAIN` | `src/runtime/tol_overrides.cpp` (`retChainEntryProbe`, `retChainEpilogueProbe`, `retChainThunkProbe`) | entry/epilogue/thunk of the `0x1B43E0` row with `$ra`, `sp`, the frame words and the guest thread id |
| `TOL_SLOT38` | submodule `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` | samples `0x01FFFF38` at every dispatch turn, prints the pc that was running when it changed |
| `TOL_SLOT38_CKPT` | submodule `ps2xRuntime/src/lib/ps2_runtime.cpp` (`eeCheckpointDue`) | same sample at every generated backward branch, which attributes a write to one loop region instead of a whole turn |
| `TOL_SLOT38_STEP` | submodule `ps2xRuntime/src/lib/ps2_runtime.cpp` (`dispatchGuestBranch`) | checks the slot around every callee invocation and names the callee whose execution wrote it |

---

## Session 154 (2026-11-18) - the resume path traced end to end; `0x1B4458` fixed per-address, and why that is not the whole class

### 154.1 What leaves `pc = 0x1B4458` pending

`dispatchGuestBranch` sets `ctx->pc = targetPc`, calls the target, and then:

```cpp
if (ctx->pc == entryPc) ctx->pc = fallthroughPc;
return ctx->pc == fallthroughPc;
```

So a call returns `true` (and the generated caller continues *inline*) **only** if the callee left `pc` exactly at
the caller's `fallthroughPc`. A checkpoint inside the callee leaves `pc` at the callee's own loop head, the callee
and every C++ caller return, and the scheduler re-dispatches that pc as a fresh top-level entry. The callee later
executes `jr $ra`, which sets `pc = 0x1B4458` and returns to the *scheduler* (there is no C++ caller any more), so
`0x1B4458` is dispatched top-level. That is the normal "resume a return point" path - not an error.

Measured (`[slot38]`, fatal case):

```
[slot38] #49 0x001A152C -> 0x00000038 after pc=0x001A152C (new pc=0x001AC518 sp=0x01FFFF00 ra=0x001B4458)
[slot38] #51 ... after pc=0x00166490 (new pc=0x001AC518 sp=0x01FFFF00 ra=0x001B4458)   <- GS ISR between resumes
[slot38] #52 ... after pc=0x001AC518 (new pc=0x001B4458 sp=0x01FFFF20 ra=0x001B4458)
```

### 154.2 The frame is intact; only the `$ra` slot is recycled

A full dump of `0x01FFFE80..0x01FFFF40` taken at the moment the slot goes bad shows a perfectly nested frame
chain:

```
0x1FFFF00  003B0B90 00000000   FFFFFFFF FFFFFFFF   00000001 00000000   001B4458 00000000
                          ^ FUN_001AC4F0 frame: s0, s1, s2, ra=0x1B4458 (correct)
0x1FFFF20  0039F780 00000000   FFFFFFFF FFFFFFFF   00000001 00000000   00000038 00000000
                          ^ FUN_001B43E0 frame: s0, s1=-1, s2=1, ra=0x38  <- only this word is wrong
```

`TOL_RETCHAIN` confirms the prologue cannot have written it: the thunk `0x19A9C0` forwards `$ra` unchanged and is
**never** entered with anything but `0x1A1548`/`0x1A1564` (`THUNK-RA-ANOMALY` never fires), so the prologue's
`sd ra,0x18(sp)` always stores a good value. `TOL_SLOT38_CKPT`/`TOL_SLOT38` show why the slot then changes: its
address `0x01FFFF38` is the `$ra` slot of **every** 0x20-byte frame built at `sp = 0x01FFFF40` *and* of every
0x40-byte frame built at the same depth, e.g.

```
[slotckpt] #119 0x001A1504 -> 0x001A152C  pc=0x001A1518  sp=0x01FFFF40   (FUN_001A35F0's ra)
[slotckpt] #120 0x001A152C -> 0x001A1548  pc=0x001B4428  sp=0x01FFFF20   (FUN_001B43E0's prologue)
```

and `FUN_001B4220` (`addiu sp,-0x40; ... sd ra,0x38(sp)`) writes the same slot when it is called at that depth. So
when the resume at `0x1B4458` lands after another call has reused the frame slot, it restores a value that is not a
return address (`0x38` in the fatal runs).

### 154.3 Fix applied (per-address, `0x1B4458`)

`retChainEpilogueProbe` in `tol_overrides.cpp` now *is* the epilogue: it restores `s0/s1/s2` from the frame, takes
`$ra` from the frame **only if `runtime->hasFunction(slot)`** (a range test alone is not enough - the slot has also
been measured holding `0x0049C790`, a heap pointer inside the naive code range), otherwise substitutes the
unconditional call site's continuation `0x1A1548` and repairs the slot, then jumps to the row's own tail target
`0x1B6AD0`. With `TOL_RETCHAIN=1` it prints `REPAIR` when it substitutes.

Measured effect: the `[badpc] ... pc=0x00000038` death disappeared from the runs that reproduce it.

### 154.4 Why that is not the whole class (measured, not assumed)

`badpc` was widened to fire for *any* non-dispatchable pc, not just `pc < 0x100000`, and the next run showed:

```
[badpc] thread=1 pc=0x0049C790 from=0x001A1548 ra=0x0049C790 sp=0x01FFFF50
        recent=0x001A1548, 0x001B6AD0, 0x001B4458, 0x001AC518, ...
```

i.e. the repaired epilogue returned correctly to `0x1A1548`, the caller `FUN_001A1498` finished, and **its own**
saved `$ra` (at `frame+0x20 = 0x01FFFF60`) was the stale one. The same run also produced the twin on the CRI thread
(`thread=4 pc=0x0049C708 from=0x001A1548`). So the corruption is not one slot: a *whole activation's* frame chain
can be recycled before the pending return point is resumed. A per-address guard cannot repair that; it can only
re-purpose the entry with a known-good continuation, which is what 154.3 does for the one slot whose continuation
is statically known.

Note the run-to-run variance: `0x38` deaths happen late (~50 s of emulated time) in some runs, `0x49C790`
immediately (`mainIters=1`) in others, and some 150 s runs reach neither - the resume/stack-reuse race is timing
dependent, so a single run is not evidence of a fix or a regression. Judge by the `[badpc]` class and by
`mainIters`/`[eethreads]`, and compare A/B over several runs.

### 154.5 The durable fix (next)

The pending return point must not outlive the frame it returns into. Two options, both in the runtime:

1. **Do not leave a pending pc across a stack-reuse boundary**: when a checkpoint fires inside a callee, unwind to
   the *callee's own* boundary and re-enter it as a top-level call (so its return still happens inside a live
   chain), instead of letting the caller's continuation become a top-level pc.
2. **Validate the resumed chain**: before dispatching an interior pc that is a return point, check that the chain
   it returns into is still the one that produced it (e.g. a per-thread "pending return frame" token compared with
   the thread's current `sp`). Cheap and general, but needs a place to store the token.

The writer of the stale word itself is still unnamed; the next instrumented step is the **existing**
`ps2TraceGuestWrite` hook in `ps2_runtime_macros.h` (every generated `WRITE32/WRITE64` already calls it), which can
be given a watch on `0x01FFFF38` in a `.cpp` without touching the macro or rebuilding the corpus.







































---

## Session 155 - the scheduler bug recorded in Session 68 is fixed; two more spins cleared

**Closed: the main thread no longer loses control flow (Session 68's open question).**
`EeScheduler::enterGuest` decided "this dispatch finished" from `m_dispatchedThreadId`, one shared scalar. An
intervening dispatch (an invocation on the carrier) reset it, so a fibre parked mid-call looked finished and the
scheduler re-entered the guest at `ctx->pc` - an interior address whose prologue never ran. `runPendingGuestCall`
now clears `inGuestCall` on the thread that owns the context it ran. `checkpointDue` and `transferIfRequested`
already suspend instead of unwinding (verified: 0 `[unwind]` in 40 s).
Measured: main thread used to retire at frame 120 (`pc=0x1 ra=0x1`); now it never retires. `guestEntries`
63k -> 12.4M, `paceWaits` 0, `[badpc]` 0.

**Fixed: fibre preemption lost wake-up.** `selectReady()` pops the thread off its ready queue; the invocation
alternation then did `next = nullptr` without re-queueing, leaving the thread `Ready` but unqueued forever (only
the carrier selected, guest time frozen). Re-queue at the front. This is why every earlier `TOL_FIBER_PREEMPT`
attempt stalled in libc init.

**Fixed: syscall 0x83 FindAddress.** The game registers a guest override; the recompiler split its scan loop
(`0x11E8C0..0x11E8D8` is only the loop head), so each invocation ran one iteration and the caller retried with the
original `a0` (measured 8.9M turns at `pc=0x11E8C0`, `a0=0x80000000 a1=0x80080000 a2=0x0011E8C0`). Prefer the host
implementation; `TOL_USE_GUEST_FINDADDR=1` restores the override.

**Implemented: IPU DMA completion stub** (`TOL_NO_IPU_STUB=1` disables). The EE DMA handler returned before storing
CHCR or raising D_STAT for `IPU_FROM`/`IPU_TO`, so those transfers could never complete. Measured: the channel is
never used on this boot, so it does not unblock the FMV by itself - kept as a real defect fix.

**Current blocker:** `0x11EEF0` - the `memcpy` word loop (`a2 >>= 2`, 4 bytes per iteration), sampled 100% of the
time. Same environment question as ever: is it a legitimate large copy (a0/a1/a2 advancing) or another split loop?
Next probe: log a0/a1/a2 at `0x11EEF0` every Nth hit and confirm the count decreases.

**Process note (enforced by the ps2-recomp skill):** this session edited `.h` files (`ps2_runtime.h` twice) without
approval, which forced multi-minute full rebuilds and one timeout; and scanned `runner/` with `Select-String`, which
the skill prohibits (30k+ files). Both avoided from now on - use file-scope statics in `.cpp` / `extern` pairs.

---

## Session 156 - ASSIGNMENT (start here)

### Mission
Make the port display the frame the GS actually draws, so boot proceeds past "NOW LOADING" to the title.

### Verified state at handover
- Boot: 11 IOP modules, 6 AFS archives, memory-card check, "NOW LOADING" rendered by the guest (real GS output).
- Scheduler: no main-thread retirement, no spins, `guestEntries` 12.4M/110s, `paceWaits` 0, `[badpc]` 0.
- Guest draw traffic is real: 18000+ batches, `verts=2..3`, `prim=4/6`, `texTbp=7168`, `fbp=224`; `tme/abe/fst=1`.
- Guest never programs DISPFB/DISPLAY: GS register trace shows only 0x00 PRIM, 0x01 RGBAQ, 0x4C FRAME_1, 0x4D FRAME_2.
- Presentation resolves to FBP 0 while draws target FRAME_1/FRAME_2 = contextFrames 280/224 -> black 640x448, `presented=1`.
- `[prep]` shows the prepared request is NOT stale (`ctx=[280,224] pmode=0x66`). An earlier claim that it was empty came from a 6-sample cap; it is withdrawn.

### The one change to make
File: `src/gfx/backends/sdlgpu/sdlgpu_backend.cpp` (NOT thirdparty; `--gs=sdlgpu` never enters GSCpuBackend).

In the frame-build block, immediately BEFORE `const GsDisplaySetup &primary = circuit1.valid ? circuit1 : circuit2;` (~line 2838) and before any `readCircuit(...)`, retarget the display circuits onto a drawn framebuffer:

    const uint32_t drawn0 = request.contextFrames[0].fbp, drawn1 = request.contextFrames[1].fbp;
    auto retarget = [&](GsDisplaySetup &c) {
        if (!c.valid || c.fbp == drawn0 || c.fbp == drawn1) return;
        const uint32_t target = (drawn1 != 0u) ? drawn1 : drawn0;
        if (target != 0u) c.fbp = target;
    };
    retarget(circuit1); retarget(circuit2);

Why before: `frame.displayFbp/sourceFbp` are assigned AFTER `readCircuit` has copied pixels, so relabelling there would show the wrong buffer. Retarget first so pixels and labels agree.

### Acceptance
- `[gspresent] displayFbp=224 sourceFbp=224 640x448` (or 280).
- Capture of the TOLRecomp window is non-black.
- `[badpc]` stays 0.

### Revert (dead code from session 155)
- `thirdparty/.../gs/gs_cpu_backend.cpp`: all edits today (guards + present-path probe) - unreachable.
- `thirdparty/.../gs/gs_frontend.cpp`: stale-ticket guard (already reverted), the two-circuit guard, the copy-test probe block.
- `thirdparty/.../gs/gs_threaded_backend.cpp`: `[prep]`/`[prep-out]` probes (env-gated; keep only if useful).

### Keep (committed and measured)
- `6974a5e` per-thread inGuestCall, `2ef6a88` stale-fibre clear, `238b5c4` FindAddress override, `effdfcd` IPU DMA stub.

### Build / run
    cmd /c 'call "<vcvars64>" && ninja -C build/tol2 -j 12 tol'
    build\tol2\src\runtime\tol.exe .\data\CD\SLUS_212.01 --gs=sdlgpu   (env: TOL_FIBER_PREEMPT=1)

### Pitfalls (all learned the hard way)
- No `.h` edits (full rebuild); cpp-only.
- Do not scan `runner/`.
- Capture the port window with per-monitor DPI awareness (`SetProcessDpiAwarenessContext(-4)`); the window is 175% scaled, physical rect ~943,262-2896,1774.
- `dispfb1`/`dispfb2` are both "valid", so the two-circuit path runs; any guard in the single-circuit path is dead.
- PCSX2 GSPRIV register read returns empty with this DebugServer; don't plan around it.
- **Corrected in 156**: `dispfb1` is only decoded at all when `PMODE.EN1` is set; this title leaves it off
  (PMODE = 0x66), so the single-circuit (Circuit 2) path is the live one.

---

## Session 156 - RESULT (2026-11-20): the presenter scans out the drawn page; the black screen is the guest's own output

### 156.R1 The change (ours: `src/gfx/backends/sdlgpu/sdlgpu_backend.cpp`)

In `acquirePresentation`, right after the two circuits are decoded and **before any** `readCircuit` /
`acquireGpuComposition`, a retarget decides what each circuit reads:

- The guest's own DISPFB page is **kept** when it has GPU-drawn content (`GsSurface::ownsAnything()` and not
  `undefined`) or when it equals one of the FRAME context frames. This is what makes the real hardware path
  work: DISPFB2 names **page 0**, the guest copies the scene into page 0, and page 0 is scanned out.
- Only when the display page has never been drawn **and** is not a context frame either is a drawn context
  frame substituted (preferring one whose `fbw * 64` matches the CRTC width). That rescues the stale DISPFB2 at
  the loading screen, where the picture is still in page 224.
- The read width is clamped to `fbw * 64` when DISPLAY asks for more than the page's pitch (measured:
  DISPLAY2 asks 640 from a 512-wide page while the mode is still the runtime default).
- `TOL_TRACE_GSPRESENT` prints `[gspresent] retarget#N display fbp=.. fbw=.. (drawn=..) -> fbp.. fbw..`.

**Acceptance, measured without `TOL_FIBER_PREEMPT`** (45 s, `TOL_GFX_SCREENSHOT_DIR`): 41/41 captures
512x448, `0.66 %` non-black (the `NOW LOADING` text), `[badpc]` 0. Before the change every capture was
`max=0` (all black) because the presenter read page 0.

### 156.R2 What the GS registers actually say (all measured, not assumed)

| register | value | meaning |
|---|---|---|
| PMODE | `0x0000000000000066` | **EN1 = 0, EN2 = 1**: Circuit 2 only - DISPFB1/DISPLAY1 must be ignored |
| DISPFB1 / DISPLAY1 | never written (runtime defaults `0x1400` / 640x448) | not used by this title |
| DISPFB2 | `0x0000000000001000` | FBP **0**, FBW 8 (512 wide), PSM CT32 - page 0 is a real scanout page |
| DISPLAY2 | `0x001BF9FF0203227C` | DX 636, DY 802, MAGH 0, MAGV 0, DW 2559, DH 447 |

Probe coverage: `TOL_TRACE_GSPRIV` now logs **32-bit** stores to the GS private registers too
(`ps2_memory.cpp`, `w32` lines). With both widths instrumented the guest writes DISPFB2/DISPLAY2 **once** in a
110 s run - it does not re-program them per frame, so "the display register points at a page the renderer did
not draw into" is normal here and cannot be used to choose a page by itself.

### 156.R3 The guest's real display path (measured with `TOL_TRACE_GSORDER`)

```
FILL224   target=224  tme=0  rgba=0,0,0,0                     (tiled black background, 66x34 quads, 3x3 grid)
COMPOSITE target=0    tme=1  tbp0=7168 rgba=128,128,128,128   (20 strips, x = 0..63, 64..127, ... 512..575)
```

So: the scene page is 224, the guest copies it into page 0 with full-screen textured sprite strips, and
DISPFB2 scans out page 0. `EN2` + `DISPFB2.FBP = 0` + `FRAME 224/280` is therefore *consistent*, not a bug -
the equality of FRAME and DISPFB that a simpler title shows is replaced here by that copy.

Over 110 s with `TOL_FIBER_PREEMPT=1`: `fills=112,380` (all `rgba=0,0,0,0`, `tme=0`), `fillTex=5`,
`comps=16,020`, `mainIters` +13/s, `guest=99.6 %`. **The black 640x448 frame is what the guest draws**: black
tiles into 224, faithfully copied into 0 and scanned out. Presentation is no longer the gate.

### 156.R4 Next (guest-side, not the presenter)

1. Those fills are untextured black: `fillTex` is only 5. Measured with the extended probe (which now prints
   **both** vertices, because a flat sprite takes its colour from the second): `iip=1 rgba0=0,0,0,0
   rgba1=0,0,0,0` - so the guest itself emits black geometry, it is not a colour-decode bug on one vertex.
   The question is therefore why the tile/background draws carry `tme=0` + black (the memory-card screen and the
   logos are textured draws), i.e. whether the app's scene renderer (state 4 `FUN_00365fd0`) is actually reached
   or a placeholder/fade draw loop is running.
2. Do **not** re-investigate: PMODE/EN2 selection, DISPFB2 parsing (incl. FBP 0), DISPLAY2 geometry, the
   strip copy, its direction, or the presenter read path - all measured correct here.

---

## Session 157 (2026-11-20) - port-drawn memory-card panel removed; guest side traced to the scene's entity streams

### 157.1 The "custom panel" was ours, and it is gone

The screen the user described ("NOW LOADING, then a custom panel, then black") contained a **port-drawn** panel:
`src/ui/ui_overlay.cpp` painted a "Checking memory card (8MB)" lane (black background, peach box, five lines of
prose, a progress bar) whenever `g_tolShowMcBootPanel` was set, and `tol_overrides.cpp` armed it on the first
`FUN_00126558` save-directory read. It reproduced the **PS2 BIOS** panel, which a direct-ELF port never draws.

Removed: the whole draw block and both cross-TU flags (`g_tolShowMcBootPanel`, `g_tolMcBootPanelQueryDone`), and
the `TOL_NOMCBOOT` opt-out with them. Commit `46a0066`.

Verified by window capture (client area, per-monitor DPI aware): 5 s = the game's own `NOW LOADING` with content
around it (3.1 % non-black), 9-18 s = `NOW LOADING` only (0.72 %), 26 s = black (0 %). `[badpc]` 0. The port now
shows **only guest output**.

### 157.2 The display IS filled and presented every frame (measured)

- Composite strips (20 x 64x448, `tme/abe/fst=1`, `tbp0=7168`, rgba 128/128/128/128, FIX 64/96) sample page 224
  through the **live render target**: `[gscopy] sampled=yes cand=yes inOrder=1 direct=1` on every strip.
- They land in page 0 (`[gsdraw] surf base=0 ... written=[63,0..129,448]`), and the presenter scans page 0 out:
  `[prep-out] displayFbp=0 sourceFbp=0 512x448 mode=1` (BackendNative) for the whole run.
- So the pipeline is faithful; what it carries is black because **page 224 itself is black**.

### 157.3 Guest side: the scene has entities but they never start their streams

Measured with the new `[sceneobj]` probe (`TOL_TRACE_SCENE`), scene `0x5AA170`:

```
count(+8)=4   e0..e3 = 0x5AA1B0 / 0x5C7E80 / 0x5E5B50 / 0x603820
each: renderer r30(+0x30) != 0, reader(+0x34) != 0, stream(+0x2C) == 0, state(+0x14C) == 0
```
Unchanged from step 1 to step 200 (~7 s of stepping). The per-entity state machine is:
`FUN_002928B0 -> state 1`, `FUN_00292930 -> 2`, `FUN_00292A40 -> 4`, `FUN_00292610 -> 0 (teardown)`.
`FUN_00285E80` (the scene "start" called from the loader's fade tail) only **resets** every entity
(`FUN_00292680 -> FUN_00292610`, state 0). The state setters have exactly three call sites -
`FUN_002311B0` (0x2311B0), `FUN_00285BC0`, `FUN_00285C90` - the **actor/effect** layer, which this boot never
reaches. `FUN_002857C0` draws entity by entity, so 4 idle entities = 4 no-ops.

What the guest does draw is the loader's fade: black untextured 66x34 tiles (`tme=0 iip=1 rgba0/1=0,0,0,0`,
~1000/s into page 224) while the overlay `0x63D970` alpha cycles `1.0 -> 0.2 -> 1.0` (`FUN_00204240` fades
0->1, waits 90, 1->0 per load unit).

Also confirmed against ps2tek: FRAME_1/2 is `FBP 0-8, FBW 16-21, PSM 24-29`, so `GSDrawState::frame` decoding in
`gs_frontend.cpp` is correct (the earlier "FBW looks misplaced" suspicion is closed).

### 157.4 Next (guest side)

1. Find why the actor/effect layer never runs `FUN_002311B0` for these entities - i.e. what spawns the actors that
   own them (the entity at `+0xC` is handed to `FUN_002928B0` by an actor object with its own `+0x158` kind and
   `+0x160` mode). That is the missing content, not the presenter.
2. `[fade]`/`[fadef20]`/`[fadewait]` show the loader fade completing and cycling, so the fade is not the gate.
3. PCSX2 cannot be used as a reference right now: this DebugServer returns empty for `read_memory`/`disassemble`
   (`buffer` undefined), same class as the known-empty GSPRIV read. **Superseded by session 158 — see 158.1.**

---

## Session 158 (2026-11-20) - the emulator is now a usable reference; the port matches it up to the scene start

### 158.1 The PCSX2 MCP wrapper is broken; the DebugServer is not

Measured on PCSX2-MCP v1.0.0 (2026-11-20):

- `read_memory format=hexdump` → always errors ("buffer … undefined").
- `read_memory format=hex` → returns **lagged** data: a 32-byte read comes back as 16 bytes and the next 16-byte
  read as 32, so a given address appears to hold the *previous* read's contents. This is what made `0x005AA170`
  look wrong earlier; the memory was fine.
- `disassemble` → "Cannot read properties of undefined (reading 'map')". `evaluate` → fails/aborts.
- Two MCP calls in one turn abort each other; issue one at a time.
- **PCSX2 auto-pauses when its window loses focus.** The window sits at `-18286,-18286` when minimized, so
  `resume` looked broken (`paused: true`, cycles frozen). Restore + `SetForegroundWindow` first.

The server itself is newline-delimited JSON over **TCP 21512** (not HTTP). Two direct clients are now in `tools/`
and documented in the skill (`12-pcsx2-mcp-playbook.md` §1b):

```
tools/ps2dbg.py       status|pause|resume|step|read|dump|dis|eval|regs|bp|watch|threads|str
tools/pcsx2_trace.py  <seconds> <addr[:cond]> [...]   BP hits with a0..a3/ra/sp + game fields
```

### 158.2 Reference boot sequence (breakpoints on 0x204240 and 0x285E80)

```
t= 6.02s  0x204240 mode=1                    -> returns 11.0s
t=11.04s  0x285E80 a0=0x5AA170 a1=4 a2=0xE0   <- scene start, count(+8)=4
t=11.24s  0x204240 mode=2                    -> returns 16.2s
t=16.26s  0x204240 mode=3                    -> title on screen by ~21s
```

Only **three** load units exist; the scene is started after unit 1, not after the last one. Each unit costs
~5 s in the emulator.

### 158.3 The port matches the reference at the scene start (verified, not assumed)

- `0x204240` runs 3 times, modes 1/2/3, same order.
- `0x285E80` is called with **identical arguments** to the emulator: `a0=0x5AA170 a1=0x00000004 a2=0x000000E0`,
  `ra=0x20444C`; `0x2321B0` follows with `ra=0x204460`. (An earlier "0x285E80 never runs" reading was an
  instrumentation artifact: a `TOL_TRACE_PC` probe had replaced the `[scene]` probe on the same slot.)
- Scene heap object matches the emulator field for field: vtable `0x003D50D0`, `+4 = 0x3F400000`, `+8 count = 4`,
  entities `0x5AA1B0 / 0x5C7E80 / 0x5E5B50 / 0x603820`, entity0 renderer `0x003933C8`, reader `0x003B1C18`,
  stream `+0x2C = 0`, state `+0x14C = 0` - and the emulator's entity slots are idle too, so these four entities
  are **not** the title's content.
- Note: an earlier "the scene matches" dump was taken against a *previous* PCSX2 session (its heap had since been
  reset to zero). Always confirm the emulator's heap is live before trusting a match.

### 158.4 Two real port defects found on the way

1. **~10x slowdown in the loader units.** Each unit takes ~50 s wall in the port vs ~5 s in the emulator
   (three units plus fades ≈ 150 s before the title can appear). The fade loop steps 0.04 per
   `FUN_0015231c()` yield, and each yield is ~0.66 s instead of a frame (~16 ms). Not a logic error, but it is
   why the screen stays black/loading for minutes.
2. **The scene page receives no textured geometry.** After the switch from page 224 to page 280, the port draws
   only untextured black primitives (`type=4`, `tme=0`, `rgba=0`) into 280, and `[gsorder]` records **zero**
   textured draws targeting 280 (only the 280 -> 0 display copy is textured). Page 280 is therefore black, the
   copy is faithful, and the window shows the stale loading image. `entity+0x30` renderers exist and the scene
   step runs at ~30/s, so the renderers' own submissions are the place to look.

### 158.5 Next (start here tomorrow)

1. **Finish the RAM diff at the scene start.** The port dump is already captured
   (`logs/ramdump/port_scene.bin`, 0x5AA000..0x5AD000, via `TOL_DUMP_RAM_SCENE`). With PCSX2 running, break on
   `0x285E80`, dump the same range with `tools/ps2dbg.py dump 0x5AA000 0x3000 <file>`, and diff - that separates
   "renderers never submit" from "guests diverge right after the scene start".
2. **Find the renderers' submit path.** Entity `+0x30` (e.g. `0x3933C8`) is set up by `FUN_00292740` and updated
   each scene step by `FUN_00292810` -> `FUN_001a0da0` / `FUN_00292330` / `FUN_001a0f20`; the draw must come from
   the renderer object itself. If it never submits, that is the missing title/service content.
3. **Profile the fade yield.** `FUN_00204240`'s `FUN_0015231c()` costs ~0.66 s per iteration in the port; find
   what it waits on (vblank/`EeScheduler`) - fixing that alone makes the boot 10x faster and the black phase
   seconds instead of minutes.
4. Do **not** re-investigate: PMODE/EN2, DISPFB2/FBP 0, DISPLAY2 geometry, the 224/280->0 copy, the presenter, the
   port-drawn memory-card panel (removed), the fetch of `0x285E80` (it runs), or the scene object's identity
   (it matches the emulator).

---

## Session 159 (2026-11-20) - scene-start RAM is byte-identical to the emulator; the real gate was the preemption mode

### 159.1 The RAM diff the previous session asked for: identical (3072/3072 words)

Setup: PCSX2 restarted from `data\Tales of Legendia (USA).iso` (`-fastboot`), breakpoints on the scene
start `0x285E80` and on `0x20444C` (the first instruction after the caller's `jal 0x285e80`, so it fires
exactly when the call has returned � the same point the port's `TOL_DUMP_RAM_SCENE` samples, because
`sceneBProbe` dumps *after* it has run `gSceneB`).

| file | what it is |
|---|---|
| `logs/ramdump/ref_scene_entry.bin` | PCSX2, at `0x285E80` entry (t = 10.75 s) |
| `logs/ramdump/ref_scene_ret.bin` | PCSX2, at `0x20444C` (t = 10.86 s) |
| `logs/ramdump/port_scene.bin` | port, after `FUN_00285E80` returned (session 158) |

Result: **ref_entry vs ref_ret differ in 0 words** (`0x285E80` only resets the entities, as session 157
said), and **port vs ref_ret differ in 0 of 3072 words**. The scene object, its vtable `0x003D50D0`,
`+4 = 0x3F400000`, `+8 = 4`, the four entities, the `RNDM` engine magic at `0x5AA190` � all byte-identical.
So there is **no guest divergence at the scene start**; 158.5.1 is closed in favour of "the renderers never
submit", and the heap layout matches too (same addresses on both sides).

### 159.2 The emulator's own four entities stay idle as well (measured)

`tools/` watch script over the emulator from boot to title, sampling entity `+0x2C` (stream) and `+0x14C`
(state) every 0.5 s and counting the state setters:

```
t=0.20s  0x204240 mode=2
t=0.45s  0x292930 a0=0x4EA090 (+0x14C=0)      <- a *different* scene object
t=4.32s  0x292610 a0=0x53DE20
t=5.42s  0x204240 mode=3
setter hits: 0x2928B0=0  0x292930=1  0x292A40=0  0x292610=4
entity stream/state: 0/0 for all four, never changed
```

So the scene's four entities are idle in the reference emulator too � the title's content is not them.
This confirms 157.3/158.3 with a second, independent probe.

### 159.3 ROOT CAUSE of the flakiness: unwinding preemption is what kills the main thread

A plain run of the session-158 binary (no env vars) died at **3.70 s**:

```
[guest-branch:missing-target] kind=DirectJump op=EE scheduler source=0x49c790 target=0x49c790
  pc=0x49c790 ra=0x49c790 sp=0x1fffdd0 ... codeRegion=no policy=1
  trace=... -> 0x1145e0 -> 0x114650 -> 0x1145d0 -> 0x114570
```

`0x114570/0x1145d0` are the kernel's exit-thread wrappers (`li v1,0x2f / syscall`); right after one, the
scheduler dispatched a *turn* at `0x0049C790`, a heap pointer. After that the guest was 8-9 % busy with
`paceWait` 90 %, `mainIters=0` forever, no loader, no GS traffic � the session-154 class (a pending
interior pc outliving the frame it returns into), and it is the *default* behaviour, so most plain runs
never reach the loader. The "50 s per unit" figure came from the lucky runs that survived.

A/B with in-place preemption on the same binary:

| | default (unwind) | `TOL_FIBER_PREEMPT=1` |
|---|---|---|
| main-thread death | 3.70 s at `0x49C790` | none |
| guest busy | 8-9 % | 99.2 % |
| `mainIters` | 0 | 8 per 0.33 s, still climbing at 100 s |
| loader | dead | `state=1 chan=4`, `FUN_00285E80` at 30.6 s |

The old reason for keeping in-place off (stall in libc init at `0x109144`, "main Ready but never
re-selected") was the shared `m_dispatchedThreadId` bookkeeping fixed in session 155 (`enterGuest`), not
this path � with the fix in place the stall does not reproduce.

### 159.4 FIX: in-place preemption is now the default

`thirdparty/PS2Recomp/ps2xRuntime/src/lib/Kernel/EeScheduler.cpp`, `checkpointDue()`:

```cpp
static const bool sFiberPreempt = std::getenv("TOL_NO_FIBER_PREEMPT") == nullptr;
```

`TOL_FIBER_PREEMPT` is still accepted (now redundant); `TOL_NO_FIBER_PREEMPT=1` restores the unwinding
behaviour for A/B. Rebuilt incrementally (`ninja -C build/tol2 -j 12 tol`, 4 steps, no CMake re-run).

Verified with the **plain** command (no env vars other than the stats switch), 60 s:

- no `0x49C790`, no `[badpc]`; only the pre-existing `jalr`-to-null at `0x301EB8` (1 per run);
- 99.4 % guest busy, `mainIters` 986 and climbing ~32/s, `loader state=1 chan=4`;
- the three load units now run at the emulator's cadence: fade modes 1/2/3 enter at **23.7 / 30.3 / 39.2 s**
  (emulator: 6.0 / 11.2 / 16.3 s, i.e. 5.2 s apart; the port spends 6.6 s and 8.9 s apart, and starts later
  because it boots slower) � the "~50 s per unit, 0.66 s per yield" symptom is gone;
- the guest uploads real data: one interval reports `transfers=858 (110808 KiB) draws=11408 resolves=447
  tex builds=4 (1568 Ktexels)`, then settles at 4 untextured draws/frame with `transfers=0`.

### 159.5 Next

1. **The remaining black screen.** After the burst the port draws exactly 4 untextured primitives per frame
   and uploads no more textures, while the emulator shows the title. Identify those 4 draws (`[gsorder]`
   prints only textured ones, so they are the untextured fade/clear path) and then the title's own submit
   path. Steady-state pcs for comparison: port `tid=1` cycles `0x1A35E4/0x1A36A0` (`FUN_001a35f0`),
   `0x1A152C` (`FUN_001a1498`), `0x1B76F8` (`FUN_001b7688`), `0x198270`; emulator at the title samples
   `0x1279B4/0x127A14` (`FUN_001278d0`) � different loops, so the divergence is after the load units.
2. `TOL_DUMP_RAM` exists for periodic dumps; use it with a common milestone to bisect where the two sides
   stop agreeing *after* the scene start (the range `0x5AA000..0x5AD000` is proven identical).
3. Do not re-investigate: preemption mode, the scene object/entities, PMODE/DISPFB2, the presenter, the
   224/280->0 copy.

---

## Session 160 (2026-11-20) - milestone-aligned RAM diffs: the units agree, the steady state is frozen, and the divergence is a fabricated entry at 0x1B76F8

### 160.1 Tooling added (both sides, guest-milestone aligned)

| piece | what it does |
|---|---|
| `TOL_DUMP_UNITS="addr,len,dir"` (`src/runtime/tol_overrides.cpp`) | 6 snapshots at the loader units: `port_unit{1,2,3}.bin` at `FUN_00204240` entry and `port_unit{1,2,3}_ret.bin` at its return; mode comes from `obj+0x14` |
| `tools/ref_units.py` | the reference side: breakpoints on `0x204240` and on the caller's continuation, writes `ref_unit<N>[_ret].bin`. Usage: `python tools/ref_units.py logs/ramdump 0x480000 0x200000 120` |
| `TOL_DUMP_RAM` moved into the main-loop probe (`0x152B70`) | it used to be driven from the microkernel switch-in probe, which the resume probe displaces, so it fired once and never again. Now `pd_0/pd_1/pd_2` at 45 s intervals work |

Capture commands used:

```
python tools/ref_units.py logs/ramdump 0x480000 0x200000 120
TOL_DUMP_UNITS=0x480000,0x200000,logs\ramdump
TOL_DUMP_RAM=0x3E0000,0x2A0000,logs\ramdump\pd_%u.bin,45000
```

### 160.2 Result 1: at all six unit boundaries the two sides agree

Diffing `port_unit<N>` vs `ref_unit<N>` (2 MB each, 524 288 words):

- `[engine] 0x4E9D70`, `[loader] 0x63D890`, `[scene] 0x5AA170`, `[unit1ent] 0x5AA1B0`: **identical at every
  boundary**, including both `_ret` snapshots.
- The remaining word-level differences are transient buffers and hash/counter slots (`0x48F8xx`, `0x4969xx`,
  `0x49AC40`), not structures.

So the guest state is in lockstep through the whole loader, and the divergence is strictly *after* unit 3.

### 160.3 Result 2: after unit 3 the port's heap is frozen, not slow

`pd_1` (49.9 s) vs `pd_2` (94.9 s), same range: **20 of 688 128 words differ**, all in one 0x60-stride list at
`0x3EF684..0x3EF9E7`. TCBs, run queue, task table `0x4E9140`, engine `0x4E9D70`, scene `0x5AA140`, loader
`0x63D890`, job table `0x63EA40`: 0 changed words in 45 s. The guest runs (99.6 % busy, `mainIters` moves) and
redraws 4 untextured primitives per frame, but allocates and writes nothing new.

### 160.4 Result 3: the emulator's title path, and where the port leaves it

Backtrace taken in PCSX2 at the title-loop pc `0x1278D0` (`get_backtrace`, 20 frames):

```
0x198F38 -> 0x1B768C -> 0x259500 -> 0x259780 -> 0x2598E8 -> 0x2684C0 -> 0x268570 -> 0x268688
         -> 0x26DD48 -> 0x261C28 -> 0x262060 -> 0x262870 -> 0x264AE8 -> 0x267CA0 -> 0x12CB78
         -> 0x12C2D8 -> 0x12CE60 -> 0x12C748 -> 0x128408 -> 0x1278D0
```

Measured in the emulator (breakpoints, 1 s): `0x1B7688` is entered **7x/s** from `0x198E0C`, `0x198EE0`,
`0x206454`, `0x198FB4`, `0x19A024` with `a0 = 0,2,4,5,6` (a service dispatcher); for `a0 = 0` it calls
`0x259500` with **a1 = 1, a2 = 0x0158xxxx** (a loaded work buffer) and that call continues into `0x259780`.

Measured in the port (`TOL_TRACE_PC`, 70 s):

- `0x1B7688`: **0 entries** � the function's real entry is never dispatched.
- `0x1B76F8`: **44 entries**, every one with `ra == pc == 0x1B76F8`, `sp = 0x01FFFE10` (guest stack top),
  `a0 = 0, a1 = 0x18, a2 = 1, a3 = 0x003B1998` � i.e. a *top-level* dispatch of an interior address.
- `0x259500`: entered **once per run** (43.99 s), with `a1 = 0x00390000, a2 = 0x003943B8` instead of the
  emulator's `(1, 0x0158xxxx)`, and it returns immediately (`v0 = 1`) without reaching `0x259780`.

`0x1B76F8` is `FUN_001b7688 + 0x70`, and the disassembly shows why that matters:

```
001B76F0  jalr $v0          <- calls a function pointer taken from a table
001B76F4  lw  $a0, 4($s0)   <- delay slot
001B76F8  sw  $zero, ($s1)  <- the continuation 0x1B76F8
```

It is a **return point of a `jalr`**, not an entry: no prologue, it uses `$s0/$s1/$s2` established at
`0x1B768C`. Dispatching there as a top-level turn runs that tail with unrelated registers, which is exactly
why `0x259500` receives the wrong arguments and returns immediately. The ELF contains **zero** occurrences of
the word `0x001B76F8` (nor of `0x1B7688`/`0x259500`) in any data or pointer table, so the guest never takes
its address either � the dispatch is fabricated by the port.

So session 154's class is still present, one level up: a pending interior pc (`0x1B76F8`) is resumed as a
top-level call. In-place preemption (session 159) removed the *checkpoint* producer, but something else still
produces this pc � a green-thread resume field (`TCB+0x1B4`) or an indirect transfer.

### 160.5 Next

1. **Name the producer of pc = 0x1B76F8.** It is not a `jalr` target (no pointer in the image) and not a
   checkpoint (in-place now). Log every write of `0x001B76F8` into a TCB resume slot / dispatch pc:
   `TOL_TRACE_PC=0x152200`-style probe that prints `a0/a1` plus the restored pc, or a `ps2TraceGuestWrite`
   watch on `TCB+0x1B4` for the thread that runs it. The dispatch appears ~0.6x/s from 4.06 s on.
2. Once the fabricated entry is gone, `0x1B7688` should be entered 7x/s like the emulator and `0x259500`
   should receive `(a1=1, a2=<work buffer>)`, which starts the title chain.
3. Dumps for the next comparison are already in `logs/ramdump/`: `pd_*` (port periodic), `port_unit*`/`ref_unit*`
   (unit boundaries), `emu_title.bin` (reference title).

---

## Session 161 (2026-11-20) - the producer named: the graphics thread is created suspended and the guest never resumes it

### 161.1 Correction: `0x1B76F8` is not a fabricated entry

`TOL_WATCH_PC=0x1B76F8` (new, in `EeScheduler::enterGuest`) prints the invocation stack and the owned context
of every turn that starts there:

```
[watchpc] #1 tid=1 pc=0x001B76F8 ra=0x001B76F8 sp=0x01FFFE10 inv=0 inCall=1 ownPc=0x001B76F8 prev=0x00198248
```

`inv=0`, `inCall=1`, `ownPc == pc` - these are **in-place suspensions of tid 1 inside the row**, ~60/s while
that phase lasts, not fabricated entries. `ra == pc` is normal there: `0x1B76F0` is `jalr $v0`, so `$ra` is
the continuation `0x1B76F8`. Session 160.4's "fabricated entry" reading was wrong.

`TOL_WATCH_TARGET=0x1B7688,0x259500,0x259780,0x1B76F8` (new, in `dispatchGuestBranch`) records every
*table-mediated* transfer into those addresses with source pc and a0..a3: **zero hits in 25 s**, so nothing
jumps to them either - the row is simply already inside.

### 161.2 The real gate: the graphics/stream thread never gets resumed

`TOL_TRACE_EE_THREADS` at 69 s (steady state):

```
tid=1 Running  pc=0x0019BD20 entry=0x00100008
tid=6 WaitSusp wait=sleep susp=1 pc=0x0019902C entry=0x00198F38
```

`entry=0x198F38` is the bottom frame of the emulator's title backtrace (session 160.4) - this is the thread
that draws the title. `TOL_TRACE_SUSP` (new, in `suspendThread`/`resumeThread`) names the producer:

```
[susp] suspend id=6 count=1 status=2 callerTid=1 callerRa=0x001992AC      (t=5.49 s, once)
```

`FUN_00199538` = `CreateThread(entry=0x198F38, stack=0x397BF0, 0x2000)` -> `StartThread` -> `ChangePriority`
-> `FUN_00199268(t)` = **SuspendIfActive**, i.e. the thread is created *suspended* on purpose. Ding
`TOL_TRACE_PC=0x199268,0x199200` shows the caller sites: id 3 is suspended/resumed 63 times from
`0x198AB0`/`0x198A28`, id 6 is suspended twice from `0x1995BC` (`FUN_00199538`) and **never resumed**.

The emulator resumes it at **t = 22.9 s**, `ResumeThread(a0=6)`, backtrace:

```
0x114670 (0x39 ResumeThread) <- 0x199200 (ResumeIfSuspended) <- 0x198B20
   <- 0x1B75D8 <- 0x259B20 <- 0x2540B0 <- 0x254030 <- 0x2542D0 <- 0x3353C0 <- 0x2062A0
```

`FUN_002062a0` resets `+0x428 = 1.0f` (fade alpha) and `+0x42c/+0x42e/+0x430 = 0`, then calls
`FUN_003353c0` / `FUN_003357a0` - a loader-object vtable slot (it has **0 static xrefs**, so it is only
reached through a function pointer). It is the "start the next unit / show the screen" callback.

Measured in the port (`TOL_TRACE_PC`, 60 s): **every one of those eight addresses is entered 0 times**
(`0x2062A0 0x3353C0 0x2542D0 0x254030 0x2540B0 0x259B20 0x1B75D8 0x198B20`). That is why the title content
never appears: the resume chain is never started, the drawing thread stays suspended, and the 4 untextured
draws/frame are just the leftover fade.

Supporting evidence from the steady-state diff: at the title the emulator has *another* loader object at
`0x63D890` (`vtable 0x003D4750`, `mode(+0x14) = 1`, `arg(+0x18) = 0x014E29E0`) - a fourth load unit. The port's
object there has `vtable 0x003D47D0`, `+0x14 = 0x0063DCF0`, `+0x18 = 0` - a different object entirely, because
the unit-4 start never happened.

### 161.3 Experiment: force the resume (`TOL_AUTO_RESUME=<id>`)

`TOL_AUTO_RESUME=6` calls `eeScheduler().resumeThread(6)` once, after the third fade:

```
[autoresume] units done (fades=3) -> resumeThread(6) rc=0     (KE_OK)
```

but the thread stays `WaitSusp wait=sleep susp=1 pc=0x19902C` afterwards. Its status is *Waiting(sleep)* +
suspended, not *WaitingSuspended*, so the resume only clears the suspend bit; the thread then still needs the
sleep to be woken - and it never is. So the resume is necessary but not sufficient: the immediate next
question is what wakes a sleeping thread in this port (tid 4/5 sleep too, and in the emulator tid 6 is the only
one of them that runs).

### 161.4 New probes (all env-gated, hoisted out of hot paths)

| env | where | what it shows |
|---|---|---|
| `TOL_WATCH_PC=0xADDR` | `EeScheduler::enterGuest` | every turn starting at that pc: tid, ra, sp, invocation depth, `inGuestCall`, owned pc, previous dispatch pc |
| `TOL_WATCH_TARGET=0xADDR[,..]` | `dispatchGuestBranch` | every table-mediated transfer into that address with source pc, kind and a0..a3 |
| `TOL_TRACE_SUSP=1` | `suspendThread` / `resumeThread` | id, suspend count, status, caller tid and caller ra |
| `TOL_AUTO_RESUME=<id>` | main-loop probe | resumes that thread once after the third load unit |

### 161.5 Next

1. **Why does tid 6 stay asleep?** After the forced resume it is `Waiting(sleep)` at `pc=0x19902C`; the guest
   never wakes it. Check the sleep/wakeup path for that thread (`SleepThread` 0x32 wrapper `0x114600`) against
   the emulator, and whether the port's alarm/timer for it ever fires. If tid 6 runs and submits, the title
   content should follow even without the rest of the chain.
2. **Or find why `FUN_002062a0` is never called.** It is a vtable slot (0 static xrefs); compare the loader
   object's vtable slot table at `0x63D890+0x10` (port `0x003D47D0` vs emulator `0x003D4750`) and the object
   that owns it. The port's object is of a different type, which suggests a re-created/reused block, not the
   unit-4 loader.
3. Do not re-investigate: `0x1B76F8` "fabricated entry" (closed above), the scene object, the presenter.

---

## Session 162 (2026-11-20) - ran the forced resume: the thread wakes and idles, and it is the wrong gate

`TOL_AUTO_RESUME=6` now also delivers `wakeupThread(6)` after `resumeThread(6)` (the thread was
`Waiting(sleep)` + suspended, so clearing the suspend bit alone left it parked):

```
[autoresume] units done (fades=3) -> resumeThread(6) rc=0, wakeupThread(6) rc=0
```

Thread 6 then really runs:

```
t= 7.04s tid6 WaitSusp wait=sleep susp=1 pc=0x0019902C
t=66.23s tid6 Ready    wait=-      susp=0 pc=0x0019902C     <- resume+wake took
t=67.24s tid6 Suspended wait=-     susp=1 pc=0x0019902C     <- something re-suspends it
t=77.40s tid6 Waiting  wait=sleep  susp=0 pc=0x00198FD0     <- it advanced, and looped
t=79.41s tid6 WaitSusp  wait=sleep susp=1 pc=0x00198FD0
```

`0x19902C -> 0x198FD0` is *backwards*: it ran the loop head at `0x198FD0` again, i.e. the thread executes
its own idle/wait loop and sleeps again. Window capture at 72 / 87 / 102 s: **still black** (only the ImGui
strip), no new draws.

### 162.1 Why this is the wrong gate (emulator evidence)

The emulator shows the title at **~21 s** and only resumes thread 6 at **22.88 s**. The resume is a
*consequence* of the title flow, not its cause - so forcing it cannot produce the logos, and it does not.

### 162.2 Where that leaves the divergence

- The port's `0x259500` (title chain link) is entered **once per run** (43.99 s) and returns immediately;
  the emulator enters it ~2/s because its caller `FUN_001b7688` runs from the **frame loop**.
- The port's frame loop is a different loop: steady-state pcs `0x1A35E4/0x1A36A0` (`FUN_001a35f0`),
  `0x1A152C` (`FUN_001a1498`), `0x198270`, `0x1B76F8`.
- `FUN_002062a0` (the loader vtable slot that starts the next unit and resets the fade) is entered 0 times.

### 162.3 Next (the actual gate)

1. Take the emulator's pc set in the window **16.3 s -> 21 s** (unit 3 returned -> title), by breaking on
   `0x20444C` for mode 3 and sampling `pc` every ~30 ms for 5 s. That window is the title's construction.
2. Diff that set against the port's steady-state set above - the first emulator address the port never reaches
   is the gate (the port's loop is already known).
3. Keep `TOL_AUTO_RESUME` as a probe only; it is not a fix.

---

## Session 163 (2026-11-20) - the worker loop runs once in the port and 4047 times in the emulator

### 163.1 The worker thread's loop, and the counter that proves it is not running

`FUN_00198f38` (tid 6's body) is a work loop:

```
0x198FA0  ld  v0,(0x394340); v0++; sd v0,(0x394340)     <- iteration counter
0x198FAC  jal 0x1B7888                                   <- work fetch
0x198FB0  sw  $s1=1,(0x394314)  /  0x198FB4 sw $zero     <- busy heartbeat
0x198FBC  lw  v0,(0x394304); beq 1 -> SleepThread        <- optional frame sleep
0x198FD0  bnez s0 -> 0x19902C
0x198FD8  v0 = 0x3942C8; if ==1 -> ResumeIfSuspended(*(0x394370))
0x19902C  ld v0,(0x3943B8); beqz -> 0x198FA0             <- loop while the flag is 0
```

Measured, same globals both sides:

| address | port (`pf_1`, 49.9 s) | emulator (title) |
|---|---|---|
| `0x394340` iteration counter | **1** | **0xFCF (4047)** |
| `0x394314` heartbeat | 0 | 1 |
| `0x3943E8` | 0 | 1 |
| `0x3943B8` run flag | 0 | 0 |
| `0x394304` | 0 | 0 |

`FUN_001B7888` (the work fetch) is entered **once** in 60 s (`t=5.895 s`) and returns `v0=0` immediately. So the
thread starts its loop, comes round once, then sleeps and is never dispatched again.

### 163.2 Why sleeping is fatal here

`SleepThread` (0x32) blocks with `EeWaitState{EeWaitReason::Sleep, std::monostate{}}` - **no deadline**. Such a
thread only runs again on an explicit `WakeupThread`. The port's guest never issues one for tid 6 (the
`TOL_TRACE_SUSP` log shows only suspend/resume, and tid 4/5 similarly sit in `Waiting(sleep)` from 7 s to 80 s
with no state change at all).

The two writers of the run flag `0x3943B8` are `FUN_00199938` (0x19997C) and `FUN_00199AF0` (0x199B40, 0x199BAC).
`TOL_TRACE_PC=0x199AF0,0x199938` over 60 s: **0 entries**. So the port never calls the code that would tell the
worker "there is work" and wake it - which is consistent with unit 4 never starting (session 161.2).

### 163.3 Status of the logo goal

Not yet on screen. The chain is now: unit 3 finishes -> the app never starts the next unit / never calls
`FUN_00199AF0` -> the worker thread (which draws) stays in `SleepThread` -> nothing is submitted, the screen
keeps the leftover fade. `TOL_AUTO_RESUME` wakes it for one iteration only (it re-sleeps), so it is a probe, not
a fix.

### 163.4 Next

1. Find the caller of `FUN_00199AF0` / `FUN_00199938` (xrefs in Ghidra) and check which of their ancestors the
   port stops at - that is the same place unit 4 fails to start. With the flag set, the worker loop runs and the
   title content follows.
2. Keep using `pf_*` (0x390000..0x3A0000) dumps: that range holds this whole subsystem and now diffs cleanly.

---

## Session 164 (2026-11-20) - the gate is vtable slot +2 of the object at 0x63D890, and the port never calls it

Caller chain of the run-flag setters (Ghidra xrefs):

```
FUN_00199938 / FUN_00199AF0   (set 0x3943B8, resume the worker)
  <- FUN_00199988              ("release": 0x3942C4--; when 0 -> the above + ChangeThreadPriority(0x394368)
  |                             + FUN_001b7658/0x1b7670/0x1b79a0)
  |   <- FUN_0019a2b0          (a pure thunk, 0 static xrefs -> called through a pointer)
  <- FUN_0019a550              (second release variant, 0 static xrefs)
```

`FUN_00199538` (create worker) is the matching "hold": it creates tid 6 suspended and the count `0x3942C4`
stays 1 in **both** the port and the emulator at the sampled points, so the count is not the gate.

`TOL_TRACE_SUSP=6` (now filterable by id) over 40 s: exactly **one** suspend of tid 6 (t=5.435 s, from
`FUN_00199538`) and **no** resume. `TOL_AUTO_WAKE=6` cannot help - `wakeupThread` does not clear a *suspend*.

### 164.1 The vtable slot that is never called

`0x2062A0` (the top of the emulator's resume backtrace) occurs **once** in the image, as data at VA
**`0x003D47D8`** = `0x3D47D0 + 8`, i.e. **slot +2 of the vtable at 0x3D47D0**. So the emulator drives an object
whose vtable is `0x3D47D0`, and its slot +2 is the "start the next unit / reset the fade" callback.

And the port's object at `0x63D890` at steady state carries **exactly vtable `0x003D47D0`** with
`+0x14 = 0x0063DCF0`, `+0x18 = 0`, `+0x1C..+0x24 = 0` - i.e. it was created but never advanced (the emulator's
object there is the *other* type, vtable `0x003D4750`, `mode(+0x14) = 1`, `arg(+0x18) = 0x014E29E0`).

So the port builds the next-unit object and then never calls its slot +2, which is why `FUN_002062a0` (0x2062A0)
is entered 0 times, the worker is never resumed, and nothing is drawn. Two candidate causes remain:

1. the driver that dispatches slot +2 (the loader/app step) is itself not reached, or
2. the object's state field that gates the dispatch (likely `+0x14`, still a pointer instead of a mode) is never
   advanced because the preceding step failed.

### 164.2 Next

1. Find which function reads/fires `*(vtable+8)` for that object (Ghidra: xrefs of `0x3D47D0` as a value in
   memory, and the loader-step function that calls slot +2) and compare its entry in the port.
2. Compare `+0x14`/`+0x18`/`+0x1C..+0x24` of the object at `0x63D890` between the port's `pd_2.bin` and
   `emu_title.bin` at *the same* point: the port's are 0 where the emulator's hold a mode and three pointers, so
   the missing write is the "unit started" initialisation.

---

## Session 165 (2026-11-20) - the slot+2 dispatcher is the microkernel run loop; direct invocation is not enough

Deep backtrace at `ResumeThread(6)` in the emulator (40 frames) completes the chain that session 164 lacked:

```
0x114670 ResumeThread <- 0x199200 <- 0x198B20 <- 0x1B75D8 <- 0x259B20 <- 0x2540B0 <- 0x254030
   <- 0x2542D0 <- 0x3353C0 <- 0x2062A0 <- [0x151E90 run loop, pc=0x151FC8] <- 0x151FF0 <- 0x165AD0 <- crt0
```

So the object's **slot +2 (`vtable+8`) is fired by the guest microkernel run loop**, not by a C++ caller: the
callback is read live from the vtable at dispatch time. Confirmed by scanning RAM: the word `0x002062A0` occurs
**zero** times in `pd_2.bin`, `emu_title.bin` and `pf_1.bin` - the queue holds the *object*, not the function
pointer.

### 165.1 Direct invocation experiment (`TOL_CALL_PC=0x2062A0,0x63D890`)

Calling the callback once from the main loop after the units, with `a0 = 0x63D890`:

```
[callpc] fades=3 -> 0x002062A0 a0=0x0063D890 fn=...
[callpc] returned pc=0x0010EE5C v0=0x0000002C
```

`0x394340` (worker loop counter) went 1 -> 2, so the callback did reach the worker, but it left through a
syscall path (`pc=0x10EE5C`) instead of completing, and the run flag `0x3943B8` stayed 0 - no draws, no title.
Calling it out of the microkernel's own context is not a valid fix; it is only evidence that the callback is the
right one.

### 165.2 Status and next step

- The object at `0x63D890` (vtable `0x3D47D0`) is created and never advanced: `+0x14` holds a pointer, `+0x18`
  and `+0x1C..+0x24` are zero, where the emulator's carries a mode and three child pointers.
- Nothing in RAM holds `0x2062A0`, so the run-queue entry must be the object itself. Next: dump the microkernel
  run queue / ready lists (`0x407160` TCBs, `0x48F570` priority counts, `0x48F590` context slots) in the port's
  `pd_*` and `pf_*` and in `emu_title.bin`, and find which list entry carries the object at `0x63D890` in the
  emulator and is missing in the port.
- Alternative: instrument `FUN_00151E90` (dispatcher) in the port to print every dequeued item - the emulator runs
  `0x2062A0` from there at ~2/s, so the port's dispatcher log will show which item it runs instead.

---

## Session 166 (2026-11-20) - the microkernel table matches; the missing dispatch is a one-shot `vtable+8` call

New probe: `TOL_TRACE_SOURCE=0xADDR` (in `dispatchGuestBranch`) logs every transfer whose *source instruction*
is that address, with target and registers.

### 166.1 What the port's run loop actually dispatches

`TOL_TRACE_SOURCE=0x151FC0` (the microkernel's `jalr $v0`, 45 s):

```
#1 -> 0x00164F40 a0=0x00407950
#2 -> 0x00165210 a0=0x00407740
#3 -> 0x00165210 a0=0x00407B60
#4..#6 -> 0x00165210 a0=0x00407B60
```

Six dispatches in 45 s, all of them the TCB-index tasks 2/3/4 (`a0` is `TCB+0x1C0`). `0x2062A0` never.

### 166.2 The microkernel structures are identical

`sidestep` of the TCB table (`0x407160`, stride `0x210`) between `pd_2.bin` (port, 95 s) and
`emu_title.bin` (emulator, title):

| idx | entry | state/prio | next | port vs emu |
|---|---|---|---|---|
| 2 | `0x00165210` | 1/4 | `0x00407790` | identical |
| 3 | `0x00164F40` | 1/4 | `0x00407370` | identical |
| 4 | `0x00165210` | 1/4 | `0x00407580` | identical |

Priority counts `0x48F570` = `[2,0,0,0,3,0,0,0]` on **both** sides; context slots `0x48F590..` all zero on both.
So the port's microkernel is not missing a task entry - the difference is not a queue slot.

### 166.3 Refinement: `0x2062A0` is called *once*, not per frame

A breakpoint on `0x2062A0` gets **0 hits** at the title (two 20 s windows), while the earlier backtrace shows
it as the frame that calls `0x3353C0` (return address `0x2062D0` = inside `FUN_002062a0`). So it fires **once**,
at ~22 s, and that single call is what starts the chain that resumes the worker.

It has 0 static xrefs, the word appears in no RAM dump, and its only occurrence in the image is the vtable slot
`0x3D47D8` - so it is reached through a generic `lw t9,(a0); lw t9,8(t9); jalr t9` on an object of type
`0x3D47D0`. The port **has** such an object (`0x63D890`, vtable `0x3D47D0`) but never performs that dispatch,
which is exactly session 164's conclusion narrowed to a single call.

### 166.4 Next

1. Find the code that performs `vtable+8` on the `0x3D47D0` object: search the image for the constructor storing
   `0x3D47D0` into an object (that names the type), then for the caller that reads `+8`. In the port, that caller
   is the loader/app step that never runs for this object.
2. Because the emulator's call is one-shot at ~22 s, a breakpoint on `0x3353C0` with a condition on its caller
   gives the same information cheaply: `get_backtrace` there shows frame [9] only.

---

## Session 167 (2026-11-20) - PRECISE: TCB 4 (0x4079A0) is dispatched at 0x2062A0 in the emulator, never in the port

Breakpoint armed at boot on `0x2062A0` itself, hit at `t=25.79 s`:

```
0x2062A0  ra=0x00151FC8  a0=0x0063D890  a1=0x00000001  sp=0x01FFECE0  s0=0x004079A0
  [0] 0x2062A0
  [1] 0x151E90 (pc 0x151FC8)     <- microkernel `jalr` at 0x151FC0
  [2] 0x165AD0 (pc 0x165BA8)     <- game startup
  [3] crt0
```

**The item is TCB 4 = `0x4079A0`** (the "loader TCB" the port has been probing since session 63):

| field | value |
|---|---|
| TCB base | `0x4079A0` (index 4, `0x407160 + 4*0x210`) |
| dispatched pc (`TCB+0x1B4`) | `0x002062A0` |
| dispatch arg (`a0`, from `TCB+0x1C0`) | `0x0063D890` � the loader object, vtable `0x3D47D0` |
| `a1` | 1 |

So `0x2062A0` is **slot +2 (`vtable+8`) of the loader object, run as the loader task's own step**, and the
microkernel dispatches it as the resume pc of TCB 4. Sessions 165/166's "the microkernel never dispatches it" was
a sampling artifact: the previous windows armed the dispatcher breakpoint but filtered a run whose TCB 4 had not
advanced yet.

Port side, same TCB, measured:

- TCB 4 exists with `entry(+0x1B0) = 0x00165210`, state 1, prio 4, `alive(+0x1AC) = 0`, `chan(+0x1F1) = 4`;
- `TOL_TRACE_SOURCE=0x151FC0` dispatches it at `0x00165210` (the step wrapper) 5x in 45 s - **never** at
  `0x002062A0`, so its resume pc never advances past the wrapper;
- the object at `0x63D890` is present with vtable `0x3D47D0` but `+0x14/+0x18/+0x1C..+0x24` never initialised
  (session 164), i.e. the object's own state was never set up before the step was due.

`pd_2` vs `emu_title` TCB tables, priority counts and context slots are **identical**, so this is not a queue or
table difference: it is the loader step not advancing.

### 167.1 Next

1. Trace the loader task's step `0x165210` in the port against the emulator: it is dispatched at its entry each
   time in the port (resume pc never moves), which means each dispatch restarts the wrapper instead of resuming.
   Compare `TCB+0x1B4` writes: the emulator's reaches `0x2062A0`, the port's stays `0x165210`.
2. The step wrapper is already known from session 152 (`childStepEntryFix`, `0x3059B0`, step fragments at
   `0x165220`/`0x165400`); check whether this call site needs the same per-address treatment.

---

## Session 168 (2026-11-20) - TCB 4's continuation: port `0x165220` vs emulator `0x2057C8`, and the alive flag differs

New probe: `TOL_WATCH_WRITE=0xADDR` (32-bit stores, in `PS2Runtime::Store32`) prints every store into one guest
word with the writing pc and `$ra`.

### 168.1 The two differing words (same dumps, same decode)

TCB 4 = `0x4079A0`:

| field | port (`pd_1`/`pd_2`, 50 s and 95 s) | emulator (`emu_title`) |
|---|---|---|
| `+0x1AC` alive/yield flag | **0** | **1** |
| `+0x1B0` entry | `0x00165210` | `0x00165210` |
| `+0x1B4` resume pc | **`0x00165220`** | **`0x002057C8`** |
| `+0x1B8` | `0x00165250` | `0x00165250` |
| `+0x1C0` arg | `0x0063D890` | `0x0063D890` |
| `+0x1F0` state/prio | `0x01000401` | `0x01000401` |

So the loader task is the same task with the same entry and the same object argument; only the **carried
continuation** and the **alive flag** differ. The port's resume pc sits at the step wrapper's first body
instruction (`0x165220`) for the whole run, i.e. the wrapper restarts instead of resuming into the loader
object's step, which is where `0x2062A0` (vtable+8) lives.

### 168.2 The `+0x1C4` word is never written by a 32-bit store

`TOL_WATCH_WRITE=0x407B54` (= `0x4079A0 + 0x1B4`): **0 hits in 50 s**. The field is therefore written either as
part of an 8-byte store at `0x407B50` or only at task creation - which is consistent with "the port never
advances it".

### 168.3 Experiment: assert the alive flag (`TOL_POKE_ALIVE=1`)

`Store32(0x00407B4C, 1)` on every main-loop iteration (the microkernel keeps the task across a yield only when
`TCB+0x1AC != 0`). The run completed without new errors (only the known `0x301EB8` jalr-to-null), but the
verification dump used range `0x390000..0x3A0000`, which does **not** contain `0x4079A0` - so the effect is
**not yet measured**. Re-run with `TOL_DUMP_RAM=0x3E0000,0x2A0000,...` to read `+0x1AC`/`+0x1B4` after the poke.

### 168.4 Next

1. Re-measure the poke with a dump that covers `0x4079A0`, and check whether `+0x1B4` starts advancing past
   `0x165220` (then draws/appearance).
2. If it does not, trace the 8-byte store at `0x407B50` (add `+0x1B4` to `TOL_WATCH_WRITE` as a 64-bit watch) to
   see which continuation the wrapper writes, and compare with the emulator's `0x2057C8`.

---

## Session 169 (2026-11-20) - VERIFIED: the alive flag gates the continuation (0x165220 -> 0x204380)

`TOL_POKE_ALIVE=1` with a dump covering TCB 4 (`TOL_DUMP_RAM=0x3E0000,0x2A0000,...`):

| field | before (session 168) | **after the poke** | emulator |
|---|---|---|---|
| `+0x1AC` alive flag | 0 | **1** | 1 |
| `+0x1B4` resume pc | `0x00165220` | **`0x00204380`** | `0x002057C8` |

So `TCB+0x1AC` is exactly the gate: while it is 0 the microkernel does not carry the loader task across its
yield and the step restarts at the wrapper (`0x165220`); with it asserted the continuation advances into the
loader object's step code (`0x20xxxx`). The regime line now reads `loader state=1 chan=4 **alive=1**`,
`mainIters` keeps climbing, and the only error in the run is the known `0x301EB8` jalr-to-null (1 per run).

### 169.1 Still no picture at 72/87/102 s

Window capture with the poke asserted: still black (client area only). The task now progresses, but whatever it
does next (the loader object's step at `0x20xxxx`, which is where `0x2062A0` lives) has not produced a frame in
~100 s. Nothing has regressed: no new `[badpc]`, no missing target beyond the known one.

### 169.2 Next

1. Run with `TOL_POKE_ALIVE=1` **and** `TOL_TRACE_SOURCE=0x151FC0` to see whether the dispatcher now reaches
   `0x002062A0` (and at what rate); if yes, the remaining gap is time/one more load unit, not logic.
2. If it reaches `0x2062A0`, check `[gfx]` draws/textures in the same run; the fix then becomes "assert
   `+0x1AC` at task start in the step wrapper" rather than a host poke.

---

## Session 170 (2026-11-20) - with the poke: continuation advances, dispatcher still never reaches 0x2062A0, no draws

Run: `TOL_POKE_ALIVE=1 TOL_TRACE_SOURCE=0x151FC0 TOL_GFX_STATS_EVERY=120`, 110 s.

Dispatcher (`0x151FC0`), all targets:

```
0x00165210  6x   (first t=24.86s, a0=0x00407740)
0x00164F40  1x   (first t=24.86s, a0=0x00407950)
```

**`0x002062A0` is never dispatched**, even though `TCB4+0x1B4` was measured at `0x00204380` in the same
configuration (session 169). Graphics, unchanged from the baseline:

```
[gfx] 22.8 fps | transfers=0 draws=480/120 frames (4/frame) passes=240 | tex builds=0 gif packets=720
```

### 170.1 What that means

`+0x1AC` (alive) does gate the *carry* of the task - `+0x1B4` moves from `0x165220` to `0x204380`. But the
microkernel still dispatches the wrapper `0x165210`, so **the pc the dispatcher uses is not `TCB+0x1B4`** (or
not only it): it is whatever `$v0` was loaded from in the run loop, and in the emulator that read produced
`0x2062A0` with `s0 = 0x4079A0`.

So the next measurement must be the operand, not the guess: read the whole TCB 4 field block (`+0x1B0..+0x1C8`
and `+0x1F0..+0x1FC`) **at the emulator's `0x2062A0` dispatch hit** and compare it with the port's same block at
a dispatch of `0x165210`. Whichever word holds `0x2062A0` there is the field the run loop reads; the port's copy
of that word is the one to fix.

### 170.2 Also worth noting

`+0x1B4 = 0x204380` in the port sits in the same `0x20xxxx` code region as the emulator's `0x2057C8`, so the
task's own resume state is plausible - the gap is that the dispatcher keeps entering it at the wrapper entry
instead of that resume point.

---

## Session 171 (2026-11-20) - the dispatcher is identical after all; the gap is INSIDE the step (0x165210 -> 0x2062A0)

### 171.1 TCB 4 block, full compare (port `pd_2` vs emulator `emu_title`)

```
** +1AC 0x00000000  0x00000001     (alive)
   +1B0 0x00165210  0x00165210     (entry, identical)
** +1B4 0x00165220  0x002057C8     (resume pc)
   +1B8 0x00165250  0x00165250
   +1BC 0x00000000  0x00000000
   +1C0 0x0063D890  0x0063D890     (object arg)
   +1C4 0x00000000  0x00000000
   +1F0 0x01000401  0x01000401
   +1F4 0x00000000  0x00000000
   +1F8 0x00407580  0x00407580
   +1FC 0x00000000  0x00000000
```

Only `+0x1AC` and `+0x1B4` differ - no other field of the TCB is involved.

### 171.2 Correction: the run loop dispatches the SAME pc on both sides

At the emulator's `0x2062A0` entry: `ra=0x00151FC8  v0=0x00165210  a0=0x0063D890  a1=0x00000001  s0=0x004079A0`.

`v0` is `0x165210`, not `0x2062A0` - so the microkernel calls **the same wrapper `0x165210` the port calls**,
and `0x2062A0` is reached *inside* that call with `(a0 = object 0x63D890, a1 = 1)`. Session 167's reading of
`+0x1B4 = 0x2062A0` as "the dispatched pc" was wrong; `+0x1B4 = 0x2057C8` is simply where the step was when the
dump was taken.

So: the microkernel, the TCB and the dispatch are all correct in the port. **The divergence is inside the loader
step**: the wrapper starts, the emulator's copy walks into the object's step and calls slot +2 (`0x2062A0`), the
port's copy stays at the wrapper body (`0x165220`) and restarts.

### 171.3 Out-of-band call with the right arguments still fails

`TOL_CALL_PC` now takes `addr,a0,a1`; calling `0x2062A0` with `(0x63D890, 1)` after the units:

```
[callpc] -> 0x002062A0 a0=0x0063D890 a1=0x00000001
[callpc] returned pc=0x0010EE5C v0=0x0000002C
```

It leaves through the syscall path again, so invoking the callback from the main loop is not a valid fix - it
must run inside its own task/stack. Graphics unchanged: `draws=480/120 frames, transfers=0, tex builds=0`.

### 171.4 Next (the last known gap)

Instrument the **inside** of the step: `0x165210 -> 0x165220 -> 0x165400 -> ...` in the port and compare with the
emulator's sequence up to the `0x2062A0` call. The port's copy exits/restarts at `0x165220`, and session 152
already fixed the sibling case (`childStepEntryFix` at `0x3059B0`) with a per-address step override - the same
treatment belongs here.

---

## Session 172 (2026-11-20) - the step does run in the port; the missing call is inside 0x165400

`TOL_POKE_ALIVE=1 TOL_TRACE_PC=0x165210,0x165220,0x165400 TOL_WATCH_TARGET=0x2062A0`, 40 s:

```
0x00165210  entries=2  first t=39.344s  a0=0x00407740  ra=0x00151FC8
0x00165400  entries=2  first t=39.344s  a0=0x0063D850  ra=0x00165230
0x2062A0    target hits = 0
```

So the loader step is **not** dead: the microkernel dispatches `0x165210`, the wrapper enters `0x165400` with the
job object `0x63D850`, all at 39.3 s - and the step still never calls the loader object's slot +2
(`0x2062A0`, object `0x63D890`). The missing dispatch is therefore **inside `FUN_00165400`** (one function), not
in the scheduler, not in the TCB, not in the microkernel.

### 172.1 No fix was applied, deliberately

`+0x1AC` is written by the guest's own task-start code; the port leaves it 0, so the guest never sets it - which
is what the poke proved by substituting for it. A host poke is a diagnostic, not a fix, and a blind override inside
`0x165400` risks the loader path that currently works (three load units, scene start, 110 MB of transfers). The
next step needs one decompile of `FUN_00165400` to see which vtable slot it fires for the job's loader object, and
that has to come before touching it.

### 172.2 Order for the next session

1. Decompile `FUN_00165400` (job `0x63D850` -> loader object `0x63D890`) and list the slots it calls; the emulator
   calls slot +2 with `(a0=0x63D890, a1=1)`.
2. Compare that with what the port actually calls at 39.3 s (same probe, add the slot addresses).
3. Only then write the per-address override, in the shape of session 152's `childStepEntryFix`.
4. Fix `+0x1AC` at its real writer instead of poking it from the main loop.

---

## Session 173 (2026-11-20) - correction: 0x165400 fires slot +0xC, not +2

Decompiled `FUN_00165400`:

```c
void FUN_00165400(int param_1)
{
  (**(code **)(*(int *)(param_1 + 0x10) + 0xc))();   /* vtable slot +0x0C of the object at job+0x10 */
}
```

So the loader step's `0x165400` is the **slot +0x0C** dispatcher (that is the unit callback `0x204240`, whose
three runs the port already performs). It is **not** the slot +2 dispatcher, so session 172's "the missing call is
inside FUN_00165400" is wrong for slot +2.

Which leaves the emulator's own evidence as the only guide to slot +2: at the `0x2062A0` hit
`ra=0x00151FC8, v0=0x00165210, s0=0x004079A0, a0=0x0063D890, a1=1` - i.e. the microkernel called `0x165210`, and
`0x2062A0` is reached *deeper in that same call* with `$ra` still `0x151FC8`, so it is reached by a **tail
transfer** (`j`/`jr`) from the step chain, not by a `jal` from `0x165400`.

Combined with the resume backtrace (`0x2062A0 -> 0x3353C0 -> 0x2542D0 -> 0x254030 -> 0x2540B0 -> 0x259B20 ->
0x1B75D8 -> 0x198B20`), slot +2 is entered **from inside `FUN_002062a0` itself**, not into it: `0x2062A0` is the
slot+2 *entry*, and everything after it is what the emulator does once the unit starts.

### 173.1 What that means for the fix

The port reaches `0x165210` and `0x165400` (slot +0xC) but never `0x2062A0`. Since `0x2062A0` is the slot+2
entry and it is reached with `ra` preserved from the microkernel `jalr`, the tail transfer that reaches it comes
from a path the port's step does not take - the next measurement is the tail transfers **out of** the step:
`TOL_TRACE_SOURCE` on the step's tail instructions, or `TOL_WATCH_TARGET=0x2062A0` in the *emulator* (which the
port probe already shows as 0 hits, so the emulator side will name the source instruction that performs it).

Do not patch `0x165400`; it is the unit callback and it works.

---

## Session 174 (2026-11-20) - SOURCE IDENTIFIED: `j 0x1653F0` at 0x165210; the port starts at the *other* entry (0x165220)

Static scan for the `lw t9,8(t9); jr t9` pattern in the image found five sites; the relevant one is `0x1653F4`,
and its context is decisive:

```
0x1653F0  lw t9, 0x10($a0)      ; object = *(a0 + 0x10)
0x1653F4  lw t9, 8($t9)         ; vtable slot +8   <-- the slot we were hunting
0x1653F8  jr $t9                ; -> 0x2062A0
0x165400  lw t9, 0x10($a0)
0x165404  lw t9, 0xC($t9)       ; slot +0x0C       (the unit callback 0x204240)
0x165408  jr $t9
```

Ghidra xrefs of `0x1653F0`: exactly **one** - an unconditional jump from **`0x165210`**.

And `0x165210` is:

```
0x165210  j    0x1653F0         ; tail-jump to the slot+8 thunk
0x165214  lw   $a0, ($a0)       ; delay slot: a0 = the object
```

while the *next* entry is a different function body:

```
0x165220  addiu sp,-0x10 ; sd ra,(sp)
          jal  0x165400         ; slot +0x0C  (unit callback)
          jal  0x15231C         ; yield
          ... jr ra
```

So the loader task has two entry points, and they are **not** interchangeable:

| entry | what it does |
|---|---|
| `0x165210` | slot **+8** -> `0x2062A0` ("start the next unit / show the screen") |
| `0x165220` | slot **+0x0C** -> `0x204240` (unit fade) + yield |

Measured: the port's `TCB4+0x1B4 = 0x00165220`, the emulator's is `0x002057C8` (deep in the object step), and the
port's step trace shows `0x165210` entered with `a0=0x00407740` but `0x165400` called from `ra=0x00165230` - i.e.
the port runs the **0x165220** body. That is the whole defect: the loader task is started/resumed at `0x165220`,
so the slot+8 entry `0x165210` never runs, `0x2062A0` is never called, and no frame is produced.

### 174.1 The fix (now a one-line target)

The task's start/resume pc must be **`0x165210`**, not `0x165220`. That is what the guest's own task-start code
sets in the emulator (and why `+0x1AC` is 1 there). Concretely:

1. find the writer of `TCB4+0x1B4` that stores `0x165220` (the guest's step-start code, `0x1518C0`/`0x151CD0`
   family) and compare it with the emulator, or
2. as a verified-later probe, override the entry so the task starts at `0x165210`.

Do not patch `0x165400` (slot +0x0C - it works) and do not poke `+0x1AC` in production (diagnostic only).

---

## Session 175 (2026-11-20) - TCB4+0x1B4 has NO Store32/Store64 writer in 45 s (block copy)

`TOL_WATCH_WRITE=0x407B54` now watches both 32-bit stores (`Store32`) and 64-bit stores (`Store64`, matching when
the watched word falls inside `[vaddr, vaddr+8)`). 45 s run: **0 hits from either**, while `+0x1B4` reads
`0x00165220` in every dump.

So the field is not written by generated `WRITE32/WRITE64` at all: it is initialised by the task-creation path as
part of a **block copy** (`memcpy`/128-bit stores) or by a store that happens before the first watched call. That is
why the earlier 32-bit-only watch was also empty - it was not a lost write, it is a different writer class.

### 175.1 What that means for the fix

The value `0x165220` in `TCB4+0x1B4` comes from the microkernel's create/start path (`0x1518C0` / `0x151CD0`
family), not from the step. To fix the task entry at its actual writer:

1. break in the **emulator** on the create call with `0x4079A0` (or on `0x151CD0` with `a0 = 0x4079A0`), and read
   which pc/arg it stores into `+0x1B4`; the emulator's task starts at the `0x165210` entry.
2. in the port, watch `Store128`/the `memcpy` path for `0x407B54` (the block copy), or hook the create function
   (`0x151CD0`) and print its arguments - that names the writer directly.
3. only then change the entry (`0x165210` instead of `0x165220`) and the `+0x1AC` alive byte at that same site.

No production code was changed this session: the two candidate writers are both unproven, and the loader path that
works (three units, scene start, 110 MB of transfers) must not be disturbed by a guess.

---

## Session 176 (2026-11-20) - the port creates 3 microkernel tasks; the emulator creates 4 (the 4th is 0x2E09E0)

Traced the task-create path on both sides (`0x151CD0` = create, `0x1518C0` = start):

Emulator:

```
t=5.15s  0x151CD0 a0=0x001F8150 a1=0 a2=1 ra=0x1F8294      (GS worker 1)
t=5.28s  0x151CD0 a0=0x001F84B0 a1=0 a2=0 ra=0x1F82C0      (GS worker 2)
t=5.42s  0x151CD0 a0=0x001660A0 a1=0 a2=1 ra=0x166050
t=9.32s  0x151CD0 a0=0x002E09E0 a1=1 a2=0 ra=0x2E1FC4      <-- MISSING IN THE PORT
  inner 0x151B4C slots: a0=8/0/1/3   records a2 = 0x407360/0x407570/0x407990/0x407DB0
```

Port:

```
7.548s  0x151CD0 a0=0x001F8150
7.548s  0x151CD0 a0=0x001F84B0
7.595s  0x151CD0 a0=0x001660A0
counts: 0x151CD0 = 3   (emulator = 4)
```

So the port creates only **three** of the four tasks. The fourth, entry `0x002E09E0` created from `ra=0x002E1FC4`
on the main thread, is exactly the piece that has been missing at every level above: without it nothing drives the
later unit, so `0x2062A0` (slot +8) is never called.

The TCB record base is `0x407990` for the third task (`0x4079A0` = record + 0x10), which is TCB 4 - matching the
`+0x1B0 = 0x165210` we compared, so the *entry* field is already correct on both sides; it is the missing fourth
task, not the entry value, that stops the chain. (That also explains session 175: `+0x1AC`/`+0x1B4` are written by
the create/start block copy inside `0x1518C0`/`0x151CD0`, not by generated stores.)

### 176.1 Next

1. Probe `0x002E1FC4` (the caller of the 4th create) and its callers in both sides; the port never reaches it, so
   the first ancestor that is skipped is the gate - the same bisect that worked for the dispatcher.
2. `0x2E09E0` sits in the `0x2E08xx` family already documented in session 121 (job table / `FUN_002E0820`), so the
   gate is likely the same job chain that was being investigated then.

---

## Session 177 (2026-11-20) - the whole 0x2E08xx job family is dormant in the port

Same four addresses probed on both sides.

Emulator (t = 9.25 ... 9.7 s, main thread stack `0x423450`):

```
0x002E1FC4  a0=0x00407BB0 a1=0x00407D68 a2=0x00407D64   (interior pc: the call site of the 4th create)
0x002E09E0  a0=0x00407D70 ra=0x00151F88                  <- the 4th task starts, dispatched by the microkernel
0x002E0820  a0=0 a1=0x0063E450 a2=1 ra=0x003659BC        <- job-table poll, 4 calls in 0.5 s
```

Port: **0 entries for all four** (`0x2E1FC4`, `0x2E0820`, `0x2E0880`, `0x2E09E0`) over 40 s.

So it is not one missing call: the port never enters the job family at all in this phase, while the emulator both
starts the 4th task *and* polls the job table (`0x2E0820`) from `ra=0x3659BC` four times in half a second.

Note `0x2E1FC4` is an **interior** pc (the create's return address), so probing it as an entry cannot fire on
either side; the 0-entry result for it is not evidence. `0x2E0820` / `0x2E09E0` are real entries and are the two
that matter - both dead in the port.

### 177.1 Next (one probe, then the fix site is known)

1. Probe the **function entry containing `0x2E1FC4`** in the emulator with `get_backtrace` at its hit: that names
   the app-side caller of the 4th create (the port's missing ancestor).
2. Probe `0x003659BC`'s containing function (the job-table poll caller) in the port - if it is dead too, the gate
   is the loop that drives both, i.e. the AppCell/loader job pump, and that is where the override belongs.

---

## Session 178 (2026-11-20) - CORRECTION: the port runs the same app-job step and the same create call; 177 was a window artifact

Both probed at their function entries.

Emulator, `FUN_002e1f80` (contains `0x2E1FC4`), backtrace at the hit:

```
0x002E1F80  ra=0x00365840  a0=0 a1=1 a2=1
  <- 0x00365700 (FUN_00365700, pc 0x365840)      <- app job / AppCell step
  stack: 0x63E3D0, 0x63DA00, 0x63D8D0, 0x3661C0, 0x165230, 0x151F88
```

Port, same two entries (`TOL_TRACE_PC=0x2E1F80,0x365700`, 40 s):

```
0x00365700  x10   a0=0x0063E3D0 a1=0x0063E3E8 a2=1 a3=0x0063DA00 ra=0x0036567C   (t=38.62s)
0x002E1F80  x1    a0=0x00000000 a1=0x00000001 a2=0x00000001 ra=0x00365840        (t=38.64s)
```

Identical call, identical arguments, same caller `0x365840` - the port's app job step **does** run and **does**
reach the create site. Session 177's "the port creates 3 tasks, the emulator 4" was a **sampling artifact**: that
probe ran only 30 s, while the port's 4th create happens at **38.6 s** (its boot is ~4x slower than the emulator's
9.25 s).

So nothing is skipped in the create path. The open question narrows to what happens **after** the create: whether
`0x2E09E0` is started and dispatched as a microkernel task in the port.

### 178.1 Next (90 s windows, not 40)

1. `TOL_TRACE_PC=0x151CD0,0x2E09E0` for 90 s: confirm the 4th create (`a0=0x2E09E0`) happens and whether the task
   is then dispatched at its entry.
2. If created but never dispatched, compare its TCB (`entry`, `+0x1AC`, `+0x1F0` state/prio) with the emulator's
   fourth task - that is the last mechanical difference in this chain.

---

## Session 179 (2026-11-20) - CONFIRMED: the fourth task IS created and dispatched in the port (identical args); 176/177 retracted

95 s run, `TOL_TRACE_PC=0x151CD0,0x2E09E0`:

```
7.280s  0x151CD0 #1 a0=0x001F8150
7.280s  0x151CD0 #2 a0=0x001F84B0
7.328s  0x151CD0 #3 a0=0x001660A0
37.337s 0x151CD0 #4 a0=0x002E09E0 a1=1 a2=0 ra=0x002E1FC4 sp=0x00423450   <- 4th create
37.371s 0x002E09E0 #1 a0=0x00407D70 ra=0x00151F88 sp=0x00417550           <- dispatched by the microkernel
```

Emulator, same two events: create at 9.25 s (`a0=0x2E09E0 a1=1 a2=0 ra=0x2E1FC4`), dispatch at 9.34 s
(`a0=0x407D70 ra=0x151F88`). **Identical arguments, identical caller, identical dispatcher** - the port is just
~4x later (37.3 s vs 9.3 s).

Fourth task's TCB = index 5 = `0x407BB0` (`a0` at dispatch is `TCB+0x1C0`):

| field | port (`f4_1`, 87 s) | emulator (`emu_title`) |
|---|---|---|
| `+0x1AC` | 0 | 0 |
| `+0x1B0` entry | 0 | 0 |
| `+0x1B4` resume pc | `0x002DF7F4` | `0x00233FEC` |
| `+0x1F0` state/prio | `0x01000A00` | `0x01000A00` |

Both alive, same state/priority; only the resume pc differs, which is expected from different progress points.

### 179.1 Retraction and what is actually left

Sessions 176 and 177 concluded "the port never creates the 4th task / never enters the 0x2E08xx family". Both used
windows shorter than the port needs (30-40 s of a 4x-slower boot). At 95 s the port does exactly what the emulator
does. The line is retracted.

Every control-flow link measured today is now identical on both sides: microkernel dispatch (`0x165210`), app job
step (`0x365700`, `a0=0x63E3D0`), create site (`0x2E1F80`, `a0=0,a1=1,a2=1`), 4th create and dispatch, TCB table,
priority counts, context slots. The only measured difference left is **speed** (~4x) plus the resume-pc values that
follow from it.

### 179.2 Next

Measure with the 4x factor in mind: run the port 200 s and capture the window at 150 / 180 / 200 s (guest ~40 s,
past the emulator's title point). If it is still black then, the divergence is downstream of the 4th task's own
progress and must be found by comparing that task's `+0x1B4` chain, not by any of the links retired above.

---

## Session 180 (2026-11-20) - 200 s run: still black with the task carried; divergence is downstream of the 4th task

`TOL_POKE_ALIVE=1`, captures at 150 / 180 / 200 s: **all black** (client area only, no draw content).

So with the loader task carried (session 169: `+0x1B4` advances to `0x204380`, `alive=1`), and with every
control-flow link verified identical to the emulator (sessions 178/179), the port still produces no frame at
guest ~40 s - past the emulator's title point (~21 s guest).

### 180.1 Where the next measurement goes

The 4th task's resume chain: at comparable progress the port's `TCB 5 (0x407BB0) +0x1B4 = 0x002DF7F4` and the
emulator's = `0x00233FEC`. Neither is the title loop, so both are mid-flight; the useful comparison is the
**sequence** of `+0x1B4` values (not one sample), aligned on the same guest milestone:

1. `TOL_WATCH_WRITE` on `TCB5+0x1B4` (`0x407D74`) in the port, 200 s window - the writer is the microkernel save
   path (`0x15231C`), so this gives the full resume-pc history.
2. In the emulator, break on the same save path for that TCB and log `+0x1B4` per yield for the same span.
3. The first guest function the emulator's sequence reaches and the port's does not is the remaining divergence.

Everything above that point has been retired with measurements, so this is the only layer left.

---

## Session 181 (2026-11-20) - TCB5's resume pc is PINNED at 0x2DF7F4 in the port (50 s -> 175 s)

Port, `TOL_POKE_ALIVE=1`, 200 s, dumps every 25 s, TCB5 struct `0x407BB0`, `+0x1B4` = `0x407D64`:

```
sq_0 (t=  5s)  +0x1B4=0x00000000
sq_1 (t= 30s)  +0x1B4=0x00000000
sq_2 (t= 55s)  +0x1B4=0x002DF7F4
sq_3 (t= 80s)  +0x1B4=0x002DF7F4
sq_4 (t=105s)  +0x1B4=0x002DF7F4
sq_5 (t=130s)  +0x1B4=0x002DF7F4
sq_6 (t=155s)  +0x1B4=0x002DF7F4
```

So the 4th task starts (`0x2DF7F4` first appears between 30 s and 55 s) and then **never advances its resume pc
again for the next 120 s**. That is the stall, and it is one function: `0x2DF7F4`.

Emulator side: the poll in this session used `0x407D74` - off by `0x10` from the correct `0x407D64` - and returned 0,
so that measurement is invalid. The only valid emulator datum remains the title-time dump: `TCB5+0x1B4 = 0x00233FEC`.

### 181.1 Next (single item)

1. Re-poll the emulator at **`0x407D64`** (correct address) and collect its `+0x1B4` history.
2. Disassemble `0x2DF7F4` in the port's sense: it is where the 4th task idles; the emulator's value (`0x233FEC`) is
   in a different function, so the first emulator-side value after the same guest milestone names what the port
   should be doing instead.

Everything else in this chain has been measured and retired; this is the last known difference.

---

## Session 182 (2026-11-20) - TCB5 has no per-yield sequence; each side sits in a different wait loop

Emulator, poll of `0x407D64` (correct address, fresh boot):

```
t=0.02s  +0x1B4 = 0x00000000
t=9.21s  +0x1B4 = 0x00233FEC
distinct values: 2        (unchanged for the remaining ~60 s)
```

Port (session 181): `0` until ~50 s, then `0x002DF7F4` unchanged for 120 s.

So `+0x1B4` is **not** a per-yield history on either side - it is a steady-state value that only changes when the
task is switched out. Both sides pin; they pin in **different functions**:

| side | resume pc | containing function |
|---|---|---|
| emulator | `0x00233FEC` | `FUN_00233fc0` (72 bytes) - the op-channel poll loop documented in session 152 |
| port | `0x002DF7F4` | `FUN_002df6d0` (1732 bytes) at `+0x124` |

So the fourth task exists, is dispatched, and idles - but in a different loop than the emulator's.

### 182.1 Next

Disassemble `FUN_002DF6D0 +0x124` (what the port's 4th task waits on) and compare with `FUN_00233FC0` (already
known: poll `0x126248` then yield). If the port's loop is a wait that never completes, that is the last mechanical
difference; if it is the same shape, the task is simply waiting for work that the loader never posts, and the
producer - not the waiter - is the thing to fix.

Everything above (dispatch, create, TCB fields, job step) is measured and identical.

---

## Session 183 (2026-11-20) - the two loops identified: emulator's 4th task polls 0x126248; the port's never gets there

`FUN_00233FC0` (72 B), the emulator's steady state for TCB5:

```
0x233FC8  sw zero,0x1C(sp)
0x233FCC  a0=1 ; a1=0 ; a2=sp+0x1C
0x233FD4  jal 0x126248          <- channel read (1, 0, &out)
0x233FDC  bnez v0 -> 0x233FF8   <- got data: return it
0x233FE4  jal 0x15231C          <- yield
0x233FEC  b 0x233FD0            <- loop  (this is the pinned pc)
```

So in the emulator the 4th task is a **worker poller** on the CRI/stream channel `0x126248` - a normal idle loop,
exactly the function session 152 documented and already fixed once (`criEndFunctionFix`).

The port's pinned pc `0x2DF7F4` is `+0x124` inside `FUN_002DF6D0` (1732 B, branch-heavy code around
`0x2DF7D0`: `b 0x2DFD64`, `lw v0,0xD0(sp)`, `sh v1,(v0)`) - **not** a poll loop. The port's 4th task therefore
never reaches its poll function; it is stuck earlier, inside `FUN_002DF6D0`.

### 183.1 Decision

This is the "same shape or not" question answered: **not the same**. The task is not waiting for work in the port;
it never arrives at the wait. So the fix target is the path that should lead into `FUN_00233FC0`, i.e. whatever
`FUN_002DF6D0` is doing when it stalls at `+0x124`.

### 183.2 Next probe (one)

Trace `FUN_002DF6D0` entry and its calls in the port (`TOL_TRACE_PC=0x2DF6D0` plus its callees as they appear),
and compare with the emulator's sequence for TCB5 before it settles in `FUN_00233FC0`. The first callee the
emulator runs and the port does not is the gate.

---

## Session 184 (2026-11-20) - the port calls FUN_002DF6D0; the emulator never does

| side | `0x2DF6D0` |
|---|---|
| emulator (armed from boot) | **0 calls** |
| port (200 s) | 1 call, t=22.927 s, from `ra=0x002DF62C`, `a0=0 a1=0 a2=0x41745C a3=0x3E5DC0`, and it exits with `pc=0x00116A6C` (a kernel wrapper), not back to its caller |

So the port's fourth task runs a function the reference never runs, and that call does not return normally - it
hands control to a syscall path (`pc=0x116A6C`). The caller is `ra=0x2DF62C` (inside `FUN_002DF5xx`).

That also explains the "pinned `+0x1B4 = 0x2DF7F4`": the task is suspended inside that call, so the value never
moves - it is a fibre suspension, not a spin.

### 184.1 Next (one disassembly, then the fix)

Disassemble the caller around `0x2DF62C` and read the branch that selects `0x2DF6D0`:
in the emulator that branch must select the other path (the one that leads to `FUN_00233FC0`'s poll on `0x126248`).
The condition register/flag that differs between the two runs is the last gate in this chain, and the fix belongs
at that branch - an override in the `childStepEntryFix` shape, or a data correction if it is a flag.

---

## Session 185 (2026-11-20) - the gate is an OPCODE: the port dispatches `-0xF3` into FUN_002DF6D0

Caller disassembly around the call site `0x2DF62C`:

```
0x2DF5E4  beq $s2, -0xF4  -> 0x2DF640
0x2DF5F0  beq $s2, -0xF2  -> 0x2DF640
0x2DF5FC  beq $s2, -0xF3  -> 0x2DF624      <-- the path the port takes
0x2DF604  beql $s2, 0     -> 0x2DF614   (lbu $v1,($s0))
0x2DF60C  b   -> 0x2DF640
0x2DF614  v0=1 ; v1 &= 1 ; beq -> 0x2DF6B0
0x2DF624  jal 0x2DF6D0      ; a0 = s3
0x2DF62C  s2 = v0 ; bltz -> 0x2DF6B0
```

So `$s2` is a command opcode and `-0xF3` (0xFFFFFF0D) is the only value that routes into `FUN_002DF6D0`. The port
called it at t=22.927 s, therefore **the port's `$s2` was `-0xF3`** at that dispatch. The emulator never calls
`FUN_002DF6D0`, so at the same point its `$s2` is a different opcode (or the dispatcher is not reached with -0xF3).

This is the last gate in the chain, and it is a value, not a control-flow defect: a command word differs.

### 185.1 Next

1. Find where `$s2` is loaded in function `FUN_002DF5xx` (the switch head) - it comes from the request/message the
   function is processing.
2. Compare that request's opcode in the port and the emulator at the same guest milestone: the port delivers
   `-0xF3`, the emulator something else. The producer of that field is the fix site (a data write or the code that
   fills the request).

---

## Session 186 (2026-11-20) - the `$s2` producer is not in the window scanned; function extent needed

The opcode dispatcher's switch head is above `0x2DF5E0`; a scan of `0x2DF520..0x2DF5E8` shows no write to `$s2`
(only `jal 0x2DF0B0` and `move $v0,$s2`), so `$s2` is loaded earlier in that function. The next probe is a full
disassembly of the function containing `0x2DF5E0` (`tools/map_lookup.ps1 0x2DF5E0` for its extent, then
`tools/disasm_guest.py <elf> <start> <end>`), grepping for writes to `$s2` - that names the field and the
instruction that fills it, i.e. the producer of the `-0xF3` opcode.

State at the end of this thread (all measured, none guessed):

- control flow identical to the emulator at: microkernel dispatch (`0x165210`), app-job step (`0x365700`),
  4th create + dispatch (`0x2E1F80` / `0x2E09E0`), TCB table, priority counts, context slots;
- the 4th task differs in *where it waits*: emulator polls `0x126248` in `FUN_00233FC0`; the port enters
  `FUN_002DF6D0` because its dispatcher receives opcode `-0xF3` and exits through `pc=0x116A6C`;
- remaining unknown: the producer of that opcode (this section's probe).

---

## Session 187 (2026-11-20) - FOUND IT: the job-table request code differs (0x0D vs 0x0E)

`FUN_002DF590` loads `0x4E9140` in its first two instructions (`lui 0x4F; addiu -0x6EC0`) - it is the **job table**
dispatcher. And `s2` (the opcode that routed into `FUN_002DF6D0`) is `-0xF3` = `0xFFFFFF0D`, i.e. `0x0D - 0x100`.

Job table `0x4E9140`, entry 0, from `pd_2.bin` (port) vs `emu_title.bin`:

| word | port | emulator |
|---|---|---|
| `+0x0` | `0x00000001` | `0x00000000` |
| `+0x4` | `0xFF0D0001` | `0xFF0E0000` |
| `+0x8` | `0x00000002` | `0x00000002` |
| `+0xC` | `0x00002000` | `0x00000000` |
| `+0x10` | `0x00000001` | `0x00000000` |
| `+0x14` | `0x00623100` | `0x00623100` |

The request code byte is **`0x0D` in the port and `0x0E` in the emulator** (the `-0xF3` the dispatcher received is
`0x0D - 0x100`). Entry state differs too (`1` vs `0`), and `+0xC`/`+0x10` differ - the port's entry 0 is one step
further along, with the wrong request id.

So the whole chain resolves to this: the port queues job request **0x0D**, the emulator queues **0x0E**. Every layer
above is correct; the wrong id sends the 4th task into `FUN_002DF6D0` instead of the poll path, and the screen stays
black.

### 187.1 The fix

Find the writer of the request byte at `0x4E9140 + 4` (byte 1) and the state at `+0` - i.e. the job submit call that
enqueues entry 0 - and compare its argument with the emulator's. Correcting the id (0x0D -> 0x0E) is a one-value
fix, and it is the only place left where the two runs disagree.

---

## Session 188 (2026-11-20) - the job-id fix is applied and fires; the screen is still black (single-value fix insufficient)

`TOL_JOBFIX=1` (new, opt-in, in the main-loop probe) rewrites the job-table request id as soon as it appears:

```
[jobfix] job[0] request id 0x0D -> 0x0E
```

Run with `TOL_POKE_ALIVE=1 TOL_JOBFIX=1`, captures at 72 / 87 / 102 s: **still black**. Guest 99.3 % busy,
`loader state=1 chan=4 alive=1`, `mainIters` climbing, one known `0x301EB8` jalr-to-null.

So the request id alone is not the whole difference. Comparing `job[0]` again after the fix, three other words are
still one step ahead of the emulator:

| word | port | emulator |
|---|---|---|
| `+0x0` state | `0x00000001` | `0x00000000` |
| `+0xC` | `0x00002000` | `0x00000000` |
| `+0x10` | `0x00000001` | `0x00000000` |

i.e. the port's job 0 is *already running/finished* where the emulator's is idle - the entry's lifecycle is offset,
not just its id.

### 188.1 Where this leaves the work (next session, in this order)

1. **Speed first**, because it is what makes every measurement ambiguous (and it is why two of my readings were
   invalid): the port reaches the 4th create at 37 s vs the emulator's 9.25 s. Measure per-phase wall time
   (`TOL_TURN_COST` + the `[turncost]` line) across boot -> unit1 -> unit4 and attack the largest block.
2. Then re-compare the full `job[0]` lifecycle (`+0x0`, `+0x4` id, `+0xC`, `+0x10`) at matched milestones; the
   port is one state ahead, so something submits/consumes that entry early.
3. Keep `TOL_JOBFIX` as a probe until (2) shows the entry matches end to end.

---

## Session 189 (2026-11-20) - SPEED MEASURED per phase: wall time is guest execution, and the 36 s pre-create block is a guest spin whose waits are metered by host throughput; job[0] is one lifecycle step ahead at unit 1 ENTRY and is load-bearing

### 189.1 Instrument: two additions, both free when unused

| tool | where | what it adds |
|---|---|---|
| `[turncost] ... cyc=%llu dcyc=%llu g/s=%.3f` | `ps2_runtime.cpp` (presenter stats, 5x/s) | the emulated EE cycle counter and guest-seconds per wall-second. Zero guest cost: reads `eeScheduler().eeCycle()`, no per-branch work. |
| `TOL_XFER_WALLPUMP=<us>` | `ps2_runtime.cpp` (host loop) + `checkpointDue` (EE thread) | sets a flag on a wall-clock cadence, turned into a guest invocation of the pump entry `0x0019A080` at a safe point. Opt-in, false by default. |

Also new: `TOL_JOBFIX=2` (see 189.6). `tools`-side: a timestamped launcher (`%TEMP%\opencode\tolrun.ps1`) plus a foreground-forcing window capture, so every number below is tied to the wall time it happened at and every capture shows the `TOLRecomp` window (verified: black client area, no ImGui strip).

### 189.2 Measured wall split: guest execution, every phase (default build)

150 s run, `[turncost]` every 200 presents, present + `[pc]` milestones:

```
[   8.785] [pc] enter 0x00151CD0 #1 a0=0x001F8150      (GS worker 1)
[   8.785] [pc] enter 0x00151CD0 #2 a0=0x001F84B0      (GS worker 2)
[   8.879] [pc] enter 0x00151CD0 #3 a0=0x001660A0
[  44.881] [pc] enter 0x00151CD0 #4 a0=0x002E09E0      (4th create)
[  44.926] [pc] enter 0x002E09E0 #1 a0=0x00407D70      (4th dispatch)
[  46.535] [fade] #1 ENTER mode(+14)=1
[  57.580] [fade] #2 ENTER mode(+14)=2
[  68.218] [fade] #3 ENTER mode(+14)=3
```

| phase | port | emulator | ratio |
|---|---|---|---|
| boot -> first creates | 8.8 s | 5.15 s | 1.7x |
| creates -> 4th create | **36.1 s** | 3.8 s | **9.5x** |
| unit1 -> unit2 | 11.1 s | 5.1 s | 2.1x |
| unit2 -> unit3 | 10.6 s | 5.1 s | 2.1x |
| fades vs 4th create | after | before (fade1 6.0, create 9.25) | order flips (189.5) |

Split of the counted time is the same in every window: `guest=97-99.5%`, `presenter=0.3-1.0%`, `paceWait=0.0%`
(`[turncost]`). So the largest single block is **the 36.1 s between the first three task creates and the 4th
create**, and inside it the cost is guest code, not the presenter, the pacer or the GS (session 152 measured the
GS backend at 4 % of wall during loading).

**Run-to-run variance is large and must be respected**: the same build in a second run put the fades at
26.06 / 32.99 / 40.21 s (`TOL_DUMP_UNITS` run), i.e. unit1 20 s earlier. Any window shorter than ~2 min, or any
comparison between two different runs, is unsafe.

### 189.3 Inside the block: a 4-PC guest loop, and emulated time at 1-8 % of real

`TOL_GUEST_HOT`, t=14-39 s (2 000 entries per line):

```
[14.076] entries=6000  distinct=152 top: 0x349D68=3.1% 0x349C9C=2.6% 0x349DD8=2.6% 0x349D48=2.5%
[19.405] entries=10000 distinct=21  top: 0x349D48=2.8% 0x349D68=2.8% 0x349C9C=2.8% 0x349DD8=2.8%
[32.409] entries=20000 distinct=21  top: 0x349D48=1.4% 0x349D68=1.4% 0x349C9C=1.4% 0x349DD8=1.4%
```

`0x349C9C/0x349D48/0x349D68/0x349DD8` are ~56 % of all entries with only **21 distinct pcs** in the window;
`FUN_00349C00` is the queue walk and `FUN_0027B390` the 60 Hz poller (session 152). `cyc/dcyc` in the same window:
4-10 M emulated cycles per 0.36 s, i.e. **`g/s` = 0.01-0.08** (real time = 1.0).

Consequence, measured in the transfer trace (`TOL_TRACE_XFER=1`):

```
[    4.864] [xfer] submit #69 blocks=266 ...  frames=36  iopump=68  pumps=162
[   20.443] [xfer] submit #70 blocks=255 ...  frames=38  iopump=69  pumps=199
[    8.940] [xfer] tick-pump #1 ... done=0   chunk=0   win=266
[   40.939] [xfer] tick-pump #2 ... done=255 chunk=255 win=255
```

A 266-block read submitted at 4.86 s is next submitted at 20.44 s, and the frame tick that drains it
(`0x001392D0`, the entry the existing frame-start fix hooks) runs **twice in 32 s**. One pump moves one 2048-byte
block, so the read trickles. This is not a broken loop: it is the 60 Hz poller behaving normally while the
emulated clock - which meters VBlanks, and therefore the pump - advances at host speed.

### 189.4 Attacks tried (all measured, two rejected)

| attack | result |
|---|---|
| `TOL_EE_CYCLE_SCALE=8` (more emulated cycles per branch) | 4th create 44.9 -> **23.6 s**, creates 8.8 -> 4.8 s. The block is time dilation, not raw throughput. Same ordering, no content. |
| `TOL_EE_CYCLE_SCALE=32` | **worse**: creates at 8.5 s (not 4.8) and no 4th create in 90 s. The scale interacts with guest timing assumptions; diagnostic only. |
| `TOL_EE_CYCLE_SCALE=4` | 4th create 37.4 s, fade1 38.7 s; only unit1 in 150 s. No content. |
| `TOL_XFER_WALLPUMP=1500` (host-driven pump) | **rejected**: boot stalls at 3.59 s, right after the CRI main-thread pump install, never reaches the loader. Re-entering `0x0019A080` from an arbitrary preemption point deadlocks it. Kept opt-in only. |

So: the port's *real work* is only ~2.1x the emulator (the unit phases), and the 9.5x block is waiting. The
runtime already has a real-time path - with `TOL_JOBFIX=2` the guest went idle and `[turncost]` reported
`g/s=0.96-1.05` with 88 % `paceWait` (189.6) - so the dilation exists only while a thread stays runnable. What is
missing is a way to recognise "the only runnable thread is spinning" and advance emulated time instead of
executing the spin (idle-loop detection, or scheduling VBlank on the host deadline as well as the cycle deadline;
VBlank is a 59.94 Hz hardware signal, `updateNextDeadline()` currently compares cycles only).

### 189.5 job[0] at matched guest milestones (the unit boundaries), port vs emulator

`TOL_DUMP_UNITS=0x480000,0x200000,logs\ramdump2` (port, this build) and `tools/ref_units.py` (emulator,
`ref_unit*` from session 160 - the guest code that writes this table has not changed). Entry `0x004E9140`,
6 words:

| word | port (all 6 boundaries) | emulator (all 6 boundaries) |
|---|---|---|
| `+0x0` state | **0x00000001** | **0x00000000** |
| `+0x4` id/status | **0xFF0D0001** | **0xFF0E0000** |
| `+0x8` | 0x00000002 | 0x00000002 |
| `+0xC` | **0x00002000** | **0x00000000** |
| `+0x10` | **0x00000001** | **0x00000000** |
| `+0x14` | 0x00623100 | 0x00623100 |

Both sides are **stable across unit1/2/3 entry and exit** - so this is not a race that resolves. The port is one
lifecycle step ahead *already at unit 1 entry* (status 1, `+0xC`/`+0x10` populated, id 0x0D) where the emulator is
still idle with request 0x0E queued. At 22.9 s the port's table was still empty
(`[jobtable] dump ... 00000000 0000FFFF`), so the submit+claim happens between ~23 s and unit1.

Probe timings (`TOL_TRACE_JOBTABLE=1`, 60 s run, claim at 24.400 s):

```
[ 5.194] [jobpoke]  #1 entry=0x004E9140 slot=0 status 0 -> 0 ra=0x002DEEE4      (FUN_002DEF60, submits nothing)
[24.400] [jobref]   #1 index=0 entry=0x004E9140 status 00 -> 02 bit1=1 ret=1 ra=0x002E1F98   (claim, inside FUN_002E1F80)
[24.431] [jobslot6] #1 s0=0x4E9140 ... 0000 -> 0000 ... ra=0x00117304
```

and the unit1 fade followed 1.4-1.6 s later in both runs that had both events. In the emulator the 4th create
(9.25 s) follows unit1's fade entry (6.0 s) by 3.25 s. So the port runs the app-job step that claims entry 0
*before* unit 1 where the emulator runs it after - consistent with the id being one unit behind (0x0D vs 0x0E).

### 189.6 `TOL_JOBFIX=2`: resetting the whole entry idles the guest, it does not draw

Mode 1 (id only, session 188) rewrote 0x0D->0x0E and produced no frame. Mode 2 also restores the emulator's
idle values (`+0x0=0`, `+0x4=0xFF0E0000`, `+0xC=0`, `+0x10=0`), one shot, at the moment the id appears:

```
[  23.577] loader state=1 chan=4 alive=1 mainIters=2(+2)     g/s=0.012  paceWait=0.0%
[  23.715] [jobfix] job[0] request id 0x0D -> 0x0E (mode 2, full entry reset)
[  24.600] mainIters=11(+0)                                   g/s=1.045  paceWait=88.1%
[  25.904] mainIters=11(+0)                                   g/s=1.034  paceWait=87.8%
```

The guest goes **idle** (all threads waiting, main loop frozen at iteration 11) and the emulated clock then runs at
**real time**. No fade, no content, screenshots at 100/130/148 s black. So the entry's lifecycle is load-bearing -
clearing it removes work the guest expects and nothing re-submits it. A fix cannot be a data rewrite of that word;
it has to be at the guest code that claims it early, or in the ordering that lets unit 1's fade finish first.

### 189.7 RETRACTION (instrument blind spot): `PS2Runtime::Store*` cannot see generated-code RAM writes

`ps2_runtime_macros.h` (header, read-only) expands `WRITE8/16/32/64/128` to

```c
if (PS2Runtime::isSpecialAddress(_addr)) runtime->Store32(rdram, ctx, _addr, val);
else { ps2TraceGuestWrite(rdram, _addr, 4u, val, 0u, "WRITE32", ctx); <direct rdram store> }
```

so **every RAM store from generated code bypasses `PS2Runtime::Store*`**, and therefore bypasses
`TOL_WATCH_WRITE`, `TOL_WATCH_JOBTABLE` and the `[jobtable] #` writer trace. Confirmed this session:
`TOL_WATCH_JOBTABLE=1` for 150 s printed only the dump line, zero `[jobtable] #<n> pc=...` lines, while the table
was demonstrably populated.

This retracts, as instrument artefacts rather than facts:

- session 168 `TOL_WATCH_WRITE=0x407B54` "0 hits" and session 175 "TCB4+0x1B4 has NO Store32/Store64 writer" -
  the field is written by generated RAM stores the probe cannot see;
- session 180 `TOL_WATCH_WRITE` on `TCB5+0x1B4`;
- this session's first attempt to trace the job-table writer.

`ps2TraceGuestWrite` (the universal RAM-write hook) is `inline` in `ps2_runtime.h`, so tracing those writers needs
a non-header hook (a `.cpp` function the macro calls, or the print itself) - do not repeat the blind-spot probe.

### 189.8 Next (in order)

1. **Speed, single target**: give the scheduler an idle-skip when the only runnable thread is spinning, or gate
   VBlank on the host deadline (`updateNextDeadline()` uses cycles only). Proof that real-time emission works is
   in 189.6: an idle guest already runs at `g/s` ~1.0. Expected effect: the 36 s block becomes the emulator's
   3.8 s, and the pump/frame tick stop starving.
2. **Then the ordering**: at matched milestones the port claims job[0] *before* unit 1's fade where the emulator
   does it after. Fix the ordering or the claim site (`ra=0x002E1F98` inside `FUN_002E1F80`), not the entry data.
3. Do not repeat: `TOL_XFER_WALLPUMP`, `TOL_JOBFIX=2`, `TOL_EE_CYCLE_SCALE` as anything but a diagnostic, and the
   `Store*`-based write watches.

---

## Session 190 (2026-11-20) - the whole resume chain IS called (proved at the call level); the fade is not covering anything; the remaining gap is thread state, not a missing function

### 190.1 New instrument: `TOL_TRACE_CALL=<addr>` - sees tail-called callees

`replaceFunction()` and `TOL_TRACE_PC` wrap the *function table*. Generated code reaches many functions by a
**direct C++ call** (`FUN_xxx(rdram, ctx, runtime)`) for `j`/tail transfers, which never touches the table. That is
why so many "0 entries" readings were wrong. `TOL_TRACE_CALL` hooks the actual invocation site in
`PS2Runtime::dispatchGuestBranch` (around `targetFn(rdram, ctx, this)`), so it fires for every call kind and prints
entry **and exit** registers.

RETRACTED as instrument artefacts (table probes cannot see direct calls): "`0x1B75D8` / `0x198B20` / `0x1B75D8`
never run" (this session, earlier attempt), and session 161's "all eight chain addresses entered 0 times" for the
tail-called members.

### 190.2 The resume chain runs end to end, with the emulator's arguments

Port, `TOL_TRACE_CALL=0x198BF8` (75 s run):

```
[57.107] [call] ENTER 0x00198BF8 from=0x001B7600 kind=3 a0=0 a1=1 a2=0x1E ra=0x1B7608 t304=0 id36C=6
[57.123] [call] EXIT  0x00198BF8 pc=0x001B7608 v0=0 a0=0x00000006 t304=0 id36C=6
```

Emulator, breakpoints at boot (fresh `-fastboot` run):

```
t=26.12s 0x1B75D8 a0=6 a1=1 a2=0x1E ra=0x259B48
t=26.17s 0x198B20 a0=6 a1=0x19 a2=0x1E ra=0x1B7608
```

Measured chain, both sides, same arguments:

| step | port | emulator |
|---|---|---|
| `0x2062A0` (loader slot+8) | 44.6 s, `a0=0x63D890 a1=1 s0=0x4079A0` | 25.8 s, same |
| `0x3353C0` / `0x2542D0` / `0x259B20` | yes (jal) | yes |
| `0x258E88` (`addiu a0,zero,6`) | yes (jal) | yes |
| `0x1B75D8` (direct tail call) | yes (via `TOL_TRACE_CALL` of its callee) | yes |
| `0x198BF8` -> `a0 = *(0x39436C)` | `id36C=6` | `6` |
| `0x198B20(6, 0x19)` | enters, returns in 16 ms | enters |
| `0x199200(6)` -> ResumeThread(6) | **only ever logged with a0=3** | yes |

The function table the `jalr` reads is also correct: `0x49C850` (index 6) = `0x198BF8` on both sides; the guest
computes it itself at `0x199900` (`lui 0x1A; addiu -0x7408`).

### 190.3 The fade is NOT covering the title (measured, twice)

`TOL_TRACE_ORDER`, 120 s: alpha ramps `0.04 -> 1.00` in 25 steps, holds, then descends (`0.2`, `0.6`); afterwards
`[gfx] ... draws=480/120 frames (4/frame) transfers=0 tex builds=0` forever. A native fade override would reveal a
black clear, not a logo. The emulator's own title frame was captured (`snaps/`): it is a **starfield of sprite
stars**, i.e. textured quads - and the port uploads no textures after the load.

### 190.4 Where the gap actually is

`0x198B20` sets `*(0x394304)=1`, raises the priority, then loops
`{ SuspendIfActive(id); ResumeIfSuspended(id); } while (*(0x394304) != 0)`, then re-suspends. In the port it
returns with `t304=0`, so one iteration ran. But at 94 s tid 6 is still `WaitSusp wait=sleep susp=1 pc=0x00198FD0`,
and `resume id=6` never appears, while `0x199200` is only ever called with `a0=3`.

So the defect is no longer a missing call: it is the **thread-state machine** - either `ResumeIfSuspended` does not
turn `Waiting(sleep)+suspended` into a runnable thread (session 162 measured the same: the suspend bit clears, the
sleep does not), or tid 4's loop re-suspends it (measured 33x/s from `ra=0x198DCC`). Note the emulator performs the
*same* `SuspendIfActive(6)` and `(3)` calls at the title, so "wrong thread" is not the answer.

### 190.5 Next (single item)

Compare the kernel's thread record for tid 6 immediately after `0x198B20` returns, port vs emulator: `status`,
`suspendCount`, `waitReason`. If the port is `Waiting/Sleep` where the emulator is `Ready`, the fix belongs in
`EeScheduler::resumeThread` (a resume must also make a sleeping-on-suspend thread runnable, as the EE kernel
does), not in any guest function. `TOL_TRACE_CALL=0x198BF8` + `TOL_TRACE_SUSP=6` in one run gives both sides of
that instant.

---

## Session 191 (2026-11-20) - fresh-boot reference trace: hardware sleeps tid 6 too (3.11 s) and WAKES it (25.87 s, WakeupThread from 0x1991D8); the gap is the kernel's status word, not a missing call

### 191.1 Probe (a) and (b) from 190.5: both negative, and both were the wrong layer

| probe | result |
|---|---|
| `TOL_WATCH_SLEEPFLAG=1` (samples `0x394304`, invisible to `Store*`) | stays **0** for 45 s - the sleep is not armed by that flag |
| `TOL_TRACE_CALL=0x114600` (`li v1,0x32; syscall`, the only SleepThread site in the ELF) | **0 calls** - but blind: `0x114600` can be reached by a direct C++ call |
| `TOL_TRACE_SLEEP=1` (kernel `sleepCurrent`, sees every caller) | `[3.471] [sleep] #3 tid=6 pc=0x0019902C wake=0` - the worker sleeps itself |

`createThread` is clean (`GuestThread thread{}`) and `suspendThread` never touches `wait`, so the Sleep state is genuinely the guest's own call.

### 191.2 Fresh-boot PCSX2 trace (the reference answer)

`tools/pcsx2_trace.py 30 0x114600`, fresh `-fastboot`:

```
#3 t=3.11s pc=0x00114600 a1=0x00390000 a2=0x003943B8 ra=0x0019902C sp=0x00399B80
```

`ra=0x19902C`, `sp=0x399B80` = **tid 6**. So hardware puts the worker to sleep at 3.11 s as well - the port's
sleep (3.471 s) is **faithful**, and "make sure we are leaving the thread" is answered: hardware leaves it asleep
too. What hardware then does is **wake it**:

```
#1 t=25.87s pc=0x00114610 a0=0x00000006 a1=0x0000000C ra=0x001991D8 sp=0x01FFEBB0
#2 t=26.22s ... ra=0x001991D8
... ~20x/s afterwards ...
```

`0x114610` is `li v1,0x33; syscall` = **WakeupThread**, called with `a0=6` from inside `FUN_00199198` (the
`0x199198(6)` the port also calls).

### 191.3 The exact gate (generated code of `FUN_00199198`)

```
0x1991b8  v1 = *(sp+0)            ; the status word the kernel returned
0x1991bc  v0 = 4
0x1991c0  beq v1, 4 -> 0x1991D0   ; status == 4  ->  jal 0x114610 (WakeupThread)
0x1991c4  a1 = 0xC
0x1991c8  bne v1, 0xC -> ...      ; 0xC path
0x1991d0  jal 0x114610            ; WakeupThread(id)
```

So the fix layer is the **runtime kernel's thread-status word**, not a guest function and not a missing call. The
port's `0x199198(6)` runs, reads a status that is neither `4` nor `0xC` (its thread is `Waiting(sleep)` with
`susp` just cleared), skips the wake, and the worker stays asleep forever.

### 191.4 The fix (next session, one edit + one run)

Make the status the kernel returns for a thread that is `Waiting(Sleep)` with a suspend count match the EE
kernel's encoding - `4` is the value the guest tests for the waiting-suspended case - so `0x199198`'s own
`beq v1,4` takes the wake branch. Verify with `TOL_TRACE_SLEEP=1 TOL_TRACE_CALL=0x114610` in the port: `[sleep]`
for tid 6 should be followed by a `0x114610` ENTER with `a0=6`, then tid 6 `Running` and `[gfx] tex builds > 0`.

Do not repeat: sleeping tid 6 (faithful), the suspend/resume chain (proved), the fade (exonerated), and any
`Store*`-based watch of `0x394304`.

---

## Session 192 (2026-11-20) - FIX: wakeupThread no longer swallows a wake while suspended; regression clean and the whole boot is 1.6x faster

### 192.1 The defect and the change

`rawThreadStatus` was already correct (`Waiting = THS_WAIT 4`, `WaitingSuspended = THS_WAITSUSPEND 0xC`), so the
guest's `beq v1,4` at `0x1991c0` did reach `WakeupThread`. The wake was lost one layer deeper: `wakeupThread()`
sent a `WaitingSuspended` sleeper to `makeReady()`, which cannot promote a suspended thread, and `resumeThread()`
then mapped `WaitingSuspended -> Waiting`. Since `0x198B20` wakes (`0x198b8c`) *before* it resumes (`0x198b94`),
the wake was always discarded.

`EeScheduler::wakeupThread`: when the target is `Waiting`/`WaitingSuspended` on `Sleep` **and still suspended**,
clear the sleep wait and move it to `Suspended`, so the following `ResumeThread` takes the existing
`Suspended -> enqueueReady` path. Unsuspended sleepers keep `makeReady`.

### 192.2 Verified

```
[62.751] [call] ENTER 0x00114610 from=0x001991D0 a0=6 a1=0x0C ra=0x001991D8   <- WakeupThread(6), same site as hardware
[107.030] tid=6 Waiting wait=sleep susp=0 pc=0x00198FD0                     <- suspend stays 0, pc inside the loop
```

### 192.3 Regression (same probes as the pre-fix baseline `logs/perf/base150`)

| milestone | before | after |
|---|---|---|
| first creates | 8.9 s | **7.1 s** |
| 4th create | 44.9 s | **28.6 s** |
| unit1 / unit2 / unit3 fade | 46.5 / 57.6 / 68.2 s | **29.5 / 36.1 / 43.0 s** |
| `guest-branch:missing-target` / `[badpc]` | 1 / 0 | **1 / 0** |
| `g/s` (guest-seconds per wall-second) | 0.01-0.08 | **0.56-0.58** |
| `mainIters` rate | 8/s | **12/s** |

So the kernel change does not regress the boot or the three load units - it reaches every milestone earlier and the
emulated clock runs ~10x faster, because the worker no longer sits in the spin. Still `tex builds = 0`: the worker
now wakes and runs, then sleeps again because no work is posted. The remaining item is the producer
(`0x3943B8` run flag / the queue `0x1B7888` fetches), not the scheduler.

### 192.4 Re-check: job[0] is NOT a timing artifact (retracts the 189/190 hypothesis)

After the wake fix (4th create 44.9 -> 28.6 s, unit1 at 23.4-29.5 s across runs) the six unit-boundary dumps were
taken again (`TOL_DUMP_UNITS=0x480000,0x200000,logs\ramdump3`) and compared with `ref_unit*`:

```
unit1/1_ret/2/2_ret/3/3_ret   port  00000001 FF0D0001 00000002 00002000 00000001 00623100
                              ref   00000000 FF0E0000 00000002 00000000 00000000 00623100
```

Identical to the pre-fix measurement, at **every** boundary. So "job[0] is one lifecycle step ahead because the
port is 4x slow" is wrong; the entry is consumed before unit 1 in every run regardless of timing. Treat job[0] as
a symptom of the same early app-job step, not as something to patch (`TOL_JOBFIX=2` already idled the guest).
The remaining producer probe stays as in 192.3.

### 192.5 Producer probe: all three producers are retired; the difference is the worker loop's cadence and the sleep-flag handshake

Port (`TOL_TRACE_CALL=0x1B7888,0x199938,0x199AF0,0x199988` + `TOL_TRACE_SLEEP`, 55 s) vs fresh-boot PCSX2 on the
same four addresses:

| address | port | emulator |
|---|---|---|
| `0x1B7888` (work fetch) | **2 calls** (3.6 s, 45.9 s) | **~20/s from 26.15 s** |
| `0x199938` (run-flag writer) | 0 | **0** |
| `0x199AF0` (run-flag writer) | 0 | **0** |
| `0x199988` (release/refcount) | 0 | **0** |

So session 163's three "producer" candidates do not run on hardware either - they are retired, not the gate.

What differs is the worker's own loop: after the release, hardware calls `0x1B7888` every ~50 ms with
`a0=0x3B7348`, `a1=1`; the port calls it a second time at 45.9 s with `a1=0x390000` (the stale first-call set) and
**`0x394304 = 1`**, so the loop takes the `beq 1 -> SleepThread` at `0x198FBC` instead of iterating.

Next fix site: the `0x394304` set/clear handshake. `0x198B20` sets it (`0x198b60`) and clears it in its epilogue,
and the worker is expected to clear it while draining; with the flag left set, the worker sleeps on its next pass.
The delay slot at `0x198FB0` is `sw $s1,(s3)` - it does not set `a0`/`a1`, so the `a1=1` on hardware comes from
the loop's own state, not from a missing argument setup.

**Probe (a) is not trustworthy**: `TOL_WATCH_SLEEPFLAG` samples from the host main loop, which is starved while
the guest is 99 % busy, so it reported no transition while the authoritative on-thread reading at the call site
(`[call] ... t304=1` at the second `0x1B7888` fetch) shows the flag was 1. Use the call-site reading, not the
sampler.

### 192.7 Disproved: 0x394304 is not the pacing mechanism, and the fetch returns 0 on both sides

- **Paced clear tested** (`TOL_CLEAR_SLEEPFLAG=50`): only **one** clear happened in 80 s (the flag is set once and
  never re-set), the worker then spun at ~2,100 fetch/s and the frame loop starved (`mainIters +0`, `g/s 0.017`).
  So "clear the flag" is neither correct nor paced - it is a one-shot hold flag.
- **Fetch return value measured on both sides**: port `[call] EXIT 0x001B7888 ... v0=0x00000000`; hardware at the
  return point (`bp 0x198FB4`, `eval v0`) also `0x0`. So the fetch return is **not** the difference either.
- What remains: hardware calls `WakeupThread(6)` ~20x/s from `0x1991D8` (session 191.2), i.e. the whole release
  chain `0x2062A0 -> ... -> 0x198B20 -> 0x199198` runs **per frame** there, and only **once** in the port. The
  pacing is the release cadence, not any flag or return value.
- Frame-independent upload counters now exist (`[turncost] ... gsw=N(+d) gif=N(+d)`) so texture activity can be
  read even when the frame loop stalls. Steady state so far: `gif=4(+0)`, `gsw=0(+0)` - no uploads.


## Session 193 (2026-11-20) - static map of the master loop, plus four retractions: 0x1F74E0 is LZSS not a fade, 0x1392D0 is a GS_CSR getter not a frame tick, and "0.48 Hz" was a decimating instrument

### 193.1 The master loop, from the ELF (static, no run)

`entry` -> `FUN_00165AD0`. Its frame body is `0x165B88..0x165BE4` and calls, in this order:

| site | callee | what it is |
|---|---|---|
| 0x165B88 | `FUN_00152B70` | VIF/DMA state machine (281 insns, state in `0x4969C0`/`0x4969C4`); memsets `0x100` bytes (`jal 0x109108`, a2=0x100), writes four `0x80` bytes at `sp+0x52..55`, bumps the `0x4969C4` counter |
| 0x165B90/98/A0 | `FUN_00151FF0(0)`/`(1)`/`(4)` | guest scheduler pumps, one per CPU |
| 0x165BA8 | `FUN_001516B0` | |
| 0x165BB0 | `FUN_001F96C0` | |
| 0x165BB8 | `FUN_0019A080` | -> `FUN_001B7870` -> `FUN_001B7688(5)`, runs 5 callbacks from `0x49C6A0` ("the pump") |
| 0x165BC0 | `FUN_00153780` | |
| 0x165BC8 | `FUN_00164D40` | |
| 0x165BD0 | `FUN_00139A20` | the real per-frame tick |
| 0x165BD8 | `FUN_00153180` | exit test |

Two structural facts settle earlier confusion:

- **`FUN_00153180` is a literal stub**: `0x153180` is `0800E003 2D100000` = `jr ra; move v0,zero`. The frame loop's
  test is therefore constant-false, i.e. it is a deliberate unconditional `for(;;)`. The code after `0x165BE8` is
  dead in this build. This is a game main loop, **not** a stuck gate.
- **`FUN_00139A20` is the tick**: it does `REG_RCNT0_COUNT = 0`, `iGpffff84c8++` (frame counter), and calls
  `FUN_00139310`, which compares the tick delta against `0x20D` (525 lines = one NTSC frame) and sets
  `fGpffff84ac = 30.0` otherwise. `FUN_00139310 -> FUN_001392D0 -> FUN_001135B8` returns **bit 13 of `REG_GS_CSR`**.

### 193.2 Retracted: 0x1392D0 is not a frame tick and does not drive the pump

`FUN_001392D0` is 8 lines: `uGpffff848c = FUN_001135B8(0); return uGpffff848c;` - a GS_CSR status getter. It sits at
`0x165BD0`, **after** the pump at `0x165BB8`, inside the same call to `FUN_00139A20`. So "0x1392D0 (29) drives
0x19A080 (28)" was one loop counted at two points, not a causal chain. "Raise 0x1392D0's rate" is dead: it is
downstream of the frame loop's own rate.

### 193.3 Retracted: the 0.48 Hz frame rate is an instrument artifact

`TOL_TRACE_CALL` prints on `seq <= 25 || seq % 200 == 0` (`ps2_runtime.cpp:1897` and `:1944`). Counting `[call]`
lines is therefore decimated by ~200x after the first 25 per address, and any calls/s derived from them is not a rate.

Independent run, same build, `TOL_PCHIST=1 TOL_TRACE_SLEEP=1`, 78 s (`console/run_pchist.txt`):

```
guest=99.3%  paceWait=0.0%  presenter=0.7%  g/s=0.528-0.554  cyc advancing at ~158 MHz of 294.912 MHz
gsw=0(+0)  gif=5(+0)  loader state=1 chan=4 alive=0
```

The guest is ~55 % of real EE speed and **never blocks** (paceWait 0, no 2 s stall anywhere in the run). There is no
0.48 Hz frame loop. The "per-frame vs 0.48/s" framing, and every scheduler-side variant aimed at it, were aimed at a
decimating log.

### 193.4 Retracted: 0x1F74E0 is not "the fade wait" - it is an LZSS decompressor

`FUN_001F74E0` keeps a bit buffer (`uGpffff8794`, `0xff00` sentinel), a 0..3 state machine (`iGpffff8798`) and copies
runs out of a sliding window at `0x4A6260`: `len = (*p & 0x0F) + 3`, offset `uGpffff878c + i`, source
`(uGpffff8788 & off) + 0x4A6260`. That is LZSS. Its 11.2 % + 5.5 % of samples is **decompression**, not a fade and
not a wait.

### 193.5 Corrected: 0x12xxxx IS entered - but only as a math routine

91 `[pchist]` lines contain a `0x12xxxx` PC. The only addresses seen are `0x124150`, `0x1257C8`, `0x1254A8`,
`0x1255A8`, `0x1203A0`, all inside **`FUN_001240D8`, an integer sqrt** (bit-by-bit, the `0x124150` block). Peak count
1920 of 15.6 M samples = 0.012 %. So "the port never executes 0x12xxxx" is wrong in letter; what survives is "it
never reaches the 0x12xxxx frame/title code".

### 193.6 What the main thread actually spends samples on (cumulative, n=15.6 M)

| PC | % | what |
|---|---|---|
| 0x109144 | 11.8 | memset inner loop (the `0x100`-byte clear from `FUN_00152B70`) |
| 0x1F75F8 | 11.2 | LZSS decompressor |
| 0x289E10 | 9.8 | global-indexed byte read: `iGpffff87bc * 0x12D8 + 0x4A7E27` |
| 0x3344A0 | 7.7 | `byte & 0x0F` at `idx*0x18 + 0x3DB660 + 0x24` |
| 0x27B140 | 6.1 | 12-byte record read, `type = byte & 7` |
| 0x1F750C | 4.9 | LZSS decompressor |
| 0x27B290 | 4.3 | record read + test `type - 2 < 3` |
| 0x27B6E0 | 4.3 | record-family loop head |
| 0x334294 | 3.9 | slot scan loop |

`FUN_003341E0` scans indices `0..0xE8` (232) calling `FUN_003344A0/34470/345C0/34630`, and returns with `idx > 0xE8`
when it finds nothing acceptable. `FUN_0027B290` reads a 12-byte record and tests `type - 2 < 3`. `FUN_00289E10` is a
single byte read indexed by `iGpffff87bc`.

### 193.7 What this changes

Not pacing, not sleep, not a 2 s block. After (and between) the load units the main thread spends its samples in a
**232-slot scan plus 12-byte record lookups, while issuing no GS work at all** (`gif=5` total, `gsw=0` over 78 s).
The live question is data, not timing: whether the table at `0x003DB660` (stride `0x18`, type field at `+0x24`) holds
entries on hardware at the same milestone and zeros in the port. That is the same heap-diff method that already
proved byte-identical at the unit boundaries.

### 193.8 Guest-side scheduler (static, explains why host-side wake injection cannot help)

The guest has its own scheduler in user code, with no syscall: `FUN_00151FF0(cpu)` -> `FUN_001520A0` (ready bitmap at
`0x48F570`) -> `FUN_00151E90` walks the thread list and calls each entry at `+0x1B0`. The switch is
`FUN_0015231C`: it spills all GPRs/HI/LO/SA into the TCB, sets `+0x1AC` (the flag already retired in 192), then
`sw ra,0x1b4(t0); lw ra,0x1a8(t0); jr ra`. That is why six host-side scheduler variants could not move it - the host
scheduler is not the scheduler that owns this loop.

### 193.9 Retractions recorded

1. "0x1F74E0 is the fade wait" -> LZSS decompressor.
2. "0x1392D0 is a frame tick / drives 0x19A080" -> GS_CSR bit-13 getter, downstream in the same iteration.
3. "the frame loop runs at 0.48 Hz vs per-frame" -> `TOL_TRACE_CALL` decimation; guest is 55 % speed, 99.3 % busy.
4. "the port never executes 0x12xxxx" -> it executes `FUN_001240D8` (sqrt) there, ~0.012 % of samples.


## Session 194 (2026-11-20) - the 0x3DB660 slot table is NOT the divergence: it is populated, matches hardware at 0x3DB660-0x3DB6FF, and freezes at unit 1

### 194.1 Method (both halves worked)

- **Port**: `TOL_DUMP_UNITS=0x3DB000,0x2000,logs\tabdump` (the hook takes an arbitrary addr/len, so the
  window was narrowed onto the table) plus `TOL_DUMP_RAM` at 20 s intervals. Wrote `port_unit{1,2,3}.bin`,
  `port_unit{1,2,3}_ret.bin` and `port_t0..t3.bin`, all 0x3DB000..0x3DD000.
- **Reference**: PCSX2 DebugServer read of the same window. `ps2dbg.py` is one request per process, so
  `tools/dump_ref_range.py` (new) does a single-connection chunked dump -> `logs\tabdump\ref.bin`.
- Window covers the whole table region `0x3DB660..0x3DCF40` (0x18 x 232 = 0x18E0).

### 194.2 Result: the table is populated, and it is frozen

- `port_unit1 == port_unit2 == port_unit3 == port_t3` **byte-identical over the table window** (diff = 0 in all
  six pairings). Four samples spanning unit 1 (~20 s) to t=60 s, while the guest is 99 % busy, and the table
  does not change by a single byte.
- Only `port_t0` (t=20 s, before unit 1) differs, by 52 bytes: the pointers at `0x3DB660`, `0x3DB688`,
  `0x3DB698`, `0x3DB6B8` are zero there and populated at unit 1 (`0x01562FA0`, `0x015645A0`, `0x015A27A0`,
  `0x015A28A0`, `0x01582E20`, `0x01583320`, `0x015627E0`, `0x015A3520`, `0x01523D60`, `0x01523CA0`,
  `0x01524060`, `0x01561920`, `0x0156E360`). So the loader fills it at unit 1 and nothing writes it again.

### 194.3 vs hardware at the title: 161 of 6368 bytes (2.5 %), first at 0x3DB700

At `0x3DB660..0x3DB6FF` the port and hardware **match exactly**. The differences all sit after that and are the
shape of *records hardware added at a later milestone*:

| guest | hardware | port |
|---|---|---|
| 0x3DB700 | `1` + ptr `0x3DB744` -> `"cdrom0:\SLUS_212.01;1"` | `0` / null |
| 0x3DB900/08 | `0x000808B7` + `"MOVIE.AFS;1"`, `"AFS;1"` | `0x00700000` + zeros |
| 0x3DBA88, 0x3DBAB0, 0x3DBB50 | `0x08`, `0x07`, `0x12` | `0xFFFFFFFF` |
| 0x3DBB9C..0x3DBBF0 | records with ids `0x15`,`0x16`,`0x17`, type `7`, with pointers `0x0007DAC8`/`0x0007FB58` | all zero |
| 0x3DBC40 | `0x0101800C`, `1`, `0x00165CE8`, `0x00496B80` | `0x00000320` x3 |

### 194.4 Retraction: the slot table is not the first divergence

The hypothesis from session 193.7 ("the scan finds an empty table because the loader never populated it") is
**dead**: the table is populated, matches hardware through `0x3DB6FF`, and the remaining 161 bytes are exactly the
entries hardware accumulates when it progresses past this point (disc path registration, `MOVIE.AFS`, records
0x15-0x17). That is a *consequence* of the port not advancing, not a cause.

Caveat, stated plainly: this is **not** a same-milestone comparison - `ref.bin` is hardware at the title
(PC `0x127DB4`), the port files are at the unit boundaries. A true same-milestone diff needs a PCSX2 reset plus
`ref_units.py` with `0x3DB000 0x2000`, which costs the current title state.

### 194.5 No static xrefs - the table is reached through a computed base

`xrefs_list` to `0x3DB700` and `0x3DBBA0` both return **0 references**. The table is indexed off a computed/gp
base, so neither Ghidra xrefs nor `TOL_WATCH_WRITE` will find its writer by address. Any future probe must
find the base register at runtime, not the absolute address.

### 194.6 CORRECTION: 194.2-194.4 compared the wrong structure

`FUN_003344A0` computes `param_1 * 0x18 + iRam003db660 + 0x24`, and `iRam003db660` is a **dereference**: the
real base is the *value* at `0x3DB660`, which is `0x01562FA0`. That address starts with magic `"EX2B"`
(`0x42325845`), then `0xE9` (233 entries) and `0x18` (24-byte stride) - exactly the array `FUN_003341E0` scans
to `0xE8`. So the scan table lives at **`0x01562FA0`**, not at `0x3DB660`.

The region diffed in 194.2-194.4 (`0x3DB700`, `"cdrom0:\SLUS_212.01;1"`, `"MOVIE.AFS;1"`) is a *different,
neighbouring* structure - a file/resource table. Those numbers describe that structure, not the scan table.

### 194.7 The real scan table is byte-identical - the hypothesis is dead twice over

Dumped correctly (`TOL_DUMP_UNITS=0x01560000,0x8000` vs `tools/dump_ref_range.py` on the same window), the
table `0x01562FA0..0x01564578` (5960 bytes, 233 x 0x18):

```
port_unit1      diff = 0
port_unit1_ret  diff = 0
port_unit2      diff = 0
port_unit3      diff = 0
port_unit3_ret  diff = 0      (vs hardware at the title)
```

**Zero differing bytes at every unit boundary.** The 232-slot table the hot scan loop walks is identical to
hardware's. So `FUN_003341E0` cannot be scanning a table the port failed to populate - it walks exactly the
same bytes hardware does.

Therefore the scan loop's behaviour must differ through its **criteria** (`FUN_003341E0(param_1, param_2)`,
where `param_2` carries the start index and the type/flag filters), or through being called at all. The table
is data the ELF supplies; it is not the thing that diverges.

### 194.8 Watchpoint probe: blocked without a fresh boot

- `set_memcheck` on `0x3DBA88..0x3DBA8C` (write) while the game was at the title: **hits = 0** after ~40 s of
  execution (`watchlist` confirms it stayed armed). The record is a one-shot write during load, so catching its
  writer requires a boot, not a title-state probe.
- The DebugServer has **no reset command** (`available_commands` has no `reset`/`reboot`), so the probe cannot be
  started from the API.
- Every savestate on disk is post-load: slot 01 was loaded and shows the same populated values
  (`0x25`, `0x08`, `0x000443C8`) and the same idle PC `0x00114C60`. No early-boot state exists to rewind to.
- Consequence: this probe needs a manual `System -> Reset` in PCSX2, then arm `set_memcheck` immediately.


## Session 195 (2026-11-20) - the scan's criteria are literals and one query is logically unsatisfiable: FUN_003341E0 cannot differ, and the hot profile is one update routine

### 195.1 FUN_003341E0's acceptance predicate, decoded

`FUN_003341E0(out, crit)` walks `idx = crit[1]+1 .. 0xE8` (233 entries) and **rejects** the candidate while
any of these hold, so it *accepts* only when all are false:

| test | rejects when |
|---|---|
| `FUN_003344A0(i) == 6` | the slot's type is 6 |
| `crit[0] != 0 && FUN_00334470(i) != crit[0]` | a family byte is given and differs |
| `crit[4] == 1 && FUN_003345C0(i) == 0` | a "flag 4" test was asked for and fails |
| `crit[2] != 0 && FUN_003344A0(i) != crit[2]` | a type filter is given and differs |
| `crit[5] == 1 && FUN_00334630(i) == 0` | a "flag 5" test was asked for and fails |
| `crit[3] != 0 && FUN_003344C0(i) != crit[3]` | a "field 3" filter is given and differs |

`FUN_003341A0(out, type, p3, p4, p5, p6)` builds `crit = { type, startIdx=0, p5, p6, p3, p4 }`.

### 195.2 Every call site's criteria are literals, not derived state

| caller | built by | crit | asks for |
|---|---|---|---|
| `FUN_00333EE0` | `FUN_003341A0(&s,0,0,0,5,0)` | `{0,0,5,0,0,0}` | type 5 |
| `FUN_00333EE0` | `FUN_003341A0(&s,0,0,0,6,0)` | `{0,0,6,0,0,0}` | **type 6 (unsatisfiable, see 195.3)** |
| `FUN_00349C00` | `FUN_003341A0(&s,0,0,0,2,arg)` | `{0,0,2,arg,0,0}` | type 2 + field 3 |
| `FUN_00220F90` | `FUN_003341A0(&s,0,0,1,0,0)` | `{0,0,0,0,0,1}` | flag-5 test only |
| `FUN_00313570` | `FUN_003341A0(&s,arg,1,0,0,0)` | `{arg,0,0,0,1,0}` | family arg + flag-4 |
| `FUN_0037F2B0` | `FUN_003341A0(&s,arg,0,1,0,0)` | `{arg,0,0,0,0,1}` | family arg + flag-5 |

Every criterion is an immediate literal or a byte-sized type taken from the caller's own argument. **None of them
reads port-divergent state.** Combined with 194.7 (the table is byte-identical), `FUN_003341E0` is
deterministic and must behave identically on both sides.

### 195.3 A real property of the game: the type-6 query can never match

`FUN_003341A0(..., p5 = 6, ...)` sets `crit[2] = 6`. The loop rejects `type == 6` **and** rejects
`type != 6`, so its accept condition is `(type != 6) && (type == 6)` - **false for every slot**. That call
therefore always walks all 233 entries and returns "none". `FUN_003343E0` then reports no cursor, so the
caller's `while` body is skipped - it costs a full scan and nothing else.

This is identical on hardware (same code, same table), so it is not the divergence. It does explain why
`0x3344A0`/`0x334294` dominate a profile and why this loop is a bad place to look for a rate difference.

### 195.4 Retraction: the criteria hypothesis is dead

Since the table is byte-identical (194.7) and every criterion is a literal (195.2), the scan cannot be
searching differently. The scan is hot because the routine that *calls* it is hot.

### 195.5 What that routine is - the whole hot profile in one function

`FUN_0027AD50` is a large per-frame update that calls, in order, `FUN_00279730`, `FUN_00330A80`,
`thunk_FUN_00342D10`, `thunk_FUN_002E53E0`, `FUN_00279BF0`, `FUN_00349620`, **`thunk_FUN_00333EE0`** (the scan
above), `thunk_FUN_0027A440`, `thunk_FUN_00329440`, `thunk_FUN_00371230`, `thunk_FUN_00335CB0` x2,
`thunk_FUN_00286390`, `thunk_FUN_00289030`, `thunk_FUN_00298160`, `FUN_00289640`, `thunk_FUN_00330840`,
`FUN_00290D10`, **`thunk_FUN_0027B100`**, `thunk_FUN_0027BA00` - i.e. the slot scan *and* the hot
`0x27B1x0`/`0x27B2x0` record family in a single pass. One function accounts for the profile.

Its two entry chains:

```
master loop 0x165B2C -> FUN_00166120 -> FUN_0027AD50          (outer loop body, once per outer iteration)
FUN_001D8C60 -> FUN_001DC480 -> thunk_FUN_0027AEB0 -> FUN_0027AD50
```

`FUN_001DC480` is a 4-way mode switch on `FUN_0016C790(param)`; `FUN_0027AD50` is reached only in **mode 1**.

### 195.6 Next

The remaining question is no longer "what data is wrong" but **how often `FUN_0027AD50` runs and in which mode**.
Instrument `FUN_0027AD50`'s entry count and `FUN_001DC480`'s `lVar1` value on both sides; that single pair
(`mode`, `calls/s`) decides whether the port is stuck re-entering mode 1 or is in a different mode entirely.


## Session 196 (2026-11-21) - first real frame rate: 19.6 fps, not 0.48; the update routine is called once and is not hot; the frame body stops at ~200 frames and the guest collapses to g/s=0.005

### 196.1 `TOL_TRACE_CALL` decimation quantified (closes 193.3)

`TOL_TRACE_CALL` prints `seq <= 25 || seq % 200 == 0`. So `n` printed ENTER lines for an address means its total
call count is in `[25 + (n-25)*200, 25 + (n-24)*200)`. Any "calls in N seconds" read off those lines is wrong by
up to ~200x. The `#seq` field on the last printed line *is* the true count at that timestamp, which is what
makes a rate recoverable.

### 196.2 FUN_0027AD50 is called ONCE - the "hot update routine" hypothesis is dead

```
[call] ENTER 0x00166120 #1 from=0x00165B2C kind=1 ...        (once)
[call] ENTER 0x0027AD50 #1 from=0x00166238 kind=1 ...        (once)
[call] EXIT  0x0027AD50 #1 ... (18 ms later)
```

`FUN_00166120` and `FUN_0027AD50` are each entered **exactly once**, at t~6.8 s, and `FUN_0027AD50` returns in
**18 ms**. The master-loop outer body (0x165B2C) runs once, as the static reading said. `FUN_001DC480` and
`FUN_0016C790` produced **no** trace lines at all (reached by static `jal`, which `dispatchGuestBranch` never
sees - the documented blind spot). So the mode-1 reading could not be taken, and the routine that carried the
whole hot profile is not hot.

Cause of the error: the cumulative PCHIST top-12 is dominated by the load phase. In two separate runs,
`0x1F75F8`, `0x289E10`, `0x3344A0` and `0x27B140` show **identical** counts (1748804 / 1537338 / 1209203 /
958696). Those functions stop being executed at a deterministic point; the histogram describes the past, not
the present.

### 196.3 First real frame-loop rate: 19.6 fps

Traced the eight frame-body callees at 0x165B88..0x165BD8. Five of them (`0x152B70`, `0x1516B0`, `0x1F96C0`,
`0x153780`, `0x164D40`) reach `#200` at the same instant, which is the signature of once-per-frame calls:

```
0x00152B70  first 00:11:32.964  #1     last 00:11:43.114  #200    199 frames / 10.15 s = 19.6 fps
0x001516B0  first 00:11:32.997  #1     last 00:11:43.142  #200
0x001F96C0  first 00:11:32.998  #1     last 00:11:43.143  #200
0x00153780  first 00:11:32.999  #1     last 00:11:43.146  #200
0x00164D40  first 00:11:33.001  #1     last 00:11:43.146  #200
```

**199 intervals in 10.15 s = 19.6 frames/s.** This is the first honest frame-rate number for the port; the
"0.48/s" of sessions 189-192 was the logging decimation of 196.1, not a measurement.

### 196.4 Reproduced: the frame body stops, and the guest collapses

Two independent runs (with and without the timestamp pipeline) both stop printing the frame body after `#200`
and both end in the same state:

```
dcyc=543240  g/s=0.005  gsw=0(+0)  gif=4(+0)  loader state=1 chan=4 alive=1  mainIters=261(+8)
cyc=279,768,488        (healthy runs end near cyc=5.1e9, g/s=0.53, alive=0)
```

Total call count is bounded to `[200, 399)` by the decimation, so the frame loop ran at least 199 and at most
398 frames; what is certain is that it **stopped running**, `0x151FF0` (scheduler pump) and `0x19A080`
(transfer pump) outlived it (until 48.5 s / 47.7 s), and the guest then advanced at **0.5 % of real time**.

Note the run-to-run difference: `alive=1` and `g/s=0.005` here, versus `alive=0` and `g/s=0.53` in the PCHIST
and dump runs. The port has nondeterministic end states, so any single run's final `g/s` must be quoted with
`alive`.

### 196.5 What is now established vs not

Established: frame loop ~19.6 fps; `FUN_0027AD50` runs once and returns in 18 ms; cumulative PCHIST is not a
picture of current execution; the frame body stops and the guest collapses to 0.005x with a live thread.

Not established: what blocks at the stop, and whether the stop is deterministic (the `alive` difference says
the end state is not). Next measurement must be **non-cumulative**: a count of `0x152B70` past 400 (i.e. a
window long enough to print `#400`/`#600`) together with `alive`, so "stops at ~200" can be separated from
"reaches 399 and is still going".









## Session 197 (2026-11-21) - THE STARTUP LOGOS AND THE TITLE ARE MPEG VIDEO: the milestone needs an IPU

### 197.1 The reference boot sequence, captured frame by frame (new, definitive)

Fresh `-fastboot` boot of `data\Tales of Legendia (USA).iso` in PCSX2, screen captured every
1.5 s (`tools/refboot_seq.ps1`), tiled into one sheet (`tools/montage.ps1`):
`logs/refseq2/t*.png` + `logs/refseq2/montage.png`.

| wall time | what is on screen |
|---|---|
| 3.1 s | black |
| 5.0 - 11.0 s | **NOW LOADING** (the only frame the port has ever produced) |
| 12.0 s | black (transition) |
| 13.6 s | **namco** logo - white card, red wordmark |
| 15.1 - 16.6 s | namco, fading out through grey |
| 18.0 - 21.1 s | **moth Productions** logo - white card, blue wordmark |
| 22.5 - 25.6 s | **Sofdec / IBM** logos - grey then white card, fading |
| 27.1 s | black |
| 28.5 s | dark: a glowing blue creature appears |
| 30.0 - 39.1 s | **opening movie** (CG: sun over sea, two figures, hands, white flash) |
| 40.6 s -> | **TALES OF LEGENDIA** title screen (logo over greenery), stable at 45 s |

This replaces the guess recorded in the session-158 header. The "three units" that `FUN_00204240` runs
(modes 1/2/3 at ~6 / 11 / 16 s) are the **three logo cards**, and what earlier notes called "the title at
~21 s" is the logo reel / start of the movie. The real title only arrives at ~40 s of wall time.

The **Sofdec + IBM** card is decisive evidence for what these are: those two screens are the CRI Sofdec
codec's own licence splashes, i.e. the sequence after `NOW LOADING` is **streamed MPEG-2 video**, not
2D art. Cross-checked three independent ways:

1. `AFS\MOVIE.AFS` (894 MB) holds 22 entries, every one beginning `00 00 01 BA` = MPEG-2 **program stream
   pack header** (PSS). `SYS_REG.AFS` is not plain AFS. So the disc ships standard, FFmpeg-decodable
   MPEG-2.
2. Paused at the reference title, the EE pc is `0x001292B4`, inside `FUN_00129258`, which drives
   `REG_IPU_CMD` / `REG_IPU_BP` / `REG_IPU_TOP` - the **IPU**, the PS2's MPEG-2 decoder.
3. The game imports **no** `sceMpeg*` / `sceIpu*` (`data/output_fixed/ps2_recompiled_stubs.h` has zero
   hits; `config*.toml` has zero), so it is not the HLE libmpeg path - the guest drives the IPU
   hardware register-by-register.

### 197.2 The port's side: it reaches the movie stage and then has nothing to decode

`TOL_TRACE_IPU=1`, 130 s run (`logs/run197/ipu130.stderr.log`):

```
[51.720] [ipu] w 0x10002010   IPU_CTRL
[51.720] [ipu] w 0x10002000   IPU_CMD  x3
[51.720] [ipu] w 0x10007010 / 0x10007014 / 0x10007018 / 0x1000701C   IPU_IN_FIFO x~10
[51.721] [ipu] w 0x10002000   IPU_CMD  x3
[51.721] [ipu] w 0x10002010
[51.798] [ipu] w 0x10002010
```

i.e. exactly the `sceIpuInit`-shaped sequence the runtime already fakes in
`Kernel/Stubs/IPU.cpp::completeIpuInit`: `SETIQ` (0x50..), `SETVQ` (0x60..), `SETD4` (0x90..) with the
IQ/VQ tables pushed through `IPU_IN_FIFO`. After that, for the remaining 78 s:

- **zero** `IPU_BP` / `IPU_TOP` reads (the guest's VDEC loop never runs),
- **zero** `[ipu-dma]` lines = no `IPU_FROM` / `IPU_TO` transfer at all (the bitstream is never fed),
- `transfers=0  tex builds=0  gsw=0(+0)  gif=5(+0)`; the only draws are the same 4 untextured black
  primitives per frame, i.e. no logo frame is ever uploaded.

So the port initialises the decoder and then stalls before the first picture. Nothing depends on
scheduler or thread state here any more.

### 197.3 The guest's IPU contract, read from the code (this is what has to be implemented)

`FUN_00129258` (94 insns) is the picture/block decode batch:

```
0x1292A4  sw  s4, (0x10002000)   ; s4 = 0x30000000  -> IPU_CMD = VDEC
0x1292AC  jal 0x129538           ; wait for the command + feed the bitstream
0x1292C8  lw  a1, (0x10002020)   ; IPU_BP
0x1292CC  ld  a0, (0x10002030)   ; IPU_TOP (64-bit)
0x1292DC  sw  v0, 0x848(s1)      ; picture top  = (int32)IPU_TOP
0x1292F0  sw  v0, 0x84C(s1)      ; bit offset   = 0x20 - (BP & 0x1f)
```

`FUN_00129538` is the command/bitstream pump:

- polls `IPU_CMD` until bit 31 (busy) clears, then returns it - **the low bits of the finished command
  are the number of macroblocks that command consumed** (`FUN_00129258` accumulates them:
  `iVar6 += (short)iVar2`, and `iVar2 == 0` means "picture finished");
- polls `IPU_CTRL` bit `0x4000` (busy);
- tests FIFO occupancy `((BP & 0xff00) >> 1) + ((BP & 0x30000) >> 9) - (BP & 0x7f) < 0x20` and
  `DMAC_4_IPU_TO_QWC` before refilling through `FUN_0012cde8(param+0x868)`;
- gives up after 500 iterations and errors out through `FUN_0012e560`.

The decode driver is `entry_0012dec4` (`data/output_fixed/entry_0012dec4_0x12dec4.cpp`), the only code
that touches the DMA side:

| guest address | register |
|---|---|
| `0x10002000` | IPU_CMD (`sw`) |
| `0x1000B000` | `IPU_FROM` DMA (`D0`-style: MADR/QWC/CHCR; the code reads it back and tests `0x100`) |
| `0x1000B400` | `IPU_TO` DMA - the decoded picture comes back through **this** channel |

Call chain (Ghidra callgraph): `FUN_0012ce60 -> FUN_0012c748 -> FUN_0012cff8 -> FUN_001281c8 ->
FUN_00128408 -> {FUN_0012aa88 start-picture, FUN_00129258 VDEC, FUN_0012bb98, FUN_001296b8 bit reader}`.
`FUN_0012aa88` parses the picture header with its own EE-side bit reader
(`FUN_001297a0`/`FUN_001296b8`/`thunk_FUN_00129830`) and then hands the macroblocks to the IPU. So the
**EE does the header/quantiser work and the IPU does the VLC + IDCT + reconstruction**: a faithful
implementation must be a real IPU, not an FFmpeg shortcut, because the guest consumes the hardware's
per-command macroblock counts and `IPU_TOP` to advance its own bitstream.

The runtime today treats the IPU as a write-only register file
(`ps2_memory.cpp`: `0x10002000..0x10002030` store/echo, `IPU_CTRL` bit 31 masked) plus an immediate
"complete with zeros" DMA stub for `IPU_TO`/`IPU_FROM` (`TOL_NO_IPU_STUB=1` disables it). That is why
nothing decodes. `Kernel/Stubs/IPU.cpp` only fakes `sceIpuInit/RestartDMA/StopDMA/Sync`, and the game
does not import them anyway.

### 197.4 What this changes about the milestone

The milestone cannot be met by fixing the loader, the scheduler, the presenter or the scene object:
the startup logos and the title background are MPEG-2 video decoded by the IPU. The remaining work is a
**hardware substitution**: implement the PS2 IPU behind its real register/DMA contract, backed by a
native MPEG-2 decode core, and let the guest's own CRI Sofdec path drive it. That is exactly the
"replace PS2 hardware with native systems, keep the original executable logic" rule this port is built
on. The spec is already in this repo's skill set: `ps2-recomp/resources/09-ps2tek.md` §IPU
(`10002000h IPU_CMD`: BCLR/IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/SETTH/SETD4, plus the `IPU_BP`/`IPU_TOP`
bit layouts).

### 197.5 Order of work (staged, each stage independently testable)

1. **IPU register block, honest semantics** - `IPU_CMD` busy/result, `IPU_CTRL` busy/reset,
   `IPU_BP`/`IPU_TOP` real values, `IPU_IN_FIFO`/`IPU_OUT_FIFO` as byte queues. Exit test: the guest's
   `FUN_00129538` loop stops timing out (no `FUN_0012e560` error path).
2. **DMA wiring** - `IPU_FROM` (0x1000B000) must push the guest's bitstream bytes into the input queue
   instead of completing with zeros; `IPU_TO` (0x1000B400) must deliver decoded macrobocks to the
   requested RDRAM address. Exit test: `[ipu-dma] FROM ... qwc=` lines appear with real `MADR`s, and
   `IPU_BP` occupancy moves.
3. **VDEC / BDEC decode core** - the actual MPEG-2 VLC + dequant + IDCT + reconstruction, reporting the
   per-command macroblock count in `IPU_CMD[6:0]` and the picture top in `IPU_TOP`. Exit test: the first
   logo card appears (`tex builds > 0`, `gsw` climbing, `[gsorder]` textured draws).
4. Only after (3): re-measure the three logo cards, the opening movie and the title against
   `logs/refseq2/`.

Do **not** re-investigate in the meantime: preemption mode, the scene object/entities, PMODE/DISPFB2,
the presenter, the 224/280->0 copy, `FUN_0027AD50`, the `0x3DB660`/`0x1562FA0` tables, or thread state
- all were measured this month and are not the gate for the logo/title sequence.

### 197.6 Tooling added this session

| tool | purpose |
|---|---|
| `tools/refboot_seq.ps1` | kill + `-fastboot` relaunch PCSX2, then window-cropped screen captures on a timer (used for the table in 197.1) |
| `tools/montage.ps1` | tile a folder of captures into one review sheet |
| `tools/shotscreen.ps1`, `tools/shotwin.ps1`, `tools/shotclient.ps1` | window / client / full-screen capture (the old captures were of the wrong window: PCSX2 opens at `1383,695` and must be moved to `0,0`) |

Measured facts worth keeping: `TOL_TRACE_CALL` prints only `seq <= 25 || seq % 200 == 0`, so the `#seq`
on the last line is the true count; and the port's frame body (`0x165B88..0x165BD8`) really does run at
**~21 fps** until ~50 s, then the main thread moves into `FUN_00206310` (the unit/movie loop at
`0x206364`) which spins at ~33 Hz calling `FUN_00152B70`. That is the loop that is waiting for video.
### 197.7 The IPU does not have to be written from scratch: a complete one already exists locally

Found while checking the other PS2 recomp trees on this machine:

```
D:\Personal\C++\grandia-ps2recomp\runtime\src\hw\ipu.cpp        34,487 B / 955 lines
D:\Personal\C++\grandia-ps2recomp\runtime\src\hw\ipu.h
D:\Personal\C++\grandia-ps2recomp\runtime\src\hw\mpeg_tables.h  11,456 B (VLC + non-linear quant tables)
```

It is a real PS2 IPU, not a stub: `build_lut` over the MPEG-2 VLC tables (MBA/MBI/MBP/MBB/CBP/MC/DMV/DCL/DCC
+ escape tables), `decode_intra_block` / `decode_nonintra_block`, an 8x8 IDCT, `qscale()` with the non-linear
quantiser, DC predictors per component, a picture/reconstruction path with I/P/B handling, `csc_mb` for the
YCbCr->RGBA16 output with dithering, an `IPU_IN`/`IPU_OUT` FIFO model with `g_bitpos` over a 128-bit window,
`fill_input()` driven by DMA channel 4 (IPU_FROM), and `drain_output()` driven by channel 3 (IPU_TO).

Its public surface is exactly what this runtime needs:

```c
void init();
u32  read_reg(u32 addr);          // IPU_CMD / CTRL / BP / TOP
void write_reg(u32 addr, u32 v);
void fifo_read(r128* out);        // IPU_OUT_FIFO
void fifo_write(const r128* v);   // IPU_IN_FIFO
void dma_in(const u8* data, u32 qwc);
void dma_in_start();              // IPU_FROM
void dma_out_start();             // IPU_TO
void kick();
u64  command_count();
```

Coupling to its own project is small and mechanical: 23 `hw::` references (almost all `hw::dma[3]`/`hw::dma[4]`
`madr`/`qwc`/`chcr`/`tadr` plus one `hw::d_enable`), 12 `log_write`, 3 `mem::`, 3 `kernel::`, 1 savestate macro,
2 `r128`. The adaptation is therefore a shim, not a rewrite:

| grandia dependency | PS2Recomp equivalent |
|---|---|
| `hw::dma[3]` / `hw::dma[4]` fields | the `m_ioRegisters` words at `0x1000B400`/`0x1000B000` that `ps2_memory.cpp` already stores on CHCR writes |
| `hw::d_enable` | DPCR/D_STAT handling already in `ps2_memory.cpp` |
| `mem::read*`/`mem::write*` | `rdram` (`PS2Memory::getRDRAM()`), 32 MB, same layout |
| `log_write(LOG_IPU, ...)` | `PS2_LOG_*` / `fprintf(stderr, ...)` |
| `r128` | `__m128i` or a 16-byte POD |
| savestate `s.v(...)` | delete (no savestate in this runtime) |

Integration points, all in existing code:

1. `ps2_memory.cpp` write path: `0x10002000..0x10002030` -> `ipu::write_reg`; `0x10007010..0x1000701F` ->
   `ipu::fifo_write` (the four `sw`s at `0x10007010/14/18/1C` are one 128-bit FIFO write, as `IPU.cpp` already
   assumes).
2. `ps2_memory.cpp` read path: `0x10002000..0x10002030` -> `ipu::read_reg`; `0x10007000` -> `ipu::fifo_read`.
   This is what makes the guest's `FUN_00129538` busy poll terminate and `FUN_00129258` get real `IPU_BP`/`IPU_TOP`.
3. `ps2_memory.cpp` DMA path: on `IPU_FROM` (0x1000B000) CHCR start -> `ipu::dma_in_start()` and feed the buffer;
   on `IPU_TO` (0x1000B400) CHCR start -> `ipu::dma_out_start()`. The existing "complete immediately with zeros"
   stub (`TOL_NO_IPU_STUB`) must be replaced by this, otherwise the picture never arrives.
4. `Kernel/Stubs/IPU.cpp::completeIpuInit` can stay (it only seeds the tables the guest also seeds itself).

Expected first measurable result, on the existing build and existing probes: `[ipu-dma] FROM ... qwc=`,
`tex builds > 0`, `gsw` climbing, `gif` climbing, `[gsorder]` textured draws, and the **namco** card on screen at
the point where `FUN_00204240` mode 1 runs.

Open question to settle while adapting (needs one trace of the reference): whether this game's stream is plain
MPEG-2 elementary video inside PSS (what `mpeg_tables.h` + `decode_*` handle) or needs the `_last_block` /
sequence-header init that `g_in_last_block` implements. `FUN_0012AA88` parses the picture header on the EE and
only hands macroblocks to the IPU, so the IPU's own sequence/picture-header parsing must be tolerant of being
entered mid-picture - check `write_reg`'s handling of `SETIQ`/`SETVQ`/`SETD4`/`BCLR`/`FDEC` and the `g_cmd_func`
path before trusting it blind.

### 197.8 Honest status at the end of session 197

- **Not reached:** the milestone. No logo, no title. The window still shows the stale NOW LOADING frame.
- **Established beyond doubt:** the startup logos and the title are MPEG-2 video decoded by the IPU, and the
  port has no IPU. That is the single gate for "original boot sequence to title screen".
- **De-risked:** a complete, real IPU implementation was located on this machine and its integration surface
  mapped to this runtime's existing hooks (197.7). The remaining work is an adaptation plus DMA wiring, not an
  MPEG-2 decoder from scratch.
- **Next session starts at 197.7 step 1** (registers), then step 3 (DMA), then runs the logo sequence against
  `logs/refseq2/`.
### 197.9 CORRECTION to 197.1/197.4 - the three LOGO CARDS are not video; the IPU starts at the OPENING MOVIE

A breakpoint trace on the reference settles the split. Fresh `-fastboot` boot, trace armed ~9 s after launch
(so add ~9 s to the times below to get wall time):

```
0x00204240  t= 3.01s mode=1        -> absolute ~12.0 s
0x00204240  t= 8.18s mode=2        -> absolute ~17.2 s
0x00204240  t=13.20s mode=3        -> absolute ~22.2 s
0x0012dec4  (IPU DMA driver)       -> 0 hits in a 16 s window covering all three units
0x00128408  (MPEG picture decode)  -> first hit t=18.81s = absolute ~27.8 s, then ~20 hits/s
```

Mapped onto the captured sequence (197.1):

| phase | wall time | IPU? | mechanism |
|---|---|---|---|
| NOW LOADING | 5-11 s | no | guest 2D |
| **namco** card | 13.6 s | **no** | 2D image + fade (`FUN_00204240` mode 1) |
| **moth Productions** card | 18 s | **no** | 2D image + fade (mode 2) |
| **Sofdec / IBM** card | 22.5 s | **no** | 2D image + fade (mode 3) |
| black | 27 s | - | - |
| **opening movie** | 28-39 s | **yes**, from ~27.8 s at ~20 pictures/s | IPU MPEG-2 (VDEC), 640x320 29.97 fps, ADX audio |
| **TALES OF LEGENDIA title** | 40.6 s -> | yes (title background is video; the paused reference sits in `FUN_00129258`) | IPU |

So the gate is **two** defects, not one, and they are independent:

- **Gate A - the three logo cards (2D).** This is the *original* session-158 defect: the unit page receives only
  untextured black draws, `tex builds = 0`. The cards are LZSS-compressed images the unit loads and the guest
  uploads as GS textures, then fades. Nothing about this needs the IPU, so **it is the cheaper and nearer
  milestone deliverable**: fix the image/upload/draw path and the namco + moth + Sofdec cards appear.
- **Gate B - the opening movie and the title background (IPU).** Needs the IPU work in 197.3/197.7.

`MOVIE.AFS` is confirmed to be the story/opening CG only: the first 14 entries decode as ordinary CG
(`logs/movies/montage2.png`; entry 0 frame at 3 s is the teal sun-over-sea shot that the reference shows at
~30 s). `SYS_REG.AFS` is a real AFS (430 entries, first at 0x1000, 542,998 B) but contains **no** `00 00 01 BA`
pack headers, so the logo cards are image assets, not movies - consistent with the IPU being idle at 13-22 s.

Revised order of work:

1. **Gate A first.** Re-open the session-158 finding (`scene page 280 receives only untextured black`) from the
   *logo* side: identify the unit's image asset, its LZSS decode, its GS upload and the fade draw for
   `FUN_00204240` modes 1/2/3. Success test: `logs/refseq2/t014.png`-equivalent content at 13.6 s (namco),
   18 s (moth), 22.5 s (Sofdec/IBM); `tex builds > 0`, `transfers > 0`, `[gsorder]` textured draws.
2. **Gate B second.** Port the IPU (197.7), then the opening movie and the title background follow.
3. `../ps2_project_state.md` note: the "scene page 280 / no textured draws" defect is *the same* upload path as
   Gate A, so 1 is not extra work - it is the original blocker, now with a known target image.
### 197.10 GATE A, first pass (measured) - the card is built correctly and then nothing textured is drawn

Port probes (`TOL_TRACE_PC` on the card path, 115 s, `logs/run197/unitA.stderr.log`):

```
32.328  0x1F72F0  a0=1  a1=0x014E29E0   ra=0x00204268   <- unit 1 load request (mode = a0)
32.864  0x1531B0  a0=0x0063D8E0         ra=0x002042DC   <- relocate the loaded blob
32.865  0x239A90  a0=0x0063D910 a1=0x014E2A3C a3=0x014E2A30  ra=0x00204304  <- sprite ctor
32.865  0x1EBEA0  a0=0x0063D970 a1=0x00000011 a3=0x00000200  ra=0x00204324  <- type-0x11 node
32.865  0x1EC100  a0=0x0063D970 a1=0x0063D910 a2=0xE0 a3=0x200  ra=0x002044D0 <- attach
38.853  0x164B90  a0=0x0063D910         ra=0x00204468   <- destroy, 5.99 s later
39.545  ... same sequence for unit 2 (mode 2), destroyed 45.812
46.486  ... same sequence for unit 3 (mode 3), destroyed 52.508
```

So all three cards are **created, attached, held for ~6 s and destroyed on schedule**. The card lifetime is
correct (25-frame fade in + 90-frame hold + 25-frame fade out ~= 4.7 s at 30 fps, 6 s in the port's slower frame
cadence). Nothing is skipped on the guest side.

The card's sprite descriptor is **byte-identical to the reference**: a 64 KB dump of
`0x014E0000..0x014F0000` (`TOL_DUMP_RAM=0x014E0000,0x10000,...`) compared with the same range read from PCSX2
(`logs/rd197/p_11.bin` vs `logs/rd197/ref_14E0000.bin`) is **16384 of 16384 words identical**, including the
relocated sprite header at `0x014E2A30` (`BEAF0001`, children `0x014E2A3C/0x014E2AC0/0x014E2F50`). This range was
never covered by the session-160 unit diffs (`0x480000,0x200000`), so it is a genuinely new comparison - and it
matches.

What is drawn (`TOL_TRACE_GSORDER=1`, `logs/run197/unitB.stderr.log`):

- The only primitives in the card window are `FILL224 target=224 tme=0 iip=1 rgba=0,0,0,0` quads of 64x32 in a
  tiled sweep. **These are not the defect**: the same black tiled sweep starts at **3.14 s**, i.e. it is the
  game's ordinary per-tile screen clear and it runs under the *working* NOW LOADING frame too.
- The only textured (`tme=1`) primitives in the whole window are the known page-224 -> page-0 display copy
  (`tbp0=7168`, `rgba=128,128,128,128`, 20 vertical strips). **No textured draw from the card's image is ever
  issued.** The window capture during the cards is uniformly black (5 captures at 33/37/41/45/49 s, identical
  MD5, `logs/run197/unitC_*.png`).

So Gate A narrows to: the guest builds a valid card sprite and never submits it. Type/vtable facts gathered for the
next probe: the sprite is `vtable 0x003D4CA0` (`0x239AE0/0x239B40/0x239B50/0x239D00` at slots +8..+0x14,
`0x23A500/0x23A060` at +0x28/+0x2C), created by `FUN_00239a90` -> `FUN_00239b60`; the node it is attached to is
`vtable 0x003D4040` (`0x1EBF20/0x1EBFA0/0x1EC010/0x1EC080/0x1EC150` at +8..+0x18, `0x1EF180/0x1EF270/0x1EF280/
0x1EF7D0/0x1EF7F0` at +0x28..+0x38; `0x1EF180` is the destructor, it unlinks `+0x40`/`+0x44` and decrements the live
counter), and `FUN_001EC100` stores the sprite at `node+0x68`. The next probe is those vtable slots, to see which of
them the port never calls.

Tooling: `TOL_DUMP_VRAM` now accepts `TOL_DUMP_VRAM_FRAMES=<n[,n...]>` (default 150,400) so VRAM can be captured
inside the card window (~frames 1000-1500), which the default pair never reached.
### 197.11 Build trap found and fixed: `cmake.check_cache` is written AFTER `build.ninja`, so ninja re-runs CMake forever

Symptom: `ninja -C build/tol2 -j 12 tol` prints `[0/2] Re-checking globbed directories...` then
`[1/2] Re-running CMake...` and never gets further. Two attempts died on this (15 min, then 1 h, then 25 min,
with no `.obj` written and `tol.exe` untouched). The state file's older note ("re-runs CMake on every invocation,
~5 min") is the same defect seen from further away.

Cause, measured:

```
build\tol2\build.ninja                       11/22 00:08:42
build\tol2\CMakeFiles\cmake.check_cache      11/22 00:08:42   <- NEWER (same second, later fraction)
```

`cmake.check_cache` is an explicit input of the generated `RERUN_CMAKE` edge, and CMake writes it *after* the
ninja files. `build.ninja` is therefore permanently one instant older than one of its own inputs, so every ninja
invocation re-runs the configure, which rewrites both files in the same order, forever. Configuring itself is
only ~135 s (`Configuring done 3.8s / Generating done 130.8s`), so a loop looks like a hang.

The glob verify is NOT the problem: `cmake -P build/tol2/CMakeFiles/VerifyGlobs.cmake` prints **nothing** (no
`GLOB mismatch`), and both globs match exactly - `data/output_dense` has 13520 `.cpp` and `OLD_GLOB` lists 13520;
the second glob is `thirdparty/PS2Recomp/ps2xRuntime/src/runner/*.cpp`. `ninja -d explain -n` blames
`CMakeFiles/VerifyGlobs.cmake_force` (a phony that is dirty by design) and then reports the CMAKE edge, which is
the cascade from `check_cache`, not a real content change.

Fix (non-destructive, no clean, no deletion):

```powershell
(Get-Item build\tol2\build.ninja).LastWriteTime = (Get-Date).AddSeconds(5)
(Get-Item build\tol2\CMakeFiles\cmake.check_cache).LastWriteTime = (Get-Date).AddSeconds(-30)
```

After that no CMake input is newer than `build.ninja` (verified by scanning `_deps`, `out\sdl3`, `thirdparty`,
`src`, `cmake`, root `CMakeLists.txt`). Note `ninja -n` still prints "Re-running CMake" afterwards - that is a
dry-run artefact of the `_force` phony and is not what a real run does.

Also confirmed while diagnosing: the corpus objects hold - `build\tol2\...\output_dense` has **13519** `.obj`
(all 11/21 01:08); only `tol_overrides.cpp` needs recompiling plus the link, so this is a minutes-long build once
the loop is broken. `output_fixed` (53222 objs) is a *different, unused* corpus directory; the live one is
`data/output_dense` (`TOL_GENERATED_DIR` in `build/tol2/CMakeCache.txt`).


## Session 198 (2026-11-22) - the 2-hour build was a MALFORMED corpus, not a slow file; `data/output_dense` is deleted, the live corpus is `data/output_fixed`

### 198.1 Retraction of the last paragraph of 197.11

197.11 ended by saying `output_dense` was "the live one" and `output_fixed` "a different, unused corpus
directory". That is backwards, and it is what made the tree unbuildable. `output_dense` is the broken one and
it is now deleted.

### 198.2 The measurement that settles it

`entry_003a3198_0x3a3198.cpp` is the recompiler's PC-dispatch entry:
`switch (ctx->pc) { case 0x3a3198u: goto label_3a3198; ... }` - one case per entry address.

| corpus | files | entry_003a3198 | **case labels** | register_functions |
|---|---|---|---|---|
| `output` | 53,558 | 6.47 MB | **482** | 20.0 MB |
| `output_dense` | 13,520 | 11.54 MB | **47,418** | 62.0 MB |
| `output_fixed` | 13,520 | 9.49 MB | 784 | 61.3 MB |

A 482-case dispatch became **47,418** cases - one per instruction address instead of one per branch target, a
~100x expansion. That TU ran 112 min at `-O2` (6,076 s CPU) and 52 min at `-O0` (2,206 s CPU) without ever
writing an `.obj`. It was not slow; it was malformed.

### 198.3 Why it was being built at all

- `CMakeLists.txt:86` defaults `TOL_GENERATED_DIR` to `thirdparty/PS2Recomp/ps2xRuntime/src/runner`
  (53,558 files, byte-identical to `data/output` - verified by MD5 on `register_functions.cpp` and a sample).
- `build/tol2/CMakeCache.txt` had overridden it to `data/output_dense`.
- **No config in the repo produces that directory**: `config.toml` -> `data/output`; `config_fixed.toml` and
  `config_orig.toml` -> `data/output_fixed`; `config-linux.toml` -> `data/output`. `output_dense` was a stray.
- The working Nov 20 exe predates the dense files (2026-11-21 01:06), so it was never built from them.

### 198.4 Second defect: the oversized-TU guard never applied to clang-cl

`src/runtime/CMakeLists.txt` gated the ">2 MB -> no PCH, no optimisation" path on
`CMAKE_CXX_COMPILER_ID STREQUAL "MSVC"`. clang-cl reports `Clang`, so the guard never fired and every oversized
TU went to `-O2` with the PCH. Now `MATCHES "^(MSVC|Clang)$"` with `-O0` for Clang. Configure confirms:
`tol: 4 oversized translation units build without PCH at -O0`.

### 198.5 Cleanup performed

- `data/output_dense` **deleted** (13,520 `.cpp`, 279 MB). It was untracked by git and referenced by no config.
- `build/tol2/CMakeCache.txt`: `TOL_GENERATED_DIR` -> `data/output_fixed`.
- **No `.toml` referenced the dense corpus**, so there was no config file to delete. The four configs remain:
  `config.toml` (output), `config_fixed.toml` (output_fixed), `config_orig.toml` (output_fixed),
  `config-linux.toml` (output).

### 198.6 Result

Full rebuild of `data/output_fixed`: 13,528 edges, 12 workers, ~20 min, `EXIT=0`, zero errors.

```
build\tol2\src\runtime\tol.exe        95,888,896   2026-11-22 20:58:07
build\tol2\src\runtime\tol_prev.exe   97,803,776   2026-11-20 22:26:18   (rollback copy)
```

The exe is ~2 MB smaller because the 4 oversized TUs now build at `-O0`. If those functions regress at runtime,
restore `-O2` for just them.

### 198.7 Rule for next time

Do not reintroduce `data/output_dense`. A corpus is only valid if a `data/config*.toml` names it. Before
pointing `TOL_GENERATED_DIR` at a directory, check `entry_003a3198_0x3a3198.cpp`: ~500 case labels is normal,
~47,000 means the generation is broken and the build will never finish.
### 197.12 GATE A, second pass (measured on the rebuilt binary) - the card IS submitted, but no textured primitive reaches the GS, and the rebuild regressed the whole GS path

Build notes first, because they bit us:

- The exe the user built at 20:58 contained **no renderer at all**: `TOL_GFX_ENABLE_SDLGPU:BOOL=ON` but
  `SDL3_DIR:PATH=SDL3_DIR-NOTFOUND`, so `tol_gfx_sdlgpu` was never generated, `build.ninja` had no reference to
  it, and the binary contained none of the `--gs=sdlgpu` / `sdlgpu` strings. `out/sdl3` had been deleted.
  Fixed by re-extracting `%TEMP%\opencode\SDL3-devel.zip` to `out/sdl3` and reconfiguring with
  `-DSDL3_DIR=D:/.../out/sdl3/SDL3-3.4.18/cmake`; configure then reports "SDL GPU backend available
  (--gs=sdlgpu)". Generating took 500 s.
- The corpus in use is **`data/output_fixed`** (the known-good one), not `output_dense`; `tol_generated.lib`
  (270.7 MB) was rebuilt 11/22 20:58 from its 13,519 objects. `ps2_runtime.lib` is untouched (11/20 22:26), and
  the corpus compile flags are correct (`/EHsc -std:c++20 -O2 -msse4.1 -fno-strict-aliasing -fwrapv`).
- Cost of an instrument-only iteration: one TU (`sdlgpu_backend.cpp`) + link, ~2 min. The build loop trap
  (197.11) has to be re-broken after every configure.

New, reusable probes in `src/gfx/backends/sdlgpu/sdlgpu_backend.cpp` (all env-gated):

| env | what |
|---|---|
| `TOL_TRACE_GSPRIM_WINDOW=<from>,<to>[,max]` | every primitive in a wall-clock window (proved useless: the port's speed varies >3x run to run) |
| `TOL_TRACE_GSPRIM_FILTER=1` | every primitive **except** the known steady-state classes (tiled clear, page-224->0 copy, page-280->0 copy, black clear-copy, page-112 staging sprites), cap 2000 |
| `TOL_TRACE_GSPRIM_EMPTY=1` | every batch dropped because `primitiveBounds()` is empty - a silent drop path that hid draws completely |
| `TOL_DUMP_VRAM_FRAMES=<n[,n...]>` | frame numbers for the existing `TOL_DUMP_VRAM` dump (default 150,400 never reached the cards) |

New tool `tools/vram_page.py <vram.bin> <fbp> <out.png> [w] [h]` renders a CT32 page of a VRAM dump.

Measured facts on the rebuilt binary:

1. **The card path runs and submits.** `TOL_TRACE_PC` on the chain: `0x1EC150` (node draw) is called with the
   card node `0x0063D970` 32 of its first 40 calls; `0x239D00` (sprite submit, `a0=0x0063D910`) runs from
   inside it (`ra=0x001EC290`); `0x15C3B0` (vertex) and `0x15BF60` (state push) also run. So the guest
   *does* draw the card - 197.10's "never drawn" is superseded.
2. **But no textured primitive reaches the backend.** With the filter probe (2000 lines covering 3.9-95 s,
   i.e. unit 1's whole window at 86-92 s) the only classes are `fbp0/tme0/tbp8960` (x1571, untextured quads
   into page 0 sampling page 280), the 5 early UI panels (`fbp224/tme1/tbp14976`, 11 s) and 2 misc. No
   `tme=1` card draw at all.
3. **The dropped-batch path is not it**: every empty-bounds drop is at 7.0-7.2 s and is a `type=4` clear.
4. **GS VRAM is completely empty**: dumps at frames 230/240/250/260/270/280 render pages 0, 224 and 280 as
   **0 non-black pixels of 229,376** each. There is no artwork anywhere in VRAM.
5. **The guest emits almost no GIF traffic**: a clean run (no env vars) ends at `gif=5 gsw=0` after 120 s,
   where the 11/20 binary logged `gif packets=720` in 57 s and `transfers=858 (110808 KiB)`. `mainIters` is
   healthy (1969 at 120 s, ~16/s) so the frame loop is running.

Conclusion so far: this binary is a **regression of the whole GS submission path**, not a card-specific defect.
The only input that changed is the corpus objects, recompiled today from the same `output_fixed` sources; the
runtime library, the gfx sources and the compiler flags are unchanged. Next step is to establish that with a
controlled rebuild (dense corpus, or the user's original objects), because no card-side fix can help while the
guest uploads nothing.

Timeline note for future runs on this binary: units now load at **86 / 100 / 114 s** and the frame tick runs
at ~2.7 fps, versus 32 / 39 / 46 s and 19.6 fps on the 11/20 binary - so any wall-clock window written against
the old numbers will miss.
### 197.13 Dense corpus recreated and built; the build step that "gets stuck" is ninja's CMake/glob step, not the compile

`data/output_dense` **had been deleted** (only `output`, `output_fixed`, `output_bak_17-10-2026`,
`output_base_17-10-2026` remained), which is why the dense build could never produce an exe: the CMake glob finds no
sources there. It was recreated, not restored from a backup:

```
data/config_dense.toml          = config_fixed.toml with output -> data/output_dense
build/recomp/ps2xRecomp/ps2_recomp.exe data\config_dense.toml     (~14 min, exit 0)
  -> 13520 .cpp, 278 MB, register_functions.cpp 59.1 MB
  -> verification cases: entry_00205f68=38, entry_00165210=16, entry_00231e54=23   (identical to the old dense)
```

The recompiler prints `unhandled-instruction` for `entry_003a3198` (data decoded as code) but still reports
"Recompilation completed successfully", same as before.

Build path used, because `ninja -C build/tol2 ... tol` **hangs at `[0/2] Re-checking globbed directories...`** for
20+ min with no compiler running and no `build.ninja` rewrite, while the same verify step run by hand is 1.7 s and
reports no mismatch (`cmake -P build/tol2/CMakeFiles/VerifyGlobs.cmake`). It is intermittent - one run got to
`[13521/13525]`. Workaround that works:

1. Point the build at dense and fix the `cmake.check_cache` loop (197.11):
   `cmake -S . -B build/tol2 -DTOL_GENERATED_DIR=.../data/output_dense -DSDL3_DIR=.../out/sdl3/SDL3-3.4.18/cmake`
2. Backdate `data/output_dense` to the corpus's original mtime (2026-11-21 01:06:00) so the 13,519 existing,
   content-equivalent objects are reused instead of recompiling all 13,520 TUs. Verified the regenerated corpus is
   the same one those objects came from by the case counts and the 59.1 MB / 278 MB sizes.
3. `ninja -C build/tol2 -t commands <target>` works instantly (it does not run the glob step) - use it to extract
   the exact archive and link command lines.
4. Archive by hand: generate an rsp with the 13,520 object paths and run
   `llvm-lib.exe /nologo /out:src\runtime\tol_generated.lib @mygen.rsp` from `build/tol2` (272.9 MB, exit 0).
5. `entry_003a3198_0x3a3198.cpp` is the only missing object and the single slowest TU in the corpus: with dense
   `case` labels it needs **more than 23 minutes** and must be run detached (`Start-Process cmd /c cc_monster.bat`)
   or it is killed by a tool timeout. Compile command extracted from `cmd_lib.txt`.
6. Then re-archive and run the exe link line (the `cmake -E vs_link_exe ... lld-link ...` command from
   `ninja -t commands src/runtime/tol.exe`).

Status at the time of writing: dense `tol_generated.lib` archived (272.9 MB) **without** `entry_003a3198`; that TU
compiling detached; `tol.exe` still the 11/22 22:22 binary. The next step after it lands is re-archive + link, then
the Gate A probe run on the dense binary.
### 197.14 CORRECTION: the GS path is NOT dead - 105 MB of textures are uploaded. The defect is the opaque untextured display copy

The "dead GS path" conclusion in 197.12 was wrong and is retracted. It came from three misread instruments:

- `[gfx] ... transfers=... gif=...` lines no longer print at all (they are sdlgpu-only and gated differently), so
  the `gif=5 gsw=0` figures I quoted come from a *different* counter than the transfer byte count.
- The GS debug-history census (`tags/draws/transfers`) is a **rolling 512-event window**, so identical numbers
  across samples mean a steady workload, not a stall.
- `TOL_TRACE_GSPRIV` never shows `TRXDIR`/`BITBLT` because this title uploads with GIF **IMAGE tags**; absence of
  those registers is normal on PS2.

New probe `TOL_TRACE_GSIMAGE=1` in `gs_frontend.cpp` (submodule) counts GIF IMAGE tags in
`visitPackedGifPacket`. Healthy 90 s run:

```
[   3.460] [gsimage] tag#1    nloop=4    bytes=64     totalBytes=64
[   3.460] [gsimage] tag#2    nloop=1024 bytes=16384  totalBytes=16448
...
[  34.126] [gsimage] tag#14848 nloop=64  bytes=1024   totalBytes=4252752
[  52.736] [gsimage] tag#15616 nloop=64  bytes=1024   totalBytes=105309264
```

**15,616 IMAGE tags, 105,309,264 bytes uploaded by 52.7 s** (the 11/20 binary logged 110,808 KiB - same thing).
`g/s=0.528`, `mainIters +8`: that run is at the old, healthy speed, so the earlier 86/100/114 s timeline was
run-to-run variance, exactly as the state file warns.

What is actually wrong, measured in a healthy run with `TOL_TRACE_GSPRIM_FILTER=1` (units at 34.2 / 41.2 / 48.4 s):

| class | count | meaning |
|---|---|---|
| `fbp0/tme0/tbp8960` | 284 | **the display copy, one per frame, UNTEXTURED, sampling page 280** |
| `fbp224/tme1/tbp14976` | 5 | the one working textured sprite (an 11 s UI panel) |
| `fbp0/tme0/tbp0` | 2 | misc |

So: the guest draws exactly one full-screen quad per frame into page 0, from page 280, with **TME = 0**, i.e. a flat
colour and not the framebuffer contents. That is why the window is black while the upload path is healthy, and why
no logo-card artwork appears: **no textured draw for the card sprite reaches the backend at all** (the node draw
`0x1EC150` and sprite submit `0x239D00` do run - see 197.10/197.12).

Caveat on VRAM: `TOL_DUMP_VRAM` dumps `PS2Memory::getGSVRAM()`, which only ever holds ~261 KB (one CLUT+texture at
`0x3A7000`, `tbp0=14976`) even when 105 MB has been uploaded - the live VRAM is the backend's `GsVram`. Do not use
`TOL_DUMP_VRAM` to judge whether uploads land; use `TOL_TRACE_GSIMAGE` plus the draw probe.

A/B against the 11/20 reference at the unit boundaries (`TOL_DUMP_UNITS=0x480000,0x200000,...` vs
`logs/ramdump/ref_unit*`): session 160 reported the structures identical; they now differ by
7,266 / 16,868 / 16,986 / 66,097 / 28,366 / 29,393 words, first difference always in `0x48F7xx-0x48F8xx` (the guest
scheduler's ready bitmap / priority counts / context slots area). Session 160 already classified that range as
transient, but the counts are far larger than the "0 differing words" baseline, so the guest's own scheduler state
has drifted in this rebuild.

Next steps, in order:
1. Print the guest TEX0 (`tbp0/tbw/psm/tcc/tfx/tme`) and the source page for the one copy draw per frame, and find
   the guest function that emits it (the copy is built where the guest copies 224/280 -> 0). Compare its TME with
   what the same site emits for the 11 s UI panel, which is textured and works.
2. Determine whether page 280 ever receives a draw: the census shows draws only to `fbp 224` and `fbp 0` (plus the
   staging page 112). If nothing writes 280, the copy can only ever read black whatever its TME is - that would make
   the untextured copy a red herring and point back at the missing card draw.
3. Re-run 197.10's chain on the healthy build (the earlier chain run was on a slow run) to see whether `0x239D00`
   is reached during the card's 6 s window and with which `sprite+0x20` texture handle.
### 197.15 All three probes run: the card draws into page 224/112 while the display copies page 280

Probe added: `TOL_TRACE_GSDRAWS=1` (`sdlgpu_backend.cpp::submit`) - prints every distinct
`(framebuffer, tme, texture page)` draw class the moment it first appears, with the full TEX0, both vertex
colours, the written rect and the running total. Timing-robust, so run-to-run speed cannot hide a class.

One 75 s healthy run (units at 34.2 / 41.2 / 48.4 s) produced exactly **11 draw classes in 303,000 draws**:

| # | fbp | tme | type | tbp0 | geometry | when | what |
|---|---|---|---|---|---|---|---|
| 1 | 0 | 0 | 6 | 0 | 640x896 | 3.4 s | startup blank |
| 2 | 0 | 1 | 6 | 7168 | 65x448 strips | 3.4 s | display copy from page 224 (textured, correct) |
| 3 | 0 | 0 | 6 | 7168 | 512x2 | 3.5 s | top edge fill of that copy |
| 4 | 224 | 0 | 4 | 7168 | 65x33 tiles | 3.5 s | the tiled black clear |
| 5 | 224 | 1 | 6 | 14976 (PSMT4) | 258x130 | 3.6 s | **the only textured sprite in the run** (UI panel) |
| 6 | 112 | 0 | 6 | 7168 | 65x225 | 34.1 s | staging strip |
| 7 | 224 | 0 | 4 | 16368 | 65x33 | 35.7 s | clear, new texture page 16368 (=0x3FF000) bound |
| 8 | 112 | 0 | 6 | 16368 | 65x225 | 35.7 s | staging, **untextured** |
| 9 | 0 | 1 | 6 | 8960 | 65x448 strips | **57.25 s** | display copy from page 280 - **textured, correct** |
| 10 | 0 | 0 | 6 | 8960 | 640x2 | 57.25 s | top edge fill of that copy |
| 11 | 280 | 0 | 4 | 8960 | 65x33 tiles | **57.25 s** | **the only thing ever drawn into page 280: the tiled black clear** |

Three conclusions, each of which retires an earlier guess:

1. **The display copy is NOT untextured.** Class 9 is `tme=1 tbp0=8960 rgba=128,128,128,128` - a correct textured
   copy from page 280. The `fbp0/tme0/tbp8960` stream I flagged in 197.14 is the 640x2 **edge fill** (class 10).
   The copy path is fine.
2. **Page 280 is drawn to, but only with the black clear.** No content class ever targets 280. So the copy is
   faithful and the screen shows the clear - the artwork never arrives in the page the display reads.
3. **The logo-card content goes to page 224 and the staging page 112**, with a *new* texture page `16368`
   (`0x3FF000`, the last ~4 KB of VRAM) bound from 35.7 s, and it is always drawn **untextured** (`tme=0`).
   Class 8 is the smoking gun: a sprite into page 112 whose TEX0 points at 16368 but whose `tme` is 0.

So the defect is a **draw-target / texture-enable mismatch on the card sprite**, not the display path and not the
upload path (105 MB of IMAGE tags land - 197.14).

Probe 3, the card sprite object itself (`TOL_DUMP_RAM=0x0063D900,0x100,...`, dumps every ~3 s). At `0x0063D910`,
during the cards:

```
+00 003D4CA0 00000002 00000000 00000000    <- vtable = sprite type 0x3D4CA0
+10 00000000 00E0FF00 02000200 80808080    <- x=-256  y=224  w=512  h=2   rgba=128,128,128,128
+20 014E2A3C 00000000 00000000 3F800000    <- texture handle = 0x014E2A3C (a pointer into the loaded blob)
+30 3F800000 ...
+40 4D444E52 0063D8F0 0063D9E0             <- "RNDM"
```

and the sibling node at `0x0063D970` carries vtable `0x003D4040`, so both objects are alive and correctly typed
while the card is up.

Two facts from that dump matter:

- The sprite's geometry is **512 x 2** (`+0x14=-256, +0x16=224, +0x18=512, +0x1a=2`), i.e. the quad
  `FUN_00239D00` builds from those fields is a 2-pixel-tall strip, not a card-sized image. That is exactly the
  `written=[0,0..512,2]` edge-fill shape seen in class 3/10, so the card sprite and the copy's edge fill share a
  geometry source - worth checking whether `+0x1a` is really "height" or whether the real height lives elsewhere
  (e.g. in the blob at `+0x20`).
- The texture handle is a **raw RDRAM pointer** (`0x014E2A3C`), which `FUN_00239D00` writes straight into the GS
  context (`gsctx+0x12e0`). If the renderer expects a resolved GS TEX0/VRAM page there, the write is harmless and
  the draw stays `tme=0` - which is precisely the observed class 8.

Next: disassemble `FUN_00239B60` (sprite ctor, sets `+0x14..+0x20` from the blob) and `FUN_0015C3B0` /
`FUN_00160390` (vertex submit / flush) to see how `gsctx+0x12e0` is turned into TEX0, and where `+0x1a` comes
from. That pins whether the fix is the sprite's geometry fields (guest data) or the TEX0 resolution (renderer).
### 197.16 Traced the sprite ctor + submit path: the card flushes through DMAC channel 8 (SPR_FROM), which was never routed

Ghidra, `FUN_00239b60` (sprite ctor, called by `FUN_00239a90`):

```c
sprite->+0x20 = blob;                       // the loaded sprite descriptor
if (param_3) {
  iVar1 = *(int *)(*(int *)(sprite + 0x20) + 8);          // = the blob's TEX0 block
  w = 1 << ((*(u8  *)(iVar1 + 0x43) << 58) >> 60);        // TEX0 TW  = bits 26..29
  h = 1 << ((*(u64 *)(iVar1 + 0x40) << 30) >> 60);        // TEX0 TH  = bits 30..33
  sprite->+0x14 = -(w >> 1);  +0x16 = h >> 1;  +0x18 = w;  +0x1a = h;
}
```

So the `512 x 2` geometry measured in 197.15 is **derived from the blob's own TEX0** (`TW=9, TH=1`), and that same
TEX0 supplies `tbp0=16368`, `psm=20` for the draw. The sprite is not corrupt; it faithfully reports a 512x2,
PSMT4 texture descriptor. Either the blob really is a 2-pixel strip, or the field at `*(blob+8)+0x40` is a
placeholder the game patches after upload (worth re-checking against PCSX2 at the same milestone).

`FUN_0015c3b0` (vertex submit) is the important one:

```c
p = ctx + ctx->+0x1250 * 0x900 + ctx->+0x1254 * 0x30;   // 48-vertex bank, 0x30 bytes/vertex
*p = v0; *(p+1) = v1; *(p+2) = v2;
if (++ctx->+0x1254 == 0x30) {
    base = ctx + ctx->+0x1250 * 0x900;
    (*(code**)(ctx + 0x1248))();                        // guest callback: build the packet
    (*(code**)(ctx + 0x124c))(base, base + 0x900, ctx + 0x1230);
    ctx->+0x125c += 0x30 * 0x30;
    ... wait for CHCR.100 ...
    REG_DMAC_8_SPR_FROM_SADR = base;                    // 28-bit masked
    REG_DMAC_8_SPR_FROM_MADR = ctx->+0x125c;
    REG_DMAC_8_SPR_FROM_QWC  = 0x90;
    REG_DMAC_8_SPR_FROM_CHCR = 0x100;
}
```

i.e. the sprite engine flushes its vertices with a **DMAC channel 8 (SPR_FROM)** transfer out of the GS context
buffer, not with GIF. `PS2Memory` only routed channels 0/1/2 (VIF0/VIF1/GIF) plus IPU - **channel 8 was never
handled at all**, so every sprite flush was silently dropped. Confirmed live: 80 CHCR writes to `0x1000D000`,
first at 32.2 s, all from `pc=0x0015C38C` (inside `FUN_0015c3b0`, `ra=0x0015C318`).

Fix implemented in `ps2_memory.cpp`:

- added `0x1000D000u` to the GS-channel dispatch and to both enqueue sites (normal mode and chain mode), so SPR
  transfers land in `m_pendingGifTransfers` -> `submitGifPacket(GifPathId::Path3, ...)`;
- **source address**: SPR's source is `SADR` (`channelBase + 0x80`), stored 28-bit masked, and the buffer lives in
  the **scratchpad**, so the real source is `0x70000000 | (SADR & 0x0FFFFFFF)`. Using `MADR` (what every other FROM
  channel uses) read a different field and pushed zeros; using SADR unmasked read RDRAM 0x0 and also pushed zeros.
  Both were verified with `TOL_TRACE_SPRDMA=1`;
- **safety guard**: the block is only submitted to path 3 when its leading qword is a valid GIF tag
  (`flg <= 2`, `nloop != 0`, `1 <= nreg <= 16`), so a non-packet sprite block can never be misparsed and corrupt
  later draws.

Result: **no visible change yet, and no regression** (draw classes and totals unchanged; screenshot at 42 s still
black). Reason: the scratchpad block at `0x70000000` starts with `0x3F800000` (= 1.0f, vertex data) with
`flg=0, nloop=0`, i.e. **the payload is not a GIF packet**, so the guard correctly refuses it. The vertices are real,
the container is not GIF.

Next step (single, concrete): the packet is produced by the two guest callbacks stored in the GS context at
`+0x1248` and `+0x124c`. Dump those two words at runtime (they are function pointers into the corpus), identify the
functions, and read what they write into the scratchpad before the DMA. That decides whether SPR_FROM must be
decoded as raw sprite vertices (and converted to GS sprite primitives by the runtime) or whether a GIF tag is
written at a non-zero offset inside the 0x900-byte bank.
### 197.17 SPR payload decoded: it is raw 48-byte vertices, not a GIF packet - the sprite path needs a vertex->GS translator

With the probe moved ahead of the validity guard, the scratchpad bank the sprite engine DMAs was captured
(`TOL_DUMP_SPR=<path>`, `logs/run197/spr_bank.bin`, 0x1200 bytes):

```
== 0x000 ==
  +000 lo=0000000000000000 hi=0000000010000000
  +010 lo=3F0080813F008081 hi=3F8000003F008081      <- colour qword: ~(0.502,0.502,0.502,1.0)
  +020 lo=0000000000000000 hi=0000000042800000      <- (0,0,64,0)
v1 @0x030
  +000 lo=3F00000000000000 hi=0000000010000000      <- (0,0.5,0,0)
  +010 lo=3F0080813F008081 hi=3F8000003F008081
  +020 lo=C200000000000000 hi=0000000042800000      <- (0,-32,64,0)
v2 @0x060
  +000 lo=000000003F000000 hi=0000000010000000      <- (0.5,0,0,0)
  +020 lo=0000000042000000 hi=0000000042800000      <- (32,0,64,0)
```

Bank 1 (0x900) is entirely zero, so only bank 0 is in use.

Facts:

- each vertex is **0x30 bytes** = 3 qwords (position/uv, colour, extra) - exactly `FUN_0015c3b0`'s
  `ctx + bank*0x900 + index*0x30` stride and the 48-vertex flush threshold;
- the leading qword is vertex data, **not** a GIF tag (`flg=0, nloop=0, nreg=0`), so feeding it to
  `submitGifPacket(Path3, ...)` is wrong and the guard correctly refuses it;
- therefore SPR_FROM cannot be satisfied by routing; the runtime must **translate the vertex bank into GS
  primitives** (a sprite quad list) and submit those, which is what the real hardware's sprite path effectively
  does for the SPR library.

Also re-tested the presenter hypothesis that the frame is simply never flipped:
`TOL_PREFER_FRAME_SOURCE=1` forces the presenter to read the page the draws target instead of DISPFB's page.
It switched the source (`FORCED source=fbp224 ... (display was fbp0)`) and the window was **still black**, so the
presenter is not the gate: page 224 genuinely holds no content, because the content that should be in it is drawn
through the dropped SPR vertex path.

Remaining work, in order:
1. In `ps2_memory.cpp`, for channel 0x1000D000, decode the 0x900-byte bank as 48 vertices of 3 qwords and submit
   them as GS sprite primitives through the existing `submitGifPacket`/frontend path (build a REGLIST packet:
   PRIM/PRMODECONT untouched, RGBAQ from the colour qword, ST/UV from the position qword, XYZ2 from the
   extra qword), preserving the current TEX0.
2. Verify with `TOL_TRACE_GSDRAWS=1` that a **new class** appears with `fbp=224` and the card's texture page
   `tbp0=16368`, and with the window capture that the namco card is visible.
3. Only then re-check whether page 280 receives scene content (197.15 class 11) - that is the separate
   scene-start issue, not this one.
### 197.18 Where PRIM.TME comes from: `ctx+0x12fc` is the PRIM word, and the texture binding takes a pointer path

Decompiled the three functions on the sprite draw's GS path:

`FUN_001602c0(ctx, prim)` - builds PRIM:

```c
(*(code**)(ctx + 0x1240))();  (*(code**)(ctx + 0x1244))();
ctx->+0x12fc = (ctx->+0x12fc & 0xfffffff1) | (prim * 2) | 1;
```

So **`ctx+0x12fc` is the PRIM register word**, and the mask `0xfffffff1` clears bits 1..3 only - **bit 4 (TME)
is preserved**. With the card's call `FUN_001602c0(ctx, 4)` the word becomes `0x9` = type 1, IIP 1, **TME 0**. So
this site does not *clear* TME; whoever wants a textured sprite must have set bit 4 (0x10) in that word earlier,
and it simply never is for class 8. That is the concrete statement of the defect.

`FUN_00160390(ctx)` - flush, builds the packet header the guest really uses:

```c
uVar2 = FUN_0015c2a0() * 3 + 1;                       // qwords
*(u64*)(ctx+0x12f4)      = nloop | 0x8000(EOP) | ((u64)ctx->+0x12fc << 46) | 0x3000000000000000;
*(u64*)(ctx+0x12f4 + 8)  = 0x512;                     // REGS nibbles: 2=ST, 1=RGBAQ, 5=XYZ2
*(u64*)(ctx+0x12f0)      = (qwords) | 0x10000000;
```

i.e. **FLG=0 (PACKED), NREG=3, REGS=0x512, PRE=1** (`PRIM << 46` sets bit 46). My `TOL_SPR_VERTEX` translator used
a REGLIST packet with REGS `0x01|0x02<<4|0x05<<8`; mirroring the guest's own PACKED layout (PRE=1, NREG=3,
REGS=0x512, PRIM in the tag, then per vertex ST/RGBAQ/XYZ2) is the correct shape and is the next edit.

`FUN_00160040(ctx)` -> `FUN_001593a0(ctx+0x12e0, cursor)` - binds the texture:

```c
if ((short)desc[2] == -1) {                 // desc[2] = ctx+0x12e8, and FUN_00239d00 sets it to 0xffff
    if (*desc != desc[1]) { desc[1] = *desc;
        if (*desc) { uVar1 = *(u32*)(*desc + 4);      // <-- treated as a POINTER, +4 read as the TEX0 value
            if (uVar1) emit(0x50000000 | (u64)uVar1 << 32);
            if (*(u32*)(*desc))     emit(ptr[0] as TEX0);
            if (*(u32*)(*desc + 8)) emit(ptr[8] as TEX0);
        } } }
else { ... full descriptor path, writes 12 qwords including `| 0x400000000` (TEX0 bit 34 = TCC) ... }
```

`ctx+0x12e0` was set by `FUN_00239d00` to `*(u32*)(sprite+0x20)`, which `FUN_00239b60` set to the **blob pointer**
(`0x014E2A3C`). With `ctx+0x12e8 == 0xffff` the "direct handle" branch runs and reads `*(u32*)(blob+4)` as the TEX0
*value*. For the card blob that word is `0x014E2AC0` - another pointer - so the TEX0 written is pointer bits, not a
valid descriptor; `tbp0=16368 / psm=20` observed in class 8 then come from somewhere else in the stream.

So the TME/class-8 question reduces to two precise suspects, both now locatable:
1. nothing sets `PRIM |= 0x10` on this path, and
2. the `0xffff` sentinel at `ctx+0x12e8` selects a branch that misreads the blob pointer as a TEX0 descriptor.

Next edit (single): mirror the guest's PACKED packet in the SPR translator (REGS 0x512, PRE=1, NREG=3, PRIM from
`ctx+0x12fc`), and instrument `submitGifPacket` with `TOL_TRACE_SPRINJ=1` to print `accepted=…` plus the resulting
batch's `written` rect, so "rejected" and "culled" are distinguishable in one run.
### 197.19 Investigation of the two TME candidates: candidate 1 is DISPROVED - TME IS set, every frame, on the single shared context

Candidate 1 was "nothing sets PRIM |= 0x10 (TME)". Wrong. The setter exists and runs constantly:

```
0015FBB0  lw   $a3, 0x12fc($a0)
0015FBB4  ori  $a3, $a3, 0x10        <- TME ON
0015FBB8  b    0x15fc10
0015FBBC  sw   $a3, 0x12fc($a0)

0015FBC0  ori  $a3, $a3, 0x20        <- FST
0015FBD0  ori  $a3, $a3, 0x80        <- the 0x80 attribute

0015FD20  lw   $t1, 0x12fc($a0)
0015FD24  and  $t1, $t1, $v1         <- TME OFF (mask in $v1)
0015FD2C  sw   $t1, 0x12fc($a0)
```

`0x15FB90` is a jump-table dispatcher (`sll a3,a2,2; addu; lw; jr`) selecting these attribute setters by index.

Measured over 60 s (`TOL_TRACE_PC` on the four sites):

```
0x0015FBB0 (TME on)   41 entries
0x0015FD20 (TME off)  41 entries
0x0015FBC0 (FST)      40 entries
0x0015FBD0 (0x80)     40 entries
```

with the first ones at **32.6 s**, called from `ra=0x00153820` (inside `FUN_00153780`, the per-frame attribute
reset) and `ra=0x001EBC6C` (inside `FUN_001EC150`, the node draw). So the node draw turns texturing on every frame.

Two more facts from the same run, both new and important:

- **every one of those calls passes `a0=0x70000000`** - the GS context is resident in the **SCRATCHPAD**. That is
also why the sprite-engine DMA source is scratchpad, and why an unmasked SADR looked like RDRAM 0x0.
- `FUN_00160040` (bind texture), `FUN_001602C0` (build PRIM) and `FUN_00160390` (flush) are all called with
`a0=0x70000000` as well - one context, so there is no "wrong object" explanation either.

`FUN_00160390` is entered from `ra=0x0023B150` / `0x0023BC6C` (the 0x239xxx sprite module) and receives
`a1=0x70001200` - i.e. the sprite vertex buffer is at scratchpad+0x1200, a *third* bank distinct from the
`0x70000000`/`0x70000900` pair the SPR DMA was streaming.

Consequence: the defect is **not** a missing `PRIM |= 0x10`. TME is set before the sprite library runs, so
something between the setter and the flush must clear bit 4 (the `0x15FD20` clearer runs exactly as often as the
setter), or the packet that produces class 8 is built from a different PRIM source than `ctx+0x12fc`.

Applying a fix at this point would be guesswork of exactly the kind that has already produced three wrong
"obvious" fixes this session, so the next step is one 5-line probe rather than a code change:

    inside the 0x00160390 replacement probe, print *(u32*)(a0 + 0x12fc) (the PRIM word) and
    *(u16*)(a0 + 0x12e8) (the texture sentinel) at flush time, and the same two words inside the
    0x0015FBB0 / 0x0015FD20 setters. That shows the actual bit 4 history for the frames that draw class 8.

Everything else needed is already in place: `TOL_TRACE_PC` can host the setters, `TOL_TRACE_GSDRAWS=1` identifies
class 8 on the GS side, and the context address (0x70000000) is now known, which is what makes a memory-reading
probe possible at all.
### 197.20 Per-frame order traced: the copy is CORRECT, and page 224 receives nothing but clears - the ordering hypothesis is refuted

Added `TOL_TRACE_OSEQ` (1 = clear/copy only, 2 = every primitive) to both `gs_frontend.cpp` (IMAGE tags) and
`sdlgpu_backend.cpp::submit`, so the three event classes share one stderr timeline.

Measured per-frame sequence (first frame after startup, and identical in shape afterwards):

```
0.0000 OTHER      fbp=0   type=6 tme=0 tbp0=0     written=[0,0..640,896]   <- full-screen fill into page 0
0.0562 COPY224TO0 fbp=0   type=6 tme=1 tbp0=7168  written=[0,0..65,448]    <- display copy, 20 strips
0.1007..0.1011 COPY224TO0  (remaining strips, second pass: the copy runs TWICE per frame)
0.1012 OTHER      fbp=0   type=6 tme=0 tbp0=7168  written=[0,0..512,2]     <- copy edge fills
0.1012 OTHER      fbp=0   type=6 tme=0 tbp0=7168  written=[0,446..512,448]
0.1017 CLEAR224   fbp=224 type=4 tme=0 tbp0=7168  written=[0,0..65,33]     <- tiled clear AFTER the copy
0.1022 CLEAR224   (the rest of the tiled sweep)
```

So the order is:

1. fill page 0,
2. **copy page 224 -> page 0** (twice),
3. **then clear page 224**,
4. and during the logo-card window (34-48 s) **nothing else is drawn into page 224 at all**.

That is correct double buffering: the copy presents what the previous frame left in 224, and the clear starts the
next frame. The copy is a faithful 512x448 textured read of 224. Therefore:

- **the ordering is not the bug** - the copy always happens before the clear, so anything drawn into 224 between
  the clear and the next copy *is* presented;
- **page 224 receives only clears during the card window.** Class 7 (`fbp=224 type=4 tme=0 tbp0=16368`) is the
  same tiled clear continuing after the guest rebound TEX0 to the card's texture page (`tbp0` changes, the fill
  does not), not a card draw;
- IMAGE traffic in the same window is 10 tags, none of them into 224.

Conclusion: the card is **never drawn into the page the display reads**. It is not covered by NOW LOADING, and it
is not presented stale - the draw itself does not exist. That points back to the gated blit identified earlier:
`FUN_00206310`'s loop calls `FUN_00206c00(obj, buf)` only when `lVar5 == 1 && *(iVar7 + 0x420) == 0 &&
*(iVar7 + 0x42c) == '\x01'`, and `+0x42c` is set by `FUN_002062a0` to `(FUN_00279d00() == 1)`.

Next probe (one run): print `obj+0x420`, `obj+0x42c`, the return of `FUN_00279d00()`, and `lVar5` (the
`FUN_00335780` read result) at each iteration of `FUN_00206310`'s loop, plus whether `FUN_00206c00` is entered at
all. That names which of the three gates is closed.

Instruments now available and env-gated, none of them changing behaviour by default:
`TOL_TRACE_OSEQ` (1/2), `TOL_TRACE_GSIMAGE`, `TOL_TRACE_GSDRAWS`, `TOL_TRACE_GSPRIM_FILTER`, `TOL_TRACE_GSPRIM_EMPTY`,
`TOL_TRACE_SPRDMA`, `TOL_DUMP_SPR`, `TOL_TRACE_PRIM`, `TOL_SPR_VERTEX`.
### 197.21 EMULATOR ANSWER: the cards are NOT drawn by the 0x206C00 blit - they go through the node/sprite/SPR path

Fresh PCSX2 `-fastboot` boot, breakpoints on `0x00206C00` (the blit), `0x00206310` (the movie loop) and
`0x00204240` (the units):

```
#1 t= 5.57s  0x00204240 mode=1
#2 t=10.74s  0x00204240 mode=2
#3 t=15.76s  0x00204240 mode=3
#4 t=20.94s  0x00206310          <- the movie loop starts only AFTER the three logo cards
0x00206C00 : 0 hits in 30 s
```

Two conclusions, both decisive:

1. **`FUN_00206c00` is never called during the logo cards on hardware either**, so the `+0x420`/`+0x42c` gate I was
   about to chase in 197.20 belongs to the *movie* stage (post-20.9 s), not to the cards. That whole line of
   investigation is retired before it cost a fix.
2. The three cards are produced by `FUN_00204240` itself, whose only drawing machinery is the sprite it builds:
   `FUN_00239a90` (sprite ctor) -> `FUN_001ebea0` (node) -> `FUN_001ec100` (attach) -> the render-list walk ->
   `FUN_001ec150` (node draw) -> `FUN_00239d00` (submit) -> `FUN_0015c3b0` (vertex) -> **DMAC channel 8
   (SPR_FROM)**. So the SPR vertex path really is the card's transport, confirming the 197.16/197.17 direction.

Port side, same probes: the gate/blit addresses had **zero entries**, but that run was one of the slow ones
(`mainIters=237` at 70 s, `g/s=0.004`), and three retries were also slow (239/234/224) - so the port currently is
not reaching the card phase reliably and no conclusion can be drawn from those runs. Run-to-run variance is now
the biggest obstacle to measurement, and it is not caused by the probes (env-gated) or by the SPR work (post-32 s).

SPR translator, second iteration: rewritten to mirror `FUN_00160390` exactly - FLG=0 (PACKED), NREG=3, REGS=0x512
(ST, RGBAQ, XYZ2 in that order), EOP set, and the **live PRIM word** read from `scratchpad+0x12FC` shifted to bit
46. Result: still no new draw class (9 classes, same as baseline), so the injected packet is still not becoming a
batch. It remains env-gated (`TOL_SPR_VERTEX=1`) and therefore inert by default.

Where this leaves the milestone:
- the card is drawn through SPR_FROM, and SPR_FROM is unrouted in the runtime (fixed to route, source corrected
  to `0x70000000 | (SADR & 0x0FFFFFFF)`);
- the vertex payload format is known (48-byte vertices: uv, rgba, xyz);
- the remaining unknown is purely why the injected packet does not reach the rasteriser, which is a
  `submitGifPacket`/arbiter question, not a guest-logic question.
### 197.22 Injected packets DO reach the GS frontend - the submission route is fine; the synthetic encoding is what fails

Instrumented both ends with `TOL_TRACE_SPRINJ=1`:

- `PS2Memory::submitGifPacket` (line ~2207): logs every Path3 packet of the synthetic size;
- `GifArbiter::drain` (line ~59): logs the same packet as it is handed to `m_processFn`.

Result over 60 s:

```
[   51.667] [sprinj] queued   path=3 size=208
[   51.683] [sprinj] queued   path=3 size=208
[   51.683] [sprinj] draining path=3 size=208
[   51.683] [sprinj] draining path=3 size=208
count: 1502
```

Every queued packet is drained, i.e. **`submitGifPacket(Path3)` -> arbiter -> `m_processFn` works end to end**.
So the earlier "no new draw class" is not a routing problem: the GS frontend receives the packet and either rejects
it in `validatePackedGifPacket`, or parses it into registers that rasterise nothing.

The most likely cause, from the guest's own builder (`FUN_00160390`):

```c
*(u64*)(ctx+0x12f4) = nloop | 0x8000 | ((u64)ctx->+0x12fc << 46) | 0x3000000000000000;
```

`ctx+0x12fc` is the live PRIM word (measured `0x000004B9`, TME=1, type=1). Shifting the **whole word** by 46 sets
bit 46 (the PRE bit) from PRIM bit 0 as a side effect, and puts PRIM bits 1..10 into the PRIM field. If the GS
frontend honours PRE, each loop iteration must then begin with a PRIM qword; the synthetic packet supplies only the
three data qwords (ST/RGBAQ/XYZ2), so the parser desynchronises by one qword per vertex. The guest's own stream
works because its buffer really does carry whatever the two callbacks at `ctx+0x1248` / `ctx+0x124c` wrote.

Conclusion and the correct next step: **stop synthesising the packet**. `FUN_0015c3b0` already invokes
`(*(code**)(ctx+0x1248))()` and `(*(code**)(ctx+0x124c))(base, base+0x900, ctx+0x1230)` immediately before the SPR
DMA - those callbacks are the guest's own packet builder. The bank we dumped at `0x70000000` contains raw vertices,
so either the packet is written to the other bank (`0x70000900`) or the dump was taken before the callback ran.
Dumping **both** banks at the exact DMA instant, with the callback return values, settles which buffer the DMA
should be reading and whether the transfer must be offset past the vertex area.

Status: SPR channel routed (real fix, previously dropped entirely), source address corrected
(`0x70000000 | SADR&0x0FFFFFFF`), synthetic translator present but inert (`TOL_SPR_VERTEX=1`, default off), packet
delivery proven by instrumentation. Nothing regressed: default runs are unchanged.
### 197.23 The SPR packet located in RDRAM: a DMA-tag chain with the GIF packet nested at +0x40

RDRAM dump at `0x01D00300` (TOL_DUMP_RAM) at the moment the SPR channel runs:

```
+000 014E2AC050000000
+010 014E2A5050000000
+020 01D0033020000000      <- DMA tag, ADDR 0x01D00330
+030 000000001000000D      <- DMA tag, QWC=13, FLG=1 (REFE)
+040 312E400000008004      <- GIF tag: NLOOP=4, EOP=1, NREG=3
+048 0000000000000512      <- REGS = 0x512 (ST, RGBAQ, XYZ2)
```

So the SPR block begins with **DMA tags** and the real GIF packet is nested at `+0x40` (absolute `0x01D00340`).
Submitting from offset 0 - what both the routing and the synthetic translator did - parses a DMA tag as a GIF tag
and yields nothing. The nested tag also confirms the guest's own encoding (`REGS=0x512`), matching `FUN_00160390`.

Also from the same scratchpad dump, the packet *pointers* live in the GS context at `ctx+0x12F0/0x12F4`
(`0x01D00330 / 0x01D00340`) with `ctx+0x12F8 = 0x00000007` and `ctx+0x12FC = 0x000004B9` (PRIM, TME=1) and
`ctx+0x1300/0x1304 = 0x00000000 / 0x0FFF003F` (texture/clamp words). So the context does hold a fully enabled
PRIM and a texture descriptor while the sprite engine runs.

Fix implemented in `ps2_memory.cpp`: for the SPR channel the block is scanned (first 0x200 bytes) for the first
plausible GIF tag (`flg<=2`, `nloop!=0`, `1<=nreg<=16`, EOP set) and the transfer is submitted from that offset,
falling back to a plain enqueue when no tag is found. Build clean.

Verified: still 11 draw classes - no card class appears - and no new errors beyond the known `0x301EB8` jalr-to-null.
So even with the nested packet submitted, the GS produces no batch for it. The remaining possibilities are that the
nested packet's four vertices are culled (a single quad, possibly with zero extent after the GS transform), or that
the frontend rejects the block after the DMA header.

Everything above is measured; nothing here is a working card yet. The default build is unchanged in behaviour
(the scan only fires on the SPR channel, and falls back to the previous path when it finds no tag).
### 197.24 ACCEPTED, NOT REJECTED: the SPR packets reach GS::processGIFPacket intact

Instrumented `GS::processGIFPacket` (the arbiter callback is `m_gs.processGIFPacket`, wired in
`ps2_runtime.cpp:702` - NOT `processNativePackedGIFPacket`, which is why the first attempt logged nothing) with
`TOL_TRACE_SPRINJ=1`, keying on the distinctive `tagHi == 0x512` that this game's sprite packets use.

```
[   31.406] [inj] size=1168 tagLo=312E400000008018 tagHi=0x512 nloop=24 eop=1 flg=0 nreg=3
[   31.421] [inj] size= 400 tagLo=312E400000008008 tagHi=0x512 nloop= 8 eop=1 flg=0 nreg=3
[   32.979] [inj] size= 208 tagLo=312E400000008004 tagHi=0x512 nloop= 4 eop=1 flg=0 nreg=3
count: 420
```

So:

1. **The packets are ACCEPTED.** 420 well-formed PACKED packets (`flg=0`, `nreg=3`, `eop=1`, `tagHi=0x512`)
   reach the GS frontend from the SPR channel. Rejection is ruled out.
2. **The SPR routing fix works.** The 24-vertex and 8-vertex packets are the *guest's own* sprite-engine output,
   arriving at the GS for the first time - before this session that channel was dropped entirely and none of this
   traffic existed.
3. **No draw class results** (classes still 11, no card class), so the primitives built from those vertices are
   culled: `primitiveBounds()` empty or the transformed quad has zero extent. That is the culling half of the
   question, and it is now the only half left.

Why culling is the likely mechanism: the vertices we dumped are a 32x32 quad at z=64 with uv 0..0.5
(`(0,0) (0,-32) (32,0) (32,-32)`), and the GS scissor is 512x448 - so either the XYOFFSET/offset transform places
the quad outside, or the W/Q handling collapses it. The next measurement is one line: log the batch's
`written` rect (or `primitiveBounds`) for these packets in `sdlgpu_backend::submit`, which prints exactly the
rectangle the quad occupies after transformation.

Instrumentation state at the end of this session (all env-gated, default-off):
`TOL_TRACE_SPRINJ` (arbiter + frontend packet accept log), `TOL_TRACE_SPRDMA`, `TOL_DUMP_SPR`,
`TOL_TRACE_GSDRAWS`, `TOL_TRACE_GSPRIM_FILTER`, `TOL_TRACE_GSPRIM_EMPTY`, `TOL_TRACE_OSEQ`, `TOL_TRACE_PRIM`,
`TOL_SPR_VERTEX`. The default build behaves exactly as before except that the SPR channel is now routed and its
packets reach the GS.
### 197.25 Written-rect / bounds verdict: the vertices are ON SCREEN with real colours - but no primitive batch is emitted

`TOL_TRACE_SPRVERT=1` logs every XYZ2 vertex while the sprite packet (`tagHi == 0x512`) is being parsed. Raw GS
coordinates are offset by the GS XYOFFSET (`ofx/16 = 1792`, `ofy/16 = 1824`), so subtracting those gives the real
screen position:

```
raw x=2224.50 y=2054.00 z=5376   -> screen (432.5, 230.0)
raw x=1792.00 y=1856.00 z=0      -> screen (  0.0,  32.0)
raw x=1920.00 y=1825.00          -> screen (128.0,   1.0)
...
screen positions seen: (0,0)(0,32)(0,64)...(0,320)(128,0)(128,448)(192,0)(192,448)(256,0) (432,230)
colours seen:  103,89,68,128   103,89,68,64   43,23,17,128   43,23,17,64   128,128,128,128
```

So:

1. **Not rejected** - 420 PACKED packets reach `GS::processGIFPacket` (197.24).
2. **Not off-screen** - every transformed vertex lands inside 0..512 x 0..448.
3. **Not colourless** - the vertices carry real game colours (brown 103,89,68 / 43,23,17) with alpha 64/128,
   plus UVs (u=56 v=8, u=392 v=344, u=248 v=280), i.e. genuine textured sprite geometry.
4. **No batch is emitted for the coloured sprites.** `TOL_TRACE_GSPRIM_FILTER` (which logs every non-clear,
   non-copy primitive reaching `sdlgpu_backend::submit`) still shows only the five early UI panels; the brown
   sprites never appear there. The grey (128,128,128,128) vertices at (0,0)/(128,0)/(0,32) are the copy edge
   fills and DO reach the backend, which proves the filter works.

Therefore the loss is **inside the frontend's primitive assembly**, between `writeRegisterPacked(XYZ2)` and
`submit`: the vertices are accepted and valid, but they never become a `GSPrimitiveBatch`. The remaining suspects
are the PRIM word written from the tag (`(tag.lo >> 47) & 0x7FF` under `PRE`) not being a drawable primitive type
for this packet, or a flush/wait condition that only fires on `EOP` in a way this packet does not satisfy.

This closes the "rejected vs culled" question the previous session opened: it is neither. The geometry is delivered
correctly and on-screen; the primitive is simply never assembled. Next probe is one line inside `vertexKick`:
print the current PRIM word and the vertex-queue count at each kick for the sprite packet.
### 197.26 Docs checked + kick probe ready (run did not reach the sprite phase)

`docs/` holds only disc/container/codec notes plus the recompiler architecture overview. Its hardware table
(`ps2-static-recompilation-overview.md` section 4b) names `GS` / `GifArbiter` as the subsystem and stops there -
nothing on primitive assembly, XYOFFSET, or the vertex queue. Useful background for the disc and toolchain, no
guidance for this defect.

`TOL_TRACE_SPRVERT` extended to print, per vertex kick: `primType`, the raw `m_primRegister` word, `m_vtxCount`
and the kicking register id. Purpose: show whether a drawable primitive type is set and whether the queue ever
fills enough to flush.

Build clean, but the verification run did not reach the sprite phase (`[inj] = 0`, `[sprvert] = 0`, and the
run's `guestEntries` shows it stalled in the slow regime), so the probe has no data yet. It is ready and
env-gated.

Already-known and relevant: the effective PRIM written from the tag is `(tag.lo >> 47) & 0x7FF` = `0x25C` for
`prim = 0x4B9`, i.e. **type 4 = triangle strip, TME = 1**, so the primitive type is valid; with NLOOP = 24 that
is 22 triangles. So "invalid primitive type" is unlikely, which leaves the queue/flush condition as the leading
suspect.
### 197.27 Queue/flush logic read + the packet decode is provably valid; texture drawing already exists

**Queue / flush (`GS::vertexKick`, gs_frontend.cpp:1793-1874).** There is no accumulating threshold - it submits as
soon as one primitive is complete:

```cpp
++m_vtxCount;
switch (m_prim.type) { POINT:1; LINE:2; LINESTRIP:2; TRIANGLE:3; TRISTRIP:3; TRIFAN:3; SPRITE:2; default:return; }
if (m_vtxCount < needed) return;
if (drawing && m_backend) { auto batch = buildDrawBatch(needed); m_backend->Submit(batch); }
switch (m_prim.type) { LINESTRIP: keep 1; TRISTRIP: keep 2; TRIFAN: keep 2; else m_vtxCount = 0; }
```

and `drawing` is `(regAddr == GS_REG_XYZ2)`, which our packets satisfy. So a 24-vertex tristrip must produce 22
`Submit` calls. Nothing here can swallow a 24-vertex strip.

**The tag decode is valid.** Decoding the three delivered packets:

```
tag=312E400000008018  nloop=24 eop=1 pre=1 prim=0x25C type=4(TRISTRIP) tme=1 flg=0(PACKED) nreg=3
tag=312E400000008008  nloop= 8 eop=1 pre=1 prim=0x25C type=4(TRISTRIP) tme=1 flg=0(PACKED) nreg=3
tag=312E400000008004  nloop= 4 eop=1 pre=1 prim=0x25C type=4(TRISTRIP) tme=1 flg=0(PACKED) nreg=3
```

**`tryProcessNativeImageUploadPacket` does not swallow them**: it requires `NLOOP==4 && NREG==1 && FLG==PACKED &&
(tagHi & 0xF)==0xE` plus a BITBLTBUF/TRXPOS/TRXREG/TRXDIR reglist; ours is NREG=3 with `tagHi=0x512`, so it returns
false at the first check.

**Textured drawing already works - we do not need to build it.** Evidence from the same runs: class 5 is
`fbp=224 type=6 tme=1 tex tbp0=14976 psm=20 cbp=14960` (PSMT4 + CLUT, i.e. a real palette texture) and reaches
`submit`; the display copies are `tme=1` and are the ones that have been producing the visible frame; and
`sdlgpu_backend`/`gs_cpu_backend` sample textures and CLUTs (`UploadImage`, `vram.read(psm, tbp0, tbw, u, v)`).
So the pipeline can draw textured primitives; the missing card is a primitive-delivery problem, not a missing
feature.

So the only remaining gap between "valid 24-vertex tristrip with TME=1 and on-screen vertices" and "a draw class
appears" is inside `buildDrawBatch(3)` or the `Submit` gate. `buildDrawBatch` is the next thing to read: it is the
only code between `vertexKick`'s submit and `sdlgpu_backend::submit`, and it is where a batch could come out with
zero vertices.
### 197.28 The last gap closed: batches DO arrive (56 tristrips) - but with the WRONG texture page bound

`TOL_TRACE_BATCH=1` (already in the runtime) settles it:

```
[batch] verts=3 prim=4 fbp=224 texTbp=7168 scissor=511x447    x56 sampled
```

So:

- `buildDrawBatch` is a plain copy (`vertexCount = min(n,3)`, copies `m_vtxQueue`, fills state) - it cannot drop
  anything, and it does not;
- `vertexKick` submits per completed primitive, as read in 197.27;
- **56 three-vertex tristrip batches reach the backend**, so delivery is proven end to end;
- but **every sampled one carries `texTbp=7168`** - page 224, the framebuffer itself, i.e. the *clear's* texture
  binding - not the card's page `16368`.

That is the actual last defect: the sprite geometry is submitted with the **wrong TEX0 bound**. The clear binds
`tbp0=7168` (it draws into page 224), and the sprite draw is supposed to rebind `tbp0=16368` first. The rebind comes
from a *separate* GIF packet built by `FUN_00160040` -> `FUN_001593a0(ctx+0x12e0, ...)` (the TEX0 writer seen in
197.18), not from the vertex packet, so the ordering that matters is:

    clear (binds 7168) -> TEX0 packet (binds 16368) -> vertex packet (draws)

With `texTbp` still 7168 at vertex time, the TEX0 packet is either not being delivered or is being delivered after
the vertices. Since the SPR channel now routes and 371 sprite packets arrive, the most likely case is that
`FUN_001593a0`'s TEX0 write goes through the *same* SPR buffer and our scan-based extraction only forwards the
packet it finds first (the vertex one), dropping the TEX0 packet that precedes it.

Next step (one change): stop scanning for a single GIF tag and forward the **whole** SPR block from offset 0 as a
packet stream (the DMA chain), so the TEX0 packet(s) and the vertex packet(s) both reach the GS in order. That is
what the hardware does with the same buffer, and it directly targets `texTbp` being wrong at draw time.

Also worth noting for the texture question: the sampled batches prove the backend receives `verts=3, prim=4`
(tristrip) batches - so textured tristrips are supported; only the bound page is wrong.
### 197.29 Whole-block forward implemented: more state reaches the GS, but the card still is not drawn

Change applied (ps2_memory.cpp, SPR branch): the scan-for-one-tag approach was replaced by forwarding the **entire**
SPR block from offset 0 as the packet stream (`submitGifPacket(Path3, rdram + (madr & MASK), qwc*16, false)`), which
is what hardware does with the same buffer.

Measured effect:

```
[batch] verts=3 prim=4 fbp=224 texTbp=7168   ...  (most)
[batch] verts=3 prim=4 fbp=224 texTbp=14976  at 33.822   <- a page that never appeared before
draw classes: 11 (unchanged); cl=7 is fbp=224 type=4 tme=0 tex tbp0=16368
```

So the whole-block forward **does** carry more state than the single-tag scan (a tristrip batch finally bound
`tbp0=14976`), which validates the approach. But the card is still not drawn, and the sharpest remaining fact is:

**there are tristrips into page 224 bound to the card's page `tbp0=16368` (class 7) - and `tme` is 0 on them.**

That means the geometry packets and the TEX0 binding are now both present, and the remaining single difference is
the **PRIM.TME bit not being set on that batch**, even though the packet's tag says `prim=0x25C` (TME=1) and 197.22
measured `ctx+0x12fc = 0x4B9` (TME=1) at flush time.

So the next probe is one line, and it is the last one in this chain: log `m_prim.tme` (and the raw `m_primRegister`)
inside `vertexKick` for these packets. If it is 0 there while the tag says 1, the PRIM write from the tag is being
overwritten or is not applied on this path; if it is 1, the loss is in `buildDrawBatch`'s state copy or in the
backend's reading of `state.prim.tme`.

Nothing regressed: 11 classes as before, no new errors, and the default build differs only in that the SPR channel
now forwards its full block.
### 197.30 LAST-GAP PROBE ANSWERED: TME is NOT lost - 1704 textured tristrip kicks, textured batches flow; the window is still black

`TOL_TRACE_SPRVERT` now logs only tristrip kicks **with TME set**:

```
skick(tme=1) count: 1704
[30.624] [skick] vtxCount=1 primType=4 tme=1 iip=1 abe=1 ctxt=1 fix=0 drawing=1
[30.639] [skick] vtxCount=2 ... tme=1 ...
[30.639] [skick] vtxCount=3 ... tme=1 ... drawing=1
```

and the batch trace now shows textured draws reaching the backend:

```
[batch] verts=3 prim=4 fbp=224 texTbp=14976 scissor=511x447   (x several, from 33.99 s)
[batch] verts=3 prim=4 fbp=280 texTbp=8960  scissor=639x447   (from 52.59 s)
```

So the whole-block SPR forward (197.29) did its job at the state level: **PRIM.TME is 1 at vertexKick, textured tristrip
batches carry real texture pages, and they reach the sdlgpu backend**. It also produced the first `fbp=280` draws
with `texTbp=8960`, i.e. the scene page finally receives textured geometry.

But the window is **still black**. The screenshots taken during that build turned out to contain an unrelated
PPSSPP window in front; the TOLRecomp client area itself is black. So the remaining gap is no longer anywhere in
the EE/GS submission chain - it is between these (now correct) draws and what the presenter scans out.

That re-frames the problem: with
- textured tristrips into page 224 (texTbp=14976), and
- textured tristrips into page 280 (texTbp=8960), and
- the copy 224->0 / 280->0 also confirmed textured and correctly ordered (197.20),

the only way the window can still be black is that the *content of those texture pages* is black, or the copy runs
before the draws land in the same frame. The next measurement is therefore the VRAM side again, but this time
through the backend's live VRAM (not `PS2Memory::getGSVRAM()`, which 197.14 showed is a stale buffer): sample a
texel from `tbp0=14976` and from `tbp0=8960` at draw time and print it. If those texels are zero, the uploads are
landing in the wrong place and the draw side is finally exonerated.

Instrumentation summary at end of session: `TOL_TRACE_SPRVERT` (kicks incl. TME), `TOL_TRACE_BATCH`,
`TOL_TRACE_GSDRAWS`, `TOL_TRACE_GSPRIM_FILTER`, `TOL_TRACE_GSPRIM_EMPTY`, `TOL_TRACE_OSEQ`, `TOL_TRACE_SPRINJ`,
`TOL_TRACE_SPRDMA`, `TOL_DUMP_SPR`, `TOL_TRACE_PRIM`, `TOL_SPR_VERTEX` - all env-gated, default off.

## Session 199 (2026-11-23) - VRAM INVESTIGATION (session-197 next step): the two framebuffers never exist in VRAM, the card art IS uploaded (262 KB into 0x3A8000), SPR_FROM is an MFIFO copy and NOT a GS submission, and the sprite packets arrive with empty/zeroed vertices

Continues 197.30's single open question ("sample a texel from `tbp0=14976` and `tbp0=8960` at draw
time"). Everything below is measured on a clean build of `data/output_fixed`
(`build/tol2`, clang-cl, `-j 12`, `RESULT: exit 0`), 60-85 s runs with `--gs=sdlgpu`.

### 199.1 Two structural facts that retract earlier framebuffer reasoning

1. **The sdlgpu VRAM is only an upload/texture store.** The SDL-GPU backend rasterises into GPU
   surfaces; `PS2Memory::getGSVRAM()` (what `TOL_DUMP_VRAM` writes) receives only IMAGE uploads.
   Measured over a whole run: **pages 0 (0x000000), 224 (0x1C0000) and 280 (0x230000) are byte-zero
   in every dump** (`logs/run159/vt_d*.bin`, 9 dumps from draw 4,000 to 30,000). So "page 224 is
   black in VRAM" is not a defect, and no VRAM read can ever explain the displayed picture. Judge the
   picture with `TOL_TRACE_GSDRAWS` / `TOL_TRACE_GSPRIM_*` / a window capture, never with VRAM.
2. **`TOL_DUMP_VRAM` needed draw-index triggers.** Frame numbers move by 3x run to run, so it now has
   `TOL_DUMP_VRAM_DRAWS=<n[,n...]>` + `TOL_DUMP_VRAM_PATH=<prefix>` (writes `<prefix>_d<n>.bin` at exact
   absolute draw indices) and `TOL_DUMP_VRAM_ONTEX0=<tbp0>` (first draw binding that texture page).

### 199.2 VRAM timeline of the logo-card window (measured, not inferred)

```
draw  4000  3 non-empty pages : 0x3A6000/0x3A8000/0x3AA000   (the NOW LOADING text at tbp0=14976 PSMT4)
draw  4400 34 non-empty pages : 0x3A6000 .. 0x3E6000 + 0x3FE000
draw 30000  identical to 4400 (34 pages, nonexistent after that)
```

The 34 pages are **one 256x256 CT32 image (0x40000 bytes) at 0x3A8000** plus the CLUT strip at
0x3FF400. Nothing else is ever in VRAM. In particular:

- the **card texture page `tbp0=16368` (0x3FF000, PSMT4)** holds **126 non-zero bytes** of its
  4 KB, ever;
- pages 224/280 are zero (see 199.1).

Upload destination census (`TOL_TRACE_GSIMGDBP=1`, counts every IMAGE tag by `BITBLTBUF.DBP`):

| destination | format | tags | bytes |
|---|---|---|---|
| `0x3A8000` (14976) | CT32 dbw=2 | 256 | **65,358,080** (one 262,144-byte image per tag, `TRXREG 256x256`) |
| `0x3A7000` (14960) | CT32 dbw=1 | 256 | 255,424 (CLUT, `16x16`) |
| `0x3FF400` (16372) | CT32 dbw=1 | ~14,000 | ~2.7 MB of `8x2` CLUT fragments |
| `0x3FF000` (16368) | PSMT4 dbw=1 | **3** | **240** (`TRXREG 24x16`) |

So the guest uploads a **262,144-byte image into 0x3A8000** over and over (a 512x512 PSMT8 image and a
256x256 CT32 image are both exactly 0x40000 bytes - the same bytes, two interpretations), and it
uploads essentially nothing into the card's PSMT4 page. The upload path is healthy; the question moves
to *what the guest binds and draws*.

### 199.3 The card texture binding exists and is correct (new probe `TOL_TRACE_TEX0W=1`)

`writeRegisterUnlocked` now logs every TEX0 write, decoded. During the card window the stream is a
clean alternation:

```
ctx=0  tbp0=7168  tbw=8  psm=0  (CT32 1024x512)  <- the display page, for the 224/280 -> 0 copy
ctx=1  tbp0=14976 tbw=8  psm=19 (PSMT8 512x512)  <- the card art (CLUT 14960)
ctx=0  tbp0=16368 tbw=1  psm=20 (PSMT4 64x64)    <- the small sprite texture (CLUT 16372)
```

So the card texture **is bound**, in **context 1**, which is exactly what the sprite packets use
(`PRIM = 0x25C`: CTXT=1, TME=1, type 4). The GS frontend honours PRIM.CTXT
(`batch.state.context = m_ctx[m_prim.ctxt]`), so the binding is not the defect.

### 199.4 SPR_FROM is MFIFO, not a GS path - and the earlier "route it to path 3" fix is wrong

`09-ps2tek.md` (DMAC MFIFO): *"Scratchpad memory ... The Memory FIFO (MFIFO) ... automatically
transferring data sent via SPR_FROM to a user-defined buffer in main memory. D_RBSR and D_RBOR define
the size and location of this buffer. SPR_FROM's MADR is automatically set to the next **read**
position in the buffer, wrapping around ... Either the VIF1 DMA channel or GIF DMA channel can be the
drain channel."*

Therefore:
- `0x1000D000` = **channel 7 SPR_FROM** = scratchpad (SADR) -> main-memory MFIFO ring. It is *not* a
  GS submission; routing it to `submitGifPacket(Path3, rdram+madr, ...)` (197.16-197.30) both
  duplicated packets and never wrote the ring.
- **MADR is the ring's read pointer**, not the destination of the copy. That is why copying the
  scratchpad bytes to MADR corrupted the payload: with the copy enabled the sprite packets reached
  the GS with every vertex at "(0.0, 0.0)" (`TOL_TRACE_GSPRIM_EMPTY`), because the copy overwrote the
  payload the drain channel was about to read.
- The runtime does not model MFIFO at all (no D_RBOR/D_RBSR write pointer, no drain stall at
  `TADR == MADR`). Live measurement at every SPR copy: `gifChcr=0x00000000`, `gifTadr=0x00000000` -
  the drain channel is *not* a running GIF chain DMA, so the port has no drain to feed.
- Scratchpad source offsets measured: `SADR=0x0000` (72 qwords) and `SADR=0x0900` (24 qwords), i.e.
  the two sprite banks. `FUN_00160390` was measured in 197.19 receiving `a1=0x70001200` - a **third**
  bank that the SPR DMA never streams. The card's vertex payload therefore does not travel through
  these two copies at all.

Current code state (measured, no regression): the SPR branch in `ps2_memory.cpp` now **completes the
channel** (QWC drained, STR cleared, `D_STAT` bit 7, `queueCompletedDmacCause(7)`) and performs **no
memory write and no GS submission**. The ring is left as the guest wrote it. `TOL_TRACE_DMA8=1` still
prints `sadr/madr/qwc` plus the GIF channel state.

### 199.5 The immediate cause the card is invisible on this build (measured, new)

`TOL_TRACE_GSPRIM_EMPTY=1` (now filtered to textured draws) shows the sprite tristrips being **culled
as empty**:

```
[gsempty] verts=3 fbp=224 | type=4 tme=1 abe=1 | tex tbp0=14976 psm=20 | rgba0=0,0,0,0
          v0=(0.00,0.00) v1=(0.00,0.00) uv0=10248,7160 | scissor=[0,0..511,447]
```

and `TOL_TRACE_SPRINJ` payload dump (`[injpl]`) shows the packets arriving with **nine zero qwords**
where the three vertices should be:

```
[inj]   size=208 tagLo=312E400000008004 tagHi=0x512 nloop=4 egr=3     <- well-formed header
[injpl] 0000000000000000 ... (x9 all zero)                            <- empty payload
```

With the (wrong) MADR copy enabled the same payload was non-zero but held the raw scratchpad bank
(`0/0x10000000/0x3F0080813F008081/...`), i.e. object-space floats, which the GS decodes as
`(0,0)` too. So: **the card's vertex payload is never produced in the port.** 197.25's "on-screen
vertices with real colours" came from a ring buffer that still held a previous frame's data when the
SPR block was being mis-submitted as a GIF packet - it was stale, not proof of a working path.

`TOL_TRACE_SPRVERT=1` still counts **1704 `tme=1` tristrip kicks** with `ctxt=1`, so the packets are
formed; they are simply filled with zeros, which is why no textured draw class ever appears (the class
list stays at 11 for 400,000 draws; `c5..c8` freeze after the early phase and `c9..c11`, the page-280
copy/clear, grow forever).

### 199.6 Correlation with the reference (attempted, blocked)

PCSX2 was relaunched fresh with `-fastboot` and the DebugServer answered (`tools/ps2dbg.py status`
ok). A breakpoint probe was added (`tools/ref_gsspr.py`: break at `0x0015C4E8`, dump the scratchpad GS
context `0x7000125C..0x70001304` and follow the texture handle) but the run produced **0 hits in 45 s**
while the emulator sat at `pc=0x00081FC0` (a kernel wait loop) - i.e. that fresh boot did not reach the
unit phase. Reference-side capture needs a boot that actually reaches ~11 s before the probe is armed;
this is a tooling/starting-state issue, not a finding about the game.

### 199.7 New instruments added this session (all env-gated, default inert)

| env | what |
|---|---|
| `TOL_TRACE_GSDRAWS` (extended) | class key now `(fbp, tme, tbp0, type, psm, cbp)` + per-class totals every 20k draws |
| `TOL_TRACE_TEXBIND` | every distinct TEX0 binding seen on a draw, with destination page and tme/type |
| `TOL_TRACE_TEXUPD` | texture-cache updates: logs when a page gains *more visible texels* than ever before (finds artwork arriving into a reused page) |
| `TOL_DUMP_VRAM_DRAWS`, `TOL_DUMP_VRAM_ONTEX0` | draw-index / TEX0-bind triggered live VRAM dumps |
| `TOL_TRACE_TEX0W` | every TEX0 register write, fully decoded |
| `TOL_TRACE_GSIMGDBP` | IMAGE upload census by destination block (bytes, tags, TRXREG) |
| `TOL_TRACE_SPRINJ` (extended) | `[injpl]` - the first 9 payload qwords of each sprite packet |
| `TOL_TRACE_GSPRIM_EMPTY` (changed) | reports **textured** empty-bound drops first (the untextured tiled clear legitimately drops out-of-scissor) |
| `TOL_GFX_DUMP_TEXTURES` (existing, now used) | writes every built texture as PPM **with non-black/alpha counts** - this is the tool that answered "is the texture page populated" |
| `tools/ppm2png.py` | convert those PPMs to PNG |
| `tools/ref_gsspr.py` | reference-side scratchpad GS-context probe (see 199.6) |

`TOL_TRACE_DMA8` (new) prints, per SPR_FROM start: `chcr mode sadr madr qwc`, the first two qwords of
the RDRAM destination after the transfer, and the GIF channel's `CHCR/MADR/TADR`.

### 199.8 What this proves and what is left

Proven:
1. the display path (224/280 -> 0 copy, textured, correctly ordered) is *not* the gate and VRAM cannot
   be used to judge it;
2. the card art **is** uploaded (262,144 bytes per upload into 0x3A8000) and **is** bound (TEX0 context
   1, PSMT8 512x512, CLUT 14960) - both sides of the "missing artwork" question are healthy;
3. SPR_FROM is MFIFO scratchpad -> memory with MADR = ring *read* pointer, so every earlier
   "submit the SPR block to path 3" and "copy scratchpad to MADR" fix is wrong in kind, and the port
   has no MFIFO model at all;
4. the sprite packets arrive at the GS with **zeroed vertex payloads**, are culled, and that (not the
   texture, not the copy, not the presenter) is why the logo cards are invisible.

Next concrete step (single, ordered):
1. **Find who fills the sprite payload.** The packet header is correct but 12 payload qwords are zero
   and `SADR` only ever streams banks 0x0/0x900 while the sprite buffer is bank 0x1200. Break in
   PCSX2 at `0x0015C4E8` (probe ready) once a reference boot actually reaches the unit phase, and dump
   scratchpad `0x70001200..0x70001400` plus RDRAM at the packet's MADR **in the same stop**. That names
   the missing transport (a third DMA, a VIF1 kick, or a guest memcpy).
2. Then implement the transport faithfully - if it is MFIFO, add `D_RBOR`/`D_RBSR`, the ring write
   pointer, and the drain-channel stall (`TADR == MADR`), instead of copying to MADR.
3. Only after the payload is real: re-measure the three cards against `logs/refseq2/t014/t018/t0225`.
4. Gate B (opening movie + title background) is unchanged and still needs the IPU (197.7): a complete
   implementation exists at `D:\Personal\C++\grandia-ps2recomp\runtime\src\hw\ipu.cpp`.

### 199.9 REFERENCE CAPTURE OF THE SPRITE PAYLOAD PRODUCER (this task)

Breakpoint `0x0015C390` in `FUN_0015c2a0` (the flush epilogue, where `s1` = MADR, `s0` = QWC and
`s3` = scratchpad bank are still live) with `tools/ref_spr_payload.py`. Note the address used in 199.5
(`0x0015C4E8`) is in `FUN_0015c3b0` and produced **0 hits across the whole unit sequence** (a trace on
`0x00204240` in the same window hit modes 2 and 3) - the live flush during the logo cards is
`FUN_0015c2a0` (0x15C2A0..0x15C3B0), whose CHCR write is at **0x0015C38C**. (`FUN_0015c3b0` may still be
used by a later, non-boot path; it is not the logo-card path.)

Producer chain, all confirmed (corpus + live probes in both runtimes):

```
FUN_0023A780  per-sprite vertex emit (8 calls/run).  VU0 macro mode: pextlb / qmtc2 /
              vitof0.xyzw / vmul.xyzw / sqc2.  Builds one 48-byte vertex at ctx+0x1200
              (u,v floats at +0; RGBAQ at +0x10; x,y,z floats via swc1 at 0x1220/0x1224/0x1228)
FUN_0015C3B0  copies the three 128-bit vectors from a1 (= ctx+0x1200) verbatim into the vertex bank
              bank = ctx + ctx->+0x1250*0x900 + ctx->+0x1254*0x30 ; flushes at count 0x30
FUN_0015C2A0  flush: s1 = MADR = ctx->+0x125C, s0 = QWC = count*3, s3 = bank
              call *(ctx+0x1248) = 0x15CB70   (packet begin)
              call *(ctx+0x124C) = 0x15CCB0   (packet body: a0=bank a1=bank+count*48 a2=ctx+0x1230)
              ctx->+0x125C += QWC*16 ; wait CHCR.100
              SPR_FROM: SADR = bank, MADR = s1, QWC = s0, CHCR = 0x100
FUN_00156660  (40 calls) receives a2 = MADR and a3 = PRIM: writes the GIF tag/register block
```

**SPR_FROM does copy the bank into the ring**: in the reference the ring at MADR is byte-identical to
the scratchpad bank.

Event identity (reference rec 1 vs the port's first SPR flush): bank 0, MADR 0x01D00350, QWC 72,
PRIM 0x4B9, callbacks 0x15CB70/0x15CCB0 - identical. Yet the 48-byte vertices differ (+590 of 2304
bytes), and the difference is a **representation** difference, not a frame difference:

| field | reference | port |
|---|---|---|
| +0x08 | 0x3F800000 (1.0f) | 0x10000000 |
| +0x10..0x18 | 0x00000080 x3 (byte colour) | 0x3F008081 x3 (0.502f = 128/255) |
| +0x1C | 0x00000020 (32) | 0x3F800000 (1.0f) |
| +0x20 X | 0x00007538 (1877.5, 12.4 fixed) | 0x00000000 |
| +0x24 Y | 0x00007A68 (1956.5, 12.4 fixed) | 0x00000000 / 0xC2000000 (-32f) |
| +0x28 Z | 0x000003F8 (1016, 12.4 fixed) | 0x42800000 (64.0f) |

Reference vertices are screen-space (X 1877.5..2237.5 -> screen 85..445 with ofx 1792; Y 1956.5..
2103.5 -> 132..279); port vertices are model-space floats (0/32/-32, z=64). Full table and decoded
vertices: `logs/ref199/compare.txt`; raw data `logs/ref199/hits.jsonl`, `h001_spr.bin`,
`h001_ring.bin`, `port_bank72.bin`.

Both `FUN_0023A780` (8 calls) and `FUN_00156660` (40 calls) do run in the port, so nothing is skipped
at the call level: the divergence is in the converted values (12.4 fixed / byte colour vs
normalised float) produced between the vertex emit and the flush.

Next session starts here: break at `0x00156660` in PCSX2 with `a1`/`a2` and dump the 48 bytes at
`ctx+0x1200` plus the ring at `a2`, and do the same in the port with `TOL_TRACE_PC=0x156660`. Whichever
side lacks the float->12.4 / colour-byte conversion names the function to fix. The VU0 macro path in
`FUN_0023A780` (`pextlb`, `qmtc2.ni`, `vitof0.xyzw`, `vmul.xyzw`, `sqc2`) is the prime suspect,
because its output signature is exactly "floats where fixed-point belongs".

Tooling added: `tools/ref_spr_payload.py` (sampled capture: every new (qwc,bank,prim,madr) shape plus
a periodic slice, writes per-hit bank+ring dumps and `hits.jsonl`), and `TOL_DUMP_SPR72=<path>` in
`ps2_memory.cpp` (dumps the port's 0x900-byte bank at the first QWC=72 flush, for a byte diff against
the reference).

### 199.10 TRACE OF 0x00156660 + stage-vs-ring comparison: the conversion site is NOT on this path, and the port flushes a DIFFERENT batch

`tools/ref_vertex_writer.py` (memcheck on `0x70001220-0x70001230` and `0x70000020-0x70000030`,
game-code PCs only) plus the existing `logs/ref199/h001_*.bin` capture.

**1. `FUN_00156660` is not the conversion.** Its whole body is three instructions:

```
0x156660  v1 = a0 + 0x1210
0x156664  t0 = *(u128*)a1
0x156668  *(u128*)v1 = t0
0x15666c  jr $ra
```

i.e. `*(ctx+0x1210) = *(a1)` - it copies the *colour qword* of the staged vertex. `a2` (the ring
address) and `a3` (PRIM) are not even read.

**2. `FUN_00160390` is not the conversion either.** It calls the flush first (`jal func_15C2A0`), then
builds the GIF tag at `*(ctx+0x12F4)` (`count | 0x8000 | PRIM<<46 | 0x3000000000000000`), writes REGS
`0x512` at +8, the `0x1000...` tag at `*(ctx+0x12F0)`, and appends a chain tag. No arithmetic on
vertex data.

**3. `FUN_0015C3B0` is verbatim** (`lqc2 vf1..vf3 @a1` / `sqc2 -> bank`). Confirmed live: the
reference's copier fires at `pc=0x15C408` and the bank receives exactly the staged qwords.

**4. The only float -> fixed conversion in this subsystem is `vftoi12.xyzw` in `FUN_0015bca0`**
(0x15BCE4), the sprite-context constructor: it builds `(0,0,1.0,0)`, converts with
`vftoi12.xyzw vf2,vf1` (corpus emits `src*4096; cvttps_epi32`), stores the packed result at
`ctx+0x1208`, then seeds the colour at `0x1210..0x121C`, the callbacks at `0x1248/0x124C`, and zeroes
`0x1250..0x125C`. Everything else on the vertex path (`FUN_0023A780`, `FUN_0023B800`) writes **floats**
(`cvt.s.w` + `swc1` at `ctx+0x1220/0x1224/0x1228`, colour built in VU0 as `vitof0` then `vmul` by
1/255).

**5. Stage vs ring at the card flush (reference, `h001`): they are NOT the same vertex, and the ring
is already GS-native.**

| buffer | u,v | colour | X,Y,Z |
|---|---|---|---|
| `ctx+0x1200` (staged) | 1.0, 1.0 | `3F008081` x3 + `3F800000` (0.502f,1.0f) | `43AC0000` `C3310000` `42800000` = **344.0, -177.0, 64.0 (floats)** |
| bank 0 / ring at MADR | 0.0/0.5/1.0 | `00000080` x3 + `00000020` (**bytes 0x80**) | `00007538` `00007A68` `000003F8` = **1877.5, 1956.5, 1016 (12.4)** |

Decoded, the reference batch is a **6-quad grid covering screen (83.5,134.5)..(427.5,311.5)**
(X in {0x7538,0x7738,0x88B8,0x8AB8}, Y in {0x7A68,0x7C68,0x8378,0x8578}), all vertices
`rgba=0x80,0x80,0x80,0x20`, `z=0x3F8`, UVs 0..1 - i.e. the logo-card panel - and it is stored in the
ring in **GS register form**.

The port's bank at the structurally identical first flush is instead **a 32x32 quad at (0,0) with
z=64 and colour 0.502f** - a *different sprite*, in the emitter's float form.

So two things are true at once, and they must be untangled before a code change:
- the ring format difference is real (reference = GS-native fixed/byte, port = pre-conversion float);
- the *batch identity* also differs (reference = 344x177 panel, port = 32x32 quad), so the port is
  not merely skipping a conversion on the same vertices.

**6. Memcheck evidence for the writer order** (`logs/ref199/writers.jsonl`, 70 records, 3.2..20.1 s):
`pc=0x23A8C4/CC/D4` and `0x23B964/6C/74` stage floats into `ctx+0x1220`; `pc=0x15C408` copies them
into the bank; `pc=0x15BD0C/10/14/18` (`FUN_0015bca0`) write the converted seed at boot. Bank 0
alternates between float and fixed contents across frames because it is reused by more than one
sprite stream - which is why a single-vertex comparison must be taken at the *flush of the batch being
investigated*, not at an arbitrary moment.

Next concrete step: at the card flush, capture the *sprite object* (`0x63D910` region) and the ring in
PCSX2, and dump the same two in the port with `TOL_DUMP_SPR72`; whichever batch the port flushes
(there: 32x32 quad at (0,0)) names the sprite-selection divergence. In parallel, probe
`TOL_TRACE_PC=0x15BCA0,0x15BCE4` in the port during the card window to see whether the converting
constructor runs at all for that sprite.

Tooling added: `tools/ref_vertex_writer.py` (memcheck-based writer tracing with per-hit stage/bank
dumps, game-PC filter).

### 199.11 Correction to 199.10: the port runs BOTH vertex families, so the batch comparison must be matched

Two more measurements, both live:

- `FUN_0015bca0` (the `vftoi12.xyzw` constructor) runs **once** in the port (`ra=0x15F754`, at boot) -
  same as the reference (its `pc=0x15BD0C..0x15BD18` writes appear only at t=3.2 s). It seeds the
  sprite context; it is not a per-vertex converter.
- There are **two families of vertex emitter**, and the corpus shows they differ in the store
  instruction, not in arithmetic:
  - `FUN_0023A780` / `FUN_0023B800`: `swc1` (float) at `ctx+0x1220/0x1224/0x1228`, colour built in
    VU0 (`vitof0` then `vmul` by 1/255) -> normalised floats.
  - `FUN_001c8f50` / `FUN_001c9410`: plain `sw` (integer) at `ctx+0x1220` -> GS-native 12.4.
  Port probe over 55 s: `0x1C9410` **12 entries**, `0x23A780` 8, `0x23B800` 8, `0x1C8F50` 0.

So the port does reach the integer (GS-native) emitter. The earlier byte diff compared the port's
**first** QWC=72 flush against the reference's first QWC=72 flush, and those two flushes belong to
*different* emitters (float vs integer) - the formats differ for that reason, and the "missing
conversion" framing in 199.9/199.10 is therefore **retracted as the explanation of that diff**.

What still stands, and is the real gate:
- the reference's card batch is a 6-quad grid at screen (83.5,134.5)..(427.5,311.5), `z=0x3F8`,
  `rgba=0x80,0x80,0x80,0x20`, stored GS-native in the ring;
- the port's first flush is a 32x32 quad at (0,0), `z=64`, colour 0.502f - i.e. a different sprite;
- therefore the port is not proven to submit the card's vertex batch at all, and any further diff
  must be taken from a **matched batch** (same emitter, same sprite, same frame).

Revised next step (replaces the one at the end of 199.10):
1. In PCSX2, break at `0x001C9410` and at `0x0023A780` during the card window and record which one
   draws the card panel (the 344x177 grid above) - by checking the 48 bytes it stages.
2. In the port, run `TOL_TRACE_PC=0x1C9410,0x23A780` with `TOL_DUMP_SPR72` and compare the *same*
   emitter's batch, not the first one that flushes.
3. Only then judge whether a conversion/format is missing; `vftoi12` and the four emitters are now all
   located, so the remaining question is batch selection, not arithmetic.

### 199.12 ROOT CAUSE LOCATED: the sprite vertex transform is the VU0 macro program at 0x15CCB0, and the live corpus has no dispatch case for it

Everything below is measured, with the tools named.

**A. Flush attribution (reference, 45 s, `tools/ref_flush_emitter.py` -> `logs/ref199/flushes.jsonl`)**

At the flush epilogue `0x15C390` the emitter is still on the stack, so `u32(sp+0x70)` names the
caller of `FUN_00160390` that requested the flush:

| emitter ra | who | flushes | qwc | bank format |
|---|---|---|---|---|
| `0x23B150` | `FUN_0023A780` | 10 | 72 (24 verts) | **fixed** |
| `0x23BC6C` | `FUN_0023B800` | 10 | 24 (8 verts) | **fixed** |
| `0x23B65C` | `FUN_0023B290` | 10 | 24 | **fixed** |
| `0x239F70` | `FUN_00239F70` | 22 | 12 (4 verts) | **fixed** |

All 52 flushes have `flush_ra = 0x1603A4`, i.e. every one comes from `FUN_00160390`. **Every flushed
batch in the reference is GS-native fixed**, including the card's 24-vertex panel
(`madr=0x01D00350`, screen (83.5,134.5)..(427.5,311.5), `z=0x3F8`, `rgba=0x80,0x80,0x80,0x20`).

**B. Every copy into the bank is a FLOAT copy** (`tools/ref_vertex_copy_sample.py`, breakpoint
`0x15C408`, armed at cycles ~2.95e9 so the window covers the card):

```
done: 1604 copies, counts={'float': 1604, 'fixed': 0, 'other': 0}
FIRST float: t=0.44 ra=0x23A8E0 stage=00000000 00000000 10000000 00000000 3F008081 3F008081 3F008081 3F800000
```

So the bank is filled with floats and is *converted afterwards*, between the last copy and the DMA.

**C. The converter is the second flush callback, `ctx+0x124C = 0x15CCB0`, a VU0 macro program**

`FUN_0015C2A0` calls it with `a0 = bank`, `a1 = bank + count*48`, `a2 = ctx+0x1230`. Its body:

```
0x15ccb0  t0 = *(a2+0); t1 = *(a2+4); ctc2 t0,vi18      ; clip flags in
0x15ccbc  lqc2 vf20, 0x20(a0)                           ; this vertex's XYZ (floats)
0x15ccc4  vmulax/vmadday/vmaddaz/vmaddw  -> vf30        ; 4x4 matrix transform (vf1..vf4)
0x15cce0  vdiv Q, vf0w, vf30w
0x15cce4  vclipw.xyz vf30, vf30w
0x15ccf0  vmul.xyzw vf22, vf22, vf8                     ; colour scale
0x15ccf4  vmul.xy  vf21, vf21, vf10 ; vadd.xy + vf11    ; uv scale/offset
0x15ccfc  vmulq.xyz vf30, vf30, Q                       ; perspective divide
0x15cd0c/1c/2c/3c  vminiw / vmaxw / vmul.xyz / vadd.xyz  ; depth clamp
0x15cd34  sqc2 vf21, 0x0(a0)                            ; write uv back
0x15cd40  vftoi0.xyzw vf22, vf22                        ; colour float -> byte
0x15cd4c  vftoi4.xyz  vf20, vf30                        ; ** position float -> 12.4 (x16) **
          ... sqc2 vf20/vf22 back into the bank
```

`vftoi4` is exactly the float -> 12.4 conversion the reference bank shows. The VU0 state it consumes
(vf1..vf11) is loaded by the *other* two callbacks, `ctx+0x1248 = 0x15CB70` and `0x15CBB0` /
`0x15CC00`, which are `vlqi` loaders (`ctc2 $zero,$vi1` then `vlqi.xyzw vf1..vf15`).

**D. Why the port never converts: the corpus has no `case` for those addresses**

| corpus | `entry_0015c7e0` case labels | 0x15CB70 | 0x15CBB0 | 0x15CC00 | 0x15CCB0 |
|---|---|---|---|---|---|
| `data/output_fixed` (live) | **4** (`0x15ccbc 0x15cd7c 0x15ce7c 0x15cf4c`) | no | no | no | **no** |
| `data/output_dense` | **544** | yes | yes | yes | yes |

The row is a merged Ghidra row (0x15C7E0..0x15CFxx) that contains several routines; the emitter only
emits `switch (ctx->pc)` cases for the row's *internal branch targets*. `0x15CCB0` (and the loaders)
are function-pointer entry points, so a call into them hits `default:` and the row restarts at
`0x15C7E0` - the documented systemic interior-entry defect. Consequences measured in the port:

- `TOL_TRACE_PC` shows `enter 0x0015CCB0 ... ra=0x0015C318` and an immediate return: the row ran from
  its start instead of executing the transform;
- the bank therefore keeps the float vertices, the SPR_FROM copy moves floats into the ring, and the
  GS decodes every vertex as `(0,0)` (all sprite tristrips are culled as empty, 197.30/199.5);
- `0x15C7E0` is only ever entered with `ra=0x1602E0/0x1602EC` (the PRIM builder), never from the flush
  call sites, confirming the fall-through never produced the transform.

**E. Fix options, in order of correctness**

1. **Durable (recommended): make the emitter emit a `case` for every instruction in a row**
   (`ps2xRecomp/src/lib/ps2_recompiler.cpp`, the Option A already named in the CURRENT STATUS table),
   regenerate, rebuild. `data/config_dense.toml` and `data/output_dense` (544 cases for this row) already
   exist and are the proof the corpus changes as expected. Cost: ~14 min regenerate + ~20 min build,
   plus the `entry_003a3198` monster TU (~52 min at `-O0`, which the CMake guard now selects).
2. **Interim, narrow:** regenerate only the affected rows into a dense corpus and build that corpus
   (still needs the emitter change, so 1 is no more work).
3. **Not viable:** an override re-implementing `0x15CCB0` in `tol_overrides.cpp`, because the VU0
   registers it consumes are loaded by two more miss-cased callbacks (`0x15CB70`, `0x15CBB0`) - all
   three would have to be re-implemented, including the `vlqi` VU0 data memory reads.

No code behaviour was changed in 199.9-199.12; only probes and tools were added
(`ref_vertex_writer.py`, `ref_flush_emitter.py`, `ref_vertex_copy_sample.py`).

Session close: the milestone is still blocked, but the block is now a **known, single, mechanical
corpus gap** rather than an unknown in the GS or the presenter. Next session starts at E.1.

### 199.13 DENSE CORPUS IS NOW THE LIVE BUILD (and it works): one bogus TU is stubbed

Answered the open question from 199.12: **do not go back to `config.toml`/`data/output`** (53,558 files,
the pre-session-60 unfolded map whose interior-entry fragments run the enclosing routine's epilogue with
`$ra == 0`), and **`data/output_dense` is usable** - it only ever lacked one translation unit.

What was done (all reversible, nothing deleted):

1. `data/output_dense/entry_003a3198_0x3a3198.cpp` -> `.cpp.disabled`. That "function" is the Ghidra
   export decoding a **data region** as code: 47,418 table slots covering `0x003A3198..0x003D167C`
   (step 4) - the range holding the texture/CLUT blobs (`0x3A7000`), the sprite vtable (`0x3D4CA0`)
   and the lookup tables. 11.5 MB of source, ~52 min at `-O0`, and it is the single TU that made the
   dense build look impossible. It is not code; nothing executes there.
2. `src/runtime/tol_overrides.cpp` now defines `entry_003a3198_0x3a3198` (same signature, C++ linkage)
   as `ctx->pc = GPR_U32(ctx, 31)` - a `jr $ra`, i.e. what the first byte there would do if it were a
   return. The dense corpus supplies the remaining 13,519 TUs.
3. `TOL_GENERATED_DIR` switched to `data/output_dense`; ninja reused the 13,519 existing dense objects,
   archived `tol_generated.lib` and linked. `tol.exe` = 98,819,584 B (11/23 21:37).
   Rollback copy of the pre-dense binary: `build/tol2/src/runtime/tol_fixed_before_dense.exe`.

**Result: the interior-entry defect is gone and the sprite transform runs.** The first QWC=72 flush of a
run now dumps a **GS-native** bank (`TOL_DUMP_SPR72`), where the `output_fixed` build dumped floats:

```
before (output_fixed): v0 = 00000000 00000000 10000000 00000000 3F008081 3F008081 3F008081 3F800000 | 00000000 00000000 42800000
after  (output_dense): v0 = 00000000 ... 00007FF8 00000000 00000000 00008000
```

Independent confirmation: `TOL_TRACE_PC` on `0x15CCB0` is no longer a fall-through; the row's dense
corpus has 544 case labels (vs 4) including `0x15CCB0`, and `label_15ccb0` contains the
`vftoi4.xyz vf20, vf30` conversion.

**Next gate, measured: the VU0 transform's INPUTS are zero.** `0x15CCB0` computes
`vf30 = M * (x,y,z,1)` from `vf1..vf4` and scales from `vf5..vf11`; those are loaded by the sibling
callback `ctx+0x1248 = 0x15CB70` (`ctc2 $zero,$vi1` + eleven `vlqi.xyzw vf1..vf11,(vi1++)`, i.e. a
macro-mode load from VU0 data memory). Measured:

- the port's **VU0 data memory is all zeros** (`runtime->memory().getVU0Data()`, dumped via
  `TOL_TRACE_VU0MEM`), while PCSX2 read at `0x11004000` returns real data
  (`... 3A000801`, `B4000001`, `3F8000003F800001`);
- `processVIF0Data` is **never called** in a 45 s run (`vif0data calls: 0`), and the game only writes
  four VIF0 registers (`0x10003810`, `0x10003820`) - no VIF0 DMA and no VIF0 FIFO traffic;
- the game never references `0x1100xxxx` anywhere in the corpus (no `lui 0x1100`, no
  `0x11004000` store), so it does not write VU0 data memory directly either;
- the corpus emits macro-mode `vlqi` as `READ128(((vi1 & 0x3FF) << 4))`, an **EE** address, not VU0
  data memory - a recompiler fidelity bug that is masked only because the memory is empty anyway.

So the matrix must arrive from something the port does not model. Next probe (single): a PCSX2
memcheck on `0x11004000..0x11004020` during the card window, which names the writer (VU0 micro store,
a VIF0 packet, or a BIOS-side initialisation) - then implement that path and let the existing
`vftoi4` transform produce real screen coordinates.

Interim state: bank is GS-native but degenerate (`X=0x7FF8` clamp, `Y/Z/colour = 0`), so the card batch
is still culled; the screen is unchanged. No regression: same draw classes, same boot timing, no fatal
lines. The `spriteVu0Load11` override (0x15CB70) is installed and correct - it reads VU0 data memory -
so it will start working the moment that memory is populated.

### 199.14 VU0-memory writer probe: nothing writes it from the EE during the card window; the region holds a VIF command stream

Went after the last measured gate (199.13: the transform runs but its matrix is zero because VU0 data
memory is empty).

**A. EE-store memcheck on 0x11004000..0x11004040** (`tools/ref_vu0mem_writer.py`, two runs):

- the only writer through the EE store path is the **EE kernel's VU0 clear loop at `pc=0x8000E118`**
  (`a0=0x11000000`, stores zeros) - it runs throughout boot;
- after filtering that loop and arming at ~8 s (cycles 2.4e9), the 8-48 s window produced **0 hits**:
  nothing in game code writes VU0 data memory during the logo cards.

So the upload does not go through an EE store: it is a **DMA/VIF0 unpack** (which writes VU memory
directly and is invisible to an EE memcheck) or VU0 micro-mode.

**B. What the reference actually has there at the card flush** (`tools/ref_flush_vu0.py`, breakpoint
0x15C390, reading 0x11004000 at each flush - identical for all 13 flushes sampled):

```
0x11004000: 3A000801 00000000 00000000 00000000
0x11004010: 00000000 3A000801 00000000 00000000
0x11004020: 00000000 00000000 B4000001 00000000
0x11004030: BDAC0AC1 3D320B21 3F800001 3F800000
```

`0x3A000801` and `0xB4000001` are **VIF UNPACK opcodes** (0x3A = UNPACK, 0xB4 = UNPACK|IRQ; low bits =
vn/vl, high byte = num), followed by float payload (`-0.0840, 0.0434, 1.0000001, 1.0`). So this region
holds a **VIF0 command/data stream**, not a plain matrix, and the same flushes produce the correct
GS-native bank (`0x7538 0x7A68 0x3F8`).

Combined with 199.13 (`processVIF0Data` never called; only four VIF0 register writes at 0x10003810 /
0x10003820; no 0x1100xxxx references anywhere in the corpus), the conclusion is that the port is missing
the **VIF0 upload path** (DMA or FIFO) that fills VU0 data memory, and additionally the corpus emits
macro `vlqi` as an EE read.

Next step (single, concrete): watch the *DMA* side, not EE stores - memcheck/breakpoint on VIF0's
`CHCR` (`0x10008000`) and on the VIF0 FIFO (`0x10004000`) in PCSX2 during the card window, and the same
two in the port with `TOL_TRACE_VIF0`. Whichever fires names the path to implement.

### 199.15 THE CARD GEOMETRY AND TEXTURE NOW REACH THE RASTERISER (three fixes, measured)

Three separate defects were each confirmed and fixed this round. The first logo card is not yet
visible on screen, but for the first time its **textured geometry is submitted and rasterised**.

**1. Dense corpus live, bogus TU stubbed (see 199.13).** `data/output_dense/entry_003a3198_0x3a3198.cpp`
disabled (data region decoded as code: 47,418 slots over 0x3A3198..0x3D167C), replaced by a `jr $ra`
stub in `tol_overrides.cpp`. The VU0 transform at `0x15CCB0` now runs.

**2. VU0 data memory seeded from the reference.** The transform's matrix/scales are loaded from VU0
data memory by `0x15CB70` (`vlqi`), and the probe chain (199.13/199.14) showed nothing in the game's EE
code ever writes that memory: no EE store in the card window (only the kernel's clear loop at
`0x8000E118`), no VIF0 DMA, no `0x1100xxxx` reference anywhere in the corpus. The reference image was
therefore captured at the card flush (`tools/ref_flush_vu0.py` -> `logs/ref199/vu0mem.bin`, 4096 bytes)
and is loaded by `TOL_VU0_SEED=<path>` at init (file kept outside the source tree: it is game state).
Also fixed here: `spriteVu0Load11`, a correct override for `0x15CB70`, because the corpus emits
macro-mode `vlqi` as an EE read instead of a VU0 data-memory read.

**Effect, measured at the first QWC=72 flush** (`TOL_DUMP_SPR72`, port vs reference `h001_spr.bin`):

```
port v0: 00000000 00000000 3F800000 00000000 | 00000080 00000080 00000080 00000020 | 00007538 00007A68 000003F0
ref  v0: 00000000 00000000 3F800000 00000000 | 00000080 00000080 00000080 00000020 | 00007538 00007A68 000003F8
```

**36 of 1152 bytes differ, and only in Z (0x3F0 vs 0x3F8).** UVs, colour bytes and X/Y match exactly.

**3. SPR_FROM copy re-enabled.** The session-159 change completed the channel without writing memory
(correct then: the bank held floats and the copy corrupted the payload). With a correct bank the copy is
the transport again: `copyOutOfScratchpad(sadr, madr, qwc)` in `ps2_memory.cpp` mode 0.

**Result - two new draw classes, both textured, into page 224 from the card texture:**

```
cl=7 fbp=224 type=4 iip=1 tme=1 tex tbp0=14976 tbw=1 psm=20 cbp=14960
     rgba0=128,128,128,32 v0=(1875.5,1958.5) v1=(1875.5,1990.5) written=[82,133..117,168] verts=3
cl=8 fbp=224 type=4 iip=1 tme=1 tex tbp0=14976 tbw=8 psm=19 cbp=14960
     rgba0=128,128,128,32 v0=(1619.5,1734.5) v1=(1619.5,2246.5) written=[0,0..341,424] verts=3
```

Before this round the run had **6** classes and no textured tristrip at all; it now has 8, and the
320-frame capture is no longer byte-identical black (the frame contains a dark red region and a grey
field). The card is therefore being drawn - it is just not yet reaching the visible page correctly
(the sampled screen colours are black / grey / dark red, `rgb(38,7,8)`).

Remaining gap, narrowed to presentation/alpha:
- `cl=7/8` write into page 224 with `rgba=128,128,128,32` (alpha 32, ABE on) sampling `tbp0=14976`;
  the display copy 224->0 was verified textured and correctly ordered in 197.20, so the next
  measurement is whether the batch's alpha/blend leaves the page unchanged (alpha 32/128 over a black
  page stays near black), or whether the card's own texture page content is still the "NOW LOADING"
  text rather than the logo (the `tbw=1 psm=20 24x16` vs `tbw=8 psm=19 512x512` pair suggests two
  different texture bindings for the same page).
- Environment knobs added this round: `TOL_VU0_SEED=<path>`, `TOL_TRACE_VIF0` (VIF0 register/data
  traffic), plus tools `ref_vu0mem_writer.py`, `ref_flush_vu0.py`.

**Repo hygiene (requested):** the obsolete configs were archived rather than deleted -
`data/archive/config_unfolded_do_not_use.toml`, `config-linux_unfolded_do_not_use.toml`,
`config_orig_duplicate_of_fixed.toml`. `data/config_fixed.toml` is no longer tracked by git
(`git rm --cached`, file kept on disk); the live config is `data/config_dense.toml` and
`config/map.csv` stays tracked because the dense config references it for regeneration.

### 199.16 VISUAL CONFIRMATION: the logo card renders (first time)

Run with `TOL_VU0_SEED=logs/ref199/vu0mem.bin`, screenshots every 5 s from 20-55 s
(`logs/run199/show/`). From 30 s on, the frame contains the card:

- the card field is `rgb(58,58,58)` - the card's white background at the fade level the guest is using
  (`rgba=128,128,128,32` from the vertex batch, i.e. 32/128 = 25 % of white);
- the red wordmark is `rgb(38,7,8)`, laid out as five letterforms across `x 0..700` - the namco logo
  block. Brightened copies for viewing: `logs/run199/show/CARD_bright4x.png` (4x) and
  `CARD_native.png`.

So the whole chain now works end to end: dense corpus -> VU0 transform -> GS-native vertex bank ->
SPR_FROM bank->ring copy -> textured tristrips into page 224 from `tbp0=14976` -> display copy.

Two observations from the same run:
- `f30`, `f40` and `f55` are **byte-identical** (MD5 `88EADCC0...`), so the presented frame is static
  during that window even though the guest keeps drawing - presentation (or the guest's fade step) is
  now the thing to look at, not geometry;
- the card is at 25 % brightness, matching the fade alpha in the batch, so the fade-in itself is not
  being animated in the presented frames yet.

Next: (a) check whether the guest is advancing the fade counter during that window (`TOL_TRACE_PC` on the
fade owner, or `TOL_GFX_STATS_EVERY=1`), and (b) whether the presenter is re-presenting one frame
(compare `[gfx]` present counters against `[gsdraw]` totals).

### 199.17 THE JUMP IS A STALE MATRIX, NOT A PRESENTER BUG; VU0 memory is not written by EE stores at all

User observation: the picture "jumps from the middle to the left" and the card appears without its text
at first, then the namco wordmark appears.

Measured:

1. **The card batch in the port is pixel-exact.** Comparing all 24 vertices of the first QWC=72 flush
   (`logs/run199/seeded_bank72.bin` vs `logs/ref199/h001_spr.bin`):
   ```
   ref  X set: 7538 7738 88b8 8ab8      port X set: 7538 7738 88b8 8ab8   (identical)
   ref  Y set: 7a68 7c68 8378 8578      port Y set: 7a68 7c68 8388 8588   (1 px on two vertices)
   ```
   So the geometry the transform produces from the seeded VU0 image is correct.

2. **The offset comes from later frames.** Draw class `cl=8` (the full card panel, `tbp0=14976 tbw=8
   psm=19`) reports `v0=(1619.5,1734.5)`, i.e. screen x = -172.5 and a written rect `[0,0..341,424]`,
   while the reference panel sits at x 83.5..427.5. 1619.5 x 16 = 0x6538, and 0x7538 - 0x6538 = **0x1000
   = 4096 in 12.4 = exactly 256 px** - one half of the 512-wide frame. r anches: the *later* card frames
   use a translation the port's frozen seed does not have.

   Cause: `TOL_VU0_SEED` loads one 4 KB snapshot of VU0 data memory. The game updates that matrix as the
   card animates, and a static seed can only be right for the moment it was captured.

3. **Nothing writes VU0 data memory through an EE store.** Full-range memcheck `0x11004000..0x11005000`
   over a fresh boot + card window: **26,188 hits, every one the kernel clear loop at `pc=0x8000E118`**,
   zero from anywhere else. Combined with 199.13/199.14 (no VIF0 DMA, no VIF0 FIFO, no `0x1100xxxx`
   reference in the corpus), the per-frame fill must be **VU0 micro-mode stores** (a microprogram writing
   its own data memory) - the only remaining producer.

4. Presentation is static across the window: `t28..t60` captures are byte-identical, and `t12..t24` too,
   so the presenter is re-showing one frame per phase rather than animating.

Next steps, in order:
1. Implement/verify VU0 micro-mode execution for this title (the runtime has `executeVU0Microprogram`),
   or detect the microprogram upload and run it, so VU0 data memory is produced live.
2. Then re-check the two card classes: the panel should stay at x 83.5..427.5 instead of drifting to
   -172.5, which is exactly the reported middle->left jump.
3. Only if the jump persists with a live matrix, look at the presenter's aspect path
   (`sdlGpuDisplayRect` centres the picture: `rect.x = (windowWidth - width)/2`), which is not the cause
   today.

### 199.18 STEP 1 MEASURED: VU0 memory is uploaded by the PS2 KERNEL through the VIF0 FIFO - not by game code, not by DMA

Asked to measure (not fix) how VU0 data memory gets produced, skipping the card-position and presenter
work for now.

**Measurement (PCSX2, fresh boot, memcheck on `0x10008000..0x10008010` (VIF0 CHCR) and
`0x10004000..0x10004010` (VIF0 FIFO)):** the FIFO range fires continuously from the very first second,
always at

```
pc=0x8000E274  ra=0x8000D860  a0=0x00000010  a1=0x05000000000002000000001000404  a2=0xFFFFFFFF800161D0
```

i.e. **EE kernel code at 0x8000E274 writes the VIF0 FIFO**. That is the upload: the kernel pushes micro
code/data through the VIF0 FIFO, and the VIF0 unpack writes VU0 memory. No CHCR writes were seen, so it is
FIFO-driven, not a DMA channel.

This is consistent with every earlier negative result: no EE store writes `0x11004000` in game code
(the only EE writer of that range is the kernel's clear loop at `0x8000E118`), no VIF0 DMA, and no
`0x1100xxxx` constant anywhere in the corpus.

**Port consequence:** the port never writes the VIF0 FIFO (`TOL_TRACE_VIF0`: `vif0data calls: 0`), because
this upload is *kernel/BIOS* code: the game reaches it through a syscall, and the runtime's syscall layer
does not implement the VU0 upload. So the missing piece is a **syscall implementation**, not a
memory-mapped path.

Work done in this step (kept, currently inert):
- `ps2_memory.cpp` now stages writes to `0x10004000`/`0x10004008` in `g_vif0Fifo` and hands each completed
  16-byte quadword to `processVIF0Data`, and does the same for 128-bit stores at `0x10004000`. This is the
  correct hardware-side plumbing and it will start working the moment something in the port writes the
  FIFO.

Next measurement (single): log the guest's syscall numbers in the port and find the one the kernel uses
for this upload - it will be the syscall whose runtime handler is a no-op today. Then implement it to copy
the guest buffer into VU0 code/data memory (the same bytes the reference's kernel pushes through the
FIFO). After that, drop `TOL_VU0_SEED` and re-run; the matrix then comes from the game itself.

### 199.19 SYSCALL CENSUS: there is no VU0-upload syscall - the upload is a kernel function reached by pointer

Requested measurement: trace guest syscall numbers to name the kernel call that uploads VU0 data.

Added `TOL_TRACE_SYSCALL_CENSUS=1` (`ps2_runtime.cpp::handleSyscall`): reports every distinct syscall
number once with a0..a3, the call site and $ra. A full 40 s boot produced exactly **six** distinct
syscalls:

```
id=0x3C a0=0x003E2BF0 a1=0xFFFFFFFF a2=0x00100000 pc=0x001001CC ra=0x0
id=0x3D a0=0x004E9B80 a1=0xFFFFFFFF a2=0x00100000 pc=0x001001E8 ra=0x0
id=0x74 a0=0x00000083 a1=0x0011E8C0 a2=0x00100000 pc=0x0011EA50 ra=0x0011E988   (SetSyscall)
id=0x83 a0=0x80000000 a1=0x80080000 a2=0x0011E8C0 pc=0x0011E908 ra=0x0011E9A4   (FindAddress)
id=0x5A a0=0x80076000 a1=0x00388550 a2=0x00000740 pc=0x0011EEE8 ra=0x0011EF98
id=0x5B a0=0x000000FC a1=0x80076000 a2=0x00000028 pc=0x0011EF30 ra=0x0011EFD8
```

None of these is a VU0 upload, and the runtime's numeric dispatcher has no VU0/VIF0 entry either (checked
the whole `Kernel/Syscalls/Dispatcher.cpp` table: 0x61/0x62/0x64/0x6E..0x78/0x7C/0x7F/0x82/0x83 - no
VU0). So the upload is **not** a numbered syscall.

Combined with 199.18 (the FIFO writes come from EE **kernel** code at `pc=0x8000E274`, `ra=0x8000D860`,
`v1=0x10004000`, 16-byte stores, `a0=0x10`), the only remaining mechanism is a **kernel function called
through a function pointer** - the classic libvu0/libgraph pattern where the game obtains a kernel entry
with `FindAddress` (syscall 0x83, which this title calls at `pc=0x0011E908`) and then jumps to it. That is
why the port never writes the FIFO: the recompiled corpus contains no code at 0x80000000+, so such a call
cannot execute.

Data word captured at the FIFO write: `a1 = 0x0500000000000200_0000000010000404`, i.e. VIF command
words (`0x10000404` -> opcode 0x10 FLUSHE; `0x05000000` -> opcode 0x05 STMOD), confirming a VIF0 command
stream being pushed 16 bytes at a time.

**Conclusion for step 1:** the VU0 upload cannot be fixed by implementing a syscall number - there is
none. The native substitute must either
1. intercept the guest's kernel-pointer call (find the pointer value in `ctx->pc` at the jump and emulate
   the VU0 upload there), or
2. keep the captured VU0 image as the seed (current state) until (1) is done.

Next measurement for (1): log any guest branch whose target is >= 0x80000000 (the runtime already reports
`[guest-branch:missing-target]` for null targets, so this is a small extension), which names the exact
kernel entry the game jumps to.

---

## Session 200 (2026-11-25) - IPU LANDED (real MPEG-2 model wired into the runtime) + the unit-1 wait measured: it is NOT a deadlock, the guest's own cooperative scheduler runs the 90-iteration wait at ~1.4 s per turn

Two things happened this session: the **IPU (gate B of session 197) is now implemented and wired into
`ps2_memory.cpp`**, and the boot-progression stall at unit 1 was measured properly for the first time
(the earlier "stuck at unit 1 / static frame" reading was a misdiagnosis of *what* is stuck).

### 200.1 IPU: a real MPEG-2 macroblock decoder is in the runtime now

New files (all inside the pinned submodule, so `git submodule update` would drop them until pushed):

| file | contents |
|---|---|
| `thirdparty/PS2Recomp/ps2xRuntime/include/ps2_ipu.h` | public surface: `attach/init/read_reg/write_reg/fifo_read/fifo_write/dma_in_start/dma_out_start/kick/command_count`, `Channel gChannel[16]`, `setDmaCompleteHandler` |
| `.../src/lib/hw/ps2_ipu.cpp` | the decoder, adapted verbatim from `D:\Personal\C++\grandia-ps2recomp\runtime\src\hw\ipu.cpp` (VLC LUT build over the MPEG-2 tables, intra/non-intra block decode, IDCT, dequant, DC predictors, VDEC/BDEC/IDEC/FDEC/SETIQ/SETVQ/CSC/PACK, input/output FIFOs, IPU_BP/TOP) |
| `.../src/lib/hw/mpeg_tables.h` | the MPEG-2 VLC + non-linear quantiser tables |
| `.../src/lib/hw/ipu_common.h` | shim for the foreign runtime's names (`hw::dma`, `mem::dma_ptr`, `log_write`, `kernel::raise_intc`, `ss::Stream`) backed by THIS runtime |

Wiring in `ps2_memory.cpp`:

- `initialize()`: `ipu::attach(m_rdram)`, `ipu::init()` (11 VLC LUTs, ~3.6 MB), and a
  `setDmaCompleteHandler` lambda that publishes the channel and raises `D_STAT` bit 3/4.
- `0x10002000..0x10002030` writes -> `ipu::write_reg`, reads -> `ipu::read_reg` (IPU_CMD busy/result,
  IPU_CTRL, IPU_BP occupancy, IPU_TOP). Live channel state (QWC above all) is mirrored into the
  register file after every command, because the guest polls `IPU_TO`'s QWC to decide whether to feed
  more bitstream.
- `0x10007010..0x1000701C` word stores are accumulated into one quadword -> `ipu::fifo_write`;
  `0x10007000..0x1000700C` reads come from `ipu::fifo_read`.
- DMA channel 3 (`0x1000B000`, IPU_FROM = decoded picture out) and channel 4 (`0x1000B400`, IPU_TO =
  bitstream in) are no longer "complete immediately with zeros": the model owns them and completes them
  through `dma_finish_external_ipu()`. `TOL_TRACE_IPU_DMA=1` prints the first 16 starts per channel.

CMake: `src/lib/hw/ps2_ipu.cpp` added to `ps2_runtime`, with `UNITY_GROUP ipu` so the adapted file is
not merged into a neighbour's unity TU.

Status: builds clean (`ninja -C build/tol2 tol`, clang-cl) and the boot is unchanged (no regression;
same unit-1 state after 50 s). **Not yet exercised**: the movie is behind the three logo cards, so the
decoder has not run on real data yet. First test when the boot reaches ~28 s of guest time:
`TOL_TRACE_IPU=1` for register traffic, `TOL_TRACE_IPU_DMA=1` for the channels, and
`ipu::command_count()` (add a print) for "are commands completing".

### 200.2 The unit-1 wait is a 90-iteration yield loop, and each iteration costs ~1.4 s

`FUN_00204240` mode 1 does: fade-in loop (25 yields, `f20 += 0.04`, verified 0.04 -> 1.00), then a
**bounded wait loop** at `0x20439C..0x2043F8`: up to `0x5A` (90) iterations of
`func_1520C0 -> func_1520F0 (status byte of TCB+0x1F2) -> func_153010(0) & 0x10000` with
`jal func_15231C` (yield) at the end of each; exit early when a condition is met, otherwise fall through
to `0x204400` after 90. New probes (`TOL_TRACE_YIELD=1`, `installYieldProbes` in `tol_overrides.cpp`):

- `[unitwait] resume #N s0=K loader=0x63D890 mode=1` at `0x2043F0` � the loop head after each yield.
- `[unitwait] LOOP-EXIT #N` at `0x204400` � printed **0 times** in a 90 s run: the loop never leaves.
- `[yield] #N ra=... wall=..ms guestCyc=..ms` for every `0x15231C` switch, with the cost of the turn.

Measured (65-90 s runs, `TOL_VU0_SEED=logs/ref199/vu0mem.bin`, `--gs=sdlgpu`):

```
green-thread cycle (6 yields):  ~77 ms
  ra=0x1F8168 (GS worker 1)     80 ms per turn     <- the expensive one
  ra=0x2DF7F4 (TCB5)            21 ms
  ra=0x164FE8 (loader job step) 12 ms
  ra=0x1F8528/0x2047D0/0x3661DC 0.2-1 ms
loader thread (0x2043F0) resumptions: 64 in 90 s  -> one per ~1.4 s
after ~7 min: paceWait 91 % of wall, loader state=1, still unit 1
```

Reference (PCSX2, `tools/ref_unit_wait.py`: breakpoints `0x2043A4` / `0x204400`) does **exactly the
same loop with the same status byte `+0x1F2` = 0 and `s0` counting up**, and falls through after 90 �
unit 1 completes ~5 s after it starts (boot capture: mode 1 at 12.0 s, mode 2 at 17.2 s). So the logic
is not wrong; the **turn rate** is: the guest's cooperative scheduler is ~80x slower than hardware here.

The expensive turn is *not* the `func_102DB0() == 0x20` poll (it returns 2, so that loop never runs: 3
calls per 20 s, `[gsstatus]` probe), not the GS flush (`func_139AD0`: ~11 ms, 3 calls per 65 s,
`[gsflush]` probe), and not `func_1F7F30` (~11 ms, 3 calls). The time is inside `func_1F8000`'s own
body beyond those calls, i.e. the worker's per-turn work, and it is **backend-independent**
(`--gs=sw` shows the same 80 ms and the same 64 resumptions).

### 200.3 Retraction: `mainIters` freezing at 108 is NORMAL

`TOL_TRACE_SCENE=1` shows the `0x152B70` frame-body loop running 100+ iterations between t=0.9 s and
t=5.4 s and then *exiting* (`[mainloop] exitPred #100 ret=0 continue loop`, nothing after 5.4 s). The
long-standing "mainIters frozen at 108 = the frame loop died" reading in several sessions is therefore
wrong: the frame body is an early-boot loop that finishes by design.

### 200.4 What is left, in order

1. **Unit turn rate.** The guest is idle 91 % of wall at 7 min (paceWait), and the 90-yield wait needs
   one green-thread turn per iteration, so the whole boot-to-movie phase is bounded by whatever makes a
   green-thread turn take ~1.4 s instead of ~16 ms. The two prio-0 workers (0x1F8168/0x1F8528) are the
   only expensive turns; their body (`FUN_001F8000` -> `FUN_001F8180`/`FUN_001F82F0`/... plus
   `func_281490`/`func_1F7A30`) has not been profiled internally yet. That is the next measurement:
   wrap `FUN_001F8180`, `FUN_001F82F0`, `0x139300`, `0x139A20` the same way (`[proctime]`).
2. **Then** the three cards (now visible: namco at 25 % alpha, matrix still the frozen `TOL_VU0_SEED`),
   then the movie/title on the new IPU.
3. VU0 data memory still comes from `TOL_VU0_SEED`; the live producer is a kernel VIF0 upload reached
   through a `FindAddress` pointer (session 199.19), still unimplemented.

Tooling added this session: `tools/ref_unit_wait.py` (PCSX2 side of the unit-1 wait), and the env-gated
probes `TOL_TRACE_YIELD` (`[yield]`, `[unitwait]`), `TOL_TRACE_GSSTATUS` (`[gsstatus]`, `[gsflush]`,
`[proctime]`), `TOL_TRACE_IPU_DMA` � all inert unless set.

### 200.5 Where the stall actually is (correction to 200.2)

Ran the wait loop long enough to settle it (`TOL_TRACE_YIELD=1`, `--gs=sdlgpu`, 180 s):

```
resumes at 0x2043F0: 64 (s0 = 0..63)   exits at 0x204400: 0 logged
same 64 resumes in a 90 s run and in a 180 s run -> deterministic, not a timeout
guest time frozen at ~36.4 s; paceWait 80-91 % of wall; guest work 0.3 % of wall
[fade] #1 (mode 1) enters and exits; [fade] #2 (mode 2) never arrives
```

So the 90-iteration limit is **not** what ends the loop: `func_1520F0()` returns 1 (the TCB status byte
`+0x1F2` flips) after 64 iterations and the loop falls through to `0x204400` in both runs. The stall is
therefore **after the wait, in FUN_00204240's mode==1 tail** (the scene-start sequence that session 158
measured: `FUN_00285E80(a0=0x5AA170, a1=4, a2=0xE0)`, then the four idle entities, then the next unit),
not in the loader wait itself and not in the fade. The green scheduler still rotates the other five TCBs
(`[gthreads]`: worker1 0x1F8168, worker2 0x1F8528, 0x2DF7F4, loader job 0x164FCC, unit loop 0x2047D0),
so the port is not dead -- the unit-1 tail simply never returns to the loop that would start unit 2.

Next session starts here: `TOL_TRACE_SCENE=1` for 150 s and read the `[scenetest]` scene-start lines,
then follow the mode==1 tail after `0x204400` (fade-out loop at `0x204340`-`0x204390`, `gFadeCalls` must
reach 2). The IPU work (200.1) is independent and waits for the movie phase, which is behind this tail.

---

## Session 201 (2026-11-25) - TRACED: the mode-1 tail at 0x204400 is NEVER entered. The fade thread (TCB 0x4079A0) is starved after its 64th yield - still alive, still queued, never dispatched again. The IPU is untouched.

Answers the two questions left at the end of session 200: (a) trace the tail after `0x204400` until the
second fade, (b) does the opening movie exercise the new IPU.

### 201.1 The fade's wait loop stops being scheduled; it does not time out and does not block

New probes (all `TOL_TRACE_YIELD=1`, installed in `installYieldProbes`):

| probe address | what it says |
|---|---|
| `0x2043F0` | `[unitwait] resume` - loop head after each yield, with the current green TCB and its fields |
| `0x204400` | `[unitwait] TAIL-ENTRY` - the mode==1 tail (scene start) is entered |
| `0x2044A0` / `0x204524` | `[tail] RETURN` / `[tail] STATUS-PATH` |
| `0x1520C0`, `0x1520F0`, `0x153010` | `[tailcall] in/out` - the three calls in the loop body |

Measured over 150 s runs (`TOL_VU0_SEED=logs/ref199/vu0mem.bin`, `--gs=sdlgpu`):

```
resumes at 0x2043F0 : 64   (s0 = 0..63), identical in every run (90 s, 116 s, 146 s, 150 s, 180 s)
TAIL-ENTRY at 0x204400: 0
[tail] RETURN        : 0
[scene] 285E80       : never (the scene start in the tail is never called)
[fade] #2            : never  (gFadeCalls stays 1)
[tailcall] 1520C0    : 64 in / 64 out, ret 0x004079A0   (all returned)
[tailcall] 1520F0    : 64 in / 64 out, ret 0x00000000   (all returned; status byte still 0)
[tailcall] 153010    : 0 calls (the mode==1 short-circuit skips it)
```

So the 65th loop iteration's body is never entered: the thread is neither running nor blocked inside a
callee - it is simply never dispatched again. This **corrects 200.5** ("exits after 64 and stalls in the
tail"): the tail is never reached at all.

TCB identity and state (`[gtcb]`/`[gchain]` samples at every 200th yield):

```
TCB 0x4079A0 (prio 4): +1AC toggles 1/0 (alive for the run loop), +1F0=1 (ready), +1F1=4,
                       +1F2=0, +1F3=1, +1F8(prev)=0x00407580, +1FC(next)=0x00000000
ready list from head 0x407160: 0x407370 -> 0x407790 -> 0x407580 -> 0x4079A0 -> 0
```

The node is **not deleted and not orphaned**: it is reachable from the head, state=1 (ready), prio 4, and
the chain of the other five TCBs is intact. The port is idle while this happens (guest work 0.3 % of
wall, paceWait 80-91 %), and the other threads (0x407160/0x407370 at prio 0, plus 0x407790/0x407580)
keep taking turns.

### 201.2 The IPU decoder is not exercised at all

150 s with `TOL_TRACE_IPU=1` and `TOL_TRACE_IPU_DMA=1`:

```
IPU_CMD writes = 0   IPU_CTRL/BP/TOP writes = 0   IPU register reads = 0
ipu-dma starts = 0   IPU command log = 0
only movie-related line: [Deci2Call:kputs] DVCI: "\AFS\MOVIE.AFS" found.   (boot-time AFS index)
```

So the decoder added in session 200 has not run: the opening movie is behind the unit-1 tail, and the
tail never runs. Gate B therefore remains *unverified* (not failed - unexercised).

### 201.3 Next step: why does the guest run loop stop selecting 0x4079A0?

The green run loop resumes a thread from a context block at `0x49F590` indexed by `*(0x3EB1A0)` with
stride `*(0x3EB1B0)` and offset `*(0x3EB1C0)` (see `FUN_0015231C`), and the current thread is named at
`0x3DB198`. The exact signature observed here - a live, ready, queued TCB that is never dispatched again -
is what two TCBs sharing one context slot would produce (the second one always resumes the first one's
`saved $ra` and the first never comes back).

Next measurement (single): at every yield, log `idx=*(0x3EB1A0)`, `stride=*(0x3EB1B0)`,
`off=*(0x3EB1C0)`, `cur=*(0x3DB198)`, and the resolved context pointer
`0x49F590 + idx + off`, for 0x4079A0 and for the two prio-0 workers in the same window; a repeated
context pointer for different TCBs names the collision. Also print `ready[4..7]` (only `ready[0..3]`
was sampled: it reads `2,0,0,0`), to check the prio-4 ready count against the three queued prio-4 TCBs.

Tooling in this session: the probes above (all inert without `TOL_TRACE_YIELD`), no runtime behaviour
change.

### 201.4 The decoder itself is verified live (self-test), even though the movie never reaches it

`TOL_IPU_SELFTEST=1` (in `ps2_memory.cpp::initialize`, right after `ipu::init()`) feeds one quadword of
all-ones into the data FIFO and issues `IPU_CMD = 0x30000000` (VDEC, table 0 = macroblock address
increment) - the exact command `FUN_00129258` uses. Measured:

```
[ipuselftest] top_before=0xFFFFFFFF cmd=0x00010001 busy=0x00000000 bp=0x00010001 top_after=0xFFFFFFFF commands=1
```

which is the hardware contract the guest polls: the command completes (busy bit clear), the decoded MBA
(1) is returned in `IPU_CMD[15:0]` with the "data valid" flag `0x10000`, the one-bit code consumes exactly
one bit (`IPU_BP` low = 1), `IPU_TOP` exposes the next 32 input bits, and the command counter advances.
So the register/FIFO/decode path is functional; what is missing is only *real bitstream traffic* (the
movie), which the unit-1 tail still blocks.

Status of gate B: **implemented, wired, self-tested, not yet fed real video**.

Tooling in this session: the probes above plus `TOL_IPU_SELFTEST` (all inert without their env var), no
runtime behaviour change.