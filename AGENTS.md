# Agent Guidelines — PS2Recomp (PS2 ELF Recompiler)

## Project Overview

This is a **PlayStation 2 emulator/recompiler** that recompiles PS2 ELF binaries for execution on x86_64 hosts. It targets the PS2's Emotion Engine (EE) R5900 CPU, VU0/VU1 vector units, Graphics Synthesizer (GS), and Input/Output Processor (IOP). The project is split into a **runtime** (emulation core + host backends) and a **recompiler/analyzer** that translates PS2 ELF binaries into recompiled function tables.

## Build System & Tooling

- **CMake 3.16+** with Conan package manager for dependencies
- **Conan**: `conan install` in each subproject directory (`PS2Recomp/`, `ps2xRuntime/`)
- **Compiler**: MSVC (Windows) or GCC/Clang (Linux). SSE2 is required; AVX2 is optional
- **Key Conan packages**: fmt, spdlog, nlohmann-json, stb, Dear ImGui, GLAD (OpenGL), GLFW3, ZLIB, libusb, xxHash, sse2neon (for non-SSE2 hosts)
- **Build commands** (Linux): `cmake -S . -B out/build && cmake --build out/build`
- **Build commands** (Windows/MSVC): `cmake -S . -B out/build -G "Visual Studio 17 2022"` then open in Visual Studio or use `cmake --build out/build --config Debug`

## Directory Structure

```
├── PS2Recomp/                    # Recompiler & analyzer toolchain
│   ├── include/ps2x/analyzer/    # ELF analysis, symbol resolution
│   ├── src/analyzer/             # Analyzer implementation
│   ├── include/ps2x/recompiler/  # JIT recompilation infrastructure
│   ├── src/recompiler/           # Recompiler implementation
│   └── tests/                    # Unit & integration tests
├── ps2xRuntime/                  # Emulation runtime (the emulator itself)
│   ├── include/                  # Public headers
│   │   ├── ps2_runtime.h         # Main PS2Runtime class — the entry point
│   │   └── runtime/              # Core subsystems
│   │       ├── ps2_memory.h      # PS2 memory model (RAM, IME, MMIO)
│   │       ├── ps2_address.h     # Address translation utilities
│   │       ├── ps2_vu1.h         # VU0/VU1 microprogram interpreter
│   │       ├── ps2_audio.h       # Audio backend interface
│   │       ├── ps2_pad.h         # Gamepad input backend
│   │       ├── ps2_rom_device.h  # CD-ROM device emulation
│   │       ├── ps2_vfs.h         # Virtual file system abstraction
│   │       └── gs/               # Graphics Synthesizer subsystem
│   ├── src/                      # Runtime implementation
│   │   ├── runner/               # ELF loading, function registration
│   │   └── ...                   # Subsystem implementations
│   └── tests/                    # Runtime unit tests
├── scripts/                      # Build/utility scripts (e.g., build.ps1)
└── CMakeLists.txt                # Root CMake (workspace-level)
```

## Core Architecture

### PS2Runtime Class (`ps2xRuntime/include/ps2_runtime.h`)

The central class. Key responsibilities:

- **CPU context**: Holds `R5900Context` — 32 GPRs as `__m128i`, COP0/COP1/COP2 control registers, VU0 state, HI/LO registers
- **Memory**: `PS2Memory` manages the PS2's memory map (RAM, IME, MMIO regions)
- **Subsystem accessors**: `cpu()`, `memory()`, `gs()`, `vu0()`, `vu1()`, `audioBackend()`, `padBackend()`, `romDevice()`, `vfs()`
- **Recompiled function dispatch**: `replaceFunction()`, `lookupFunction()`, `dispatchGuestBranch()`, `reportMissingFunction()` — the JIT runtime's function table is a flat array (`g_ps2RecompiledFunctionTable[]`) indexed by PS2 address slots
- **Memory operations**: `Load8/16/32/64/128` and `Store8/16/32/64/128` — these handle address translation, alignment checks, and special memory region behavior
- **IOP communication**: `loadIopModule()`, `stopIopModule()`, IOP memory read/write via `PS2IopHostAdapter` / `ps2x::iop::IopSubsystem`
- **EE Kernel**: Syscall handling (`handleSyscall`), TLB management, exception dispatch, heap management (`guestMalloc`, `guestCalloc`)
- **Scheduling**: `EeScheduler` with VSync timing, EE events, checkpointing

### Memory Model (`ps2xRuntime/include/runtime/ps2_memory.h`)

The PS2 memory map is divided into:
- **RAM** (32MB): Main game memory, guest heap starts at 0x00100000 by default
- **IME** (Interrupt Memory Area): ~4MB region for IOP communication
- **MMIO**: Hardware register regions for GS, VIF, GIFO, DMA channels, etc.
- Address translation uses `Ps2IsSpecialAddress()` to detect non-RAM regions

### Recompiler (`PS2Recomp/`)

The recompiler translates PS2 ELF binaries into native x86_64 code:

1. **Analyzer** (`ps2x/analyzer/`): Parses ELF files, resolves symbols, analyzes control flow
2. **Recompiler** (`ps2x/recompiler/`): Generates recompiled function stubs that dispatch to JIT-compiled code
3. **Function table**: Generated as `register_functions.cpp` — a flat array of `PS2Runtime::RecompiledFunction` pointers indexed by `(PC >> 4) & slot_mask`

### IOP Subsystem (`ps2xRuntime/include/ps2x/iop/`)

The Input/Output Processor is emulated as a separate subsystem:
- Communicates with the EE via DMA and shared memory
- Module loading/stopping via `PS2IopHostAdapter`
- RPC mechanism for IOP-to-EE communication (`RpcAbi`, `SifTransfer`)

### VU0/VU1 Interpreter (`ps2xRuntime/include/ps2_vu1.h`)

Vector Unit microprogram execution:
- VF (vector float), VI (vector integer), ACC, Q, P registers
- Microprogram loading via TPC (Tightly Coupled Program memory)
- Executed by `PS2Runtime::executeVU0Microprogram()` and `vu0StartMicroProgram()`

## Coding Conventions

### Naming

- **Classes**: PascalCase with PS2/ps2x prefix when in the runtime namespace (e.g., `PS2Runtime`, `PS2Memory`, `GifArbiter`)
- **Namespaces**: `ps2x` for top-level, `ps2x::iop` for IOP subsystem, `ps2x::recompiler` for recompiler code
- **Functions/Methods**: camelCase (e.g., `loadELF`, `dispatchGuestBranch`, `guestMalloc`)
- **Constants/Macros**: UPPER_CASE with PS2_ prefix (e.g., `PS2_RAM_SIZE`, `PS2_PATH_WATCH_ADDR`)
- **Private members**: m_ prefix (e.g., `m_memory`, `m_cpuContext`, `m_guestHeapBase`)
- **Types/Enums**: PascalCase (e.g., `GuestBranchKind`, `MissingFunctionPolicy`)

### Types

- Use fixed-width types from `<cstdint>`: `uint32_t`, `int64_t`, etc.
- PS2 addresses are always 32-bit unsigned (`uint32_t`)
- Sizes use `size_t` or `uint32_t` depending on context
- SIMD registers: `__m128i` for integer/SIMD, `__m128` for float/SIMD

### Error Handling

- Return `bool` for operations that can fail (`initialize()`, `loadELF()`)
- Use structured result types like `ps2x::iop::ModuleLoadResult`, `ps2x::iop::RpcResult`
- Exceptions are not the primary error mechanism; prefer return codes and status structs

### Logging

- Uses spdlog via `PS2_LOG_*` macros (see `ps2_log.h`)
- Log levels: DEBUG, INFO, WARNING, ERROR
- Format strings use fmt library syntax

### SIMD / Vectorization

- GPRs are stored as `__m128i` arrays for efficient SIMD operations
- Use intrinsics from `<immintrin.h>` (SSE4.1) or `<smmintrin.h>` (SSE4.1)
- On non-SSE2 platforms, sse2neon provides SSE intrinsics via NEON
- MSVC uses `<intrin.h>`

### Memory Alignment

- `alignas(16)` is used for structures that interact with SIMD (e.g., `R5900Context`)
- Guest heap allocations respect 16-byte alignment by default
- PS2 hardware often requires specific alignments — always check address requirements

### Threading & Synchronization

- Use `std::mutex` and `std::atomic` for shared state
- EE kernel state uses `mutable std::mutex m_eeKernelStateMutex`
- Guest heap operations are mutex-protected (`m_guestHeapMutex`)
- The main emulation loop runs on a single thread; IOP subsystem may have async callbacks

## Key Patterns

### Function Dispatch (JIT Runtime)

```cpp
// Recompiled functions are stored in a flat table indexed by address slot
RecompiledFunction func = runtime.lookupFunction(pc);
if (!func) {
    runtime.reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, name);
    return;
}
func(rdram, ctx, &runtime);
```

### Memory Access Pattern

```cpp
// All memory access goes through PS2Runtime methods for address translation
uint32_t value = runtime.Load32(rdram, ctx, guestAddress);
runtime.Store32(rdram, ctx, guestAddress, newValue);
```

### IOP Module Loading

```cpp
auto result = runtime.loadIopModule(modulePath);
if (result.moduleId < 0) { /* handle error */ }
```

### Exception Handling

```cpp
runtime.SignalException(ctx, EXCEPTION_SYSCALL);
// Or dispatch specific handlers:
runtime.handleSyscall(rdram, ctx);
runtime.handleBreak(rdram, ctx);
```

## Testing

- Tests use CMake's built-in testing (CTest)
- Runtime tests are in `ps2xRuntime/tests/`
- Recompiler tests are in `PS2Recomp/tests/`
- Run tests with: `ctest --test-dir build` or `cmake --build build --target test`

## Important Constants & Addresses

| Constant | Value | Description |
|----------|-------|-------------|
| `PS2_RAM_SIZE` | 0x02000000 (32MB) | Main RAM size |
| `PS2_RAM_MASK` | 0x01FFFFFF | RAM address mask |
| Guest heap default base | 0x00100000 | Where guest malloc starts |
| Async callback stack floor | 0x01F00000 | Bottom of async callback area |
| Path watch address | 0x01EFFFA0 | Memory region for path tracing |

## R5900 CPU Context Notes

- **GPRs**: Stored as `__m128i` — each register holds two 32-bit values. Use `_mm_extract_epi32(reg, lane)` to extract individual lanes
- **Return values**: `$v0` is register 2. Use `setReturnU32()`, `setReturnS32()`, `setReturnU64()` helpers
- **Delay slots**: Tracked via `in_delay_slot` and `branch_pc` in context
- **LL/SC**: Load-linked/store-conditionally use `llbit` and `lladdr` fields
- **COP0 Status reset**: `0x00010001` (EIE | IE) — matches libkernel's handoff state

## Agent Workflow Guidelines

1. **Before making changes**: Understand the subsystem you're modifying. Read the relevant header first to understand interfaces and invariants
2. **For recompiler changes**: Study how existing functions are registered and dispatched. The function table generation is in `runner/register_functions.cpp`
3. **For runtime changes**: Always go through `PS2Runtime` methods for memory access — never directly dereference guest pointers
4. **When adding new features**: Follow the naming conventions, use the existing logging infrastructure, and add tests
5. **When debugging**: Use the debug PC registers (`m_debugPc`, `m_debugRa`, etc.) and the context dump functionality (`ctx.dump()`)
6. **IOP subsystem changes**: Be careful with DMA boundaries and shared memory consistency between EE and IOP
