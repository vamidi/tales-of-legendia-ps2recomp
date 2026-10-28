#include "iop_ioman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace ps2x::iop::detail
{
    IopIoman::IopIoman(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopIoman::reset()
    {
        m_devices.clear();
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            // TEMP DIAGNOSTIC (session 37): which ioman entry points does IOP
            // module code actually call?  Only AddDrv/DelDrv are implemented
            // above, so an AFS/CVFS module that opens an archive needs these
            // and currently gets nothing.  Bounded, print-once style.
            {
                static uint32_t s_logged = 0u;
                if (s_logged < 40u)
                {
                    ++s_logged;
                    std::string path;
                    const uint32_t pathArg = m_memory.read32(a0);
                    if (pathArg >= 0x1000u && pathArg < 0x200000u)
                    {
                        path = m_memory.readString(pathArg, 64u);
                    }
                    std::cerr << "[IOP:ioman] ordinal=" << ordinal << " a0=0x" << std::hex << a0
                              << std::dec << " path=\"" << path << "\"" << std::endl;
                }
            }
            return false;
        }
    }
}
