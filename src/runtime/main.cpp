// TOL_LINK_GENERATED enables the game launcher. Without it, the executable
// reports its version so the project can build without a game dump.

#include <cstdio>

#ifndef TOL_VERSION
#define TOL_VERSION "0.0.0-dev"
#endif

#if defined(TOL_LINK_GENERATED)

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_native_iop.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "screen_size.h"
#include "vu_bounds.h"
#include "../../tools/runtime/render_cadence.h"

#if defined(TOL_HAS_SDLGPU)
#include "gfx/backends/sdlgpu/sdlgpu_backend.h"
#include "gfx/gsdump/gs_backend_trace.h"
#include "ui/ui_overlay.h"
#include "../../tools/runtime/gs_fan_probe.h"
#include "runtime/ps2_pad_host.h"

#include <utility>

namespace
{
    // Publish input after presenting when SDL owns the window.
    bool pumpSdlGpuPresenter(void *userData)
    {
        auto *backend = static_cast<tol::gfx::SdlGpuBackend *>(userData);
        if (!backend->pumpEvents())
        {
            return false;
        }
        uint32_t held = 0u;
        uint32_t pressed = 0u;
        uint32_t sticks = 0u;
        backend->hostPadState(held, pressed, sticks);
        ps2PadPublishHostState(held, pressed, sticks);
        return true;
    }

    // The size the player left the window at, or 4:3 at most of the desktop.
    std::pair<uint32_t, uint32_t> initialWindowSize(const tol::ui::Settings &settings)
    {
        if (settings.windowWidth >= 320 && settings.windowHeight >= 240)
        {
            return {static_cast<uint32_t>(settings.windowWidth), static_cast<uint32_t>(settings.windowHeight)};
        }
        SDL_Rect usable{0, 0, 1280, 960};
        SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
        const uint32_t height = static_cast<uint32_t>(std::max(448.0f, usable.h * 0.8f));
        return {height * 4u / 3u, height};
    }

    std::unique_ptr<tol::ui::Overlay> createMenu(PS2Runtime &runtime, tol::gfx::SdlGpuBackend &backend,
                                                 tol::ui::Settings settings, const std::string &settingsPath,
                                                 bool sound)
    {
        tol::ui::HostServices host;
        host.version = TOL_VERSION;
        // --mute keeps the game silent whatever the menu's volume says.
        host.setVolume = [sound](float volume) { ps2_native_iop::setVolume(sound ? volume : 0.0f); };
        // The sound driver runs in step with the audio device, so stopping
        // the device stops the IOP along with the EE's clock.
        host.setPaused = [&runtime](bool paused) {
            runtime.eeScheduler().setHostPaused(paused);
            ps2_native_iop::setPaused(paused);
        };
        host.setSpeed = [&runtime](double speed) { runtime.eeScheduler().setHostSpeed(speed); };
        host.completedRenders = [] {
            return tol::diagnostics::RenderCadenceProbe::instance().publishedCompletedRoutines();
        };
        host.guestFrame = [] { return ps2PadCurrentGuestFrame(); };
        auto menu = std::make_unique<tol::ui::Overlay>(backend, std::move(settings), settingsPath, std::move(host));
        std::string error;
        if (!menu->initialize(backend.window(), backend.windowTextureFormat(), error))
        {
            std::fprintf(stderr, "[tol] in-game menu unavailable: %s\n", error.c_str());
            return nullptr;
        }
        menu->applySettings();
        backend.setOverlay(menu.get());
        return menu;
    }
}
#endif

#if defined(TOL_LINK_OVERLAYS)
#include "overlay/overlay_manager.h"
#endif

namespace
{
    // Redirect the retail no-op wrappers to the surviving guest printf.
    // This matches the PCSX2 trace patch and preserves guest formatting.
    constexpr uint32_t kDebugPrintfWrapper = 0x00146E20u;
    constexpr uint32_t kBattleLogWrapper = 0x00321620u;

    // The ELF header points at a scratchpad-address table at 0x00100008. For Dragon Quest 8 the first executable
    // instruction (crt0_start) sat 0x84 further on, and the reference build started there. Tales of Legendia's
    // crt0 IS 0x00100008 - the same address the corpus table is based at - and 0x0010008C has no compiled
    // function, so starting there jumps to an unmapped target and the guest never runs. Override with --entry=.
    constexpr uint32_t kDefaultEntryPoint = 0x00100008u;
}

// BOOT TRACE (TOL_TRACE_BOOT=1): crt0 returns to ra=0 and the EE thread ends, so log which of its calls is the last
// one entered. Only ctx->pc is printed, so this does not depend on the R5900Context layout.
namespace
{
    struct BootPoint
    {
        uint32_t pc;
        const char *name;
    };
    const BootPoint kBootPoints[] = {
        {0x00100008u, "crt0 entry"},
        {0x0011EA58u, "FUN_0011ea58 libc-init"},
        {0x0011E900u, "FUN_0011e900 syscall 0x83"},
        {0x0011E808u, "FUN_0011e808"},
        {0x0011E948u, "FUN_0011e948"},
        {0x0011EF38u, "FUN_0011ef38"},
        {0x0011F0B0u, "FUN_0011f0b0"},
        {0x0011F2F0u, "FUN_0011f2f0"},
        {0x001154B8u, "FUN_001154b8"},
        {0x0011EB80u, "FUN_0011eb80"},
        {0x0011E070u, "FUN_0011e070"},
        {0x00120770u, "FUN_00120770 tail"},
        {0x00165AD0u, "FUN_00165ad0 game-startup"},
        {0x001072B8u, "FUN_001072b8 game-run"},
    };
    constexpr int kBootPointCount = static_cast<int>(sizeof(kBootPoints) / sizeof(kBootPoints[0]));
    PS2Runtime::RecompiledFunction gBootOriginal[kBootPointCount] = {};
    uint32_t gBootPc[kBootPointCount] = {};
    const char *gBootName[kBootPointCount] = {};
    int gBootCount = 0;

    void bootTraceCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int idx = -1;
        for (int i = 0; i < gBootCount; ++i)
        {
            if (gBootPc[i] == ctx->pc)
                idx = i;
        }
        if (idx < 0 || gBootOriginal[idx] == nullptr)
        {
            std::fprintf(stderr, "[boot] probe dispatch with unknown pc=0x%08X\n", ctx->pc);
            return;
        }
        std::fprintf(stderr, "[boot] enter %s pc=0x%08X ra=0x%08X sp=0x%08X\n", gBootName[idx], ctx->pc,
                     getRegU32(ctx, 31), getRegU32(ctx, 29));
        gBootOriginal[idx](rdram, ctx, runtime);
        std::fprintf(stderr, "[boot] exit  %s pc=0x%08X ra=0x%08X sp=0x%08X\n", gBootName[idx], ctx->pc,
                     getRegU32(ctx, 31), getRegU32(ctx, 29));
    }
}
int main(int argc, char **argv)
{
    std::string elfPath;
    std::string isoPath;
    std::string cdRoot;
    uint32_t entryPoint = kDefaultEntryPoint;
    bool tracePrintf = false;
    bool sound = true;
    std::string rasterBackend = "sw";
    uint32_t resolutionScale = 0u; // 0: the menu's saved setting

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg.rfind("--entry=", 0) == 0)
        {
            entryPoint = static_cast<uint32_t>(std::strtoul(arg.c_str() + 8, nullptr, 0));
        }
        else if (arg == "--trace-printf")
        {
            tracePrintf = true;
        }
        else if (arg == "--mute")
        {
            sound = false;
        }
        else if (arg.rfind("--gs=", 0) == 0)
        {
            rasterBackend = arg.substr(5);
        }
        else if (arg.rfind("--scale=", 0) == 0)
        {
            resolutionScale = static_cast<uint32_t>(std::strtoul(arg.c_str() + 8, nullptr, 10));
        }
        else if (arg.rfind("--iso=", 0) == 0)
        {
            isoPath = arg.substr(6);
        }
        else if (arg.rfind("--cd-root=", 0) == 0)
        {
            cdRoot = arg.substr(10);
        }
        else if (arg == "-h" || arg == "--help")
        {
            std::printf("usage: tol <path-to-SLUS_212.07> [options]\n"
                        "  --entry=0xADDR   override the start PC (default 0x%08X = crt0_start)\n"
                        "  --iso=PATH       disc image for sector reads, including HD6 archives\n"
                        "  --cd-root=DIR    cdrom0: directory (default: the ELF's directory)\n"
                        "  --gs=BACKEND     raster backend: sw (default) or sdlgpu\n"
                        "  --scale=N        internal resolution multiplier for --gs=sdlgpu, for this run\n"
                        "                   (default: the in-game menu's setting)\n"
                        "  --mute           run the sound driver without playing anything\n"
                        "  --trace-printf   restore retail debug logging through the guest formatter\n"
                        "                   Set PS2X_DECI2_LOG_LIMIT=0 to remove the log limit.\n",
                        kDefaultEntryPoint);
            return 0;
        }
        else if (elfPath.empty())
        {
            elfPath = arg;
        }
    }

    if (elfPath.empty())
    {
        std::fprintf(stderr,
                     "TOLRecomp %s\n"
                     "usage: tol <path-to-SLUS_212.07> [--entry=0xADDR]\n",
                     TOL_VERSION);
        return 1;
    }

    std::printf("TOLRecomp %s -- loading %s, entry 0x%08X\n",
               TOL_VERSION, elfPath.c_str(), entryPoint);

    PS2Runtime runtime;

#if defined(TOL_HAS_SDLGPU)
    // Built before initialize(), because that is where the runtime would open
    // its own window: with a presenter installed it opens none, and raylib is
    // never initialised at all.
    std::unique_ptr<tol::gfx::SdlGpuBackend> sdlBackend;
    // Declared after the runtime, so it goes before the backend it draws with.
    std::unique_ptr<tol::ui::Overlay> menu;
    std::string backendError;
    if (rasterBackend == "sdlgpu")
    {
        sdlBackend = tol::gfx::createSdlGpuBackend(backendError);
        if (sdlBackend)
        {
            tol::ui::Settings settings;
            const std::string settingsPath = tol::ui::defaultSettingsPath();
            if (!tol::ui::loadSettings(settingsPath, settings))
            {
                std::fprintf(stderr, "[tol] could not read %s; using default settings\n", settingsPath.c_str());
            }
            // --scale is for this run; the saved setting stays as it was.
            sdlBackend->setResolutionScale(resolutionScale != 0u ? resolutionScale : settings.resolutionScale);
            const auto [width, height] = initialWindowSize(settings);
            std::string windowError;
            // Allow renderer comparisons using the runtime's presenter.
            const char *noWindow = std::getenv("TOL_GFX_NO_WINDOW");
            if (noWindow != nullptr && *noWindow != '\0')
            {
                std::printf("[tol] TOL_GFX_NO_WINDOW: presenting through the runtime\n");
            }
            else if (!sdlBackend->openWindow("TOLRecomp", width, height, windowError))
            {
                std::fprintf(stderr,
                             "[tol] could not open a window (%s); presenting through "
                             "the runtime instead\n",
                             windowError.c_str());
            }
            else
            {
                runtime.setExternalPresenter(&pumpSdlGpuPresenter, sdlBackend.get());
                menu = createMenu(runtime, *sdlBackend, std::move(settings), settingsPath, sound);
            }
        }
    }
    auto *sdlWindowBackend = sdlBackend.get();
#endif

    if (!runtime.initialize("TOLRecomp"))
    {
        std::fprintf(stderr, "[tol] PS2Runtime::initialize() failed\n");
        return 1;
    }

    // The sound drivers from the disc run unmodified on an emulated IOP, and
    // the game's libsdr and Sound Kit reach them over SIF. Audio output starts
    // when sceSifLoadModule loads the first of them.
    ps2_native_iop::setModules(runtime, {"LIBSD.IRX", "SDRDRV.IRX", "MODHSYN.IRX", "MODMIDI.IRX",
                                         "MODMSIN.IRX", "SKSOUND.IRX", "SKHSYNTH.IRX", "SKMIDI.IRX",
                                         "SKMSIN.IRX", "PCMPLAY.IRX"});
    if (!sound)
        ps2_native_iop::setVolume(0.0f);

    // After initialize(), which is what creates GS local memory and hands it to
    // the default software backend. setRasterBackend flushes and carries local
    // memory across, so swapping here is safe.
    if (rasterBackend == "sdlgpu")
    {
#if defined(TOL_HAS_SDLGPU)
        if (!sdlBackend)
        {
            std::fprintf(stderr,
                         "[tol] --gs=sdlgpu: %s\n"
                         "[tol] falling back to the software rasteriser\n",
                         backendError.c_str());
        }
        else
        {
            std::printf("[tol] GS backend: sdlgpu (%s), video %s, internal scale %ux, %s\n",
                        sdlBackend->driverName().c_str(),
                        sdlBackend->videoDriverName().c_str(),
                        sdlBackend->resolutionScale(),
                        sdlBackend->hasWindow()
                            ? "presenting natively (raylib not initialised)"
                            : "presenting through the runtime");
            std::unique_ptr<GSRasterBackend> backend = std::move(sdlBackend);
            if (const char *worker = std::getenv("TOL_GS_WORKER"); !worker || std::string(worker) != "0") {
                // 4 MiB of queued GS work. A deeper queue lets the GPU fall a
                // quarter second behind in heavy scenes, and the window then
                // shows a third of the frames. TOL_GS_QUEUE_MB overrides it.
                backend = std::make_unique<GSThreadedBackend>(std::move(backend), [] {
                    const char *mib = std::getenv("TOL_GS_QUEUE_MB");
                    const long value = mib ? std::atol(mib) : 0;
                    return static_cast<size_t>(value > 0 ? value : 4) * 1024u * 1024u;
                }());
                std::printf("[tol] ordered GS worker enabled\n");
            }
            if (const char *trace = std::getenv("TOL_GS_TRACE")) {
                const char *start = std::getenv("TOL_GS_TRACE_START");
                const char *trigger = std::getenv("TOL_GS_TRACE_TRIGGER");
                backend = std::make_unique<tol::gfx::GsTraceBackend>(
                    std::move(backend), trace, start ? std::strtoul(start, nullptr, 10) : 7550u,
                    4u, trigger ? trigger : "");
            }
            runtime.gs().setRasterBackend(tol::diagnostics::wrapGsFanProbe(
                std::move(backend), runtime.memory().getRDRAM(), PS2_RAM_SIZE));
        }
#else
        std::fprintf(stderr,
                     "[tol] --gs=sdlgpu: this build has no SDL GPU backend "
                     "(SDL3 was not found at configure time)\n");
        return 1;
#endif
    }
    else if (rasterBackend != "sw")
    {
        std::fprintf(stderr, "[tol] --gs=%s: expected sw or sdlgpu\n", rasterBackend.c_str());
        return 1;
    }

    if (!runtime.loadELF(elfPath))
    {
        std::fprintf(stderr, "[tol] PS2Runtime::loadELF(%s) failed\n", elfPath.c_str());
        return 1;
    }

    // Both of these must come after loadELF(): it calls
    // configureIoPathsFromElf(), which overwrites cdRoot with the ELF's own
    // directory. That default is already correct when the ELF is read straight
    // out of an extracted disc tree, so --cd-root only matters when it is not.
    if (!isoPath.empty() || !cdRoot.empty())
    {
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        if (!isoPath.empty())
        {
            paths.cdImage = isoPath;
        }
        if (!cdRoot.empty())
        {
            paths.cdRoot = cdRoot;
        }
        PS2Runtime::setIoPaths(paths);
        std::printf("[tol] cd root = %s\n", paths.cdRoot.string().c_str());
        if (!paths.cdImage.empty())
        {
            std::printf("[tol] cd image = %s\n", paths.cdImage.string().c_str());
        }
    }

    // Keep the runtime arena above the guest allocator's ceiling to prevent overlap.
    {
        // The guest reserves most available RAM, so keep this arena small.
        constexpr uint32_t kRuntimeArenaBytes = 0x00010000u;
        const uint32_t heapTop = runtime.guestHeapHardLimit();
        const uint32_t guestCeiling = heapTop - kRuntimeArenaBytes;
        runtime.setGuestHeapCeiling(guestCeiling);
        runtime.configureGuestHeap(guestCeiling, heapTop);
        std::printf("[tol] guest heap ends 0x%08X, runtime arena 0x%08X-0x%08X\n",
                    guestCeiling, guestCeiling, heapTop);
    }

    // loadELF() just set cpu().pc to the raw ELF header entry (see above);
    // override it with crt0_start (or the caller's --entry) before the
    // scheduler starts executing.
    runtime.cpu().pc = entryPoint;
    // BOOT TRACE (TOL_TRACE_BOOT=1): wrap crt0 and the three calls it makes, so the last one entered names the stall.
    if (std::getenv("TOL_TRACE_BOOT") != nullptr)
    {
        for (int i = 0; i < kBootPointCount; ++i)
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kBootPoints[i].pc);
            if (original == nullptr)
            {
                std::fprintf(stderr, "[boot] 0x%08X (%s) is not in the function table\n", kBootPoints[i].pc,
                             kBootPoints[i].name);
                continue;
            }
            gBootPc[gBootCount] = kBootPoints[i].pc;
            gBootName[gBootCount] = kBootPoints[i].name;
            gBootOriginal[gBootCount] = original;
            runtime.replaceFunction(kBootPoints[i].pc, &bootTraceCall);
            ++gBootCount;
        }
        std::fprintf(stderr, "[boot] tracing %d of %d points\n", gBootCount, kBootPointCount);
    }

    if (!std::getenv("TOL_DISABLE_VU_BOUNDS_FIX") && !tol::installVuBoundsComparisons(runtime))
        std::fprintf(stderr, "[tol] VU bounds comparison repair could not be installed\n");

    if (tracePrintf)
    {
        // TOL redirected its debug-printf stub onto FUN_00119508_0x119508, which lives in TOL's corpus. Tales of
        // Legendia has no mapped equivalent yet, so the flag is accepted and ignored instead of failing the boot.
        std::fprintf(stderr, "[tol] --trace-printf: no guest printf wrapper is mapped for this title yet\n");
    }

#if defined(TOL_LINK_OVERLAYS)
    // Arena addresses must dispatch through the currently resident overlay.
    if (!tol::overlay::install(runtime))
    {
        std::fprintf(stderr, "[tol] overlay manager failed to install\n");
    }
#endif

    std::printf("[tol] starting execution at 0x%08X\n", entryPoint);
#if defined(TOL_HAS_SDLGPU)
    // Both feed the window's menu, so they are installed only with one.
    const bool hasMenu = menu != nullptr;
    if (hasMenu && tol::installScreenSizeProbe(runtime))
        sdlWindowBackend->setGameWidescreenQuery(&tol::gameScreenSize);
#else
    const bool hasMenu = false;
#endif
    [[maybe_unused]] const bool renderCounterReady =
        tol::diagnostics::configureRenderCadenceProbe(runtime, hasMenu);
#if defined(TOL_HAS_SDLGPU)
    if (renderCounterReady && sdlWindowBackend && sdlWindowBackend->hasWindow())
        sdlWindowBackend->setCompletedRenderCounter([] {
            return tol::diagnostics::RenderCadenceProbe::instance().publishedCompletedRoutines();
        });
#endif
    runtime.run();

#if defined(TOL_HAS_SDLGPU)
    if (sdlWindowBackend)
        sdlWindowBackend->setOverlay(nullptr);
    menu.reset();
#endif
    std::printf("[tol] PS2Runtime::run() returned\n");
    return 0;
}

#else // !TOL_LINK_GENERATED

int main(int /*argc*/, char ** /*argv*/)
{
    std::printf("TOLRecomp %s - runtime stub "
               "(configure with -DTOL_LINK_GENERATED=ON to link the recompiled game)\n",
               TOL_VERSION);
    return 0;
}

#endif
