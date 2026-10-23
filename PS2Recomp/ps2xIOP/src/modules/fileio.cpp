#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kFileIoSid = 0x80000001u;
        constexpr std::array<uint32_t, 1> kSids{kFileIoSid};

        // The fileio/ioman service the EE's own libkernel talks to.  Its init
        // call (0xff) is a hand shake: the EE compares the first reply word
        // against its own version literal and remembers the protocol revision
        // from the second word, and it refuses to open anything until both
        // match (FUN_00119770 -> "ready" only when the word is "3000").
        constexpr uint32_t kRpcInit = 0xFFu;
        constexpr uint32_t kRpcOpen = 0x0u;
        constexpr uint32_t kRpcClose = 0x1u;
        constexpr uint32_t kRpcRead = 0x2u;
        constexpr uint32_t kRpcWrite = 0x3u;
        constexpr uint32_t kRpcLseek = 0x4u;
        constexpr uint32_t kIopOsVersion = 0x30303033u; // "3000"
        constexpr uint32_t kFileIoProtocolVersion = 2u;

        // Every call carries the same request header in the send buffer
        // (measured from the guest's own sceOpen, FUN_00119A68):
        //   +0x00  semaphore the EE's completion callback signals
        //   +0x04  EE address the operation result is written to
        //   +0x08  size of that result
        //   +0x0C  open flags
        //   +0x10  the caller's fourth argument
        //   +0x14  NUL-terminated path (open only)
        //   +0x414 the caller's file slot (the fd it computed on its side)
        //
        // The other calls reuse the same header shape and always end with the
        // caller's file slot in the last word of the request:
        //   close 0x14: +0x0C fd,                       +0x10 slot
        //   read  0x20: +0x0C fd, +0x10 buffer, +0x14 size,   +0x1C slot
        //   write 0x30: +0x0C fd, +0x10 buffer, +0x14 size,   +0x2C slot
        //   lseek 0x1C: +0x0C fd, +0x10 offset, +0x14 whence, +0x18 slot
        constexpr uint32_t kResultPointerOffset = 0x04u;
        constexpr uint32_t kResultSizeOffset = 0x08u;
        constexpr uint32_t kFlagsOffset = 0x0Cu;
        constexpr uint32_t kArgument0Offset = 0x10u;
        constexpr uint32_t kArgument1Offset = 0x14u;
        constexpr uint32_t kPathOffset = 0x14u;
        constexpr uint32_t kPathCapacity = 0x400u;
        constexpr uint32_t kSlotOffset = 0x414u;
        constexpr uint32_t kMaxTransferSize = 16u * 1024u * 1024u;

        constexpr uint32_t kDumpBytes = 96u;

        std::string hexDump(IopHost &host, GuestBuffer buffer)
        {
            if (buffer.address == 0u || buffer.size == 0u)
                return {};

            const uint32_t limit =
                std::min<uint32_t>(buffer.size, std::min<uint32_t>(kDumpBytes, 512u));
            std::vector<uint8_t> bytes(limit, 0u);
            if (!host.readGuest(buffer.address, bytes.data(), bytes.size()))
                return "<unreadable>";

            std::ostringstream out;
            out << std::hex << std::setfill('0');
            for (size_t i = 0; i < bytes.size(); ++i)
            {
                out << (i % 4u == 0u ? " " : "") << std::setw(2)
                    << static_cast<uint32_t>(bytes[i]);
            }
            out << " |";
            for (const uint8_t value : bytes)
                out << (value >= 32u && value < 127u ? static_cast<char>(value) : '.');
            out << "|";
            return out.str();
        }

        std::string readGuestCString(IopHost &host, uint32_t address, uint32_t limit)
        {
            if (address == 0u || limit == 0u)
                return {};

            std::vector<char> bytes(limit, '\0');
            if (!host.readGuest(address, bytes.data(), bytes.size()))
                return {};

            const auto terminator = std::find(bytes.begin(), bytes.end(), '\0');
            return std::string(bytes.begin(), terminator);
        }

        // The EE's fileio client.  Every call carries the request in the send
        // buffer and expects a small reply; the actual file lives on the host,
        // so the service does the I/O itself through IopHost's VFS.
        class FileIoService final : public IopService
        {
        public:
            explicit FileIoService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "fileio";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // ioman is part of the IOP OS image (IOPRP300.IMG), not of the
            // game's module set, so nothing ever loads it here.
            [[nodiscard]] bool alwaysAvailable() const override
            {
                return true;
            }

            void reset() override
            {
                for (auto &entry : m_files)
                {
                    if (entry.second.hostHandle != 0u)
                        m_host.closeHostFile(entry.second.hostHandle);
                }
                m_files.clear();
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result{};
                if (request.sid != kFileIoSid)
                {
                    return result;
                }

                if (request.function == kRpcInit)
                {
                    const std::array<uint32_t, 2> reply{kIopOsVersion, kFileIoProtocolVersion};
                    result.handled = writeRpcWords(m_host, request.receive, reply);
                    logCall(request, "init");
                    logHeapState("init");
                    return result;
                }

                if (request.function == kRpcOpen)
                {
                    return handleOpen(request);
                }

                if (request.function == kRpcClose)
                {
                    return handleClose(request);
                }

                if (request.function == kRpcRead)
                {
                    return handleRead(request);
                }

                if (request.function == kRpcLseek)
                {
                    return handleLseek(request);
                }

                // TEMP DIAGNOSTIC (replace with the real handling as each
                // call is mapped): the guest falls back to the fabricated
                // zero reply, so the run still ends at the same trap while
                // this records the full protocol.
                logCall(request, "UNMAPPED");
                return result;
            }

        private:
            struct OpenFile
            {
                uint64_t hostHandle = 0u;
                uint64_t size = 0u;
                uint64_t position = 0u;
            };

            // Every request ends with the caller's file slot in its last word
            // (see the header layout above), so it does not need per-call
            // offsets.
            [[nodiscard]] uint32_t requestSlot(const RpcRequest &request) const
            {
                return requestWord(request, requestSlotOffset(request));
            }

            [[nodiscard]] static uint32_t requestSlotOffset(const RpcRequest &request)
            {
                return request.send.size >= sizeof(uint32_t) ? request.send.size - sizeof(uint32_t) : 0u;
            }

            [[nodiscard]] uint32_t requestWord(const RpcRequest &request, uint32_t offset) const
            {
                uint32_t value = 0u;
                if (request.send.address == 0u || offset + sizeof(value) > request.send.size)
                    return 0u;
                (void)m_host.readGuest(request.send.address + offset, &value, sizeof(value));
                return value;
            }

            // The IOP answers with a non-zero "accepted" word in the caller's
            // receive buffer and writes the operation result to the pointer in
            // the request header, which the guest reads after its semaphore is
            // signalled.
            bool completeRequest(const RpcRequest &request, uint32_t value)
            {
                const std::array<uint32_t, 1> reply{1u};
                const bool replied = writeRpcWords(m_host, request.receive, reply);
                const uint32_t resultPointer = requestWord(request, kResultPointerOffset);
                const uint32_t resultSize = requestWord(request, kResultSizeOffset);
                if (resultPointer != 0u && resultSize >= sizeof(uint32_t))
                {
                    (void)m_host.writeGuest(resultPointer, &value, sizeof(value));
                }
                return replied;
            }

            void logCompletion(const RpcRequest &request, const char *what, uint32_t slot,
                               int32_t value, size_t bytes) const
            {
                std::ostringstream out;
                out << "[IOP:fileio] " << what << " slot=" << slot << " value=" << value;
                if (bytes != 0u)
                    out << " bytes=" << bytes;
                out << " handled=1";
                m_host.log(LogLevel::Info, out.str());
                (void)request;
            }

            // TEMP DIAGNOSTIC: the guest allocates the read destination with
            // its own heap (FUN_001013b0 -> FUN_00100700, context pointer kept
            // at 0x003DAF84 and filled by FUN_00100230).  At read time the
            // context is all zeroes, so the allocation returns NULL and the AFS
            // loader passes a NULL buffer to sceRead.  Log the context on every
            // fileio request to find the point where it empties.
            void logHeapState(const char *tag) const
            {
                uint32_t heapContext = 0u;
                if (!m_host.readGuest(0x003DAF84u, &heapContext, sizeof(heapContext)))
                    return;

                std::ostringstream out;
                out << "[IOP:heap] " << tag << " ctx=0x" << std::hex << heapContext;
                if (heapContext != 0u && heapContext < 0x02000000u)
                {
                    for (uint32_t offset = 0u; offset < 0x40u; offset += 4u)
                    {
                        uint32_t word = 0u;
                        if (m_host.readGuest(heapContext + offset, &word, sizeof(word)))
                            out << " +" << std::dec << offset << "=0x" << std::hex
                                << std::setw(8) << std::setfill('0') << word;
                    }
                }
                m_host.log(LogLevel::Info, out.str());
            }

            [[nodiscard]] RpcResult handleClose(const RpcRequest &request)
            {
                RpcResult result{};
                const uint32_t slot = requestSlot(request);
                bool closed = false;
                const auto it = m_files.find(slot);
                if (it != m_files.end())
                {
                    if (it->second.hostHandle != 0u)
                        m_host.closeHostFile(it->second.hostHandle);
                    m_files.erase(it);
                    closed = true;
                }
                const uint32_t value = closed ? 0u : static_cast<uint32_t>(-1);
                result.handled = completeRequest(request, value);
                logCompletion(request, "close", slot, static_cast<int32_t>(value), 0u);
                logHeapState("close");
                return result;
            }

            [[nodiscard]] RpcResult handleRead(const RpcRequest &request)
            {
                RpcResult result{};
                const uint32_t slot = requestSlot(request);
                const uint32_t buffer = requestWord(request, kArgument0Offset);
                const uint32_t requested = requestWord(request, kArgument1Offset);

                int32_t value = -1;
                size_t transferred = 0u;
                bool hostOk = false;
                bool guestOk = false;
                uint64_t position = 0u;
                uint64_t fileSize = 0u;

                const auto it = m_files.find(slot);
                if (it != m_files.end())
                {
                    position = it->second.position;
                    fileSize = it->second.size;
                }

                if (it != m_files.end() && it->second.hostHandle != 0u && buffer != 0u &&
                    requested != 0u && requested <= kMaxTransferSize)
                {
                    if (it->second.position >= it->second.size)
                    {
                        value = 0;
                    }
                    else
                    {
                        const size_t available = static_cast<size_t>(it->second.size - it->second.position);
                        const size_t chunk = std::min<size_t>(requested, available);
                        std::vector<uint8_t> data(chunk, 0u);
                        size_t got = 0u;
                        hostOk = m_host.readHostFile(it->second.hostHandle, it->second.position,
                                                     data.data(), data.size(), got);
                        if (hostOk && got != 0u)
                        {
                            guestOk = m_host.writeGuest(buffer, data.data(), got);
                            if (guestOk)
                            {
                                it->second.position += got;
                                transferred = got;
                                value = static_cast<int32_t>(got);
                            }
                        }
                    }
                }

                result.handled = completeRequest(request, static_cast<uint32_t>(value));

                // TEMP DIAGNOSTIC: the read path still has one unresolved
                // mismatch (the guest asks for data and gets -1), so record the
                // whole decision instead of just the outcome.
                std::ostringstream out;
                out << "[IOP:fileio] read slot=" << slot
                    << " buffer=0x" << std::hex << buffer
                    << " requested=0x" << requested << std::dec
                    << " pos=" << position << " size=" << fileSize
                    << " hostOk=" << (hostOk ? 1 : 0)
                    << " guestOk=" << (guestOk ? 1 : 0)
                    << " value=" << value;

                // TEMP DIAGNOSTIC: the caller allocates the destination with
                // the guest's own heap and got NULL, so record the request
                // words; logHeapState() below dumps the heap context.
                out << " req=";
                for (uint32_t i = 0u; i < request.send.size; i += 4u)
                {
                    out << std::hex << std::setw(8) << std::setfill('0')
                        << requestWord(request, i) << " ";
                }
                m_host.log(LogLevel::Info, out.str());
                logHeapState("read");
                return result;
            }

            [[nodiscard]] RpcResult handleLseek(const RpcRequest &request)
            {
                RpcResult result{};
                const uint32_t slot = requestSlot(request);
                const int32_t offset = static_cast<int32_t>(requestWord(request, kArgument0Offset));
                const uint32_t whence = requestWord(request, kArgument1Offset);

                int32_t value = -1;
                const auto it = m_files.find(slot);
                if (it != m_files.end() && it->second.hostHandle != 0u && whence <= 2u)
                {
                    int64_t base = 0;
                    if (whence == 1u)
                        base = static_cast<int64_t>(it->second.position);
                    else if (whence == 2u)
                        base = static_cast<int64_t>(it->second.size);

                    int64_t target = base + static_cast<int64_t>(offset);
                    if (target < 0)
                        target = 0;
                    if (target > static_cast<int64_t>(it->second.size))
                        target = static_cast<int64_t>(it->second.size);
                    it->second.position = static_cast<uint64_t>(target);
                    value = static_cast<int32_t>(target);
                }

                result.handled = completeRequest(request, static_cast<uint32_t>(value));
                logCompletion(request, "lseek", slot, value, 0u);
                logHeapState("lseek");
                return result;
            }

            [[nodiscard]] RpcResult handleOpen(const RpcRequest &request)
            {
                RpcResult result{};
                if (request.send.address == 0u || request.send.size < kPathOffset + 1u)
                {
                    return result;
                }

                uint32_t flags = 0u;
                uint32_t resultPointer = 0u;
                uint32_t resultSize = 0u;
                uint32_t slot = 0u;
                (void)m_host.readGuest(request.send.address + kFlagsOffset, &flags, sizeof(flags));
                (void)m_host.readGuest(request.send.address + kResultPointerOffset, &resultPointer,
                                       sizeof(resultPointer));
                (void)m_host.readGuest(request.send.address + kResultSizeOffset, &resultSize,
                                       sizeof(resultSize));
                if (request.send.size >= kSlotOffset + sizeof(slot))
                    (void)m_host.readGuest(request.send.address + kSlotOffset, &slot, sizeof(slot));

                const uint32_t pathCapacity =
                    std::min<uint32_t>(kPathCapacity, request.send.size - kPathOffset);
                const std::string path =
                    readGuestCString(m_host, request.send.address + kPathOffset, pathCapacity);
                if (path.empty())
                {
                    return result;
                }

                const std::string hostPath = m_host.translateGuestPath(path);
                const uint64_t handle = hostPath.empty() ? 0u : m_host.openHostFile(hostPath);
                if (handle == 0u)
                {
                    std::ostringstream out;
                    out << "[IOP:fileio] open FAILED path='" << path
                        << "' host='" << hostPath << "'";
                    m_host.log(LogLevel::Warning, out.str());
                    return result;
                }

                uint64_t size = 0u;
                (void)m_host.hostFileSize(handle, size);
                OpenFile &file = m_files[slot];
                if (file.hostHandle != 0u)
                    m_host.closeHostFile(file.hostHandle);
                file.hostHandle = handle;
                file.size = size;
                file.position = 0u;

                // The guest only tests the reply word for non-zero and takes
                // the file slot it allocated on its own side as the fd.
                const std::array<uint32_t, 1> reply{1u};
                result.handled = writeRpcWords(m_host, request.receive, reply);

                // The operation result lands where the request header points,
                // which is on the caller's stack rather than in the reply.
                if (resultPointer != 0u && resultSize >= sizeof(uint32_t))
                {
                    const uint32_t initial = 0u;
                    (void)m_host.writeGuest(resultPointer, &initial, sizeof(initial));
                }

                std::ostringstream out;
                out << "[IOP:fileio] open slot=" << slot << " flags=0x" << std::hex << flags
                    << std::dec << " size=" << size << " path='" << path << "' -> host='"
                    << hostPath << "' handled=" << (result.handled ? 1 : 0);
                m_host.log(LogLevel::Info, out.str());
                logHeapState("open");
                return result;
            }

            void logCall(const RpcRequest &request, const char *what)
            {
                std::ostringstream out;
                out << "[IOP:fileio] " << what << " rpc=0x" << std::hex << request.function
                    << " mode=0x" << request.mode
                    << " send=0x" << request.send.address
                    << " send_size=0x" << request.send.size
                    << " recv=0x" << request.receive.address
                    << " recv_size=0x" << request.receive.size
                    << std::dec << " data:" << hexDump(m_host, request.send);
                m_host.log(LogLevel::Info, out.str());
            }

            IopHost &m_host;
            std::unordered_map<uint32_t, OpenFile> m_files;
        };
    }

    std::unique_ptr<IopService> createFileIoService(IopHost &host)
    {
        return std::make_unique<FileIoService>(host);
    }
}
