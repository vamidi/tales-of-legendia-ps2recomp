#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/ps2_memory.h"
#include "ps2_log.h"
#include "gs_gl_interop.h"
#include "gs_interface.hpp"
#include "thread_id.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>

namespace
{
    constexpr size_t VramSize = 4 * 1024 * 1024;
    uint64_t word(const void *data)
    {
        uint64_t value;
        std::memcpy(&value, data, 8);
        return value;
    }

    // One-time log (per mode change) of the exact video-mode registers fed to
    // parallel-gs: confirms that the CRT seed / guest pmode-SMODE programming
    // actually reaches scanout, and shows which SMODE1 combo is in use.
    void logPresentationModeOnce(const GSPresentationRequest &request)
    {
        static uint64_t lastPmode = ~0ull;
        static uint64_t lastSmode1 = ~0ull;
        static uint64_t lastSmode2 = ~0ull;
        static uint64_t lastDispfb1 = ~0ull;
        static uint64_t lastDisplay1 = ~0ull;
        if (request.pmode == lastPmode && request.smode1 == lastSmode1 && request.smode2 == lastSmode2 &&
            request.dispfb1 == lastDispfb1 && request.display1 == lastDisplay1)
        {
            return;
        }
        lastPmode = request.pmode;
        lastSmode1 = request.smode1;
        lastSmode2 = request.smode2;
        lastDispfb1 = request.dispfb1;
        lastDisplay1 = request.display1;

        const unsigned lc = unsigned((request.smode1 >> 3) & 127);
        const unsigned cmod = unsigned((request.smode1 >> 13) & 3);
        fprintf(stderr,
                "[GS] present mode: pmode=0x%016llx smode1=0x%016llx (LC=%u CMOD=%u INT=%llu) "
                "smode2=0x%016llx (INT=%llu FFMD=%llu) dispfb1=0x%016llx display1=0x%016llx\n",
                (unsigned long long)request.pmode, (unsigned long long)request.smode1, lc, cmod,
                (unsigned long long)(request.smode1 & 1ull), (unsigned long long)request.smode2,
                (unsigned long long)(request.smode2 & 1ull), (unsigned long long)((request.smode2 >> 1) & 1ull),
                (unsigned long long)request.dispfb1, (unsigned long long)request.display1);
    }

    unsigned bitsPerPixel(unsigned psm)
    {
        switch (psm & 63)
        {
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return 24;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return 16;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            return 8;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return 4;
        default:
            return 32;
        }
    }

    using Reader = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    using Writer = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    struct PixelAccess
    {
        Reader read;
        Writer write;
    };

    PixelAccess pixelAccess(unsigned psm)
    {
        using namespace GSMem;
        switch (psm & 63)
        {
#define PSM(id, name) \
    case GS_PSM_##id: \
        return {Read##name, Write##name}
            PSM(CT32, CT32);
            PSM(CT24, CT24);
            PSM(CT16, CT16);
            PSM(CT16S, CT16S);
            PSM(T8, P8);
            PSM(T8H, P8H);
            PSM(T4, P4);
            PSM(T4HL, P4HL);
            PSM(T4HH, P4HH);
            PSM(Z32, Z32);
            PSM(Z24, Z24);
            PSM(Z16, Z16);
            PSM(Z16S, Z16S);
#undef PSM
        default:
            return {ReadNull, WriteNull};
        }
    }

    class ParallelBackend final : public GSRasterBackend, private ParallelGS::SignalInterface
    {
        // All GSInterface/Device entry points are serialized, including presentation.
        mutable std::recursive_mutex mutex;
        GSGLInterop interop;
        mutable ParallelGS::GSInterface gs;
        GSRegisters &priv;
        struct Lock
        {
            std::lock_guard<std::recursive_mutex> guard;
            explicit Lock(std::recursive_mutex &m) : guard(m) { Util::register_thread_index(0); }
        };
        uint64_t bitblt = 0, trxreg = 0;
        GSTransferSnapshot transfer;
        size_t transferBytes = 0, uploadedBytes = 0;
        std::array<uint8_t, 8> uploadTail{};
        size_t uploadTailSize = 0;
        std::array<uint8_t, 16> fifoTail{};
        size_t fifoOffset = 16;
        mutable uint64_t readbacks = 0;

        void observeRegister(uint8_t addr, uint64_t value)
        {
            if (addr == GS_REG_BITBLTBUF)
                bitblt = value;
            if (addr == GS_REG_TRXREG)
                trxreg = value;
            if (addr == GS_REG_TRXDIR)
            {
                transfer = {};
                transfer.direction = value & 3;
                transfer.totalPixels = uint32_t(trxreg & 0xfff) * uint32_t((trxreg >> 32) & 0xfff);
                unsigned psm = unsigned(bitblt >> (transfer.direction == 1 ? 24 : 56)) & 63;
                transferBytes = (size_t(transfer.totalPixels) * bitsPerPixel(psm) + 7) / 8;
                transfer.localToHostPendingBytes = transfer.direction == 1 ? transferBytes : 0;
                if (transfer.direction == 1 && transferBytes)
                    ++readbacks;
                transfer.copiedPixels = transfer.direction == 2 ? transfer.totalPixels : 0;
                uploadedBytes = uploadTailSize = 0;
                fifoOffset = fifoTail.size();
            }

            if (addr == GS_REG_HWREG)
                observeImage(8);
        }
        void observeImage(size_t bytes)
        {
            if (transfer.direction != 0)
                return;
            uploadedBytes = std::min(transferBytes, uploadedBytes + bytes);
            transfer.copiedPixels = std::min<size_t>(transfer.totalPixels, uploadedBytes * 8 / bitsPerPixel(unsigned(bitblt >> 56)));
            auto width = uint32_t(trxreg & 0xfff);
            if (width)
            {
                transfer.x = transfer.copiedPixels % width;
                transfer.y = transfer.copiedPixels / width;
            }
        }

        // Observe transfer boundaries only; the upstream decoder alone executes commands.
        // Start from upstream's saved PATH state, including packets split across DMA calls.
        void observeGIF(uint32_t path, const uint8_t *data, size_t size)
        {
            // TEMP DIAGNOSTIC (session 37): bounded GIF traffic census.
            // Guest A+D never reaches WriteRegisterRaw - it arrives inside whole
            // GIF packets via gs.gif_transfer - so every frontend counter and
            // the raw-register histogram are dead.  Counting tags, image bytes
            // and draw tags here is the only way to decide whether the guest is
            // submitting scene geometry or merely repeating image uploads while
            // it waits for data.
            static uint64_t s_gifBytes = 0u;
            static uint64_t s_gifImageBytes = 0u;
            static uint64_t s_gifDrawTags = 0u;
            static uint64_t s_gifTags = 0u;
            static uint64_t s_flg[4] = {0u, 0u, 0u, 0u};
            static uint32_t s_gifPrints = 0u;
            // Session 39: the census tracks *bytes*, which cannot distinguish
            // "the guest stopped submitting geometry" from "the guest submits
            // geometry and we drop it" - both end with an empty draw buffer.
            // Count the A+D writes that actually drive rasterisation instead.
            static uint64_t s_adPrim = 0u;    // PRIM (0x00)     - one per primitive
            static uint64_t s_adXyz = 0u;     // XYZ2 (0x05)     - one per vertex
            static uint64_t s_adTex = 0u;     // TEX0_1 (0x06)
            static uint64_t s_adFrame = 0u;   // FRAME_1 (0x1A)
            // Session 40: per-context capture with the real GS register map.
            // Session 39 read 0x40/0x4C/0x18 and called them TEX0_1/FRAME_1/
            // XYOFFSET_1; they are in fact context-2 registers (context 2 =
            // context 1 + 0x3A) and PABE.  A game is free to leave context 2
            // unconfigured, so only the per-context split can say anything.
            static uint32_t s_xyoff1 = 0u, s_xyoff2 = 0u;
            static uint64_t s_scissor1 = 0u, s_scissor2 = 0u;
            static uint32_t s_frame1 = 0u, s_frame2 = 0u;
            static uint32_t s_test1 = 0u, s_alpha1 = 0u;
            static uint64_t s_xyzSample[3] = {0u, 0u, 0u};
            static uint32_t s_xyzSampleN = 0u;
            static uint64_t s_xyzIn = 0u, s_xyzOut = 0u;
            static int32_t s_minX = 0x7FFFFFFF, s_maxX = -0x7FFFFFFF;
            static int32_t s_minY = 0x7FFFFFFF, s_maxY = -0x7FFFFFFF;
            // FLG=1 (packed-reglist) sends nreg A words then nreg D words per
            // loop, so a loop can straddle a DMA call; park the A words here.
            static uint64_t s_reglistA[4][16] = {};
            auto applyAD = [&](uint8_t adReg, uint64_t adValue) {
                switch (adReg)
                {
                case 0x00: ++s_adPrim; break;
                case 0x05:
                {
                    ++s_adXyz;
                    if (s_xyzSampleN < 3u)
                    {
                        s_xyzSample[s_xyzSampleN++] = adValue;
                    }
                    // XYZ2 is 12.4 fixed point, X in bits 0-15, Y in 16-31.
                    const int32_t vx = (static_cast<int32_t>(adValue & 0xFFFFu) << 16) >> 20;
                    const int32_t vy = (static_cast<int32_t>((adValue >> 16) & 0xFFFFu) << 16) >> 20;
                    if (vx < s_minX) s_minX = vx;
                    if (vx > s_maxX) s_maxX = vx;
                    if (vy < s_minY) s_minY = vy;
                    if (vy > s_maxY) s_maxY = vy;
                    if (vx >= 0 && vx < 512 && vy >= 0 && vy < 448)
                        ++s_xyzIn;
                    else
                        ++s_xyzOut;
                    break;
                }
                case 0x06: ++s_adTex; break;                              // TEX0_1
                case 0x09: s_xyoff1 = uint32_t(adValue); break;            // XYOFFSET_1 (32-bit)
                case 0x12: s_scissor1 = adValue; break;                    // SCISSOR_1
                case 0x13: s_alpha1 = uint32_t(adValue); break;            // ALPHA_1
                case 0x17: s_test1 = uint32_t(adValue); break;             // TEST_1
                case 0x1A: ++s_adFrame; s_frame1 = uint32_t(adValue); break; // FRAME_1
                case 0x43: s_xyoff2 = uint32_t(adValue); break;            // XYOFFSET_2
                case 0x4C: s_scissor2 = adValue; break;                    // SCISSOR_2
                case 0x54: s_frame2 = uint32_t(adValue); break;            // FRAME_2
                default: break;
                }
            };
            s_gifBytes += size;
            auto state = gs.get_gif_path(path);
            for (size_t offset = 0; offset < size;)
            {
                if (state.loop == state.tag.NLOOP)
                {
                    std::memcpy(&state.tag, data + offset, 16);
                    state.reg = state.loop = 0;
                    ++s_gifTags;
                    ++s_flg[state.tag.FLG & 3u];
                    offset += 16;
                    continue;
                }
                unsigned nreg = state.tag.NREG ? state.tag.NREG : 16;
                if (state.tag.FLG >= 2)
                {
                    size_t count = std::min<size_t>(state.tag.NLOOP - state.loop, (size - offset) / 16);
                    observeImage(count * 16);
                    s_gifImageBytes += count * 16u;
                    state.loop += uint32_t(count);
                    offset += count * 16;
                }
                else if (state.tag.FLG == 1)
                {
                    // Packed-reglist: a loop is nreg A words then nreg D words,
                    // NOT interleaved.  state.reg counts 0..2*nreg-1 across both
                    // phases so a loop straddling a DMA call stays correct.
                    ++s_gifDrawTags;
                    while (offset < size && state.loop < state.tag.NLOOP)
                    {
                        if (state.reg < nreg)
                        {
                            s_reglistA[path & 3u][state.reg] = word(data + offset);
                            ++state.reg;
                        }
                        else
                        {
                            const uint32_t k = state.reg - nreg;
                            applyAD(uint8_t(word(data + offset) & 127), s_reglistA[path & 3u][k]);
                            ++state.reg;
                        }
                        offset += 16;
                        if (state.reg >= nreg * 2u)
                        {
                            state.reg = 0;
                            ++state.loop;
                        }
                    }
                }
                else
                {
                    ++s_gifDrawTags;
                    const uint8_t adReg = uint8_t(word(data + offset + 8) & 127);
                    const uint64_t adValue = word(data + offset);
                    // Only packed A+D can address the transfer registers (>= 0x50).
                    const uint64_t descriptors = word(reinterpret_cast<const uint8_t *>(&state.tag) + 8);
                    if (((descriptors >> (state.reg * 4)) & 15) == 14)
                    {
                        observeRegister(adReg, adValue);
                    }
                    applyAD(adReg, adValue);
                    if (++state.reg == nreg)
                    {
                        state.reg = 0;
                        ++state.loop;
                    }
                    offset += 16;
                }
            }
            if (s_gifPrints < 40u && s_gifBytes >= (static_cast<uint64_t>(s_gifPrints + 1u) << 18))
            {
                ++s_gifPrints;
                std::cerr << "[GS-census] bytes=" << s_gifBytes << " tags=" << s_gifTags
                          << " imageBytes=" << s_gifImageBytes << " drawTags=" << s_gifDrawTags
                          << " dir=" << unsigned(transfer.direction) << " need=" << transferBytes
                          << " up=" << uploadedBytes << " px=" << transfer.copiedPixels << "/"
                          << transfer.totalPixels << " prim=" << s_adPrim << " xyz=" << s_adXyz
                          << " in=" << s_xyzIn << " out=" << s_xyzOut << " X=" << s_minX << ".."
                          << s_maxX << " Y=" << s_minY << ".." << s_maxY << " tex=" << s_adTex
                          << " frame=" << s_adFrame << std::hex << " F1=0x" << s_frame1
                          << " S1=0x" << s_scissor1 << " XY1=0x" << s_xyoff1 << " T1=0x" << s_test1
                          << " A1=0x" << s_alpha1 << " F2=0x" << s_frame2 << " S2=0x" << s_scissor2
                          << " XY2=0x" << s_xyoff2 << " v0=0x" << s_xyzSample[0] << " v1=0x"
                          << s_xyzSample[1] << " v2=0x" << s_xyzSample[2]
                          << " flg=" << s_flg[0] << "/" << s_flg[1] << "/" << s_flg[2] << "/"
                          << s_flg[3] << " ourFLG=" << unsigned(state.tag.FLG) << " ourNLOOP="
                          << state.tag.NLOOP << " ourLoop=" << state.loop << " upFLG="
                          << unsigned(gs.get_gif_path(path).tag.FLG) << " upNLOOP="
                          << gs.get_gif_path(path).tag.NLOOP << " upLoop="
                          << gs.get_gif_path(path).loop << std::dec << std::endl;
            }
        }

        bool on_signal(uint64_t value) override
        {
            auto mask = uint32_t(value >> 32);
            auto id = (uint32_t(priv.siglblid) & ~mask) | (uint32_t(value) & mask);
            priv.siglblid = (priv.siglblid & 0xffffffff00000000ull) | id;
            priv.csr.fetch_or(1);
            return false;
        }

        bool on_finish(uint64_t) override
        {
            gs.flush();
            interop.device().wait_idle();
            priv.csr.fetch_or(2);
            return false;
        }

        bool on_label(uint64_t value) override
        {
            auto mask = uint32_t(value >> 32);
            auto id = (uint32_t(priv.siglblid >> 32) & ~mask) | (uint32_t(value) & mask);
            priv.siglblid = (uint64_t(id) << 32) | uint32_t(priv.siglblid);
            return false;
        }

    public:
        explicit ParallelBackend(GSRegisters &registers) : priv(registers)
        {
            if (!gs.init(&interop.device(), ParallelGS::GSOptions{}))
                throw std::runtime_error("parallel-gs: GS initialization failed; check Vulkan feature diagnostics above");
            gs.set_signal_interface(this);
        }

        ~ParallelBackend() override
        {
            Lock lock(mutex);
            gs.flush();
            interop.device().wait_idle();
        }

        bool UsesRawCommands() const override
        {
            return true;
        }

        uint64_t GetReadbackCount() const override
        {
            Lock lock(mutex);
            return readbacks;
        }

        void Initialize(uint8_t *vram, uint32_t size) override
        {
            Lock lock(mutex);
            if (!vram || size < VramSize)
                throw std::invalid_argument("parallel-gs requires 4 MiB initial VRAM");
            std::memcpy(gs.map_vram_write(0, VramSize), vram, VramSize);
            gs.end_vram_write(0, VramSize);
        }

        void Reset() override
        {
            Lock lock(mutex);
            gs.flush();
            interop.device().wait_idle();
            gs.reset_context_state();
            bitblt = trxreg = transferBytes = uploadedBytes = uploadTailSize = 0;
            fifoOffset = 16;
            transfer = {};
        }

        void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t size) override
        {
            Lock lock(mutex);
            if (path < 1 || path > 3 || size % 16)
                throw std::invalid_argument("parallel-gs: invalid GIF path or size");
            if (!data || !size)
                return;
            // TEMP DIAGNOSTIC (session 37): what actually reaches parallel-gs,
            // per path, to bisect the Path2 image loss.
            {
                static uint64_t s_inBytes[4] = {0u, 0u, 0u, 0u};
                static uint64_t s_inTotal = 0u;
                static uint32_t s_inPrints = 0u;
                s_inBytes[path & 3u] += size;
                s_inTotal += size;
                if (s_inPrints < 16u && s_inTotal >= ((static_cast<uint64_t>(s_inPrints) + 1u) << 19))
                {
                    ++s_inPrints;
                    std::cerr << "[GIFrecv] total=" << s_inTotal << " p1=" << s_inBytes[1]
                              << " p2=" << s_inBytes[2] << " p3=" << s_inBytes[3] << std::endl;
                }
            }
            observeGIF(path, data, size);
            gs.gif_transfer(path, data, size);
            // Session 41: parallel-gs's own accounting.  Everything in the
            // census above only proves what *we* parsed; this proves what it
            // actually rasterised and copied.  consume_flush_stats() resets the
            // counters, so accumulate.
            {
                static uint64_t s_psPrims = 0u, s_psPasses = 0u, s_psCopies = 0u;
                static uint64_t s_psPalettes = 0u, s_psBytes = 0u;
                static uint32_t s_psPrints = 0u;
                const auto stats = gs.consume_flush_stats();
                s_psPrims += stats.num_primitives;
                s_psPasses += stats.num_render_passes;
                s_psCopies += stats.num_copies;
                s_psPalettes += stats.num_palette_updates;
                s_psBytes += size;
                if (s_psPrints < 40u && s_psBytes >= (static_cast<uint64_t>(s_psPrints + 1u) << 18))
                {
                    ++s_psPrints;
                    std::cerr << "[GSpost] bytes=" << s_psBytes << " prims=" << s_psPrims
                              << " passes=" << s_psPasses << " copies=" << s_psCopies
                              << " palettes=" << s_psPalettes << std::endl;
                }
            }
        }

        void WriteRegisterRaw(uint8_t addr, uint64_t value) override
        {
            Lock lock(mutex);
            if (addr >= 128)
                return; // Reserved addresses never index upstream's 128-entry table.
            // TEMP DIAGNOSTIC (session 37): bounded A+D histogram.  Guest GIF
            // traffic bypasses the frontend switch entirely (UsesRawCommands),
            // so every primitive/vertex counter there is dead; counting raw
            // register writes here is the only way to tell "the guest is
            // drawing" from "the guest is stuck waiting".
            {
                static std::array<uint32_t, 128> s_adCounts{};
                static uint64_t s_adTotal = 0u;
                static uint32_t s_adPrints = 0u;
                ++s_adCounts[addr];
                ++s_adTotal;
                if ((s_adTotal & 0xFFFull) == 0u && s_adPrints < 12u)
                {
                    ++s_adPrints;
                    std::cerr << "[GS-adhist] total=" << std::dec << s_adTotal << " :";
                    for (uint32_t r = 0u; r < 128u; ++r)
                    {
                        if (s_adCounts[r] != 0u)
                        {
                            std::cerr << " 0x" << std::hex << r << std::dec << "=" << s_adCounts[r];
                        }
                    }
                    std::cerr << std::endl;
                }
            }
            observeRegister(addr, value);
            // Compatibility aliases used by native stubs, not actual GIF registers.
            switch (addr)
            {
            case 0x59:
                priv.writeDisplayFramebuffer(0, value);
                return;
            case 0x5a:
                priv.display1 = value;
                return;
            case 0x5b:
                priv.writeDisplayFramebuffer(1, value);
                return;
            case 0x5c:
                priv.display2 = value;
                return;
            case 0x5f:
                priv.bgcolor = value;
                return;
            }
            gs.write_register(static_cast<ParallelGS::RegisterAddr>(addr), value);
        }

        void Submit(const GSPrimitiveBatch &) override
        {
            throw std::logic_error("parallel-gs received a CPU primitive batch");
        }

        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override
        {
        }

        void BeginTransfer(const GSTransferCommand &) override
        {
            throw std::logic_error("parallel-gs requires raw transfer registers");
        }

        void UploadImage(const uint8_t *data, uint32_t size) override
        {
            Lock lock(mutex);
            if (!data || transfer.direction != 0)
                return;
            size = uint32_t(std::min<size_t>(size, transferBytes - uploadedBytes));
            observeImage(size);
            while (size)
            {
                auto n = std::min<size_t>(size, 8 - uploadTailSize);
                std::memcpy(uploadTail.data() + uploadTailSize, data, n);
                uploadTailSize += n;
                data += n;
                size -= uint32_t(n);
                if (uploadTailSize == 8 || uploadedBytes == transferBytes && size == 0)
                {
                    std::fill(uploadTail.begin() + uploadTailSize, uploadTail.end(), 0);
                    gs.write_register(ParallelGS::RegisterAddr::HWREG, word(uploadTail.data()));
                    uploadTailSize = 0;
                }
            }
        }

        void Flush() override
        {
            Lock lock(mutex);
            gs.flush();
        }

        void TextureFlush() override
        {
            WriteRegisterRaw(GS_REG_TEXFLUSH, 0);
        }

        void Sync(GSSyncReason reason) override
        {
            Lock lock(mutex);
            if (reason == GSSyncReason::Finish || reason == GSSyncReason::Reset)
            {
                gs.flush();
                interop.device().wait_idle();
            }
            // map_vram_read and the upstream FIFO synchronize their own readbacks.
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            Lock lock(mutex);
            logPresentationModeOnce(request);
            gs.flush();
            auto &p = gs.get_priv_register_state();
            p.qwords_lo[0] = request.pmode;
            p.qwords_lo[2] = request.smode1;
            p.qwords_lo[4] = request.smode2;
            p.qwords_lo[12] = request.syncv;
            p.qwords_lo[14] = request.dispfb1;
            p.qwords_lo[16] = request.display1;
            p.qwords_lo[18] = request.dispfb2;
            p.qwords_lo[20] = request.display2;
            p.qwords_lo[28] = request.bgcolor;
            ParallelGS::VSyncInfo info{};
            info.phase = request.field;
            info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
            info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
            info.crtc_offsets = true;
            return interop.present(gs.vsync(info));
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            Lock lock(mutex);
            auto *mapped = const_cast<uint8_t *>(static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize)));
            ++readbacks;
            return pixelAccess(psm).read(mapped, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            Lock lock(mutex);
            gs.map_vram_read(0, VramSize); // Preserve untouched pixels and packed-format lanes.
            auto *mapped = static_cast<uint8_t *>(gs.map_vram_write(0, VramSize));
            ++readbacks;
            pixelAccess(psm).write(mapped, base, bw, x, y, value);
            gs.end_vram_write(0, VramSize);
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            Lock lock(mutex);
            auto *mapped = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
            ++readbacks;
            out.assign(mapped, mapped + VramSize);
        }

        bool ClearFramebuffer(const GSContext &ctx, uint32_t rgba) override
        {
            Lock lock(mutex);
            if (!ctx.frame.fbw || (ctx.frame.psm != GS_PSM_CT32 && ctx.frame.psm != GS_PSM_CT24 &&
                                   ctx.frame.psm != GS_PSM_CT16 && ctx.frame.psm != GS_PSM_CT16S))
                return false;

            gs.map_vram_read(0, VramSize); // FBMSK and untouched VRAM require current contents.
            auto *mapped = static_cast<uint8_t *>(gs.map_vram_write(0, VramSize));
            ++readbacks;
            auto access = pixelAccess(ctx.frame.psm);

            if (ctx.fba & 1)
                rgba |= 0x80000000u;
            if (bitsPerPixel(ctx.frame.psm) == 16)
                rgba = ((rgba >> 3) & 31) | ((rgba >> 6) & 0x3e0) | ((rgba >> 9) & 0x7c00) | ((rgba >> 16) & 0x8000);

            for (uint32_t y = ctx.scissor.y0; y <= ctx.scissor.y1; ++y)
                for (uint32_t x = ctx.scissor.x0; x <= ctx.scissor.x1; ++x)
                {
                    auto old = access.read(mapped, ctx.frame.fbp * 32, ctx.frame.fbw, x, y);
                    access.write(mapped, ctx.frame.fbp * 32, ctx.frame.fbw, x, y,
                                 (rgba & ~ctx.frame.fbmsk) | (old & ctx.frame.fbmsk));
                }
            gs.end_vram_write(0, VramSize);
            return true;
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            Lock lock(mutex);
            if (!dst)
                return 0;
            auto count = uint32_t(std::min<size_t>(maxBytes, transfer.localToHostPendingBytes));
            for (uint32_t i = 0; i < count; ++i)
            {
                if (fifoOffset == 16)
                {
                    gs.read_transfer_fifo(fifoTail.data(), 1);
                    fifoOffset = 0;
                }
                dst[i] = fifoTail[fifoOffset++];
            }
            transfer.localToHostPendingBytes -= count;
            return count;
        }

        GSTransferSnapshot GetTransferSnapshot() const override
        {
            Lock lock(mutex);
            return transfer;
        }

        void ReadRegisterState(GSDebugSnapshot &s) const override
        {
            Lock lock(mutex);
            const auto &r = gs.get_register_state();
            for (unsigned i = 0; i < 2; ++i)
            {
                auto &c = s.ctx[i];
                const auto &v = r.ctx[i];
                auto f = v.frame.bits, t = v.tex0.bits, sc = v.scissor.bits, z = v.zbuf.bits;
                c.frame = {uint32_t(f & 511), uint32_t((f >> 16) & 63), uint8_t((f >> 24) & 63), uint32_t(f >> 32)};
                c.zbuf = {uint32_t(z & 511), uint8_t(((z >> 24) & 15) | 0x30), bool((z >> 32) & 1)};
                c.scissor = {uint16_t(sc & 2047), uint16_t((sc >> 16) & 2047), uint16_t((sc >> 32) & 2047), uint16_t((sc >> 48) & 2047)};
                c.xyoffset = {uint16_t(v.xyoffset.bits), uint16_t(v.xyoffset.bits >> 32)};
                c.tex0 = {uint32_t(t & 16383), uint8_t((t >> 14) & 63), uint8_t((t >> 20) & 63), uint8_t((t >> 26) & 15),
                          uint8_t((t >> 30) & 15), uint8_t((t >> 34) & 1), uint8_t((t >> 35) & 3), uint32_t((t >> 37) & 16383),
                          uint8_t((t >> 51) & 15), uint8_t((t >> 55) & 1), uint8_t((t >> 56) & 31), uint8_t(t >> 61)};
                c.tex1 = v.tex1.bits;
                c.miptbp1 = v.miptbl_1_3.bits;
                c.miptbp2 = v.miptbl_4_6.bits;
                c.clamp = v.clamp.bits;
                c.alpha = v.alpha.bits;
                c.test = v.test.bits;
                c.fba = v.fba.bits;
            }

            auto p = r.prim.bits;
            s.prim = {GSPrimType(p & 7), bool(p & 8), bool(p & 16), bool(p & 32), bool(p & 64), bool(p & 128), bool(p & 256), bool(p & 512), bool(p & 1024)};
            auto b = r.bitbltbuf.bits, t = r.trxpos.bits, a = r.texa.bits, c = r.texclut.bits;
            s.texa = {uint8_t(a), bool(a & 0x8000), uint8_t(a >> 32)};
            s.texclut = {uint8_t(c & 63), uint8_t((c >> 6) & 63), uint16_t((c >> 12) & 1023)};
            s.scanmsk = r.scanmsk.bits;
            s.dimx = r.dimx.bits;
            s.dthe = r.dthe.bits;
            s.colclamp = r.colclamp.bits;
            s.bitbltbuf = {uint32_t(b & 16383), uint8_t((b >> 16) & 63), uint8_t((b >> 24) & 63),
                           uint32_t((b >> 32) & 16383), uint8_t((b >> 48) & 63), uint8_t((b >> 56) & 63)};

            s.trxpos = {uint16_t(t & 2047), uint16_t((t >> 16) & 2047), uint16_t((t >> 32) & 2047), uint16_t((t >> 48) & 2047), uint8_t((t >> 59) & 3)};
            s.trxreg = {uint16_t(r.trxreg.bits & 4095), uint16_t((r.trxreg.bits >> 32) & 4095)};
            s.trxdir = uint32_t(r.trxdir.bits & 3);
        }
    };
}

std::unique_ptr<GSRasterBackend> CreateParallelGSBackend(GSRegisters &registers)
{
    RUNTIME_WARNING("GS Parallel backend is experimental and may not be fully compatible with all games.");
    return std::make_unique<ParallelBackend>(registers);
}
