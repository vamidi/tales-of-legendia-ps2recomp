# PS2Recomp Stripped Game Walkthrough

This is a practical guide for games that have no useful symbols and mostly look like:

* `sub_0010B1A0`
* `sub_0031D5F8`
* `entry_00123456`

It focuses on:

* getting first boot
* moving from boot to menu
* debugging "stuck" states
* using address bindings and overrides safely

---

## 1. What Matters Most

Runtime dispatch is address based.

The important identity is:

* `0xADDR -> function pointer`

Not:

* `sub_xxx` text name

So:

* Renaming generated C++ functions does not fix execution by itself.
* If `0x00123456` should behave like `sceCdRead`, bind that address to the handler.
* Address mappings are build specific. Do not assume they are portable across region/revision ELFs.

---

## 2. Build Once First

From repo root:

```powershell
cmake -S . -B out/build
cmake --build out/build --config Debug --target ps2_analyzer ps2_recomp ps2EntryRunner
```

Typical binaries:

* `out/build/ps2xAnalyzer/Debug/ps2_analyzer.exe`
* `out/build/ps2xRecomp/Debug/ps2_recomp.exe`
* `out/build/ps2xRuntime/Debug/ps2EntryRunner.exe`

---

## 3. One Full Recompile Cycle

### Step A: Analyze ELF

```powershell
.\out\build\ps2xAnalyzer\Debug\ps2_analyzer.exe game.elf game.toml
```

### Step B: Improve function boundaries for stripped games (recommended)

If possible, export a Ghidra function map CSV and set:

```toml
[general]
ghidra_output = "path/to/ghidra_functions.csv"
```

This reduces bad function splits and missing starts.

### Step C: Recompile

```powershell
.\out\build\ps2xRecomp\Debug\ps2_recomp.exe game.toml
```

### Step D: Move generated output into runtime runner

Generated output must end up in:

* `ps2xRuntime/src/runner/` for `.cpp`
* `ps2xRuntime/include/` for generated headers

Important:

* Replace the placeholder `ps2xRuntime/src/runner/register_functions.cpp` with the generated one.
* If `registerAllFunctions(...)` stays empty, runtime cannot dispatch anything useful.

### Step E: Build and run

```powershell
cmake --build out/build --config Debug --target ps2EntryRunner
.\out\build\ps2xRuntime\Debug\ps2EntryRunner.exe game.elf
```

---

## 4. Config Strategy for No-Symbol Games

Use `general.stubs` with address bindings:

```toml
[general]
stubs = [
  "sceCdRead@0x00123456",
  "SifLoadModule@0x00127890"
]
```

Temporary triage bindings:

```toml
[general]
stubs = [
  "ret0@0x001D9410",
  "ret1@0x001D5BC8",
  "reta0@0x0024B7C0"
]
```

Meaning:

* `ret0`: returns `0`
* `ret1`: returns `1`
* `reta0`: returns argument `$a0`

Use them to test control flow quickly, then replace with real behavior.

---

## 5. First Boot Debug Loop

Run, read logs, fix the first hard blocker, repeat.

### Blocker A: Function not found

Log pattern:

* `Warning: Function at address 0x... not found`

Actions:

1. Confirm generated `register_functions.cpp` is compiled.
2. Confirm `registerAllFunctions(runtime)` is called before `loadELF`.
3. Search generated code for unresolved raw dispatch:

```powershell
rg -n "lookupFunction\\(0x" ps2xRuntime/src/runner
```

4. For missing targets, add address mapping in TOML or via override.
5. If many targets are missing, improve boundaries (Ghidra map).

### Blocker B: Unimplemented stub

Log pattern:

* `Warning: Unimplemented PS2 stub called. name=...`

Actions:

1. If address is known, bind `handler@0xADDR` in TOML.
2. If behavior unknown, test `ret0` / `ret1` / `reta0`.
3. If game progresses with triage return, implement a proper handler later.

### Blocker C: Unknown syscall

Log pattern:

* `[Syscall TODO] ...`

Actions:

1. Implement syscall semantics in `ps2xRuntime/src/lib/ps2_syscalls.cpp` and included syscall `.inl` files.
2. Return values matter. Many games only need expected success/failure contract first.

### Blocker D: PC not updating (spin/hang)

Log pattern:

* `CPU is doing some work at PC 0x... PC not updating.`

Actions:

1. Open generated function for that address.
2. Check loop exit conditions and called helpers.
3. Set breakpoints in:
* `PS2Runtime::lookupFunction`
* `ps2_stubs::TODO_NAMED`
* `ps2_syscalls::TODO`

---

## 6. How to Infer `sub_xxx` Behavior

It is possible often, but not from address alone.

Use a combined method:

### Static clues

* all callsites of that address
* argument setup (`$a0..$a3`)
* return checks on `$v0` (`==0`, `<0`, pointer use)
* immediate constants and nearby strings
* side effects (buffer writes, DMA, RPC payloads)

### Dynamic clues

* stub log gives `PC`, `RA`, args
* syscall TODO log gives encoded and fallback syscall IDs
* repeated call frequency at same PC tells you if it is a probe loop or hard dependency

### Quick classification trick

1. Try `ret0`.
2. Try `ret1`.
3. Try `reta0`.
4. Observe branch direction after return in caller.

If one value clearly unlocks progress, you learned the caller contract.

---

## 7. Using Game Overrides

Runtime has override registration keyed by ELF metadata:

* ELF basename
* optional entry point
* optional CRC32

Header:

* `ps2xRuntime/include/game_overrides.h`

### Where to put override file

Recommended for zero CMake edits:

* place in `ps2xRuntime/src/runner/` (runner uses `src/runner/*.cpp` glob)

Alternative:

* place in `ps2xRuntime/src/lib/` and add file to `ps2xRuntime/CMakeLists.txt`

### Minimal override module template

```cpp
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

namespace
{
    void applyMyGameOverrides(PS2Runtime &runtime)
    {
        // Safe direct custom wrapper that auto-returns if callee did not move PC.
        runtime.registerFunction(0x00123456u,
            [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
            {
                const uint32_t entryPc = ctx->pc;
                ps2_stubs::sceCdRead(rdram, ctx, rt);
                if (ctx->pc == entryPc)
                {
                    ctx->pc = getRegU32(ctx, 31);
                }
            });

        // Good for explicit triage handlers that already return.
        ps2_game_overrides::bindAddressHandler(runtime, 0x0031D5F8u, "ret0");
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "my-game-us",
    "SLUS_XXXX.XX",
    0x00100008u,
    0u,
    applyMyGameOverrides);
```

### Important safety note

`ps2_game_overrides::bindAddressHandler(...)` binds the raw handler directly.

Many stub/syscall handlers do not advance `ctx->pc` on their own.
If bound directly and `pc` is unchanged, you can get infinite redispatch to the same address.

For non-trivial handlers, prefer `runtime.registerFunction(...lambda...)` and add:

```cpp
if (ctx->pc == entryPc) { ctx->pc = getRegU32(ctx, 31); }
```

---

## 8. Address Binding: TOML vs Override

Use TOML `handler@0xADDR` when:

* you are already recompiling
* you want generated stub wrappers (with return-to-RA fallback behavior)
* you need simple, reproducible mapping in config

Use runtime overrides when:

* you want per-build routing with ELF metadata matching
* you need custom logic around handler calls
* you want to avoid editing the generated output directly

---

## 9. Practical Progress Plan (Boot -> Menu)

### Phase 1: Boot stability

Goal:

* no immediate crash
* entry function dispatch works

Do:

* verify function registration
* resolve first unknown function addresses
* resolve critical syscall TODOs

### Phase 2: I/O and module loading

Goal:

* game loads assets
* transitions past logo/intro

Do:

* prioritize `sceCd*`, `fio*`, `Sif*`, module loading paths
* watch for unresolved CD/file requests in logs

### Phase 3: menu reach

Goal:

* frame loop reaches menu logic

Do:

* remove temporary returns from hot loops
* replace with stable semantics
* keep overrides targeted to this title/build

---

## 10. Common Mistakes

* Forgetting to replace placeholder `register_functions.cpp`.
* Assuming symbol names matter more than addresses.
* Copying address map from a different ELF build.
* Binding raw handlers in overrides without PC fallback.
* Leaving too many `ret0/ret1` stubs in final build and then debugging random late failures.

---

## 11. Useful Commands

Find unresolved static address calls in generated code:

```powershell
rg -n "lookupFunction\\(0x" ps2xRuntime/src/runner
```

Find everything about a specific address:

```powershell
rg -n "0x00123456|_0x123456|sub_00123456|entry_00123456" ps2xRuntime/src/runner
```

Find where a runtime handler exists:

```powershell
rg -n "sceCdRead|SifLoadModule|ret0|ret1|reta0" ps2xRuntime/include/ps2_call_list.h ps2xRuntime/src/lib
```

---

## 12. Final Checklist Before Asking "Why Is It Stuck?"

1. `registerAllFunctions(runtime)` is non-empty and built.
2. Entry point function is registered.
3. First unresolved function addresses are mapped or implemented.
4. First unknown syscalls are handled.
5. No obvious same-PC infinite loop from direct raw handler binding.
6. Temporary return stubs are limited and documented.

If all 6 are true, you are in real behavior emulation territory, not plumbing failure.

