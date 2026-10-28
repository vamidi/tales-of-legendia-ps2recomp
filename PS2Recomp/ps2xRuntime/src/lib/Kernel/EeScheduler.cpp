#include "runtime/ee_scheduler.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    // TEMP DIAGNOSTIC (session 26): the boot now streams assets correctly and then
    // dies by jumping to pc=0x1 with ra=0x1 from the top of main's stack
    // (sp=0x1ffff60, the frame below crt0's `move sp, v0`).  The recompiler's
    // dispatch trace only stores pcs, so record the entry registers of the last
    // 24 dispatches; the dump below shows the exact call/return that produced
    // the bogus pc.
    constexpr uint32_t kEeRegRingSize = 24u;
    uint32_t g_eeRegRingPc[kEeRegRingSize] = {};
    uint32_t g_eeRegRingRa[kEeRegRingSize] = {};
    uint32_t g_eeRegRingSp[kEeRegRingSize] = {};
    uint32_t g_eeRegRingA0[kEeRegRingSize] = {};
    uint32_t g_eeRegRingV0[kEeRegRingSize] = {};
    int32_t g_eeRegRingTid[kEeRegRingSize] = {};
    uint32_t g_eeRegRingPos = 0u;

    void recordEeRegRing(const R5900Context &ctx, int32_t tid)
    {
        const uint32_t idx = g_eeRegRingPos % kEeRegRingSize;
        g_eeRegRingPc[idx] = ctx.pc;
        g_eeRegRingRa[idx] = getRegU32(&ctx, 31);
        g_eeRegRingSp[idx] = getRegU32(&ctx, 29);
        g_eeRegRingA0[idx] = getRegU32(&ctx, 4);
        g_eeRegRingV0[idx] = getRegU32(&ctx, 2);
        g_eeRegRingTid[idx] = tid;
        ++g_eeRegRingPos;
    }

    void dumpEeRegRing(std::ostream &os)
    {
        const uint32_t count = std::min<uint32_t>(g_eeRegRingPos, kEeRegRingSize);
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (g_eeRegRingPos - count + i) % kEeRegRingSize;
            os << " [reg" << i << " tid=" << g_eeRegRingTid[idx]
               << " pc=0x" << std::hex << g_eeRegRingPc[idx]
               << " ra=0x" << g_eeRegRingRa[idx]
               << " sp=0x" << g_eeRegRingSp[idx]
               << " a0=0x" << g_eeRegRingA0[idx]
               << " v0=0x" << g_eeRegRingV0[idx] << std::dec << "]";
        }
    }

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    // TEMP DIAGNOSTIC (session 23): why is vblank still ~27 Hz after the
    // checkpointDue host gate landed? Count which gate preempts, and measure
    // VBlankStart delivery lag against its own published host deadline.
    std::atomic<uint64_t> s_hostGateFires{0};
    std::atomic<uint64_t> s_cycleGateFires{0};
    uint64_t s_vbDelivered = 0u;
    uint64_t s_vbLagSumUs = 0u;
    int64_t s_vbLagMaxUs = 0;
    uint32_t s_lastDispPc = 0u;
    uint32_t s_lastDispThread = 0u;
    std::chrono::steady_clock::time_point s_lastDispStart{};
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    // TEMP DIAGNOSTIC (black-content triage): recent guest dispatch PCs, mirroring
    // PS2Runtime's dispatch history which is file-local to ps2_runtime.cpp. Dumped
    // when a thread goes dormant so the execution tail that led to PC=0 is visible.
    constexpr uint32_t kDispatchTraceSize = 24u;
    std::array<uint32_t, kDispatchTraceSize> g_dispatchTrace{};
    uint32_t g_dispatchTraceNext = 0u;
    bool g_dispatchTraceWrapped = false;
    int g_dormantTraceBudget = 3;
    int g_exitTraceBudget = 3;
    int g_startThreadTraceBudget = 20;

    void pushDispatchTrace(uint32_t pc)
    {
        g_dispatchTrace[g_dispatchTraceNext] = pc;
        g_dispatchTraceNext = (g_dispatchTraceNext + 1u) % kDispatchTraceSize;
        if (g_dispatchTraceNext == 0u)
        {
            g_dispatchTraceWrapped = true;
        }
    }

    std::string formatDispatchTrace()
    {
        const uint32_t count = g_dispatchTraceWrapped ? kDispatchTraceSize : g_dispatchTraceNext;
        if (count == 0u)
        {
            return "(empty)";
        }
        std::ostringstream oss;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (g_dispatchTraceNext + kDispatchTraceSize - count + i) % kDispatchTraceSize;
            if (i != 0u)
            {
                oss << " -> ";
            }
            oss << "0x" << std::hex << g_dispatchTrace[idx];
        }
        return oss.str();
    }

    // TEMP DIAGNOSTIC: dump a small region of guest RAM as hex words.
    std::string formatHexWords(uint8_t *rdram, uint32_t base, uint32_t words)
    {
        if (!rdram)
        {
            return "(no-ram)";
        }
        std::ostringstream oss;
        for (uint32_t i = 0u; i < words; ++i)
        {
            uint32_t value = 0u;
            std::memcpy(&value, rdram + (base + i * 4u), sizeof(value));
            if (i != 0u)
            {
                oss << " ";
            }
            oss << std::hex << value;
        }
        return oss.str();
    }

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    // VBlank is paced on wall-clock time (see the schedule sites below), so these
    // cycle equivalents are kept only as documentation of the intended cadence.
    [[maybe_unused]] constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    [[maybe_unused]] constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    // PS2X_DET_TICK=1: measure how much guest work one frame consumes, so the
    // boot race can be quantified instead of guessed at.  It reports the
    // cycles/frame average and its min/max spread.
    //
    // It deliberately does NOT pace VBlank on the cycle counter.  Tried in
    // session 26: m_eeCycle is an *accountancy* counter, not a clock.  It is
    // charged from per-dispatch estimates while the guest is busy and from
    // wall-clock sleep time while the guest is idle, and the two run at rates
    // that differ by an order of magnitude (a CDV poll spin accrues ~60k cyc/s,
    // an idle sleep accrues ~295M cyc/s).  A cycle-paced VBlank therefore
    // collapsed delivery from 60 Hz to ~3.5 Hz - one 8.4 s dispatch - while
    // still ending in the same fiber death.  A virtual clock needs accountCycles
    // to be time-proportional first; until then this stays a measurement.
    const bool s_detTick = []()
    {
        const char *value = std::getenv("PS2X_DET_TICK");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    // 0 means "calibrate from the wall-clock cadence of the first frames, then
    // freeze".  Pin it with PS2X_DET_VBLANK_CYCLES once a run has reported the
    // calibrated value, so different sessions compare the same cadence.
    uint64_t s_detVBlankCycles = []()
    {
        const char *value = std::getenv("PS2X_DET_VBLANK_CYCLES");
        return (value != nullptr && value[0] != '\0')
                   ? static_cast<uint64_t>(std::strtoull(value, nullptr, 0))
                   : 0ull;
    }();
    uint64_t s_detCalSumCycles = 0u;
    uint32_t s_detCalFrames = 0u;
    uint64_t s_detCalLastCycle = 0u;
    uint64_t s_detRepSumCycles = 0u;
    uint64_t s_detRepMinCycles = 0u;
    uint64_t s_detRepMaxCycles = 0u;
    uint32_t s_detRepFrames = 0u;
    constexpr uint32_t kDetCalibrationFrames = 120u;
    constexpr uint32_t kDetReportFrames = 600u;

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_transferPending = false;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    // VBlank is a real-time source, not a guest-cycle source.  Tales of Legendia's
    // vblank INTC handler (0x166490, registered by FUN_00113b50 via
    // AddIntcHandler(2, ...)) is what wakes the CRI ADXF/Stm server *threads* that
    // drive every stream read - see PS2_PROJECT_STATE.md session 20.  Gating it on
    // elapsed guest cycles stalls boot completely, because the recompiled guest
    // runs orders of magnitude slower than the 299 MHz it expects (~4.9M cycles
    // per frame would be ~168 s of wall time at the observed throughput).
    // deadlineCycle = 0 makes the cycle gate in processDueDeadlines() trivially
    // satisfied so the host deadline alone paces the interrupt.
    scheduleEvent(0u,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    g_dispatchTrace = {};
    g_dispatchTraceNext = 0u;
    g_dispatchTraceWrapped = false;
    g_dormantTraceBudget = 3;
    g_exitTraceBudget = 3;
    g_startThreadTraceBudget = 20;
    std::cerr << "[EE-boot] main thread entry=0x" << std::hex << mainContext.pc
              << " ra=0x" << getRegU32(&mainContext, 31)
              << " sp=0x" << getRegU32(&mainContext, 29)
              << " gp=0x" << getRegU32(&mainContext, 28)
              << " t9=0x" << getRegU32(&mainContext, 25) << std::dec << std::endl;
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    // TEMP PROFILER (session 20): the guest advances at only ~66k EE cyc/s
    // (~4500x slower than real time) and each loop iteration stretches the guest
    // by just ~180 cycles, so ~2.6 ms of host wall time is being spent somewhere
    // other than executing recompiled code.  Attribute the wall clock between the
    // guest call itself and event processing to find out where.
    static const auto s_profStart = std::chrono::steady_clock::now();
    static auto s_profLast = s_profStart;
    static uint64_t s_profGuestUs = 0u;
    static uint64_t s_profEventsUs = 0u;
    static uint64_t s_profIters = 0u;
    // Per-dispatch-PC host cost: which guest function eats the 2.2 ms average.
    static std::map<uint32_t, std::pair<uint64_t, uint64_t>> s_profByPc;

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        m_transferPending = false;
        ++s_profIters;
        const auto s_eventsStart = std::chrono::steady_clock::now();
        processPendingEvents();
        s_profEventsUs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - s_eventsStart).count());
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            // TEMP HLE (Tales of Legendia SLUS_212.01, boot-stall triage):
            // complete the orphaned half of the game's own suspend/resume pair.
            //
            // Thread 1 suspends the staged-init pollers through FUN_00199268()
            // before the sem-60/65 handshake, and the matching entry_00199240()
            // resume is never reached for thread 5. By that point the word the
            // poller spins on (0x3943B8, one of the six gates written by
            // releaseStagedInitGates) is already 1, so the thread is runnable in
            // every respect except the unbalanced suspend. Resuming it once lets
            // its epilogue write the stage-done marker at (ret_1991E8 + 0x43C0).
            {
                static bool s_pollResumeDone = false;
                if (!s_pollResumeDone && m_rdram)
                {
                    uint64_t pollGate = 0;
                    std::memcpy(&pollGate, m_rdram + 0x3943B8u, sizeof(pollGate));
                    if (pollGate != 0)
                    {
                        for (auto &[id, item] : m_threads)
                        {
                            if (item.suspendCount == 0)
                            {
                                continue;
                            }
                            const R5900Context &pollContext = item.activeContext();
                            // Staged-init waits only: the gate-spin cluster
                            // (FUN_00198c60 / FUN_00198cb8) and the stage poll
                            // loop (entry_00198f38 .. 0x199038).
                            const bool stagedWait =
                                (pollContext.pc >= 0x198C60u && pollContext.pc < 0x198D50u) ||
                                (pollContext.pc >= 0x198F38u && pollContext.pc < 0x199038u);
                            if (!stagedWait)
                            {
                                continue;
                            }
                            while (item.suspendCount > 0)
                            {
                                resumeThread(id, false);
                            }
                        }
                        s_pollResumeDone = true;
                    }
                }
            }

            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                {
                    static auto s_idleLast = std::chrono::steady_clock::now();
                    const auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - s_idleLast).count() >= 3)
                    {
                        s_idleLast = now;
                        std::cerr << "[EE-hb] IDLE pending=" << m_pendingInvocations.size()
                                  << " trace=" << formatDispatchTrace() << std::dec << std::endl;
                        // TEMP DIAGNOSTIC (boot-stall triage): with no runnable
                        // thread, dump what every guest thread is blocked on.
                        for (const auto &[id, item] : m_threads)
                        {
                            const R5900Context &threadContext = item.activeContext();
                            std::cerr << "[EE-thread] id=" << id
                                      << " status=" << static_cast<int>(item.status)
                                      << " wait=" << static_cast<int>(item.wait.reason)
                                      << " waitId=" << waitObjectId(item.wait)
                                      << " suspend=" << item.suspendCount
                                      << " wake=" << item.wakeupCount
                                      << " inv=" << item.invocations.size()
                                      << " pc=0x" << std::hex << threadContext.pc
                                      << " ra=0x" << getRegU32(&threadContext, 31)
                                      << " sp=0x" << getRegU32(&threadContext, 29)
                                      << " entry=0x" << item.entry
                                      << std::dec << std::endl;
                        }
                        for (const auto &[id, item] : m_semaphores)
                        {
                            std::cerr << "[EE-sem] id=" << id << " count=" << item.count
                                      << " max=" << item.maxCount
                                      << " waiters=" << item.waiters.size() << std::endl;
                        }
                        for (const auto &[id, item] : m_eventFlags)
                        {
                            std::cerr << "[EE-evf] id=" << id << " bits=0x" << std::hex << item.bits
                                      << " init=0x" << item.initBits << std::dec
                                      << " waiters=" << item.waiters.size() << std::endl;
                        }
                    }
                }
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        // TEMP DIAGNOSTIC (session 26): with the CRI pipeline finally running the
        // boot completes its first asset request and then dies with ra==1 inside
        // the 0x22d0xx CRI SJ helpers.  Log every dispatch into that window with
        // ra/sp/a0 so the frame that clobbers the saved return address is visible.
        {
            static uint32_t s_winLogs = 0u;
            R5900Context &winCtx = running->activeContext();
            if (winCtx.pc >= 0x22c000u && winCtx.pc < 0x22f000u && s_winLogs < 400u)
            {
                ++s_winLogs;
                std::cerr << "[EE-win] pc=0x" << std::hex << winCtx.pc
                          << " ra=0x" << getRegU32(&winCtx, 31)
                          << " sp=0x" << getRegU32(&winCtx, 29)
                          << " a0=0x" << getRegU32(&winCtx, 4)
                          << " a1=0x" << getRegU32(&winCtx, 5)
                          << " v0=0x" << getRegU32(&winCtx, 2)
                          << std::dec << " id=" << running->id << std::endl;
            }
        }
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            completion(running->activeContext());
            if (m_transferPending || m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        {
            static auto s_hbLast = std::chrono::steady_clock::now();
            static uint64_t s_hbIters = 0;
            ++s_hbIters;
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - s_hbLast).count() >= 3)
            {
                s_hbLast = now;
                // TEMP DIAGNOSTIC (session 11): the loader stalls on `ld v0,0(s0)`
                // at 0x198c90 and loops while it is zero; s0 is set by the caller,
                // so dump s0 (and the word it waits on) here.
                const uint32_t hbS0 = getRegU32(&context, 16);
                std::cerr << "[EE-hb] run iters=" << s_hbIters << " id=" << running->id
                          << " kind=" << (running->invocations.empty()
                                              ? -1
                                              : static_cast<int>(running->invocations.back().kind))
                          << " pc=0x" << std::hex << context.pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " sp=0x" << getRegU32(&context, 29)
                          << " gp=0x" << getRegU32(&context, 28)
                          << " t9=0x" << getRegU32(&context, 25)
                          << " inv=" << running->invocations.size()
                          << " s0=0x" << hbS0
                          << " v0=0x" << getRegU32(&context, 2)
                          << " v1=0x" << getRegU32(&context, 3)
                          << " waitAt=" << formatHexWords(m_rdram, hbS0 & 0x1FFFFFF8u, 4u)
                          << " s387658=" << formatHexWords(m_rdram, 0x387658u, 1u)
                          << " s387660=" << formatHexWords(m_rdram, 0x387660u, 1u)
                          << " cmd=" << formatHexWords(m_rdram, 0x3DED98u, 8u)
                          << " h=" << formatHexWords(m_rdram, 0x3DEF40u, 32u)
                          << " pkt=" << formatHexWords(m_rdram, 0x3E0000u, 4u)
                          << " tick=" << std::dec << m_vsyncTick
                          << " cyc=" << m_eeCycle
                          << " trace=" << formatDispatchTrace() << std::dec << std::endl;
                // TEMP DIAGNOSTIC (session 26): enumerate every live guest thread.
                // The CRI servers are woken from vblank only when
                // iReferThreadStatus reports THS_WAIT/THS_WAITSUSPEND (the guest
                // handler 0x199148 tests `status == 4 || status == 0xc`), and the
                // whole CRI chain (pump 0x197940, ADXF exec 0x1973a8, Stm exec
                // 0x19f468) freezes after ~t=15s while thread 1 keeps cycling at
                // 60 Hz.  Without a per-thread status a thread that was never
                // woken is indistinguishable from one that was woken and
                // re-blocked, and `wake` shows deferred wakeups accumulating.
                for (const auto &[tid, item] : m_threads)
                {
                    const char *stName = "?";
                    switch (item.status)
                    {
                    case EeThreadStatus::Running: stName = "Running"; break;
                    case EeThreadStatus::Ready: stName = "Ready"; break;
                    case EeThreadStatus::Waiting: stName = "Waiting"; break;
                    case EeThreadStatus::WaitingSuspended: stName = "WaitSusp"; break;
                    case EeThreadStatus::Suspended: stName = "Suspended"; break;
                    case EeThreadStatus::Dormant: stName = "Dormant"; break;
                    }
                    const char *wName = "?";
                    switch (item.wait.reason)
                    {
                    case EeWaitReason::None: wName = "none"; break;
                    case EeWaitReason::Sleep: wName = "sleep"; break;
                    case EeWaitReason::Semaphore: wName = "sema"; break;
                    case EeWaitReason::EventFlag: wName = "evf"; break;
                    case EeWaitReason::VSync: wName = "vsync"; break;
                    case EeWaitReason::External: wName = "ext"; break;
                    case EeWaitReason::Mpeg: wName = "mpeg"; break;
                    }
                    const R5900Context &thrCtx = item.activeContext();
                    std::cerr << "[EE-thr] id=" << tid
                              << " st=" << static_cast<int>(item.status) << "(" << stName << ")"
                              << " wait=" << wName
                              << " waitId=" << waitObjectId(item.wait)
                              << " susp=" << item.suspendCount
                              << " wake=" << item.wakeupCount
                              << " inv=" << item.invocations.size()
                              << " pc=0x" << std::hex << thrCtx.pc
                              << " ra=0x" << getRegU32(&thrCtx, 31)
                              << " sp=0x" << getRegU32(&thrCtx, 29)
                              << " stack=0x" << item.stack
                              << " sz=0x" << item.stackSize
                              << " entry=0x" << item.entry
                              << " prio=" << std::dec << item.currentPriority
                              << std::endl;
                }
                {
                    const auto profNow = std::chrono::steady_clock::now();
                    const auto spanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            profNow - s_profLast).count();
                    std::cerr << "[EE-prof] span_ms=" << spanMs
                              << " t_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 profNow - s_profStart).count()
                              << " guest_ms=" << (s_profGuestUs / 1000u)
                              << " events_ms=" << (s_profEventsUs / 1000u)
                              << " iters=" << s_profIters
                              << " cyc=" << m_eeCycle << std::endl;
                    std::vector<std::pair<uint32_t, std::pair<uint64_t, uint64_t>>> ranked(
                        s_profByPc.begin(), s_profByPc.end());
                    std::sort(ranked.begin(), ranked.end(),
                              [](const auto &left, const auto &right)
                              { return left.second.second > right.second.second; });
                    std::cerr << "[EE-profp] top host cost:";
                    for (size_t i = 0u; i < ranked.size() && i < 10u; ++i)
                    {
                        std::cerr << " 0x" << std::hex << ranked[i].first << std::dec
                                  << " n=" << ranked[i].second.first
                                  << " ms=" << (ranked[i].second.second / 1000u)
                                  << " avg_us=" << (ranked[i].second.first
                                                        ? ranked[i].second.second / ranked[i].second.first
                                                        : 0u);
                    }
                    std::cerr << std::endl;
                    s_profLast = profNow;
                    s_profGuestUs = 0u;
                    s_profEventsUs = 0u;
                    s_profIters = 0u;
                    s_profByPc.clear();
                }
                // TEMP DIAGNOSTIC (session 16, idle-thread triage): while the guest
                // sits in its own idle loop (0x1B6B80), show what every other thread
                // is blocked on and which semaphores are empty.
                if (context.pc == 0x1B6B80u)
                {
                    for (const auto &[threadId, item] : m_threads)
                    {
                        const R5900Context &threadContext = item.activeContext();
                        std::cerr << "[DIAG-thread] id=" << threadId
                                  << " status=" << static_cast<int>(item.status)
                                  << " wait=" << static_cast<int>(item.wait.reason)
                                  << " waitId=" << waitObjectId(item.wait)
                                  << " suspend=" << item.suspendCount
                                  << " wake=" << item.wakeupCount
                                  << " inv=" << item.invocations.size()
                                  << " pc=0x" << std::hex << threadContext.pc
                                  << " ra=0x" << getRegU32(&threadContext, 31)
                                  << " entry=0x" << item.entry << std::dec << std::endl;
                    }
                    for (const auto &[semId, item] : m_semaphores)
                    {
                        std::cerr << "[DIAG-sem] id=" << semId << " count=" << item.count
                                  << " max=" << item.maxCount
                                  << " waiters=" << item.waiters.size() << std::endl;
                    }
                }
            }
        }
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    completed.onComplete(completed.context, running->activeContext());
                }
                continue;
            }
            if (g_dormantTraceBudget > 0)
            {
                --g_dormantTraceBudget;
                std::cerr << "[EE-dormant] thread=" << running->id
                          << " pc=0x" << std::hex << context.pc
                          << " ra=0x" << getRegU32(&context, 31)
                          << " sp=0x" << getRegU32(&context, 29)
                          << " gp=0x" << getRegU32(&context, 28)
                          << " v0=0x" << getRegU32(&context, 2)
                          << " t9=0x" << getRegU32(&context, 25)
                          << " trace=" << formatDispatchTrace() << std::dec << std::endl;
                // A dispatch that returns to pc=0 never reaches the missing-target
                // branch, so this is the only place the last few dispatches can be
                // reported. The ring's newest entry is the function whose return
                // landed on 0.
                std::cerr << "[EE-regring]";
                dumpEeRegRing(std::cerr);
                std::cerr << std::endl;
            }
            // TEMP DIAGNOSTIC (session 26): FUN_0015231c (0x15231c, 32 dispatches
            // before the exit) is the game's own task/fiber switch: it saves the
            // caller's context into the slot selected by
            //   t0 = table[*(0x3DB1A0) * *(0x3DB1B0) + *(0x3DB1C0)]
            // and then does `jr ra` with `ra = *(t0+0x1a8)`.  If t0 is NULL that
            // reads address 0x1a8 (zeroed kernel RAM) -> 0, i.e. exactly the
            // `pc=0 ra=0` we observe.  Dump the selector globals and the slot.
            {
                static int s_fiberDumps = 3;
                if (s_fiberDumps > 0)
                {
                    --s_fiberDumps;
                    auto ram32 = [this](uint32_t addr) -> uint32_t {
                        uint32_t v = 0u;
                        std::memcpy(&v, m_rdram + (addr & 0x01FFFFFFu), sizeof(v));
                        return v;
                    };
                    const uint32_t f1 = ram32(0x3DB1A0u);
                    const uint32_t f2 = ram32(0x3DB1B0u);
                    const uint32_t f3 = ram32(0x3DB1C0u);
                    // FUN_0015231c computes the slot as
                    //   t0 = *(0x48F590 + *(0x3DB1A0) * *(0x3DB1B0) + *(0x3DB1C0))
                    // i.e. a *byte* offset into the node array (stride 0x1D0, the
                    // ctxPtr field at +0x1C0) followed by a single pointer load.
                    // f2/f3 are the struct stride and the field offset, NOT
                    // selector components - the earlier "idx = f1*f2+f3, table[idx]"
                    // reading multiplied by 4 again and looked at the wrong word.
                    const uint32_t stride = (f2 >= 8u && f2 <= 0x4000u) ? f2 : 0x1D0u;
                    const uint32_t fieldAddr = 0x48F590u + f1 * stride + f3;
                    const uint32_t slot = ram32(fieldAddr);
                    uint32_t liveNodes = 0u;
                    uint32_t lastLive = 0xFFFFFFFFu;
                    for (uint32_t i = 0u; i < 928u; ++i)
                    {
                        if (ram32(0x48F590u + i * stride + f3) != 0u)
                        {
                            ++liveNodes;
                            lastLive = i;
                        }
                    }
                    std::cerr << "[EE-fiber] cnt=" << f1
                              << " stride=" << f2
                              << " off=0x" << std::hex << f3
                              << " field=0x" << fieldAddr
                              << " ctx=0x" << slot
                              << " resume=0x" << ram32(slot + 0x1A8u)
                              << " flag=" << std::dec << ram32(slot + 0x1ACu)
                              << " ownRa=0x" << std::hex << ram32(slot + 0x1B4u)
                              << " cb=0x" << ram32(slot + 0x1B0u)
                              << " live=" << std::dec << liveNodes
                              << " lastLive=";
                    if (lastLive == 0xFFFFFFFFu)
                    {
                        std::cerr << "none";
                    }
                    else
                    {
                        std::cerr << lastLive;
                    }
                    std::cerr << std::endl;
                    if (slot != 0u)
                    {
                        std::cerr << "[EE-fiber-ctx]";
                        for (uint32_t off = 0x1A0u; off <= 0x1C0u; off += 4u)
                        {
                            std::cerr << " +0x" << std::hex << off
                                      << "=0x" << ram32(slot + off);
                        }
                        std::cerr << std::dec << std::endl;
                    }
                    else
                    {
                        // Which nodes actually have a context, and does node 0
                        // (the one this suspend read) have one?
                        std::cerr << "[EE-fiber-live]";
                        uint32_t shown = 0u;
                        for (uint32_t i = 0u; i < 928u && shown < 24u; ++i)
                        {
                            const uint32_t ctxPtr = ram32(0x48F590u + i * stride + f3);
                            if (ctxPtr != 0u)
                            {
                                std::cerr << " [" << i << "]=0x" << std::hex << ctxPtr;
                                ++shown;
                            }
                        }
                        std::cerr << std::dec << std::endl;
                    }
                }
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        recordEeRegRing(context, running->id);


        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                if (g_dormantTraceBudget > 0)
                {
                    --g_dormantTraceBudget;
                    std::cerr << "[EE-dormant-missing] thread=" << running->id
                              << " pc=0x" << std::hex << context.pc
                              << " ra=0x" << getRegU32(&context, 31)
                              << " trace=" << formatDispatchTrace() << std::dec << std::endl;
                    std::cerr << "[EE-regring]";
                    dumpEeRegRing(std::cerr);
                    std::cerr << std::endl;
                }
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);
        pushDispatchTrace(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            const auto s_guestStart = std::chrono::steady_clock::now();
            const uint32_t s_profPc = context.pc;
            s_lastDispPc = s_profPc;
            s_lastDispThread = running->id;
            s_lastDispStart = s_guestStart;
            function(m_rdram, &context, &m_runtime);
            const uint64_t s_guestUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - s_guestStart).count());
            s_profGuestUs += s_guestUs;
            auto &slot = s_profByPc[s_profPc];
            slot.first += 1u;
            slot.second += s_guestUs;
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_insideInterrupt = false;
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        m_transferPending = true;
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        s_cycleGateFires.fetch_add(1, std::memory_order_relaxed);
        m_checkpointPending.store(true, std::memory_order_release);
        m_transferPending = true;
        return true;
    }

    // Session 23: wall-clock-paced events (deadlineCycle 0 - VBlankStart/VBlankEnd)
    // are invisible to the cycle gate above, so without this preemption the
    // interrupt is only noticed when the time slice expires: delivery lag equals
    // the slice length, measured 23 Hz instead of 60 Hz (run132). This is the
    // consumer session 21 promised when it added m_nextDeadlineHostUs.
    // m_nextDeadlineHostUs is published for wall-clock-paced events ONLY (see
    // updateNextDeadline), so whenever this fires processDueDeadlines() has
    // something deliverable - the run127 thrash (gate firing on cycle-gated
    // events' host estimates, guest starved to guest_ms=3/3000) cannot recur.
    const uint64_t preemptHostUs = m_nextDeadlineHostUs.load(std::memory_order_acquire);
    if (preemptHostUs != 0u)
    {
        const uint64_t nowUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                         std::chrono::steady_clock::now().time_since_epoch())
                                                         .count());
        if (nowUs >= preemptHostUs)
        {
            s_hostGateFires.fetch_add(1, std::memory_order_relaxed);
            m_checkpointPending.store(true, std::memory_order_release);
            m_transferPending = true;
            return true;
        }
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        m_transferPending = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    // TEMP DIAGNOSTIC (session 26): the CRI servers must end up at priorities
    // 1/8/16/18/25 with the caller demoted to 0x18 (ADXPS2_Init default config,
    // FUN_001997a8 with a0 == 0).  Log every creation so a wrong priority is
    // visible immediately instead of inferred from the run.
    {
        static uint32_t s_createLogs = 0u;
        if (s_createLogs < 40u)
        {
            ++s_createLogs;
            std::cerr << "[EE-create] id=" << id
                      << " entry=0x" << std::hex << params.entry
                      << std::dec << " prio=" << params.priority
                      << " stack=0x" << std::hex << params.stack
                      << std::dec << " size=" << params.stackSize << std::endl;
        }
    }
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    if (g_startThreadTraceBudget > 0)
    {
        --g_startThreadTraceBudget;
        std::cerr << "[EE-startThread] id=" << id
                  << " entry=0x" << std::hex << target->entry
                  << " arg=0x" << arg << std::dec << std::endl;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    if (g_exitTraceBudget > 0)
    {
        --g_exitTraceBudget;
        R5900Context &exitingContext = exiting->activeContext();
        std::cerr << "[EE-exit] thread=" << id
                  << " delete=" << (deleteThreadRecord ? 1 : 0)
                  << " pc=0x" << std::hex << exitingContext.pc
                  << " ra=0x" << getRegU32(&exitingContext, 31)
                  << " v0=0x" << getRegU32(&exitingContext, 2)
                  << " trace=" << formatDispatchTrace() << std::dec << std::endl;
    }
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    m_transferPending = true;
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
        // TEMP DIAGNOSTIC (session 26): a wake that only bumps the counter means
        // the target is not sleep-waiting (or is inside a suspend), so it never
        // becomes runnable.  The CRI servers are woken from vblank every frame;
        // if their wakes land here the CRI chain can never advance no matter how
        // fast vblank runs.  Capped: this is a per-frame path.
        static uint32_t s_wakeDeferLogs = 0u;
        if (s_wakeDeferLogs < 40u)
        {
            ++s_wakeDeferLogs;
            std::cerr << "[EE-wake-defer] id=" << id
                      << " st=" << static_cast<int>(target->status)
                      << " wait=" << static_cast<int>(target->wait.reason)
                      << " waitId=" << waitObjectId(target->wait)
                      << " wake=" << target->wakeupCount
                      << " from=" << m_currentThreadId << std::endl;
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    // TEMP EXPERIMENT (session 26): the game's own task pump pins the main
    // thread to priority 1 (see [EE-prio] id=1 new=1 old=24 right after
    // ADXPS2_Init's ChangeThreadPriority(id, 0x18)) while the CRI servers it
    // just created sit at 8/16/18/25.  Under a strict-priority EE scheduler a
    // lower-priority thread only runs when the higher-priority one blocks, and
    // thread 1 never blocks (sceSifSetDma completes the IOP reply and queues
    // the DMAC-5 handler for the next dispatch boundary, so every WaitSema
    // finds its semaphore already signalled).  PS2X_MAIN_PRIO=<n> remaps that
    // pin so the servers can preempt, which measures whether the frozen CRI
    // chain (pump 0x197940 / ADXF exec 0x1973a8 / Stm exec 0x19f468) and the
    // asset load advance at all once the servers actually get CPU.
    static const int s_mainPrioOverride = [] {
        int value = 0;
        if (const char *env = std::getenv("PS2X_MAIN_PRIO"))
        {
            value = static_cast<int>(std::strtol(env, nullptr, 10));
        }
        return value;
    }();
    if (s_mainPrioOverride > 0 && id == kMainThreadId && priority == 1)
    {
        priority = s_mainPrioOverride;
    }
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    // TEMP DIAGNOSTIC (session 26): ADXPS2_Init ends with
    // ChangeThreadPriority(GetThreadId(), 0x18); if the caller lands anywhere
    // else the CRI servers never preempt it.
    {
        static uint32_t s_prioLogs = 0u;
        if (s_prioLogs < 40u)
        {
            ++s_prioLogs;
            std::cerr << "[EE-prio] id=" << id << " new=" << priority
                      << " old=" << oldPriority
                      << " st=" << static_cast<int>(target->status)
                      << " cur=" << m_currentThreadId << std::endl;
        }
    }
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    m_transferPending = true;
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    m_transferPending = true;
}

void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    m_transferPending = true;
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    // TEMP DIAGNOSTIC (session 26): both crash runs died with sp=0x1ffff60, i.e.
    // on a stack at the very END of the async-callback pool (region base
    // 0x1F00000, top PS2_RAM_SIZE).  Log the pool hand-outs so exhaustion /
    // leakage is visible instead of inferred.
    {
        static uint32_t s_stackLogs = 0u;
        if (s_stackLogs < 96u)
        {
            ++s_stackLogs;
            std::cerr << "[EE-invstk] tid=" << owner->id << " depth=" << depth
                      << " top=0x" << std::hex << top << std::dec
                      << " n=" << s_stackLogs << std::endl;
        }
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    // TEMP DIAGNOSTIC (session 16, idle-thread triage): who registers what, and
    // whether the recompiled table can actually dispatch the handler.
    std::cerr << "[DIAG-irq-reg] id=" << id << " dmac=" << (dmac ? 1 : 0)
              << " cause=" << cause << " handler=0x" << std::hex << handler
              << " arg=0x" << argument << std::dec
              << " append=" << (append ? 1 : 0)
              << " hasFn=" << (m_runtime.hasFunction(handler) ? 1 : 0) << std::endl;
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    const bool masked = cause < 32u && (mask & (1u << cause)) == 0u;
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    // TEMP DIAGNOSTIC (session 16, idle-thread triage): does a VBLANK/VBLANK-END
    // interrupt actually reach any guest handler? Print on change, the first few,
    // and then sparsely.
    if (cause == 2u || cause == 3u)
    {
        static uint64_t s_irqSeq = 0;
        static size_t s_lastMatching = ~size_t{0};
        static bool s_lastMasked = false;
        const uint64_t seq = ++s_irqSeq;
        const bool changed = matching.size() != s_lastMatching || masked != s_lastMasked;
        s_lastMatching = matching.size();
        s_lastMasked = masked;
        if (changed || seq <= 4u || (seq % 240u) == 0u)
        {
            std::cerr << "[DIAG-irq] seq=" << seq << " cause=" << cause
                      << " masked=" << (masked ? 1 : 0)
                      << " enabledMask=0x" << std::hex << mask << std::dec
                      << " handlers=" << handlers.size()
                      << " matching=" << matching.size();
            for (const EeIrqHandler &handler : matching)
            {
                std::cerr << " [pc=0x" << std::hex << handler.handler << std::dec
                          << " arg=0x" << std::hex << handler.argument << std::dec << "]";
            }
            std::cerr << std::endl;
        }
    }
    if (masked)
    {
        return;
    }
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    m_transferPending = true;
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_transferPending = false;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{} || now < pacingDeadline)
            {
                // Never park the executor here.  VBlank is paced on wall-clock, so
                // a future pacing deadline always exists now that the cycle gate is
                // relaxed for it; blocking would throttle the guest to roughly one
                // time slice per interrupt (measured ~6k EE cyc/s, versus ~59k when
                // the cycle gate accidentally kept this wait short).  The guest runs
                // flat out and the interrupt is taken at the next checkpoint once its
                // deadline passes; blocking lives in waitForEvent(), which only runs
                // when no guest thread is runnable.
                updateNextDeadline();
                return;
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // Session 24: re-arm VBlankStart on the DEADLINE GRID
                // (fired deadline + period) instead of from *now*. Re-arming
                // from now lets the steady-state delivery latency (~2.8 ms of
                // preemption + dispatch pipeline) accumulate into every frame
                // period: 16.67+3.5 = 20.2 ms = 49.5 Hz. On the grid the
                // latency is paid once per frame but never accumulates, so
                // deliveries hold 60 Hz. The frame-drop design is preserved:
                // if the grid point has already passed (guest overrun, boot
                // stall), clamp to now so a guest that cannot keep up simply
                // misses frames instead of collecting a vblank backlog and
                // livelocking inside its own INTC handler.
                const auto vblankBase = std::chrono::steady_clock::now();
                auto nextVBlankStart = scheduled.hostDeadline + kVBlankPeriod;
                if (nextVBlankStart < vblankBase)
                {
                    nextVBlankStart = vblankBase;
                }
                // TEMP DIAGNOSTIC (session 23): delivery lag against the event's
                // own published host deadline - the number that says whether the
                // gate never fires, or fires but delivery is still late.
                {
                    const int64_t lagUs = static_cast<int64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            vblankBase - scheduled.hostDeadline)
                            .count());
                    ++s_vbDelivered;
                    if (lagUs > 0)
                    {
                        s_vbLagSumUs += static_cast<uint64_t>(lagUs);
                    }
                    if (lagUs > s_vbLagMaxUs)
                    {
                        s_vbLagMaxUs = lagUs;
                    }
                    if (lagUs > 5000)
                    {
                        const uint64_t lastDispUs = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                vblankBase - s_lastDispStart)
                                .count());
                        std::cerr << "[DIAG-vblag] tick=" << (m_vsyncTick + 1u)
                                  << " lag_us=" << lagUs
                                  << " lastPc=0x" << std::hex << s_lastDispPc
                                  << " thread=" << std::dec << s_lastDispThread
                                  << " lastDispUs=" << lastDispUs
                                  << " trace=" << formatDispatchTrace() << std::dec << std::endl;
                    }
                }
                // PS2X_DET_TICK: sample the accountancy counter once per frame so
                // the per-frame guest work - and how far it varies - is visible.
                if (s_detTick)
                {
                    const uint64_t deltaCycles = (s_detCalLastCycle != 0u)
                                                     ? (m_eeCycle - s_detCalLastCycle)
                                                     : 0u;
                    s_detCalLastCycle = m_eeCycle;
                    if (deltaCycles != 0u)
                    {
                        if (s_detVBlankCycles == 0u)
                        {
                            s_detCalSumCycles += deltaCycles;
                            ++s_detCalFrames;
                            if (s_detCalFrames >= kDetCalibrationFrames)
                            {
                                s_detVBlankCycles = std::max<uint64_t>(1u, s_detCalSumCycles / s_detCalFrames);
                                s_detCalSumCycles = 0u;
                                s_detCalFrames = 0u;
                                std::cerr << "[EE-dettick] baseline cycles/frame=" << s_detVBlankCycles
                                          << " (accountancy counter, not a clock)" << std::endl;
                            }
                        }
                        else
                        {
                            s_detRepSumCycles += deltaCycles;
                            ++s_detRepFrames;
                            s_detRepMinCycles = (s_detRepMinCycles == 0u)
                                                    ? deltaCycles
                                                    : std::min(s_detRepMinCycles, deltaCycles);
                            s_detRepMaxCycles = std::max(s_detRepMaxCycles, deltaCycles);
                            if (s_detRepFrames >= kDetReportFrames)
                            {
                                const uint64_t avgCycles = s_detRepSumCycles / s_detRepFrames;
                                const uint64_t minCycles = std::max<uint64_t>(1u, s_detRepMinCycles);
                                std::cerr << "[EE-dettick] cycles/frame avg=" << avgCycles
                                          << " min=" << s_detRepMinCycles
                                          << " max=" << s_detRepMaxCycles
                                          << " spread=" << ((s_detRepMaxCycles * 100u) / minCycles) << "%"
                                          << std::endl;
                                s_detRepSumCycles = 0u;
                                s_detRepMinCycles = 0u;
                                s_detRepMaxCycles = 0u;
                                s_detRepFrames = 0u;
                            }
                        }
                    }
                }
                scheduleEvent(0u, vblankBase + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(0u, nextVBlankStart,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }

        // Hand control back to the guest after one batch.  VBlank is paced on
        // wall-clock (deadlineCycle 0), so it is always "cycle-met" and
        // pacingDeadline is therefore always set; looping here would wait for the
        // next vblank forever and the guest would never execute a single
        // instruction (observed as dbgPc stuck at the ELF entry point).
        return;
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        // TEMP DIAGNOSTIC (session 19, first-frame triage): is the VBLANK event
        // advancing at all, and does the guest ever flip a display buffer?
        if (m_vsyncTick <= 3u || (m_vsyncTick % 30u) == 0u)
        {
            std::cerr << "[DIAG-vb] tick=" << m_vsyncTick
                      << " thread=" << m_currentThreadId
                      << " threads=" << m_threads.size()
                      << " flips=" << m_runtime.memory().gs().displayFlipCount[0].load(
                                          std::memory_order_relaxed)
                      << "," << m_runtime.memory().gs().displayFlipCount[1].load(
                                     std::memory_order_relaxed)
                      << " pendingInv=" << m_pendingInvocations.size()
                      << " pc=0x" << std::hex << (currentContext() ? currentContext()->pc : 0u) << std::dec
                      << " presents=" << m_runtime.memory().gs().sdkPresentCount.load(
                                             std::memory_order_relaxed)
                      << " gate=" << s_hostGateFires.load(std::memory_order_relaxed)
                      << " cycGate=" << s_cycleGateFires.load(std::memory_order_relaxed)
                      << " vbLagAvgUs=" << (s_vbDelivered != 0u ? s_vbLagSumUs / s_vbDelivered : 0u)
                      << " vbLagMaxUs=" << s_vbLagMaxUs << std::endl;
        }
        // TEMP DIAGNOSTIC (session 30): sample the framebuffer census several
        // times so an early (still loading) VRAM can be told apart from one that
        // already holds a drawn frame. Budget: PS2_FB_DUMP defaults to 3.
        if (m_vsyncTick == 600u || m_vsyncTick == 1200u || m_vsyncTick == 2400u)
        {
            extern bool Ps2DumpFramebufferBmp(GS &frontend, const GSRegisters &regs, const char *tag);
            const std::string dumpTag = "forced" + std::to_string(m_vsyncTick);
            Ps2DumpFramebufferBmp(m_runtime.gs(), m_runtime.memory().gs(), dumpTag.c_str());
        }
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    // Wake on the nearest *host* deadline.  VBlank is paced purely on wall-clock
    // (deadlineCycle 0), so ordering by cycle deadline alone is not usable here:
    // the vblank entry always wins the cycle minimum, its host deadline is the
    // only real bound, and the cycle-gap charge below collapses to zero - which
    // turns this wait into a busy spin that never lets the guest run (verified:
    // reverting to cycle-ordering pins two cores at 100% with no boot progress).
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    uint64_t nextCycleDeadline = 0u;
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.hostDeadline != right.hostDeadline)
                                               {
                                                   return left.hostDeadline < right.hostDeadline;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        hostDeadline = next->hostDeadline;
        const auto cycleOrMax = [](const ScheduledEvent &item)
        { return item.deadlineCycle == 0u ? std::numeric_limits<uint64_t>::max() : item.deadlineCycle; };
        const auto nextCycle = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                                [&cycleOrMax](const ScheduledEvent &left,
                                                              const ScheduledEvent &right)
                                                {
                                                    const uint64_t l = cycleOrMax(left);
                                                    const uint64_t r = cycleOrMax(right);
                                                    if (l != r)
                                                    {
                                                        return l < r;
                                                    }
                                                    return left.sequence < right.sequence;
                                                });
        if (nextCycle->deadlineCycle != 0u)
        {
            nextCycleDeadline = nextCycle->deadlineCycle;
        }
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            hostDeadline = timerHostDeadline;
        }
        if (nextCycleDeadline == 0u || m_eeCycle + timerCycles < nextCycleDeadline)
        {
            nextCycleDeadline = m_eeCycle + timerCycles;
        }
    }

    const auto sleepStart = std::chrono::steady_clock::now();
    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        // Idle time is real time: charge the EE for the interval actually slept,
        // capped at the nearest cycle-gated deadline so an idle wait for a
        // wall-clock-paced event can never run the EE clock past an event that
        // still has to be seen as cycle-due.  The cap matters: with no cap the
        // charge inflated m_eeCycle to 1.35e9 while the guest executed nothing,
        // racing the EE timers and regressing boot (session 20, run127).
        const auto sleptUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - sleepStart).count();
        uint64_t elapsed = sleptUs > 0
                               ? microsecondsToEeCycles(static_cast<uint64_t>(sleptUs))
                               : 0u;
        if (nextCycleDeadline != 0u && nextCycleDeadline > m_eeCycle)
        {
            const uint64_t gap = nextCycleDeadline - m_eeCycle;
            if (elapsed > gap)
            {
                elapsed = gap;
            }
        }
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        m_nextDeadlineHostUs.store(0u, std::memory_order_release);
        return;
    }

    // Nearest host (wall-clock) deadline, published separately so checkpointDue()
    // can preempt on it: a wall-clock-paced event such as VBlank is never
    // cycle-due, and without this the interrupt is only noticed when the current
    // time slice expires, collapsing the vblank rate to the guest's throughput.
    const auto hostUs = [](const ScheduledEvent &item)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         item.hostDeadline.time_since_epoch())
                                         .count());
    };
    // Session 23: publish the nearest host deadline for WALL-CLOCK-PACED events
    // only (deadlineCycle 0: VBlankStart/VBlankEnd). Cycle-gated events already
    // have m_nextDeadlineCycle below; including their host estimates here let
    // checkpointDue() preempt for events processDueDeadlines() will not deliver
    // (not cycle-due yet), re-arming the gate every checkpoint and starving the
    // guest (session 21, run127: guest_ms=3/3000, tick collapsed to 9 Hz).
    const auto hostOrMax = [&hostUs](const ScheduledEvent &item)
    { return item.deadlineCycle == 0u ? hostUs(item) : std::numeric_limits<uint64_t>::max(); };
    const auto nextHost = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [&hostOrMax](const ScheduledEvent &left,
                                                        const ScheduledEvent &right)
                                           {
                                               const uint64_t l = hostOrMax(left);
                                               const uint64_t r = hostOrMax(right);
                                               if (l != r)
                                               {
                                                   return l < r;
                                               }
                                               return left.sequence < right.sequence;
                                           });
    const uint64_t wallHostUs = hostOrMax(*nextHost);
    m_nextDeadlineHostUs.store(wallHostUs == std::numeric_limits<uint64_t>::max() ? 0u : wallHostUs,
                               std::memory_order_release);

    // Ignore wall-clock-only events (deadlineCycle 0, e.g. VBlank) when publishing
    // the next *cycle* deadline: they are never cycle-due, and letting them win the
    // min would store 0 and disable the cycle-due preemption hint for every other
    // event (EE timer, alarms) while a vblank is pending.
    const auto cycleOrMax = [](const ScheduledEvent &item)
    { return item.deadlineCycle == 0u ? std::numeric_limits<uint64_t>::max() : item.deadlineCycle; };
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [&cycleOrMax](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         const uint64_t l = cycleOrMax(left);
                                         const uint64_t r = cycleOrMax(right);
                                         if (l != r)
                                         {
                                             return l < r;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
