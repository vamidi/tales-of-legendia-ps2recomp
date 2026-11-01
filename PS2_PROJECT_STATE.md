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
