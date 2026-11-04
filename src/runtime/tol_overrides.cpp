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

    // fn 1: {handle, blocks, buffer, info[0], info[1], info[2], desc, countOut}
    bool gameFileRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t request)
    {
        if (request == 0u)
            return false;
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

        std::vector<uint8_t> data(size);
        size_t read = 0u;
        if (size != 0u)
        {
            if (std::fseek(gGameFileStream[slot], static_cast<long>(offset), SEEK_SET) != 0)
                return false;
            read = std::fread(data.data(), 1, size, gGameFileStream[slot]);
        }
        for (size_t i = 0; i < read; ++i)
        {
            runtime->Store8(rdram, ctx, buffer + static_cast<uint32_t>(i), data[i]);
        }
        completeGameIo(runtime, rdram, ctx, countOut, static_cast<uint32_t>(read));
        releaseGameIoLock(rdram, ctx, runtime);
        if (std::getenv("TOL_TRACE_FILEIO") != nullptr)
        {
            std::fprintf(stderr, "[fileio] gamefile read sector 0x%X (file slot %d +%u) %zu/%u bytes at 0x%08X\n",
                         sector, slot, sectorOffset, read, size, buffer);
        }
        return true;
    }

    // IxAudio's server, sid 0x90000200: the module carries a complete server record for it (link vaddr
    // 0xD180 in IXAUDIO.IRX) but never calls sceSifRegisterRpc, and no other module does either, so the
    // game's SJX_Init fails and the boot stalls on its loading screen.
    //
    // The functions the game drives are thin wrappers that use the reply as an object handle:
    //   fn 0x002  DTX create      (FUN_001acac8 stores the non-zero reply as the DTX handle)
    //   fn 0x422  SJUNI remote    (FUN_001b6400 -> ps2rna_init_psj loops forever on a zero reply)
    // Both are answered with a fresh handle; the rest reply 0. Audio is not produced - the module that would
    // stream it never registers - but the game's initialisation no longer dead-ends.
    constexpr uint32_t kIxAudioSid = 0x90000200u;
    constexpr uint32_t kIxAudioHandleFunctions[] = {0x002u, 0x400u, 0x422u};
    uint32_t gIxAudioNextHandle = 1u;

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
                SET_GPR_S32(ctx, 2, 0);
                return;
            }
            if (function == kGameDataReadFunction && gameFileRead(rdram, ctx, runtime, sendBuffer))
            {
                SET_GPR_S32(ctx, 2, 0);
                return;
            }
            // Unsupported: leave the caller's state consistent rather than half-done.
            completeGameIo(runtime, rdram, ctx, 0u, 0u);
            SET_GPR_S32(ctx, 2, -1);
            return;
        }

        // IxAudio: answer the DTX lifecycle so the game's SJX_Init completes. Audio itself is not produced -
        // the module that would stream it never registers - but the boot no longer stops on the failure.
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
            // The 0x400/0x422 pair is re-issued every few seconds. Dump what the game actually sends (it is
            // the only description of the request we have) together with the reply and the guest return path,
            // so the pair can be matched against the SJX remote semantics instead of guessed at.
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
            std::fprintf(stderr, "[pc] exit  0x%08X #%u pc=0x%08X v0=0x%08X\n", gPcProbeAddr[idx], call, ctx->pc,
                         GPR_U32(ctx, 2));
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
            gAdxfStepOriginal(rdram, ctx, runtime);

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

        if (call <= 6u || (stateAtEntry == 3u && sSawThree <= 20u) || (indexAtEntry == 5u && sLoadFive++ < 10u))
        {
            std::fprintf(stderr,
                         "[poll-in] #%u i=%u ptid=%d state=%u count=%u progress=%u total=%u consumed=%u transfer=%u"
                         " | sState=%u sStream=0x%08X sTotal=%u sDone=%u sBlocks=%u sWin=%u sDevBase=0x%X sDev=%u"
                         " | saw3=%u sawCount=%u\n",
                         call, indexAtEntry, static_cast<int32_t>(ptidAtEntry), stateAtEntry, countAtEntry,
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

        const bool log = call <= 16u || (call % 512u) == 0u;
        if (gDvciPollOriginal != nullptr)
            gDvciPollOriginal(rdram, ctx, runtime);

        if (log)
        {
            dumpDvci(rdram, ctx, runtime, call, static_cast<int32_t>(GPR_U32(ctx, 2)));
        }
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
        }

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
        std::fprintf(stderr, "[tol] no native overrides installed (the Timer-2 override was withdrawn)\n");
    }
}



