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

The root `CMakeLists.txt` references files that were not pasted. Configure will fail until each is either supplied or
removed:

| Missing | Referenced by | Notes |
|---|---|---|
| `cmake/Dq8Utf8CodePage.cmake` | `src/runtime/CMakeLists.txt:8` | UTF-8 code page on MSVC |
| `cmake/Dq8Icon.cmake` | `src/runtime/CMakeLists.txt:10` | embeds `res/icon/dq8.icns` |
| `cmake/Dq8HostSimd.cmake` | `CMakeLists.txt:115` | wires SIMDe for the generated corpus |
| `res/icon/dq8.icns` | `src/runtime/CMakeLists.txt:137` | macOS app icon only |
| `tools/runtime` | `CMakeLists.txt:192` (`add_subdirectory ... EXCLUDE_FROM_ALL`) | harness |
| `config/` | DQ8 regeneration flow | recompiler TOMLs; we have `data/config.toml` from ToL |
| `setup.py` | DQ8 regeneration flow | drives analyzer + recompiler |

Also note: the skeleton sets `PS2X_VU_PROGRAM_PROFILES`, which the **pinned** PS2Recomp does not define (it is a newer
feature), so those blocks are inert here.

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

## 6. Open decisions (asked, not yet answered)

1. Link the **existing** `data/output` corpus first, or regenerate with the pinned recompiler?
2. Keep the corpus in `data/output` (point `DQ8_GENERATED_DIR` at it) or move it under `build/generated/...`?
3. Supply the missing skeleton files from the DQ8 repo, or replace them with minimal local versions?
4. Rename the `dq8` target / `DQ8_*` variables to `tol`/`TOL_*`, or keep the DQ8 names?
5. Toolchain on this machine: MSVC (Visual Studio 18 is installed) or clang/Ninja?

## 7. Working rules for this tree

- `thirdparty/*` are submodules: never edit them, fix locally in `src/`.
- Generated `.cpp` are build inputs, not source: they live under `data/` (gitignored) or `build/`.
- `data/*` is gitignored (game files); only `data/README.txt` is tracked.
- Keep the corpus flags (`-fno-strict-aliasing -fwrapv`, `-msse4.1`) — they preserve guest semantics.
