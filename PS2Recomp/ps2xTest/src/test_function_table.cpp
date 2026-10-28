#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>

// For Unit tests link ps2_runtime without the generated runner source.
// g_ps2RecompiledEeAbiVersion is now provided by ps2_runtime.cpp itself.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = PS2_RAM_SIZE;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = (g_ps2RecompiledFunctionTableEnd - g_ps2RecompiledFunctionTableBase) >> 2;

PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[g_ps2RecompiledFunctionTableSlotCount] = {};

// Stub for a runner-generated function referenced by the Tales of Legendia
// resume-label override in game_overrides.cpp. Unit tests never load that ELF,
// so the symbol only has to exist for the link; if it were ever dispatched it
// returns to $ra, like the leaf it stands in for.
void entry_1ad5a0_0x1ad728(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ctx->pc = GPR_U32(ctx, 31);
}

// Same story for 0x232460's mid-function resume label 0x2324BC, also
// registered by game_overrides.cpp.
void entry_232460_0x2324d0(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ctx->pc = GPR_U32(ctx, 31);
}

void reset_ps2_test_function_table()
{
    std::fill(g_ps2RecompiledFunctionTable,
              g_ps2RecompiledFunctionTable + g_ps2RecompiledFunctionTableSlotCount,
              nullptr);
}
