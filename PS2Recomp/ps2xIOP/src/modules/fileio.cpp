#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
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
        constexpr uint32_t kResultPointerOffset = 0x04u;
        constexpr uint32_t kResultSizeOffset = 0x08u;
        constexpr uint32_t kFlagsOffset = 0x0Cu;
        constexpr uint32_t kPathOffset = 0x14u;
        constexpr uint32_t kPathCapacity = 0x400u;
        constexpr uint32_t kSlotOffset = 0x414u;

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
            out << std::hex;
            for (size_t i = 0; i < bytes.size(); ++i)
            {
                out << (i % 4u == 0u ? " " : "") << static_cast<uint32_t>(bytes[i]);
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
                    return result;
                }

                if (request.function == kRpcOpen)
                {
                    return handleOpen(request);
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
