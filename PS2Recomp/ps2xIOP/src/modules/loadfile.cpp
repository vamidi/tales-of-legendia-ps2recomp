#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kLoadFileSid = 0x80000006u;
        constexpr std::array<uint32_t, 1> kSids{kLoadFileSid};

        // The RPC numbers the EE-side loader uses on this sid:
        //   * 0xff queries the IOP OS version (the guest compares the answer
        //     with its own "3000" literal before it trusts the service), and
        //   * 0 loads a module by name - the EE sends a 512-byte path buffer
        //     ("cdrom0:\IRX\<name>.IRX;1") and expects the module id back.
        constexpr uint32_t kRpcGetVersion = 0xFFu;
        constexpr uint32_t kRpcLoadModule = 0x0u;
        constexpr uint32_t kIopOsVersion = 0x30303033u; // "3000"
        constexpr uint32_t kMaxModulePath = 512u;

        // The EE passes one buffer for both directions, laid out as
        //   { int result; int moduleId; char name[...]; }
        // so the module name starts 8 bytes in and the reply overwrites the
        // result/moduleId words.  (The guest accepted an all-zero answer and
        // retries the load when the result word comes back negative, so the
        // first word is the status, not the id.)
        constexpr uint32_t kCommandHeaderSize = 8u;
        constexpr uint32_t kResultOk = 0u;
        constexpr uint32_t kResultFailed = 0xFFFFFFFFu;

        [[nodiscard]] std::string readGuestCString(IopHost &host, GuestBuffer buffer)
        {
            if (buffer.address == 0u || buffer.size == 0u)
            {
                return {};
            }

            const uint32_t limit = std::min<uint32_t>(buffer.size, kMaxModulePath);
            std::vector<char> bytes(limit, '\0');
            if (!host.readGuest(buffer.address, bytes.data(), bytes.size()))
            {
                return {};
            }

            const auto terminator = std::find(bytes.begin(), bytes.end(), '\0');
            return std::string(bytes.begin(), terminator);
        }

        // The IOP OS's own module loader.  On hardware this is the code inside
        // IOPRP300.IMG (ioman: LoadModuleByEE) that reads an IRX off the CD and
        // hands it to loadcore; the emulated IOP provides it directly, which is
        // also the only way the guest's modules ever reach the emulator.
        class LoadFileService final : public IopService
        {
        public:
            LoadFileService(IopHost &host, LoadModuleCallback load)
                : m_host(host),
                  m_load(std::move(load))
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "loadfile";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // Every other module load goes through this service, so it cannot
            // itself wait for a module to be marked loaded.
            [[nodiscard]] bool alwaysAvailable() const override
            {
                return true;
            }

            void reset() override
            {
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result{};
                if (request.sid != kLoadFileSid)
                {
                    return result;
                }

                if (request.function == kRpcGetVersion)
                {
                    const std::array<uint32_t, 1> version{kIopOsVersion};
                    result.handled = writeRpcWords(m_host, request.receive, version);
                    return result;
                }

                if (request.function != kRpcLoadModule)
                {
                    return result;
                }

                const GuestBuffer nameBuffer{
                    request.send.address + kCommandHeaderSize,
                    request.send.size > kCommandHeaderSize ? request.send.size - kCommandHeaderSize : 0u};
                const std::string path = readGuestCString(m_host, nameBuffer);
                if (path.empty())
                {
                    return result;
                }

                const ModuleLoadResult loaded = m_load ? m_load(path) : ModuleLoadResult{};
                const bool success = loaded.moduleId > 0;
                const std::array<uint32_t, 2> reply{
                    success ? kResultOk : kResultFailed,
                    success ? static_cast<uint32_t>(loaded.moduleId) : 0u};
                result.handled = writeRpcWords(m_host, request.receive, reply);

                std::ostringstream out;
                out << "[IOP:loadfile] module='" << path << "' "
                    << (success ? "loaded id=0x" : "FAILED result=0x") << std::hex
                    << (success ? static_cast<uint32_t>(loaded.moduleId) : kResultFailed)
                    << " handled=" << (loaded.handled ? 1 : 0) << std::dec;
                m_host.log(LogLevel::Info, out.str());
                return result;
            }

        private:
            IopHost &m_host;
            LoadModuleCallback m_load;
        };
    }

    std::unique_ptr<IopService> createLoadFileService(IopHost &host, LoadModuleCallback load)
    {
        return std::make_unique<LoadFileService>(host, std::move(load));
    }
}
