### Game Override Hooks

Game overrides are runtime-side, build-scoped patch modules.

Definition:

* A game override is C++ code that runs during `loadELF` and can replace function bindings by address for one specific game build.
* This is separate from recompilation output and separate from global runtime stubs/syscalls.

Why this helps (especially stripped `sub_xxx` games):

* You can scope hacks/fixes to one ELF build instead of changing behavior for all games.
* You can keep per-game address bindings in one place, not scattered in generated code.
* You can add custom wrapper logic around handlers when plain `handler@0xADDR` is not enough.
* You do not need to hand-edit generated runner files every recompile.

How it works:

1. Runner registers generated functions normally via `registerAllFunctions(runtime)`.
2. `loadELF` calls override matcher.
3. Matching descriptors are selected by ELF basename (case-insensitive), optional entry point, and optional ELF CRC32.
4. Matched override callback runs and can call `runtime.registerFunction(address, fn)` for full custom replacement, or `ps2_game_overrides::bindAddressHandler(runtime, address, "handler")` for direct binding to an existing runtime syscall/stub handler.
5. The runtime function table now contains game-specific replacements for that build only.

API:

* Header: `ps2xRuntime/include/game_overrides.h`
* Register macro: `PS2_REGISTER_GAME_OVERRIDE(name, elfName, entry, crc32, applyFn)`
* Direct bind helper: `ps2_game_overrides::bindAddressHandler(runtime, addr, "handler")`

#### Game Override Example

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

        // You can also register fully custom replacements by address.
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