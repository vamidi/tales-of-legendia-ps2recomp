#include "tol_overrides.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // ---------------------------------------------------------------------------------------------------------
    // GENERIC PC PROBE (TOL_TRACE_PC=0xADDR[,0xADDR...])
    //
    // Wraps each listed guest function: prints pc/ra/sp on entry and on the return to the runtime. A generated
    // function returns when it hands control back (a jump, a preempting checkpoint, or jr $ra), so the exit line
    // shows where control actually went - which is how a lost return address becomes visible.
    constexpr int kMaxPcProbes = 16;
    uint32_t gPcProbeAddr[kMaxPcProbes] = {};
    PS2Runtime::RecompiledFunction gPcProbeOriginal[kMaxPcProbes] = {};
    int gPcProbeCount = 0;

    int pcProbeIndex(uint32_t pc)
    {
        for (int i = 0; i < gPcProbeCount; ++i)
        {
            if (gPcProbeAddr[i] == pc)
                return i;
        }
        return -1;
    }

    void pcProbeCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int idx = pcProbeIndex(ctx->pc);
        if (idx < 0 || gPcProbeOriginal[idx] == nullptr)
        {
            std::fprintf(stderr, "[pc] probe dispatch with unknown pc=0x%08X\n", ctx->pc);
            return;
        }
        std::fprintf(stderr, "[pc] enter 0x%08X pc=0x%08X ra=0x%08X sp=0x%08X\n", gPcProbeAddr[idx], ctx->pc,
                     GPR_U32(ctx, 31), GPR_U32(ctx, 29));
        {
            const uint32_t sp = GPR_U32(ctx, 29);
            if (sp != 0u && sp < 0x02000000u)
            {
                std::fprintf(stderr, "[pc]   frame");
                for (uint32_t off = 0u; off <= 0x38u; off += 8u)
                    std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, sp + off));
                std::fprintf(stderr, "\n");
            }
        }
        gPcProbeOriginal[idx](rdram, ctx, runtime);
        std::fprintf(stderr, "[pc] exit  0x%08X pc=0x%08X ra=0x%08X sp=0x%08X\n", gPcProbeAddr[idx], ctx->pc,
                     GPR_U32(ctx, 31), GPR_U32(ctx, 29));
    }

    void installPcProbes(PS2Runtime &runtime)
    {
        const char *spec = std::getenv("TOL_TRACE_PC");
        if (spec == nullptr || *spec == '\0')
            return;

        const char *cursor = spec;
        while (*cursor != '\0' && gPcProbeCount < kMaxPcProbes)
        {
            char *end = nullptr;
            const unsigned long value = std::strtoul(cursor, &end, 0);
            if (end == cursor)
                break;
            const uint32_t pc = static_cast<uint32_t>(value);
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(pc);
            if (original == nullptr)
            {
                std::fprintf(stderr, "[pc] 0x%08X is not in the function table\n", pc);
            }
            else
            {
                gPcProbeAddr[gPcProbeCount] = pc;
                gPcProbeOriginal[gPcProbeCount] = original;
                ++gPcProbeCount;
                runtime.replaceFunction(pc, &pcProbeCall);
                std::fprintf(stderr, "[pc] probe installed on 0x%08X\n", pc);
            }
            cursor = (*end == ',') ? end + 1 : end;
            while (*cursor == ' ')
                ++cursor;
        }
    }

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

    // ---------------------------------------------------------------------------------------------------------
    // TICK LOOP PROBE (TOL_TRACE_TICK=1)
    //
    // FUN_0011F650 is the timer-tick dispatcher the guest parks in after SetupThread/SetupHeap. Its state block is
    // addressed as `s5 - 0x7308` with `s5 = 0x00390000` (emitted verbatim as `lui $s5, 0x39` at 0x11F690), so
    // state = 0x00388CF8 and the active list head is the 32-bit pointer at state + 0x18 = 0x00388D10. That is read
    // from the recompiled code, not guessed.
    //
    // Layout of the state block, from FUN_0011F650 / FUN_0011F590 / FUN_0011F618:
    //   +0x00 u32  timer-2 overflow counter (the high half of the 64-bit time base)
    //   +0x10 u32  node count
    //   +0x14 u32  free-list head
    //   +0x18 u32  active-list head      <- the queue the tick drains
    //   +0x1C u32  handle currently running
    //
    // Layout of one node (32-bit pointer, fields per the same three functions):
    //   +0x00 u32 next | +0x08 u32 id | +0x0C u32 flags (bit 1 = periodic)
    //   +0x10 u64 interval | +0x18 u64 last | +0x20 u64 start | +0x28 u32 callback | +0x30 u32 arg
    constexpr uint32_t kTickLoopPc = 0x0011F650u;
    constexpr uint32_t kTickState = 0x00388CF8u;
    constexpr uint32_t kTickOffCount = 0x10u;
    constexpr uint32_t kTickOffFree = 0x14u;
    constexpr uint32_t kTickOffActive = 0x18u;
    constexpr uint32_t kTickOffCurrent = 0x1Cu;

    PS2Runtime::RecompiledFunction gTickOriginal = nullptr;

    bool tickTraceEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_TICK") != nullptr;
        return enabled;
    }

    bool plausibleNode(uint32_t pc)
    {
        return (pc & 3u) == 0u && pc >= 0x00100000u && pc < 0x02000000u;
    }

    void dumpTickQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t entries)
    {
        // $s5 is 0 on entry: the function loads it itself (`lui $s5, 0x39` at 0x11F690), so the state block has to
        // come from the recompiled code rather than from the entry register file. Measured, not assumed.
        const uint32_t s5 = GPR_U32(ctx, 21);
        const uint32_t state = kTickState;
        const uint32_t count = runtime->Load32(rdram, ctx, state + kTickOffCount);
        const uint32_t freeHead = runtime->Load32(rdram, ctx, state + kTickOffFree);
        const uint32_t active = runtime->Load32(rdram, ctx, state + kTickOffActive);
        const uint32_t current = runtime->Load32(rdram, ctx, state + kTickOffCurrent);

        std::fprintf(stderr,
                     "[tick] #%u s5=0x%08X sp=0x%08X ra=0x%08X state=0x%08X count=%u free=0x%08X active=0x%08X "
                     "current=0x%08X\n",
                     entries, s5, GPR_U32(ctx, 29), GPR_U32(ctx, 31), state, count, freeHead, active, current);

        uint32_t node = active;
        for (int i = 0; i < 8 && plausibleNode(node); ++i)
        {
            const uint32_t next = runtime->Load32(rdram, ctx, node + 0x00u);
            const uint32_t id = runtime->Load32(rdram, ctx, node + 0x08u);
            const uint32_t flags = runtime->Load32(rdram, ctx, node + 0x0Cu);
            const uint32_t interval = runtime->Load32(rdram, ctx, node + 0x10u);
            const uint32_t last = runtime->Load32(rdram, ctx, node + 0x18u);
            const uint32_t start = runtime->Load32(rdram, ctx, node + 0x20u);
            const uint32_t callback = runtime->Load32(rdram, ctx, node + 0x28u);
            const uint32_t arg = runtime->Load32(rdram, ctx, node + 0x30u);
            std::fprintf(stderr,
                         "[tick]   node[%d] @0x%08X next=0x%08X id=0x%08X flags=0x%08X interval=%u last=%u "
                         "start=%u cb=0x%08X arg=0x%08X\n",
                         i, node, next, id, flags, interval, last, start, callback, arg);
            if (next == node)
                break;
            node = next;
        }
        if (!plausibleNode(active))
            std::fprintf(stderr, "[tick]   active list EMPTY\n");
    }

    void tickProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sEntries = 0;
        const uint32_t entry = ++sEntries;
        // The loop runs ~16x/s; log the first few passes and then a heartbeat, so a 15 s run stays readable.
        if (entry <= 6u || (entry % 64u) == 0u)
            dumpTickQueue(rdram, ctx, runtime, entry);

        if (gTickOriginal != nullptr)
            gTickOriginal(rdram, ctx, runtime);
    }

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
        // The tick-loop probe wraps rather than replaces: it dumps the dispatcher's queue and then runs the real
        // recompiled body, so nothing about guest behaviour changes while the queue is being observed.
        installPcProbes(runtime);
        if (tickTraceEnabled())
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kTickLoopPc);
            if (original == nullptr)
            {
                std::fprintf(stderr, "[tick] 0x%08X is not in the function table\n", kTickLoopPc);
            }
            else
            {
                gTickOriginal = original;
                runtime.replaceFunction(kTickLoopPc, &tickProbe);
                std::fprintf(stderr, "[tick] probe installed on 0x%08X (state 0x%08X)\n", kTickLoopPc, kTickState);
            }
        }
        std::fprintf(stderr, "[tol] no native overrides installed (the Timer-2 override was withdrawn)\n");
    }
}
