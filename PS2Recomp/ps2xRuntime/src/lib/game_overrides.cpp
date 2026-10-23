#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}
namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime,
                       const std::string &elfPath,
                       uint32_t entry,
                       uint32_t fileCrc32,
                       bool fileCrcValid)
    {

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}

// ----------------------------------------------------------------------------
// Registered game overrides
// ----------------------------------------------------------------------------

// Tales of Legendia (USA) - SLUS_212.01
// Load CD assets from an extracted directory tree (data\CD next to the ELF)
// instead of sector-reading the ISO image. cdRoot is pinned to the extracted
// tree and cdImage is cleared, so every sceCdSearchFile/sceCdRead resolves
// through registered host files (pseudo-LBNs). Unresolved LBNs report loudly
// on stderr, which makes any hardcoded-LBN access visible immediately.
static void talesOfLegendiaAssetPaths(PS2Runtime & /*runtime */)
{
    PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
    const std::filesystem::path extracted = paths.elfDirectory / "CD";

    std::error_code ec;
    if (std::filesystem::is_directory(extracted, ec) && !ec)
    {
        paths.cdRoot = extracted;
        paths.cdImage.clear();
        PS2Runtime::setIoPaths(paths);
        std::cerr << "[game_overrides] Tales of Legendia (USA): CD assets from extracted tree '"
                  << extracted.string() << "' (ISO image disabled)" << std::endl;
    }
    else
    {
        std::cerr << "[game_overrides] Tales of Legendia (USA): extracted CD tree not found at '"
                  << extracted.string() << "'; leaving ISO image mount in place." << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Tales of Legendia (USA)", "SLUS_212.01", 0, 0, talesOfLegendiaAssetPaths);

// The generated function table has a hole at 0x1AD548: Ghidra never identified
// it as a function (it sits in the 0x38-byte gap between the epilogue at
// 0x1AD534 and FUN_001ad580), so every call to it is reported as a missing
// target and the calling thread goes dormant.  The original is a self-contained
// leaf that the AFS layer calls before opening a file: it scans the 40-entry
// file-slot table at 0x003A6B00 (stride 0x48) and returns the last entry whose
// first byte (the occupancy flag) is zero, or 0 when every slot is taken.
//
//   0x1AD548  lui   $v0, 0x003A
//   0x1AD54C  daddu $a1, $zero, $zero
//   0x1AD550  addiu $v1, $v0, 0x6B00
//   0x1AD554  addiu $a0, $zero, 0x27       ; 40 slots
//   0x1AD558  lb    $v0, 0($v1)
//   0x1AD55C  addiu $a0, $a0, -1
//   0x1AD560  xori  $v0, $v0, 0
//   0x1AD564  movz  $a1, $v1, $v0          ; keep the last free slot
//   0x1AD568  nop
//   0x1AD56C  bgez  $a0, 0x1AD558
//   0x1AD570  addiu $v1, $v1, 0x48         ; (delay) next slot
//   0x1AD574  jr    $ra
//   0x1AD578  daddu $v0, $a1, $zero        ; (delay) slot address or 0
//
// Implementing it on the host keeps the guest's own table authoritative.
static void ps2_1ad548_afsSlotFinder(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    constexpr uint32_t kSlotTableBase = 0x003A6B00u;
    constexpr uint32_t kSlotCount = 40u;
    constexpr uint32_t kSlotStride = 0x48u;

    uint32_t slotAddress = 0u;
    for (uint32_t index = 0u; index < kSlotCount; ++index)
    {
        const uint32_t candidate = kSlotTableBase + index * kSlotStride;
        if (runtime->Load8(rdram, ctx, candidate) == 0u)
        {
            slotAddress = candidate;
        }
    }

    setReturnU32(ctx, slotAddress);
    ctx->pc = GPR_U32(ctx, 31); // jr $ra
}

static void talesOfLegendiaMissingFunctions(PS2Runtime &runtime)
{
    if (!runtime.replaceFunction(0x001AD548u, &ps2_1ad548_afsSlotFinder))
    {
        std::cerr << "[game_overrides] Tales of Legendia (USA): could not install the AFS slot "
                     "finder at 0x1AD548" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Tales of Legendia (USA) - AFS slot finder (0x1AD548)",
                           "SLUS_212.01", 0, 0, talesOfLegendiaMissingFunctions);

// The generated table registers function *starts* only.  A thread can also be
// resumed at an internal label: when a callee returns while an EE transfer is
// pending (a guest syscall asked the scheduler to re-evaluate), the caller's
// dispatchGuestBranch() hands control back with ctx->pc set to the return
// address, which is a mid-function label.  The generated code exposes exactly
// those labels through its `switch (ctx->pc)` prologue, so the same function
// has to be registered at each of them as well.
//
// 0x1AD5A0-0x1AD728 is the libc/AFS helper block the boot loader runs through;
// its resume labels are the ones listed in that function's prologue.
extern void entry_1ad5a0_0x1ad728(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

static void talesOfLegendiaResumeLabels(PS2Runtime &runtime)
{
    static constexpr uint32_t kResumeLabels[] = {
        0x001AD5F0u,
        0x001AD610u,
        0x001AD624u,
        0x001AD644u,
        0x001AD664u,
        0x001AD66Cu,
        0x001AD674u,
        0x001AD67Cu,
        0x001AD688u,
        0x001AD6A0u,
        0x001AD6A8u,
    };

    for (const uint32_t address : kResumeLabels)
    {
        (void)runtime.registerFunction(address, &entry_1ad5a0_0x1ad728);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Tales of Legendia (USA) - AFS helper resume labels",
                           "SLUS_212.01", 0, 0, talesOfLegendiaResumeLabels);
