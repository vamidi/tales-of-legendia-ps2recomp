# 18 — Game Override Hooks

> **Source:** PS2Recomp wiki `Game-Override-Hooks.md`, verified against `ps2xRuntime/include/game_overrides.h`.
> **Use when:** you need per-build address routing, custom wrapper logic, or a fix that must not affect other games.

---

## Definition

A **game override** is C++ that runs during `loadELF` and can **replace function bindings by address for one specific game build**.

It is separate from:

- recompilation output (`runner/*.cpp`)
- global runtime stubs/syscalls (`ps2_call_list.h`, `Kernel/Stubs/`, `Kernel/Syscalls/`)

---

## Why this exists (especially for stripped `sub_xxx` games)

| Benefit | Detail |
|---------|--------|
| **Scoped hacks** | Fix one ELF build without changing behaviour for every game |
| **One place** | All per-game address bindings live here, not scattered in generated code |
| **Custom wrappers** | Add logic around handlers when plain `handler@0xADDR` is not enough |
| **Survives recompile** | No hand-editing generated runner files every time you re-run `ps2_recomp` |

---

## How it works

1. Runner registers generated functions normally via `registerAllFunctions(runtime)`.
2. `loadELF` calls the override matcher.
3. Matching descriptors are selected by **ELF basename** (case-insensitive), optional **entry point**, and optional **ELF CRC32**.
4. The matched override callback runs and can either:

   ```cpp
   runtime.registerFunction(address, fn);                                    // full custom replacement
   ps2_game_overrides::bindAddressHandler(runtime, address, "handler");      // bind to existing handler
   ```

5. The runtime function table now contains game-specific replacements **for that build only**.

### Matcher semantics

| Field | `0` means |
|-------|-----------|
| `entry` | Entry-point matching **disabled** |
| `crc32` | CRC32 matching **disabled** |

With all three omitted/zero, the override applies to **any** ELF with that basename.

---

## API

```cpp
// ps2xRuntime/include/game_overrides.h
namespace ps2_game_overrides
{
    using ApplyFn = void (*)(PS2Runtime &runtime);

    struct Descriptor
    {
        const char *name    = "";      // diagnostic label
        const char *elfName = "";      // ELF basename, case-insensitive
        uint32_t    entry   = 0;       // 0 = don't match on entry
        uint32_t    crc32   = 0;       // 0 = don't match on CRC32
        ApplyFn     apply   = nullptr;
    };

    void registerDescriptor(const Descriptor &descriptor);
    void applyMatching(PS2Runtime &runtime, const std::string &elfPath,
                       uint32_t entry, uint32_t crc32, bool crc32Valid);

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address,
                            std::string_view handlerName);
}

#define PS2_REGISTER_GAME_OVERRIDE(NAME_LITERAL, ELF_NAME_LITERAL, ENTRY_U32, CRC32_U32, APPLY_FN)
```

`PS2_REGISTER_GAME_OVERRIDE` creates a file-scope `Descriptor` plus an `AutoRegister` static, so the override self-registers at static-init time. **No CMake edit needed** if the file is compiled.

---

## Where to put the override file

| Option | Trade-off |
|--------|-----------|
| `ps2xRuntime/src/runner/` | **Recommended** — runner globs `src/runner/*.cpp`, so zero CMake edits. ⚠️ But `runner/` is normally machine-generated output — keep your override file clearly named (e.g. `game_override_<title>.cpp`) and never let a recompile overwrite it. |
| `ps2xRuntime/src/lib/` | Cleaner separation, but requires adding the file to `ps2xRuntime/CMakeLists.txt`. |

---

## Example

```cpp
#include "game_overrides.h"
#include "ps2_runtime.h"

namespace
{
    void applyObscureOverrides(PS2Runtime &runtime)
    {
        // Route stripped addresses to existing handlers.
        ps2_game_overrides::bindAddressHandler(runtime, 0x00123456u, "sceCdRead");
        ps2_game_overrides::bindAddressHandler(runtime, 0x00127890u, "SifLoadModule");

        // Temporary triage routing.
        ps2_game_overrides::bindAddressHandler(runtime, 0x0031D5F8u, "ret0");

        // Full custom replacement by address.
        runtime.registerFunction(0x0031DD28u,
            [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
            {
                setReturnS32(ctx, 0);
                ctx->pc = getRegU32(ctx, 31);
            });
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "obscure-us",
    "SLES_527.37",
    0x00100008u, // entry (0 disables entry matching)
    0u,          // CRC32 (0 disables CRC matching)
    applyObscureOverrides);
```

---

## ⚠️ Safety: the PC-advance trap

`ps2_game_overrides::bindAddressHandler(...)` binds the **raw handler directly**.

Many stub/syscall handlers **do not advance `ctx->pc`** on their own. Bound directly, with `pc` unchanged, you get **infinite redispatch to the same address** — the classic "hangs at one PC" symptom.

For non-trivial handlers, prefer `runtime.registerFunction(...lambda...)` with an explicit fallback:

```cpp
runtime.registerFunction(0x00123456u,
    [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
    {
        const uint32_t entryPc = ctx->pc;
        ps2_stubs::sceCdRead(rdram, ctx, rt);
        if (ctx->pc == entryPc)          // handler didn't advance PC
        {
            ctx->pc = getRegU32(ctx, 31); // return to $ra
        }
    });
```

`bindAddressHandler` is fine for handlers that **already return** (`ret0`, `ret1`, `reta0`) — those are the intended case.

---

## TOML vs override — when to use which

| Need | Use |
|------|-----|
| Already recompiling; want generated stub wrappers with return-to-`$ra` fallback | **TOML** `handler@0xADDR` |
| Simple, reproducible mapping living in config | **TOML** |
| Per-build routing with ELF metadata (basename / entry / CRC32) | **Override** |
| Custom logic around the handler call | **Override** |
| Avoid editing generated output | **Override** |

---

## Common mistakes

- Binding a raw handler in an override **without** a PC fallback → same-PC infinite loop.
- Copying an address map from a **different ELF build** (region/revision) — addresses are build-specific.
- Putting override logic in `runner/*.cpp` generated files → lost on the next recompile.
- Leaving triage `ret0`/`ret1` bindings in a "final" build, then debugging random late failures.

---

## Cross-references

- Working example → `examples/game-override-template.cpp`
- Triage with `ret0`/`ret1`/`reta0` → `19-stripped-game-walkthrough.md` §4
- Runtime symptom table → `17-ps2x-runtime.md`
