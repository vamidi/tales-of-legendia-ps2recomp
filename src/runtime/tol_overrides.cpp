#include "tol_overrides.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    bool traceSifEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_SIF") != nullptr;
        return enabled;
    }

    // SIF register block as used by this title's own driver (all physical addresses; the guest reaches them
    // uncached through 0xB000xxxx, which the runtime translates).
    constexpr uint32_t kSifMscom = 0x10001000u; // EE -> IOP command / status
    constexpr uint32_t kSifSmcom = 0x10001010u; // IOP -> EE command / status
    constexpr uint32_t kIopPendingBit = 0x800u;

    // Native implementation of the guest's SIF command issuer - FUN_0011F400 at 0x0011F400.
    //
    // Contract, pinned from all three call sites (0x11F338, 0x11F854, 0x11FC74):
    //   - the return value is never consumed (two callers set v0 themselves right after the call);
    //   - the effect is the register write; completion is observed by the caller polling 0x10001010 for bit
    //     0x800, clearing it and bumping a retry counter when it is set, against a 0x7333 timeout.
    //
    // On hardware the IOP clears that bit once it has taken the command. This runtime has no IOP behind the
    // register file, so the bit stays exactly as the guest wrote it (0x0F82 / 0x0B82 both carry 0x800), the
    // caller sees "still pending" on every pass and retries forever - which is the boot stall: no file I/O, no
    // draws. The native equivalent of an IOP that accepts immediately is to complete the command here.
    [[maybe_unused]] void tolSifIssueCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t command = GPR_U32(ctx, 4);

        const uint32_t mscom = runtime->Load32(rdram, ctx, kSifMscom);
        const uint32_t smcom = runtime->Load32(rdram, ctx, kSifSmcom);
        runtime->Store32(rdram, ctx, kSifMscom, mscom & ~kIopPendingBit);
        runtime->Store32(rdram, ctx, kSifSmcom, smcom & ~kIopPendingBit);

        if (traceSifEnabled())
        {
            static uint32_t sIssued = 0u;
            if (sIssued < 64u)
            {
                ++sIssued;
                std::fprintf(stderr,
                             "[sif-native] command=0x%08x mscom 0x%08x->done smcom 0x%08x->done\n",
                             command, mscom, smcom);
            }
        }

        SET_GPR_U32(ctx, 2, 0u);
    }
}

namespace tol
{
    void installOverrides(PS2Runtime &runtime)
    {
        // 0x0011F400 was overridden here as a "SIF command issuer". That reading was wrong: the registers it
        // touches (0x10001000 / 0x10001010 / 0x10001020) are EE Timer 2's COUNT / MODE / TARGET, per the
        // hardware map (PCSX2 Hw.h: RCNT2_COUNT 0x10001000, RCNT2_MODE 0x10001010, RCNT2_TARGET 0x10001020),
        // not SIF MSCOM/SMCOM/MSFLG. The routine is a timer-driven poll, and the stub below wrote timer
        // registers with made-up values, corrupting the guest's sense of time.
        //
        // It is disabled rather than deleted: the same module is where real overrides belong once they are
        // understood, and this note is the reason the previous attempt must not be reinstated as-is.
        (void)runtime;
        std::fprintf(stderr, "[tol] no native overrides installed (the Timer-2 override was withdrawn)\n");
    }
}
