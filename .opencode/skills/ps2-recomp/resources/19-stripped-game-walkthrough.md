# 19 — Stripped Game Walkthrough (Boot → Menu)

> **Source:** PS2Recomp wiki `PS2Recomp-Stripped-Game-Walkthrough-For-LLMs.md`.
> **Use when:** the game has no symbols and everything is `sub_0010B1A0`, `entry_00123456`. This is the practical end-to-end loop.

---

## 1. What matters most

**Runtime dispatch is address based.**

The identity that matters is:

```
0xADDR  →  function pointer
```

**Not** `sub_xxx` text names.

Consequences:

- Renaming generated C++ functions does **not** fix execution by itself.
- If `0x00123456` should behave like `sceCdRead`, **bind that address to the handler**.
- Address mappings are **build specific**. Never assume portability across region/revision ELFs.

---

## 2. Build once first

From the `PS2Recomp/` repo root:

```powershell
cmake -S . -B out/build
cmake --build out/build --config Debug --target ps2_analyzer ps2_recomp ps2EntryRunner
```

Typical binaries:

```
out/build/ps2xAnalyzer/Debug/ps2_analyzer.exe
out/build/ps2xRecomp/Debug/ps2_recomp.exe
out/build/ps2xRuntime/Debug/ps2EntryRunner.exe
```

> **Build gate still applies.** No `--clean-first`, no `--target clean`, and `cmake` must run inside `vcvars64`. See `SKILL.md` §4. Full rebuild = 30+ hours.

---

## 3. One full recompile cycle

### Step A — Analyze the ELF

```powershell
.\out\build\ps2xAnalyzer\Debug\ps2_analyzer.exe game.elf game.toml
```

### Step B — Improve function boundaries (recommended for stripped games)

Export a Ghidra function map CSV and set:

```toml
[general]
ghidra_output = "path/to/ghidra_functions.csv"
```

This reduces bad function splits and missing starts. **Do this before Step C** — fixing boundaries later means redoing triage.

### Step C — Recompile

```powershell
.\out\build\ps2xRecomp\Debug\ps2_recomp.exe game.toml
```

### Step D — Move generated output into the runtime runner

Generated output must end up in:

- `ps2xRuntime/src/runner/` for `.cpp`
- `ps2xRuntime/include/` for generated headers

**Critical:**

- **Replace** the placeholder `ps2xRuntime/src/runner/register_functions.cpp` with the generated one.
- If `registerAllFunctions(...)` stays empty, the runtime cannot dispatch anything useful.

### Step E — Build and run

```powershell
cmake --build out/build --config Debug --target ps2EntryRunner
.\out\build\ps2xRuntime\Debug\ps2EntryRunner.exe game.elf
```

---

## 4. Config strategy for no-symbol games

Use `general.stubs` with **address bindings**:

```toml
[general]
stubs = [
  "sceCdRead@0x00123456",
  "SifLoadModule@0x00127890"
]
```

### Temporary triage bindings

```toml
[general]
stubs = [
  "ret0@0x001D9410",
  "ret1@0x001D5BC8",
  "reta0@0x0024B7C0"
]
```

| Handler | Returns |
|---------|---------|
| `ret0` | `0` |
| `ret1` | `1` |
| `reta0` | argument `$a0` |

Use them to test control flow quickly, then **replace with real behaviour**. Never ship a build full of triage returns — it converts a crisp blocker into a diffuse late-game failure.

---

## 5. First-boot debug loop

Run, read the logs, fix **the first hard blocker**, repeat. One at a time.

### Blocker A — Function not found

Log pattern:

```
Warning: Function at address 0x... not found
```

Actions:

1. Confirm generated `register_functions.cpp` is compiled.
2. Confirm `registerAllFunctions(runtime)` is called before `loadELF`.
3. Search generated code for unresolved raw dispatch:

   ```powershell
   rg -n "lookupFunction\(0x" ps2xRuntime/src/runner
   ```

4. For missing targets, add an address mapping in TOML or via an override.
5. If **many** targets are missing, the boundaries are wrong → improve them (Ghidra map), don't whack-a-mole.

### Blocker B — Unimplemented stub

Log pattern:

```
Warning: Unimplemented PS2 stub called. name=...
```

Actions:

1. If the address is known, bind `handler@0xADDR` in TOML.
2. If behaviour is unknown, test `ret0` / `ret1` / `reta0`.
3. If the game progresses with a triage return, implement a proper handler later.

### Blocker C — Unknown syscall

Log pattern:

```
[Syscall TODO] ...
```

Actions:

1. Implement syscall semantics in `ps2xRuntime/src/lib/Kernel/Syscalls/` (and update `include/ps2_call_list.h`).
2. **Return values matter.** Many games only need the expected success/failure contract first.

### Blocker D — PC not updating (spin/hang)

Log pattern:

```
CPU is doing some work at PC 0x... PC not updating.
```

Actions:

1. Open the generated function for that address.
2. Check loop exit conditions and the helpers it calls.
3. Set breakpoints in:
   - `PS2Runtime::lookupFunction`
   - `ps2_stubs::TODO_NAMED`
   - `ps2_syscalls::TODO`

> Also suspect a **raw handler bound without PC fallback** — see `18-game-override-hooks.md` §Safety.

---

## 6. How to infer `sub_xxx` behaviour

Often possible — but **not from the address alone**.

### Static clues

- all callsites of that address
- argument setup (`$a0..$a3`)
- return checks on `$v0` (`== 0`, `< 0`, pointer use)
- immediate constants and nearby strings
- side effects (buffer writes, DMA, RPC payloads)

### Dynamic clues

- stub log gives `PC`, `RA`, args
- syscall TODO log gives encoded and fallback syscall IDs
- repeated call frequency at the same PC tells you whether it is a probe loop or a hard dependency

### Quick classification trick

1. Try `ret0`.
2. Try `ret1`.
3. Try `reta0`.
4. Observe the **branch direction after return** in the caller.

If one value clearly unlocks progress, you have learned the caller's contract.

Full inference playbook: `06-game-porting-playbook.md`.

---

## 7. Using game overrides

Runtime has override registration keyed by ELF metadata:

- ELF basename
- optional entry point
- optional CRC32

Header: `ps2xRuntime/include/game_overrides.h`

### Where to put the override file

Recommended for zero CMake edits:

- place in `ps2xRuntime/src/runner/` (runner uses a `src/runner/*.cpp` glob)

Alternative:

- place in `ps2xRuntime/src/lib/` and add the file to `ps2xRuntime/CMakeLists.txt`

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

`ps2_game_overrides::bindAddressHandler(...)` binds the **raw handler** directly.

Many stub/syscall handlers **do not advance `ctx->pc`** on their own. If bound directly and `pc` is unchanged, you get **infinite redispatch to the same address**.

For non-trivial handlers, prefer `runtime.registerFunction(...lambda...)` plus:

```cpp
if (ctx->pc == entryPc) { ctx->pc = getRegU32(ctx, 31); }
```

---

## 8. Address binding: TOML vs override

| Use TOML `handler@0xADDR` when… | Use runtime overrides when… |
|--------------------------------|------------------------------|
| you are already recompiling | you want per-build routing with ELF metadata matching |
| you want generated stub wrappers (with return-to-`$ra` fallback) | you need custom logic around handler calls |
| you need simple, reproducible mapping in config | you want to avoid editing generated output directly |

---

## 9. Practical progress plan (Boot → Menu)

### Phase 1 — Boot stability

**Goal:** no immediate crash; entry function dispatch works.

- verify function registration
- resolve first unknown function addresses
- resolve critical syscall TODOs

### Phase 2 — I/O and module loading

**Goal:** game loads assets; transitions past logo/intro.

- prioritise `sceCd*`, `fio*`, `Sif*`, module loading paths
- watch for unresolved CD/file requests in logs

### Phase 3 — Menu reach

**Goal:** frame loop reaches menu logic.

- remove temporary returns from hot loops
- replace with stable semantics
- keep overrides targeted to this title/build

---

## 10. Common mistakes

- Forgetting to replace the placeholder `register_functions.cpp`.
- Assuming **symbol names** matter more than **addresses**.
- Copying an address map from a different ELF build.
- Binding raw handlers in overrides without a PC fallback.
- Leaving too many `ret0`/`ret1` stubs in the final build, then debugging random late failures.

---

## 11. Useful commands

Find unresolved static address calls in generated code:

```powershell
rg -n "lookupFunction\(0x" ps2xRuntime/src/runner
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

## 12. Final checklist before asking "why is it stuck?"

1. `registerAllFunctions(runtime)` is non-empty and built.
2. Entry point function is registered.
3. First unresolved function addresses are mapped or implemented.
4. First unknown syscalls are handled.
5. No obvious same-PC infinite loop from direct raw handler binding.
6. Temporary return stubs are limited and documented.

If all 6 are true, you are in **real behaviour emulation** territory — not plumbing failure. Switch to `13-decisional-brain.md` and `12-pcsx2-mcp-playbook.md` for A/B analysis.

---

## Cross-references

- Analyzer → `15-ps2x-analyzer.md`
- Recompiler / TOML → `16-ps2x-recomp.md`
- Runtime internals → `17-ps2x-runtime.md`
- Override details → `18-game-override-hooks.md`
- Operational phases (0-5) → `11-operational-phases.md`
- Guardrails → `10-agent-guardrails.md`
