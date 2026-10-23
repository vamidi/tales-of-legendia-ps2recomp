#include "Common.h"
#include "SIF.h"
#include "CD.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        bool g_sifIopBooted = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifIopBooted = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        // ---- Emulated IOP side of the SIFCMD / SIFRPC initialization -------
        // This game links its own libkernel SIF implementation, so its SIFCMD
        // traffic never reaches the sceSif* stubs (or their RPC bridge): it is
        // posted to the IOP with raw sceSifSetDma descriptors.  The runtime has
        // to play the IOP's documented part of the handshake instead.
        //
        // "SIFCMD Init" (80000002h) is sent twice by the EE during SIFRPC
        // initialization - first with opt=0, then with opt=1 - and the IOP
        // answers with "Set SREG" (80000001h), which updates the EE's
        // _sif_cmd_data.sregs[] software register mirror.  sceSifInitRpc spins
        // on sregs[SIF_SREG_RPCINIT] until the opt=1 answer arrives.
        //
        // Only opt != 0 finishes SIFRPC init; opt == 0 publishes the EE's SIF0
        // receive buffer and is already covered by the SIF_SMFLG seed.
        constexpr uint32_t kSifCmdSifcmdInit = 0x80000002u;
        constexpr uint32_t kSifSregRpcInit = 0u;
        constexpr uint32_t kSifSysregCmdData = 0x80000001u; // holds &_sif_cmd_data
        constexpr uint32_t kSifCmdDataSregsOffset = 0x1Cu;
        constexpr uint32_t kSifCmdHeaderCidOffset = 0x8u;
        constexpr uint32_t kSifCmdHeaderOptOffset = 0xCu;
        constexpr uint32_t kSifCmdHeaderSize = 16u;

        void emulateIopSifCmdInitHandshake(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                           const Ps2SifDmaTransfer &xfer)
        {
            if (!runtime || xfer.src == 0u || xfer.size < static_cast<int32_t>(kSifCmdHeaderSize))
            {
                return;
            }

            const uint32_t src = xfer.src;
            if (runtime->Load32(rdram, ctx, src + kSifCmdHeaderCidOffset) != kSifCmdSifcmdInit)
            {
                return;
            }

            const uint32_t opt = runtime->Load32(rdram, ctx, src + kSifCmdHeaderOptOffset);
            if (opt == 0u)
            {
                return;
            }

            uint32_t cmdData = 0u;
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                auto it = g_sifRegs.find(kSifSysregCmdData);
                if (it != g_sifRegs.end())
                {
                    cmdData = it->second;
                }
            }

            // SIF_SYSREG_MAINADDR is published by the EE's sceSifInitCmd; it is
            // the only way to find the guest's sregs[] mirror from here.
            static bool loggedFailure = false;
            const uint32_t sregs = cmdData != 0u ? runtime->Load32(rdram, ctx, cmdData + kSifCmdDataSregsOffset) : 0u;
            if (sregs == 0u)
            {
                if (!loggedFailure)
                {
                    loggedFailure = true;
                    std::cerr << "[SIF-iop] SIFCMD init: _sif_cmd_data/sregs unavailable "
                                 "(SIF reg 0x80000001 = 0x"
                              << std::hex << cmdData << "); cannot answer Set SREG" << std::dec << std::endl;
                }
                return;
            }

            const uint32_t slot = sregs + (kSifSregRpcInit * 4u);
            runtime->Store32(rdram, ctx, slot, 1u);

            static bool loggedSuccess = false;
            if (!loggedSuccess)
            {
                loggedSuccess = true;
                std::cerr << "[SIF-iop] SIFCMD init opt=" << opt << ": answered Set SREG RPCINIT=1 at 0x"
                          << std::hex << slot << " (_sif_cmd_data=0x" << cmdData
                          << " sregs=0x" << sregs << ")" << std::dec << std::endl;
            }
        }

        // ---- Raw SIFRPC command stream -------------------------------------
        // The RPC commands share the 16-byte SifCmdHeader above and add an
        // SifRpcPktHeader tail (rec_id, pkt_addr, rpc_id); the command specific
        // payload always starts with the client data pointer the EE passed to
        // its in-ELF sceSifBindRpc/sceSifCallRpc.
        constexpr uint32_t kSifCmdRpcEnd = 0x80000008u;
        constexpr uint32_t kSifCmdRpcBind = 0x80000009u;
        constexpr uint32_t kSifCmdRpcCall = 0x8000000Au;
        constexpr uint32_t kSifCmdRpcData = 0x8000000Bu;
        constexpr uint32_t kSifCmdRpcOtherData = 0x8000000Cu;
        constexpr uint32_t kSifCmdRebootIop = 0x80000003u;

        // Module-version handshake reply.
        //
        // A service is asked for its version with rpc 0xff (no send payload,
        // 4-byte reply) and the guest memcmps that answer against the version
        // literal it was built for: FUN_0011cfe0 compares the reply against the
        // ASCII "3000" at 0x38763c (and against the wildcard-ish "...." string
        // that 0x387714 points at).  On real hardware the IOP module reports its
        // own build version here; the emulated IOP has to report the version the
        // guest requires, otherwise FUN_0011d6b8 returns 0xFFFEFFFC and its
        // caller's retry loop (entry_00166360) never terminates.
        constexpr uint32_t kSifIopModuleVersion = 0x30303033u; // "3000"
        constexpr uint32_t kSifRpcGetVersion = 0xFFu;

        // Deci2 handshake reply.
        //
        // Once the SIF layer is up the guest's libdbc asks the DBCMAN IOP
        // service for its version (rpc 0x80001363, no meaningful request) and
        // warns
        //     libdbc: Module version mismatch [libdbc.a = 3.10, dbcman.irx = 0.00]
        // when the answer does not match the 3.10 it was built against; it then
        // stops making progress.
        //
        // The version is BCD, so 0x0310 reads as "3.10", and it is repeated
        // across the reply.  This is the same value ps2xIOP's HLE dbcman service
        // reports (ps2xIOP/src/modules/dbcman.cpp, kReportedVersion).
        constexpr uint32_t kDbcManRpcCheckVersion = 0x80001363u;
        constexpr uint32_t kDbcManReportedVersion = 0x00000310u; // BCD "3.10"

        // SIF heap service (sid 0x80000003).
        //
        // libsif's sceSifAllocIopHeap()/sceSifFreeIopHeap() are not local: they
        // are RPCs to the IOP heap manager.  rpc 1 sends a u32 size and expects
        // the u32 IOP address back (0 = failure), rpc 2 sends the address to
        // release.  No module in the emulated IOP claims this sid, so the
        // generic reply path used to send back 0 for every allocation and
        // SJX_Init treated it as fatal:
        //     E0100301: SJX_Init can't allocate IOP Heap
        // after which it falls into the nop sled at 0x1B6B80 and never returns,
        // which from the outside looks exactly like an idle thread.
        constexpr uint32_t kSifSidHeap = 0x80000003u;
        constexpr uint32_t kSifHeapRpcAlloc = 1u;
        constexpr uint32_t kSifHeapRpcFree = 2u;

        // fileio/ioman service.  The guest's stdio layer (sceStdioInit at
        // 0x119568) submits every file operation as an *asynchronous* request:
        // the send header starts with the semaphore the completion notification
        // signals (+0x00 sema, +0x04 result pointer, +0x08 result size,
        // +0x0C flags, +0x10 argument, +0x14 path / data).  On hardware the IOP
        // performs the I/O, writes the payload to the result pointer and then
        // posts SIFCMD cid 0x80000011 with the slot index; the EE handler at
        // 0x119030 copies the data and calls iSignalSema(slot->sema).  The HLE
        // service does the I/O synchronously, so it answers here instead of
        // posting that notification - without it sceOpen waits on a semaphore
        // that nothing ever signals.
        constexpr uint32_t kSifSidFileIo = 0x80000001u;
        constexpr uint32_t kSifFileIoRpcInit = 0xFFu;
        constexpr uint32_t kSifMaxSemaphoreId = 256u;

        // libcdvd RPC services.  The game's CRI ADXF library (ADXF/PS2EE 7.14)
        // links its own copy of libcdvd and talks to cdvd.irx through these
        // sids instead of the imported sceCd* entry points, so the calls never
        // reach the emulated IOP.  They are served from the same host-CD model
        // the sceCd* stubs use, which keeps the pseudo-LBN assignment shared:
        //   0x80000597 - main channel   (CdSearchFile: rpc 0)
        //   0x80000595 - stream channel (stream start/read: rpc 1)
        //   0x80000592 - "N cmd" channel (CdApplyNCmd)
        //   0x80000593 - "S cmd" channel (CdApplySCmd)
        constexpr uint32_t kSifSidCdvdMain = 0x80000597u;
        constexpr uint32_t kSifSidCdvdStream = 0x80000595u;
        constexpr uint32_t kSifSidCdvdNcmd = 0x80000592u;
        constexpr uint32_t kSifSidCdvdScmd = 0x80000593u;
        constexpr uint32_t kSifCdvdRpcSearchFile = 0u;
        constexpr uint32_t kSifCdvdRpcStreamRead = 1u;
        constexpr uint32_t kSifCdvdSearchFilePathOffset = 0x24u;

        // The libcdvd RPC clients are ordinary EE functions; hand a scratch
        // context to the matching sceCd* stub so both paths share one CD model.
        void setScratchGpr(R5900Context &scratch, int reg, uint32_t value)
        {
            if (reg <= 0 || reg > 31)
            {
                return;
            }
            scratch.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        // SIF_SMFLG (register 4) carries the boot progress handshake flags:
        //   0x10000 - SIF DMA/hardware initialized
        //   0x20000 - SIFCMD initialized
        //   0x40000 - the IOP has finished booting (announced with EESYNC)
        // The game polls 0x40000 in a spin loop until the IOP reports ready.
        constexpr uint32_t kSifRegSmflg = 0x4u;
        constexpr uint32_t kSifSmflgBootedMask = 0x00070000u;

        // _sif_cmd_data layout used by the guest's own libkernel sifcmd/sifrpc
        // (confirmed against FUN_00117aa8 = _SifCmdIntHandler).
        constexpr uint32_t kSifCmdDataPktBufOffset = 0x00u;
        constexpr uint32_t kSifCmdDataSysHandlersOffset = 0x0Cu;
        constexpr uint32_t kSifCmdDataNrSysOffset = 0x10u;
        constexpr uint32_t kSifCmdHandlerStride = 12u;

        // TEMP DIAGNOSTIC (raw-SIFRPC bridge): the game registers its RPC
        // command handlers through its in-ELF libkernel, so the runtime can
        // only learn which cids matter - and which guest routines implement
        // them - by reading _sif_cmd_data.sys_cmd_handlers[] straight out of
        // guest RAM once the RPC layer is up.
        void dumpSifCmdHandlersOnce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                    const Ps2SifDmaTransfer &xfer)
        {
            static bool dumped = false;
            if (dumped || !runtime || xfer.src == 0u || xfer.size < static_cast<int32_t>(kSifCmdHeaderSize))
            {
                return;
            }
            if (runtime->Load32(rdram, ctx, xfer.src + kSifCmdHeaderCidOffset) != kSifCmdRpcBind)
            {
                return;
            }

            uint32_t cmdData = 0u;
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                auto it = g_sifRegs.find(kSifSysregCmdData);
                if (it != g_sifRegs.end())
                {
                    cmdData = it->second;
                }
            }
            if (cmdData == 0u)
            {
                return;
            }

            dumped = true;
            const uint32_t table = runtime->Load32(rdram, ctx, cmdData + kSifCmdDataSysHandlersOffset);
            const uint32_t count = runtime->Load32(rdram, ctx, cmdData + kSifCmdDataNrSysOffset);
            const uint32_t pktbuf = runtime->Load32(rdram, ctx, cmdData + kSifCmdDataPktBufOffset);
            std::cerr << "[SIF-iop] sys_cmd_handlers table=0x" << std::hex << table
                      << " nr=0x" << count << " pktbuf=0x" << pktbuf << std::dec << std::endl;
            for (uint32_t i = 0u; i < count && i < 64u; ++i)
            {
                const uint32_t entry = table + i * kSifCmdHandlerStride;
                const uint32_t func = runtime->Load32(rdram, ctx, entry);
                if (func == 0u)
                {
                    continue;
                }
                const uint32_t arg = runtime->Load32(rdram, ctx, entry + 4u);
                std::cerr << "[SIF-iop]   cid=0x" << std::hex << (0x80000000u | i)
                          << " func=0x" << func << " arg=0x" << arg << std::dec << std::endl;
            }
        }

        // ---- Emulated IOP side of the raw SIFRPC stream ---------------------
        // The game drives SIFRPC through its own in-ELF libkernel, so the
        // runtime's sceSif* stubs (and the RPC bridge behind them) are never
        // reached: every request is a plain SIF command posted with
        // sceSifSetDma.  The IOP answers with a SIF_CMD_RPC_END (80000008h)
        // packet whose +0x20 field carries the cid of the request that just
        // finished - exactly what the guest's own _SifRpcEndHandler
        // (FUN_00117fa0) dispatches on before it signals client->hdr.sema_id.

        uint32_t sifCmdDataAddr()
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto it = g_sifRegs.find(kSifSysregCmdData);
            return it != g_sifRegs.end() ? it->second : 0u;
        }

        // Defined further down with the other EE memory helpers.
        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes);

        // Posts one emulated IOP -> EE command into the receive buffer the EE
        // published with SifInitPkt (opt=0), i.e. _sif_cmd_data.pktbuf.  The
        // caller raises SIF0/DMAC-5 afterwards (sceSifSetDma already does that
        // once it has finished the outgoing transfers).
        bool postIopSifCommand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                               const uint32_t *words, uint32_t wordCount)
        {
            if (!runtime || !words || wordCount == 0u)
            {
                return false;
            }
            const uint32_t cmdData = sifCmdDataAddr();
            if (cmdData == 0u)
            {
                return false;
            }
            const uint32_t pktbuf = runtime->Load32(rdram, ctx, cmdData + kSifCmdDataPktBufOffset);
            if (pktbuf == 0u)
            {
                return false;
            }
            return writeEeRange(rdram, pktbuf, words, wordCount * 4u);
        }

        // Guest-RAM server descriptor published as client->server.  The SDK
        // treats server == NULL as "bind failed, retry", so the bind answer has
        // to carry a readable non-zero pointer - the same contract the runtime
        // implements on its sceSifBindRpc stub path.
        struct SifRpcServerSlot
        {
            uint32_t sid;
            uint32_t addr;
        };
        constexpr uint32_t kSifRpcServerSlotCount = 32u;
        constexpr uint32_t kSifRpcServerSlotBytes = 0x30u;
        SifRpcServerSlot g_sifRpcServerSlots[kSifRpcServerSlotCount]{};

        uint32_t sifRpcServerAddr(PS2Runtime *runtime, uint32_t sid)
        {
            for (uint32_t i = 0u; i < kSifRpcServerSlotCount; ++i)
            {
                if (g_sifRpcServerSlots[i].addr != 0u && g_sifRpcServerSlots[i].sid == sid)
                {
                    return g_sifRpcServerSlots[i].addr;
                }
            }
            for (uint32_t i = 0u; i < kSifRpcServerSlotCount; ++i)
            {
                if (g_sifRpcServerSlots[i].addr == 0u)
                {
                    const uint32_t addr = runtime->guestCalloc(1u, kSifRpcServerSlotBytes);
                    if (addr != 0u)
                    {
                        g_sifRpcServerSlots[i] = {sid, addr};
                    }
                    return addr;
                }
            }
            return 0u;
        }

        // TEMP DIAGNOSTIC (raw-SIFRPC bridge): remembered between the reply
        // post and the DMAC-5 dispatch so the guest's own handlers can be
        // observed doing their work.
        uint32_t g_lastRpcClientPtr = 0u;
        uint32_t g_lastRpcServerWanted = 0u;

        // raw-SIFRPC -> emulated-IOP routing.
        //
        // The guest's own statically linked libkernel (not the runtime's
        // sceSif* stubs) drives the SIF RPC protocol, so this responder is the
        // only place a guest call can reach the emulated IOP.  Two things are
        // needed to route one:
        //   * which sid the calling client bound (recovered from the bind
        //     exchange below), and
        //   * the EE address of the call's send payload, which the guest hands
        //     over as a sibling DMA descriptor of the command packet
        //     (sceSifSendCmd issues one descriptor for the packet and one for
        //     the payload, in either order).
        std::unordered_map<uint32_t, uint32_t> g_sifRpcClientSid;
        uint32_t g_sifRpcPayloadPrecedingEeAddr = 0u;
        uint32_t g_sifRpcPayloadPrecedingSize = 0u;
        uint32_t g_sifRpcPayloadFollowingEeAddr = 0u;
        uint32_t g_sifRpcPayloadFollowingSize = 0u;
        uint64_t g_sifRpcRoutedCallCount = 0u;

        // SIDs already given a bounded IOP catch-up (see catchUpIopForSid).
        std::unordered_set<uint32_t> g_sifIopCatchUpTriedSids;

        // The EE and the IOP are separate processors: a guest module registers
        // its RPC servers from the start-up thread its entry creates, and that
        // thread only runs while the IOP is pumped - SIF traffic itself is
        // usually what releases it.  A call from a client whose bound service
        // the emulated IOP does not own yet is therefore given a bounded
        // catch-up first, otherwise the EE reaches its first call before the
        // module it just loaded has finished starting.
        constexpr uint32_t kIopCatchUpRounds = 32u;
        constexpr uint64_t kIopCatchUpEeCycles = 1048576u;

        void catchUpIopForSid(PS2Runtime *runtime, uint32_t sid)
        {
            if (runtime == nullptr || sid == 0u)
                return;
            if (g_sifIopCatchUpTriedSids.count(sid) != 0u)
                return;

            g_sifIopCatchUpTriedSids.insert(sid);
            if (PS2IopTransport::canBindRpc(runtime, sid))
                return;

            for (uint32_t round = 0u; round < kIopCatchUpRounds; ++round)
            {
                PS2IopTransport::catchUp(runtime, kIopCatchUpEeCycles);
                if (PS2IopTransport::canBindRpc(runtime, sid))
                {
                    std::cerr << "[SIF-iop] IOP catch-up: sid=0x" << std::hex << sid
                              << std::dec << " claimed by the emulated IOP after "
                              << (round + 1u) << " round(s)" << std::endl;
                    return;
                }
            }

            const ps2x::iop::DebugSnapshot snapshot = runtime->iopDebugSnapshot();
            std::cerr << "[SIF-iop] IOP catch-up: sid=0x" << std::hex << sid << std::dec
                      << " still unclaimed after " << kIopCatchUpRounds
                      << " round(s); iop cycles=" << snapshot.emulatorCycles
                      << " modules=" << snapshot.emulatorLoadedModules
                      << " threads=" << snapshot.emulatorThreads
                      << " rpcServers=" << snapshot.emulatorRpcServers << std::endl;
        }

        void emulateIopSifRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                              const Ps2SifDmaTransfer &xfer)
        {
            if (!runtime || xfer.src == 0u || xfer.size < 0x24)
            {
                return;
            }

            const uint32_t packet = xfer.src;
            const uint32_t cid = runtime->Load32(rdram, ctx, packet + kSifCmdHeaderCidOffset);

            if (cid == kSifCmdRebootIop)
            {
                // Reboot IOP (SifIopResetPkt): the EE hands the IOP a module list
                // to boot.  On real hardware the IOP loads them and then announces
                // completion with EESYNC, which sets SMFLG bit 0x40000 ("IOP has
                // finished booting").  The emulated IOP always succeeds, so the
                // completion is latched here and reported through sceSifGetReg.
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                g_sifIopBooted = true;
                g_sifRegs[kSifRegSmflg] |= kSifSmflgBootedMask;
                std::cerr << "[SIF-iop] Reboot IOP: module list accepted, IOP boot complete (SMFLG=0x"
                          << std::hex << g_sifRegs[kSifRegSmflg] << std::dec << ")" << std::endl;
                return;
            }

            if (cid != kSifCmdRpcBind && cid != kSifCmdRpcCall)
            {
                return;
            }

            const uint32_t recId = runtime->Load32(rdram, ctx, packet + 0x10u);
            const uint32_t clientPktAddr = runtime->Load32(rdram, ctx, packet + 0x14u);
            const uint32_t rpcId = runtime->Load32(rdram, ctx, packet + 0x18u);
            const uint32_t clientPtr = runtime->Load32(rdram, ctx, packet + 0x1Cu);

            if (cid == kSifCmdRpcCall)
            {
                // SifRpcCallPkt: {SifCmdHeader sifcmd; rec_id; pkt_addr; rpc_id;
                //                 SifRpcClientData *client; u32 rpc_number;
                //                 u32 send_size; void *receive; u32 recv_size;
                //                 u32 rmode; SifRpcServerData *server;}
                const uint32_t rpcNumber = runtime->Load32(rdram, ctx, packet + 0x20u);
                const uint32_t sendSize = runtime->Load32(rdram, ctx, packet + 0x24u);
                const uint32_t receiveAddr = runtime->Load32(rdram, ctx, packet + 0x28u);
                const uint32_t recvSize = runtime->Load32(rdram, ctx, packet + 0x2Cu);
                const uint32_t rmode = runtime->Load32(rdram, ctx, packet + 0x30u);
                const uint32_t server = runtime->Load32(rdram, ctx, packet + 0x34u);

                // The send payload travels as the DMA sibling whose recorded
                // size matches the call's declared send size.  Both the routed
                // path and the HLE services below need it.
                uint32_t callPayloadAddr = 0u;
                if (sendSize != 0u)
                {
                    if (g_sifRpcPayloadPrecedingSize == sendSize)
                    {
                        callPayloadAddr = g_sifRpcPayloadPrecedingEeAddr;
                    }
                    else if (g_sifRpcPayloadFollowingSize == sendSize)
                    {
                        callPayloadAddr = g_sifRpcPayloadFollowingEeAddr;
                    }
                }

                uint32_t recvBefore[4] = {0u, 0u, 0u, 0u};
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    recvBefore[i] = runtime->Load32(rdram, ctx, receiveAddr + i * 4u);
                }

                // TEMP DIAGNOSTIC: the per-service responses still have to be
                // derived from each call site; echo the request shape so the next
                // stall can be triaged from the log alone.
                std::cerr << "[SIF-iop] RPC CALL sid=- client=0x" << std::hex << clientPtr
                          << " rpc_number=0x" << rpcNumber
                          << " send_size=0x" << sendSize
                          << " receive=0x" << receiveAddr
                          << " recv_size=0x" << recvSize
                          << " rmode=0x" << rmode
                          << " server=0x" << server
                          << " recv_before=" << recvBefore[0] << "," << recvBefore[1] << ","
                          << recvBefore[2] << "," << recvBefore[3] << std::dec << std::endl;

                // IOP routing: when the service this client bound is owned by
                // the emulated IOP (a loaded IRX) or by an HLE service, hand the
                // call over instead of fabricating a reply.  The IOP side copies
                // the send payload out of EE memory into the service's buffer and
                // writes the response straight into the caller's EE receive
                // buffer, so no extra plumbing is required here.
                bool iopHandled = false;
                uint32_t boundSid = 0u;
                {
                    const auto clientSid = g_sifRpcClientSid.find(clientPtr);
                    if (clientSid != g_sifRpcClientSid.end())
                    {
                        boundSid = clientSid->second;
                    }
                }

                catchUpIopForSid(runtime, boundSid);

                if (boundSid != 0u && PS2IopTransport::canBindRpc(runtime, boundSid))
                {
                    // Locate the payload descriptor: prefer the sibling whose
                    // recorded size matches the call's declared send size.
                    const uint32_t sendEeAddr = callPayloadAddr;

                    if (sendSize == 0u || sendEeAddr != 0u)
                    {
                        ps2x::iop::RpcRequest request{};
                        request.clientAddress = clientPtr;
                        request.serverAddress = server;
                        request.sid = boundSid;
                        request.function = rpcNumber;
                        request.mode = rmode;
                        request.send = {sendEeAddr, sendSize};
                        request.receive = {receiveAddr, recvSize};
                        request.endFunction = 0u;
                        request.endParameter = 0u;

                        const ps2x::iop::RpcResult routed =
                            PS2IopTransport::handleRpc(runtime, rdram, ctx, request);
                        iopHandled = routed.handled;
                        ++g_sifRpcRoutedCallCount;

                        std::cerr << "[SIF-iop] RPC ROUTE sid=0x" << std::hex << boundSid
                                  << " rpc=0x" << rpcNumber
                                  << " send=0x" << sendEeAddr
                                  << " send_size=0x" << sendSize
                                  << " receive=0x" << receiveAddr
                                  << " recv_size=0x" << recvSize
                                  << std::dec
                                  << " handled=" << (iopHandled ? 1 : 0)
                                  << std::endl;

                        // fileio: complete the request the way the IOP would.
                        // The stdio layer posts the operation, then waits on the
                        // semaphore in the request header for the completion
                        // notification (see kSifSidFileIo).
                        if (iopHandled && boundSid == kSifSidFileIo &&
                            rpcNumber != kSifFileIoRpcInit && sendSize >= 4u)
                        {
                            const uint32_t completionSema =
                                runtime->Load32(rdram, ctx, sendEeAddr);
                            if (completionSema > 0u && completionSema < kSifMaxSemaphoreId)
                            {
                                const int signalled = runtime->eeScheduler().signalSemaphore(
                                    static_cast<int>(completionSema), true);
                                std::cerr << "[SIF-iop] fileio completion sema=" << completionSema
                                          << " rpc=0x" << std::hex << rpcNumber << std::dec
                                          << " ret=" << signalled << std::endl;
                            }
                        }
                    }
                    else
                    {
                        std::cerr << "[SIF-iop] RPC ROUTE sid=0x" << std::hex << boundSid
                                  << " rpc=0x" << rpcNumber
                                  << " send_size=0x" << sendSize
                                  << std::dec
                                  << " payload descriptor not found -> fabricating"
                                  << std::endl;
                    }
                }

                if (!iopHandled)
                {
                    // TEMP DIAGNOSTIC: an unrouted call still has a payload
                    // descriptor in the SIF DMA chain.  Dump it so a custom
                    // service (0x80000597, the CDV/AFS stream loader) can be
                    // reconstructed from the request bytes alone.
                    if (boundSid != 0u && sendSize != 0u && sendSize <= 0x400u)
                    {
                        uint32_t payloadAddr = 0u;
                        if (g_sifRpcPayloadPrecedingSize == sendSize)
                        {
                            payloadAddr = g_sifRpcPayloadPrecedingEeAddr;
                        }
                        else if (g_sifRpcPayloadFollowingSize == sendSize)
                        {
                            payloadAddr = g_sifRpcPayloadFollowingEeAddr;
                        }

                        std::cerr << "[SIF-iop] UNROUTED sid=0x" << std::hex << boundSid
                                  << " rpc=0x" << rpcNumber
                                  << " client=0x" << clientPtr
                                  << " payload=0x" << payloadAddr
                                  << " size=0x" << sendSize << " words:";
                        if (payloadAddr != 0u)
                        {
                            for (uint32_t offset = 0u; offset + 4u <= sendSize; offset += 4u)
                            {
                                std::cerr << " " << runtime->Load32(rdram, ctx, payloadAddr + offset);
                            }
                        }
                        std::cerr << std::dec << std::endl;
                    }

                    // The IOP answers a call by placing the service's response in the
                    // caller's receive buffer, then completing the call with an RPC
                    // END whose +0x20 names the request cid: _SifRpcEndHandler
                    // (0x117fa0) then runs client->end_function and signals
                    // client->hdr.sema_id, which is what sceSifCallRpc waits on.
                    for (uint32_t offset = 0u; offset + 4u <= recvSize; offset += 4u)
                    {
                        runtime->Store32(rdram, ctx, receiveAddr + offset, 0u);
                    }

                    // SIF heap service: answer the guest's IOP-heap requests from
                    // the emulated IOP allocator (the same one sceSifAllocIopHeap
                    // uses).  The request payload is the DMA descriptor that the
                    // guest posted right before the packet, which the SIF handler
                    // already recorded as the payload sibling.
                    if (boundSid == kSifSidHeap)
                    {
                        uint32_t heapRequest = 0u;
                        if (sendSize == 4u)
                        {
                            if (g_sifRpcPayloadPrecedingSize == sendSize)
                            {
                                heapRequest = g_sifRpcPayloadPrecedingEeAddr;
                            }
                            else if (g_sifRpcPayloadFollowingSize == sendSize)
                            {
                                heapRequest = g_sifRpcPayloadFollowingEeAddr;
                            }
                        }

                        if (heapRequest != 0u)
                        {
                            const uint32_t argument = runtime->Load32(rdram, ctx, heapRequest);
                            if (rpcNumber == kSifHeapRpcAlloc && recvSize >= 4u)
                            {
                                const uint32_t address = runtime->allocateIopMemory(argument, 64u);
                                runtime->Store32(rdram, ctx, receiveAddr, address);
                                std::cerr << "[SIF-iop] IOP heap alloc 0x" << std::hex
                                          << argument << " -> 0x" << address << std::dec
                                          << std::endl;
                            }
                            else if (rpcNumber == kSifHeapRpcFree)
                            {
                                const bool released = runtime->freeIopMemory(argument);
                                std::cerr << "[SIF-iop] IOP heap free 0x" << std::hex
                                          << argument << (released ? " ok" : " unknown")
                                          << std::dec << std::endl;
                            }
                            else
                            {
                                std::cerr << "[SIF-iop] IOP heap rpc 0x" << std::hex << rpcNumber
                                          << " unhandled (send=0x" << argument << ")" << std::dec
                                          << std::endl;
                            }
                        }
                    }

                    // libcdvd service.  The ADXF library's own libcdvd copy
                    // issues raw RPCs (CdSearchFile, stream start) instead of
                    // calling the imported sceCd* entries, so answer them from
                    // the shared host-CD model.  CdSearchFile fills the request
                    // buffer's leading sceCdlFILE (lsn/size/name/date) and
                    // returns 1; the stream request is {lsn, sectors, dest}.
                    if (boundSid == kSifSidCdvdMain || boundSid == kSifSidCdvdStream ||
                        boundSid == kSifSidCdvdNcmd || boundSid == kSifSidCdvdScmd)
                    {
                        std::cerr << "[SIF-iop] cdvd sid=0x" << std::hex << boundSid
                                  << " rpc=0x" << rpcNumber
                                  << " payload=0x" << callPayloadAddr
                                  << " send_size=0x" << sendSize
                                  << " recv_size=0x" << recvSize << std::dec << std::endl;

                        if (callPayloadAddr != 0u)
                        {
                        if (boundSid == kSifSidCdvdMain && rpcNumber == kSifCdvdRpcSearchFile &&
                            sendSize >= kSifCdvdSearchFilePathOffset)
                        {
                            R5900Context scratch{};
                            scratch.pc = 0x00101a60u;
                            setScratchGpr(scratch, 4, callPayloadAddr);
                            setScratchGpr(scratch, 5, callPayloadAddr + kSifCdvdSearchFilePathOffset);
                            ps2_stubs::sceCdSearchFile(rdram, &scratch, runtime);
                            const uint32_t found = getRegU32(&scratch, 2);
                            if (recvSize >= 4u)
                            {
                                runtime->Store32(rdram, ctx, receiveAddr, found);
                            }
                            std::cerr << "[SIF-iop] cdvd search -> " << found << std::endl;
                        }
                        else if (boundSid == kSifSidCdvdStream &&
                                 rpcNumber == kSifCdvdRpcStreamRead && sendSize >= 0x18u)
                        {
                            const uint32_t lsn = runtime->Load32(rdram, ctx, callPayloadAddr + 0x00u);
                            const uint32_t sectors = runtime->Load32(rdram, ctx, callPayloadAddr + 0x04u);
                            const uint32_t dest = runtime->Load32(rdram, ctx, callPayloadAddr + 0x08u);
                            R5900Context scratch{};
                            scratch.pc = 0x001029a8u;
                            setScratchGpr(scratch, 4, lsn);
                            setScratchGpr(scratch, 5, sectors);
                            setScratchGpr(scratch, 6, dest);
                            ps2_stubs::sceCdRead(rdram, &scratch, runtime);
                            std::cerr << "[SIF-iop] cdvd stream read lsn=0x" << std::hex << lsn
                                      << " sectors=" << std::dec << sectors
                                      << " dest=0x" << std::hex << dest << std::dec << std::endl;
                        }
                        }
                    }

                    // Version query: answer with the emulated IOP module version so
                    // the guest's module/version check (FUN_0011cfe0) accepts the
                    // service instead of retrying the whole bind+call sequence.
                    if (rpcNumber == kSifRpcGetVersion && sendSize == 0u && recvSize >= 4u)
                    {
                        runtime->Store32(rdram, ctx, receiveAddr, kSifIopModuleVersion);
                    }

                    // Deci2/dbcman version query (see kDbcManReportedVersion): the
                    // guest compares the answer against its own libdbc version and
                    // stalls when it mismatches, so report the 3.10 it expects.
                    if (rpcNumber == kDbcManRpcCheckVersion && recvSize >= 16u)
                    {
                        for (uint32_t offset = 0u; offset < 16u; offset += 4u)
                        {
                            runtime->Store32(rdram, ctx, receiveAddr + offset, kDbcManReportedVersion);
                        }
                    }
                }

                uint32_t reply[12] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
                reply[0] = 0x30u;          // psize = 0x30, dsize = 0
                reply[2] = kSifCmdRpcEnd;  // cid   = 80000008h
                reply[4] = recId;          // rec_id
                reply[5] = clientPktAddr;  // pkt_addr
                reply[6] = rpcId;          // rpc_id
                reply[7] = clientPtr;      // client
                reply[8] = kSifCmdRpcCall; // request this END finishes
                reply[9] = server;         // server

                const bool posted = postIopSifCommand(rdram, ctx, runtime, reply, 12u);
                std::cerr << "[SIF-iop] RPC CALL client=0x" << std::hex << clientPtr
                          << (posted ? " -> RPC END posted" : " -> RPC END FAILED")
                          << std::dec << std::endl;
                return;
            }

            const uint32_t sid = runtime->Load32(rdram, ctx, packet + 0x20u);
            const uint32_t server = sifRpcServerAddr(runtime, sid);
            const uint32_t clientSema = runtime->Load32(rdram, ctx, clientPtr + 0x08u);

            // Remember what this client bound so a later call can be routed to
            // the emulated IOP when it (or an HLE service) owns the sid.
            g_sifRpcClientSid[clientPtr] = sid;

            // SifRpcRendPkt: {SifCmdHeader sifcmd; rec_id; pkt_addr; rpc_id;
            //                 SifRpcClientData *client; u32 cid;
            //                 SifRpcServerData *server; void *buff, *cbuff;}
            uint32_t reply[12] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
            // The SIF header's first word is the packet size in bytes, and the
            // receiver copies ceil(psize/16) quadwords (see _SifCmdIntHandler at
            // 0x117ad4/0x117af0).  The whole 0x30-byte SifRpcRendPkt has to be
            // declared, otherwise pkt+0x1c..0x28 are never copied and the END
            // handler reads stale stack instead of the bind answer.
            reply[0] = 0x30u;          // psize = 0x30, dsize = 0
            reply[2] = kSifCmdRpcEnd;  // cid   = 80000008h
            reply[4] = recId;          // rec_id
            reply[5] = clientPktAddr;  // pkt_addr
            reply[6] = rpcId;          // rpc_id
            reply[7] = clientPtr;      // client
            reply[8] = kSifCmdRpcBind; // request this END finishes
            reply[9] = server;         // server -> client->server
            reply[10] = 0u;            // buff   -> client->buf

            const uint32_t preServer = runtime->Load32(rdram, ctx, clientPtr + 0x24u);
            g_lastRpcClientPtr = clientPtr;
            g_lastRpcServerWanted = server;

            // TEMP DIAGNOSTIC: did the guest's own _SifCmdIntHandler consume the
            // previous reply?  It clears pktbuf[0] as it dequeues the command.
            uint32_t pkt0Before = 0u;
            {
                const uint32_t cdAddr = sifCmdDataAddr();
                if (cdAddr != 0u)
                {
                    pkt0Before = runtime->Load32(rdram, ctx, runtime->Load32(rdram, ctx, cdAddr));
                }
            }

            const bool posted = postIopSifCommand(rdram, ctx, runtime, reply, 12u);
            std::cerr << "[SIF-iop] RPC BIND sid=0x" << std::hex << sid
                      << " pre_server=0x" << preServer
                      << " pkt0_before=0x" << pkt0Before
                      << " rec_id=" << std::dec << recId
                      << " client=0x" << std::hex << clientPtr
                      << " sema=" << static_cast<int32_t>(clientSema)
                      << " server=0x" << server
                      << (posted ? " -> RPC END posted" : " -> RPC END FAILED") << std::dec << std::endl;
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            // TEMP DIAGNOSTIC (session 11): every EE-side application of an IOP ->
            // EE copy, with a dedicated report for the 0x3943xx handshake block.
            {
                static std::mutex watchMutex;
                static uint32_t watchTotal = 0u;
                std::lock_guard<std::mutex> lock(watchMutex);
                ++watchTotal;
                const uint64_t lo = address;
                const uint64_t hi = static_cast<uint64_t>(address) + sizeBytes;
                const bool inBlock = (hi > 0x394300ull && lo < 0x394400ull);
                if (inBlock || watchTotal <= 60u)
                {
                    std::ostringstream out;
                    out << "[WATCH-SIFW] #" << watchTotal << " addr=0x" << std::hex << address
                        << " size=0x" << sizeBytes
                        << (inBlock ? " <== HANDSHAKE BLOCK" : "") << std::dec;
                    std::cerr << out.str() << std::endl;
                }
            }
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifRpcClientSid.clear();
        g_sifIopCatchUpTriedSids.clear();
        g_sifRpcPayloadPrecedingEeAddr = 0u;
        g_sifRpcPayloadPrecedingSize = 0u;
        g_sifRpcPayloadFollowingEeAddr = 0u;
        g_sifRpcPayloadFollowingSize = 0u;
        seedDefaultSifRegsLocked();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->guestMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->guestFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(packetAddress);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->guestFree(packetAddress);
            return false;
        }
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(reqSize, 64u) : 0u);
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(size, 64u) : 0u);
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        std::vector<uint8_t> payload(size);
        if (!runtime || !runtime->isIopMemoryRange(srcAddr, size) ||
            !canAccessEeRange(rdram, dstAddr, size) ||
            !runtime->readIopMemory(srcAddr, payload.data(), payload.size()) ||
            !writeEeRange(rdram, dstAddr, payload.data(), size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::AfterCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            // The emulated IOP reports its own boot progress: once the game has
            // handed it the module list (Reboot IOP) the DMA, SIFCMD and IOP boot
            // milestones all hold, so SMFLG reads them back set.  The game spin
            // waits on the 0x40000 "IOP finished booting" bit before it will
            // leave its SIF bring-up loop.
            if (reg == kSifRegSmflg && g_sifIopBooted)
            {
                value |= kSifSmflgBootedMask;
                g_sifRegs[reg] = value;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        // TEMP DIAGNOSTIC (black-content triage)
        std::cerr << "[SIF-SetDma] pc=0x" << std::hex << (ctx ? ctx->pc : 0u)
                  << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                  << " dmat=0x" << dmatAddr
                  << " count=" << std::dec << count << std::endl;
        for (uint32_t _di = 0u; _di < count && _di < 8u; ++_di)
        {
            const uint32_t _entAddr = dmatAddr + _di * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer));
            const uint8_t *_ent = getConstMemPtr(rdram, _entAddr);
            if (_ent)
            {
                uint32_t _w[4] = {0u, 0u, 0u, 0u};
                std::memcpy(_w, _ent, sizeof(_w));
                std::cerr << "[SIF-SetDma]   ent" << _di << " w0=0x" << std::hex << _w[0]
                          << " w1=0x" << _w[1] << " w2=0x" << _w[2] << " w3=0x" << _w[3]
                          << std::dec << std::endl;
                const uint8_t *_pkt = getConstMemPtr(rdram, _w[0]);
                if (_pkt && _w[2] >= 16u)
                {
                    const uint32_t _n = _w[2] >= 40u ? 40u : (_w[2] & ~3u);
                    uint32_t _h[10] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
                    if (_n > 0u)
                    {
                        std::memcpy(_h, _pkt, _n);
                    }
                    std::cerr << "[SIF-SetDma]   pkt n=" << _n << std::hex;
                    for (uint32_t _i = 0u; _i < (_n / 4u); ++_i)
                    {
                        std::cerr << " +" << (_i * 4u) << "=0x" << _h[_i];
                    }
                    std::cerr << std::dec << std::endl;
                }
            }
        }

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!runtime || !canAccessEeRange(rdram, xfer.src, sizeBytes) || !runtime->isIopMemoryRange(xfer.dest, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            // Track each command packet's sibling descriptors so the responder
            // can locate an RPC call's send payload (see g_sifRpcPayload*).
            uint32_t previousEntrySrc = 0u;
            uint32_t previousEntrySize = 0u;
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                std::vector<uint8_t> payload(sizeBytes);
                if (!readEeRange(rdram, xfer.src, payload.data(), sizeBytes) || !runtime->writeIopMemory(xfer.dest, payload.data(), payload.size()))
                {
                    ok = false;
                    break;
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::AfterCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                g_sifRpcPayloadPrecedingEeAddr = previousEntrySrc;
                g_sifRpcPayloadPrecedingSize = previousEntrySize;
                g_sifRpcPayloadFollowingEeAddr =
                    (i + 1u < pendingCount) ? pending[i + 1u].src : 0u;
                g_sifRpcPayloadFollowingSize =
                    (i + 1u < pendingCount) ? static_cast<uint32_t>(pending[i + 1u].size) : 0u;

                // The emulated IOP answers the SIFCMD initialization commands
                // it just received (see emulateIopSifCmdInitHandshake).
                emulateIopSifCmdInitHandshake(rdram, ctx, runtime, xfer);
                dumpSifCmdHandlersOnce(rdram, ctx, runtime, xfer);
                emulateIopSifRpc(rdram, ctx, runtime, xfer);

                previousEntrySrc = xfer.src;
                previousEntrySize = sizeBytes;
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        std::cerr << "[SIF-SetDma] ok=1 pending=" << pendingCount << std::dec << std::endl;
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        // TEMP DIAGNOSTIC (raw-SIFRPC bridge): what the guest's own handlers
        // made of the reply we just posted.
        if (g_lastRpcClientPtr != 0u)
        {
            const uint32_t postServer = runtime->Load32(rdram, ctx, g_lastRpcClientPtr + 0x24u);
            const uint32_t postSema = runtime->Load32(rdram, ctx, g_lastRpcClientPtr + 0x08u);
            uint32_t pkt0 = 0u;
            const uint32_t cmdData = sifCmdDataAddr();
            if (cmdData != 0u)
            {
                pkt0 = runtime->Load32(rdram, ctx, runtime->Load32(rdram, ctx, cmdData));
            }
            std::cerr << "[SIF-iop] post-bind client=0x" << std::hex << g_lastRpcClientPtr
                      << " server=0x" << postServer << " want=0x" << g_lastRpcServerWanted
                      << " pkt0=0x" << pkt0 << " sema=" << std::dec
                      << static_cast<int32_t>(postSema) << std::endl;
            g_lastRpcClientPtr = 0u;
        }

        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId()));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}
