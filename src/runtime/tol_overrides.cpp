#include "tol_overrides.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // ---------------------------------------------------------------------------------------------------------
    // NATIVE SIF RPC (the game's own libsif is statically linked, so its API entry points are guest code)
    //
    // Tales of Legendia carries libsif inside SLUS_212.01: sceSifBindRpc at 0x00118448 and sceSifCallRpc at
    // 0x00118628. Both send a SIF command to the IOP and block on a semaphore that only the IOP's reply would
    // signal, and this runtime has no IOP command channel behind the register file, so the main thread blocked
    // there forever (ee-dispatches=44 per 300 frames, transfers=0 draws=0).
    //
    // The runtime already implements these two services for titles that *import* libsif (ps2_syscalls::SifBindRpc
    // / SifCallRpc, which route through PS2IopTransport and the native IOP). Their argument conventions are the
    // SDK's, so substituting them for the guest's copies keeps every caller above them - the game's loadfile and
    // iopheap layers - unchanged while making the calls complete synchronously instead of waiting for a reply
    // that cannot arrive.
    constexpr uint32_t kGuestSifBindRpc = 0x00118448u;
    constexpr uint32_t kGuestSifCallRpc = 0x00118628u;
    // RPC sid of the IOP's loadfile service, which is what the game's module loader talks to.
    constexpr uint32_t kLoadFileSid = 0x80000006u;
    // The game's loadfile client requests: 0xFF initialises, anything else names a module to load.
    constexpr uint32_t kLoadFileInitFunction = 0xFFu;
    // Version string the game expects back from the loadfile init ("3000", i.e. IOPRP300).
    constexpr uint32_t kLoadFileVersion3000 = 0x30303033u; // '3','0','0','0' little-endian
    // Loadfile request layout: { argLen, 0, path[0xFC], args[] }.
    constexpr uint32_t kLoadFilePathOffset = 8u;

    // The game's own IOP file server (sid 0x80000597), used by its DVCI/HTCI layer.
    //
    // FUN_00101a60 opens through it: client 0x3DBA80, fn 0, a 300-byte request whose path sits at +0x24,
    // and a 4-byte reply. The IOP writes the file handle into the request's first word as well as
    // returning it, and fills stat words at +0x08/+0x10/+0x18/+0x20 (unused by the caller on the DVCI
    // path). Reads are asynchronous calls on a second client (fn 1, mode 1), so they are not handled here
    // yet - an unsupported function answers "not found" rather than pretending to succeed.
    constexpr uint32_t kGameFileSid = 0x80000597u;
    constexpr uint32_t kGameFileOpenFunction = 0u;
    constexpr uint32_t kGameFilePathOffset = 0x24u;

    constexpr int kMaxGameFiles = 32;
    uint32_t gGameFileHandle[kMaxGameFiles] = {};
    std::FILE *gGameFileStream[kMaxGameFiles] = {};
    uint64_t gGameFileSize[kMaxGameFiles] = {};
    uint32_t gGameFileNextHandle = 1u;

    // Opens through the same path translation the runtime's fileio stubs use, so "\AFS\FLIST.DIR;1"
    // lands on cdRoot + AFS/FLIST.DIR with the ISO version suffix stripped. The path was never the
    // problem: the open used to fail because this service had no implementation at all.
    uint32_t openGameFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pathAddress)
    {
        std::string guestPath;
        for (uint32_t i = 0; i < 256u; ++i)
        {
            const uint8_t byte = runtime->Load8(rdram, ctx, pathAddress + i);
            if (byte == 0u)
                break;
            guestPath.push_back(static_cast<char>(byte));
        }
        if (guestPath.empty())
            return 0u;

        const std::string hostPath = translatePs2Path(guestPath.c_str());
        const bool trace = std::getenv("TOL_TRACE_FILEIO") != nullptr;
        if (hostPath.empty())
        {
            if (trace)
                std::fprintf(stderr, "[fileio] gamefile open '%s' -> FAILED (no translation)\n", guestPath.c_str());
            return 0u;
        }

        std::FILE *stream = std::fopen(hostPath.c_str(), "rb");
        if (stream == nullptr)
        {
            if (trace)
                std::fprintf(stderr, "[fileio] gamefile open '%s' -> '%s' FAILED (missing)\n", guestPath.c_str(),
                             hostPath.c_str());
            return 0u;
        }

        for (int i = 0; i < kMaxGameFiles; ++i)
        {
            if (gGameFileHandle[i] == 0u)
            {
                gGameFileHandle[i] = gGameFileNextHandle++;
                gGameFileStream[i] = stream;
                gGameFileSize[i] = 0u;
                if (std::fseek(stream, 0, SEEK_END) == 0)
                {
                    const long size = std::ftell(stream);
                    if (size > 0)
                        gGameFileSize[i] = static_cast<uint64_t>(size);
                }
                std::fseek(stream, 0, SEEK_SET);
                if (trace)
                    std::fprintf(stderr, "[fileio] gamefile open '%s' -> '%s' ok, %llu bytes (handle %u)\n",
                                 guestPath.c_str(), hostPath.c_str(),
                                 static_cast<unsigned long long>(gGameFileSize[i]), gGameFileHandle[i]);
                return gGameFileHandle[i];
            }
        }
        std::fclose(stream);
        return 0u;
    }

    // Bind is per client, and the call only carries the client pointer, so the sid is remembered here.
    constexpr int kMaxRpcClients = 16;
    uint32_t gRpcClientAddr[kMaxRpcClients] = {};
    uint32_t gRpcClientSid[kMaxRpcClients] = {};
    int gRpcClientCount = 0;

    void rememberRpcClient(uint32_t client, uint32_t sid)
    {
        for (int i = 0; i < gRpcClientCount; ++i)
        {
            if (gRpcClientAddr[i] == client)
            {
                gRpcClientSid[i] = sid;
                return;
            }
        }
        if (gRpcClientCount < kMaxRpcClients)
        {
            gRpcClientAddr[gRpcClientCount] = client;
            gRpcClientSid[gRpcClientCount] = sid;
            ++gRpcClientCount;
        }
    }

    uint32_t rpcClientSid(uint32_t client)
    {
        for (int i = 0; i < gRpcClientCount; ++i)
        {
            if (gRpcClientAddr[i] == client)
                return gRpcClientSid[i];
        }
        return 0u;
    }

    // Runs the runtime's module loader for a path already sitting in guest memory, leaving the current
    // context's registers untouched so the RPC caller never sees the synthetic call.
    int32_t loadModuleFromGuestMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t pathAddress)
    {
        const uint32_t savedA0 = GPR_U32(ctx, 4);
        const uint32_t savedA1 = GPR_U32(ctx, 5);
        const uint32_t savedA2 = GPR_U32(ctx, 6);
        const uint32_t savedV0 = GPR_U32(ctx, 2);

        SET_GPR_U32(ctx, 4, pathAddress);
        SET_GPR_U32(ctx, 5, 0u);
        SET_GPR_U32(ctx, 6, 0u);
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
        const int32_t moduleId = static_cast<int32_t>(GPR_U32(ctx, 2));

        SET_GPR_U32(ctx, 4, savedA0);
        SET_GPR_U32(ctx, 5, savedA1);
        SET_GPR_U32(ctx, 6, savedA2);
        SET_GPR_U32(ctx, 2, savedV0);
        return moduleId;
    }

    bool traceRpcEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_RPC") != nullptr;
        return enabled;
    }

    void tolSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (traceRpcEnabled())
        {
            static uint32_t sLogs = 0u;
            if (sLogs < 64u)
            {
                ++sLogs;
                std::fprintf(stderr, "[rpc] bind client=0x%08X sid=0x%08X mode=0x%X\n", GPR_U32(ctx, 4),
                             GPR_U32(ctx, 5), GPR_U32(ctx, 6));
            }
        }
        rememberRpcClient(GPR_U32(ctx, 4), GPR_U32(ctx, 5));
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void tolSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t client = GPR_U32(ctx, 4);
        const uint32_t function = GPR_U32(ctx, 5);
        const uint32_t sendBuffer = GPR_U32(ctx, 7);
        const uint32_t sendSize = GPR_U32(ctx, 8);
        const uint32_t receiveBuffer = GPR_U32(ctx, 9);
        const uint32_t receiveSize = GPR_U32(ctx, 10);
        const uint32_t sid = rpcClientSid(client);

        if (traceRpcEnabled())
        {
            static uint32_t sLogs = 0u;
            if (sLogs < 256u)
            {
                ++sLogs;
                std::fprintf(stderr,
                             "[rpc] call client=0x%08X sid=0x%08X fn=0x%X mode=0x%X send=0x%08X/%u recv=0x%08X/%u\n",
                             client, sid, function, GPR_U32(ctx, 6), sendBuffer, sendSize, receiveBuffer,
                             receiveSize);
            }
        }

        // The loadfile service is the one that actually reads from the disc: the game sends a module path in
        // the send buffer and expects a module id back. No native IOP server answers sid 0x80000006, so the
        // runtime's own module loader serves it - the same one sceSifLoadModule reaches when a title imports
        // libsif instead of carrying it.
        if (sid == kLoadFileSid)
        {            uint32_t status = 0u;
            uint32_t moduleId = 0u;
            if (function == kLoadFileInitFunction)
            {
                // The game compares this reply against "3000" and "...." and only proceeds when it matches
                // one, i.e. it wants the version of the IOP image it just loaded (IOPRP300 -> "3000").
                status = kLoadFileVersion3000;
            }
            else if (sendBuffer != 0u && sendSize != 0u)
            {
                // The loadfile request is { argLen, 0, path[0xFC], args[] }: the path sits at +8, not at
                // the start of the send buffer (the game copies it to 0x3E2208 and sends from 0x3E2200).
                const uint32_t pathAddress = sendBuffer + kLoadFilePathOffset;
                const int32_t loaded = loadModuleFromGuestMemory(rdram, ctx, runtime, pathAddress);
                if (loaded > 0)
                {
                    moduleId = static_cast<uint32_t>(loaded);
                }
                else
                {
                    status = 0xFFFEFFFFu;
                }
                if (traceRpcEnabled())
                {
                    static uint32_t sPathLogs = 0u;
                    if (sPathLogs < 20u)
                    {
                        ++sPathLogs;
                        std::fprintf(stderr, "[rpc] loadfile path '%.96s'\n",
                                     reinterpret_cast<const char *>(rdram) + (pathAddress & 0x01FFFFFFu));
                    }
                }
            }
            if (receiveBuffer != 0u && receiveSize >= 4u)
            {
                runtime->Store32(rdram, ctx, receiveBuffer, status);
            }
            if (receiveBuffer != 0u && receiveSize >= 8u)
            {
                runtime->Store32(rdram, ctx, receiveBuffer + 4u, moduleId);
            }
            if (traceRpcEnabled())
            {
                std::fprintf(stderr, "[rpc] loadfile fn=0x%X -> status=0x%08X module=%u\n", function, status,
                             moduleId);
            }
            SET_GPR_S32(ctx, 2, 0);
            return;
        }

        // The game's own file server: fn 0 opens a path and answers with the handle. This is the call
        // behind "DVCI: Can't read filelist file.(\AFS\FLIST.DIR)" - the path was never the problem.
        if (sid == kGameFileSid)
        {
            if (function == kGameFileOpenFunction)
            {
                const uint32_t handle =
                    (sendBuffer != 0u) ? openGameFile(rdram, ctx, runtime, sendBuffer + kGameFilePathOffset) : 0u;
                if (traceRpcEnabled())
                {
                    static uint32_t sOpenLogs = 0u;
                    if (sOpenLogs < 64u)
                    {
                        ++sOpenLogs;
                        std::fprintf(stderr, "[rpc] gamefile open '%.96s' -> handle %u\n",
                                     reinterpret_cast<const char *>(rdram) +
                                         ((sendBuffer + kGameFilePathOffset) & 0x01FFFFFFu),
                                     handle);
                    }
                }
                if (handle != 0u)
                {
                    // The IOP also writes the handle into the request's first word.
                    runtime->Store32(rdram, ctx, sendBuffer, handle);
                    if (receiveBuffer != 0u && receiveSize >= 4u)
                    {
                        runtime->Store32(rdram, ctx, receiveBuffer, handle);
                    }
                    SET_GPR_S32(ctx, 2, 0);
                    return;
                }
            }
            // Anything else is an unsupported function of this service.
            if (receiveBuffer != 0u && receiveSize >= 4u)
            {
                runtime->Store32(rdram, ctx, receiveBuffer, 0u);
            }
            SET_GPR_S32(ctx, 2, -1);            return;
        }

        ps2_syscalls::SifCallRpc(rdram, ctx, runtime);
    }

    void installSifRpcOverrides(PS2Runtime &runtime)
    {
        struct Override
        {
            uint32_t pc;
            const char *name;
            PS2Runtime::RecompiledFunction replacement;
        };
        const Override overrides[] = {
            {kGuestSifBindRpc, "sceSifBindRpc", &tolSifBindRpc},
            {kGuestSifCallRpc, "sceSifCallRpc", &tolSifCallRpc},
        };
        for (const Override &entry : overrides)
        {
            if (runtime.lookupFunction(entry.pc) == nullptr)
            {
                std::fprintf(stderr, "[rpc] %s (0x%08X) is not in the function table\n", entry.name, entry.pc);
                continue;
            }
            runtime.replaceFunction(entry.pc, entry.replacement);
        }
        std::fprintf(stderr, "[rpc] native SIF bind/call installed (0x%08X, 0x%08X)\n", kGuestSifBindRpc,
                     kGuestSifCallRpc);
    }

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
        installSifRpcOverrides(runtime);
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
