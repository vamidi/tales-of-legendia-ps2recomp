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
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
    constexpr size_t VramSize = 4 * 1024 * 1024;
    // TEMP EXPERIMENT (session 47, option B): when set, rewrite the game's
    // per-frame empty ctx0 SCISSOR_1 write (0x100000001) to the full-screen
    // value in the forwarded GIF stream, to test whether that scissor is THE
    // blocker for the black scene.
    bool forceFullScissorEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_FORCE_FULL_SCISSOR");
            return e && *e && *e != '0';
        }();
        return v;
    }
    // TEMP DIAGNOSTIC (session 54): the last BITBLTBUF the guest wrote.  Shared
    // between the observeGIF walk (writer) and ProcessGIF's payload probe
    // (reader), which are different functions - a function-local static in the
    // walk is not visible from the forwarder.
    uint64_t &bitbltMirror()
    {
        static uint64_t v = 0u;
        return v;
    }
    // TEMP DIAGNOSTIC (session 49): gate for the buffer-boundary provenance
    // stamp (see [GIFsub2] in ps2_memory.cpp).  Opt-in via PS2X_GIF_PROVENANCE=1.
    bool gifProvenanceEnabled()
    {
        static const bool v = std::getenv("PS2X_GIF_PROVENANCE") != nullptr;
        return v;
    }
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
        // TEMP EXPERIMENT (session 47): PACKED-path SCISSOR_1 offsets within the
        // current chunk whose A word is the empty rect, recorded by observeGIF
        // and patched by the forward site when PS2X_FORCE_FULL_SCISSOR is set.
        std::vector<std::pair<size_t, uint64_t>> m_scisPatch;
        uint64_t m_scisPatched = 0u;
        // TEMP DIAGNOSTIC (session 48): vertex-pair extents on the PACKED path
        // (FLG=0, reliable A+D pairing).  Consecutive XYZ2/XYZ3 A words within
        // one primitive are paired (the pair resets at each PRIM write), and
        // |dx|/|dy| in 12.4 fixed point are histogrammed per prim type to test
        // whether the black-phase sprites are degenerate (zero-area) quads.
        uint64_t m_xyzPrev = 0;
        bool m_haveXyzPrev = false;
        uint32_t m_pairPrim = 0;
        uint64_t m_pairCount[8] = {};
        uint64_t m_pairZero[8] = {};
        uint32_t m_pairMaxW = 0, m_pairMaxH = 0;
        uint64_t m_pairRaw[4][2] = {};
        uint32_t m_pairRawPrim[4] = {};
        uint32_t m_pairSamples = 0;
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
            // Register numbers for the A+D D-field come from gs_types.h, which is
            // the GIF REGS table with the two contexts interleaved (XYOFFSET_1 =
            // 0x18, SCISSOR_1 = 0x40, FRAME_1 = 0x4C, ...).  Session 40 swapped in
            // the hardware MMIO index map and that is what made the echo read as
            // garbage; the low registers (PRIM 0x00, RGBAQ 0x01, XYZ2 0x05,
            // TEX0_1 0x06) are the same in both schemes.
            static uint64_t s_adPrim = 0u;    // PRIM (0x00)     - one per primitive
            static uint64_t s_adXyz = 0u;     // XYZ2 (0x05)     - one per vertex
            static uint64_t s_adTex = 0u;     // TEX0_1 (0x06)
            static uint64_t s_adFrame = 0u;   // FRAME_1 (0x4C)
            static uint64_t s_adRgba = 0u;    // RGBAQ (0x01)    - one per vertex/colour
            static uint32_t s_xyoff1 = 0u, s_xyoff2 = 0u;
            static uint64_t s_scissor1 = 0u, s_scissor2 = 0u;
            static uint32_t s_frame1 = 0u, s_frame2 = 0u;
            static uint32_t s_test1 = 0u, s_alpha1 = 0u;
            static uint64_t s_zbuf1 = 0u;     // ZBUF_1 (0x4E)
            static uint64_t s_xyzSample[3] = {0u, 0u, 0u};
            static uint32_t s_xyzSampleN = 0u;
            static uint64_t s_xyzIn = 0u, s_xyzOut = 0u;
            static int32_t s_minX = 0x7FFFFFFF, s_maxX = -0x7FFFFFFF;
            static int32_t s_minY = 0x7FFFFFFF, s_maxY = -0x7FFFFFFF;
            // Session 43: the VRAM census counts *non-zero* words, so it cannot
            // tell "nothing was rasterised" from "everything was rasterised in
            // black".  parallel-gs reports 49.5k primitives and 915 render passes
            // after the transition, so colour is now a first-class suspect.  Count
            // what RGBAQ actually carries: R/G/B are bits 0-23, A is bits 24-31.
            static uint64_t s_rgbBlack = 0u;   // R=G=B=0
            static uint64_t s_rgbOpaque = 0u;  // A == 0xFF
            static uint64_t s_rgbZeroA = 0u;   // A == 0
            static uint64_t s_rgbaSample[3] = {0u, 0u, 0u};
            static uint32_t s_rgbaSampleN = 0u;
            // PRIM flags: IIP bit 0, TME bit 1, FGE bit 2, ABE bit 3, FST bit 5.
            static uint64_t s_primTme = 0u, s_primAbe = 0u, s_primFge = 0u, s_primIip = 0u;
            static uint64_t s_primSample[3] = {0u, 0u, 0u};
            static uint32_t s_primSampleN = 0u;
            static uint64_t s_primType[8] = {};   // 0 Point 1 Line 2 LineStrip 3 Tri
                                                 // 4 TriStrip 5 TriFan 6 Sprite
            static uint64_t s_lastPrimVal = 0u;   // PRIM in force when a packet is dumped
            // FLG=1 (packed-reglist) sends nreg A words then nreg D words per
            // loop, so a loop can straddle a DMA call; park the A words here.
            static uint64_t s_reglistA[4][16] = {};
            // Session 46: record *where* each parked A word came from, so a PRIM
            // value can be shown next to the raw bytes it was read out of.  The
            // CTXT=3 anomaly lives in the upper bits of the A word, which is
            // exactly where the reglist pairing error would show up, so it has to
            // be confirmed from raw bytes rather than from this walk's decode.
            static size_t s_reglistAOff[4][16] = {};
            static uint32_t s_primRawGood = 2u, s_primRawBad = 6u;
            auto wantPrimRaw = [&]() -> bool {
                if (s_gifBytes >= (12u << 20))
                    return s_primRawBad != 0u;
                return s_gifBytes >= (2u << 20) && s_primRawGood != 0u;
            };
            auto takePrimRaw = [&]() {
                if (s_gifBytes >= (12u << 20))
                    --s_primRawBad;
                else
                    --s_primRawGood;
            };
            // Print the raw QWs around a PRIM write: two before, the pair itself,
            // two after.  In FLG=0 the A and D halves share one QW; in FLG=1 they
            // are nreg QWs apart, and aOff marks the one this walk paired with.
            auto primRawDump = [&](const char *flgKind, size_t dOff, size_t aOff, uint64_t aVal) {
                std::cerr << "[GS-prim] phase=" << (s_gifBytes >= (12u << 20) ? "black" : "loading")
                          << " path=" << unsigned(path) << " flg=" << flgKind
                          << " bytes=" << s_gifBytes << "\n";
                const size_t first = dOff >= 32u ? (dOff - 32u) & ~size_t(15u) : 0u;
                const size_t last = std::min(size, dOff + 48u);
                for (size_t q = first; q + 16u <= last; q += 16u)
                {
                    std::cerr << "   " << (q == dOff ? "D>" : (q == aOff ? "A>" : " ."))
                              << " qword@0x" << std::hex << q << " = 0x" << word(data + q + 8)
                              << ":" << word(data + q) << std::dec << "\n";
                }
                std::cerr << "   paired A=0x" << std::hex << aVal << std::dec
                          << "  PRIM=" << unsigned(aVal & 7u)
                          << " iip=" << unsigned((aVal >> 3) & 1u)
                          << " tme=" << unsigned((aVal >> 4) & 1u)
                          << " fge=" << unsigned((aVal >> 5) & 1u)
                          << " abe=" << unsigned((aVal >> 6) & 1u)
                          << " aa1=" << unsigned((aVal >> 7) & 1u)
                          << " fst=" << unsigned((aVal >> 8) & 1u)
                          << " ctxt=" << unsigned((aVal >> 9) & 3u)
                          << " fix=0x" << std::hex << ((aVal >> 11) & 0x1FFFu) << std::dec << "\n";
            };
            auto applyAD = [&](uint8_t adReg, uint64_t adValue) {
                switch (adReg)
                {
                case 0x00:
                    // parallel-gs's PRIMBits: PRIM bits 0-2, IIP 3, TME 4, FGE 5,
                    // ABE 6, AA1 7, FST 8, CTXT 9-10.  The earlier flag decode
                    // used bits 1/2/3 and so mislabelled every tme/fge/abe count.
                    ++s_adPrim;
                    s_lastPrimVal = adValue;
                    ++s_primType[adValue & 7u];
                    if (adValue & 0x8u) ++s_primIip;
                    if (adValue & 0x10u) ++s_primTme;
                    if (adValue & 0x20u) ++s_primFge;
                    if (adValue & 0x40u) ++s_primAbe;
                    if (s_primSampleN < 3u)
                    {
                        s_primSample[s_primSampleN++] = adValue;
                    }
                    break;
                case 0x01:
                    ++s_adRgba;
                    if ((adValue & 0x00FFFFFFull) == 0u) ++s_rgbBlack;
                    if (((adValue >> 24) & 0xFFu) == 0xFFu) ++s_rgbOpaque;
                    if (((adValue >> 24) & 0xFFu) == 0u) ++s_rgbZeroA;
                    if (s_rgbaSampleN < 3u)
                    {
                        s_rgbaSample[s_rgbaSampleN++] = adValue;
                    }
                    break;
                case 0x05:
                {
                    ++s_adXyz;
                    if (s_xyzSampleN < 3u)
                    {
                        s_xyzSample[s_xyzSampleN++] = adValue;
                    }
                    // XYZ2 is 12.4 fixed point, X in bits 0-15, Y in 16-31.  The
                    // hardware subtracts XYOFFSET before clipping, so raw values
                    // near the offset are on-screen; keep the raw extent only.
                    const int32_t vx = (static_cast<int32_t>(adValue & 0xFFFFu) << 16) >> 20;
                    const int32_t vy = (static_cast<int32_t>((adValue >> 16) & 0xFFFFu) << 16) >> 20;
                    if (vx < s_minX) s_minX = vx;
                    if (vx > s_maxX) s_maxX = vx;
                    if (vy < s_minY) s_minY = vy;
                    if (vy > s_maxY) s_maxY = vy;
                    const int32_t ox = static_cast<int32_t>(s_xyoff1 & 0xFFFFu) >> 4;
                    const int32_t oy = static_cast<int32_t>(s_xyoff1 >> 16) >> 4;
                    const int32_t sx = vx - ox, sy = vy - oy;
                    if (sx >= 0 && sx < 512 && sy >= 0 && sy < 448)
                        ++s_xyzIn;
                    else
                        ++s_xyzOut;
                    break;
                }
                case 0x06: ++s_adTex; break;                                // TEX0_1
                case 0x18: s_xyoff1 = uint32_t(adValue); break;             // XYOFFSET_1
                case 0x40: s_scissor1 = adValue; break;                     // SCISSOR_1
                case 0x42: s_alpha1 = uint32_t(adValue); break;             // ALPHA_1
                case 0x50: bitbltMirror() = adValue; break;                  // BITBLTBUF
                case 0x4C: ++s_adFrame; s_frame1 = uint32_t(adValue); break; // FRAME_1
                case 0x4D: s_frame2 = uint32_t(adValue); break;             // FRAME_2
                case 0x4E: s_zbuf1 = adValue; break;                        // ZBUF_1
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
                    // Session 45: every conclusion about *why* nothing rasterises
                    // has rested on this walk's decoded register values, and those
                    // are now known to be mis-paired (FRAME_1 read as 0x80070 while
                    // parallel-gs holds FBP 224 / FBW 8 / PSM 0).  So stop decoding:
                    // dump the raw QWs of a few PACKED packets and hand-decode them.
                    // Tag boundaries themselves are not in doubt - they mirror the
                    // upstream state machine - and in FLG=0 each 16-byte group is
                    // one A+D pair: low 8 bytes = A (the register's data), high 8
                    // bytes = D (low 8 bits = GS register number).  One sample while
                    // the loading screen still draws (>= 2 MiB) and five after the
                    // transition (>= 12 MiB; the clear lands near 10 MiB).
                    static uint32_t s_packGood = 1u, s_packBad = 5u;
                    const uint32_t want = s_gifBytes >= (12u << 20) ? s_packBad
                                      : (s_gifBytes >= (2u << 20) ? s_packGood : 0u);
                    if (state.tag.FLG == 0u && want != 0u)
                    {
                        if (s_gifBytes >= (12u << 20))
                            --s_packBad;
                        else
                            --s_packGood;
                        std::cerr << "[GS-pack] phase=" << (s_gifBytes >= (12u << 20) ? "black" : "loading")
                                  << " path=" << unsigned(path) << " bytes=" << s_gifBytes
                                  << " NLOOP=" << state.tag.NLOOP << " NREG=" << state.tag.NREG
                                  << " EOP=" << state.tag.EOP << " tag=0x" << std::hex
                                  << word(data + offset + 8) << ":" << word(data + offset)
                                  << " lastPRIM=0x" << s_lastPrimVal << std::dec
                                  << " type=" << unsigned(s_lastPrimVal & 7u)
                                  << " iip=" << unsigned((s_lastPrimVal >> 3) & 1u)
                                  << " tme=" << unsigned((s_lastPrimVal >> 4) & 1u)
                                  << " abe=" << unsigned((s_lastPrimVal >> 6) & 1u)
                                  << " fst=" << unsigned((s_lastPrimVal >> 8) & 1u)
                                  << " ctxt=" << unsigned((s_lastPrimVal >> 9) & 3u) << "\n";
                        const size_t n = std::min<size_t>(12u, (size - offset - 16) / 16);
                        uint32_t nXyz = 0u, nVtx = 0u;
                        for (size_t k = 0; k < n; ++k)
                        {
                            const size_t q = offset + 16 + k * 16;
                            const uint64_t dW = word(data + q + 8);
                            if ((dW & 0xFFu) == 0x05u || (dW & 0xFFu) == 0x0Du)
                                ++nXyz;
                            if ((dW & 0xFFu) == 0x00u)
                                ++nVtx;
                            std::cerr << "   q" << k << " A=0x" << std::hex << word(data + q)
                                      << " D=0x" << dW << std::dec << "\n";
                        }
                        std::cerr << "   window: qws=" << n << " xyz=" << nXyz
                                  << " primInWindow=" << nVtx << " (NLOOP=" << state.tag.NLOOP
                                  << " -> " << (state.tag.NLOOP * (state.tag.NREG ? state.tag.NREG : 16))
                                  << " A+D pairs)\n";
                    }
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
                            s_reglistAOff[path & 3u][state.reg] = offset;
                            ++state.reg;
                        }
                        else
                        {
                            const uint32_t k = state.reg - nreg;
                            const uint64_t dWord = word(data + offset);
                            const uint64_t aWord = s_reglistA[path & 3u][k];
                            applyAD(uint8_t(dWord & 127), aWord);
                            if ((dWord & 0xFFu) == 0x00u && wantPrimRaw())
                            {
                                takePrimRaw();
                                primRawDump("reglist", offset, s_reglistAOff[path & 3u][k], aWord);
                            }
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
                    // TEMP DIAGNOSTIC (session 48): pair consecutive XYZ2/XYZ3
                    // A words within one primitive (PACKED path only) and
                    // histogram |dx|/|dy| in 12.4 units.  A sprite is degenerate
                    // when dx==0 or dy==0; parallel-gs rasterises zero-area
                    // primitives to nothing, which would explain the census's
                    // zero pixels despite sane scissor/TEST/ALPHA state.
                    if (adReg == 0x00u)
                    {
                        m_pairPrim = uint32_t(adValue & 7u);
                        m_haveXyzPrev = false;
                    }
                    else if (adReg == 0x05u || adReg == 0x0Du)
                    {
                        if (m_haveXyzPrev)
                        {
                            const uint32_t x0 = uint32_t(m_xyzPrev) & 0xFFFFu;
                            const uint32_t y0 = (uint32_t(m_xyzPrev >> 16)) & 0xFFFFu;
                            const uint32_t x1 = uint32_t(adValue) & 0xFFFFu;
                            const uint32_t y1 = (uint32_t(adValue >> 16)) & 0xFFFFu;
                            const uint32_t w = x1 > x0 ? x1 - x0 : x0 - x1;
                            const uint32_t h = y1 > y0 ? y1 - y0 : y0 - y1;
                            ++m_pairCount[m_pairPrim & 7u];
                            if (!w || !h)
                                ++m_pairZero[m_pairPrim & 7u];
                            if (w > m_pairMaxW)
                                m_pairMaxW = w;
                            if (h > m_pairMaxH)
                                m_pairMaxH = h;
                            if (m_pairSamples < 4u && s_gifBytes >= (12u << 20))
                            {
                                m_pairRaw[m_pairSamples][0] = m_xyzPrev;
                                m_pairRaw[m_pairSamples][1] = adValue;
                                m_pairRawPrim[m_pairSamples] = m_pairPrim;
                                ++m_pairSamples;
                            }
                        }
                        m_xyzPrev = adValue;
                        m_haveXyzPrev = true;
                    }
                    // TEMP DIAGNOSTIC (session 47): what SCISSOR values does the
                    // guest actually send?  parallel-gs holds ctx0 scissor
                    // 0x100000001 (empty: SCAX0=1,SCAX1=0,SCAY0=1,SCAY1=0) during
                    // the black-phase textured-sprite window.  This logger shows
                    // whether the game wrote that itself or we mangled it.  PACKED
                    // (FLG=0) A+D pairing is reliable (both halves share one QW);
                    // false positives on vertex data are possible but recognizable
                    // by value shape.
                    {
                        static uint32_t s_scisPrints = 0u;
                        if ((adReg == 0x40u || adReg == 0x41u) && s_scisPrints < 48u)
                        {
                            ++s_scisPrints;
                            std::cerr << "[GS-scis] " << (adReg == 0x40u ? "SCISSOR_1" : "SCISSOR_2")
                                      << " bytes=" << s_gifBytes << " off=0x" << std::hex << offset
                                      << " val=0x" << adValue << std::dec << std::endl;
                        }
                    }
                    if (adReg == 0x40u && adValue == 0x100000001ull && forceFullScissorEnabled())
                        m_scisPatch.push_back({offset, 0x01bf000001ff0000ull});
                    if (adReg == 0x00u && wantPrimRaw())
                    {
                        takePrimRaw();
                        primRawDump("packed", offset, offset, adValue);
                    }
                    if (++state.reg == nreg)
                    {
                        state.reg = 0;
                        ++state.loop;
                    }
                    offset += 16;
                }
            }
            if (s_gifPrints < 200u && s_gifBytes >= (static_cast<uint64_t>(s_gifPrints + 1u) << 18))
            {
                ++s_gifPrints;
                std::cerr << "[GS-census] bytes=" << s_gifBytes << " tags=" << s_gifTags
                          << " imageBytes=" << s_gifImageBytes << " drawTags=" << s_gifDrawTags
                          << " dir=" << unsigned(transfer.direction) << " need=" << transferBytes
                          << " up=" << uploadedBytes << " px=" << transfer.copiedPixels << "/"
                          << transfer.totalPixels << " prim=" << s_adPrim << " xyz=" << s_adXyz
                          << " in=" << s_xyzIn << " out=" << s_xyzOut << " X=" << s_minX << ".."
                          << s_maxX << " Y=" << s_minY << ".." << s_maxY << " tex=" << s_adTex
                          << " frame=" << s_adFrame
                           << " rgba=" << s_adRgba << " black=" << s_rgbBlack << " a0="
                           << s_rgbZeroA << " a255=" << s_rgbOpaque << " tme=" << s_primTme
                           << " abe=" << s_primAbe << " fge=" << s_primFge << " iip="
                           << s_primIip << " ptype=" << s_primType[0] << "/" << s_primType[1]
                           << "/" << s_primType[2] << "/" << s_primType[3] << "/" << s_primType[4]
                           << "/" << s_primType[5] << "/" << s_primType[6] << "/" << s_primType[7]
                           << std::hex << " c0=0x" << s_rgbaSample[0] << " c1=0x"
                           << s_rgbaSample[1] << " c2=0x" << s_rgbaSample[2] << " p0=0x"
                           << s_primSample[0] << " Z1=0x" << s_zbuf1 << " F1=0x" << s_frame1
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
            // TEMP DIAGNOSTIC (session 49): buffer-boundary stamp.  Printed to
            // STDOUT immediately before parallel-gs consumes this buffer, so
            // every TRACE line (tag, VERT, Primitive, Degenerate) that follows
            // belongs to it; match qw0 hex against the ps2_prov_sub.bin record
            // to recover the guest source address and VIF code.
            if (gifProvenanceEnabled())
            {
                std::cout << "[GS-recv] path=" << path << " size=" << size
                          << " qw0=0x" << std::hex << word(data + 8) << ":" << word(data)
                          << " qwEnd=0x" << word(data + size - 8) << ":" << word(data + size - 16)
                          << std::dec << std::endl;
            }
            observeGIF(path, data, size);
            // TEMP DIAGNOSTIC (session 54): is the IMAGE payload the guest sends for
            // DBP=0x3FF000 actually zeros, or do we lose it?  Guest GIF traffic
            // reaches parallel-gs through THIS raw path, not through UploadImage(),
            // so the payload has to be counted here.  BITBLTBUF is mirrored by the
            // walk above.  0x3FF000 is the TBP0 the black-phase sprites sample.
            {
                static const bool s_imageDump = std::getenv("PS2X_IMAGE_DUMP") != nullptr;
                static uint32_t s_imgPrints = 0u;
                static uint64_t s_imgBytes = 0u, s_imgNonZero = 0u;
                if (s_imageDump && s_imgPrints < 12u && data != nullptr &&
                    ((bitbltMirror() >> 32) & 0x3FFFu) == 0x3FF0u)
                {
                    uint64_t nz = 0u;
                    for (size_t i = 0u; i < size; ++i)
                    {
                        if (data[i] != 0u)
                            ++nz;
                    }
                    s_imgBytes += size;
                    s_imgNonZero += nz;
                    std::cerr << "[IMG] dbp=0x3FF000 chunk=" << size << " nz=" << nz
                              << " cumBytes=" << s_imgBytes << " cumNz=" << s_imgNonZero
                              << " first=";
                    for (size_t i = 0u; i < 32u && i < size; ++i)
                        std::cerr << std::hex << (i ? ":" : "") << unsigned(data[i]);
                    std::cerr << std::dec << std::endl;
                    ++s_imgPrints;
                }
            }
            // TEMP EXPERIMENT (session 47, option B): forward a patched copy with
            // the empty ctx0 SCISSOR_1 rewrites applied.
            if (forceFullScissorEnabled() && !m_scisPatch.empty())
            {
                std::vector<uint8_t> buf(data, data + size);
                for (const auto &p : m_scisPatch)
                {
                    if (p.first + 8u <= size)
                    {
                        std::memcpy(buf.data() + p.first, &p.second, 8);
                        ++m_scisPatched;
                    }
                }
                m_scisPatch.clear();
                if (m_scisPatched <= 5u || (m_scisPatched & 0x1FFull) == 0u)
                    std::cerr << "[GS-scisPatch] patched=" << m_scisPatched << std::endl;
                gs.gif_transfer(path, buf.data(), size);
            }
            else
            {
                m_scisPatch.clear();
                gs.gif_transfer(path, data, size);
            }
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
                if (s_psPrints < 200u && s_psBytes >= (static_cast<uint64_t>(s_psPrints + 1u) << 18))
                {
                    ++s_psPrints;
                    std::cerr << "[GSpost] bytes=" << s_psBytes << " prims=" << s_psPrims
                              << " passes=" << s_psPasses << " copies=" << s_psCopies
                              << " palettes=" << s_psPalettes << std::endl;
                    // TEMP DIAGNOSTIC (session 47): compact per-threshold sampler of
                    // the state that decides coverage.  Session 47 found ctx0's
                    // scissor = 0x100000001 (SCAX0=1,SCAX1=0,SCAY0=1,SCAY1=0 ->
                    // empty) while textured blended sprites (PRIM=0x15e) were the
                    // active draw, and full-screen scissor only alongside
                    // untextured unblended sprites (PRIM=0x106 = clears).  Sample
                    // every 256 KiB to see which state dominates the black phase.
                    {
                        const auto &rs = gs.get_register_state();
                        const uint64_t pb = rs.prim.bits;
                        const uint64_t s0 = rs.ctx[0].scissor.bits, s1 = rs.ctx[1].scissor.bits;
                        std::cerr << "[GS-sc] bytes=" << s_psBytes
                                  << std::hex << " prim=0x" << pb << " type=" << (pb & 7ull)
                                  << " ctxt=" << ((pb >> 9) & 3ull) << " tme=" << ((pb >> 4) & 1ull)
                                  << " abe=" << ((pb >> 6) & 1ull)
                                  << " c0sc=0x" << s0 << " c1sc=0x" << s1
                                  << " c0fbp=0x" << (rs.ctx[0].frame.bits & 0x1FFull)
                                  << " c1fbp=0x" << (rs.ctx[1].frame.bits & 0x1FFull)
                                  // TEX0/TEX1: is a texture actually bound during the black
                                  // phase, and where does TBP0 point?  The only populated
                                  // VRAM in that phase is 0x3A7000..0x3FF43C, so TBP0*256
                                  // outside that range cannot sample anything.
                                  << " t0=0x" << rs.ctx[0].tex0.bits
                                  << " t1=0x" << rs.ctx[1].tex0.bits
                                  // TEX0: TBP0 bits 0-13 (x256 = byte address), TBW 14-19,
                                  // PSM 20-25, CBP 37-50.
                                  << " t0tbp=0x" << ((rs.ctx[0].tex0.bits & 0x3FFFull) * 256ull)
                                  << " t0tbw=" << ((rs.ctx[0].tex0.bits >> 14) & 0x3Full)
                                  << " t0psm=0x" << ((rs.ctx[0].tex0.bits >> 20) & 0x3Full)
                                  << " t0cbp=0x" << (((rs.ctx[0].tex0.bits >> 37) & 0x3FFFull) * 256ull)
                                  << " bb=0x" << rs.bitbltbuf.bits
                                  << " tp=0x" << rs.trxpos.bits
                                  << " tr=0x" << rs.trxreg.bits
                                  << " td=" << rs.trxdir.bits
                                  << " rgba=0x" << rs.rgbaq.bits
                                  << std::dec
                                  << " pairs=" << m_pairCount[4] << "/" << m_pairCount[6]
                                  << " zero4=" << m_pairZero[4] << " zero6=" << m_pairZero[6]
                                  << " maxWH=" << m_pairMaxW << "/" << m_pairMaxH
                                  << std::hex
                                  << " pr0=" << m_pairRaw[0][0] << ":" << m_pairRaw[0][1]
                                  << " pr1=" << m_pairRaw[1][0] << ":" << m_pairRaw[1][1]
                                  << std::dec << std::endl;
                    }
                }
                // TEMP DIAGNOSTIC (session 47): parallel-gs's OWN register state,
                // read from get_register_state() - the only trusted source left
                // after session 46 voided the walk's values.  The open question is
                // what PRIM the stream really carries: CTXT especially, because
                // gs_interface.cpp indexes registers.ctx[prim.desc.CTXT] with no
                // clamp and ctx[] holds only two entries, so CTXT=2/3 is
                // out-of-bounds garbage (degenerate draws -> zero pixels AND zero
                // Z, which is exactly the black-phase signature).  Dump at byte
                // thresholds straddling the loading->black transition (~10-12 MB).
                static const uint64_t s_regDumpAt[] = {4u << 20, 8u << 20, 12u << 20, 16u << 20,
                                                       24u << 20, 32u << 20, 40u << 20};
                static uint32_t s_regDumpN = 0u;
                while (s_regDumpN < 7u && s_psBytes >= s_regDumpAt[s_regDumpN])
                {
                    ++s_regDumpN;
                    const auto &rs = gs.get_register_state();
                    const uint64_t pb = rs.prim.bits;
                    std::cerr << "[GS-reg] #" << s_regDumpN << " bytes=" << s_psBytes
                              << std::hex << "\n  PRIM=0x" << pb
                              << " type=" << (pb & 7ull) << " iip=" << ((pb >> 3) & 1ull)
                              << " tme=" << ((pb >> 4) & 1ull) << " fge=" << ((pb >> 5) & 1ull)
                              << " abe=" << ((pb >> 6) & 1ull) << " aa1=" << ((pb >> 7) & 1ull)
                              << " fst=" << ((pb >> 8) & 1ull) << " CTXT=" << ((pb >> 9) & 3ull)
                              << " FIX=0x" << ((pb >> 11) & 0x1FFFull)
                              << " prmodecont=0x" << rs.prmodecont.bits
                              << "\n  RGBAQ=0x" << rs.rgbaq.bits << " UV=0x" << rs.uv.bits
                              << " ST=0x" << rs.st.bits << " FOG=0x" << rs.fog.bits
                              << " SCANMSK=0x" << rs.scanmsk.bits << " PABE=0x" << rs.pabe.bits
                              << " DIMX=0x" << rs.dimx.bits << " DTHE=0x" << rs.dthe.bits
                              << " TEXCLUT=0x" << rs.texclut.bits << " TEXA=0x" << rs.texa.bits
                              << " COLCLAMP=0x" << rs.colclamp.bits;
                    for (unsigned ci = 0; ci < 2; ++ci)
                    {
                        const auto &cc = rs.ctx[ci];
                        const uint64_t fb = cc.frame.bits, zb = cc.zbuf.bits, tb = cc.test.bits;
                        std::cerr << "\n  ctx" << ci
                                  << " FRAME=0x" << fb << " fbp=" << (fb & 0x1Full)
                                  << " fbw=" << ((fb >> 16) & 0x3Full) << " psm=" << ((fb >> 24) & 0x3Full)
                                  << " FMASK=0x" << (fb >> 32)
                                  << " ZBUF=0x" << zb << " zbp=" << (zb & 0x1Full)
                                  << " zpsm=" << ((zb >> 24) & 0xFull) << " zmsk=" << ((zb >> 56) & 1ull)
                                  << " TEST=0x" << tb << " ATE=" << (tb & 1ull)
                                  << " ATST=" << ((tb >> 1) & 7ull) << " AREF=0x" << ((tb >> 4) & 0xFFull)
                                  << " AFAIL=" << ((tb >> 12) & 3ull) << " DATE=" << ((tb >> 14) & 1ull)
                                  << " DATM=" << ((tb >> 15) & 1ull) << " ZTE=" << ((tb >> 16) & 1ull)
                                  << " ZTST=" << ((tb >> 17) & 3ull)
                                  << " ALPHA=0x" << cc.alpha.bits << " A=0x" << (cc.alpha.bits & 0xFFull)
                                  << " B=0x" << ((cc.alpha.bits >> 8) & 0xFFull)
                                  << " C=0x" << ((cc.alpha.bits >> 16) & 0xFFull)
                                  << " D=0x" << ((cc.alpha.bits >> 24) & 0xFFull)
                                  << " XYOFF=0x" << cc.xyoffset.bits
                                  << " SCIS=0x" << cc.scissor.bits
                                  << " TEX0=0x" << cc.tex0.bits << " tbp0=0x" << (cc.tex0.bits & 0x3FFFull)
                                  << " tbw=" << ((cc.tex0.bits >> 14) & 0x3Full)
                                  << " psm=" << ((cc.tex0.bits >> 20) & 0x3Full)
                                  << " tw=" << ((cc.tex0.bits >> 26) & 0xFull)
                                  << " th=" << ((cc.tex0.bits >> 30) & 0xFull)
                                  << " tcc=" << ((cc.tex0.bits >> 34) & 1ull)
                                  << " tfx=" << ((cc.tex0.bits >> 35) & 3ull)
                                  << " cbp=0x" << ((cc.tex0.bits >> 37) & 0x3FFFull)
                                  << " FBA=0x" << cc.fba.bits << " CLAMP=0x" << cc.clamp.bits;
                    }
                    std::cerr << std::dec << std::endl;
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
            // TEMP DIAGNOSTIC (session 54): is the IMAGE payload the guest hands us
            // actually zero, or do we drop it?  Tracks the DBP=0x3FF000 transfer -
            // the texture the black-phase sprites sample - counting non-zero bytes
            // per completed transfer and sampling the first 32 bytes.
            {
                static const bool s_imageDump = std::getenv("PS2X_IMAGE_DUMP") != nullptr;
                static uint64_t s_imgBytes = 0u, s_imgNonZero = 0u;
                static uint32_t s_imgPrints = 0u;
                static bool s_imgSampled = false;
                if (s_imageDump)
                {
                    const uint32_t dbp = uint32_t((bitblt >> 32) & 0x3FFFu);
                    if (dbp == 0x3FF0u)
                    {
                        for (uint32_t i = 0u; i < size; ++i)
                        {
                            if (data[i] != 0u)
                                ++s_imgNonZero;
                        }
                        s_imgBytes += size;
                        if (!s_imgSampled && s_imgPrints < 8u)
                        {
                            s_imgSampled = true;
                            std::cerr << "[IMG] dbp=0x" << std::hex << (dbp * 256u) << " first=";
                            for (uint32_t i = 0u; i < 32u && i < size; ++i)
                                std::cerr << std::hex << (i ? ":" : "") << unsigned(data[i]);
                            std::cerr << std::dec << " (chunk=" << size << ")" << std::endl;
                        }
                        if (transferBytes != 0u && uploadedBytes >= transferBytes && s_imgPrints < 8u)
                        {
                            ++s_imgPrints;
                            std::cerr << "[IMG] dbp=0x3FF000 transfer done bytes=" << s_imgBytes
                                      << " nonZero=" << s_imgNonZero << std::endl;
                            s_imgBytes = 0u;
                            s_imgNonZero = 0u;
                            s_imgSampled = false;
                        }
                    }
                }
            }
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
