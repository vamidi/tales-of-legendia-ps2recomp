# 17 — ps2xRuntime (Execution Environment)

> **Source:** PS2Recomp wiki `Ps2xRuntime.md`, corrected against this repo's actual layout.
> **Use when:** the game boots but misbehaves, a log line needs interpreting, or you need to know which file to edit for a syscall/stub/MMIO fix.

---

## What the runtime is

`ps2xRuntime` **executes the recompiled C++** produced by `ps2_recomp`. It provides a minimal PS2 "execution environment": memory, CPU context, MMIO routing, function dispatch, and syscall/stub plumbing.

> **Status: work in progress.** The runtime is historically the most underdeveloped part of the project.
> Expect to implement/fix syscalls and stubs per game.

---

## Repository layout (verified)

Paths are relative to `PS2Recomp/`:

```
ps2xRuntime/
├── include/
│   ├── ps2_runtime.h            # PS2Runtime + R5900Context (struct at line ~61)
│   ├── ps2_runtime_macros.h     # ADD32 / FAST_READ32 / READ32 ... translation helpers
│   ├── ps2_recompiled_functions.h
│   ├── ps2_call_list.h          # handler name registry
│   ├── ps2_stubs.h              # ps2_stubs:: namespace
│   ├── ps2_syscalls.h
│   ├── game_overrides.h         # override API
│   └── games_database.h
└── src/
    ├── runner/                  # ← generated game code goes HERE
    │   ├── register_functions.cpp   # ← replace the placeholder with the generated one
    │   └── <game>_<func>_0x<addr>.cpp
    └── lib/
        ├── ps2_runtime.cpp      # core runtime
        ├── ps2_memory.cpp       # memory model, MMIO
        ├── game_overrides.cpp
        ├── Kernel/
        │   ├── Syscalls/        # Dispatcher, FileIO, Thread, Sync, RPC, ...
        │   └── Stubs/           # CD, FileIO, GS, SIF, Pad, Audio, LibC, ...
        ├── gs/                  # Graphics Synthesizer
        └── vu/                  # VU0/VU1
```

> ⚠️ **Wiki drift note:** older docs point at `src/lib/ps2_syscalls.cpp` and `src/lib/stubs/...`. Those paths no longer exist. Syscalls live in `src/lib/Kernel/Syscalls/`, stubs in `src/lib/Kernel/Stubs/`. Always `Test-Path` before editing — see `SKILL.md` §3, rule 5.

---

## Getting generated code in

1. Recompiled `.cpp` output → `ps2xRuntime/src/runner/`
2. Generated/required headers → `ps2xRuntime/include/`
3. **Replace** the placeholder `src/runner/register_functions.cpp` with the generated one.

If `registerAllFunctions(...)` stays empty, the runtime cannot dispatch anything useful and every call becomes `Function at address 0x... not found`.

---

## What the runtime provides

### Memory model

- **32 MB main RAM (RDRAM)**
- **16 KB scratchpad**
- Basic regions for EE I/O, BIOS window, GS registers, VU memory windows

### CPU context — `R5900Context`

Declared in `include/ps2_runtime.h`. Key shape (see `20-r5900-context-layout.md` for the full verified layout):

- 128-bit GPRs (`__m128i r[32]`)
- `pc`, `insn_count`, `hi/lo`, `hi1/lo1`, `sa`
- COP0 registers
- COP1 (FPU) registers + `fcr31` + `f_acc`
- VU0 macro-mode state (VF/VI/Q/P/I/R/ACC, status, MAC/clip flags, TPC, FBRST, …)
- LL/SC reservation (`llbit`, `lladdr`) and delay-slot tracking (`in_delay_slot`, `branch_pc`)
- `dump()` for a formatted register dump

### Function table

Maps guest addresses → generated C++ function pointers. This is effectively **"dynamic linking" for translated code**. Identity is the **address**, not the `sub_xxx` name.

### Syscall / stub entry points

- Central syscall handler (`handleSyscall`)
- Stub hooks used by the `stubs` / `skip` generated wrappers

### Hybrid fast/slow memory access

- **Fast path:** direct masked RDRAM reads/writes
- **Slow path:** routes "special addresses" through runtime `Load*` / `Store*` to handle MMIO, scratchpad, BIOS, etc.

---

## Recommended iteration loop

1. Run with a minimal config and **no aggressive skipping**.
2. Fix **hard blockers first** — `function not found`, syscall TODO, critical I/O stubs.
3. Use **temporary return stubs only to classify call importance**.
4. **Promote** temporary fixes to real implementations.
5. Move per-game hacks into **game overrides** keyed by ELF metadata.
6. **Re-test from cold boot** after each batch.

---

## Getting unstuck: symptom → action

### `Warning: Function at address 0x... not found`

- Ensure the generated `register_functions.cpp` is actually **compiled into** the runner.
- Ensure `registerAllFunctions(runtime)` is called **before** `loadELF`.
- If the address is valid but missing, add an address binding or an override registration for it.

### `Warning: Unimplemented PS2 stub called. name=...`

- Add/adjust a `general.stubs` mapping for the stripped address.
- Or implement that stub in `ps2xRuntime/src/lib/Kernel/Stubs/<Area>.cpp`.

### `[Syscall TODO] ...`

- Implement the numeric syscall path under `ps2xRuntime/src/lib/Kernel/Syscalls/`.
- Update the mapping/list in `ps2xRuntime/include/ps2_call_list.h`.

### `CPU is doing some work at PC 0x... PC not updating.`

- You are in a **loop / spin / wait** path.
- Open the generated function for that PC address (`<game>_<func>_0x<addr>.cpp`) and inspect the loop condition and the helpers it calls.
- Set breakpoints in:
  - `PS2Runtime::lookupFunction`
  - `ps2_stubs::TODO_NAMED`
  - `ps2_syscalls::TODO`

---

## Syscalls and stubs

The runtime is **intentionally minimal**. Many games will require additional work here.

### Files you will edit

| File | Role |
|------|------|
| `ps2xRuntime/include/ps2_call_list.h` | Handler name registry / name→ID mapping |
| `ps2xRuntime/src/lib/Kernel/Syscalls/*.cpp` | Syscall implementations (grouped by area) |
| `ps2xRuntime/src/lib/Kernel/Stubs/*.cpp` | Stub implementations (grouped by library area) |

### Typical workflow

1. Run the game and observe the failure (missing syscall ID, TODO stub hit, unhandled path).
2. Identify which syscall/stub the game is hitting.
3. Add or fix the implementation in the matching file.
4. Update the mapping/list in `ps2_call_list.h`.
5. Rebuild and repeat.

### How TOML `stubs` / `skip` relate to the runtime

| TOML directive | Generated code does | Runtime must provide |
|----------------|--------------------|----------------------|
| `stubs` | Calls a runtime stub handler **by name** | That handler must exist in `ps2_call_list.h` |
| `skip` | Calls `ps2_stubs::TODO_NAMED("...")` | Safe default: return a sane value, emulate minimally, or log+abort |

Both cases require the runtime to supply **safe behaviour** — a sane default return, minimal emulation, or a deliberate log+abort where continuing would be worse.

---

## Runtime macros (translation support)

`ps2_runtime_macros.h` provides:

- arithmetic helpers (`ADD32`, `SUB32_OV`, …)
- PS2 MMI/VU-friendly helpers via SSE intrinsics
- fast read/write macros (`FAST_READ32`, …)
- slow-path fallback macros (`READ32`, `WRITE32`, …) that route MMIO/special addresses through `PS2Runtime`

If you see **weird game behaviour**, pay particular attention to:

- address classification in `PS2Runtime::isSpecialAddress`
- correctness of the `Load*` / `Store*` implementations for the region the game touches

---

## Platform / SIMD notes

The runtime uses SIMD types and intrinsics heavily (`__m128i`, SSE).

| Target | Requirement |
|--------|-------------|
| **MSVC** | `<intrin.h>` |
| **Clang/GCC x86/x64** | `<immintrin.h>` / `<smmintrin.h>` |
| **Non-x86 (ARM)** | Build with `USE_SSE2NEON` and provide `sse2neon.h` |

If the target platform has no native SSE, enabling `USE_SSE2NEON` is **required**.

---

## Debugging tips

- Start with a **known, simple ELF** first (homebrew / debug build).
- Keep `stubs` and `skip` lists **small and intentional**.
- Watch for:
  - bad pointer translations (guest → host)
  - missing syscall/stub behaviour
  - games writing to MMIO regions (timers / DMAC / INTC / GS / VIF)
- If you hit **instruction translation** issues, fix them in the recompiler output stage first — the runtime assumes translated code is semantically valid.

---

## Current limitations

- Many syscalls and I/O behaviours are incomplete.
- Complex DMA / VIF / GS behaviour may be missing or simplified.
- Some games will require **per-title fixes** (stubs, patches, and MMIO handling).

---

## Cross-references

- TOML directives → `16-ps2x-recomp.md`
- Full `R5900Context` layout → `20-r5900-context-layout.md`
- Override API → `18-game-override-hooks.md`
- Boot/menu triage → `19-stripped-game-walkthrough.md`
- Runtime A/B debugging → `12-pcsx2-mcp-playbook.md`
- Syscall table → `db-syscalls.md`, `db-sdk-functions.md`
