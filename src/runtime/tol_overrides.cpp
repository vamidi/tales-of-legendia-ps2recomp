#include "tol_overrides.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <vector>

// Host-level scheduler counters, defined in EeScheduler.cpp at global scope. Reading them from a probe
// distinguishes "the guest thread is still being entered" from "the executor loop stopped turning".
extern std::atomic<uint64_t> g_eeRunLoopIterations;
extern std::atomic<uint64_t> g_eeProcessPendingEventsCount;
extern std::atomic<uint64_t> g_eeEnterGuestCount;
extern uint32_t g_lastDispatchPcs[32];
extern uint32_t g_lastDispatchCount;
extern int32_t g_lastDispatchTids[32];
extern uint32_t g_callRingSrc[16];
extern uint32_t g_callRingTgt[16];
extern uint32_t g_callRingCount;

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    // Shared timeline counters. Every probe stamps these, so submits, polls and frames sit on one axis.
    uint32_t gFrameTicks = 0u;

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
    constexpr uint32_t kLoadFileArgsOffset = 8u + 0xFCu;

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
    // The server is SECTOR-addressed, not handle-addressed. Its open returns a BASE SECTOR (word 0 of the
    // reply) and every read names an absolute sector, `base + position`; the ADXF/CRI file device does exactly
    // that arithmetic itself (FUN_001AD850: `offset = entry[+0x20] + entry[+0xC]`, then FUN_001ab4f8 passes it
    // as the first argument of FUN_001029a8). Handing out consecutive small handles made `base + position`
    // walk into the NEXT file's handle (2 -> 3 -> 4 ...), so block 1 of an archive was served from another
    // archive and block 3 fell off the table entirely, which the SJRBF request machine reports as state 9 and
    // the DVCI completion poll latches as device state 3 - the "stream never completes" wall.
    //
    // Bases are therefore one stride apart. The stride must exceed the largest archive in sectors
    // (MOVIE.AFS = 436,462), so 0x100000 sectors (2 GiB) is comfortable and 32 slots cannot overflow.
    constexpr uint32_t kGameFileBaseStride = 0x00100000u;
    uint32_t gGameFileHandle[kMaxGameFiles] = {};
    std::FILE *gGameFileStream[kMaxGameFiles] = {};
    uint64_t gGameFileSize[kMaxGameFiles] = {};

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
                gGameFileHandle[i] = static_cast<uint32_t>(i + 1) * kGameFileBaseStride;
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
                    std::fprintf(stderr, "[fileio] gamefile open '%s' -> '%s' ok, %llu bytes (base sector 0x%X)\n",
                                 guestPath.c_str(), hostPath.c_str(),
                                 static_cast<unsigned long long>(gGameFileSize[i]), gGameFileHandle[i]);
                return gGameFileHandle[i];
            }
        }
        std::fclose(stream);
        return 0u;
    }

    // The game's file *data* service (sid 0x80000595, client 0x3863D0): FUN_00101d80 binds it and waits
    // for its bound flag, FUN_00101ed8 asks it for state (fn 0xE), and FUN_001029a8 reads through it
    // (fn 1, mode 1, request {handle, blocks, buffer, info[3], desc, countOut}).
    //
    // The real service answers asynchronously through an end callback whose reply descriptor layout is not
    // derivable from this side, so the read is completed in place instead: the bytes land directly in the
    // caller's cached buffer (where DVCI parses from), the count-out word the request points at is filled,
    // and the busy flag the poll loop watches is cleared - the state the completion would leave behind.
    constexpr uint32_t kGameDataSid = 0x80000595u;
    constexpr uint32_t kGameDataStateFunction = 0x0Eu;
    constexpr uint32_t kGameDataReadFunction = 0x1u;
    constexpr uint32_t kGameDataBusyFlag = 0x00385234u;
    // The I/O mutex the open and read paths take with PollSema. FUN_001029a8 takes it and leaves it held
    // for the completion to release, so a read that never completes also never unlocks the file layer.
    constexpr uint32_t kGameIoSema = 0x00385228u;
    constexpr uint32_t kGameDataBlockShift = 11u;  // blocks << 11 for info[2] == 0
    constexpr uint32_t kGameDataBlockStrideA = 0x918u;
    constexpr uint32_t kGameDataBlockStrideB = 0x924u;

    int gameFileSlot(uint32_t handle)
    {
        for (int i = 0; i < kMaxGameFiles; ++i)
        {
            if (gGameFileHandle[i] == handle)
                return i;
        }
        return -1;
    }

    void completeGameIo(PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t countOut, uint32_t bytes)
    {
        if (countOut != 0u)
        {
            runtime->Store32(rdram, ctx, countOut, bytes);
        }
        runtime->Store32(rdram, ctx, kGameDataBusyFlag, 0u);
    }

    // Releases the I/O mutex the read path left held, the way its completion callback would.
    void releaseGameIoLock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sema = runtime->Load32(rdram, ctx, kGameIoSema);
        if (sema == 0u)
            return;
        const uint32_t savedA0 = GPR_U32(ctx, 4);
        const uint32_t savedV0 = GPR_U32(ctx, 2);
        SET_GPR_U32(ctx, 4, sema);
        ps2_syscalls::SignalSema(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 4, savedA0);
        SET_GPR_U32(ctx, 2, savedV0);
    }

    // FUN_00118828 (the guest's libsif "is this client busy?" check) reads client[0] as a header pointer and
    // tests header[0x10] bit 0. The native overrides below replace the guest's sceSifCallRpc, so nothing
    // clears that bit after a completed read; the CRI completion poll then sleeps DelayThread(4000) forever.
    // Clear it on the same path that already completes the request.
    void clearGuestRpcBusy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t client)
    {
        if (client < 0x00100000u || client >= 0x02000000u)
            return;
        const uint32_t header = runtime->Load32(rdram, ctx, client);
        if (header < 0x00100000u || header >= 0x02000000u)
            return;
        const uint32_t flags = runtime->Load32(rdram, ctx, header + 0x10u);
        runtime->Store32(rdram, ctx, header + 0x10u, flags & ~1u);
    }

    uint32_t gGameFileReadCount = 0u;

    // fn 1: {handle, blocks, buffer, info[0], info[1], info[2], desc, countOut}
    bool gameFileRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t request)
    {        if (request == 0u)
            return false;
        const bool timing = std::getenv("TOL_TRACE_FILEIO_TIME") != nullptr;
        const auto entryTime = std::chrono::steady_clock::now();
        const uint32_t sector = runtime->Load32(rdram, ctx, request + 0x00u);
        const uint32_t blocks = runtime->Load32(rdram, ctx, request + 0x04u);
        const uint32_t buffer = runtime->Load32(rdram, ctx, request + 0x08u);
        const uint8_t blockMode = runtime->Load8(rdram, ctx, request + 0x0Eu);
        const uint32_t countOut = runtime->Load32(rdram, ctx, request + 0x14u);

        // The first word is an absolute sector: the open's base plus the caller's position. Resolve it to the
        // file whose [base, base + size) contains it, and read from that sector onward.
        int slot = -1;
        uint32_t sectorOffset = 0u;
        for (int i = 0; i < kMaxGameFiles; ++i)
        {
            if (gGameFileHandle[i] == 0u)
                continue;
            const uint32_t base = gGameFileHandle[i];
            const uint64_t sectors = (gGameFileSize[i] + 0x7FFu) >> 11;
            if (sector >= base && static_cast<uint64_t>(sector - base) < (sectors == 0u ? 1u : sectors))
            {
                slot = i;
                sectorOffset = sector - base;
                break;
            }
        }
        if (slot < 0 || buffer == 0u)
        {
            if (std::getenv("TOL_TRACE_DVCI") != nullptr || std::getenv("TOL_TRACE_FILEIO") != nullptr)
            {
                std::fprintf(stderr, "[fileio] gamefile read sector 0x%X REJECTED (no file, buffer=0x%08X)\n", sector,
                             buffer);
            }
            return false;
        }

        uint32_t size = blocks << kGameDataBlockShift;
        if (blockMode == 1u)
            size = blocks * kGameDataBlockStrideA;
        else if (blockMode >= 2u)
            size = blocks * kGameDataBlockStrideB;

        const uint64_t offset = static_cast<uint64_t>(sectorOffset) << kGameDataBlockShift;
        const uint32_t remaining =
            gGameFileSize[slot] > offset ? static_cast<uint32_t>(gGameFileSize[slot] - offset) : 0u;
        if (size > remaining)
            size = remaining;

        // Reuse one buffer: this is called per sector during streaming, and a fresh vector per 2 KiB block is
        // pure allocator churn.
        static thread_local std::vector<uint8_t> data;
        data.resize(size);
        size_t read = 0u;
        const auto beforeRead = std::chrono::steady_clock::now();
        if (size != 0u)
        {
            if (std::fseek(gGameFileStream[slot], static_cast<long>(offset), SEEK_SET) != 0)
                return false;
            read = std::fread(data.data(), 1, size, gGameFileStream[slot]);
        }
        const auto afterRead = std::chrono::steady_clock::now();
        // The streaming buffer is ordinary EE RAM; copy it in one shot instead of 2048 Store8 calls.
        const uint32_t physical = buffer & 0x1FFFFFFFu;
        const bool bulkCopy = read != 0u && physical < 0x02000000u && read <= 0x02000000u - physical;
        if (bulkCopy)
        {
            std::memcpy(rdram + physical, data.data(), read);
        }
        else
        {
            for (size_t i = 0; i < read; ++i)
            {
                runtime->Store8(rdram, ctx, buffer + static_cast<uint32_t>(i), data[i]);
            }
        }
        const auto afterCopy = std::chrono::steady_clock::now();
        completeGameIo(runtime, rdram, ctx, countOut, static_cast<uint32_t>(read));
        releaseGameIoLock(rdram, ctx, runtime);
        ++gGameFileReadCount;
        if (timing)
        {
            static uint32_t sTimingLogs = 0u;
            if (sTimingLogs < 64u)
            {
                ++sTimingLogs;
                auto us = [](auto from, auto to)
                { return std::chrono::duration<double, std::micro>(to - from).count(); };
                std::fprintf(stderr,
                             "[fileio-time] sector=0x%X bytes=%zu read=%.1fus copy=%.1fus total=%.1fus\n", sector,
                             read, us(beforeRead, afterRead), us(afterRead, afterCopy), us(entryTime, afterCopy));
            }
        }
        if (std::getenv("TOL_TRACE_FILEIO") != nullptr)
        {
            std::fprintf(stderr, "[fileio] gamefile read sector 0x%X (file slot %d +%u) %zu/%u bytes at 0x%08X\n",
                         sector, slot, sectorOffset, read, size, buffer);
        }
        return true;
    }

    // IxAudio's server, sid 0x90000200. IXAUDIO.IRX registers it, but its DTX create (fn 0x002) and PS2RNA
    // create (fn 0x408) return 0 unless the module's full state machine has been driven, and the game uses
    // those replies as object handles. Supply the handles the game's SJX/CRI layers need; the rest reply 0.
    // Audio is not produced, but the loading sequence no longer dead-ends on a zero handle.
    constexpr uint32_t kIxAudioSid = 0x90000200u;
    constexpr uint32_t kIxAudioHandleFunctions[] = {0x002u, 0x400u, 0x408u, 0x422u};
    uint32_t gIxAudioNextHandle = 1u;

    // The pad/memory-card service the init step polls used to be answered natively (sid 0x80000593). It is
    // the game's cdvd/pad service and is now served by cdvdfsv on the IOP; see tolSifCallRpc.

    // ioman (sid 0x80000001): the game's sceSifInitIoman (FUN_00119568) binds it and sends fn 0xFF with an
    // 8-byte request of two pointers, expecting an 8-byte reply. The IOPRP image's IOMAN module does not
    // register the server here (it wants the real sifman), so the init is answered natively. Reply word 1 is
    // compared against 2 to enable a newer capability path, which this runtime does not provide.
    constexpr uint32_t kIomanSid = 0x80000001u;
    constexpr uint32_t kIomanInitFunction = 0xFFu;
    // Version string the game accepts ("3000", the IOP image it boots); "...." is the other known value.
    constexpr uint32_t kIomanVersion3000 = 0x30303033u;

    // IOP->EE SIF command delivery for this title.
    //
    // The game carries its own libsif: sceSifInit (FUN_00117540) sets up a receive buffer whose first byte is
    // the packet size and registers FUN_00117AA8 as the DMAC channel-5 handler that drains it and dispatches
    // by cid through the table at 0x3DEDC0. Nothing in this runtime ever produced such a command, so every
    // handler the game registers through its own sceSifAddCmdHandler was dead code - including ioman's
    // 0x80000011 (request completion) and 0x80000013 (registered callback dispatch).
    constexpr uint32_t kSifRxBuffer = 0x003DECC0u;
    constexpr uint32_t kSifRxHandler = 0x00117AA8u;
    constexpr uint32_t kSifCmdHeaderSize = 0x10u;

    // Builds one SIF command packet in the guest's receive buffer and runs the guest's receive handler, the
    // way the DMAC interrupt would. Returns false when the title's SIF layer is not up yet.
    bool deliverSifCommand(uint8_t *rdram,
                           R5900Context *ctx,
                           PS2Runtime *runtime,
                           uint32_t cid,
                           const uint32_t *payload,
                           uint32_t payloadBytes)
    {
        PS2Runtime::RecompiledFunction handler = runtime->lookupFunction(kSifRxHandler);
        if (handler == nullptr)
        {
            return false;
        }
        // The buffer pointer lives at 0x3DED98; sceSifInit writes it. Zero means the layer is not up.
        const uint32_t buffer = runtime->Load32(rdram, ctx, 0x003DED98u);
        if (buffer == 0u)
        {
            return false;
        }
        const uint32_t total = kSifCmdHeaderSize + payloadBytes;
        runtime->Store32(rdram, ctx, buffer + 0x00u, total); // size; the low byte is what the handler reads
        runtime->Store32(rdram, ctx, buffer + 0x04u, 0u);    // dest
        runtime->Store32(rdram, ctx, buffer + 0x08u, cid);
        runtime->Store32(rdram, ctx, buffer + 0x0Cu, 0u);
        for (uint32_t index = 0; index < payloadBytes; ++index)
        {
            const uint32_t word = payload[index / 4u];
            runtime->Store8(rdram, ctx, buffer + kSifCmdHeaderSize + index,
                            static_cast<uint8_t>((word >> ((index % 4u) * 8u)) & 0xFFu));
        }
        handler(rdram, ctx, runtime);
        return true;
    }

    // ioman file calls (sid 0x80000001), completed synchronously.
    //
    // The game's wrappers are asynchronous by design: the RPC returns a status, then the IOP sends SIF command
    // 0x80000011 whose handler fills the request record and signals the semaphore the caller is about to wait
    // on. Doing the I/O here, writing the result where the caller reads it and signalling that semaphore gives
    // the caller exactly the same values without the round trip.
    //
    //   fn 0 open   0x418 bytes {sema, resultPtr, 4, flags, mode, path[0x400]}  -> result[0] = fd
    //   fn 1 close  0x14  bytes {sema, resultPtr, 4, fd}                        -> result[0] = 0
    //   fn 2 read   0x20  bytes {sema, resultPtr, 4, fd, buffer, size}          -> result[0] = bytes read
    //   fn 4 lseek  0x1C  bytes {sema, resultPtr, 4, fd, offset, whence}        -> result[0] = position
    constexpr uint32_t kIomanOpen = 0u;
    constexpr uint32_t kIomanClose = 1u;
    constexpr uint32_t kIomanRead = 2u;
    constexpr uint32_t kIomanLseek = 4u;
    // The result pointer sits at +0x04 (the request words are sema, resultPtr, 4, fd, ...); writing it to
    // +0x08 instead put the fd at address 4 and the caller read garbage from its own stack.
    constexpr uint32_t kIomanResultOffset = 0x04u;
    constexpr uint32_t kIomanFdOffset = 0x0Cu;
    constexpr uint32_t kIomanBufferOffset = 0x10u;
    constexpr uint32_t kIomanSizeOffset = 0x14u;
    constexpr uint32_t kIomanPathOffset = 0x14u;

    constexpr int kMaxIomanFiles = 16;
    uint32_t gIomanFd[kMaxIomanFiles] = {};
    std::FILE *gIomanStream[kMaxIomanFiles] = {};
    uint64_t gIomanSize[kMaxIomanFiles] = {};
    uint32_t gIomanNextFd = 3u; // 0..2 are the standard streams

    int iomanSlot(uint32_t fd)
    {
        for (int i = 0; i < kMaxIomanFiles; ++i)
        {
            if (gIomanFd[i] == fd)
                return i;
        }
        return -1;
    }

    void signalGuestSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t sema)
    {
        if (sema == 0u || sema > 0xFFFFu)
            return;
        const uint32_t savedA0 = GPR_U32(ctx, 4);
        const uint32_t savedV0 = GPR_U32(ctx, 2);
        SET_GPR_U32(ctx, 4, sema);
        ps2_syscalls::SignalSema(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 4, savedA0);
        SET_GPR_U32(ctx, 2, savedV0);
    }

    // Runs one ioman call. Returns false when the function is not one of the four above.
    bool handleIomanFileCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t function,
                             uint32_t request, uint32_t receiveBuffer, uint32_t receiveSize)
    {
        if (request == 0u)
            return false;
        const uint32_t sema = runtime->Load32(rdram, ctx, request);
        const uint32_t resultPtr = runtime->Load32(rdram, ctx, request + kIomanResultOffset);
        const uint32_t fd = runtime->Load32(rdram, ctx, request + kIomanFdOffset);
        int32_t result = -1;

        if (function == kIomanOpen)
        {
            std::string guestPath;
            for (uint32_t i = 0; i < 0x400u; ++i)
            {
                const uint8_t byte = runtime->Load8(rdram, ctx, request + kIomanPathOffset + i);
                if (byte == 0u)
                    break;
                guestPath.push_back(static_cast<char>(byte));
            }
            const std::string hostPath = guestPath.empty() ? std::string() : translatePs2Path(guestPath.c_str());
            std::FILE *stream = hostPath.empty() ? nullptr : std::fopen(hostPath.c_str(), "rb");
            if (stream != nullptr)
            {
                for (int i = 0; i < kMaxIomanFiles; ++i)
                {
                    if (gIomanFd[i] == 0u)
                    {
                        gIomanFd[i] = gIomanNextFd++;
                        gIomanStream[i] = stream;
                        gIomanSize[i] = 0u;
                        if (std::fseek(stream, 0, SEEK_END) == 0)
                        {
                            const long size = std::ftell(stream);
                            if (size > 0)
                                gIomanSize[i] = static_cast<uint64_t>(size);
                        }
                        std::fseek(stream, 0, SEEK_SET);
                        result = static_cast<int32_t>(gIomanFd[i]);
                        break;
                    }
                }
                if (result < 0)
                    std::fclose(stream);
            }
            if (std::getenv("TOL_TRACE_FILEIO") != nullptr)
            {
                std::fprintf(stderr, "[fileio] ioman open '%s' -> '%s' %s (fd %d)\n", guestPath.c_str(),
                             hostPath.c_str(), result >= 0 ? "ok" : "FAILED", result);
            }
        }
        else if (function == kIomanClose)
        {
            const int slot = iomanSlot(fd);
            if (slot >= 0)
            {
                std::fclose(gIomanStream[slot]);
                gIomanFd[slot] = 0u;
                gIomanStream[slot] = nullptr;
                result = 0;
            }
        }
        else if (function == kIomanRead)
        {
            const int slot = iomanSlot(fd);
            const uint32_t buffer = runtime->Load32(rdram, ctx, request + kIomanBufferOffset);
            const uint32_t size = runtime->Load32(rdram, ctx, request + kIomanSizeOffset);
            if (slot >= 0 && buffer != 0u && size != 0u)
            {
                std::vector<uint8_t> data(size);
                const size_t read = std::fread(data.data(), 1, size, gIomanStream[slot]);
                for (size_t i = 0; i < read; ++i)
                {
                    runtime->Store8(rdram, ctx, buffer + static_cast<uint32_t>(i), data[i]);
                }
                result = static_cast<int32_t>(read);
                if (std::getenv("TOL_TRACE_FILEIO") != nullptr)
                {
                    static uint32_t sReads = 0u;
                    if (sReads < 32u)
                    {
                        ++sReads;
                        std::fprintf(stderr, "[fileio] ioman read fd=%u buf=0x%08X size=%u -> %zu\n", fd, buffer,
                                     size, read);
                    }
                }
            }
            else if (slot >= 0)
            {
                result = 0;
            }
            else if (std::getenv("TOL_TRACE_FILEIO") != nullptr)
            {
                // A read on a handle this table never issued: the ADXF/CRI reader opens through the game's own
                // file server (sid 0x80000597), whose handles live in a different table, so answering -1 here
                // makes the container read return no bytes and the AFS header check fail downstream.
                static uint32_t sUnknown = 0u;
                if (sUnknown < 32u)
                {
                    ++sUnknown;
                    std::fprintf(stderr, "[fileio] ioman read fd=%u -> unknown fd (not opened here)\n", fd);
                }
            }
        }
        else if (function == kIomanLseek)
        {
            const int slot = iomanSlot(fd);
            const int32_t offset = static_cast<int32_t>(runtime->Load32(rdram, ctx, request + kIomanBufferOffset));
            const uint32_t whence = runtime->Load32(rdram, ctx, request + kIomanSizeOffset);
            if (slot >= 0)
            {
                const int origin = whence == 1u ? SEEK_CUR : (whence == 2u ? SEEK_END : SEEK_SET);
                if (std::fseek(gIomanStream[slot], offset, origin) == 0)
                {
                    const long position = std::ftell(gIomanStream[slot]);
                    result = position < 0 ? -1 : static_cast<int32_t>(position);
                }
            }
            if (std::getenv("TOL_TRACE_FILEIO") != nullptr)
            {
                static uint32_t sSeeks = 0u;
                if (sSeeks < 32u)
                {
                    ++sSeeks;
                    std::fprintf(stderr, "[fileio] ioman lseek fd=%d off=%d whence=%u -> %d%s\n", static_cast<int>(fd),
                                 static_cast<int>(offset), whence, result, slot >= 0 ? "" : " (unknown fd)");
                }
            }
        }
        else
        {
            return false;
        }

        if (resultPtr != 0u)
        {
            runtime->Store32(rdram, ctx, resultPtr, static_cast<uint32_t>(result));
        }
        if (receiveBuffer != 0u && receiveSize >= 4u)
        {
            runtime->Store32(rdram, ctx, receiveBuffer, 1u); // accepted: the caller waits on the semaphore
        }
        signalGuestSema(rdram, ctx, runtime, sema);
        return true;
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
    int32_t loadModuleFromGuestMemory(uint8_t *rdram,
                                      R5900Context *ctx,
                                      PS2Runtime *runtime,
                                      uint32_t pathAddress,
                                      uint32_t argLength,
                                      uint32_t argsAddress)
    {
        const uint32_t savedA0 = GPR_U32(ctx, 4);
        const uint32_t savedA1 = GPR_U32(ctx, 5);
        const uint32_t savedA2 = GPR_U32(ctx, 6);
        const uint32_t savedV0 = GPR_U32(ctx, 2);

        SET_GPR_U32(ctx, 4, pathAddress);
        SET_GPR_U32(ctx, 5, argLength);
        SET_GPR_U32(ctx, 6, argsAddress);
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

    void tolSifCallRpcBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
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
                // the start of the send buffer (the game copies it to 0x3E2208 and sends from 0x3E2200),
                // and the module arguments follow the path. Dropping them loads modules unconfigured.
                const uint32_t pathAddress = sendBuffer + kLoadFilePathOffset;
                const uint32_t argLength = runtime->Load32(rdram, ctx, sendBuffer);
                const uint32_t argsAddress = sendBuffer + kLoadFileArgsOffset;
                const int32_t loaded =
                    loadModuleFromGuestMemory(rdram, ctx, runtime, pathAddress, argLength, argsAddress);
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
        if (sid == kGameFileSid)        {
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
                    // The IOP writes the handle into the request's first word and answers 1 for success -
                    // the caller tests the reply against exactly 1 (FUN_001ae288), which is why the first
                    // file opened "by accident" when its handle happened to be 1.
                    //
                    // The response is a {handle, size} pair: FUN_00101a60 copies response+0x00..0x07 into
                    // the caller's first 8-byte word, and the CRI file device open (0x1AD5A0) reads
                    // `s0+0x24` - i.e. response+0x04 - as the file size in BYTES and stores
                    // `(size + 0x7FF) >> 11` as the handle's sector count. Leaving it zero is what made
                    // every AFS read see "0 sectors remaining" (session 65's seek(END) shim).
                    // dvCiGetFile (FUN_001ae288) likewise returns {handle, size} to its caller.
                    runtime->Store32(rdram, ctx, sendBuffer, handle);
                    const int sizeSlot = gameFileSlot(handle);
                    if (sizeSlot >= 0)
                    {
                        runtime->Store32(rdram, ctx, sendBuffer + 4u,
                                         static_cast<uint32_t>(gGameFileSize[sizeSlot]));
                    }
                    if (receiveBuffer != 0u && receiveSize >= 4u)
                    {
                        runtime->Store32(rdram, ctx, receiveBuffer, 1u);
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
            SET_GPR_S32(ctx, 2, -1);
            return;
        }

        // The game's file data service: state query and reads.
        if (sid == kGameDataSid)        {
            if (function == kGameDataStateFunction)
            {
                if (receiveBuffer != 0u && receiveSize >= 4u)
                {
                    runtime->Store32(rdram, ctx, receiveBuffer, 0u);
                }
                clearGuestRpcBusy(rdram, ctx, runtime, client);
                SET_GPR_S32(ctx, 2, 0);
                return;
            }
            if (function == kGameDataReadFunction && gameFileRead(rdram, ctx, runtime, sendBuffer))
            {
                clearGuestRpcBusy(rdram, ctx, runtime, client);
                SET_GPR_S32(ctx, 2, 0);
                return;
            }
            // Unsupported: leave the caller's state consistent rather than half-done.
            completeGameIo(runtime, rdram, ctx, 0u, 0u);
            SET_GPR_S32(ctx, 2, -1);
            return;
        }

        // IxAudio: answer the DTX/PS2RNA lifecycle so the game's SJX_Init and CRI PS2RNA creation complete.
        if (sid == kIxAudioSid)
        {
            uint32_t reply = 0u;
            for (const uint32_t handleFunction : kIxAudioHandleFunctions)
            {
                if (function == handleFunction)
                {
                    reply = gIxAudioNextHandle++;
                }
            }
            if (receiveBuffer != 0u && receiveSize >= 4u)
            {
                runtime->Store32(rdram, ctx, receiveBuffer, reply);
            }
            if (traceRpcEnabled())
            {
                static uint32_t sLogs = 0u;
                if (sLogs < 32u)
                {
                    ++sLogs;
                    std::fprintf(stderr, "[rpc] ixaudio fn=0x%X -> reply=0x%08X\n", function, reply);
                }
            }
            if (std::getenv("TOL_TRACE_IXAUDIO") != nullptr)
            {
                static uint32_t sDetail = 0u;
                if (sDetail < 40u)
                {
                    ++sDetail;
                    std::string sent;
                    for (uint32_t index = 0u; index < sendSize && index < 32u; index += 4u)
                    {
                        char word[16];
                        std::snprintf(word, sizeof(word), " %08X",
                                      runtime->Load32(rdram, ctx, sendBuffer + index));
                        sent += word;
                    }
                    std::fprintf(stderr,
                                 "[ixaudio] fn=0x%X send=0x%08X/%u [%s ] reply=0x%08X pc=0x%08X ra=0x%08X\n",
                                 function, sendBuffer, sendSize, sent.c_str(), reply, ctx->pc,
                                 GPR_U32(ctx, 31));
                }
            }
            SET_GPR_S32(ctx, 2, 0);
            return;
        }


        // ioman: answer the init so sceSifInitIoman succeeds. Other ioman functions (open/read/...) report
        // "unsupported" rather than pretending, because the game's own file server (sid 0x80000597) is what
        // its DVCI layer actually reads through.
        if (sid == kIomanSid)
        {
            if (function == kIomanInitFunction)
            {
                // The reply's first word is the ioman version, and the game gates every file call on it:
                // FUN_00119770() memcmps it against "3000" and "...." and only lets a call through when it
                // matches one. Answering 0 made every open return -0x10004 and the loading sequence hang.
                if (receiveBuffer != 0u && receiveSize >= 4u)
                {
                    runtime->Store32(rdram, ctx, receiveBuffer, kIomanVersion3000);
                }
                if (receiveBuffer != 0u && receiveSize >= 8u)
                {
                    // Second word 2 would enable the slot-indexed completion path; keep the simpler one.
                    runtime->Store32(rdram, ctx, receiveBuffer + 4u, 0u);
                }
                if (traceRpcEnabled())
                {
                    std::fprintf(stderr, "[rpc] ioman init -> ok\n");
                }
                // The IOP's file layer signals readiness with command 0x80000013, whose handler calls the
                // callback the game registered (FUN_00119520 -> (*(code*)arg[0])(arg[1])). Deliver it so the
                // game's own notification path runs at least once.
                const bool delivered = deliverSifCommand(rdram, ctx, runtime, 0x80000013u, nullptr, 0u);
                if (traceRpcEnabled())
                {
                    std::fprintf(stderr, "[rpc] ioman ready notification -> %s\n",
                                 delivered ? "delivered" : "no SIF layer");
                }
                SET_GPR_S32(ctx, 2, 0);
                return;
            }
            if (traceRpcEnabled())
            {
                static uint32_t sLogs = 0u;
                if (sLogs < 32u)
                {
                    ++sLogs;
                    std::fprintf(stderr, "[rpc] ioman fn=0x%X -> unsupported\n", function);
                }
            }
            // The game's file wrappers are asynchronous: do the work here and signal their semaphore.
            if (handleIomanFileCall(rdram, ctx, runtime, function, sendBuffer, receiveBuffer, receiveSize))
            {
                SET_GPR_S32(ctx, 2, 0);
                return;
            }
            SET_GPR_S32(ctx, 2, -1);
            return;
        }

        // Pad / memory-card service (sid 0x80000593). This used to be answered natively because the IOPRP
        // modules were not relocation-loaded and cdvdfsv never registered a server. With the IOPRP slice now
        // carrying the relocation tables, cdvdfsv serves the sid itself (it is the game's cdvd/pad service),
        // and the cdvdman imports behind it are the LLE kernel's image-backed handlers. Do not intercept it:
        // a native reply here bypasses the real read path.

        ps2_syscalls::SifCallRpc(rdram, ctx, runtime);
    }

    // The PS2 ABI passes the eighth argument of sceSifCallRpc — the SIF RPC end
    // function — in $t3, with the end parameter as the first stack word. This
    // replacement answers the RPC inline (gameFileRead, loadfile, IxAudio, ...),
    // so the normal libsif reply path never runs and that end function would be
    // dropped. Completion-driven callers depend on it: libcdvd's async read
    // (FUN_001029A8 passes 0x1016D0) clears DAT_00385234 from its end function,
    // and without it the CRI read state machine waits forever, freezing the
    // scene/ADXF stream. Run the end function after the body on the scheduler,
    // exactly like the runtime's own SifCallRpc does for the reply callback.
    void tolSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t endFunction = GPR_U32(ctx, 11);
        const uint32_t sp = GPR_U32(ctx, 29);
        uint32_t endParameter = 0u;
        if (sp + 4u <= PS2_RAM_SIZE)
            endParameter = runtime->Load32(rdram, ctx, sp);

        tolSifCallRpcBody(rdram, ctx, runtime);

        if (endFunction == 0u || !runtime->hasFunction(endFunction))
            return;

        GuestInvocation callback{};
        callback.kind = GuestInvocationKind::RpcCallback;
        callback.context = *ctx;
        callback.context.pc = endFunction;
        SET_GPR_U32(&callback.context, 4, endParameter);
        SET_GPR_U32(&callback.context, 29, runtime->eeScheduler().invocationStackTop());
        SET_GPR_U32(&callback.context, 31, 0u);
        runtime->eeScheduler().queueInvocation(std::move(callback));

        // The libcdvd async read (client 0x3863D0) keeps DAT_00385234 ("read
        // pending") set until its completion runs. FUN_001014C0 only clears it
        // when the reply status is 0xB or the libsif mode flags allow it; the
        // HLE path bypasses the guest SIF reply handler that normally writes
        // those, so the flag can stay set and FUN_00101F70(1) keeps reporting
        // busy, freezing the CRI/ADXF read. This end function is the libcdvd
        // read completion, so the read is done: clear the flag here.
        if (endFunction == 0x001016D0u)
            runtime->Store32(rdram, ctx, 0x00385234u, 0u);
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
        // Count calls and report the argument registers: for a routine like memset(0x109108) that is what says
        // whether the guest is clearing a sane buffer, and the exit line says whether it came back at all.
        static uint32_t sCalls[kMaxPcProbes] = {};
        const uint32_t call = ++sCalls[idx];
        const bool log = call <= 40u || (call % 500u) == 0u;

        if (log)
        {
            std::fprintf(stderr,
                         "[pc] enter 0x%08X #%u a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X ra=0x%08X sp=0x%08X\n",
                         gPcProbeAddr[idx], call, GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6),
                         GPR_U32(ctx, 7), GPR_U32(ctx, 31), GPR_U32(ctx, 29));
        }
        gPcProbeOriginal[idx](rdram, ctx, runtime);
        if (log)
        {
            std::fprintf(stderr, "[pc] exit  0x%08X #%u pc=0x%08X ra=0x%08X v0=0x%08X\n", gPcProbeAddr[idx], call,
                         ctx->pc, GPR_U32(ctx, 31), GPR_U32(ctx, 2));
        }
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
    uint32_t gTickEntries = 0u;

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
        const uint32_t entry = ++gTickEntries;
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

    // ---------------------------------------------------------------------------------------------------------
    // DVCI / CRI ASYNC-LOAD PROBE (TOL_TRACE_DVCI=1)
    //
    // The loading sequence FUN_001664d0 starts six AFS loads and then waits on
    //
    //     while (FUN_00196460(i) != 3) FUN_0019a080();     // FUN_0019a080 is the CRI I/O pump
    //
    // so the whole screen advance depends on that poll reaching 3. This probe wraps the poll and dumps the state
    // block it reads. Every address below is taken from the disassembly of FUN_001961a8 (request), FUN_00196460
    // (poll) and FUN_00196f58 (read step), not from guesses:
    //
    //   0x3910A4 u32   progress: entry index inside the current request
    //   0x3910A8 u32   CRI handle record serving the request (a 0x238-byte slot in the table at 0x3A7FB0)
    //   0x3910AC s32   load index i in flight (-1 = idle)
    //   0x3910B0 u32   read buffer, malloc'd by the caller (the AFS path was memcpy'd to +4)
    //   0x3910B4 u32   block count (request size >> 11)
    //   0x390B78 u32[] per-index request records
    //
    // Handle record layout, from FUN_001b1690 / FUN_001b0d00: +0x01 u8 state (2 = read in flight, 3 = done),
    // +0x04 u8 flags, +0x08 u32 entry count, +0x20 u32 queue cursor, +0x24 u32 size, +0x28 u32 byte offset.
    // The addresses were confirmed against the disassembly of FUN_00196460 (`lui v0,0x39; lw v1,0x10ac(v0)`).
    constexpr uint32_t kDvciPollPc = 0x00196460u;
    constexpr uint32_t kDvciProgress = 0x003910A4u;
    constexpr uint32_t kDvciHandlePtr = 0x003910A8u;
    constexpr uint32_t kDvciLoadIndex = 0x003910ACu;
    constexpr uint32_t kDvciBuffer = 0x003910B0u;
    constexpr uint32_t kDvciBlocks = 0x003910B4u;
    constexpr uint32_t kDvciRequestTable = 0x00390B78u;
    constexpr uint32_t kCriHandleTable = 0x003A7FB0u;
    constexpr uint32_t kCriHandleStride = 0x238u;
    constexpr int kCriHandleCount = 32;
    constexpr uint32_t kCriDispatchTable = 0x0049C6A0u;
    constexpr uint32_t kCriDispatchStride = 0x30u;
    constexpr uint32_t kCriRequestPool = 0x003B1998u;
    constexpr uint32_t kCriRequestInUse = 0x003B199Cu;

    PS2Runtime::RecompiledFunction gDvciPollOriginal = nullptr;

    bool dvciTraceEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_DVCI") != nullptr;
        return enabled;
    }

    void dumpDvci(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t call, int32_t result)
    {
        const uint32_t loadIndex = runtime->Load32(rdram, ctx, kDvciLoadIndex);
        const uint32_t progress = runtime->Load32(rdram, ctx, kDvciProgress);
        const uint32_t handle = runtime->Load32(rdram, ctx, kDvciHandlePtr);
        const uint32_t buffer = runtime->Load32(rdram, ctx, kDvciBuffer);
        const uint32_t blocks = runtime->Load32(rdram, ctx, kDvciBlocks);

        std::fprintf(stderr,
                     "[dvci] #%u poll(i=%u) -> %d | pc=0x%08X ra=0x%08X | load=%d progress=%u handle=0x%08X "
                     "buf=0x%08X blocks=%u\n",
                     call, GPR_U32(ctx, 4), result, ctx->pc, GPR_U32(ctx, 31),
                     static_cast<int32_t>(loadIndex), progress, handle, buffer, blocks);

        if (handle >= 0x00100000u && handle < 0x02000000u)
        {
            std::fprintf(stderr, "[dvci]   handle:");
            for (uint32_t off = 0u; off <= 0x2Cu; off += 4u)
                std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, handle + off));
            std::fprintf(stderr, "\n");
            std::fprintf(stderr, "[dvci]   state=%u flags=%u type=%u\n",
                         static_cast<unsigned>(runtime->Load8(rdram, ctx, handle + 1u)),
                         static_cast<unsigned>(runtime->Load8(rdram, ctx, handle + 4u)),
                         static_cast<unsigned>(runtime->Load8(rdram, ctx, handle + 0x0Fu)));
        }

        if (buffer >= 0x00100000u && buffer < 0x02000000u)
        {
            char path[0x40] = {};
            for (uint32_t i = 0u; i < sizeof(path) - 1u; ++i)
            {
                const uint8_t byte = runtime->Load8(rdram, ctx, buffer + 4u + i);
                if (byte == 0u)
                    break;
                path[i] = static_cast<char>(byte);
            }
            std::fprintf(stderr, "[dvci]   request: +00=%08X +04=%08X +08=%08X +0C=%08X +110=%08X +114=%08X path='%s'\n",
                         runtime->Load32(rdram, ctx, buffer + 0x00u),
                         runtime->Load32(rdram, ctx, buffer + 0x04u),
                         runtime->Load32(rdram, ctx, buffer + 0x08u),
                         runtime->Load32(rdram, ctx, buffer + 0x0Cu),
                         runtime->Load32(rdram, ctx, buffer + 0x110u),
                         runtime->Load32(rdram, ctx, buffer + 0x114u), path);
        }

        // The 32 slots of the CRI handle table; only slots that look live are worth printing.
        for (int slot = 0; slot < kCriHandleCount; ++slot)
        {
            const uint32_t record = kCriHandleTable + static_cast<uint32_t>(slot) * kCriHandleStride;
            const uint32_t state = runtime->Load8(rdram, ctx, record + 1u);
            const uint32_t flags = runtime->Load8(rdram, ctx, record + 4u);
            const uint32_t size = runtime->Load32(rdram, ctx, record + 0x24u);
            if (state == 0u && flags == 0u && size == 0u)
                continue;
            std::fprintf(stderr,
                         "[dvci]   slot[%d] state=%u flags=%u entries=%u cursor=%u size=%u\n", slot,
                         static_cast<unsigned>(state), static_cast<unsigned>(flags),
                         runtime->Load32(rdram, ctx, record + 0x08u),
                         runtime->Load32(rdram, ctx, record + 0x20u), size);
        }

        // Raw heads: the device slot table the ADXF pump walks (0x3A7FB0, 0x238 per slot, live when byte 0
        // is 1), the six dispatch callbacks of pump slot 5 (0x49C6A0 + 5*0x30), and the CRI request pool at
        // 0x3B1998 (0x40 per record, in-use flag at 0x3B199C + i*0x10).
        if (call == 1u || call == 20000u)
        {
            for (int slot = 0; slot < 3; ++slot)
            {
                const uint32_t record = kCriHandleTable + static_cast<uint32_t>(slot) * kCriHandleStride;
                std::fprintf(stderr, "[dvci]   za7fb0[%d]:", slot);
                for (uint32_t off = 0u; off <= 0x1Cu; off += 4u)
                    std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, record + off));
                std::fprintf(stderr, "\n");
            }
            std::fprintf(stderr, "[dvci]   pump5 callbacks:");
            for (uint32_t i = 0u; i < 6u; ++i)
                std::fprintf(stderr, " %u=0x%08X", i,
                             runtime->Load32(rdram, ctx, kCriDispatchTable + 5u * kCriDispatchStride + i * 8u));
            std::fprintf(stderr, "\n");
            std::fprintf(stderr, "[dvci]   adxf pump: gate=0x%08X\n", runtime->Load32(rdram, ctx, 0x0039E838u));
            for (int slot = 0; slot < 6; ++slot)
            {
                const uint32_t record =
                    runtime->Load32(rdram, ctx, kDvciRequestTable + static_cast<uint32_t>(slot) * 4u);
                if (record < 0x00100000u || record >= 0x02000000u)
                {
                    std::fprintf(stderr, " [dvci]   reqrec[%d]=0x%08X (not a pointer)\n", slot, record);
                    continue;
                }
                std::fprintf(stderr, " [dvci]   reqrec[%d]=0x%08X:", slot, record);
                for (uint32_t off = 0u; off <= 0x1Cu; off += 4u)
                    std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, record + off));
                std::fprintf(stderr, " type=%u\n", runtime->Load8(rdram, ctx, record + 0x0Fu));
                std::fprintf(stderr, " [dvci]     extents[%d]:", slot);
                for (uint32_t off = 0x100u; off <= 0x128u; off += 4u)
                    std::fprintf(stderr, " +%03X=%08X", off, runtime->Load32(rdram, ctx, record + off));
                std::fprintf(stderr, "\n");
            }
            for (int slot = 0; slot < 4; ++slot)
            {
                const uint32_t record = 0x0039E848u + static_cast<uint32_t>(slot) * 0x60u;
                std::fprintf(stderr, " [dvci]   adxf[%d] active=%u:", slot, runtime->Load8(rdram, ctx, record));
                for (uint32_t off = 0u; off <= 0x1Cu; off += 4u)
                    std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, record + off));
                std::fprintf(stderr, "\n");
            }
            {
                // The reader the DVCI handle points at (handle + 4) is slot 16 of the same table.
                const uint32_t handle = runtime->Load32(rdram, ctx, kDvciHandlePtr);
                const uint32_t reader = handle >= 0x00100000u && handle < 0x02000000u
                                            ? runtime->Load32(rdram, ctx, handle + 4u)
                                            : 0u;
                if (reader >= 0x00100000u && reader < 0x02000000u)
                {
                    std::fprintf(stderr, "[dvci]   reader slot=0x%08X:", reader);
                    for (uint32_t off = 0u; off <= 0x2Cu; off += 4u)
                        std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, reader + off));
                    std::fprintf(stderr, " active=%u\n", runtime->Load8(rdram, ctx, reader));
                    std::fprintf(stderr, "[dvci]   reader tail:");
                    for (uint32_t off = 0x30u; off <= 0x5Cu; off += 4u)
                        std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, reader + off));
                    std::fprintf(stderr, "\n[dvci]   reader flags: 44=%u 45=%u 46=%u 47=%u 48=%u 49=%u\n",
                                 runtime->Load8(rdram, ctx, reader + 0x44u),
                                 runtime->Load8(rdram, ctx, reader + 0x45u),
                                 runtime->Load8(rdram, ctx, reader + 0x46u),
                                 runtime->Load8(rdram, ctx, reader + 0x47u),
                                 runtime->Load8(rdram, ctx, reader + 0x48u),
                                 runtime->Load8(rdram, ctx, reader + 0x49u));
                    // reader+8 is the CRI file object: [0] = device object with inline slots
                    // (+0x10 open, +0x18 seek, +0x1C tell, +0x20 read submit, +0x2C status), [1] = device handle.
                    const uint32_t fileObject = runtime->Load32(rdram, ctx, reader + 8u);
                    if (fileObject >= 0x00100000u && fileObject < 0x02000000u)
                    {
                        const uint32_t device = runtime->Load32(rdram, ctx, fileObject);
                        std::fprintf(stderr, "[dvci]   fileobj=0x%08X dev=0x%08X handle=0x%08X\n", fileObject, device,
                                     runtime->Load32(rdram, ctx, fileObject + 4u));
                        if (device >= 0x00100000u && device < 0x02000000u)
                        {
                            std::fprintf(stderr, "[dvci]     dev slots:");
                            for (uint32_t off = 0u; off <= 0x2Cu; off += 4u)
                                std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, device + off));
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
            }
            for (int slot = 0; slot < 4; ++slot)
            {
                const uint32_t record = kCriRequestPool + static_cast<uint32_t>(slot) * 0x40u;
                std::fprintf(stderr, " [dvci]   req[%d] used=%u:", slot,
                             runtime->Load32(rdram, ctx, kCriRequestInUse + static_cast<uint32_t>(slot) * 0x40u));
                for (uint32_t off = 0u; off <= 0x1Cu; off += 4u)
                    std::fprintf(stderr, " +%02X=%08X", off, runtime->Load32(rdram, ctx, record + off));
                std::fprintf(stderr, "\n");
            }
        }
    }

    // The poll reports failures through FUN_00195A18, which formats the message with the library's own sprintf;
    // wrapping it and dumping its string argument is what names the failing branch instead of guessing from the
    // decompiler's shape.
    constexpr uint32_t kDvciErrorPrintPc = 0x00195A18u;
    PS2Runtime::RecompiledFunction gDvciErrorPrintOriginal = nullptr;

    std::string guestString(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address)
    {
        std::string text;
        if (address < 0x00100000u || address >= 0x02000000u)
            return text;
        for (uint32_t i = 0u; i < 160u; ++i)
        {
            const uint8_t byte = runtime->Load8(rdram, ctx, address + i);
            if (byte == 0u)
                break;
            text.push_back(static_cast<char>(byte < 0x20u || byte > 0x7Eu ? '.' : byte));
        }
        return text;
    }

    void dvciErrorPrintProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sPrints = 0u;
        if (++sPrints <= 24u)
        {
            std::fprintf(stderr, "[dvci] error print ra=0x%08X text='%s'\n", GPR_U32(ctx, 31),
                         guestString(rdram, ctx, runtime, GPR_U32(ctx, 4)).c_str());
        }
        if (gDvciErrorPrintOriginal != nullptr)
            gDvciErrorPrintOriginal(rdram, ctx, runtime);
    }

    constexpr uint32_t kDvciStartPc = 0x00196058u;
    PS2Runtime::RecompiledFunction gDvciStartOriginal = nullptr;

    void dvciStartProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sStarts = 0u;
        const uint32_t call = ++sStarts;
        const uint32_t index = GPR_U32(ctx, 4);

        if (gDvciStartOriginal != nullptr)
            gDvciStartOriginal(rdram, ctx, runtime);

        if (call <= 24u)
        {
            std::fprintf(stderr, "[dvci] start(i=%u) #%u -> %d | ptid=%d handle=0x%08X\n", index, call,
                         static_cast<int32_t>(GPR_U32(ctx, 2)),
                         static_cast<int32_t>(runtime->Load32(rdram, ctx, kDvciLoadIndex)),
                         runtime->Load32(rdram, ctx, kDvciHandlePtr));
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // The CRI file device keeps the file size at device-handle+0x08 and the current position at +0x0C, both
    // counted in 2048-byte SECTORS; its seek slot FUN_001AD780 computes position = [+0x08] + offset for SEEK_END
    // and its tell slot FUN_001AD818 returns [+0x0C]. FUN_0019f298 turns that into the ADXF stream's sector
    // count (stream+0x14) and byte window (stream+0x10 = count << 11), and FUN_00196960 copies it into the DVCI
    // handle's +0x0C.
    //
    // This used to need a shim (TOL_DVCI_SIZE_SHIM) that answered seek(END) from the host file size, because
    // the device never learned the size: the open reply is {base, sizeInBytes} and only the base was filled.
    // The file server now returns the real size at request+0x04 (see openGameFile), so the guest computes the
    // sector count itself and the shim is gone. Do not reintroduce it - if the size reads 0 again, the open
    // reply is the place to look.

    // The ADXF stream step (FUN_0019F298) is what should advance a stream: it opens the file, computes the window
    // and, at its tail, calls the read step FUN_0019EF58 when (`state == 2 && stream[0x49] == 1`). Probing the step
    // itself (rather than the DVCI handle from the main thread) shows the flags at the exact moment the worker pumps.
    constexpr uint32_t kAdxfStepPc = 0x0019F298u;
    PS2Runtime::RecompiledFunction gAdxfStepOriginal = nullptr;
    // Shared counter: the poll freeze is really the question "is anyone still pumping the ADXF streams?", so
    // the poll line reports how many times the stream step has run so far.
    uint32_t gAdxfStepCalls = 0u;
    uint64_t gAdxfStepWallNs = 0u;
    uint64_t gAdxfStepCpuNs = 0u;
    uint64_t threadCpuTimeNs();

    void dumpStream(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t stream, const char *when)
    {
        if (stream < 0x00100000u || stream >= 0x02000000u)
        {
            std::fprintf(stderr, "[adxf] %s stream=0x%08X (not a pointer)\n", when, stream);
            return;
        }
        // stream+0x08 is the file object created by the ADXF open; its second word is the device handle the
        // CRI file driver uses (0x003A6B00 + index*0x48), whose byte +2 is the transfer state (1 = done).
        uint32_t devHandle = 0u;
        const uint32_t fileObject = runtime->Load32(rdram, ctx, stream + 8u);
        if (fileObject >= 0x00100000u && fileObject < 0x02000000u && (fileObject & 3u) == 0u)
        {
            devHandle = runtime->Load32(rdram, ctx, fileObject + 4u);
        }
        std::fprintf(stderr,
                     "[adxf] %s stream=0x%08X state=%u | 44=%u 45=%u 46=%u 47=%u 48=%u 49=%u"
                     " | +10=%08X +14=%08X +18=%08X +1C=%08X +20=%08X +24=%08X +28=%08X"
                     " | +2C=%08X +30=%08X +34=%08X +40=%08X +50=%08X +54=%08X +58=%08X +5C=%08X"
                     " | dev=%08X devState=%u sync=%u\n",
                     when, stream, runtime->Load8(rdram, ctx, stream + 1u),
                     runtime->Load8(rdram, ctx, stream + 0x44u), runtime->Load8(rdram, ctx, stream + 0x45u),
                     runtime->Load8(rdram, ctx, stream + 0x46u), runtime->Load8(rdram, ctx, stream + 0x47u),
                     runtime->Load8(rdram, ctx, stream + 0x48u), runtime->Load8(rdram, ctx, stream + 0x49u),
                     runtime->Load32(rdram, ctx, stream + 0x10u), runtime->Load32(rdram, ctx, stream + 0x14u),
                     runtime->Load32(rdram, ctx, stream + 0x18u), runtime->Load32(rdram, ctx, stream + 0x1Cu),
                     runtime->Load32(rdram, ctx, stream + 0x20u), runtime->Load32(rdram, ctx, stream + 0x24u),
                     runtime->Load32(rdram, ctx, stream + 0x28u), runtime->Load32(rdram, ctx, stream + 0x2Cu),
                     runtime->Load32(rdram, ctx, stream + 0x30u), runtime->Load32(rdram, ctx, stream + 0x34u),
                     runtime->Load32(rdram, ctx, stream + 0x40u),
                     runtime->Load32(rdram, ctx, stream + 0x50u), runtime->Load32(rdram, ctx, stream + 0x54u),
                     runtime->Load32(rdram, ctx, stream + 0x58u), runtime->Load32(rdram, ctx, stream + 0x5Cu),
                     devHandle,
                     (devHandle >= 0x00100000u && devHandle < 0x02000000u)
                         ? runtime->Load8(rdram, ctx, devHandle + 2u)
                         : 0u,
                     runtime->Load32(rdram, ctx, 0x003A7784u));
        std::fprintf(stderr,
                     "[adxf]   +04=%08X +08=%08X +0C=%08X +3C=%08X +60=%08X devActive=%u pumpGuard=%u\n",
                     runtime->Load32(rdram, ctx, stream + 4u),
                     runtime->Load32(rdram, ctx, stream + 8u),
                     runtime->Load32(rdram, ctx, stream + 0xCu),
                     runtime->Load32(rdram, ctx, stream + 0x3Cu),
                     runtime->Load32(rdram, ctx, stream + 0x60u),
                     static_cast<unsigned>(runtime->Load8(rdram, ctx, 0x003A55D0u)),
                     static_cast<unsigned>(runtime->Load32(rdram, ctx, 0x00393208u)));
        const uint32_t pathPtr = runtime->Load32(rdram, ctx, stream + 0x50u);
        if (pathPtr >= 0x00100000u && pathPtr < 0x02000000u)
        {
            char path[64] = {};
            for (int i = 0; i < 63; ++i)
            {
                path[i] = static_cast<char>(runtime->Load8(rdram, ctx, pathPtr + static_cast<uint32_t>(i)));
                if (path[i] == '\0')
                    break;
            }
            std::fprintf(stderr, "[adxf]   path=0x%08X '%s'\n", pathPtr, path);
        }
    }

    void adxfStepProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        static uint32_t sSame = 0u;
        static uint32_t sLastTuple = 0xFFFFFFFFu;
        const uint32_t call = ++sCalls;
        const uint32_t stream = GPR_U32(ctx, 4);
        const bool log = call <= 40u;
        ++gAdxfStepCalls;

        if (log)
            dumpStream(rdram, ctx, runtime, stream, "step-in ");

        if (gAdxfStepOriginal != nullptr)
        {
            const auto wall0 = std::chrono::steady_clock::now();
            const uint64_t cpu0 = threadCpuTimeNs();
            gAdxfStepOriginal(rdram, ctx, runtime);
            const uint64_t cpu1 = threadCpuTimeNs();
            const auto wall1 = std::chrono::steady_clock::now();
            gAdxfStepWallNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count());
            gAdxfStepCpuNs += cpu1 - cpu0;
            if ((gAdxfStepCalls % 100u) == 0u)
            {
                const double wallMs = static_cast<double>(gAdxfStepWallNs) / 1.0e6;
                const double cpuMs = static_cast<double>(gAdxfStepCpuNs) / 1.0e6;
                const double ratio = gAdxfStepWallNs != 0u
                                         ? static_cast<double>(gAdxfStepCpuNs) / static_cast<double>(gAdxfStepWallNs)
                                         : 0.0;
                const double stepsPerRead =
                    gGameFileReadCount != 0u
                        ? static_cast<double>(gAdxfStepCalls) / static_cast<double>(gGameFileReadCount)
                        : 0.0;
                std::fprintf(stderr,
                             "[adxf-time] steps=%u wall=%.1fms cpu=%.1fms cpu/wall=%.3f reads=%u steps/read=%.2f\n",
                             gAdxfStepCalls, wallMs, cpuMs, ratio, gGameFileReadCount, stepsPerRead);
            }
        }

        // Stall detector: the stream step FUN_0019F298 is a no-op unless it decides to call the read/completion
        // state machine FUN_0019EF58, and the whole DVCI load waits on that. When it stops changing anything the
        // poll can never see handle state 3, so print the fields the decision uses instead of guessing.
        if (stream >= 0x00100000u && stream < 0x02000000u && (stream & 3u) == 0u)
        {
            const uint32_t state = runtime->Load8(rdram, ctx, stream + 1u);
            const uint32_t busy = runtime->Load8(rdram, ctx, stream + 2u);
            const uint32_t f45 = runtime->Load8(rdram, ctx, stream + 0x45u);
            const uint32_t f48 = runtime->Load8(rdram, ctx, stream + 0x48u);
            const uint32_t f49 = runtime->Load8(rdram, ctx, stream + 0x49u);
            const uint32_t buf = runtime->Load32(rdram, ctx, stream + 0x24u);
            const uint32_t doneBytes = runtime->Load32(rdram, ctx, stream + 0x34u);
            const uint32_t blocks = runtime->Load32(rdram, ctx, stream + 0x58u);
            const uint32_t window = runtime->Load32(rdram, ctx, stream + 0x5Cu);
            uint32_t devState = 0u;
            uint32_t devBase = 0u;
            const uint32_t fileObject = runtime->Load32(rdram, ctx, stream + 8u);
            if (fileObject >= 0x00100000u && fileObject < 0x02000000u && (fileObject & 3u) == 0u)
            {
                const uint32_t dev = runtime->Load32(rdram, ctx, fileObject + 4u);
                if (dev >= 0x00100000u && dev < 0x02000000u)
                {
                    devState = runtime->Load8(rdram, ctx, dev + 2u);
                    devBase = runtime->Load32(rdram, ctx, dev + 0x20u);
                }
            }

            uint32_t tuple = state | (busy << 8) | (f45 << 16) | (f48 << 24);
            tuple = tuple * 16777619u ^ (f49 + 0x9E3779B9u);
            tuple = tuple * 16777619u ^ buf;
            tuple = tuple * 16777619u ^ doneBytes;
            tuple = tuple * 16777619u ^ (blocks + 0x85EBCA6Bu);
            tuple = tuple * 16777619u ^ (window + 0xC2B2AE35u);
            tuple = tuple * 16777619u ^ devState;
            if (tuple == sLastTuple)
            {
                ++sSame;
                if ((sSame % 400u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[adxf-stall] step #%u unchanged x%u | state=%u busy=%u 45=%u 48=%u 49=%u"
                                 " buf=0x%08X done=%u blocks=%u win=%u dev=%u devBase=0x%X\n",
                                 call, sSame, state, busy, f45, f48, f49, buf, doneBytes, blocks, window, devState,
                                 devBase);
                }
            }
            else
            {
                sSame = 0u;
                sLastTuple = tuple;
            }
        }

        if (log)
        {
            dumpStream(rdram, ctx, runtime, stream, "step-out");
            std::fprintf(stderr, "[adxf] step #%u done pc=0x%08X\n", call, ctx->pc);
        }
    }

    // TOL_TRACE_CRI_TIME=1: CPU vs wall time for the CRI state machine FUN_001AB4F8, plus ADXF steps per
    // completed 2 KiB read. Thread CPU time comes from GetThreadTimes (the guest runs on the dispatcher
    // thread and its fibers), so preemption shows up as wall >> cpu.
#if defined(_WIN32)
    uint64_t threadCpuTimeNs()
    {
        FILETIME creation, exit, kernel, user;
        if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
        {
            ULARGE_INTEGER kernelTime, userTime;
            kernelTime.LowPart = kernel.dwLowDateTime;
            kernelTime.HighPart = kernel.dwHighDateTime;
            userTime.LowPart = user.dwLowDateTime;
            userTime.HighPart = user.dwHighDateTime;
            return (kernelTime.QuadPart + userTime.QuadPart) * 100ull;
        }
        return 0ull;
    }
#else
    uint64_t threadCpuTimeNs() { return 0ull; }
#endif

    constexpr uint32_t kCriStateMachinePc = 0x001AB5C8u;
    PS2Runtime::RecompiledFunction gCriStateOriginal = nullptr;
    uint64_t gCriStateCalls = 0u;
    uint64_t gCriStateWallNs = 0u;
    uint64_t gCriStateCpuNs = 0u;

    void criStateTimeProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto wall0 = std::chrono::steady_clock::now();
        const uint64_t cpu0 = threadCpuTimeNs();
        if (gCriStateOriginal != nullptr)
            gCriStateOriginal(rdram, ctx, runtime);
        const uint64_t cpu1 = threadCpuTimeNs();
        const auto wall1 = std::chrono::steady_clock::now();
        gCriStateWallNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count());
        gCriStateCpuNs += cpu1 - cpu0;
        ++gCriStateCalls;
        if ((gCriStateCalls % 100u) == 0u)
        {
            const double wallMs = static_cast<double>(gCriStateWallNs) / 1.0e6;
            const double cpuMs = static_cast<double>(gCriStateCpuNs) / 1.0e6;
            const double ratio =
                gCriStateWallNs != 0u ? static_cast<double>(gCriStateCpuNs) / static_cast<double>(gCriStateWallNs)
                                      : 0.0;
            const double stepsPerRead = gGameFileReadCount != 0u
                                            ? static_cast<double>(gAdxfStepCalls) /
                                                  static_cast<double>(gGameFileReadCount)
                                            : 0.0;
            std::fprintf(stderr,
                         "[cri-time] calls=%llu wall=%.1fms cpu=%.1fms cpu/wall=%.3f | steps=%u reads=%u "
                         "steps/read=%.2f\n",
                         static_cast<unsigned long long>(gCriStateCalls), wallMs, cpuMs, ratio, gAdxfStepCalls,
                         gGameFileReadCount, stepsPerRead);
        }
    }

    // FUN_001973A8 is the DVCI progress tick: it walks 16 handles at 0x00390738 (stride 0x44) and calls the credit
    // routine FUN_001972E0 for every handle whose byte 0 is 1. The pump runs it ~35x per run but the credit routine
    // is entered once, so the active flag the iterator sees is not the one the poll reports - this probe prints the
    // flags from inside the pump to settle it.
    constexpr uint32_t kDvciTickPc = 0x001973A8u;
    constexpr uint32_t kDvciHandleTable = 0x00390738u;
    constexpr uint32_t kDvciHandleStride = 0x44u;
    PS2Runtime::RecompiledFunction gDvciTickOriginal = nullptr;

    void dvciTickProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sTicks = 0u;
        const uint32_t tick = ++sTicks;
        if (tick <= 10u || (tick % 8u) == 0u)
        {
            std::fprintf(stderr, "[dvci] tick #%u flags:", tick);
            for (int slot = 0; slot < 16; ++slot)
            {
                const uint32_t record = kDvciHandleTable + static_cast<uint32_t>(slot) * kDvciHandleStride;
                const uint32_t b0 = runtime->Load8(rdram, ctx, record);
                if (b0 == 0u)
                    continue;
                std::fprintf(stderr, " [%d]0x%08X b0=%u b1=%u", slot, record, b0,
                             runtime->Load8(rdram, ctx, record + 1u));
            }
            std::fprintf(stderr, " | dvci handle=0x%08X\n", runtime->Load32(rdram, ctx, kDvciHandlePtr));

            // The done-vs-total question, read from inside the tick: FUN_0019EF58 only sets stream[+1] = 3 when
            // blocksDone (stream+0x58) reaches totalSectors (stream+0x14); until then it keeps arming blocks.
            const uint32_t handlePtr = runtime->Load32(rdram, ctx, kDvciHandlePtr);
            if (handlePtr >= 0x00100000u && handlePtr < 0x02000000u)
            {
                const uint32_t reader = runtime->Load32(rdram, ctx, handlePtr + 4u);
                if (reader >= 0x00100000u && reader < 0x02000000u)
                {
                    std::fprintf(stderr,
                                 "[dvci]   credit: handleState=%u consumed=%u transfer=%u | streamState=%u"
                                 " blocksDone=%u totalSectors=%u totalBytes=%08X doneBytes=%u pending=%u\n",
                                 runtime->Load8(rdram, ctx, handlePtr + 1u),
                                 runtime->Load32(rdram, ctx, handlePtr + 0x14u),
                                 runtime->Load32(rdram, ctx, handlePtr + 0x1Cu),
                                 runtime->Load8(rdram, ctx, reader + 1u), runtime->Load32(rdram, ctx, reader + 0x58u),
                                 runtime->Load32(rdram, ctx, reader + 0x14u),
                                 runtime->Load32(rdram, ctx, reader + 0x10u),
                                 runtime->Load32(rdram, ctx, reader + 0x34u), runtime->Load32(rdram, ctx, reader + 0x20u));
                }
            }
        }
        if (gDvciTickOriginal != nullptr)
            gDvciTickOriginal(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------------------------------------------
    // CRI I/O LATENCY (TOL_CRI_IO_LATENCY=1) - diagnostic.
    //
    // FUN_00197940 pumps as `FUN_0019f468(); … FUN_0019f468(); FUN_001973a8();` and the tick FUN_001973a8 (which
    // credits the DVCI handle) therefore samples the ADXF stream AFTER two stream pumps. Our file RPCs complete
    // synchronously, so a read goes 2 (reading) -> 3 (done) -> 2 (next read armed) inside one cycle and the tick
    // almost never observes 3: measured 1 observation in 148k polls, which is why the 430-entry AFS table freezes at
    // 255 entries. Real hardware takes milliseconds, so state 3 survives many pump cycles.
    //
    // This wrapper gives every completed device read one extra poll of latency (reports "busy" once, then "done"),
    // restoring the window the guest's state machine expects.
    constexpr uint32_t kCriDeviceStatusPc = 0x001ADAF8u;
    PS2Runtime::RecompiledFunction gCriStatusOriginal = nullptr;

    struct CriStatusDefer
    {
        uint32_t handle = 0u;
        bool deferred = false;
    };
    CriStatusDefer gCriStatusDefer[8] = {};

    void criStatusLatencyProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t handle = GPR_U32(ctx, 4);

        if (gCriStatusOriginal != nullptr)
            gCriStatusOriginal(rdram, ctx, runtime);

        const uint32_t status = GPR_U32(ctx, 2);
        if (status != 1u || handle == 0u)
            return;

        CriStatusDefer *slot = nullptr;
        for (CriStatusDefer &entry : gCriStatusDefer)
        {
            if (entry.handle == handle)
            {
                slot = &entry;
                break;
            }
            if (slot == nullptr && entry.handle == 0u)
                slot = &entry;
        }
        if (slot == nullptr)
            return;
        slot->handle = handle;

        if (!slot->deferred)
        {
            // First observation of "done": hold it one pump so the tick can still see state 3.
            slot->deferred = true;
            SET_GPR_U32(ctx, 2, 0u);
        }
        else
        {
            // Second observation: let the completion through.
            slot->deferred = false;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // CRI DEVICE READ PROBE (TOL_TRACE_DVCI=1) - diagnostic.
    //
    // The DVCI device runs in ASYNCHRONOUS mode (DAT_003A7784 == 0): the read slot FUN_001ad850 starts a transfer,
    // stores handle[+2] = 2 (busy) and expects a completion to raise it to 1. The stream step FUN_0019ef58 only
    // credits blocks/bytes when the status it samples is exactly 1; status 3 is its discard branch. Logging the
    // entry state, the return value and the exit state shows whether a read was refused (busy) or accepted.
    constexpr uint32_t kCriDeviceReadPc = 0x001AD850u;
    PS2Runtime::RecompiledFunction gCriReadOriginal = nullptr;

    void criReadProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t handle = GPR_U32(ctx, 4);
        const uint32_t blocks = GPR_U32(ctx, 5);
        const uint32_t buffer = GPR_U32(ctx, 6);
        const bool valid = handle >= 0x00100000u && handle < 0x02000000u;
        const uint8_t before = valid ? runtime->Load8(rdram, ctx, handle + 2u) : 0u;

        if (gCriReadOriginal != nullptr)
            gCriReadOriginal(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        const uint8_t after = valid ? runtime->Load8(rdram, ctx, handle + 2u) : 0u;
        static uint32_t sReads = 0u;
        if (++sReads <= 40u)
        {
            std::fprintf(stderr,
                         "[cri] read handle=0x%08X blocks=%u buf=0x%08X state=%u->%u ret=%u | pos=%u/%u mode=%u"
                         " | 18=%08X 1C=%08X 20=%08X 24=%08X\n",
                         handle, blocks, buffer, before, after, ret,
                         valid ? runtime->Load32(rdram, ctx, handle + 0x0Cu) : 0u,
                         valid ? runtime->Load32(rdram, ctx, handle + 0x08u) : 0u,
                         valid ? runtime->Load8(rdram, ctx, handle + 0x44u) : 0u,
                         valid ? runtime->Load32(rdram, ctx, handle + 0x18u) : 0u,
                         valid ? runtime->Load32(rdram, ctx, handle + 0x1Cu) : 0u,
                         valid ? runtime->Load32(rdram, ctx, handle + 0x20u) : 0u,
                         valid ? runtime->Load32(rdram, ctx, handle + 0x24u) : 0u);
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // ADXF READ STEP PROBE (TOL_TRACE_DVCI=1) - diagnostic.
    //
    // The field layout has two structs that share offsets, which made this look contradictory for a long time:
    //   * the DVCI *handle* (lVar3) uses +0x0C = total blocks, +0x14 = consumed blocks, +0x1C = this arm's blocks,
    //     +1 = handle state (2 armed / 3 done), +8 = transfer buffer;
    //   * the ADXF *stream* (handle+4) uses +0x14 = total blocks, +0x58 = blocks done, +0x34 = bytes done in this
    //     arm, +0x5C = arm blocks, +8 = open file object, +1 = stream state, +0x45 open req, +0x46 close req,
    //     +0x48 abort req, +0x49 file open.
    // FUN_0019EF58 only raises stream[+1] = 3 once (+0x34 >> 11) >= +0x5C, and it needs stream+8 (the open file
    // object) to actually transfer. Logging entry/exit for the MAP.AFS stream shows which branch returns early.
    constexpr uint32_t kAdxfReadStepPc = 0x0019EF58u;
    constexpr uint32_t kMapAfsStream = 0x0039EE48u;
    PS2Runtime::RecompiledFunction gAdxfReadStepOriginal = nullptr;

    void adxfReadStepProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stream = GPR_U32(ctx, 4);
        static uint32_t sCalls = 0u;
        ++sCalls;
        const bool want = (stream == kMapAfsStream) && (sCalls <= 6u || (sCalls % 200u) == 0u);

        auto snap = [&](const char *phase) {
            if (!want)
                return;
            std::fprintf(stderr,
                         "[adxf-read] #%u %s | st=%u busy=%u 45=%u 46=%u 47=%u 48=%u 49=%u"
                         " | 08=%08X 14=%u 1C=%u 20=%u 30=%u 34=%u 40=%u 58=%u 5C=%u\n",
                         sCalls, phase, runtime->Load8(rdram, ctx, stream + 1u),
                         runtime->Load8(rdram, ctx, stream + 2u), runtime->Load8(rdram, ctx, stream + 0x45u),
                         runtime->Load8(rdram, ctx, stream + 0x46u), runtime->Load8(rdram, ctx, stream + 0x47u),
                         runtime->Load8(rdram, ctx, stream + 0x48u), runtime->Load8(rdram, ctx, stream + 0x49u),
                         runtime->Load32(rdram, ctx, stream + 8u), runtime->Load32(rdram, ctx, stream + 0x14u),
                         runtime->Load32(rdram, ctx, stream + 0x1Cu), runtime->Load32(rdram, ctx, stream + 0x20u),
                         runtime->Load32(rdram, ctx, stream + 0x30u), runtime->Load32(rdram, ctx, stream + 0x34u),
                         runtime->Load32(rdram, ctx, stream + 0x40u), runtime->Load32(rdram, ctx, stream + 0x58u),
                         runtime->Load32(rdram, ctx, stream + 0x5Cu));
        };

        snap("in ");
        if (gAdxfReadStepOriginal != nullptr)
            gAdxfReadStepOriginal(rdram, ctx, runtime);
        snap("out");
    }

    // ---------------------------------------------------------------------------------------------------------
    // ADXF ARM DISPATCH PROBE (TOL_TRACE_DVCI=1) - diagnostic.
    //
    // FUN_00196DB0 arms one read arm on the DVCI handle: it resets the stream to state 1 (FUN_0019EEC8 ->
    // FUN_0019EE60), computes handle[0x1C] = handle[0xC] - handle[0x14], and arms the stream with
    // handle[0x1C] blocks (FUN_0019EE18 -> FUN_0019EDA0 sets stream state 2 and stream[0x5C] = arm blocks).
    // If handle[0xC] <= handle[0x14] the arm is empty and the handle jumps straight to state 3 without ever
    // arming the stream, which is exactly the "handle done, stream still state 1" combination we saw.
    constexpr uint32_t kAdxfArmPc = 0x00196DB0u;
    PS2Runtime::RecompiledFunction gAdxfArmOriginal = nullptr;

    void adxfArmProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t handle = GPR_U32(ctx, 4);
        const uint32_t blocks = GPR_U32(ctx, 5);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if ((sCalls <= 5u || (sCalls % 50u) == 0u) && handle >= 0x00100000u && handle < 0x02000000u)
        {
            const uint32_t stream = runtime->Load32(rdram, ctx, handle + 4u);
            std::fprintf(stderr,
                         "[adxf-arm] #%u handle=0x%08X blocks=%u buf=0x%08X stream=0x%08X"
                         " | h1=%u hC=%u h14=%u h1C=%u h2C=%u | s1=%u s08=%08X s14=%u s5C=%u s49=%u\n",
                         sCalls, handle, blocks, GPR_U32(ctx, 6), stream,
                         runtime->Load8(rdram, ctx, handle + 1u), runtime->Load32(rdram, ctx, handle + 0xCu),
                         runtime->Load32(rdram, ctx, handle + 0x14u), runtime->Load32(rdram, ctx, handle + 0x1Cu),
                         runtime->Load32(rdram, ctx, handle + 0x2Cu), runtime->Load8(rdram, ctx, stream + 1u),
                         runtime->Load32(rdram, ctx, stream + 8u), runtime->Load32(rdram, ctx, stream + 0x14u),
                         runtime->Load32(rdram, ctx, stream + 0x5Cu), runtime->Load8(rdram, ctx, stream + 0x49u));
        }
        if (gAdxfArmOriginal != nullptr)
            gAdxfArmOriginal(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------------------------------------------
    // ASYNC LOAD JOB PROBE (TOL_TRACE_LOAD=1) - diagnostic.
    //
    // FUN_001EC670 runs five sequential load stages:
    //     job = FUN_001EC9A0(stage, resourceId, 1);
    //     while (FUN_0027BFF0(job) == 0) { FUN_00151FF0(0); FUN_0019A080(); FUN_001EE060(); FUN_001392D0(); }
    // FUN_001EC9F0 creates the job and starts it with FUN_001F72F0; FUN_0027BFF0 is just `*job == 2`.
    // If a stage never reaches state 2 the startup blocks inside FUN_001EC670 (reached from FUN_00164EA0
    // via FUN_00166870) and the frame loop never advances. These probes show which stage hangs and the
    // job's live state word.
    constexpr uint32_t kLoadStartPc = 0x001EC9F0u;
    constexpr uint32_t kLoadStatusPc = 0x0027BFF0u;
    constexpr uint32_t kResourceLookupPc = 0x003710C0u;
    constexpr uint32_t kTransferSubmitPc = 0x001F8BE0u;
    PS2Runtime::RecompiledFunction gLoadStartPrev = nullptr;
    PS2Runtime::RecompiledFunction gLoadStatusPrev = nullptr;
    PS2Runtime::RecompiledFunction gResourceLookupPrev = nullptr;
    PS2Runtime::RecompiledFunction gTransferSubmitPrev = nullptr;

    bool loadTraceEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_LOAD") != nullptr;
        return enabled;
    }

    void loadStartProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t resId = GPR_U32(ctx, 4);
        const uint32_t resource = GPR_U32(ctx, 5);
        const uint32_t flag = GPR_U32(ctx, 6);
        if (gLoadStartPrev != nullptr)
            gLoadStartPrev(rdram, ctx, runtime);
        if (loadTraceEnabled())
        {
            const uint32_t job = GPR_U32(ctx, 2);
            const uint32_t state = (job >= 0x00100000u && job < PS2_RAM_SIZE) ? runtime->Load32(rdram, ctx, job)
                                                                             : 0xFFFFFFFFu;
            std::fprintf(stderr, "[load] start res=%u resource=0x%08X flag=%u -> job=0x%08X state=%u\n", resId,
                         resource, flag, job, state);
        }
    }

    void loadStatusProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t job = GPR_U32(ctx, 4);
        static uint32_t sCalls = 0u;
        static uint32_t sLastJob = 0u;
        static uint32_t sLastState = 0xFFFFFFFFu;
        ++sCalls;
        const uint32_t state = (job >= 0x00100000u && job < PS2_RAM_SIZE) ? runtime->Load32(rdram, ctx, job)
                                                                         : 0xFFFFFFFFu;
        if (loadTraceEnabled() && (sCalls <= 10u || job != sLastJob || state != sLastState ||
                                   (sCalls % 500u) == 0u))
        {
            std::fprintf(stderr, "[load] status #%u job=0x%08X state=%u\n", sCalls, job, state);
            sLastJob = job;
            sLastState = state;
        }
        if (gLoadStatusPrev != nullptr)
            gLoadStatusPrev(rdram, ctx, runtime);
    }

    // FUN_003710C0(id) = *(u32*)(table + 0x10 + id*4) >> 2, where table = *(gp - 0x7534). Printing the table
    // pointer and the raw entry tells a missing resource table (null pointer -> reads address 0x20) apart from a
    // genuine asset id.
    void resourceLookupProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t id = GPR_U32(ctx, 4);
        const uint32_t gp = GPR_U32(ctx, 28);
        const uint32_t slot = ADD32(gp, 4294937292u); // gp - 0x7534
        const uint32_t table = runtime->Load32(rdram, ctx, slot);
        if (gResourceLookupPrev != nullptr)
            gResourceLookupPrev(rdram, ctx, runtime);
        if (loadTraceEnabled())
        {
            const uint32_t result = GPR_U32(ctx, 2);
            uint32_t entry = 0xFFFFFFFFu;
            if (table >= 0x00100000u && table < PS2_RAM_SIZE)
                entry = runtime->Load32(rdram, ctx, table + 0x10u + id * 4u);
            std::fprintf(stderr,
                         "[load] resource id=%u gp=0x%08X slot=0x%08X table=0x%08X entry=0x%08X -> 0x%08X\n", id,
                         gp, slot, table, entry, result);
        }
    }

    // FUN_001F8BE0 queues one 0x2C-byte transfer descriptor at 0x4A72C0 + slot*0x2C. The descriptor built by
    // FUN_001f82f0 carries id at +4, byte size at +0x10 and the destination guest pointer at +0x1C. The
    // transfer itself is a VIF1 DMA unpack; the status byte (+2) starts at 2 and is only cleared by the DMA
    // completion ISR, which this port never runs - hence the forced "done". Logging the descriptor shows
    // whether real data was expected (nonzero size) while the destination stays untouched.
    void transferSubmitProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t desc = GPR_U32(ctx, 4);
        const bool valid = desc >= 0x00100000u && desc < PS2_RAM_SIZE && (desc & 3u) == 0u;
        const uint32_t id = valid ? runtime->Load32(rdram, ctx, desc + 4u) : 0u;
        const uint32_t size = valid ? runtime->Load32(rdram, ctx, desc + 0x10u) : 0u;
        const uint32_t dest = valid ? runtime->Load32(rdram, ctx, desc + 0x1Cu) : 0u;
        const uint32_t type = valid ? runtime->Load8(rdram, ctx, desc + 2u) : 0u;
        if (gTransferSubmitPrev != nullptr)
            gTransferSubmitPrev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (loadTraceEnabled() && sCalls <= 12u)
        {
            const uint32_t handle = GPR_U32(ctx, 2);
            std::fprintf(stderr,
                         "[load] submit #%u type=%u id=%u size=%u dest=0x%08X -> handle=0x%08X\n", sCalls, type,
                         id, size, dest, handle);
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // TRANSFER DESCRIPTOR TRACKER (TOL_TRACE_LOAD=1) - diagnostic.
    //
    // Descriptors live at 0x4A72C0 + slot*0x2C: +1 kind, +2 status (2 queued / 1 busy / 0 done), +4 id,
    // +8 handle, +0x10 size, +0x1C dest, +0x20 mode. FUN_001f8DA0 is the reap: it is called with the
    // descriptor pointer right after the handler returns, and pops it from the queue. Logging the descriptor
    // on reap - plus a snapshot of every live slot at each handler entry - shows which request actually
    // completes and where the other one is stuck.
    constexpr uint32_t kReapPc = 0x001F8DA0u;
    constexpr uint32_t kDescBase = 0x004A72C0u;
    constexpr uint32_t kDescStride = 0x2Cu;
    constexpr uint32_t kDescSlots = 16u;
    PS2Runtime::RecompiledFunction gReapPrev = nullptr;
    PS2Runtime::RecompiledFunction gTransferHandlerPrev = nullptr;

    void dumpDescriptor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *tag, uint32_t desc)
    {
        const bool valid = desc >= 0x00100000u && desc < PS2_RAM_SIZE && (desc & 3u) == 0u;
        if (!valid)
        {
            std::fprintf(stderr, "[xfer] %s desc=0x%08X (invalid)\n", tag, desc);
            return;
        }
        std::fprintf(stderr,
                     "[xfer] %s desc=0x%08X kind=%u status=%u id=%u handle=0x%08X size=%u dest=0x%08X\n", tag,
                     desc, runtime->Load8(rdram, ctx, desc + 1u), runtime->Load8(rdram, ctx, desc + 2u),
                     runtime->Load32(rdram, ctx, desc + 4u), runtime->Load32(rdram, ctx, desc + 8u),
                     runtime->Load32(rdram, ctx, desc + 0x10u), runtime->Load32(rdram, ctx, desc + 0x1Cu));
    }

    void dumpLiveDescriptors(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *tag)
    {
        for (uint32_t slot = 0; slot < kDescSlots; ++slot)
        {
            const uint32_t desc = kDescBase + slot * kDescStride;
            const uint32_t status = runtime->Load8(rdram, ctx, desc + 2u);
            const uint32_t queued = runtime->Load8(rdram, ctx, desc);
            if (status == 0u && queued == 0u)
                continue;
            char label[32];
            std::snprintf(label, sizeof(label), "%s[%u]", tag, slot);
            dumpDescriptor(rdram, ctx, runtime, label, desc);
        }
    }

    void reapProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t desc = GPR_U32(ctx, 4);
        if (loadTraceEnabled())
        {
            dumpDescriptor(rdram, ctx, runtime, "reap-in ", desc);
            dumpLiveDescriptors(rdram, ctx, runtime, "before");
        }
        if (gReapPrev != nullptr)
            gReapPrev(rdram, ctx, runtime);
        if (loadTraceEnabled())
            dumpDescriptor(rdram, ctx, runtime, "reap-out", desc);
    }

    void transferHandlerEntryProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (loadTraceEnabled())
            dumpLiveDescriptors(rdram, ctx, runtime, "handler-entry");
        if (gTransferHandlerPrev != nullptr)
            gTransferHandlerPrev(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------------------------------------------
    // TRANSFER STATUS WAIT (always installed) - fix.
    //
    // FUN_001660A0 loads the boot script like this:
    //     v = FUN_001f72f0(0, buf, 0);              // queue the transfer
    //     while (FUN_001f8480(v) != 0) FUN_0015231C();
    //     *flag = 1;
    // FUN_001f8480 returns the descriptor status byte (+2): 2 queued, 1 busy, 0 done. It used to be stubbed
    // to always report done, which set the flag before the 1,252,176-byte transfer into 0x01523010 had landed,
    // so FUN_00166120 read an empty script (count = 0). Reporting the real status alone deadlocked, because
    // the wait only yields and nothing pumped the transfer.
    //
    // This wrapper reports the REAL status and, while the descriptor is still pending, runs the guest
    // scheduler (FUN_00151FF0) so worker 2 and the transfer handler make progress. Guest code stays in charge;
    // if the scheduler is preempted we keep its resume PC and return without fabricating a status.
    constexpr uint32_t kStatusThunkPc = 0x001F7310u; // thunk_FUN_001f8480 — what FUN_001660A0 calls
    constexpr uint32_t kStatusRealPc = 0x001F8480u;  // the status function itself
    constexpr uint32_t kSchedEntryPc = 0x00151FF0u;  // guest scheduler entry
    PS2Runtime::RecompiledFunction gStatusThunkPrev = nullptr;
    PS2Runtime::RecompiledFunction gStatusRealPrev = nullptr;
    PS2Runtime::RecompiledFunction gSchedEntryFn = nullptr;
    bool gInStatusWait = false;
    uint32_t gStatusWaits = 0u;

    void drivePendingTransfer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                              PS2Runtime::RecompiledFunction prev, uint32_t entryPc)
    {
        const uint32_t savedRa = GPR_U32(ctx, 31);
        const uint32_t handle = GPR_U32(ctx, 4);

        if (handle != 0u && !gInStatusWait && gSchedEntryFn != nullptr)
        {
            const uint32_t slot = handle & 0x3Fu;
            const uint32_t desc = 0x004A72C0u + slot * 0x2Cu;
            if (runtime->Load32(rdram, ctx, desc + 8u) == handle)
            {
                const uint32_t status = runtime->Load8(rdram, ctx, desc + 2u);
                if (status != 0u)
                {
                    gInStatusWait = true;
                    ++gStatusWaits;
                    if (std::getenv("TOL_TRACE_LOAD") != nullptr && gStatusWaits <= 8u)
                    {
                        std::fprintf(stderr,
                                     "[xfer] status-wait #%u slot=%u status=%u -> pumping guest scheduler\n",
                                     gStatusWaits, slot, status);
                    }
                    SET_GPR_U32(ctx, 31, savedRa);
                    SET_GPR_U32(ctx, 4, 0u);
                    ctx->pc = kSchedEntryPc;
                    gSchedEntryFn(rdram, ctx, runtime);
                    gInStatusWait = false;
                    if (ctx->pc != savedRa)
                    {
                        // Preempted inside the scheduler: keep its resume point.
                        SET_GPR_U32(ctx, 31, savedRa);
                        return;
                    }
                }
            }
        }

        if (prev != nullptr)
        {
            SET_GPR_U32(ctx, 31, savedRa);
            ctx->pc = entryPc;
            prev(rdram, ctx, runtime);
            if (ctx->pc != savedRa)
            {
                SET_GPR_U32(ctx, 31, savedRa);
                return;
            }
        }
        SET_GPR_U32(ctx, 31, savedRa);
        ctx->pc = savedRa;
    }

    void statusThunkWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        drivePendingTransfer(rdram, ctx, runtime, gStatusThunkPrev, kStatusThunkPc);
    }

    void statusRealWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        drivePendingTransfer(rdram, ctx, runtime, gStatusRealPrev, kStatusRealPc);
    }

    // ---------------------------------------------------------------------------------------------------------
    // TRANSFER LOCK (always installed) - fix.
    //
    // FUN_001f7390 opens the transfer handler with a yield-spinlock:
    //     while (iGpffff8780 != 0) FUN_0015231C();
    //     iGpffff8780 = 1;  ... reset decoder state ...
    // The handler is long-running and can be preempted at a cooperative checkpoint while holding it; the port
    // then re-enters the handler, sees the lock still set and spins forever (observed: 0x1F8530 entered 22x,
    // never reaching FUN_00196b88/FUN_001970e8). Everything here runs on one guest thread, so the lock can
    // only ever be stale, never genuinely contended - clear it before acquiring.
    constexpr uint32_t kTransferLockPc = 0x001F7390u;
    constexpr uint32_t kTransferLockAddr = 0x003DB370u; // gp - 0x7880 (iGpffff8780)
    PS2Runtime::RecompiledFunction gTransferLockPrev = nullptr;
    uint32_t gStaleLocksCleared = 0u;

    void transferLockProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t held = runtime->Load32(rdram, ctx, kTransferLockAddr);
        if (held != 0u)
        {
            ++gStaleLocksCleared;
            if (std::getenv("TOL_TRACE_LOAD") != nullptr && gStaleLocksCleared <= 8u)
            {
                std::fprintf(stderr, "[load] clearing stale transfer lock (was %u, clear #%u)\n", held,
                             gStaleLocksCleared);
            }
            runtime->Store32(rdram, ctx, kTransferLockAddr, 0u);
        }
        if (gTransferLockPrev != nullptr)
            gTransferLockPrev(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------------------------------------------
    // YIELD PUMP (always installed) - fix.
    //
    // The whole asset path waits by yielding: FUN_001f8530 (transfer handler) loops
    //     while (desc[2] != 3) { ...; if (FUN_00197668(dvci) == 3 && ...) goto done; FUN_0015231C(); }
    // and FUN_001660A0 waits `while (FUN_001f8480(h) != 0) FUN_0015231C();`. On hardware FUN_0015231C is the
    // thread yield, so the CRI/DVCI pump runs on another context and the read retires. This port has no
    // scheduler, so yielding did nothing and the reads never completed - which is why the boot script transfer
    // (id 0, 1,252,176 bytes into 0x01523010) never landed and FUN_001f8480 had to be force-completed.
    //
    // Make the yield do what the scheduler would: run the DVCI/ADXF pump (FUN_00197940) before returning.
    // Guarded against re-entry, since the pump itself can reach the yield again.
    constexpr uint32_t kYieldPc = 0x0015231Cu;
    constexpr uint32_t kDvciPumpPc = 0x00197940u;
    PS2Runtime::RecompiledFunction gYieldPrev = nullptr;
    PS2Runtime::RecompiledFunction gDvciPumpFn = nullptr;
    uint32_t gYieldPumpCount = 0u;

    void yieldPump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static bool sInYieldPump = false;
        const uint32_t savedRa = GPR_U32(ctx, 31);

        // Original context save (harmless bookkeeping: spills registers into the per-thread slot).
        if (gYieldPrev != nullptr)
        {
            SET_GPR_U32(ctx, 31, savedRa);
            ctx->pc = kYieldPc;
            gYieldPrev(rdram, ctx, runtime);
            if (ctx->pc != savedRa)
            {
                // Preempted inside the save: keep its resume point.
                SET_GPR_U32(ctx, 31, savedRa);
                return;
            }
        }

        if (!sInYieldPump && gDvciPumpFn != nullptr)
        {
            sInYieldPump = true;
            ++gYieldPumpCount;
            SET_GPR_U32(ctx, 31, savedRa);
            ctx->pc = kDvciPumpPc;
            gDvciPumpFn(rdram, ctx, runtime);
            sInYieldPump = false;
            // A cooperative checkpoint inside the pump (observed: FUN_001AD220, the DVCI completion poll)
            // bails out with ctx->pc at the resume point. Overwriting it here is what kept restarting the
            // transfer handler from scratch - it re-took its spinlock and bailed again, never reaching
            // FUN_00196b88. Preserve the resume point instead.
            if (ctx->pc != savedRa)
            {
                SET_GPR_U32(ctx, 31, savedRa);
                return;
            }
            if (std::getenv("TOL_TRACE_LOAD") != nullptr)
            {
                static uint32_t sLogged = 0u;
                ++sLogged;
                if (sLogged <= 8u || (sLogged % 200000u) == 0u)
                {
                    std::fprintf(stderr, "[load] yield-pump #%u (total %u) ra=0x%08X\n", sLogged,
                                 gYieldPumpCount, savedRa);
                }
            }
        }

        SET_GPR_U32(ctx, 31, savedRa);
        ctx->pc = savedRa;
    }

    // ---------------------------------------------------------------------------------------------------------
    // TRANSFER QUEUE PUMP (always installed) - fix.
    //
    // FUN_001f8be0 queues asset transfers at 0x4A72C0 and links them through the head at gp-0x7860
    // (0x003DB390). Worker 2 (0x1F84B0) is what drains that queue: its loop loads the head and dispatches
    // 0x1F8530 / 0x1F87C0 per descriptor. This port enters guest thread entries only once, so nothing ever
    // drains the queue: the boot script transfer (id 0, 1,252,176 bytes into 0x01523010) is queued but never
    // performed, leaving the script buffer zeroed and the startup blocked in FUN_001EC670. Pump worker 2 from
    // the I/O wait function, which every load wait loop calls on each iteration.
    constexpr uint32_t kIoPumpPc = 0x0019A080u;
    constexpr uint32_t kTransferQueueHead = 0x003DB390u;
    PS2Runtime::RecompiledFunction gIoPumpPrev = nullptr;
    uint32_t gTransferPumps = 0u;

    void ioPumpProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static bool sBusy = false;
        if (!sBusy)
        {
            const uint32_t head = runtime->Load32(rdram, ctx, kTransferQueueHead);
            if (head >= 0x00100000u && head < PS2_RAM_SIZE)
            {
                PS2Runtime::RecompiledFunction worker = runtime->lookupFunction(0x001F84B0u);
                if (worker != nullptr)
                {
                    sBusy = true;
                    const uint32_t savedRa = GPR_U32(ctx, 31);
                    ++gTransferPumps;
                    if (std::getenv("TOL_TRACE_LOAD") != nullptr && gTransferPumps <= 8u)
                    {
                        std::fprintf(stderr, "[load] pumping transfer queue head=0x%08X (pump %u)\n", head,
                                     gTransferPumps);
                    }
                    ctx->pc = 0x001F84B0u;
                    worker(rdram, ctx, runtime);
                    const bool bailed = (ctx->pc != savedRa);
                    SET_GPR_U32(ctx, 31, savedRa);
                    sBusy = false;
                    if (bailed)
                    {
                        // The transfer handler was preempted: the worker returned without running its
                        // epilogue and left ctx->pc at the resume point (guest frame intact). Hand control
                        // back to the runtime there instead of running the I/O pump over it.
                        return;
                    }
                }
            }
        }
        if (gIoPumpPrev != nullptr)
            gIoPumpPrev(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------------------------------------------
    // SCRIPT CONTEXT PROBE (TOL_TRACE_SCRIPT=1) - diagnostic.
    //
    // A script handle is a packed 64-bit value: low 32 bits = pointer to the command block, high 32 bits = the
    // current command index. `count` is the block's first word. The API is
    //   thunk 0x1F7340 -> FUN_001f88f0(handle) = !(index < count)      ("finished")
    //   thunk 0x1F7350 -> FUN_001f8920(handle) = *(u32*)(base+0x10+index*0x10)  ("peek next opcode")
    //   thunk 0x1F7370 -> FUN_001f89a0(handle) = (index + 1) packed     ("advance")
    // The dispatcher FUN_001ECBE0 spins when `status` never becomes non-zero, so print the live base/index/count
    // and the peeked opcode: a frozen index and a bogus count look completely different here.
    constexpr uint32_t kScriptStatusPc = 0x001F7340u;   // thunk_FUN_001f88f0
    constexpr uint32_t kScriptPeekPc = 0x001F7350u;     // thunk_FUN_001f8920
    constexpr uint32_t kScriptAdvancePc = 0x001F7370u;  // thunk_FUN_001f89a0
    PS2Runtime::RecompiledFunction gScriptStatusPrev = nullptr;
    PS2Runtime::RecompiledFunction gScriptPeekPrev = nullptr;
    PS2Runtime::RecompiledFunction gScriptAdvancePrev = nullptr;

    bool scriptTraceEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_SCRIPT") != nullptr;
        return enabled;
    }

    // FUN_001f88f0 is the script "finished?" predicate: it loads `count` from the command block and returns
    // !(index < count). A dispatcher can be handed a **null** command block (object field +0x10 == 0), in which
    // case the packed handle is (index << 32) | 0 and `count` is read from guest address 0, which holds
    // 0x3F800000 (1065353216). The dispatcher then has to walk a billion command indices before it reports
    // "finished" - observed as a permanent spin at 0x1ECC40 in FUN_001ECBE0 with base=0x00000000 and
    // count=0x3F800000. A null block has no commands, so report finished immediately. The guard is applied in
    // scriptStatusProbe, which is installed on the 0x1F7340 thunk that the dispatchers actually call.

    void scriptDump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *tag, uint32_t seq,
                    uint64_t handle, uint32_t extra)
    {
        const uint32_t base = static_cast<uint32_t>(handle);
        const uint32_t index = static_cast<uint32_t>(handle >> 32);
        uint32_t count = 0u;
        // Guest RAM starts at 0, so a null base is a *valid* read - it just reads whatever sits at address 0.
        // Reporting it as unreadable previously hid the fact that the stalled dispatcher is running on a null
        // handle.
        const bool readable = (base & 3u) == 0u && base < PS2_RAM_SIZE;
        if (readable)
            count = runtime->Load32(rdram, ctx, base);
        std::fprintf(stderr,
                     "[script] %s #%u handle=%016llX base=0x%08X index=%u count=%u readable=%u extra=0x%08X\n",
                     tag, seq, static_cast<unsigned long long>(handle), base, index, count,
                     readable ? 1u : 0u, extra);

        // Command records: 16 bytes each at base + 0x10 + index*0x10 (layout pinned in session 86; the
        // opcode peek is that address). Print the neighbourhood of the current index once per index so the
        // command the dispatcher is parked on is visible.
        if (readable && scriptTraceEnabled())
        {
            static uint32_t sLastDumpedIndex = 0xFFFFFFFFu;
            if (index != sLastDumpedIndex && index >= 12u && index <= 24u)
            {
                sLastDumpedIndex = index;
                for (int32_t d = -1; d <= 2; ++d)
                {
                    const int32_t i = static_cast<int32_t>(index) + d;
                    if (i < 0)
                        continue;
                    const uint32_t rec = base + 0x10u + static_cast<uint32_t>(i) * 0x10u;
                    std::fprintf(stderr, "[script]    rec[%d] @0x%08X: %08X %08X %08X %08X\n", i, rec,
                                 runtime->Load32(rdram, ctx, rec), runtime->Load32(rdram, ctx, rec + 4u),
                                 runtime->Load32(rdram, ctx, rec + 8u), runtime->Load32(rdram, ctx, rec + 12u));
                }
            }
        }
    }

    void scriptStatusProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t handle = GPR_U64(ctx, 4);
        static uint32_t sCalls = 0u;
        static uint32_t sNullBlocks = 0u;
        ++sCalls;

        // Null command block: (index << 32) | 0. Reporting "finished" here is what stops the dispatcher's
        // billion-iteration walk; see scriptStatusGuard for the full explanation.
        if (GPR_U32(ctx, 4) == 0u)
        {
            ++sNullBlocks;
            if (scriptTraceEnabled() && (sCalls <= 16u || sNullBlocks <= 5u))
            {
                std::fprintf(stderr,
                             "[script] status #%u null command block -> finished (ra=0x%08X, null-hits=%u)\n",
                             sCalls, GPR_U32(ctx, 31), sNullBlocks);
            }
            SET_GPR_S32(ctx, 2, 1);
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }

        if (scriptTraceEnabled() && (sCalls <= 16u || (sCalls % 500u) == 0u))
            scriptDump(rdram, ctx, runtime, "status ", sCalls, handle, 0u);
        if (gScriptStatusPrev != nullptr)
            gScriptStatusPrev(rdram, ctx, runtime);
        if (scriptTraceEnabled() && (sCalls <= 24u || (sCalls % 500u) == 0u))
            std::fprintf(stderr, "[script] status-ret #%u -> 0x%08X\n", sCalls, GPR_U32(ctx, 2));
    }

    void scriptPeekProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t handle = GPR_U64(ctx, 4);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (const char *mixPath = std::getenv("TOL_DUMP_MIX"))
        {
            static bool sMixDumped = false;
            if (!sMixDumped && sCalls == 8u)
            {
                sMixDumped = true;
                if (std::FILE *mf = std::fopen(mixPath, "wb"))
                {
                    for (uint32_t i = 0u; i < 0x81000u; ++i)
                    {
                        const uint8_t byte = runtime->Load8(rdram, ctx, 0x01523010u + i);
                        std::fwrite(&byte, 1u, 1u, mf);
                    }
                    std::fclose(mf);
                    std::fprintf(stderr, "[script] dumped MIX window 0x1523010..0x15A4010 -> %s\n", mixPath);
                }
            }
        }
        const bool log = scriptTraceEnabled() && (sCalls <= 16u || (sCalls % 500u) == 0u);
        if (log)
            scriptDump(rdram, ctx, runtime, "peek   ", sCalls, handle, 0u);
        if (gScriptPeekPrev != nullptr)
            gScriptPeekPrev(rdram, ctx, runtime);
        if (log)
            std::fprintf(stderr, "[script] peek   #%u -> opcode=0x%08X\n", sCalls, GPR_U32(ctx, 2));
    }

    void scriptAdvanceProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t handle = GPR_U64(ctx, 4);
        static uint32_t sCalls = 0u;
        ++sCalls;
        const bool log = scriptTraceEnabled() && (sCalls <= 16u || (sCalls % 500u) == 0u);
        if (log)
            scriptDump(rdram, ctx, runtime, "advance", sCalls, handle, 0u);
        if (gScriptAdvancePrev != nullptr)
            gScriptAdvancePrev(rdram, ctx, runtime);
        if (log)
        {
            const uint64_t after = GPR_U64(ctx, 2);
            std::fprintf(stderr, "[script] advance #%u -> handle=%016llX (index %u -> %u)\n", sCalls,
                         static_cast<unsigned long long>(after), static_cast<uint32_t>(handle >> 32),
                         static_cast<uint32_t>(after >> 32));
        }
    }

    // Which host thread drives the script dispatcher? FUN_001ECBE0 is called from FUN_001edf80 (the
    // 16-slot object loop), reached from FUN_001ee060, which the loader (FUN_00164f80) calls - and
    // FUN_00164ea0 creates that thread with entry 0x164f80 at priority 0. These probes name the tid.
    PS2Runtime::RecompiledFunction gDispatchPrev = nullptr;
    PS2Runtime::RecompiledFunction gSlotLoopPrev = nullptr;
    PS2Runtime::RecompiledFunction gLoaderEntryPrev = nullptr;
    PS2Runtime::RecompiledFunction gLoaderStartPrev = nullptr;
    uint32_t gDispatchCalls = 0u;

    void scriptDispatchProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t ra = GPR_U32(ctx, 31);
        const int tid = runtime->eeScheduler().currentThreadId();
        if (gDispatchPrev != nullptr)
            gDispatchPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 12u || (sCalls % 256u) == 0u)
            std::fprintf(stderr, "[script] dispatch #%u tid=%d ra=0x%08X\n", sCalls, tid, ra);
    }

    void scriptSlotLoopProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t ra = GPR_U32(ctx, 31);
        const uint32_t arg = GPR_U32(ctx, 4);
        const int tid = runtime->eeScheduler().currentThreadId();
        if (gSlotLoopPrev != nullptr)
            gSlotLoopPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 12u || (sCalls % 256u) == 0u)
        {
            std::fprintf(stderr, "[script] slot-loop #%u tid=%d ra=0x%08X a0=0x%08X slots:", sCalls, tid, ra, arg);
            for (uint32_t s = 0u; s < 4u; ++s)
            {
                std::fprintf(stderr, " [%u]", s);
                for (uint32_t f = 0u; f < 5u; ++f)
                    std::fprintf(stderr, " %08X", runtime->Load32(rdram, ctx, 0x004A5840u + s * 0x24u + f * 4u));
            }
            std::fprintf(stderr, "\n");
        }
    }

    PS2Runtime::RecompiledFunction gTaskFindPrev = nullptr;
    PS2Runtime::RecompiledFunction gLoaderPollPrev = nullptr;
    PS2Runtime::RecompiledFunction gLoopRunPrev = nullptr;
    PS2Runtime::RecompiledFunction gLoopE3b0Prev = nullptr;
    PS2Runtime::RecompiledFunction gLoopD230Prev = nullptr;

    void loopRunProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t prio = GPR_U32(ctx, 4);
        if (gLoopRunPrev != nullptr)
            gLoopRunPrev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (sCalls <= 300u)
            std::fprintf(stderr, "[loop] ff0(%u)\n", prio);
    }

    void loopE3b0Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gLoopE3b0Prev != nullptr)
            gLoopE3b0Prev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (sCalls <= 120u)
            std::fprintf(stderr, "[loop] e3b0\n");
    }

    void loopD230Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gLoopD230Prev != nullptr)
            gLoopD230Prev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (sCalls <= 120u)
            std::fprintf(stderr, "[loop] d230\n");
    }
    PS2Runtime::RecompiledFunction gReaderAvailPrev = nullptr;
    PS2Runtime::RecompiledFunction gReaderCmdPrev = nullptr;
    void loaderPollProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t job = GPR_U32(ctx, 4);
        uint32_t key = 0u;
        uint32_t jobWords[6] = {};
        if (job >= 0x00100000u && job < 0x02000000u)
        {
            key = runtime->Load32(rdram, ctx, job);
            for (uint32_t i = 0u; i < 6u; ++i)
                jobWords[i] = runtime->Load32(rdram, ctx, job + i * 4u);
        }
        if (gLoaderPollPrev != nullptr)
            gLoaderPollPrev(rdram, ctx, runtime);
        const uint32_t ret = GPR_U32(ctx, 2);
        static uint32_t sCalls = 0u;
        static uint32_t sLastJob = 0xFFFFFFFFu, sLastRet = 0xFFFFFFFFu;
        ++sCalls;
        if (job != sLastJob || ret != sLastRet || (sCalls % 500u) == 0u)
        {
            sLastJob = job;
            sLastRet = ret;
            std::fprintf(stderr, "[poll] #%u job=0x%08X busy=%u\n", sCalls, job, ret);
            if (job >= 0x00100000u && job < 0x02000000u)
            {
                std::fprintf(stderr, "[poll]   job words: %08X %08X %08X %08X %08X %08X (key=0x%08X)\n",
                             jobWords[0], jobWords[1], jobWords[2], jobWords[3], jobWords[4], jobWords[5], key);
                std::fprintf(stderr, "[poll]   blocks with +0x200 == key:");
                bool any = false;
                for (uint32_t i = 0u; i < 64u; ++i)
                {
                    const uint32_t b = 0x00407160u + i * 0x210u;
                    if (runtime->Load32(rdram, ctx, b + 0x200u) != key)
                        continue;
                    any = true;
                    char name[24] = {};
                    for (uint32_t c = 0u; c < sizeof(name) - 1u; ++c)
                    {
                        const uint8_t ch = runtime->Load8(rdram, ctx, b + 0x1D0u + c);
                        if (ch == 0u)
                            break;
                        name[c] = static_cast<char>(ch);
                    }
                    std::fprintf(stderr, " [%u]%s active=%u prio=%u", i, name,
                                 static_cast<unsigned>(runtime->Load8(rdram, ctx, b + 0x1F0u)),
                                 static_cast<unsigned>(runtime->Load8(rdram, ctx, b + 0x1F1u)));
                }
                if (!any)
                    std::fprintf(stderr, " (none)");
                std::fprintf(stderr, "  ret=0x%08X\n", ret);
            }
        }
    }

    void scriptReaderAvailProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t h = GPR_U32(ctx, 4);
        if (gReaderAvailPrev != nullptr)
            gReaderAvailPrev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        const uint32_t ret = GPR_U32(ctx, 2);
        if (ret != 0u || sCalls <= 20u || (sCalls % 500u) == 0u)
            std::fprintf(stderr, "[reader-avail] #%u h=0x%08X ret=0x%08X\n", sCalls, h, ret);
    }

    void scriptReaderCmdProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t h = GPR_U32(ctx, 4);
        if (gReaderCmdPrev != nullptr)
            gReaderCmdPrev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        const uint32_t ret = GPR_U32(ctx, 2);
        if (sCalls <= 40u || (sCalls % 500u) == 0u)
            std::fprintf(stderr, "[reader-cmd] #%u h=0x%08X id=0x%X\n", sCalls, h, ret);
    }
    PS2Runtime::RecompiledFunction gEnginePrev = nullptr;
    PS2Runtime::RecompiledFunction gFreePrev = nullptr;
    PS2Runtime::RecompiledFunction gAllocPrev = nullptr;
    PS2Runtime::RecompiledFunction gFree2Prev = nullptr;
    PS2Runtime::RecompiledFunction gDtorPrev = nullptr;
    PS2Runtime::RecompiledFunction gDtor2Prev = nullptr;
    void engineDtor2Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[enginedtor2] obj=0x%08X flag=0x%08X ra=0x%08X\n", GPR_U32(ctx, 4), GPR_U32(ctx, 5),
                     GPR_U32(ctx, 31));
        if (gDtor2Prev != nullptr)
            gDtor2Prev(rdram, ctx, runtime);
    }
    void engineDtorProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::fprintf(stderr, "[enginedtor] obj=0x%08X flag=0x%08X ra=0x%08X\n", GPR_U32(ctx, 4), GPR_U32(ctx, 5),
                     GPR_U32(ctx, 31));
        if (gDtorPrev != nullptr)
            gDtorPrev(rdram, ctx, runtime);
    }
    void heapFree2Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t arg = GPR_U32(ctx, 4);
        const uint32_t arg2 = GPR_U32(ctx, 5);
        if ((arg >= 0x004E9000u && arg <= 0x004EA800u) || (arg2 >= 0x004E9000u && arg2 <= 0x004EA800u))
        {
            std::fprintf(stderr, "[heapfree2] tid=%d a0=0x%08X a1=0x%08X ra=0x%08X\n", runtime->eeScheduler().currentThreadId(), arg, arg2, GPR_U32(ctx, 31));
            // Diagnostic: PCSX2 never frees this block. Refusing the free shows whether the delete is
            // the gate that stops the boot.
            if (std::getenv("TOL_REFUSE_ENGINE_FREE") != nullptr && arg == 0x004E9D70u)
            {
                static uint32_t sBlocked = 0u;
                if (sBlocked < 8u)
                {
                    ++sBlocked;
                    std::fprintf(stderr, "[refuse-free] blocked free(0x4E9D70) #%u ra=0x%08X\n", sBlocked,
                                 GPR_U32(ctx, 31));
                }
                return;
            }
            // Guest stack scan: $ra is unreliable (two table entries cover the same instructions), so
            // look for values on the stack that are real dispatch targets - those are return addresses.
            const uint32_t sp = GPR_U32(ctx, 29);
            std::fprintf(stderr, "[stackscan] sp=0x%08X\n", sp);
            for (int32_t off = -0x600; off < 0x140; off += 4)
            {
                const uint32_t v = runtime->Load32(rdram, ctx, sp + static_cast<uint32_t>(off));
                if (v >= 0x00100000u && v < 0x003D1CBCu && runtime->hasFunction(v))
                    std::fprintf(stderr, "[stackscan] sp%+d = 0x%08X\n", off, v);
            }
            std::fprintf(stderr, "[stackscan] raw sp-0x60..sp+0x30:");
            for (int32_t off = -0x60; off < 0x30; off += 4)
                std::fprintf(stderr, " %+d:%08X", off, runtime->Load32(rdram, ctx, sp + static_cast<uint32_t>(off)));
            std::fprintf(stderr, "\n");
        }
        if (gFree2Prev != nullptr)
            gFree2Prev(rdram, ctx, runtime);
    }

    void heapAllocProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t size = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        // Walk the allocator's free list just before the renderer's first 0x154 buffer request.
        if (size == 0x154u && std::getenv("TOL_DUMP_FREELIST") != nullptr)
        {
            static bool sDumped = false;
            if (!sDumped)
            {
                sDumped = true;
                const uint32_t root = runtime->Load32(rdram, ctx, 0x003DAF84u);
                const uint32_t head = runtime->Load32(rdram, ctx, root + 8u);
                std::fprintf(stderr, "[freelist] root=0x%08X head=0x%08X\n", root, head);
                uint32_t node = head;
                uint32_t guard = 0u;
                for (uint32_t n = 0u; n < 64u && node >= 0x00100000u && node < 0x02000000u; ++n)
                {
                    const uint32_t magic = runtime->Load32(rdram, ctx, node);
                    const uint32_t nodeSize = runtime->Load32(rdram, ctx, node + 0x10u);
                    const uint32_t prev = runtime->Load32(rdram, ctx, node + 0x14u);
                    const uint32_t next = runtime->Load32(rdram, ctx, node + 0x18u);
                    std::fprintf(stderr, "[freelist] node=0x%08X magic=0x%08X size=0x%X prev=0x%08X next=0x%08X\n",
                                 node, magic, nodeSize, prev, next);
                    if (next == guard || next == node)
                        break;
                    guard = node;
                    node = next;
                }
            }
        }
        if (gAllocPrev != nullptr)
            gAllocPrev(rdram, ctx, runtime);
        const uint32_t ret = GPR_U32(ctx, 2);
        if (std::getenv("TOL_TRACE_ALLOC") != nullptr)
        {
            static uint32_t sLogged = 0u;
            if (sLogged < 400u)
            {
                ++sLogged;
                std::fprintf(stderr, "[alloc] #%u size=0x%X ret=0x%08X ra=0x%08X\n", sLogged, size, ret, ra);
            }
        }
        if (ret >= 0x004E9D00u && ret <= 0x004EA400u)
            std::fprintf(stderr, "[heapalloc] size=0x%X ret=0x%08X ra=0x%08X\n", size, ret, ra);
    }

    // The engine object at 0x4E9D70 is released while the global at 0x3DB320 still points at it.
    // FUN_001008d0 is the underlying free; logging frees whose argument lands in its neighbourhood
    // names the caller that releases it.
    void heapFreeProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t arg = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        if (arg >= 0x004E9000u && arg <= 0x004EA800u)
            std::fprintf(stderr, "[heapfree] ptr=0x%08X ra=0x%08X caller's ra=0x%08X\n", arg, ra,
                         runtime->eeScheduler().currentThreadId());
        if (gFreePrev != nullptr)
            gFreePrev(rdram, ctx, runtime);
    }
    PS2Runtime::RecompiledFunction gEngineObjPrev = nullptr;

    // FUN_001ED0C0 returns the engine object; FUN_001EE060 calls vtable slot 0x1C on it to step the
    // script. Naming the object and its step entry says whether the engine is being advanced at all.
    void scriptEngineObjProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gEngineObjPrev != nullptr)
            gEngineObjPrev(rdram, ctx, runtime);
        const uint32_t obj = GPR_U32(ctx, 2);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (sCalls <= 8u || (sCalls % 500u) == 0u)
        {
            uint32_t vtable = 0u, step = 0u, f0 = 0u, f1 = 0u, f2 = 0u, f3 = 0u;
            if (obj >= 0x00100000u && obj < 0x02000000u)
            {
                vtable = runtime->Load32(rdram, ctx, obj);
                f0 = runtime->Load32(rdram, ctx, obj + 4u);
                f1 = runtime->Load32(rdram, ctx, obj + 8u);
                f2 = runtime->Load32(rdram, ctx, obj + 12u);
                f3 = runtime->Load32(rdram, ctx, obj + 16u);
                if (vtable >= 0x00100000u && vtable < 0x02000000u)
                    step = runtime->Load32(rdram, ctx, vtable + 0x1Cu);
            }
            std::fprintf(stderr, "[engineobj] #%u obj=0x%08X vt=0x%08X step=0x%08X fields=%08X %08X %08X %08X\n",
                         sCalls, obj, vtable, step, f0, f1, f2, f3);
        }
    }

    // FUN_001EE060 is the script engine's per-iteration entry. Its two globals gate the engine, and
    // FUN_001ED0C0 returns the object whose vtable slot 0x1C is called to step it, so logging them says
    // whether the engine is advancing or parked.
    void scriptEngineProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gEnginePrev != nullptr)
            gEnginePrev(rdram, ctx, runtime);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (std::getenv("TOL_DUMP_LOADER") != nullptr && sCalls >= 2u && sCalls <= 4u)
        {
            std::fprintf(stderr, "[tasks] #%u active tasks at 0x407160:", sCalls);
            for (uint32_t i = 0u; i < 64u; ++i)
            {
                const uint32_t b = 0x00407160u + i * 0x210u;
                const uint8_t busy = static_cast<uint8_t>(runtime->Load8(rdram, ctx, b + 0x1F0u));
                if (busy == 0u)
                    continue;
                const uint32_t prio = runtime->Load8(rdram, ctx, b + 0x1F1u);
                const uint32_t namePtr = b + 0x1D0u;
                char name[24] = {};
                for (uint32_t c = 0u; c < sizeof(name) - 1u; ++c)
                {
                    const uint8_t ch = runtime->Load8(rdram, ctx, namePtr + c);
                    if (ch == 0u)
                        break;
                    name[c] = static_cast<char>(ch);
                }
                std::fprintf(stderr, " [%u]=%s(p%u)", i, name, prio);
            }
            std::fprintf(stderr, "\n");
        }
        if (const char *dumpPath = std::getenv("TOL_DUMP_ENGINE"))
        {
            static bool sDumped = false;
            if (!sDumped && sCalls >= 1u)
            {
                sDumped = true;
                if (std::FILE *fp = std::fopen(dumpPath, "wb"))
                {
                    for (uint32_t i = 0u; i < 0x400u; ++i)
                    {
                        const uint8_t byte = runtime->Load8(rdram, ctx, 0x004E9D40u + i);
                        std::fwrite(&byte, 1u, 1u, fp);
                    }
                    std::fclose(fp);
                    std::fprintf(stderr, "[engine] dumped 0x4E9D40..0x4EA140 -> %s\n", dumpPath);
                }
            }
        }
        if (const char *scriptPath = std::getenv("TOL_DUMP_SCRIPT"))
        {
            static bool sScriptDumped = false;
            if (!sScriptDumped)
            {
                sScriptDumped = true;
                if (std::FILE *sf = std::fopen(scriptPath, "wb"))
                {
                    for (uint32_t i = 0u; i < 0x20000u; ++i)
                    {
                        const uint8_t byte = runtime->Load8(rdram, ctx, 0x01523010u + i);
                        std::fwrite(&byte, 1u, 1u, sf);
                    }
                    std::fclose(sf);
                    std::fprintf(stderr, "[engine] dumped script 0x1523010..0x1543010 -> %s\n", scriptPath);
                }
            }
        }
        if (sCalls <= 40u || (sCalls % 200u) == 0u)
        {
            const uint32_t g334 = runtime->Load32(rdram, ctx, 0x003DB334u);
            const uint32_t g328 = runtime->Load32(rdram, ctx, 0x003DB328u);
            const uint32_t sub = runtime->Load32(rdram, ctx, 0x003DB320u);
            uint32_t sub08 = 0u, sub0c = 0u, sub10 = 0u;
            uint8_t sub14 = 0u;
            if (sub >= 0x00100000u && sub < 0x02000000u)
            {
                const uint32_t renderer = runtime->Load32(rdram, ctx, sub + 0x0Cu);
                if (renderer >= 0x00100000u && renderer < 0x02000000u)
                {
                    sub08 = runtime->Load32(rdram, ctx, renderer + 0x08u);
                    sub0c = runtime->Load32(rdram, ctx, renderer + 0x0Cu);
                    sub10 = runtime->Load32(rdram, ctx, renderer + 0x10u);
                    sub14 = runtime->Load8(rdram, ctx, renderer + 0x14u);
                    const uint32_t h18 = runtime->Load32(rdram, ctx, renderer + 0x18u);
                    const uint32_t h1c = runtime->Load32(rdram, ctx, renderer + 0x1Cu);
                    const uint32_t h24 = runtime->Load32(rdram, ctx, renderer + 0x24u);
                    const uint32_t pend = runtime->Load32(rdram, ctx, renderer + 0x28u);
                    const uint32_t cnt34 = runtime->Load32(rdram, ctx, renderer + 0x34u);
                    const uint32_t mode = runtime->Load32(rdram, ctx, renderer + 0x158u);
                    const uint32_t st160 = runtime->Load32(rdram, ctx, renderer + 0x160u);
                    std::fprintf(stderr,
                                 "[engine] #%u script h18=0x%08X h1C=0x%08X reader=0x%08X pending=%u cnt34=%u "
                                 "mode=%u st160=%u\n",
                                 sCalls, h18, h1c, h24, pend, cnt34, mode, st160);
                    for (uint32_t k = 0u; k < 3u; ++k)
                    {
                        const uint32_t off = (k == 2u) ? 0x18u : (0x0Cu + k * 4u);
                        const uint32_t obj = runtime->Load32(rdram, ctx, sub + off);
                        uint32_t vt = 0u, step = 0u;
                        if (obj >= 0x00100000u && obj < 0x02000000u)
                        {
                            vt = runtime->Load32(rdram, ctx, obj);
                            if (vt >= 0x00100000u && vt < 0x02000000u)
                                step = runtime->Load32(rdram, ctx, vt + 0x1Cu);
                        }
                        std::fprintf(stderr, "[engine] #%u substep%u obj=0x%08X vt=0x%08X step1C=0x%08X\n",
                                     sCalls, k, obj, vt, step);
                    }
                    if (std::getenv("TOL_DUMP_LOADER") != nullptr && sCalls >= 2u && sCalls <= 4u)
                    {
                        std::fprintf(stderr, "[loader] globals 0x3DB1E0:");
                        for (uint32_t w = 0u; w < 16u; ++w)
                            std::fprintf(stderr, " %08X", runtime->Load32(rdram, ctx, 0x003DB1E0u + w * 4u));
                        std::fprintf(stderr, "\n[loader] table 0x497010:");
                        for (uint32_t w = 0u; w < 16u; ++w)
                            std::fprintf(stderr, " %08X", runtime->Load32(rdram, ctx, 0x00497010u + w * 4u));
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            std::fprintf(stderr, "[engine] #%u g334=0x%08X g328=0x%08X renderer +8=%08X +C=%08X +10=%08X +14=%u\n",
                         sCalls, g334, g328, sub08, sub0c, sub10, static_cast<unsigned>(sub14));
        }
    }

    // FUN_00152160 scans the 64 objects at 0x407160 (stride 0x210) for one whose byte +0x1F0 is set and
    // whose +0x200 equals its argument; FUN_001521c0 turns that into "the load task is still busy", which
    // is the condition FUN_00164F80's loader loop spins on. Naming the object says which subsystem the
    // boot is waiting for.
    void scriptTaskFindProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t arg = GPR_U32(ctx, 4);
        const uint32_t callerRa = GPR_U32(ctx, 31);
        if (gTaskFindPrev != nullptr)
            gTaskFindPrev(rdram, ctx, runtime);
        const uint32_t ret = GPR_U32(ctx, 2);
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (std::getenv("TOL_FORCE_TASK_IDLE") != nullptr && arg == 0x00407790u)
        {
            static uint32_t sForced = 0u;
            if (sForced < 3u)
            {
                ++sForced;
                std::fprintf(stderr, "[taskfind] forcing daemon block idle (call #%u)\n", sCalls);
            }
            SET_GPR_U32(ctx, 2, 0u);
            return;
        }
        if (sCalls <= 20u || (sCalls % 2000u) == 0u)
        {
            std::fprintf(stderr, "[taskfind] #%u arg=0x%08X ret=0x%08X ra=0x%08X", sCalls, arg, ret, callerRa);
            if (ret >= 0x00407000u && ret < 0x00410000u)
                std::fprintf(stderr, " slot=%u busy=%u id=0x%08X", (ret - 0x00407160u) / 0x210u,
                             runtime->Load8(rdram, ctx, ret + 0x1F0u), runtime->Load32(rdram, ctx, ret + 0x200u));
            std::fprintf(stderr, "\n");
        }
        if (sCalls == 12u)
        {
            std::fprintf(stderr, "[taskfind] script handle 0x015230A0:");
            for (uint32_t off = 0u; off < 0x20u; off += 4u)
                std::fprintf(stderr, " %08X", runtime->Load32(rdram, ctx, 0x015230A0u + off));
            std::fprintf(stderr, "\n");
            for (uint32_t slot = 2u; slot <= 3u; ++slot)
            {
                const uint32_t base = 0x00407160u + slot * 0x210u;
                std::fprintf(stderr, "[taskfind] --- slot %u @0x%08X ---\n", slot, base);
                for (uint32_t off = 0u; off < 0x40u; off += 16u)
                {
                    uint32_t w0 = runtime->Load32(rdram, ctx, base + off);
                    uint32_t w1 = runtime->Load32(rdram, ctx, base + off + 4u);
                    uint32_t w2 = runtime->Load32(rdram, ctx, base + off + 8u);
                    uint32_t w3 = runtime->Load32(rdram, ctx, base + off + 12u);
                    std::fprintf(stderr, "[taskfind]   +0x%02X: %08X %08X %08X %08X\n", off, w0, w1, w2, w3);
                }
                for (uint32_t off = 0x1B0u; off < 0x210u; off += 16u)
                {
                    uint32_t w0 = runtime->Load32(rdram, ctx, base + off);
                    uint32_t w1 = runtime->Load32(rdram, ctx, base + off + 4u);
                    uint32_t w2 = runtime->Load32(rdram, ctx, base + off + 8u);
                    uint32_t w3 = runtime->Load32(rdram, ctx, base + off + 12u);
                    std::fprintf(stderr, "[taskfind]   +0x%02X: %08X %08X %08X %08X\n", off, w0, w1, w2, w3);
                }
            }
        }
    }

    void scriptLoaderEntryProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)    {
        static uint32_t sCalls = 0u;
        const int tid = runtime->eeScheduler().currentThreadId();
        if (gLoaderEntryPrev != nullptr)
            gLoaderEntryPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 8u || (sCalls % 256u) == 0u)
            std::fprintf(stderr, "[script] loader-entry #%u tid=%d\n", sCalls, tid);
    }

    void scriptLoaderStartProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const int tid = runtime->eeScheduler().currentThreadId();
        if (gLoaderStartPrev != nullptr)
            gLoaderStartPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 8u || (sCalls % 256u) == 0u)
            std::fprintf(stderr, "[script] loader-start #%u tid=%d\n", sCalls, tid);
    }

    void installScriptThreadProbes(PS2Runtime &runtime)
    {
        if (!scriptTraceEnabled())
            return;
        const struct
        {
            uint32_t pc;
            PS2Runtime::RecompiledFunction probe;
            PS2Runtime::RecompiledFunction *prev;
            const char *name;
        } table[] = {
            {0x00152160u, &scriptTaskFindProbe, &gTaskFindPrev, "taskfind"},
            {0x00165420u, &loaderPollProbe, &gLoaderPollPrev, "poll"},
            {0x00151FF0u, &loopRunProbe, &gLoopRunPrev, "loop-ff0"},
            {0x0016E3B0u, &loopE3b0Probe, &gLoopE3b0Prev, "loop-e3b0"},
            {0x0016D230u, &loopD230Probe, &gLoopD230Prev, "loop-d230"},
            {0x001F88F0u, &scriptReaderAvailProbe, &gReaderAvailPrev, "reader-avail"},
            {0x001F8920u, &scriptReaderCmdProbe, &gReaderCmdPrev, "reader-cmd"},
            {0x001EE060u, &scriptEngineProbe, &gEnginePrev, "engine"},
            {0x001ED0C0u, &scriptEngineObjProbe, &gEngineObjPrev, "engineobj"},
            {0x001013C0u, &heapFreeProbe, &gFreePrev, "heapfree"},
            {0x00101420u, &heapAllocProbe, &gAllocPrev, "heapalloc"},
            {0x00101440u, &heapFree2Probe, &gFree2Prev, "heapfree2"},
            {0x00231E20u, &engineDtorProbe, &gDtorPrev, "enginedtor"},
            {0x00231ED0u, &engineDtor2Probe, &gDtor2Prev, "enginedtor2"},
            {0x001ECBE0u, &scriptDispatchProbe, &gDispatchPrev, "dispatch"},
            {0x001EDF80u, &scriptSlotLoopProbe, &gSlotLoopPrev, "slot-loop"},
            {0x00164F80u, &scriptLoaderEntryProbe, &gLoaderEntryPrev, "loader-entry"},
            {0x00164EA0u, &scriptLoaderStartProbe, &gLoaderStartPrev, "loader-start"},
        };
        for (const auto &entry : table)
        {
            PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(entry.pc);
            if (fn == nullptr)
            {
                std::fprintf(stderr, "[script] 0x%08X (%s) not in function table\n", entry.pc, entry.name);
                continue;
            }
            *entry.prev = fn;
            runtime.replaceFunction(entry.pc, entry.probe);
        }
        std::fprintf(stderr, "[script] thread probes installed (dispatch/slot-loop/loader)\n");
    }

    // ---------------------------------------------------------------------------------------------------------
    // PUMP HEALTH PROBE (TOL_TRACE_DVCI=1) - diagnostic.
    //
    // The extent-table loop is `while (FUN_00196460(i) != 3) FUN_0019A080();`. FUN_0019A080 -> FUN_001B7688(5)
    // dispatches slot 5; FUN_00198280 (inside the full pump FUN_00198230) calls FUN_00197940, the DVCI/ADXF pump,
    // which ends with the credit tick FUN_001973A8 (the only thing that can raise the handle to state 3).
    // FUN_00197940 early-returns while DAT_00393208 != 0, so sampling that guard *outside* the pump tells us
    // whether the pump is actually running or wedged.
    constexpr uint32_t kAdxfPumpPc = 0x00197940u;
    constexpr uint32_t kIoWaitPc = 0x0019A080u;
    constexpr uint32_t kDvciHandle = 0x00390738u;
    constexpr uint32_t kSlot5Table = 0x0049C790u;
    PS2Runtime::RecompiledFunction gAdxfPumpOriginal = nullptr;
    PS2Runtime::RecompiledFunction gIoWaitOriginal = nullptr;
    uint32_t gPumpCalls = 0u;
    uint32_t gPumpBlocked = 0u;

    void adxfPumpProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t guard = runtime->Load32(rdram, ctx, 0x00393208u);
        ++gPumpCalls;
        if (guard != 0u)
            ++gPumpBlocked;
        if (gPumpCalls <= 10u || (gPumpCalls % 500u) == 0u)
        {
            std::fprintf(stderr, "[adxf-pump] #%u guard=%u blocked=%u\n", gPumpCalls, guard, gPumpBlocked);
        }
        if (gAdxfPumpOriginal != nullptr)
            gAdxfPumpOriginal(rdram, ctx, runtime);

        // A completed pump walks the guard 1..6 and clears it. Anything else means the pump left the
        // state machine mid-sequence - either a checkpoint bail (the recompiled body returns and the
        // scheduler resumes at ctx->pc later) or a dropped invocation - and every later call then
        // early-returns at `if (DAT_00393208 != 0)`. Log the transitions, not every call.
        const uint32_t after = runtime->Load32(rdram, ctx, 0x00393208u);
        if (gPumpCalls == 60u || gPumpCalls == 2000u)
        {
            if (const char *dumpPath = std::getenv("TOL_DUMP_VRAM"))
            {
                char scriptPath[512];
                std::snprintf(scriptPath, sizeof(scriptPath), "%s_script.bin", dumpPath);
                if (std::FILE *sf = std::fopen(scriptPath, "wb"))
                {
                    for (uint32_t i = 0u; i < 0x81000u; ++i)
                    {
                        const uint8_t byte = runtime->Load8(rdram, ctx, 0x01520000u + i);
                        std::fwrite(&byte, 1u, 1u, sf);
                    }
                    std::fclose(sf);
                    std::fprintf(stderr, "[adxf] script window dumped -> %s\n", scriptPath);
                }
                char destPath[512];
                std::snprintf(destPath, sizeof(destPath), "%s_dest.bin", dumpPath);
                if (std::FILE *df = std::fopen(destPath, "wb"))
                {
                    for (uint32_t i = 0u; i < 0x1000u; ++i)
                    {
                        const uint8_t byte = runtime->Load8(rdram, ctx, 0x005C64C0u + i);
                        std::fwrite(&byte, 1u, 1u, df);
                    }
                    std::fclose(df);
                    std::fprintf(stderr, "[adxf] load dest dumped -> %s\n", destPath);
                }
            }
        }
        static uint32_t sLastAfter = 0u;
        static uint32_t sLeaks = 0u;
        if (after != sLastAfter)
        {
            if (sLeaks < 40u)
            {
                std::fprintf(stderr, "[adxf-guard] #%u before=%u after=%u pc=0x%08X ra=0x%08X sp=0x%08X\n",
                             gPumpCalls, guard, after, ctx->pc, GPR_U32(ctx, 31), GPR_U32(ctx, 29));
                ++sLeaks;
            }
            sLastAfter = after;
        }
    }

    void ioWaitProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        ++sCalls;
        if (sCalls <= 10u || (sCalls % 10u) == 0u)
        {
            std::fprintf(stderr, "[io-wait] #%u guard=%u gate=%08X slot5:", sCalls,
                         runtime->Load32(rdram, ctx, 0x00393208u), runtime->Load32(rdram, ctx, 0x0039E838u));
            for (int i = 0; i < 6; ++i)
                std::fprintf(stderr, " %08X", runtime->Load32(rdram, ctx, kSlot5Table + static_cast<uint32_t>(i) * 8u));
            std::fprintf(stderr, " | h1=%u hC=%u h14=%u | s1=%u s58=%u\n",
                         runtime->Load8(rdram, ctx, kDvciHandle + 1u),
                         runtime->Load32(rdram, ctx, kDvciHandle + 0xCu),
                         runtime->Load32(rdram, ctx, kDvciHandle + 0x14u),
                         runtime->Load8(rdram, ctx, kMapAfsStream + 1u),
                         runtime->Load32(rdram, ctx, kMapAfsStream + 0x58u));
        }
        if (sCalls == 60u)
        {
            std::fprintf(stderr, "[adxf-slots] all streams at wait #%u:\n", sCalls);
            for (int i = 0; i < 40; ++i)
            {
                const uint32_t s = 0x0039E848u + static_cast<uint32_t>(i) * 0x60u;
                const uint32_t active = runtime->Load8(rdram, ctx, s);
                if (active == 0u)
                    continue;
                std::fprintf(stderr,
                             "[adxf-slots]  [%2d] 0x%08X act=%u st=%u busy=%u 45=%u 46=%u 47=%u 48=%u 49=%u"
                             " 08=%08X 14=%u 58=%u 5C=%u path=0x%08X\n",
                             i, s, active, runtime->Load8(rdram, ctx, s + 1u), runtime->Load8(rdram, ctx, s + 2u),
                             runtime->Load8(rdram, ctx, s + 0x45u), runtime->Load8(rdram, ctx, s + 0x46u),
                             runtime->Load8(rdram, ctx, s + 0x47u), runtime->Load8(rdram, ctx, s + 0x48u),
                             runtime->Load8(rdram, ctx, s + 0x49u), runtime->Load32(rdram, ctx, s + 8u),
                             runtime->Load32(rdram, ctx, s + 0x14u), runtime->Load32(rdram, ctx, s + 0x58u),
                             runtime->Load32(rdram, ctx, s + 0x5Cu), runtime->Load32(rdram, ctx, s + 0x50u));
            }
        }
        if (gIoWaitOriginal != nullptr)
            gIoWaitOriginal(rdram, ctx, runtime);
    }

    // ---------------------------------------------------------------------------------------------------------
    // GAME READ CLIENT PROBE (TOL_TRACE_DVCI=1) - diagnostic.
    //
    // FUN_001029a8 is the game's block-read client (sid 0x80000595 fn 1). The SJRBF request machine
    // FUN_001ab4f8 turns state 1 into state 9 (device error) exactly when this returns anything but 1, and the
    // DVCI completion poll FUN_001ad220 then latches the entry to 3, which is the ADXF "discard" branch.
    constexpr uint32_t kGameReadPc = 0x001029A8u;
    PS2Runtime::RecompiledFunction gGameReadOriginal = nullptr;

    void gameReadProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t handle = GPR_U32(ctx, 4);
        const uint32_t blocks = GPR_U32(ctx, 5);
        const uint32_t buffer = GPR_U32(ctx, 6);
        const uint32_t desc = GPR_U32(ctx, 7);
        const uint32_t ra = GPR_U32(ctx, 31);

        if (gGameReadOriginal != nullptr)
            gGameReadOriginal(rdram, ctx, runtime);

        const uint32_t ret = GPR_U32(ctx, 2);
        static uint32_t sReads = 0u;
        if (++sReads <= 60u)
        {
            const uint8_t mode = (desc >= 0x00100000u && desc < 0x02000000u)
                                     ? runtime->Load8(rdram, ctx, desc + 2u)
                                     : 0u;
            std::fprintf(stderr, "[cri] gameread handle=%u blocks=%u buf=0x%08X mode=%u ra=0x%08X -> %u\n", handle,
                         blocks, buffer, mode, ra, ret);
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // CRI PUMP PLACEMENT - the main-thread pump must also service the CRI server.
    //
    // FUN_001982a0 (the ADXT/CRI init) registers the pumps that FUN_001b7688 dispatches when the game calls
    // FUN_0019a080 from inside a wait loop:
    //
    //   single-threaded  slot 5 = FUN_00198230 = FUN_00198260 + FUN_00198280 (CRI server) + FUN_00198210 (SJRBF)
    //   with a worker    slot 5 = FUN_00198210 (SJRBF only), slot 4 = FUN_00198280 (CRI server, on the worker)
    //
    // This title takes the threaded path (FUN_00199428 really creates the worker and it pumps ~310 times), but
    // the loading sequence busy-waits: `while (FUN_00196460(i) != 3) FUN_0019a080();` never yields, and once the
    // worker stops the stream step FUN_0019F298 is never called again. Measured at the stall: the step counter
    // frozen at 310 across 268,000 polls, DAT_0039E838 (the ABXR gate) = 0, the file device idle, the ADXF
    // stream left at state 2 with 0 bytes done - so the DVCI handle can never be credited to 3 and the poll can
    // never return 3. It is the same wall as "the second block never completes", one layer up.
    //
    // The library already contains the single-threaded pump, so use it: slot 5 is registered with the full
    // FUN_00198230 instead of the SJRBF-only FUN_00198210. FUN_00197940 has a re-entrancy guard
    // (DAT_00393208), so a worker that is still alive cannot be corrupted by the main thread pumping too.
    constexpr uint32_t kPumpRegisterPc = 0x001B7378u;
    constexpr uint32_t kSjrbfOnlyPump = 0x00198210u;
    constexpr uint32_t kFullMainThreadPump = 0x00198230u;
    PS2Runtime::RecompiledFunction gPumpRegisterOriginal = nullptr;

    void pumpRegisterProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t slot = GPR_U32(ctx, 4);
        const uint32_t function = GPR_U32(ctx, 5);
        if (slot == 5u && function == kSjrbfOnlyPump)
        {
            SET_GPR_U32(ctx, 5, kFullMainThreadPump);
            static bool sLogged = false;
            if (!sLogged)
            {
                sLogged = true;
                std::fprintf(stderr,
                             "[cri] main-thread pump slot 5: 0x%08X -> 0x%08X (the full CRI server pump; this "
                             "port's main thread never yields to the worker)\n",
                             kSjrbfOnlyPump, kFullMainThreadPump);
            }
        }
        if (gPumpRegisterOriginal != nullptr)
            gPumpRegisterOriginal(rdram, ctx, runtime);
    }

    // NOTE: FUN_0019EE18(stream, sectors) used to be wrapped here (TOL_CRI_WINDOW_FIX) to force the stream
    // window to one request, because stream[+0x14] stayed 0 and FUN_0019EF58 could never see its window
    // satisfied. That window value was a symptom of the size bug (the open reply did not carry the file size)
    // and of the pump bug fixed above. With both fixed the guest sets the window itself and all six archives
    // load without the shim; forcing it instead overwrote the real total (1 sector instead of 436,462 for
    // MOVIE.AFS), i.e. it made correct state wrong. Removed.

    void dvciPollProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        static uint32_t sSawThree = 0u;
        static uint32_t sSawCount = 0u;
        static uint32_t sLoadFive = 0u;
        const uint32_t call = ++sCalls;

        // Entry state, before the original runs: this is exactly what the poll's own ptid/state checks see.
        const uint32_t indexAtEntry = GPR_U32(ctx, 4);
        const uint32_t handleAtEntry = runtime->Load32(rdram, ctx, kDvciHandlePtr);
        uint32_t stateAtEntry = 0xFFu;
        if (handleAtEntry >= 0x00100000u && handleAtEntry < 0x02000000u)
            stateAtEntry = runtime->Load8(rdram, ctx, handleAtEntry + 1u);
        uint32_t countAtEntry = 0u;
        if (indexAtEntry < 16u)
        {
            const uint32_t rec = runtime->Load32(rdram, ctx, kDvciRequestTable + indexAtEntry * 4u);
            if (rec >= 0x00100000u && rec < 0x02000000u)
                countAtEntry = runtime->Load32(rdram, ctx, rec + 8u);
        }
        uint32_t totalAtEntry = 0u;
        uint32_t transferAtEntry = 0u;
        uint32_t consumedAtEntry = 0u;
        if (handleAtEntry >= 0x00100000u && handleAtEntry < 0x02000000u)
        {
            totalAtEntry = runtime->Load32(rdram, ctx, handleAtEntry + 0x0Cu);
            transferAtEntry = runtime->Load32(rdram, ctx, handleAtEntry + 0x1Cu);
            consumedAtEntry = runtime->Load32(rdram, ctx, handleAtEntry + 0x14u);
        }
        const uint32_t progressAtEntry = runtime->Load32(rdram, ctx, kDvciProgress);
        const uint32_t ptidAtEntry = runtime->Load32(rdram, ctx, kDvciLoadIndex);

        // The other half of the comparison the guest makes: FUN_00197668 returns handle[+1], which the credit
        // tick FUN_001972E0 copies from the ADXF stream (handle[+4], stream[+1]). Reading the stream here - at
        // the exact instant the poll samples the handle - is what separates "the poll is early" from "the
        // stream is not where the handle says it is". devBase names the file the stream is really opened on
        // (open hands out slot+1 * 0x100000), so a mismatch is visible immediately.
        uint32_t streamPtr = 0u;
        uint32_t streamState = 0u;
        uint32_t streamTotal = 0u;
        uint32_t streamDoneBytes = 0u;
        uint32_t streamBlocks = 0u;
        uint32_t streamWindow = 0u;
        uint32_t streamDeviceBase = 0u;
        uint32_t streamDeviceState = 0u;
        if (handleAtEntry >= 0x00100000u && handleAtEntry < 0x02000000u)
        {
            streamPtr = runtime->Load32(rdram, ctx, handleAtEntry + 4u);
            if (streamPtr >= 0x00100000u && streamPtr < 0x02000000u && (streamPtr & 3u) == 0u)
            {
                streamState = runtime->Load8(rdram, ctx, streamPtr + 1u);
                streamTotal = runtime->Load32(rdram, ctx, streamPtr + 0x14u);
                streamDoneBytes = runtime->Load32(rdram, ctx, streamPtr + 0x34u);
                streamBlocks = runtime->Load32(rdram, ctx, streamPtr + 0x58u);
                streamWindow = runtime->Load32(rdram, ctx, streamPtr + 0x5Cu);
                const uint32_t fileObject = runtime->Load32(rdram, ctx, streamPtr + 8u);
                if (fileObject >= 0x00100000u && fileObject < 0x02000000u && (fileObject & 3u) == 0u)
                {
                    const uint32_t deviceHandle = runtime->Load32(rdram, ctx, fileObject + 4u);
                    if (deviceHandle >= 0x00100000u && deviceHandle < 0x02000000u)
                    {
                        streamDeviceBase = runtime->Load32(rdram, ctx, deviceHandle + 0x20u);
                        streamDeviceState = runtime->Load8(rdram, ctx, deviceHandle + 2u);
                    }
                }
            }
        }

        if (stateAtEntry == 3u)
            ++sSawThree;
        if (countAtEntry != 0u)
            ++sSawCount;

        if (call <= 6u || (stateAtEntry == 3u && sSawThree <= 20u) || (indexAtEntry == 5u && sLoadFive++ < 10u) ||
            (call % 32u) == 0u)
        {
            std::fprintf(stderr,
                         "[poll-in] #%u frame=%u i=%u ptid=%d state=%u count=%u progress=%u total=%u consumed=%u transfer=%u"
                         " | sState=%u sStream=0x%08X sTotal=%u sDone=%u sBlocks=%u sWin=%u sDevBase=0x%X sDev=%u"
                         " | saw3=%u sawCount=%u\n",
                         call, gFrameTicks, indexAtEntry, static_cast<int32_t>(ptidAtEntry), stateAtEntry, countAtEntry,
                         progressAtEntry, totalAtEntry, consumedAtEntry, transferAtEntry, streamState, streamPtr,
                         streamTotal, streamDoneBytes, streamBlocks, streamWindow, streamDeviceBase,
                         streamDeviceState, sSawThree, sSawCount);
        }
        // Periodic line: the freeze happens with state != 3, so the lines above never fire once it sticks.
        if ((call % 512u) == 0u)
        {
            const uint32_t streamBusy = (streamPtr >= 0x00100000u && streamPtr < 0x02000000u)
                                            ? runtime->Load8(rdram, ctx, streamPtr + 2u)
                                            : 0u;
            const uint32_t streamOpen = (streamPtr >= 0x00100000u && streamPtr < 0x02000000u)
                                            ? runtime->Load8(rdram, ctx, streamPtr + 0x45u)
                                            : 0u;
            const uint32_t streamActive = (streamPtr >= 0x00100000u && streamPtr < 0x02000000u)
                                              ? runtime->Load8(rdram, ctx, streamPtr + 0x49u)
                                              : 0u;
            // FUN_0019F468 refuses to walk the stream table while DAT_0039E838 is held, and the main-thread pump
            // slot 5 only runs FUN_00198210 (SJRBF) when the guest took its threaded path. Both make the step
            // counter stand still, so report the gate and the registered pump entry points.
            const uint32_t abxrGate = runtime->Load32(rdram, ctx, 0x0039E838u);
            const uint32_t slot5Pump = runtime->Load32(rdram, ctx, 0x0049C6A0u + 5u * 0x30u);
            const uint32_t slot4Pump = runtime->Load32(rdram, ctx, 0x0049C6A0u + 4u * 0x30u);
            std::fprintf(stderr,
                         "[poll-stuck] #%u i=%u ptid=%d state=%u count=%u progress=%u total=%u consumed=%u"
                         " | sState=%u sTotal=%u sDone=%u sBlocks=%u sWin=%u sDevBase=0x%X sDev=%u"
                         " | sBusy=%u sOpen=%u sAct=%u steps=%u gate=%u slot5=0x%08X slot4=0x%08X\n",
                         call, indexAtEntry, static_cast<int32_t>(ptidAtEntry), stateAtEntry, countAtEntry,
                         progressAtEntry, totalAtEntry, consumedAtEntry, streamState, streamTotal, streamDoneBytes,
                         streamBlocks, streamWindow, streamDeviceBase, streamDeviceState, streamBusy, streamOpen,
                         streamActive, gAdxfStepCalls, abxrGate, slot5Pump, slot4Pump);
        }

        const bool log = call <= 16u || (call % 512u) == 0u || indexAtEntry == 5u;
        if (gDvciPollOriginal != nullptr)
            gDvciPollOriginal(rdram, ctx, runtime);

        if (log)
        {
            dumpDvci(rdram, ctx, runtime, call, static_cast<int32_t>(GPR_U32(ctx, 2)));
        }
    }

    // DVCI completion fix. FUN_00196460 (the poll) parses the AFS extent table and is
    // supposed to return 3 when handle[8] <= progress. For the small MOVIE.AFS table
    // (22 entries, blocks=1) the recompiled flow falls through to the next-read path
    // and returns the idle stream state (0) without calling FUN_001963a8, so the
    // loading loop in FUN_001664D0 (`while (FUN_00196460(i) != 3)`) never advances
    // past i=5. Once the table has been parsed (progress >= the request's entry
    // count), finish the load the way FUN_001963a8 does and report done.
    PS2Runtime::RecompiledFunction gDvciCompletionPrev = nullptr;

    void dvciCompletionFix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t index = GPR_U32(ctx, 4);
        if (gDvciCompletionPrev != nullptr)
            gDvciCompletionPrev(rdram, ctx, runtime);

        const int32_t result = static_cast<int32_t>(GPR_U32(ctx, 2));
        const uint32_t active = runtime->Load32(rdram, ctx, kDvciLoadIndex);
        const uint32_t handle =
            (index < 16u) ? runtime->Load32(rdram, ctx, kDvciRequestTable + index * 4u) : 0u;
        uint32_t count = 0u;
        if (handle >= 0x00100000u && handle < 0x02000000u)
            count = runtime->Load32(rdram, ctx, handle + 8u);
        const uint32_t progress = runtime->Load32(rdram, ctx, kDvciProgress);
        if (std::getenv("TOL_TRACE_DVCI") != nullptr)
        {
            static uint32_t sFixLogs = 0u;
            ++sFixLogs;
            const bool willComplete =
                (result != 3 && active == index && handle >= 0x00100000u && handle < 0x02000000u &&
                 count != 0u && progress >= count);
            if (willComplete || index == 5u || sFixLogs <= 20u || (sFixLogs % 500u) == 0u)
            {
                std::fprintf(stderr,
                             "[dvci-fix] #%u idx=%u active=%u result=%d count=%u progress=%u done=%d\n",
                             sFixLogs, index, active, result, count, progress, willComplete ? 1 : 0);
            }
        }

        if (result == 3 || index >= 16u)
            return;
        if (active != index)
            return;
        if (handle < 0x00100000u || handle >= 0x02000000u)
            return;
        if (count == 0u || progress < count)
            return;

        if (std::getenv("TOL_TRACE_DVCI") != nullptr)
            std::fprintf(stderr, "[dvci-done] idx=%u count=%u progress=%u\n", index, count, progress);

        runtime->Store32(rdram, ctx, kDvciHandlePtr, 0u);
        runtime->Store32(rdram, ctx, kDvciLoadIndex, 0xFFFFFFFFu);
        runtime->Store32(rdram, ctx, kDvciProgress, 0u);
        runtime->Store32(rdram, ctx, kDvciBlocks, 0u);
        SET_GPR_S32(ctx, 2, 3);
    }
}

namespace tol
{
    // ---------------------------------------------------------------------------------------------------------
    // WORKER 2 BACK-EDGE (fix).
    //
    // 0x1F84B0 is the transfer-queue worker created as a guest thread:
    //     while (queue == 0) yield;
    //     lock; dispatch(queue); reap; unlock;        (loop forever)
    // Its prologue is `addiu sp,sp,-0x20 / sd ra,0x10(sp) / sq s0,0(sp)`. This port enters guest thread
    // entries only once, so the infinite loop used to be bounded by overriding the *entry* - which made
    // every call inside the loop a host call, and a cooperative bail inside the long transfer handler
    // (0x1F8530) unwound past frames host code could not resume (that is why the coroutine experiment in
    // session 91 stalled).
    //
    // Bound it at the two back edges instead:
    //     0x1F8518  b 0x1F84C0   (work done)
    //     0x1F8528  b 0x1F84BC   (idle iteration)
    // Running the real epilogue there returns to the caller after exactly one iteration, while the loop
    // body itself executes as ordinary guest code - lock/handler/reap/unlock are real `jal`s, so
    // dispatchGuestBranch owns the bail and resume bookkeeping and 0x1F8530 resumes at its preemption
    // point instead of restarting.
    void worker2BackEdge(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        // Restore the saved frame: $s0 at sp+0x00, $ra at sp+0x10, frame size 0x20.
        SET_GPR_VEC(ctx, 16, READ128(sp));
        const uint32_t ra = READ32(sp + 0x10u);
        SET_GPR_U32(ctx, 29, sp + 0x20u);
        SET_GPR_U32(ctx, 31, ra);
        ctx->pc = ra;
    }

    void installWorker2BackEdge(PS2Runtime &runtime)
    {
        runtime.replaceFunction(0x001F8518u, &worker2BackEdge);
        runtime.replaceFunction(0x001F8528u, &worker2BackEdge);
    }

    // The GS/transfer workers (0x1F8150, 0x1F84B0) are **green threads of the game's own microkernel**
    // (0x1517E0/0x1518C0/0x151CD0, TCBs at 0x407160, contexts at 0x48F590). That microkernel is alive in this
    // port: 0x152200 restores a thread's sp/registers and jumps to its saved pc, 0x15231C saves the current
    // thread and jumps back into the run loop at 0x151F88. The three host-side pumps below were written before
    // that was known and they *mask* it:
    //   * the 0x1F8150 entry override returns to $ra, but a microkernel-dispatched thread has no meaningful $ra
    //     (it is entered by 0x152200, not by a jal), so the thread dies at pc 0;
    //   * worker2BackEdge forces the same early return at 0x1F8518/0x1F8528;
    //   * ioPumpProbe re-enters 0x1F84B0 from the main stack instead of letting the microkernel dispatch it;
    //   * yieldPump runs the save and then discards the yield by restoring $ra.
    // Verified with TOL_TRACE_EE_THREADS=1: thread 1 (main) went Dormant with pc=0 inside the worker-2 loop while
    // the ADX thread spun at 0x198C88. Let the guest body and the microkernel drive them by default;
    // TOL_LEGACY_WORKER_PUMPS=1 restores the old host pumps for A/B.
    bool legacyWorkerPumpsEnabled()
    {
        static const bool enabled = std::getenv("TOL_LEGACY_WORKER_PUMPS") != nullptr;
        return enabled;
    }

    // ---------------------------------------------------------------------------------------------------------
    // GREEN-THREAD (game microkernel) STATE DUMP - diagnostic, TOL_TRACE_GTHREADS=1.
    //
    // The game's own microkernel owns the GS/transfer workers: TCBs at 0x407160 stride 0x210 (state +0x1F0,
    // priority +0x1F1, entry +0x1B0, arg/self +0x1C0, started +0x1F3), a run-queue linked list through
    // +0x1F8/+0x1FC, and per-priority ready counters at 0x48F570. FUN_00151E90 walks that list; a thread that
    // yields (FUN_0015231C) returns to the run loop at 0x151F88, and the run loop then *deletes* the thread
    // unless TCB+0x1AC is non-zero. Print the table so a disappearing thread is visible instead of inferred.
    constexpr uint32_t kGreenTcbBase = 0x00407160u;
    constexpr uint32_t kGreenTcbStride = 0x210u;
    constexpr uint32_t kGreenTcbCount = 64u;
    constexpr uint32_t kGreenReadyCounts = 0x0048F570u;

    void dumpGreenThreads(uint8_t *rdram, PS2Runtime *runtime, R5900Context *ctx, const char *tag)
    {
        std::fprintf(stderr, "[gthreads] %s ready0=%u ready1=%u\n", tag,
                     runtime->Load16(rdram, ctx, kGreenReadyCounts),
                     runtime->Load16(rdram, ctx, kGreenReadyCounts + 2u));
        for (uint32_t i = 0; i < kGreenTcbCount; ++i)
        {
            const uint32_t tcb = kGreenTcbBase + i * kGreenTcbStride;
            const uint32_t entry = runtime->Load32(rdram, ctx, tcb + 0x1B0u);
            const uint32_t state = runtime->Load8(rdram, ctx, tcb + 0x1F0u);
            const uint32_t prio = runtime->Load8(rdram, ctx, tcb + 0x1F1u);
            const uint32_t started = runtime->Load8(rdram, ctx, tcb + 0x1F3u);
            const uint32_t yieldFlag = runtime->Load32(rdram, ctx, tcb + 0x1ACu);
            const uint32_t resumePc = runtime->Load32(rdram, ctx, tcb + 0x1B4u);
            if (entry == 0u && state == 0u)
                continue;
            std::fprintf(stderr,
                         "[gthreads]   slot=%2u tcb=0x%08X state=%u prio=%u started=%u yield=%u entry=0x%08X "
                         "resume=0x%08X\n",
                         i, tcb, state, prio, started, yieldFlag, entry, resumePc);
        }
    }

    // Wraps FUN_0015231C (the yield) so the dump is taken exactly at the point where the run loop decides whether
    // to delete the thread, and prints the context pointer the save actually wrote through.
    constexpr uint32_t kGreenYieldPc = 0x0015231Cu;
    constexpr uint32_t kGreenEnterPc = 0x00152200u;
    constexpr uint32_t kGreenIndexA = 0x003DB1A0u;
    constexpr uint32_t kGreenIndexB = 0x003DB1B0u;
    constexpr uint32_t kGreenIndexC = 0x003DB1C0u;
    constexpr uint32_t kGreenContextArray = 0x0048F590u;
    constexpr uint32_t kGreenTransferHead = 0x003DB390u;
    PS2Runtime::RecompiledFunction gGreenYieldPrev = nullptr;
    PS2Runtime::RecompiledFunction gGreenEnterPrev = nullptr;
    // Set by the transfer waiter (first tell poll) so the dispatch probe logs every turn during the wait,
    // and the TCB table is dumped at the boundaries rather than only at the first twelve yields.
    bool gTransferWaitActive = false;
    uint32_t gWaitTurnLogs = 0u;

    // FUN_00152200 is the "enter thread" half of the microkernel switch: FUN_00151E90 stores the chosen TCB at
    // scheduler-context+0x1C0 (0x48F920) and calls this, which zeroes TCB+0x1AC, restores the thread's registers
    // and jumps to TCB+0x1B4. Logging it names the dispatch order and the resume pc of every turn.
    void greenEnterProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sTurns = 0u;
        ++sTurns;
        if (sTurns <= 40u || (sTurns % 200u) == 0u)
        {
            const uint32_t a = runtime->Load32(rdram, ctx, kGreenIndexA);
            const uint32_t b = runtime->Load32(rdram, ctx, kGreenIndexB);
            const uint32_t c = runtime->Load32(rdram, ctx, kGreenIndexC);
            const uint32_t tcb = runtime->Load32(rdram, ctx, kGreenContextArray + a * b + c);
            const bool readable = tcb >= 0x00100000u && tcb < PS2_RAM_SIZE;
            const uint32_t resume = readable ? runtime->Load32(rdram, ctx, tcb + 0x1B4u) : 0u;
            const uint32_t state = readable ? runtime->Load8(rdram, ctx, tcb + 0x1F0u) : 0u;
            const uint32_t queue = runtime->Load32(rdram, ctx, kGreenTransferHead);
            std::fprintf(stderr,
                         "[gthreads] turn #%u tcb=0x%08X state=%u resume=0x%08X xferQueue=0x%08X ready0=%u ra=0x%08X\n",
                         sTurns, tcb, state, resume, queue,
                         runtime->Load16(rdram, ctx, kGreenReadyCounts), GPR_U32(ctx, 31));
        }
        // While the transfer waiter is parked, every dispatch matters: this is the window in which a pump
        // thread would have to be scheduled.
        if (gTransferWaitActive && gWaitTurnLogs < 120u)
        {
            const uint32_t a = runtime->Load32(rdram, ctx, kGreenIndexA);
            const uint32_t b = runtime->Load32(rdram, ctx, kGreenIndexB);
            const uint32_t c = runtime->Load32(rdram, ctx, kGreenIndexC);
            const uint32_t tcb = runtime->Load32(rdram, ctx, kGreenContextArray + a * b + c);
            const bool readable = tcb >= 0x00100000u && tcb < PS2_RAM_SIZE;
            ++gWaitTurnLogs;
            std::fprintf(stderr,
                         "[wait-turn] #%u tcb=0x%08X entry=0x%08X state=%u prio=%u resume=0x%08X ready1=%u\n",
                         sTurns, tcb, readable ? runtime->Load32(rdram, ctx, tcb + 0x1B0u) : 0u,
                         readable ? runtime->Load8(rdram, ctx, tcb + 0x1F0u) : 0u,
                         readable ? runtime->Load8(rdram, ctx, tcb + 0x1F1u) : 0u,
                         readable ? runtime->Load32(rdram, ctx, tcb + 0x1B4u) : 0u,
                         runtime->Load16(rdram, ctx, kGreenReadyCounts + 2u));
        }
        if (gGreenEnterPrev != nullptr)
            gGreenEnterPrev(rdram, ctx, runtime);
    }

    void greenYieldProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t a = runtime->Load32(rdram, ctx, kGreenIndexA);
        const uint32_t b = runtime->Load32(rdram, ctx, kGreenIndexB);
        const uint32_t c = runtime->Load32(rdram, ctx, kGreenIndexC);
        const uint32_t offset = a * b + c;
        const uint32_t block = runtime->Load32(rdram, ctx, kGreenContextArray + offset);
        const bool readable = block >= 0x00100000u && block < PS2_RAM_SIZE;
        static uint32_t sYields = 0u;
        ++sYields;
        if (sYields <= 12u || (sYields % 200u) == 0u)
        {
            std::fprintf(stderr,
                         "[gthreads] yield #%u A=0x%08X B=0x%08X C=0x%08X offset=0x%X block=0x%08X ra=0x%08X "
                         "sp=0x%08X schedPc=%s0x%08X resumePc=%s0x%08X\n",
                         sYields, a, b, c, offset, block, GPR_U32(ctx, 31), GPR_U32(ctx, 29),
                         readable ? "" : "(?)", readable ? runtime->Load32(rdram, ctx, block + 0x1A8u) : 0u,
                         readable ? "" : "(?)", readable ? runtime->Load32(rdram, ctx, block + 0x1B4u) : 0u);
        }
        if (gGreenYieldPrev != nullptr)
            gGreenYieldPrev(rdram, ctx, runtime);
        if (sYields <= 12u || (sYields % 200u) == 0u)
        {
            std::fprintf(stderr, "[gthreads] yield #%u -> pc=0x%08X ra=0x%08X sp=0x%08X\n", sYields, ctx->pc,
                         GPR_U32(ctx, 31), GPR_U32(ctx, 29));
        }
        if (sYields <= 12u && std::getenv("TOL_TRACE_GTHREADS") != nullptr)
            dumpGreenThreads(rdram, runtime, ctx, "after-yield");
    }

    void installGreenThreadProbe(PS2Runtime &runtime)
    {
        if (std::getenv("TOL_TRACE_GTHREADS") == nullptr)
            return;
        PS2Runtime::RecompiledFunction yieldFn = runtime.lookupFunction(kGreenYieldPc);
        if (yieldFn == nullptr)
            return;
        gGreenYieldPrev = yieldFn;
        runtime.replaceFunction(kGreenYieldPc, &greenYieldProbe);

        PS2Runtime::RecompiledFunction switchFn = runtime.lookupFunction(kGreenEnterPc);
        if (switchFn != nullptr)
        {
            gGreenEnterPrev = switchFn;
            runtime.replaceFunction(kGreenEnterPc, &greenEnterProbe);
        }
    }

    // ---- Boot-transfer read path (TOL_TRACE_XFER=1) ------------------------------------------------
    // FUN_001f8530 is a guest async file-read state machine, not a device:
    //     open 0x196B88 -> size 0x197550 -> read 0x1970E8 -> tell 0x197638
    //     while (status != 3) { ... }
    // These probes only observe. Each one runs the real recompiled body and then reports the arguments
    // and the result, so "did the read move any bytes?" is answered by measurement instead of by another
    // host-side pump. Nothing here changes guest behaviour.
    PS2Runtime::RecompiledFunction gXferOpenPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferSizePrev = nullptr;
    PS2Runtime::RecompiledFunction gXferReadPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferTellPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferCompletePrev = nullptr;
    PS2Runtime::RecompiledFunction gXferStatePrev = nullptr;
    PS2Runtime::RecompiledFunction gXferDevPosPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferReapPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferSeekPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferPumpPrev = nullptr;
    PS2Runtime::RecompiledFunction gXferSubmitPrev = nullptr;
    PS2Runtime::RecompiledFunction gDvciPumpDriverPrev = nullptr;
    PS2Runtime::RecompiledFunction gDvciCbPrev = nullptr;
    uint32_t gDvciPumpCalls = 0u;
    uint32_t gDvciCbCalls = 0u;
    int gDvciPumpDepth = 0;
    PS2Runtime::RecompiledFunction gFrameTickPrev = nullptr;
    
    PS2Runtime::RecompiledFunction gIoPump2Prev = nullptr;
    uint32_t gIoPump2Calls = 0u;

    bool xferTraceEnabled()
    {
        static const bool enabled = std::getenv("TOL_TRACE_XFER") != nullptr;
        return enabled;
    }

    void xferLog(const char *what, uint32_t call, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                 int32_t result, uint32_t pc)
    {
        std::fprintf(stderr,
                     "[xfer] %s #%u a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X -> %d (0x%08X) pc=0x%08X\n",
                     what, call, a0, a1, a2, a3, result, static_cast<uint32_t>(result), pc);
    }

    // The CRI file handle is the structure the read path mutates. In FUN_00196f58 the fields that matter are
    //   +0x01 state (2 == hard EOF, every read returns 0), +0x04 device, +0x08 pending request,
    //   +0x0C cached size, +0x20 position (what FUN_00197638 "tell" returns),
    //   +0x24 dest, +0x28 last request size.
    void xferDumpHandle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t handle, const char *tag,
                        uint32_t call)
    {
        if (handle < 0x00100000u || handle >= 0x02000000u)
        {
            std::fprintf(stderr, "[xfer] %s #%u handle=0x%08X (not a RAM pointer)\n", tag, call, handle);
            return;
        }
        std::fprintf(stderr,
                     "[xfer] %s #%u handle=0x%08X state=%u dev=0x%08X req=0x%08X size=%d pos=%u dest=0x%08X "
                     "len=%u\n",
                     tag, call, handle, static_cast<unsigned>(runtime->Load8(rdram, ctx, handle + 0x01u)),
                     runtime->Load32(rdram, ctx, handle + 0x04u), runtime->Load32(rdram, ctx, handle + 0x08u),
                     static_cast<int32_t>(runtime->Load32(rdram, ctx, handle + 0x0Cu)),
                     runtime->Load32(rdram, ctx, handle + 0x20u), runtime->Load32(rdram, ctx, handle + 0x24u),
                     runtime->Load32(rdram, ctx, handle + 0x28u));
    }

    void xferOpenProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5);
        const uint32_t a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        if (gXferOpenPrev != nullptr)
            gXferOpenPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 8u)
        {
            xferLog("open", sCalls, a0, a1, a2, a3, static_cast<int32_t>(GPR_U32(ctx, 2)), ctx->pc);
            xferDumpHandle(rdram, ctx, runtime, GPR_U32(ctx, 2), "open", sCalls);
        }
    }

    void xferSizeProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5);
        const uint32_t a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        if (gXferSizePrev != nullptr)
            gXferSizePrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 8u)
        {
            xferLog("size", sCalls, a0, a1, a2, a3, static_cast<int32_t>(GPR_U32(ctx, 2)), ctx->pc);
            xferDumpHandle(rdram, ctx, runtime, a0, "size", sCalls);
        }
    }

    void xferReadProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        static int64_t sBytes = 0;
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5);
        const uint32_t a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        const uint32_t ra = GPR_U32(ctx, 31);
        // The transfer handler's two call sites (FUN_001f8530): 0x1F85A4 -> ra 0x1F85AC,
        // and 0x1F8718 -> ra 0x1F8720. These are logged unconditionally, because the ordinary
        // counter cap is exactly what hid the handler's read last time.
        const bool fromHandler = (ra == 0x001F85ACu || ra == 0x001F8720u);
        if (fromHandler)
            std::fprintf(stderr,
                         "[xfer] HANDLER-READ enter ra=0x%08X handle=0x%08X blocks=%u dest=0x%08X sp=0x%08X "
                         "pumpDepth=%d frames=%u pumps=%u\n",
                         ra, a0, a1, a2, GPR_U32(ctx, 29), gDvciPumpDepth, gFrameTicks, gDvciPumpCalls);
        if (gXferReadPrev != nullptr)
            gXferReadPrev(rdram, ctx, runtime);
        ++sCalls;
        const int32_t result = static_cast<int32_t>(GPR_U32(ctx, 2));
        if (result > 0)
            sBytes += result;
        if (fromHandler)
            std::fprintf(stderr,
                         "[xfer] HANDLER-READ exit ra=0x%08X pc=0x%08X %s result=%d\n", ra, ctx->pc,
                         ctx->pc == ra ? "RETURNED-OK" : "BAILED", result);
        if (sCalls <= 12u || (sCalls % 64u) == 0u)
        {
            xferLog("read", sCalls, a0, a1, a2, a3, result, ctx->pc);
            std::fprintf(stderr, "[xfer]      caller ra=0x%08X\n", ra);
            std::fprintf(stderr, "[xfer]      total=%lld bytes over %u reads\n",
                         static_cast<long long>(sBytes), sCalls);
            if (sCalls <= 6u)
                xferDumpHandle(rdram, ctx, runtime, a0, "read", sCalls);
        }
    }

    void xferTellProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5);
        const uint32_t a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        if (gXferTellPrev != nullptr)
            gXferTellPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls == 1u)
        {
            gTransferWaitActive = true;
            dumpGreenThreads(rdram, runtime, ctx, "wait-begin");
        }
        else if (sCalls == 128u || sCalls == 256u)
        {
            dumpGreenThreads(rdram, runtime, ctx, "wait-later");
        }
        if (sCalls <= 12u || (sCalls % 64u) == 0u)
            std::fprintf(stderr, "[xfer]      tell-count#%u pumps=%u cb4=%u ticks=%u frames=%u iopump=%u lock=0x%08X status=0x%08X\n", sCalls,
                         gDvciPumpCalls, gDvciCbCalls, gTickEntries, gFrameTicks, gIoPump2Calls, runtime->Load32(rdram, ctx, GPR_U32(ctx, 28) - 0x7880u), ctx->cop0_status);
        if (sCalls <= 12u || (sCalls % 64u) == 0u)
            std::fprintf(stderr, "[xfer]      host loops=%llu evts=%llu enterGuest=%llu\n",
                         static_cast<unsigned long long>(g_eeRunLoopIterations.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(
                             g_eeProcessPendingEventsCount.load(std::memory_order_relaxed)),
                         static_cast<unsigned long long>(g_eeEnterGuestCount.load(std::memory_order_relaxed)));
        if (sCalls <= 12u || (sCalls % 64u) == 0u)
            std::fprintf(stderr, "[xfer]      timers t0=%u/%u/%u t1=%u/%u/%u t2=%u/%u/%u\n",
                         runtime->Load32(rdram, ctx, 0x10000000u), runtime->Load32(rdram, ctx, 0x10000010u),
                         runtime->Load32(rdram, ctx, 0x10000020u), runtime->Load32(rdram, ctx, 0x10000800u),
                         runtime->Load32(rdram, ctx, 0x10000810u), runtime->Load32(rdram, ctx, 0x10000820u),
                         runtime->Load32(rdram, ctx, 0x10001000u), runtime->Load32(rdram, ctx, 0x10001010u),
                         runtime->Load32(rdram, ctx, 0x10001020u));
        if (sCalls <= 3u || (sCalls % 512u) == 0u)
            xferDumpHandle(rdram, ctx, runtime, a0, "tell", sCalls);
    }

    // FUN_00197128 is the only writer of handle[0x20] (what "tell" returns): it reaps a finished read and
    // sets pos = device_position - consumed. Counting it answers "does anything complete the request".
    void xferCompleteProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t handle = GPR_U32(ctx, 4);
        const bool ram = handle >= 0x00100000u && handle < 0x02000000u;
        const uint32_t stateIn = ram ? runtime->Load8(rdram, ctx, handle + 1u) : 0u;
        if (gXferCompletePrev != nullptr)
            gXferCompletePrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 16u || (sCalls % 256u) == 0u)
            std::fprintf(stderr, "[xfer] complete #%u handle=0x%08X state_in=%u -> %d pos=%u\n", sCalls, handle,
                         stateIn, static_cast<int32_t>(GPR_U32(ctx, 2)),
                         ram ? runtime->Load32(rdram, ctx, handle + 0x20u) : 0u);
    }

    // FUN_00197668 returns handle[1]; the transfer loop spins until it reads 3 (done). Report transitions
    // together with the handle and its device, so "stuck at 2" is distinguishable from "never polled".
    void xferStateProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        static uint32_t sLastHandle = 0u;
        static uint32_t sLastState = 0xFFFFFFFFu;
        const uint32_t handle = GPR_U32(ctx, 4);
        if (gXferStatePrev != nullptr)
            gXferStatePrev(rdram, ctx, runtime);
        ++sCalls;
        const uint32_t state = GPR_U32(ctx, 2) & 0xFFu;
        const bool ram = handle >= 0x00100000u && handle < 0x02000000u;
        if (handle != sLastHandle || state != sLastState)
        {
            sLastHandle = handle;
            sLastState = state;
            std::fprintf(stderr, "[xfer] state #%u handle=0x%08X -> %u\n", sCalls, handle, state);
            if (ram)
            {
                const uint32_t dev = runtime->Load32(rdram, ctx, handle + 4u);
                std::fprintf(stderr,
                             "[xfer]   pos=%u consumed=%u size=%d dest=0x%08X len=%u dev=0x%08X\n",
                             runtime->Load32(rdram, ctx, handle + 0x20u),
                             runtime->Load32(rdram, ctx, handle + 0x14u),
                             static_cast<int32_t>(runtime->Load32(rdram, ctx, handle + 0x0Cu)),
                             runtime->Load32(rdram, ctx, handle + 0x24u),
                             runtime->Load32(rdram, ctx, handle + 0x28u), dev);
                if (dev >= 0x00100000u && dev < 0x02000000u)
                    std::fprintf(stderr, "[xfer]   dev +08=0x%08X +58=0x%08X (position)\n",
                                 runtime->Load32(rdram, ctx, dev + 8u),
                                 runtime->Load32(rdram, ctx, dev + 0x58u));
            }
        }
    }

    // FUN_0019ed88 is the device byte position that FUN_00197128 subtracts the consumed count from.
    void xferDevPosProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t dev = GPR_U32(ctx, 4);
        if (gXferDevPosPrev != nullptr)
            gXferDevPosPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 16u || (sCalls % 256u) == 0u)
            std::fprintf(stderr, "[xfer] devpos #%u dev=0x%08X -> %d\n", sCalls, dev,
                         static_cast<int32_t>(GPR_U32(ctx, 2)));
    }

    // The two helpers that are allowed to reap a pending read (and therefore to run FUN_00197128).
    void xferReapProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t handle = GPR_U32(ctx, 4);
        if (gXferReapPrev != nullptr)
            gXferReapPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 12u || (sCalls % 128u) == 0u)
            std::fprintf(stderr, "[xfer] reap 0x196C98 #%u handle=0x%08X -> %d\n", sCalls, handle,
                         static_cast<int32_t>(GPR_U32(ctx, 2)));
    }

    void xferSeekProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t handle = GPR_U32(ctx, 4);
        if (gXferSeekPrev != nullptr)
            gXferSeekPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 12u || (sCalls % 128u) == 0u)
            std::fprintf(stderr, "[xfer] seek 0x197408 #%u handle=0x%08X off=%d whence=%u -> %d\n", sCalls, handle,
                         static_cast<int32_t>(GPR_U32(ctx, 5)), GPR_U32(ctx, 6),
                         static_cast<int32_t>(GPR_U32(ctx, 2)));
    }

    // The chaining decision in FUN_0019ef58. After each chunk:
    //     dev[0x58] += dev[0x20]                    blocks done
    //     dev[0x34] += dev[0x20] << 11              bytes done
    //     if (blocksDone < remaining) {
    //         if ((bytesDone >> 11) < dev[0x5C]) continue;   // window not reached
    //         if (0xFFFFE < dev[0x5C]) continue;
    //     }
    //     dev[1] = 3;                                DONE
    // so a small dev[0x5C] ends the transfer after one chunk. This prints the fields around every call.
    void xferPumpProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t dev = GPR_U32(ctx, 4);
        const bool ram = dev >= 0x00100000u && dev < 0x02000000u;
        uint32_t before[6] = {};
        const uint32_t offsets[6] = {0x14u, 0x20u, 0x30u, 0x34u, 0x58u, 0x5Cu};
        if (ram)
            for (int i = 0; i < 6; ++i)
                before[i] = runtime->Load32(rdram, ctx, dev + offsets[i]);
        if (gXferPumpPrev != nullptr)
            gXferPumpPrev(rdram, ctx, runtime);
        ++sCalls;
        if (!ram || sCalls > 400u)
            return;
        const uint32_t win = runtime->Load32(rdram, ctx, dev + 0x5Cu);
        // The handler's request uses a 266-block window; the DVCI background path uses 1. Keep the
        // multi-block windows and the first 40 calls, so the handler's chain is visible.
        if (win <= 1u && sCalls > 40u)
            return;
        const uint32_t state = runtime->Load8(rdram, ctx, dev + 1u);
        std::fprintf(stderr,
                     "[xfer] pump #%u dev=0x%08X state=%u | remain=%u chunk=%u total=%u bytes=%u done=%u win=%u "
                     "(was remain=%u chunk=%u bytes=%u done=%u)\n",
                     sCalls, dev, state, runtime->Load32(rdram, ctx, dev + 0x14u),
                     runtime->Load32(rdram, ctx, dev + 0x20u), runtime->Load32(rdram, ctx, dev + 0x30u),
                     runtime->Load32(rdram, ctx, dev + 0x34u), runtime->Load32(rdram, ctx, dev + 0x58u),
                     runtime->Load32(rdram, ctx, dev + 0x5Cu), before[0], before[1], before[3], before[4]);
    }

    // FUN_00196db0 is the submit: it re-arms the device (FUN_0019ee18 -> FUN_0019eda0 zeroes dev[0x34]),
    // then  handle[0x1C] = min(blocks, handle[0xC] - handle[0x14])  and  dev[0x5C] = handle[0x1C].
    // So the window is the request's own block count; this shows which operand is 1.
    void xferSubmitProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t handle = GPR_U32(ctx, 4);
        const uint32_t blocks = GPR_U32(ctx, 5);
        const uint32_t request = GPR_U32(ctx, 6);
        const bool ram = handle >= 0x00100000u && handle < 0x02000000u;
        if (gXferSubmitPrev != nullptr)
            gXferSubmitPrev(rdram, ctx, runtime);
        ++sCalls;
        const uint32_t sizeAfter = ram ? runtime->Load32(rdram, ctx, handle + 0x0Cu) : 0u;
        const uint32_t consumedAfter = ram ? runtime->Load32(rdram, ctx, handle + 0x14u) : 0u;
        const uint32_t windowAfter = ram ? runtime->Load32(rdram, ctx, handle + 0x1Cu) : 0u;
        const uint32_t dev = ram ? runtime->Load32(rdram, ctx, handle + 0x04u) : 0u;
        // Always report a multi-block request: the handler asks for 266 and must not be filtered out by
        // the counter cap again.
        if (!ram || (blocks <= 1u && sCalls > 40u))
            return;
        const uint32_t dev0 = (dev >= 0x00100000u && dev < 0x02000000u) ? runtime->Load32(rdram, ctx, dev) : 0u;
        const uint32_t dev1 = (dev >= 0x00100000u && dev < 0x02000000u) ? runtime->Load8(rdram, ctx, dev + 1u) : 0u;
        const uint32_t dev24 = (dev >= 0x00100000u && dev < 0x02000000u) ? runtime->Load32(rdram, ctx, dev + 0x24u) : 0u;
        std::fprintf(stderr,
                     "[xfer] submit #%u handle=0x%08X blocks=%u request=0x%08X | size=%u consumed=%u "
                     "size-cons=%d -> window=%u dev=0x%08X devWin=%u dev0=0x%08X dev1=%u dev24=0x%08X "
                     "| frames=%u iopump=%u pumps=%u cb4=%u\n",
                     sCalls, handle, blocks, request, sizeAfter, consumedAfter,
                     static_cast<int32_t>(sizeAfter - consumedAfter), windowAfter, dev,
                     (dev >= 0x00100000u && dev < 0x02000000u) ? runtime->Load32(rdram, ctx, dev + 0x5Cu) : 0u,
                     dev0, dev1, dev24, gFrameTicks, gIoPump2Calls, gDvciPumpCalls, gDvciCbCalls);
    }

    // FUN_00197940 is the DVCI pump (kDvciPumpPc); FUN_00198280 is the callback registered into slot 4
    // by FUN_001982a0 (FUN_001b7378(4, 0x198280, 0)). Logging the caller's $ra says which driver runs it,
    // and the timestamps against the handler's submit say when it stops.
    void dvciPumpDriverProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t ra = GPR_U32(ctx, 31);
        ++gDvciPumpDepth;
        if (gDvciPumpDriverPrev != nullptr)
            gDvciPumpDriverPrev(rdram, ctx, runtime);
        --gDvciPumpDepth;
        ++sCalls;
        ++gDvciPumpCalls;
        if (sCalls <= 60u || (sCalls % 64u) == 0u)
            std::fprintf(stderr, "[xfer] dvci-pump #%u ra=0x%08X\n", sCalls, ra);
    }

    void dvciCbProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t ra = GPR_U32(ctx, 31);
        if (gDvciCbPrev != nullptr)
            gDvciCbPrev(rdram, ctx, runtime);
        ++sCalls;
        ++gDvciCbCalls;
        if (sCalls <= 40u || (sCalls % 64u) == 0u)
            std::fprintf(stderr, "[xfer] dvci-cb4 #%u ra=0x%08X\n", sCalls, ra);
    }

    void ioPump2Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t ra = GPR_U32(ctx, 31);
        if (gIoPump2Prev != nullptr)
            gIoPump2Prev(rdram, ctx, runtime);
        ++sCalls;
        ++gIoPump2Calls;
        if (sCalls <= 24u || (sCalls % 64u) == 0u)
            std::fprintf(stderr, "[xfer] iopump #%u ra=0x%08X\n", sCalls, ra);
    }

    void frameTickProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gFrameTickPrev != nullptr)
            gFrameTickPrev(rdram, ctx, runtime);
        ++gFrameTicks;
    }








    // ---- drain probe (TOL_TRACE_DRAIN=1): pure observation of FUN_0019EEC8 ------------------------
    // FUN_0019EEC8 is the device drain:
    //     FUN_0019ee60(dev);  do { do { pump } while (dev[1] != 1); } while (dev[0x24] != 0);
    // It is the natural place to fix "armed but never started", because its whole job is to drive the
    // device to idle. This wrapper only observes first: it records the entry/exit state so the real
    // transitions are known before any behaviour is added.
    PS2Runtime::RecompiledFunction gDrainPrev = nullptr;
    uint32_t gDrainCalls = 0u;

    struct DrainFields
    {
        uint32_t dev0;
        uint8_t state;
        uint8_t busy;
        uint8_t flag3;
        uint32_t remaining;
        uint32_t chunk;
        uint32_t req;
        uint32_t done;
        uint32_t window;
    };

    DrainFields drainRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t dev)
    {
        DrainFields f{};
        if (dev < 0x00100000u || dev >= 0x02000000u)
            return f;
        f.dev0 = runtime->Load32(rdram, ctx, dev);
        f.state = runtime->Load8(rdram, ctx, dev + 1u);
        f.busy = runtime->Load8(rdram, ctx, dev + 2u);
        f.flag3 = runtime->Load8(rdram, ctx, dev + 3u);
        f.remaining = runtime->Load32(rdram, ctx, dev + 0x14u);
        f.chunk = runtime->Load32(rdram, ctx, dev + 0x20u);
        f.req = runtime->Load32(rdram, ctx, dev + 0x24u);
        f.done = runtime->Load32(rdram, ctx, dev + 0x58u);
        f.window = runtime->Load32(rdram, ctx, dev + 0x5Cu);
        return f;
    }

    void drainProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dev = GPR_U32(ctx, 4);
        const uint32_t returnPc = GPR_U32(ctx, 31);
        const DrainFields in = drainRead(rdram, ctx, runtime, dev);
        if (gDrainPrev != nullptr)
            gDrainPrev(rdram, ctx, runtime);
        ++gDrainCalls;
        const bool bailed = ctx->pc != returnPc;
        const DrainFields out = drainRead(rdram, ctx, runtime, dev);
        // Log the interesting cases in full: first calls, any entry while armed, and any exit transition.
        const bool interesting = gDrainCalls <= 20u || in.state == 2u || in.state != out.state;
        if (interesting && gDrainCalls <= 400u)
            std::fprintf(stderr,
                         "[drain] #%u dev=0x%08X in{dev0=0x%08X st=%u busy=%u f3=%u rem=%u chunk=%u req=0x%08X "
                         "done=%u win=%u} out{st=%u busy=%u rem=%u chunk=%u req=0x%08X done=%u}%s\n",
                         gDrainCalls, dev, in.dev0, in.state, in.busy, in.flag3, in.remaining, in.chunk, in.req,
                         in.done, in.window, out.state, out.busy, out.remaining, out.chunk, out.req, out.done,
                         bailed ? " BAILED" : "");
    }

    void installDrainProbe(PS2Runtime &runtime)
    {
        if (std::getenv("TOL_TRACE_DRAIN") == nullptr)
            return;
        PS2Runtime::RecompiledFunction drain = runtime.lookupFunction(0x0019EEC8u);
        if (drain == nullptr)
        {
            std::fprintf(stderr, "[drain] 0x0019EEC8 is not in the function table\n");
            return;
        }
        gDrainPrev = drain;
        runtime.replaceFunction(0x0019EEC8u, &drainProbe);
        std::fprintf(stderr, "[drain] probe installed on 0x0019EEC8\n");
    }

    // ---- frame-tick start (TOL_XFER_START=1) ------------------------------------------------------
    // The drain cannot fix this: FUN_0019ee60 sees dev[1]==2, dev[2]==0, writes dev[1]=1 and the loop
    // exits, discarding the armed read. The transfer must be *started* (FUN_0019ef58 sets dev[2]=1) before
    // any drain. The load stage runs FUN_001392D0 (frame tick) every iteration, so tick there.
    PS2Runtime::RecompiledFunction gFrameStartPrev = nullptr;
    uint32_t gFrameStarts = 0u;

    bool frameStartEnabled()
    {
        // Default ON: this is the fix that unblocks the boot script load. It gives the load stage
        // (FUN_00166010) the same per-frame FUN_0019a080 pump the frame loop already does, which is what
        // drains the armed CRI read. Measured effect: the boot script lands (script count 5/48 readable),
        // the resource table at 0x003DB6BC populates (ids 1,2,3...), and the script engine starts walking
        // commands. Reproducible. Disable with TOL_XFER_NO_TICK=1 to reproduce the old stall.
        static const bool enabled = std::getenv("TOL_XFER_NO_TICK") == nullptr;
        return enabled;
    }

    void frameStartFix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t returnPc = GPR_U32(ctx, 31);
        if (gFrameStartPrev != nullptr)
            gFrameStartPrev(rdram, ctx, runtime);
        if (ctx->pc != returnPc)
            return; // the frame tick bailed; propagate like generated code

        // Gate: the device exists and the pump is not inside itself. Deliberately NOT keyed on the
        // handle/device state: those flip 2 -> 0 -> 3 between ticks (measured), so any state test makes
        // the hook fire once. The frame loop pumps unconditionally every frame; this mirrors that.
        constexpr uint32_t kHandle = 0x00390738u;
        const uint32_t dev = runtime->Load32(rdram, ctx, kHandle + 0x04u);
        if (dev < 0x00100000u || dev >= 0x02000000u)
            return;
        if (runtime->Load8(rdram, ctx, dev + 2u) != 0u) // 1 = the pump is inside itself right now
            return;

        // The frame loop does  ... FUN_00151ff0(0/1/4); ... FUN_0019a080(); ...  once per frame.
        // The load stage (FUN_00166010) runs FUN_001392d0 every iteration but never the IO pump, so an
        // armed read is never drained while a load is in progress.
        //
        // Queued invocation, NOT an inline call: the pump chain (FUN_0019a080 -> FUN_001b7870 ->
        // FUN_001b7688) is emitted as direct C++ tail-calls, which never touch the guest $ra register.
        // An inline call therefore has no legitimate return address and the innermost handler's
        // `jr $ra` lands on whatever was in $ra (measured: source=0xdeadbeef). sp=0 / ra=0 is the
        // contract dispatchIrq uses: the scheduler hands out an invocation stack and treats the
        // function's return as completion.
        constexpr uint32_t kPumpEntry = 0x0019A080u;
        if (runtime->lookupFunction(kPumpEntry) == nullptr)
            return;
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context = *ctx;
        invocation.context.pc = kPumpEntry;
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        runtime->eeScheduler().queueInvocation(std::move(invocation));
        ++gFrameStarts;
        if (gFrameStarts <= 8u || (gFrameStarts % 64u) == 0u)
        {
            const bool devOk = dev >= 0x00100000u && dev < 0x02000000u;
            std::fprintf(stderr,
                         "[xfer] tick-pump #%u queued dev=0x%08X st=%u busy=%u done=%u chunk=%u win=%u\n",
                         gFrameStarts, dev, devOk ? runtime->Load8(rdram, ctx, dev + 1u) : 0u,
                         devOk ? runtime->Load8(rdram, ctx, dev + 2u) : 0u,
                         devOk ? runtime->Load32(rdram, ctx, dev + 0x58u) : 0u,
                         devOk ? runtime->Load32(rdram, ctx, dev + 0x20u) : 0u,
                         devOk ? runtime->Load32(rdram, ctx, dev + 0x5Cu) : 0u);
        }
    }

    void installFrameStartFix(PS2Runtime &runtime)
    {
        if (!frameStartEnabled())
            return;
        PS2Runtime::RecompiledFunction tick = runtime.lookupFunction(0x001392D0u);
        if (tick == nullptr)
            return;
        gFrameStartPrev = tick;
        runtime.replaceFunction(0x001392D0u, &frameStartFix);
        std::fprintf(stderr, "[xfer] frame-start installed on 0x001392D0\n");
    }

    // ---- stream-release unblock (TOL_STREAM_RELEASE=1) --------------------------------------------
    // FUN_00198C60 is a waiter thread:
    //     while (DAT_00394378 == 0) FUN_00198C48();   // FUN_00198C48: DAT_00394320 += 1; return
    //     DAT_00394380 = 1; ExitDeleteThread();
    // FUN_00198C48 does not yield, and per the EE kernel model (09-ps2tek "Thread Scheduler":
    // cooperative, absolute priority, syscall-only) a priority-8 spin starves the priority 16/18/99
    // threads - measured: tid 3/4/6 Ready forever.
    // DAT_00394378 is set only by FUN_00199938, which runs from the audio layer's release path when the
    // refcount DAT_003942c4 reaches 0. The IxAudio/KORG RPCs are short-circuited (they hang), so that
    // open->close cycle never completes. Invoke the guest's own release routine after the spin has
    // clearly made no progress, so the waiter can exit and the starved threads run.
    PS2Runtime::RecompiledFunction gSpinReleasePrev = nullptr;
    uint32_t gSpinReleaseCalls = 0u;

    void spinReleaseProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gSpinReleasePrev != nullptr)
            gSpinReleasePrev(rdram, ctx, runtime);
        if (++gSpinReleaseCalls != 4096u)
            return;
        // Complete the ADX lifecycle through the guest's own detach: refcount 1 -> 0, which makes
        // FUN_00199988 run its release branch (FUN_00199938 sets the stream flags). Calling
        // FUN_00199938 directly would set the flags but leave the refcount at 1, which is not the
        // lifecycle the game closes and reopens with.
        PS2Runtime::RecompiledFunction detach = runtime->lookupFunction(0x00199988u);
        if (detach == nullptr)
            return;
        const int32_t before = static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u));
        SET_GPR_U32(ctx, 31, 0xDEADBEF0u);
        detach(rdram, ctx, runtime);
        SET_GPR_U32(ctx, 31, 0x198C90u);
        const int32_t after = static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u));
        std::fprintf(stderr,
                     "[xfer] adx-detach after %u spins: refcount %d -> %d, stream flag 0x394378 = %u\n",
                     gSpinReleaseCalls, before, after, runtime->Load32(rdram, ctx, 0x00394378u));
    }

    void installSpinRelease(PS2Runtime &runtime)
    {
        // Default ON: this completes the ADX lifecycle (refcount 1 -> 0 via the guest's own detach), which
        // is what makes FUN_00199938 set the stream flags. Without it the priority-8 waiter at 0x198C60
        // spins forever and starves the priority 16/18/99 threads, because the EE kernel schedules by
        // absolute priority (09-ps2tek "Thread Scheduler"). TOL_NO_STREAM_RELEASE=1 disables it.
        if (std::getenv("TOL_NO_STREAM_RELEASE") != nullptr)
            return;
        PS2Runtime::RecompiledFunction spin = runtime.lookupFunction(0x00198C48u);
        if (spin == nullptr)
            return;
        gSpinReleasePrev = spin;
        runtime.replaceFunction(0x00198C48u, &spinReleaseProbe);
        std::fprintf(stderr, "[xfer] stream-release hook installed on 0x00198C48\n");
    }

    // ---- ADX attach/detach lifecycle (TOL_TRACE_ADX=1) ---------------------------------------------
    // FUN_001997a8 = attach: at refcount 0 it initialises ADXPS2 and registers callback slot 6
    // (0x198bf8), then DAT_003942c4++. FUN_00199988 / FUN_0019a550 = detach: DAT_003942c4-- and, at
    // exactly 0, run FUN_00199938 which sets the stream flags (0x394378..) the waiter 0x198C60 spins on.
    // If attach never runs, a detach takes the count to -1 and the release branch can never fire.
    PS2Runtime::RecompiledFunction gAdxAttachPrev = nullptr;
    PS2Runtime::RecompiledFunction gAdxDetach1Prev = nullptr;
    PS2Runtime::RecompiledFunction gAdxDetach2Prev = nullptr;
    uint32_t gAdxAttachCalls = 0u;
    uint32_t gAdxDetachCalls = 0u;

    void adxAttachProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t before = static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u));
        if (gAdxAttachPrev != nullptr)
            gAdxAttachPrev(rdram, ctx, runtime);
        ++gAdxAttachCalls;
        if (gAdxAttachCalls <= 8u)
            std::fprintf(stderr, "[adx] attach #%u refcount %d -> %d\n", gAdxAttachCalls, before,
                         static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u)));
    }

    void adxDetachProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        const int32_t before = static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u));
        if (gAdxDetach1Prev != nullptr)
            gAdxDetach1Prev(rdram, ctx, runtime);
        ++gAdxDetachCalls;
        if (gAdxDetachCalls <= 8u)
            std::fprintf(stderr, "[adx] detach #%u (0x00199988) refcount %d -> %d ra=0x%08X\n", gAdxDetachCalls,
                         before, static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u)), ra);
    }

    void adxDetach2Probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        const int32_t before = static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u));
        if (gAdxDetach2Prev != nullptr)
            gAdxDetach2Prev(rdram, ctx, runtime);
        ++gAdxDetachCalls;
        if (gAdxDetachCalls <= 8u)
            std::fprintf(stderr, "[adx] detach #%u (0x0019a550) refcount %d -> %d ra=0x%08X\n", gAdxDetachCalls,
                         before, static_cast<int32_t>(runtime->Load32(rdram, ctx, 0x003942C4u)), ra);
    }

    void installAdxLifecycleProbe(PS2Runtime &runtime)
    {
        if (std::getenv("TOL_TRACE_ADX") == nullptr)
            return;
        const struct
        {
            uint32_t pc;
            PS2Runtime::RecompiledFunction probe;
            PS2Runtime::RecompiledFunction *prev;
            const char *name;
        } table[] = {
            {0x001997A8u, &adxAttachProbe, &gAdxAttachPrev, "attach"},
            {0x00199988u, &adxDetachProbe, &gAdxDetach1Prev, "detach1"},
            {0x0019A550u, &adxDetach2Probe, &gAdxDetach2Prev, "detach2"},
        };
        for (const auto &entry : table)
        {
            PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(entry.pc);
            if (fn == nullptr)
            {
                std::fprintf(stderr, "[adx] 0x%08X (%s) not in function table\n", entry.pc, entry.name);
                continue;
            }
            *entry.prev = fn;
            runtime.replaceFunction(entry.pc, entry.probe);
        }
        std::fprintf(stderr, "[adx] lifecycle probes installed\n");
    }

    // ---- thread creation audit (TOL_TRACE_CREATE=1) ------------------------------------------------
    // CreateThread(ThreadParam*): +0 status, +4 func, +8 stack, +0xC stack_size, +0x10 gp,
    // +0x14 initial_priority, +0x18 current_priority, +0x1C attr, +0x20 option.
    // Logging every creation names the origin of the priority-8 waiter at 0x198C60 and the priority
    // each other thread is created with, which decides whether the starvation is the port's or the game's.
    PS2Runtime::RecompiledFunction gCreateThreadPrev = nullptr;
    uint32_t gCreateThreadCalls = 0u;

    void createThreadProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t param = GPR_U32(ctx, 4);
        const uint32_t ra = GPR_U32(ctx, 31);
        if (param >= 0x00100000u && param < 0x02000000u)
        {
            const uint32_t func = runtime->Load32(rdram, ctx, param + 4u);
            const uint32_t stack = runtime->Load32(rdram, ctx, param + 8u);
            const uint32_t prio = runtime->Load32(rdram, ctx, param + 0x14u);
            ++gCreateThreadCalls;
            if (gCreateThreadCalls <= 40u)
                std::fprintf(stderr, "[create] #%u func=0x%08X prio=%u stack=0x%08X ra=0x%08X%s\n",
                             gCreateThreadCalls, func, prio, stack, ra, func == 0x00198C60u ? "  <-- WAITER" : "");
        }
        if (gCreateThreadPrev != nullptr)
            gCreateThreadPrev(rdram, ctx, runtime);
    }

    PS2Runtime::RecompiledFunction gChangePrioPrev = nullptr;

    void changePriorityProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t sCalls = 0u;
        const uint32_t tid = GPR_U32(ctx, 4);
        const uint32_t prio = GPR_U32(ctx, 5);
        const uint32_t ra = GPR_U32(ctx, 31);
        if (gChangePrioPrev != nullptr)
            gChangePrioPrev(rdram, ctx, runtime);
        ++sCalls;
        if (sCalls <= 40u)
            std::fprintf(stderr, "[create] setprio tid=%u -> %u ra=0x%08X%s\n", tid, prio, ra,
                         prio == 8u ? "  <-- TO 8" : "");
    }

    void installCreateThreadProbe(PS2Runtime &runtime)
    {
        if (std::getenv("TOL_TRACE_CREATE") == nullptr)
            return;
        PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(0x001144E0u);
        if (fn == nullptr)
        {
            std::fprintf(stderr, "[create] 0x001144E0 not in function table\n");
            return;
        }
        gCreateThreadPrev = fn;
        runtime.replaceFunction(0x001144E0u, &createThreadProbe);
        PS2Runtime::RecompiledFunction prio = runtime.lookupFunction(0x00114570u);
        if (prio != nullptr)
        {
            gChangePrioPrev = prio;
            runtime.replaceFunction(0x00114570u, &changePriorityProbe);
        }
        std::fprintf(stderr, "[create] probe installed on 0x001144E0 (+0x00114570)\n");
    }

    // ---- boot loop trace (TOL_TRACE_BOOT=1) ------------------------------------------------------
    // FUN_00165ad0 is the game's outer loop:
    //   do { ...load stage...; FUN_00166870(); FUN_00139d50(); ...; do { frame } while (FUN_00153180()==0); } while(true);
    // The thread table ends with tid 1 Dormant at pc=0x41, i.e. the ELF entry returned instead of staying
    // in this loop. These probes show whether the loop and FUN_00139d50 are entered, and how often.
    PS2Runtime::RecompiledFunction gBootLoopPrev = nullptr;
    PS2Runtime::RecompiledFunction gBootStagePrev = nullptr;
    uint32_t gBootLoopEntries = 0u;
    uint32_t gBootStageEntries = 0u;

    void bootLoopProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++gBootLoopEntries;
        if (gBootLoopEntries <= 8u)
            std::fprintf(stderr, "[boot] FUN_00165AD0 (outer loop) entry #%u tid=%d\n", gBootLoopEntries,
                         runtime->eeScheduler().currentThreadId());
        if (gBootLoopPrev != nullptr)
            gBootLoopPrev(rdram, ctx, runtime);
    }

    void bootStageProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++gBootStageEntries;
        if (gBootStageEntries <= 8u)
            std::fprintf(stderr, "[boot] FUN_00139D50 (post-pipeline) entry #%u tid=%d\n", gBootStageEntries,
                         runtime->eeScheduler().currentThreadId());
        if (gBootStagePrev != nullptr)
            gBootStagePrev(rdram, ctx, runtime);
    }

    // One outer iteration, end to end: load stage -> asset pipeline -> its body -> frame loop.
    PS2Runtime::RecompiledFunction gLoadStagePrev = nullptr;
    PS2Runtime::RecompiledFunction gPipePrev = nullptr;
    PS2Runtime::RecompiledFunction gPipeBodyPrev = nullptr;
    PS2Runtime::RecompiledFunction gFramePrev = nullptr;
    uint32_t gBootSeq = 0u;

    void bootEntryExit(const char *name, PS2Runtime::RecompiledFunction &prev, uint8_t *rdram, R5900Context *ctx,
                       PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        const bool logging = gBootSeq < 120u;
        if (logging)
            std::fprintf(stderr, "[boot] %3u %s enter ra=0x%08X\n", ++gBootSeq, name, ra);
        else
            ++gBootSeq;
        if (prev != nullptr)
            prev(rdram, ctx, runtime);
        if (logging)
            std::fprintf(stderr, "[boot] %3u %s exit  pc=0x%08X %s\n", ++gBootSeq, name, ctx->pc,
                         ctx->pc == ra ? "(normal)" : "(bailed)");
        else
            ++gBootSeq;
    }

    void loadStageProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        bootEntryExit("load-stage", gLoadStagePrev, rdram, ctx, runtime);
    }
    void pipelineProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        bootEntryExit("pipeline", gPipePrev, rdram, ctx, runtime);
    }
    void pipelineBodyProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        bootEntryExit("pipe-body", gPipeBodyPrev, rdram, ctx, runtime);
    }
    void frameLoopProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        bootEntryExit("frame", gFramePrev, rdram, ctx, runtime);
    }

    void installBootTrace(PS2Runtime &runtime)
    {
        if (std::getenv("TOL_TRACE_BOOT") == nullptr)
            return;
        const struct
        {
            uint32_t pc;
            PS2Runtime::RecompiledFunction probe;
            PS2Runtime::RecompiledFunction *prev;
            const char *name;
        } table[] = {
            {0x00165AD0u, &bootLoopProbe, &gBootLoopPrev, "outer-loop"},
            {0x00166010u, &loadStageProbe, &gLoadStagePrev, "load-stage"},
            {0x00166870u, &pipelineProbe, &gPipePrev, "pipeline"},
            {0x00164EA0u, &pipelineBodyProbe, &gPipeBodyPrev, "pipe-body"},
            {0x00139D50u, &bootStageProbe, &gBootStagePrev, "stage"},
            {0x00152B70u, &frameLoopProbe, &gFramePrev, "frame"},
        };
        for (const auto &entry : table)
        {
            PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(entry.pc);
            if (fn == nullptr)
            {
                std::fprintf(stderr, "[boot] 0x%08X (%s) not in function table\n", entry.pc, entry.name);
                continue;
            }
            *entry.prev = fn;
            runtime.replaceFunction(entry.pc, entry.probe);
        }
        std::fprintf(stderr, "[boot] probes installed\n");
    }

    // ---- GS history dump (TOL_TRACE_GSHIST=1) ------------------------------------------------------
    // GS keeps a ring of GSDebugHistoryEntry (512 entries) but starts PAUSED (m_debugHistoryPaused =
    // true), so nothing is recorded until it is unpaused. This unpauses it at the first frame tail and
    // reports, every 30 frames, what the guest actually submitted to the GS: how many GIF tags, register
    // writes, Draw entries, texture transfers and Present events, plus the vertex volume and bounds of
    // the draws and which framebuffer was presented. That separates "the guest submits nothing" from
    // "the guest submits degenerate geometry" - the first question the console-graphics method asks.
    PS2Runtime::RecompiledFunction gGsHistPrev = nullptr;
    uint64_t gGsHistLastSeq = 0u;
    uint32_t gGsHistFrames = 0u;

    void gsHistoryProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gGsHistPrev != nullptr)
            gGsHistPrev(rdram, ctx, runtime);
        static bool sUnpaused = false;
        if (!sUnpaused)
        {
            runtime->gs().setDebugHistoryPaused(false);
            runtime->gs().clearDebugHistory();
            sUnpaused = true;
            std::fprintf(stderr, "[gs] history unpaused\n");
        }
        if ((++gGsHistFrames % 30u) != 0u)
        {
            // TOL_DUMP_VRAM=<path>: write the live 4 MB GS VRAM at two frame tails, so the framebuffer
            // the display reads and the framebuffer the draws target can be compared byte for byte.
            if (const char *dumpPath = std::getenv("TOL_DUMP_VRAM"))
            {
                if (gGsHistFrames == 150u || gGsHistFrames == 400u)
                {
                    char finalPath[512];
                    std::snprintf(finalPath, sizeof(finalPath), "%s_%u.bin", dumpPath, gGsHistFrames);
                    if (std::FILE *fp = std::fopen(finalPath, "wb"))
                    {
                        std::fwrite(runtime->memory().getGSVRAM(), 1u, 4u * 1024u * 1024u, fp);
                        std::fclose(fp);
                        std::fprintf(stderr, "[gs] vram dumped at frame %u -> %s\n", gGsHistFrames, finalPath);
                    }
                    char scriptPath[512];
                    std::snprintf(scriptPath, sizeof(scriptPath), "%s_script_%u.bin", dumpPath, gGsHistFrames);
                    if (std::FILE *sf = std::fopen(scriptPath, "wb"))
                    {
                        std::fwrite(runtime->memory().getRDRAM() + 0x01520000u, 1u, 0x10000u, sf);
                        std::fclose(sf);
                        std::fprintf(stderr, "[gs] script window dumped -> %s\n", scriptPath);
                    }
                }
            }
            return;
        }

        const std::vector<GSDebugHistoryEntry> history = runtime->gs().getDebugHistory();
        uint32_t tags = 0u, regs = 0u, draws = 0u, transfers = 0u, presents = 0u;
        uint64_t verts = 0u;
        uint32_t lastDrawPrim = 0u;
        float yMin = 0.0f, yMax = 0.0f;
        uint32_t lastDrawFbp = 0xFFFFFFFFu;
        uint32_t minFbp = 0xFFFFFFFFu, maxFbp = 0u;
        uint32_t displayFbp = 0u, sourceFbp = 0u;
        bool sawPresent = false;
        bool usedPreferred = false;
        // Aggregate the whole window so "the geometry is degenerate" and "the geometry is real but
        // lands off-screen" can be told apart. Coordinates arrive in GS fixed point (x16).
        float allXMin = 1e9f, allXMax = -1e9f, allYMin = 1e9f, allYMax = -1e9f;
        uint32_t degenerate = 0u;
        uint32_t primHist[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        uint32_t fbpHist[512] = {0u};
        for (const GSDebugHistoryEntry &entry : history)
        {
            if (entry.seq <= gGsHistLastSeq)
                continue;
            switch (entry.kind)
            {
            case GSDebugEventKind::GifTag: ++tags; break;
            case GSDebugEventKind::Register: ++regs; break;
            case GSDebugEventKind::Draw:
                ++draws;
                verts += entry.vertexCount;
                lastDrawPrim = static_cast<uint32_t>(entry.prim.type) & 7u;
                ++primHist[lastDrawPrim];
                yMin = entry.yMin;
                yMax = entry.yMax;
                if (entry.xMin < allXMin) allXMin = entry.xMin;
                if (entry.xMax > allXMax) allXMax = entry.xMax;
                if (entry.yMin < allYMin) allYMin = entry.yMin;
                if (entry.yMax > allYMax) allYMax = entry.yMax;
                if ((entry.xMax - entry.xMin) < 1.0f && (entry.yMax - entry.yMin) < 1.0f)
                    ++degenerate;
                lastDrawFbp = entry.frame.fbp;
                if (entry.frame.fbp < minFbp)
                    minFbp = entry.frame.fbp;
                if (entry.frame.fbp > maxFbp)
                    maxFbp = entry.frame.fbp;
                if (entry.frame.fbp < 512u)
                    ++fbpHist[entry.frame.fbp];
                break;
            case GSDebugEventKind::Transfer: ++transfers; break;
            case GSDebugEventKind::Present:
                ++presents;
                displayFbp = entry.displayFbp;
                sourceFbp = entry.sourceFbp;
                usedPreferred = entry.usedPreferred;
                sawPresent = true;
                break;
            }
        }
        if (!history.empty())
            gGsHistLastSeq = history.back().seq;
        uint64_t maxSeq = 0u;
        uint64_t maxVsync = 0u;
        for (const GSDebugHistoryEntry &entry : history)
        {
            if (entry.seq > maxSeq) maxSeq = entry.seq;
            if (entry.vsyncTick > maxVsync) maxVsync = entry.vsyncTick;
        }
        const uint64_t guestTick = runtime->memory().gs().vsyncTick.load(std::memory_order_acquire);
        std::fprintf(stderr, "[gs]    seqMax=%llu entryTickMax=%llu guestTick=%llu entries=%u\n",
                     static_cast<unsigned long long>(maxSeq), static_cast<unsigned long long>(maxVsync),
                     static_cast<unsigned long long>(guestTick), static_cast<unsigned>(history.size()));
        std::fprintf(stderr,
                     "[gs] f=%u tags=%u regs=%u draws=%u verts=%llu transfers=%u presents=%u"
                     " lastPrim=%u y=[%.1f..%.1f] drawFbp=%u range=[%u..%u] fbp=%u<-%u preferred=%d\n",
                     gGsHistFrames, tags, regs, draws, static_cast<unsigned long long>(verts), transfers, presents,
                     lastDrawPrim, yMin, yMax, lastDrawFbp, minFbp == 0xFFFFFFFFu ? 0u : minFbp, maxFbp, displayFbp, sourceFbp, (sawPresent && usedPreferred) ? 1 : 0);
        std::fprintf(stderr,
                     "[gs]    bounds x=[%.1f..%.1f] y=[%.1f..%.1f] px(x/16) x=[%.1f..%.1f] y=[%.1f..%.1f]"
                     " degenerate=%u prim[0..7]=%u/%u/%u/%u/%u/%u/%u/%u\n",
                     allXMin, allXMax, allYMin, allYMax, allXMin / 16.0f, allXMax / 16.0f, allYMin / 16.0f,
                     allYMax / 16.0f, degenerate, primHist[0], primHist[1], primHist[2], primHist[3], primHist[4],
                     primHist[5], primHist[6], primHist[7]);
        if (minFbp != 0xFFFFFFFFu)
            std::fprintf(stderr, "[gs]    fbp0=%u fbp224=%u\n", fbpHist[0], fbpHist[224]);

        // Per-frame summary: draws, target framebuffer and vertex volume for the newest frames in the
        // ring. Double-buffered games alternate FRAME between two FBPs; seeing which FBP the newest
        // frames draw into, next to the FBP the display registers select, says whether the presented
        // buffer is a frame the guest never finished or the wrong buffer entirely.
        struct FrameDigest
        {
            uint32_t draws = 0u;
            uint64_t verts = 0u;
            uint32_t fbp = 0xFFFFFFFFu;
            bool mixed = false;
        };
        std::map<uint32_t, FrameDigest> digests;
        for (const GSDebugHistoryEntry &entry : history)
        {
            if (entry.kind != GSDebugEventKind::Draw)
                continue;
            FrameDigest &digest = digests[entry.frameIndex];
            ++digest.draws;
            digest.verts += entry.vertexCount;
            if (digest.fbp == 0xFFFFFFFFu)
                digest.fbp = entry.frame.fbp;
            else if (digest.fbp != entry.frame.fbp)
                digest.mixed = true;
        }
        uint32_t shown = 0u;
        for (auto it = digests.rbegin(); it != digests.rend() && shown < 6u; ++it, ++shown)
        {
            std::fprintf(stderr, "[gs]    frame %u draws=%u verts=%llu fbp=%u%s\n", it->first, it->second.draws,
                         static_cast<unsigned long long>(it->second.verts), it->second.fbp,
                         it->second.mixed ? " (mixed)" : "");
        }
    }

    void installGsHistoryProbe(PS2Runtime &runtime)
    {
        if (std::getenv("TOL_TRACE_GSHIST") == nullptr && std::getenv("TOL_DUMP_VRAM") == nullptr)
            return;
        PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(0x00139A20u);
        if (fn == nullptr)
        {
            std::fprintf(stderr, "[gs] 0x00139A20 (frame tail) not in function table\n");
            return;
        }
        gGsHistPrev = fn;
        runtime.replaceFunction(0x00139A20u, &gsHistoryProbe);
        std::fprintf(stderr, "[gs] history probe installed on 0x00139A20\n");
    }

    void installXferProbes(PS2Runtime &runtime)
    {
        if (!xferTraceEnabled())
            return;
        const struct
        {
            uint32_t pc;
            PS2Runtime::RecompiledFunction probe;
            PS2Runtime::RecompiledFunction *prev;
            const char *name;
        } table[] = {
            {0x00196B88u, &xferOpenProbe, &gXferOpenPrev, "open"},
            {0x00197550u, &xferSizeProbe, &gXferSizePrev, "size"},
            {0x001970E8u, &xferReadProbe, &gXferReadPrev, "read"},
            {0x00197638u, &xferTellProbe, &gXferTellPrev, "tell"},
            {0x00197128u, &xferCompleteProbe, &gXferCompletePrev, "complete"},
            {0x00197668u, &xferStateProbe, &gXferStatePrev, "state"},
            {0x0019ED88u, &xferDevPosProbe, &gXferDevPosPrev, "devpos"},
            {0x00196C98u, &xferReapProbe, &gXferReapPrev, "reap"},
            {0x00197408u, &xferSeekProbe, &gXferSeekPrev, "seek"},
            {0x0019EF58u, &xferPumpProbe, &gXferPumpPrev, "pump"},
            {0x00196DB0u, &xferSubmitProbe, &gXferSubmitPrev, "submit"},
            {0x00197940u, &dvciPumpDriverProbe, &gDvciPumpDriverPrev, "dvci-pump"},
            {0x00198280u, &dvciCbProbe, &gDvciCbPrev, "dvci-cb4"},
            {0x001392D0u, &frameTickProbe, &gFrameTickPrev, "frame-tick"},
            {0x0019A080u, &ioPump2Probe, &gIoPump2Prev, "iopump"},
        };
        for (const auto &entry : table)
        {
            PS2Runtime::RecompiledFunction fn = runtime.lookupFunction(entry.pc);
            if (fn == nullptr)
            {
                std::fprintf(stderr, "[xfer] 0x%08X (%s) is not in the function table\n", entry.pc, entry.name);
                continue;
            }
            *entry.prev = fn;
            runtime.replaceFunction(entry.pc, entry.probe);
            std::fprintf(stderr, "[xfer] probe installed on 0x%08X (%s)\n", entry.pc, entry.name);
        }
    }

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
        installXferProbes(runtime);
        // installXferStartFix is deliberately NOT called: wrapping 0x00196DB0 perturbs the submit path
        // (41 submits -> 2) even when the wrapper only observes. See HANDOFF §0o.
        installDrainProbe(runtime);
        installFrameStartFix(runtime);
        installSpinRelease(runtime);
        installScriptThreadProbes(runtime);
        installAdxLifecycleProbe(runtime);
        installCreateThreadProbe(runtime);
        installBootTrace(runtime);
        installGsHistoryProbe(runtime);

        // Function-table gap: 0x139690 is a shared clamp-setter epilogue
        // (`jr ra; sw a0, -0x7b44(gp)`) reached by a `j 0x139690` tail jump from
        // FUN_00139ca0. Ghidra folded it into its callers, so the dense table has
        // a null slot here (indices 0x139684..0x13969c are all gaps). The sound
        // init's clamp helper tail-jumps here, the jump target is reported missing,
        // and the runtime then spins on the missing-PC stub forever, which is why
        // the first frame never finishes and the screen stays on NOW LOADING.
        // Re-express the two instructions natively rather than regenerating the
        // whole corpus for one 8-byte setter.
        runtime.replaceFunction(0x00139690u,
                                [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                                    const uint32_t gp = GPR_U32(ctx, 28);
                                    WRITE32(gp - 0x7b44u, GPR_U32(ctx, 4));
                                    ctx->pc = GPR_U32(ctx, 31);
                                });

        // 0x1F8150 is the GS/VU1 worker loop:
        //     do { FUN_001f8000(); FUN_0015231c(); } while (true);
        // On hardware it runs as its own guest thread and the loop yields each iteration, so the game's own
        // microkernel owns it. Legacy host pump only (see legacyWorkerPumpsEnabled).
        if (legacyWorkerPumpsEnabled())
            runtime.replaceFunction(0x001F8150u,
                                [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) {
                                    // Guard against the flush path re-entering the worker
                                    // (FUN_0015231c -> FUN_00151ff0 -> FUN_00151e90 -> handler).
                                    static bool sInWorkerStep = false;
                                    static uint32_t sGuardHits = 0u;
                                    static uint32_t sEntries = 0u;
                                    if (sInWorkerStep)
                                    {
                                        ++sGuardHits;
                                        if (std::getenv("TOL_TRACE_GS") != nullptr && sGuardHits <= 20u)
                                        {
                                            std::fprintf(stderr,
                                                         "[gs] worker GUARD hit #%u ra=0x%08X entries=%u\n",
                                                         sGuardHits, GPR_U32(ctx, 31), sEntries);
                                        }
                                        ctx->pc = GPR_U32(ctx, 31);
                                        return;
                                    }
                                    sInWorkerStep = true;
                                    ++sEntries;
                                    const uint32_t savedRa = GPR_U32(ctx, 31);
                                    if (std::getenv("TOL_TRACE_GS") != nullptr)
                                    {
                                        static uint32_t sWorkerSteps = 0;
                                        ++sWorkerSteps;
                                        if (sWorkerSteps <= 8u || (sWorkerSteps % 5000u) == 0u)
                                        {
                                            std::fprintf(stderr, "[gs] worker step #%u ra=0x%08X\n",
                                                         sWorkerSteps, savedRa);
                                        }
                                    }

                                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(0x001F8000u);
                                    SET_GPR_U32(ctx, 31, 0x001F8160u);
                                    ctx->pc = 0x001F8000u;
                                    step(rdram, ctx, runtime);
                                    const uint32_t pcAfterStep = ctx->pc;

                                    PS2Runtime::RecompiledFunction flush = runtime->lookupFunction(0x0015231Cu);
                                    SET_GPR_U32(ctx, 31, 0x001F8168u);
                                    ctx->pc = 0x0015231Cu;
                                    flush(rdram, ctx, runtime);
                                    const uint32_t pcAfterSave = ctx->pc;

                                    sInWorkerStep = false;
                                    SET_GPR_U32(ctx, 31, savedRa);
                                    ctx->pc = savedRa;
                                    if (std::getenv("TOL_TRACE_GS") != nullptr && sEntries <= 20u)
                                    {
                                        std::fprintf(stderr,
                                                     "[gs] worker entry #%u savedRa=0x%08X afterStep=0x%08X "
                                                     "afterSave=0x%08X exit=0x%08X guardHits=%u\n",
                                                     sEntries, savedRa, pcAfterStep, pcAfterSave, savedRa,
                                                     sGuardHits);
                                    }
                                    if (std::getenv("TOL_TRACE_GS") != nullptr)
                                    {
                                        static uint32_t sFlagLogs = 0;
                                        if (sFlagLogs < 6u && rdram != nullptr)
                                        {
                                            uint32_t flagPtr = 0;
                                            std::memcpy(&flagPtr, rdram + 0x00407740u, sizeof(flagPtr));
                                            uint32_t flagVal = 0;
                                            if (flagPtr + sizeof(flagPtr) <= PS2_RAM_SIZE)
                                            {
                                                std::memcpy(&flagVal, rdram + flagPtr, sizeof(flagVal));
                                            }
                                            ++sFlagLogs;
                                            std::fprintf(stderr,
                                                         "[gs] flag ctx=0x00407740 ptr=0x%08X val=0x%08X sp=0x%08X\n",
                                                         flagPtr, flagVal, GPR_U32(ctx, 29));
                                        }
                                    }
                                });
        // The generated function resumes at interior labels (the loop back-edge
        // lands on 0x1F8168), and lookupFunction goes through the dense table
        // slot for that exact address, so replace every slot in the body too.
        if (legacyWorkerPumpsEnabled())
        {
            const PS2Runtime::RecompiledFunction workerStep = runtime.lookupFunction(0x001F8150u);
            for (uint32_t pc = 0x001F8154u; pc < 0x001F8170u; pc += 4u)
            {
                runtime.replaceFunction(pc, workerStep);
            }
        }

        if (legacyWorkerPumpsEnabled())
            installWorker2BackEdge(runtime);

        // Green-thread microkernel dump (TOL_TRACE_GTHREADS=1, see dumpGreenThreads).
        installGreenThreadProbe(runtime);


        // Script "finished?" entry point: this is the address the dispatchers actually call (the thunk at
        // 0x1F7340 jumps to FUN_001f88f0 by direct C++ call, so overriding 0x1F88F0 in the table would never
        // fire). Installed unconditionally because it carries the null-command-block fix.
        {
            PS2Runtime::RecompiledFunction scriptStatus = runtime.lookupFunction(kScriptStatusPc);
            if (scriptStatus != nullptr)
            {
                gScriptStatusPrev = scriptStatus;
                runtime.replaceFunction(kScriptStatusPc, &scriptStatusProbe);
            }
        }

        // Clear a stale transfer lock before the handler acquires it (see transferLockProbe).
        {
            PS2Runtime::RecompiledFunction lockFn = runtime.lookupFunction(kTransferLockPc);
            if (lockFn != nullptr)
            {
                gTransferLockPrev = lockFn;
                runtime.replaceFunction(kTransferLockPc, &transferLockProbe);
            }
        }

        // Pump the DVCI/ADXF engine whenever the guest yields (see yieldPump). Legacy host pump only: with the
        // guest microkernel running, 0x15231C's real save+yield must reach the run loop at 0x151F88.
        if (legacyWorkerPumpsEnabled())
        {
            PS2Runtime::RecompiledFunction yieldFn = runtime.lookupFunction(kYieldPc);
            PS2Runtime::RecompiledFunction dvciPump = runtime.lookupFunction(kDvciPumpPc);
            if (yieldFn != nullptr && dvciPump != nullptr)
            {
                gYieldPrev = yieldFn;
                gDvciPumpFn = dvciPump;
                runtime.replaceFunction(kYieldPc, &yieldPump);
            }
        }

        // Pump queued asset transfers from the I/O wait (see ioPumpProbe). Legacy host pump only: the microkernel
        // dispatches worker 2 itself, and re-entering it from the main stack kills thread 1.
        if (legacyWorkerPumpsEnabled())
        {
            PS2Runtime::RecompiledFunction ioPump = runtime.lookupFunction(kIoPumpPc);
            if (ioPump != nullptr)
            {
                gIoPumpPrev = ioPump;
                runtime.replaceFunction(kIoPumpPc, &ioPumpProbe);
            }
        }

        // Async load job probes (see loadStartProbe/loadStatusProbe).
        if (loadTraceEnabled())
        {
            PS2Runtime::RecompiledFunction loadStart = runtime.lookupFunction(kLoadStartPc);
            if (loadStart != nullptr)
            {
                gLoadStartPrev = loadStart;
                runtime.replaceFunction(kLoadStartPc, &loadStartProbe);
            }
            PS2Runtime::RecompiledFunction loadStatus = runtime.lookupFunction(kLoadStatusPc);
            if (loadStatus != nullptr)
            {
                gLoadStatusPrev = loadStatus;
                runtime.replaceFunction(kLoadStatusPc, &loadStatusProbe);
            }
            PS2Runtime::RecompiledFunction resourceLookup = runtime.lookupFunction(kResourceLookupPc);
            if (resourceLookup != nullptr)
            {
                gResourceLookupPrev = resourceLookup;
                runtime.replaceFunction(kResourceLookupPc, &resourceLookupProbe);
            }
            PS2Runtime::RecompiledFunction transferSubmit = runtime.lookupFunction(kTransferSubmitPc);
            if (transferSubmit != nullptr)
            {
                gTransferSubmitPrev = transferSubmit;
                runtime.replaceFunction(kTransferSubmitPc, &transferSubmitProbe);
            }
            PS2Runtime::RecompiledFunction reapFn = runtime.lookupFunction(kReapPc);
            if (reapFn != nullptr)
            {
                gReapPrev = reapFn;
                runtime.replaceFunction(kReapPc, &reapProbe);
            }
            PS2Runtime::RecompiledFunction handlerFn = runtime.lookupFunction(0x001F8530u);
            if (handlerFn != nullptr)
            {
                gTransferHandlerPrev = handlerFn;
                runtime.replaceFunction(0x001F8530u, &transferHandlerEntryProbe);
            }
            std::fprintf(stderr, "[load] probes installed on 0x%08X/0x%08X/0x%08X\n", kLoadStartPc, kLoadStatusPc,
                         kResourceLookupPc);
        }

        // Wait for pending transfers while driving the guest scheduler (see drivePendingTransfer).
        // EXPERIMENTAL / OFF BY DEFAULT: see the note at the status-stub block above.
        if (std::getenv("TOL_VIF_STATUS_WAIT") != nullptr && std::getenv("TOL_VIF_STATUS_REAL") != nullptr)
        {
            PS2Runtime::RecompiledFunction schedEntry = runtime.lookupFunction(kSchedEntryPc);
            PS2Runtime::RecompiledFunction statusThunk = runtime.lookupFunction(kStatusThunkPc);
            PS2Runtime::RecompiledFunction statusReal = runtime.lookupFunction(kStatusRealPc);
            if (schedEntry != nullptr && statusThunk != nullptr)
            {
                gSchedEntryFn = schedEntry;
                gStatusThunkPrev = statusThunk;
                runtime.replaceFunction(kStatusThunkPc, &statusThunkWait);
                if (statusReal != nullptr)
                {
                    gStatusRealPrev = statusReal;
                    runtime.replaceFunction(kStatusRealPc, &statusRealWait);
                }
                std::fprintf(stderr, "[load] transfer status wait installed (pumps guest scheduler)\n");
            }
        }

        // Script-context probes (see scriptPeekProbe/scriptAdvanceProbe).
        if (scriptTraceEnabled())
        {
            PS2Runtime::RecompiledFunction scriptPeek = runtime.lookupFunction(kScriptPeekPc);
            if (scriptPeek != nullptr)
            {
                gScriptPeekPrev = scriptPeek;
                runtime.replaceFunction(kScriptPeekPc, &scriptPeekProbe);
            }
            PS2Runtime::RecompiledFunction scriptAdvance = runtime.lookupFunction(kScriptAdvancePc);
            if (scriptAdvance != nullptr)
            {
                gScriptAdvancePrev = scriptAdvance;
                runtime.replaceFunction(kScriptAdvancePc, &scriptAdvanceProbe);
            }
            std::fprintf(stderr, "[script] probes installed on 0x%08X/0x%08X/0x%08X\n", kScriptStatusPc,
                         kScriptPeekPc, kScriptAdvancePc);
        }
        // 0x1F8480 is the VIF1 transfer status read used by the loading
        // callback 0x1660A0: it returns record[2] from the guest transfer table
        // at 0x4A72C0, which the runtime never retires (the VIF1 DMA packet is
        // not executed to completion), so it stays 2 (busy) forever and
        // FUN_00166010 never leaves its wait loop. The host GS backend already
        // consumes submitted packets synchronously, so report the transfer as
        // complete. The callback dispatches to thunk 0x1F7310, whose generated
        // body calls FUN_001f8480_0x1f8480 directly (bypassing the table), so
        // both slots are replaced.
        {
            // Forced "transfer complete" (opt-in, TOL_VIF_STATUS_STUB=1).
            //
            // FUN_001660A0 waits as  while (FUN_001f8480(handle) != 0) { FUN_0015231C(); }
            // and the yield really does return to the guest run loop (FUN_00151E90 at 0x151F88), so the
            // microkernel keeps running other threads while this one waits - verified on hardware
            // 2026-11-06. The earlier claim that "the wait only yields and nothing pumps I/O" was wrong.
            // Leaving the stub off is the truthful configuration; the deadlock it used to hide is the
            // thing to diagnose (see TOL_TRACE_XFER).
            // DEFAULT OFF as of the 2026-11-06 reassessment: 0x1F8480 reads desc[2] from the *guest's own*
            // transfer table (0x4A72C0 + (handle & 0x3F) * 0x2C, validated against desc[0x8]) - it is not a
            // hardware register, so answering it with a constant fabricates guest state and makes the waiter
            // proceed before the data lands. Opt in with TOL_VIF_STATUS_STUB=1 to reproduce the old race.
            if (std::getenv("TOL_VIF_STATUS_STUB") != nullptr)
            {
                PS2Runtime::RecompiledFunction statusDone =
                    [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *) {
                        (void)rdram;
                        SET_GPR_S32(ctx, 2, 0);
                        ctx->pc = GPR_U32(ctx, 31);
                    };
                runtime.replaceFunction(0x001F7310u, statusDone);
                runtime.replaceFunction(0x001F8480u, statusDone);
            }
        }
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
        if (dvciTraceEnabled())
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kDvciPollPc);
            if (original == nullptr)
            {
                std::fprintf(stderr, "[dvci] 0x%08X is not in the function table\n", kDvciPollPc);
            }
            else
            {
                gDvciPollOriginal = original;
                runtime.replaceFunction(kDvciPollPc, &dvciPollProbe);
                std::fprintf(stderr, "[dvci] probe installed on 0x%08X\n", kDvciPollPc);
            }
            PS2Runtime::RecompiledFunction errorPrint = runtime.lookupFunction(kDvciErrorPrintPc);
            if (errorPrint != nullptr)
            {
                gDvciErrorPrintOriginal = errorPrint;
                runtime.replaceFunction(kDvciErrorPrintPc, &dvciErrorPrintProbe);
                std::fprintf(stderr, "[dvci] error-print probe installed on 0x%08X\n", kDvciErrorPrintPc);
            }
            PS2Runtime::RecompiledFunction start = runtime.lookupFunction(kDvciStartPc);
            if (start != nullptr)
            {
                gDvciStartOriginal = start;
                runtime.replaceFunction(kDvciStartPc, &dvciStartProbe);
                std::fprintf(stderr, "[dvci] start probe installed on 0x%08X\n", kDvciStartPc);
            }
            PS2Runtime::RecompiledFunction step = runtime.lookupFunction(kAdxfStepPc);
            if (step != nullptr)
            {
                gAdxfStepOriginal = step;
                runtime.replaceFunction(kAdxfStepPc, &adxfStepProbe);
                std::fprintf(stderr, "[adxf] step probe installed on 0x%08X\n", kAdxfStepPc);
            }
            PS2Runtime::RecompiledFunction tick = runtime.lookupFunction(kDvciTickPc);
            if (tick != nullptr)
            {
                gDvciTickOriginal = tick;
                runtime.replaceFunction(kDvciTickPc, &dvciTickProbe);
                std::fprintf(stderr, "[dvci] tick probe installed on 0x%08X\n", kDvciTickPc);
            }
            PS2Runtime::RecompiledFunction read = runtime.lookupFunction(kCriDeviceReadPc);
            if (read != nullptr)
            {
                gCriReadOriginal = read;
                runtime.replaceFunction(kCriDeviceReadPc, &criReadProbe);
                std::fprintf(stderr, "[dvci] device read probe installed on 0x%08X\n", kCriDeviceReadPc);
            }
            PS2Runtime::RecompiledFunction gameRead = runtime.lookupFunction(kGameReadPc);
            if (gameRead != nullptr)
            {
                gGameReadOriginal = gameRead;
                runtime.replaceFunction(kGameReadPc, &gameReadProbe);
                std::fprintf(stderr, "[dvci] game read probe installed on 0x%08X\n", kGameReadPc);
            }
            PS2Runtime::RecompiledFunction adxfRead = runtime.lookupFunction(kAdxfReadStepPc);
            if (adxfRead != nullptr)
            {
                gAdxfReadStepOriginal = adxfRead;
                runtime.replaceFunction(kAdxfReadStepPc, &adxfReadStepProbe);
                std::fprintf(stderr, "[adxf] read step probe installed on 0x%08X\n", kAdxfReadStepPc);
            }
            PS2Runtime::RecompiledFunction adxfArm = runtime.lookupFunction(kAdxfArmPc);
            if (adxfArm != nullptr)
            {
                gAdxfArmOriginal = adxfArm;
                runtime.replaceFunction(kAdxfArmPc, &adxfArmProbe);
                std::fprintf(stderr, "[adxf] arm probe installed on 0x%08X\n", kAdxfArmPc);
            }
            PS2Runtime::RecompiledFunction adxfPump = runtime.lookupFunction(kAdxfPumpPc);
            if (adxfPump != nullptr)
            {
                gAdxfPumpOriginal = adxfPump;
                runtime.replaceFunction(kAdxfPumpPc, &adxfPumpProbe);
                std::fprintf(stderr, "[adxf] pump probe installed on 0x%08X\n", kAdxfPumpPc);
            }
            PS2Runtime::RecompiledFunction ioWait = runtime.lookupFunction(kIoWaitPc);
            if (ioWait != nullptr)
            {
                gIoWaitOriginal = ioWait;
                runtime.replaceFunction(kIoWaitPc, &ioWaitProbe);
                std::fprintf(stderr, "[adxf] io-wait probe installed on 0x%08X\n", kIoWaitPc);
            }
        }

        // Unconditional DVCI completion fix (see dvciCompletionFix). Installed after the trace probe, so it wraps
        // the probe when TOL_TRACE_DVCI is on and the plain poll otherwise.
        gDvciCompletionPrev = runtime.lookupFunction(kDvciPollPc);
        runtime.replaceFunction(kDvciPollPc, &dvciCompletionFix);

        // Behaviour-changing replacements live OUTSIDE the TOL_TRACE_DVCI gate; each is enabled by its own variable.
        // Gating them behind the trace made the window fix look timing-dependent - it was simply never installed
        // unless the probes were on. The DVCI size shim that used to live here is gone: the file server's open
        // reply carries the byte size now, so the guest derives the sector count itself.
        if (std::getenv("TOL_CRI_IO_LATENCY") != nullptr)
        {
            PS2Runtime::RecompiledFunction status = runtime.lookupFunction(kCriDeviceStatusPc);
            if (status != nullptr)
            {
                gCriStatusOriginal = status;
                runtime.replaceFunction(kCriDeviceStatusPc, &criStatusLatencyProbe);
                std::fprintf(stderr, "[dvci] io-latency shim installed on 0x%08X\n", kCriDeviceStatusPc);
            }
        }
        // CRI pump placement: slot 5 gets the full main-thread pump (FUN_00198230) instead of the SJRBF-only
        // FUN_00198210. Unconditional - this is a fix, not a probe (see the block comment at the definition).
        {
            PS2Runtime::RecompiledFunction registrar = runtime.lookupFunction(kPumpRegisterPc);
            if (registrar != nullptr)
            {
                gPumpRegisterOriginal = registrar;
                runtime.replaceFunction(kPumpRegisterPc, &pumpRegisterProbe);
            }
            else
            {
                std::fprintf(stderr, "[cri] 0x%08X (pump registrar) is not in the function table\n",
                             kPumpRegisterPc);
            }
        }
        if (std::getenv("TOL_TRACE_CRI_TIME") != nullptr)
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kCriStateMachinePc);
            if (original != nullptr)
            {
                gCriStateOriginal = original;
                runtime.replaceFunction(kCriStateMachinePc, &criStateTimeProbe);
                std::fprintf(stderr, "[cri] time probe installed on 0x%08X\n", kCriStateMachinePc);
            }
            else
            {
                std::fprintf(stderr, "[cri] 0x%08X (state machine) is not in the function table\n",
                             kCriStateMachinePc);
            }
        }
        std::fprintf(stderr, "[tol] no native overrides installed (the Timer-2 override was withdrawn)\n");
    }
}



