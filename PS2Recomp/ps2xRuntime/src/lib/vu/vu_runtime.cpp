#include "runtime/vu/vu_runtime.h"
#include "runtime/vu/vu_unit.h"
#include "runtime/vu/vu_decoder.h"
#include "ps2_runtime.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace ps2vu
{
    // Guard against a VU microprogram that never reaches its end (E-bit / program
    // end). This runtime waits *synchronously* for the running microprogram before
    // starting the next one, so a program that never terminates -- one waiting on
    // VIF data that cannot arrive while the EE thread is blocked here, or one
    // started on microcode that was never uploaded -- would hang the emulator
    // forever with no output. Bound the wait, report the stuck state, then reset the
    // unit so the next start/resume is legal (reset() is the only transition back
    // to Ready; start() rejects Running/Fault and resume() requires Ready).
    // The spin budgets are deliberately small: wait() normally exits immediately
    // (the VU clock drains the running program as the EE clock advances), so this
    // loop only spins when the program is genuinely stuck. 4096 spins = 262144 VU
    // cycles ~= 10% of one PAL frame -- far above any legitimate drain, while the
    // forced reset stays cheap enough to survive repeated hits (a Debug-build
    // interpreter costs ~75us per 64-cycle step).
    constexpr uint64_t kWaitSpinBudgetVu1 = 1ull << 12; // ~262k VU cycles (~1.8ms of VU time)
    constexpr uint64_t kWaitSpinBudgetVu0 = 1ull << 14; // ~1.05M VU cycles (~7ms of VU time)
    constexpr uint64_t kWaitWarnEverySpins = 1ull << 10;
    constexpr uint32_t kWaitMaxWarnReports = 4u;
    constexpr uint64_t kTransferSpinBudget = 1ull << 12;
    constexpr uint32_t kTransferMaxWarnReports = 4u;

    Runtime::Runtime(PS2Memory &memory, std::function<void(uint32_t)> advanceClock)
        : m_memory(memory), m_advanceClock(std::move(advanceClock))
    {
        MemoryServices services;
        services.context = this;
        services.readShared = readShared;
        services.writeShared = writeShared;
        services.submitGif = submitGif;
        for (uint32_t index = 0; index < 2; ++index)
        {
            const bool vu1 = index != 0;
            services.data = vu1 ? memory.getVU1Data() : memory.getVU0Data();
            m_units[index] = std::make_unique<Unit>(vu1, std::span(vu1 ? memory.getVU1Code() : memory.getVU0Code(), vu1 ? PS2_VU1_CODE_SIZE : PS2_VU0_CODE_SIZE), services);
        }
    }

    Runtime::~Runtime() = default;
    Unit &Runtime::unit(bool vu1) noexcept { return *m_units[vu1]; }

    bool Runtime::failed() const noexcept
    {
        return m_units[0]->status() == UnitStatus::Fault || m_units[1]->status() == UnitStatus::Fault;
    }

    void Runtime::reportFailure() const
    {
        for (uint32_t index = 0; index < 2; ++index)
            if (m_units[index]->status() == UnitStatus::Fault)
            {
                const auto &failure = m_units[index]->failure();
                throw std::runtime_error("VU" + std::to_string(index) + " execution failed at PC " +
                                         std::to_string(failure.pc) + ": " + failure.reason.data());
            }
    }

    void Runtime::run(bool vu1, uint32_t cycles)
    {
        auto &target = unit(vu1);
        while (cycles)
        {
            if (vu1 && unit(false).status() == UnitStatus::Running)
            {
                // TEMP DIAGNOSTIC (boot-stall triage): VU1 stalls entirely while VU0 runs.
                static uint64_t s_vu1StarvedByVu0 = 0u;
                if ((++s_vu1StarvedByVu0 % 8192u) == 0u)
                    std::cerr << "[VU-run] vu1 starved by vu0 pc=0x" << std::hex << unit(false).state().pc
                              << std::dec << std::endl;
                m_advanceClock(1);
                reportFailure();
                --cycles;
                if (target.status() != UnitStatus::Running)
                    return;
                continue;
            }

            const auto exit = target.advance(cycles);
            if (!vu1)
                m_contextDirty = true;

            if (exit.cycles)
                m_advanceClock(static_cast<uint32_t>(exit.cycles));

            reportFailure();
            cycles -= static_cast<uint32_t>(exit.cycles);
            if (exit.reason != ExitReason::Event || !exit.cycles)
                return;
        }
    }

    void Runtime::wait(bool vu1)
    {
        // Bounded (see kWaitSpinBudgetVu1): a microprogram that never reaches its
        // end must not deadlock the EE thread. Report progress a few times, then
        // reset the unit and let the guest continue.
        const uint64_t spinBudget = vu1 ? kWaitSpinBudgetVu1 : kWaitSpinBudgetVu0;
        const uint8_t *code = vu1 ? m_memory.getVU1Code() : m_memory.getVU0Code();
        const uint32_t codeSize = vu1 ? PS2_VU1_CODE_SIZE : PS2_VU0_CODE_SIZE;
        auto reportStuck = [&](uint64_t spins, const char *label)
        {
            const VU1State &vuState = unit(vu1).state();
            uint32_t word0 = 0u;
            uint32_t word1 = 0u;
            if (code != nullptr && vuState.pc + 8u <= codeSize)
            {
                std::memcpy(&word0, code + vuState.pc, sizeof(word0));
                std::memcpy(&word1, code + vuState.pc + 4u, sizeof(word1));
            }
            std::cerr << label << " vu" << (vu1 ? 1 : 0) << " spins=" << spins
                      << " (~" << (spins * 64ull) << " cycles)"
                      << " pc=0x" << std::hex << vuState.pc
                      << " w0=0x" << word0 << " w1=0x" << word1 << std::dec
                      << " ebit=" << (vuState.ebit ? 1 : 0)
                      << " top=0x" << std::hex << vuState.top << " itop=0x" << vuState.itop << std::dec
                      << " otherStatus=" << static_cast<int>(unit(!vu1).status()) << std::endl;
        };

        uint64_t spins = 0u;
        uint32_t reports = 0u;
        while (unit(vu1).status() == UnitStatus::Running)
        {
            run(vu1, 64);
            ++spins;
            if ((spins % kWaitWarnEverySpins) == 0u && reports < kWaitMaxWarnReports)
            {
                ++reports;
                reportStuck(spins, "[VU-wait]");
            }
            if (spins >= spinBudget)
            {
                reportStuck(spins, "[VU-stall]");
                std::cerr << "[VU-stall] vu" << (vu1 ? 1 : 0)
                          << " microprogram never ended; resetting the unit so the EE thread can continue"
                          << std::endl;
                unit(vu1).reset();
                break;
            }
        }
        if (spins != 0u) // TEMP DIAGNOSTIC (boot-stall triage): a wait that had to spin.
            std::cerr << "[VU-wait] vu" << (vu1 ? 1 : 0) << " returned after spins=" << spins
                      << " status=" << static_cast<int>(unit(vu1).status()) << std::endl;
        reportFailure();
    }

    void Runtime::start0(R5900Context &context, uint32_t pc)
    {
        m_accessContext = &context;
        const bool running = unit(false).status() == UnitStatus::Running;
        wait(false);
        if (running)
            exportContext(unit(false).state(), context);

        importContext(context, unit(false).state());
        if (!unit(false).start(pc, 0, context.vu0_itop, StartSource::Ee))
            reportFailure();

        exportContext(unit(false).state(), context);
        m_accessContext = nullptr;
    }

    void Runtime::startVif0(R5900Context &context, uint32_t pc, uint32_t itop, bool resume)
    {
        m_accessContext = &context;
        const bool running = unit(false).status() == UnitStatus::Running;

        wait(false);

        if (running)
            exportContext(unit(false).state(), context);

        importContext(context, unit(false).state());
        const bool started = resume ? unit(false).resume(0, itop) : unit(false).start(pc, 0, itop);
        m_accessContext = nullptr;
        if (!started)
        {
            reportFailure();
            throw std::runtime_error("VIF startup requested while VU0 is stopped");
        }
        exportContext(unit(false).state(), context);
    }

    void Runtime::start1(uint32_t pc, uint32_t top, uint32_t itop, bool resume)
    {
        // TEMP DIAGNOSTIC (boot-stall triage).
        std::cerr << "[VU1-mscal] pc=0x" << std::hex << pc << " top=0x" << top << " itop=0x" << itop << std::dec
                  << " resume=" << (resume ? 1 : 0)
                  << " running=" << (unit(true).status() == UnitStatus::Running ? 1 : 0) << std::endl;
        wait(true);
        const bool started = resume ? unit(true).resume(top, itop) : unit(true).start(pc, top, itop);
        if (!started)
        {
            reportFailure();
            throw std::runtime_error("VIF startup requested while VU1 is stopped");
        }
        std::cerr << "[VU1-mscal-done] status=" << static_cast<int>(unit(true).status()) << std::endl;
    }

    void Runtime::flush0(R5900Context &context)
    {
        m_accessContext = &context;
        wait(false);
        m_accessContext = nullptr;
    }

    void Runtime::flush1(bool gif)
    {
        // TEMP DIAGNOSTIC (boot-stall triage).
        std::cerr << "[VU1-flush] gif=" << (gif ? 1 : 0)
                  << " status=" << static_cast<int>(unit(true).status())
                  << " transferring=" << (unit(true).transferring() ? 1 : 0) << std::endl;
        wait(true);
        // TEMP DIAGNOSTIC (boot-stall triage).
        std::cerr << "[VU1-flush] wait returned status=" << static_cast<int>(unit(true).status()) << std::endl;
        if (gif)
        {
            uint64_t transferSpins = 0u;
            uint32_t reports = 0u;
            while (unit(true).transferring())
            {
                run(true, 2);
                ++transferSpins;
                if ((transferSpins % kWaitWarnEverySpins) == 0u && reports < kTransferMaxWarnReports)
                {
                    ++reports;
                    std::cerr << "[VU1-flush] still transferring, spins=" << transferSpins << std::endl;
                }
                if (transferSpins >= kTransferSpinBudget)
                {
                    std::cerr << "[VU1-flush] XGKICK transfer never completed after " << transferSpins
                              << " spins; abandoning the wait so the EE thread can continue" << std::endl;
                    break;
                }
            }
        }
    }

    void Runtime::advance(uint32_t cycles, R5900Context &context) noexcept
    {
        auto &activeContext = m_accessContext ? *m_accessContext : context;
        m_cycle += cycles;

        auto &vu0 = unit(false);
        const bool updateContext = vu0.pending() || m_contextDirty;
        if (updateContext && !m_contextDirty && vu0.status() != UnitStatus::Running)
            importContext(activeContext, vu0.state());

        advanceUnit(false, m_cycle);
        advanceUnit(true, m_cycle);

        if (updateContext)
            exportContext(vu0.state(), activeContext);

        m_contextDirty = false;
        activeContext.vu0_vpu_stat = (activeContext.vu0_vpu_stat & ~0x0701u) |
                                     (vu0.status() == UnitStatus::Running ? 1u : 0u) |
                                     (unit(true).status() == UnitStatus::Running ? 0x100u : 0u) |
                                     (unit(true).state().stoppedByD ? 0x200u : 0u) | (unit(true).state().stoppedByT ? 0x400u : 0u);
    }

    void Runtime::advanceUnit(bool vu1, uint64_t cycle) noexcept
    {
        auto &target = unit(vu1);
        if (target.state().cycles >= cycle)
            return;

        if (!target.pending() && !target.transferring())
        {
            if (target.status() != UnitStatus::Fault)
                target.state().cycles = cycle;
            return;
        }

        // A running unit can only be one time slice behind this clock, so a far
        // larger gap means its counter was desynchronised (a mid-run reset used
        // to zero it). Draining that gap would run the microprogram for a huge
        // number of cycles -- and an unfinished microprogram never ends -- so
        // snap the counter forward and report it instead.
        constexpr uint64_t kMaxCatchUpCycles = 1ull << 20;
        const uint64_t gap = cycle - target.state().cycles;
        if (gap > kMaxCatchUpCycles)
        {
            static uint64_t s_resyncReports = 0u;
            if (s_resyncReports < 8u)
            {
                ++s_resyncReports;
                std::cerr << "[VU-clock] vu" << (vu1 ? 1 : 0) << " unit cycles=" << target.state().cycles
                          << " clock=" << cycle << " gap=" << gap << "; resynchronising" << std::endl;
            }
            target.state().cycles = cycle;
            return;
        }

        while (target.state().cycles < cycle)
        {
            const auto remaining = static_cast<uint32_t>(std::min<uint64_t>(cycle - target.state().cycles, UINT32_MAX));
            const auto exit = target.advance(remaining);
            if (!exit.cycles || exit.reason == ExitReason::Fault)
                break;
        }
    }

    void Runtime::beforeAccess(R5900Context &context, uint32_t instruction)
    {
        const auto opcode = instruction >> 26;
        if (opcode == 0x36 || opcode == 0x3e)
        {
            const auto target = (instruction >> 16) & 31;
            if (unit(false).pending() || m_contextDirty)
                context.vu0_vf[target] = _mm_loadu_ps(unit(false).state().vf[target]);
            reportFailure();
            return;
        }

        const auto format = (instruction >> 21) & 31;
        const auto reg = (instruction >> 11) & 31;
        auto &vu0 = unit(false);
        if (!vu0.pending() && !m_contextDirty)
        {
            if (format == 5 || format == 6)
                vu0.state().mBit = false;
            reportFailure();
            return;
        }

        m_accessContext = &context;
        if (vu0.status() != UnitStatus::Running)
            importContext(context, vu0.state());

        const bool interlocked = (instruction & 1) != 0;
        const bool writeTransfer = format == 5 || format == 6;
        if (interlocked && writeTransfer)
        {
            while (vu0.status() == UnitStatus::Running && !vu0.state().mBit)
                run(false, 1);
        }
        else if (format >= 16 || interlocked)
            wait(false);

        if (writeTransfer)
            vu0.state().mBit = false;

        const auto &ready = vu0.readiness();
        uint32_t delay = 0;
        if ((format == 1 || format == 5) && interlocked)
            for (uint32_t lane = 0; lane < 4; ++lane)
                delay = std::max(delay, uint32_t(ready.registers[reg * 4 + lane]));
        else if ((format == 2 || format == 6) && reg < 16 && interlocked)
            delay = ready.registers[kViReadyBase + reg];
        else if (format >= 16)
        {
            const uint32_t op = instruction & 63;
            if (op != 0x38 && op != 0x39)
            {
                const auto special = ((instruction >> 4) & 0x7c) | (instruction & 3);
                const bool lower = op >= 0x30 && (op < 0x3c || special >= 0x30);
                const auto decoded = decodeInstructionPair(lower ? 0x80000000u | (instruction & 0x1ffffff) : 0x8000033cu,
                                                           lower ? 0x2ffu : instruction & 0x1ffffff, false);
                for (uint32_t index = 0; index < decoded.readDependencyCount; ++index)
                    delay = std::max(delay, uint32_t(ready.registers[decoded.readDependencies[index]]));
                for (const auto &usage : {decoded.upperUsage, decoded.lowerUsage})
                {
                    if (usage.vfWrite.reg)
                        for (uint32_t lane = 0; lane < 4; ++lane)
                            delay = std::max(delay, uint32_t(ready.registers[usage.vfWrite.reg * 4 + lane]));
                    for (uint32_t reg = 1; reg < 16; ++reg)
                        if (usage.viWrite & (1u << reg))
                            delay = std::max(delay, uint32_t(ready.registers[kViReadyBase + reg]));
                }
                if (decoded.lowerUsage.waitQ || decoded.lowerUsage.pipeline == PipelineFdiv)
                    delay = std::max(delay, uint32_t(ready.q));
            }
        }

        if (delay)
            run(false, delay);

        exportContext(vu0.state(), context);
        m_accessContext = nullptr;
    }

    void Runtime::afterAccess(R5900Context &context, uint32_t instruction)
    {
        const auto opcode = instruction >> 26;
        if (opcode == 0x36)
        {
            const auto target = (instruction >> 16) & 31;
            if (target)
                _mm_storeu_ps(unit(false).state().vf[target], context.vu0_vf[target]);
            else
                context.vu0_vf[0] = _mm_set_ps(1, 0, 0, 0);
            return;
        }

        if (opcode == 0x3e)
            return;
        if (unit(false).pending())
            importContext(context, unit(false).state());

        const auto format = (instruction >> 21) & 31;
        const auto reg = (instruction >> 11) & 31;
        if (format == 6 && reg == 28)
        {
            const auto value = static_cast<uint32_t>(_mm_cvtsi128_si32(context.r[(instruction >> 16) & 31]));
            for (uint32_t index = 0; index < 2; ++index)
            {
                auto &target = *m_units[index];
                const auto bits = value >> (index * 8);
                if (bits & 2)
                    target.reset();
                else if (bits & 1)
                    target.forceBreak();
                target.state().dBitEnabled = (bits & 4) != 0;
                target.state().tBitEnabled = (bits & 8) != 0;
                if (bits & 2)
                    target.state().cycles = m_cycle;
            }
            if (value & 2)
                exportContext(unit(false).state(), context);
        }

        if (format == 6 && reg == 31)
        {
            m_accessContext = &context;
            wait(true);
            const auto &state = unit(true).state();
            unit(true).start((context.vu0_cmsar1 & 0x7ff) * 8, state.top, state.itop, StartSource::Ee);
            m_accessContext = nullptr;
        }
    }

    bool Runtime::submitGif(void *context, const uint8_t *data, uint32_t size) noexcept
    {
        try
        {
            static_cast<Runtime *>(context)->m_memory.submitGifPacket(GifPathId::Path1, data, size);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool Runtime::readShared(void *context, uint64_t cycle, uint32_t offset, uint32_t *words) noexcept
    {
        auto &runtime = *static_cast<Runtime *>(context);
        runtime.advanceUnit(true, cycle);
        if (runtime.failed())
            return false;

        auto &state = runtime.unit(true).state();
        const auto reg = (offset & 0x3ff) >> 4;
        std::fill_n(words, 4, 0u);

        if (reg < 32)
            std::memcpy(words, state.vf[reg], 16);
        else if (reg < 48)
            words[0] = static_cast<uint16_t>(state.vi[reg - 32]);
        else
            switch (reg - 32)
            {
            case 16:
                words[0] = state.status;
                break;
            case 17:
                words[0] = state.mac;
                break;
            case 18:
                words[0] = state.clip;
                break;
            case 20:
                words[0] = state.r;
                break;
            case 21:
                words[0] = std::bit_cast<uint32_t>(state.i);
                break;
            case 22:
                words[0] = std::bit_cast<uint32_t>(state.q);
                break;
            case 23:
                words[0] = std::bit_cast<uint32_t>(state.p);
                break;
            case 26:
                words[0] = state.pc / 8;
                break;
            default:
                break;
            }
        return true;
    }

    bool Runtime::writeShared(void *context, uint64_t cycle, uint32_t offset, const uint32_t *words, uint8_t lanes) noexcept
    {
        auto &runtime = *static_cast<Runtime *>(context);
        runtime.advanceUnit(true, cycle);
        if (runtime.failed())
            return false;

        auto &target = runtime.unit(true);
        auto &state = target.state();
        const auto reg = (offset & 0x3ff) >> 4;
        if (reg && reg < 32)
            for (uint32_t lane = 0; lane < 4; ++lane)
                if (lanes & (8u >> lane))
                    state.vf[reg][lane] = std::bit_cast<float>(words[lane]);

        if (!(lanes & 8))
            return true;

        if (reg > 32 && reg < 48)
            state.vi[reg - 32] = static_cast<int16_t>(words[0]);
        else
            switch (reg - 32)
            {
            case 16:
                state.status = (state.status & 0x3f) | (words[0] & 0xfc0);
                break;
            case 18:
                state.clip = words[0] & 0xffffff;
                break;
            case 20:
                state.r = 0x3f800000 | (words[0] & 0x7fffff);
                break;
            case 21:
                state.i = std::bit_cast<float>(words[0]);
                break;
            case 22:
                state.q = std::bit_cast<float>(words[0]);
                break;
            case 31:
                return target.start((words[0] & 0x7ff) * 8, state.top, state.itop, StartSource::Ee);
            default:
                break;
            }
        return true;
    }
}
