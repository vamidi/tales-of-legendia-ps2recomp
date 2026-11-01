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
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

// Session 58u: path-2 GIF handoff counters, defined in ps2_gif_arbiter.cpp (hop 5 = this backend).
namespace ps2x::gifhop
{
    void count(int hop, uint32_t path, uint64_t bytes) noexcept;
    void countSynthTag() noexcept;
    void reportIfDue() noexcept;
}

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
    // TEMP DIAGNOSTIC (session 54): decisive experiment for the black phase.  With
    // TME=1 the texture replaces the vertex colour, and a dark/mostly-index-0
    // texture samples to near-black - which the VRAM census cannot distinguish from
    // "nothing drawn".  Clearing TME makes the primitive use its vertex colour (a
    // dark brown 0x2B171180, i.e. non-zero) and forcing RGBAQ makes it bright
    // opaque, so either way rasterised pixels become visible in the census.
    bool forceNoTexEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_FORCE_NOTEX");
            return e && *e && *e != '0';
        }();
        return v;
    }
    bool forceRgbaqEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_FORCE_RGBAQ");
            return e && *e && *e != '0';
        }();
        return v;
    }
    // R=G=B=0x80, A=0x80, Q=1.0
    constexpr uint64_t kBrightRgbaq = 0x3F80000080808080ull;
    // TEX0 bit 34 is TCC (0 = RGB-only texture, so alpha comes from TEXA;
    // 1 = RGBA texture, alpha comes from the texture/CLUT and TEXA is ignored).
    // TEXA: TA0 = bits 0-7, AEM = bit 15, TA1 = bits 32-39.
    constexpr uint64_t kTexaFixedAlpha = 0x0000008000000080ull;
    constexpr uint64_t kTex0TccBit = 1ull << 34;
    // Session 54: forces a known non-zero sampled alpha.  The black-phase sprites
    // use a T8 texture with TCC=1, so As comes from the CLUT entry; if that alpha is
    // 0 the ALPHA blend (Cs-Cd)*As+Cd collapses to Cd = black.  Clearing TCC and
    // pinning TEXA makes As = 0x80 regardless of the texture contents.
    bool forceAlphaEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_FORCE_ALPHA");
            return e && *e && *e != '0';
        }();
        return v;
    }
    // PS2X_FORCE_TBP=1 (session 58w): move the T4 texture the logo sprites sample from tbp0 0x3ff0 to 0x3fe0, the
    // nearest PAGE-ALIGNED 4-bit destination, in BOTH the upload (BITBLTBUF.DBP) and the sampler (TEX0.TBP0).
    //
    // Why: with the VIF1 desync fixed the renderer receives 20164 complete 24x16 T4 transfers (3.97 MB) and calls
    // copy_vram 41511 times, yet 0x3FF000 still holds ~2 non-zero bytes, while the 8x2 CT32 CLUT at 0x3FF400 lands.
    // Both sit mid-page (0x3FF000 is offset 4096 in page 511). A 4-bit page is 8192 B, so a page-aligned DBP must be
    // a multiple of 32 in 256-byte units: 0x3ff0 rounds down to 0x3fe0 (= 0x3FE000, the start of page 511).
    //   - if the texture lands there and the picture changes -> the T4 mid-page/straddle write is the bug;
    //   - if it still does not land -> T4 destination writes are broken regardless of alignment.
    bool forceTbpEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_FORCE_TBP");
            return e && *e && *e != '0';
        }();
        return v;
    }
    constexpr uint32_t kForceTbpFrom = 0x3FF0u; // page index the guest uses (mid-page)
    constexpr uint32_t kForceTbpTo = 0x3FE0u;   // 0x3fe0 * 256 = 0x3FE000, page-aligned
    // PS2X_FORCE_TEXA_ONLY=1: pin TEXA.TA0 = 0x80 but leave TCC alone. Separates "TEXA never reaches the sampler"
    // (this alone restores the frame) from "the texture/CLUT alpha really is 0" (this alone changes nothing).
    bool forceTexaOnlyEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_FORCE_TEXA_ONLY");
            return e && *e && *e != '0';
        }();
        return v;
    }
    // Session 54: dump the raw bytes at the T8 texture (0x3FF000) and CLUT
    // (0x3FF400) the black-phase sprites sample, decode index -> CLUT entry, and
    // report the colour+alpha actually stored there.
    bool texDumpEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_TEX_DUMP");
            return e && *e && *e != '0';
        }();
        return v;
    }
    // Session 54: log the UV/ST the black-phase sprites sample.  TEX0 says PSM 0x14
    // (T4, 16-entry palette, index 0 transparent), so if every UV is 0 the sprites
    // sample texel 0 -> index 0 -> fully transparent, which would explain a 0.0%
    // framebuffer without anything being wrong in the texture path.
    bool uvDumpEnabled()
    {
        static const bool v = [] {
            const char *e = std::getenv("PS2X_UV_DUMP");
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
        uint64_t bitblt = 0, trxreg = 0, trxposBits = 0;
        GSTransferSnapshot transfer;
        size_t transferBytes = 0, uploadedBytes = 0;
        bool xferCompletionLogged = false;
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
            if (addr == GS_REG_TRXPOS)
                trxposBits = value;
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
                xferCompletionLogged = false;
                if (transfer.direction == 0)
                    noteUploadStart(unsigned((bitblt >> 32) & 0x3FFFu), transferBytes);
                // PS2X_XFER_LOG=1: every GS transfer with both buffer pointers. Tales of Legendia draws into frame
                // buffer 224 while DISPFB2 keeps pointing at 0 and never changes, so the only thing that can put a
                // drawn logo on screen is a direction-2 copy (VRAM->VRAM); direction 0 is host->local (an upload),
                // 1 is local->host (a readback).
                static const bool s_logXfer = std::getenv("PS2X_XFER_LOG") != nullptr;
                if (s_logXfer)
                {
                    static uint64_t s_xfers = 0u;
                    const uint64_t n = s_xfers++;
                    if (n < 96u || (n % 512u) == 0u)
                        std::fprintf(stderr,
                                     "[XFER] #%llu dir=%u src(sbp=0x%x sbw=%u spsm=0x%x) dst(dbp=0x%x dbw=%u dpsm=0x%x) "
                                     "rect=%u,%u %ux%u px=%u bytes=%zu\n",
                                     static_cast<unsigned long long>(n), transfer.direction,
                                     unsigned(bitblt & 0x3FFFu), unsigned((bitblt >> 16) & 0x3Fu),
                                     unsigned((bitblt >> 24) & 0x3Fu), unsigned((bitblt >> 32) & 0x3FFFu),
                                     unsigned((bitblt >> 48) & 0x3Fu), unsigned((bitblt >> 56) & 0x3Fu),
                                     unsigned(trxposBits & 0x7FFu), unsigned((trxposBits >> 16) & 0x7FFu),
                                     unsigned(trxreg & 0xFFFu), unsigned((trxreg >> 32) & 0xFFFu),
                                     transfer.totalPixels, transferBytes);
                }
            }

            if (addr == GS_REG_HWREG)
            {
                // PS2X_XFER_STATE=1: our transfer mirror beside parallel-gs's own, at the moment payload bytes flow.
                // If its trxdir says 0 while we are accumulating bytes, it is discarding data we happily count - the
                // gap between "1 MB received" and "44 bytes in VRAM".
                static const bool s_logState = std::getenv("PS2X_XFER_STATE") != nullptr;
                if (s_logState && transfer.direction == 0 && transferBytes != 0u)
                {
                    static uint32_t s_n = 0u;
                    if (s_n < 16u)
                    {
                        ++s_n;
                        const auto &rs = gs.get_register_state();
                        std::fprintf(stderr,
                                     "[GS-xferstate] #%u ours{dir=%u dbp=0x%x need=%zu up=%zu} theirs{trxdir=%llu "
                                     "dbp=0x%llx trxreg=0x%llx}\n",
                                     s_n, transfer.direction, unsigned((bitblt >> 32) & 0x3FFFu), transferBytes,
                                     uploadedBytes, static_cast<unsigned long long>(rs.trxdir.bits & 3u),
                                     static_cast<unsigned long long>((rs.bitbltbuf.bits >> 32) & 0x3FFFu),
                                     static_cast<unsigned long long>(rs.trxreg.bits));
                    }
                }
                observeImage(8);
            }
        }
        void observeImage(size_t bytes)
        {
            if (transfer.direction != 0)
                return;
            uploadedBytes = std::min(transferBytes, uploadedBytes + bytes);
            noteUploadBytes(bytes);
            transfer.copiedPixels = std::min<size_t>(transfer.totalPixels, uploadedBytes * 8 / bitsPerPixel(unsigned(bitblt >> 56)));
            auto width = uint32_t(trxreg & 0xfff);
            if (width)
            {
                transfer.x = transfer.copiedPixels % width;
                transfer.y = transfer.copiedPixels / width;
            }
            // PS2X_XFER_LOG=1: whether the upload actually delivered the whole rectangle. A copy whose payload never
            // completes is indistinguishable from "nothing drawn" further down the pipeline.
            static const bool s_logXfer = std::getenv("PS2X_XFER_LOG") != nullptr;
            if (s_logXfer && !xferCompletionLogged && transferBytes != 0 && uploadedBytes >= transferBytes)
            {
                xferCompletionLogged = true;
                std::fprintf(stderr, "[XFER] complete dbp=0x%x %ux%u px=%u bytes=%zu\n",
                             unsigned((bitblt >> 32) & 0x3FFFu), unsigned(trxreg & 0xFFFu),
                             unsigned((trxreg >> 32) & 0xFFFu), transfer.totalPixels, transferBytes);
            }
        }

        // Called from the post-packet hook: prints each distinct 4-bit texture state once, which is where those
        // states actually exist. The byte-threshold register dump rarely lands on one.
        void dumpT4TextureOnce()
        {
            static const bool s_enabled = std::getenv("PS2X_T4_DUMP") != nullptr;
            if (!s_enabled)
                return;
            static uint64_t s_seen[8] = {};
            static unsigned s_n = 0u;
            if (s_n >= 8u)
                return;
            const auto &rs = gs.get_register_state();
            for (unsigned ci = 0; ci < 2u; ++ci)
            {
                const uint64_t t0 = rs.ctx[ci].tex0.bits;
                const unsigned psm = unsigned((t0 >> 20) & 0x3Fu);
                if (psm != 0x14u && psm != 0x24u && psm != 0x2Cu) // T4, T4HL, T4HH
                    continue;
                const uint64_t key = (t0 & 0x3FFFu) | (static_cast<uint64_t>((t0 >> 37) & 0x3FFFu) << 16) |
                                     (static_cast<uint64_t>(psm) << 32) | (static_cast<uint64_t>(ci) << 40);
                bool duplicate = false;
                for (unsigned i = 0; i < s_n; ++i)
                    duplicate = duplicate || (s_seen[i] == key);
                if (duplicate)
                    continue;
                s_seen[s_n++] = key;
                dumpT4Texture(ci, t0, rs.texa.bits);
            }
        }

        // PS2X_TEXA_LOG=1: what the sampler actually holds (parallel-gs's own register state, not our parse) the first
        // few times it changes. Answers "does TEXA reach the sampler" without needing a particular game phase.
        void logSamplerState()
        {
            static const bool s_enabled = std::getenv("PS2X_TEXA_LOG") != nullptr;
            if (!s_enabled)
                return;
            const auto &rs = gs.get_register_state();
            const uint64_t t0 = rs.ctx[0].tex0.bits, t1 = rs.ctx[1].tex0.bits, tx = rs.texa.bits;
            static uint64_t s_lastKey = ~0ull;
            static uint64_t s_logged = 0u;
            const uint64_t key = t0 ^ (t1 * 31ull) ^ (tx * 131ull);
            if (key == s_lastKey || s_logged >= 16u)
                return;
            s_lastKey = key;
            ++s_logged;
            std::fprintf(stderr,
                         "[GS-texa] #%llu TEXA=0x%llx ta0=0x%llx aem=%llu | ctx0{psm=0x%llx tcc=%llu cbp=0x%llx} "
                         "ctx1{psm=0x%llx tcc=%llu cbp=0x%llx}\n",
                         static_cast<unsigned long long>(s_logged), static_cast<unsigned long long>(tx),
                         static_cast<unsigned long long>(tx & 0xFFu), static_cast<unsigned long long>((tx >> 15) & 1u),
                         static_cast<unsigned long long>((t0 >> 20) & 0x3Fu),
                         static_cast<unsigned long long>((t0 >> 34) & 1u),
                         static_cast<unsigned long long>((t0 >> 37) & 0x3FFFu),
                         static_cast<unsigned long long>((t1 >> 20) & 0x3Fu),
                         static_cast<unsigned long long>((t1 >> 34) & 1u),
                         static_cast<unsigned long long>((t1 >> 37) & 0x3FFFu));
        }

        // TEX0's TCC bit is meaningless for PSMCT24 (psm 0x01): the format has no alpha channel, so the GS takes the
        // sampled alpha from TEXA.TA0. A sampler that reads "texture alpha" gets 0 instead, and with ATE=1 /
        // ATST=GREATER / AREF=0x20 (Tales of Legendia's logo sprites) that rejects every pixel - the black phase.
        // Applied to the register after the packet is dispatched, which is path independent (packed A+D, reglist or
        // DIRECT), unlike a byte patch inside one decoder branch.
        void fixCt24Alpha()
        {
            // Opt-in: this guard provably never fires on Tales of Legendia (no TEX0 is CT24-with-TCC-set), and it
            // reads the whole register state on every packet, so it stays off unless asked for.
            static const bool s_enabled = std::getenv("PS2X_CT24_FIX") != nullptr;
            if (!s_enabled)
                return;
            const auto &rs = gs.get_register_state();
            for (unsigned ci = 0; ci < 2u; ++ci)
            {
                const uint64_t t0 = rs.ctx[ci].tex0.bits;
                if ((t0 & kTex0TccBit) == 0u || ((t0 >> 20) & 0x3Fu) != 0x01u)
                    continue;
                gs.write_register(static_cast<ParallelGS::RegisterAddr>(ci == 0u ? 0x06u : 0x07u),
                                  t0 & ~kTex0TccBit);
                static uint64_t s_patched = 0u;
                if (s_patched < 3u)
                    std::cerr << "[GS-tex24] CT24 ctx" << ci
                              << " TCC cleared -> alpha from TEXA.TA0 (n=" << ++s_patched << ")" << std::endl;
            }
        }

        // PS2X_T4_DUMP=1: a 4-bit texture and its palette, read out of raw GS memory and decoded with our own page
        // tables - not through the psm-aware accessor, whose 4-bit read returned all zeros. Prints the index
        // histogram next to each palette entry's alpha, so "the sprite samples the transparent half" becomes a
        // number instead of a hypothesis. Entry indices 8-15 of the ToL palette are zero (alpha 0); if the sprite's
        // texels land there an ATE test with AREF=0x20 rejects every pixel.
        void dumpT4Texture(unsigned ci, uint64_t tex0bits, uint64_t texaBits)
        {
            const unsigned psm = unsigned((tex0bits >> 20) & 0x3Fu);
            if (psm != 0x14u && psm != 0x24u && psm != 0x2Cu) // T4, T4HL, T4HH
                return;
            const unsigned tbp = unsigned(tex0bits & 0x3FFFu), tbw = unsigned((tex0bits >> 14) & 0x3Fu);
            const unsigned tw = 1u << unsigned((tex0bits >> 26) & 0xFu);
            const unsigned th = 1u << unsigned((tex0bits >> 30) & 0xFu);
            const unsigned cbp = unsigned((tex0bits >> 37) & 0x3FFFu), cpsm = unsigned((tex0bits >> 51) & 0xFu);
            const unsigned csm = unsigned((tex0bits >> 55) & 1u), csa = unsigned((tex0bits >> 56) & 0x1Fu);
            const auto *vram = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
            if (vram == nullptr)
                return;
            static GSMem::TexturePageCache cache;
            unsigned hist[16] = {};
            unsigned long sampledZeroAlpha = 0u, sampled = 0u;
            for (unsigned y = 0; y < th; ++y)
            {
                for (unsigned x = 0; x < tw; ++x)
                {
                    const unsigned idx = unsigned(GSMem::ReadTexture(cache, vram, psm, tbp, tbw, x, y) & 0xFFu);
                    if (idx < 16u)
                        ++hist[idx];
                    ++sampled;
                }
            }
            std::fprintf(stderr,
                         "\n  ctx%u T4DUMP psm=0x%x tbp=0x%x bw=%u %ux%u cbp=0x%x cpsm=0x%x csm=%u csa=%u texa=0x%llx\n",
                         ci, psm, tbp, tbw, tw, th, cbp, cpsm, csm, csa, static_cast<unsigned long long>(texaBits));
            for (unsigned i = 0; i < 16u; ++i)
            {
                const unsigned entryIndex = (csm != 0u) ? (csa * 16u + i) : i;
                uint32_t entry = 0u;
                if (cpsm == 0u)
                    std::memcpy(&entry, vram + cbp * 256u + entryIndex * 4u, sizeof(entry));
                const unsigned alpha = entry >> 24;
                if (hist[i] != 0u && alpha <= 0x20u)
                    sampledZeroAlpha += hist[i];
                if (hist[i] != 0u || entry != 0u)
                    std::fprintf(stderr, "    idx %2u used=%-6u alpha=0x%02x entry=0x%08x%s\n", i, hist[i], alpha, entry,
                                 (hist[i] != 0u && alpha <= 0x20u) ? "  <= sampled, alpha <= AREF 0x20" : "");
            }
            std::fprintf(stderr, "    sampled=%lu rejectedByAlphaTest=%lu\n", sampled, sampledZeroAlpha);
        }

        // PS2X_WATCH_PAGE=<hex byte address> (default 0x3FF000): hash one 8 KB GS page in VRAM after each packet and
        // report when it changes. Answers "does the guest ever upload the texture the logo sprites sample" without
        // trusting our register mirror - BITBLTBUF is not seen on every path, so PS2X_IMAGE_DUMP can stay silent
        // while an upload really happens. Throttled to every 8th packet after the first 64; prints at most 8 changes.
        void watchPage()
        {
            static const char *const s_env = std::getenv("PS2X_WATCH_PAGE");
            if (s_env == nullptr)
                return;
            static const uint32_t s_base =
                (s_env[0] != '\0') ? static_cast<uint32_t>(std::strtoul(s_env, nullptr, 16)) : 0x3FF000u;
            static uint64_t s_checks = 0u;
            static uint64_t s_lastHash = ~0ull;
            static uint64_t s_prints = 0u;
            ++s_checks;
            if (s_checks > 32u && (s_checks % 4096u) != 0u)
                return;
            const auto *vram = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
            if (vram == nullptr || static_cast<uint64_t>(s_base) >= VramSize)
                return;
            const uint32_t watchBytes = std::min<uint32_t>(8192u, static_cast<uint32_t>(VramSize - s_base));
            const uint8_t *page = vram + s_base;
            uint64_t hash = 1469598103934665603ull;
            uint32_t nonZero = 0u;
            for (uint32_t i = 0u; i < watchBytes; ++i)
            {
                hash ^= page[i];
                hash *= 1099511628211ull;
                if (page[i] != 0u)
                    ++nonZero;
            }
            if (hash == s_lastHash)
                return;
            const bool first = s_lastHash == ~0ull;
            s_lastHash = hash;
            if (s_prints++ >= 32u)
                return;
            std::fprintf(stderr, "[GS-page] %s base=0x%x nonZeroBytes=%u hash=0x%016llx check=%llu\n",
                         first ? "initial" : "changed", s_base, nonZero, static_cast<unsigned long long>(hash),
                         static_cast<unsigned long long>(s_checks));
        }

        // PS2X_UPLOAD_STATS=1: dir=0 (host->local) transfers accounted per destination DBP: how many started, how many
        // received their whole payload, and which one is outstanding. Separates "we drop the payload" from "the guest
        // never sends this texture" - the page watch showed 0x3FF000 is never written, and the census showed
        // transfers with up=0 of need=N.
        struct UploadStats
        {
            uint32_t started = 0u, completed = 0u;
            uint64_t bytesStarted = 0u, bytesReceived = 0u;
        };
        std::map<uint32_t, UploadStats> uploads;
        uint32_t uploadDbp = 0u;
        size_t uploadNeed = 0u, uploadGot = 0u;

        void noteUploadStart(uint32_t dbp, size_t bytes)
        {
            uploadDbp = dbp;
            uploadNeed = bytes;
            uploadGot = 0u;
            UploadStats &s = uploads[dbp];
            ++s.started;
            s.bytesStarted += bytes;
        }
        void noteUploadBytes(size_t bytes)
        {
            uploadGot = std::min(uploadNeed, uploadGot + bytes);
            const auto it = uploads.find(uploadDbp);
            if (it == uploads.end())
                return;
            it->second.bytesReceived += bytes;
            if (uploadNeed != 0u && uploadGot >= uploadNeed && it->second.completed < it->second.started)
                ++it->second.completed;
        }
        // PS2X_PRESENT_FROM_CONTEXT=1: keep DISPFB2 pointing at the buffer the game draws into. Tales of Legendia
        // writes its frame context to FBP 224 and leaves DISPFB2 at FBP 0 without ever rewriting it, so nothing it
        // draws after the loading screen would be scanned out - the same situation the frontend's
        // "preferred display source" heuristic exists for, forced instead of inferred. Field layout comes from
        // parallel-gs: FRAME{FBP 0-8, FBW 16-21, PSM 24-29} -> DISPFB{FBP 0-8, FBW 9-14, PSM 15-19}, DBX/DBY kept.
        void followContextFrame()
        {
            static const bool s_enabled = std::getenv("PS2X_PRESENT_FROM_CONTEXT") != nullptr;
            if (!s_enabled)
                return;
            const auto &rs = gs.get_register_state();
            const uint64_t frame = rs.ctx[0].frame.bits;
            const auto &dfb = gs.get_priv_register_state().dispfb2;
            const uint64_t dispfb = static_cast<uint64_t>(dfb.FBP) | (static_cast<uint64_t>(dfb.FBW) << 9) |
                                    (static_cast<uint64_t>(dfb.PSM) << 15) | (static_cast<uint64_t>(dfb.DBX) << 32) |
                                    (static_cast<uint64_t>(dfb.DBY) << 43);
            const uint64_t wanted = (dispfb & ~static_cast<uint64_t>(0x7FFFFull)) | (frame & 0x1FFull) |
                                    (((frame >> 16) & 0x3Full) << 9) | (((frame >> 24) & 0x1Full) << 15);
            if (wanted == dispfb)
                return;
            gs.write_register(static_cast<ParallelGS::RegisterAddr>(0x5Au), wanted); // DISPFB2
            static uint32_t s_prints = 0u;
            if (s_prints++ < 4u)
                std::fprintf(stderr, "[GS-present] DISPFB2 0x%llx -> 0x%llx (following ctx0 FRAME 0x%llx: fbp=%llu)\n",
                             static_cast<unsigned long long>(dispfb), static_cast<unsigned long long>(wanted),
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(frame & 0x1FFull));
        }

        // PS2X_VRAM_SCAN=1: search all of VRAM for the logo palette's first entry (0x8079a9d7, seen in the T4 dump at
        // cbp=0x3a70) and report where it actually lives. Content-based, so it cannot be fooled by either transfer
        // state machine: if the palette is only at 0x3A700 then uploads DO land and the page watch was misreading; if
        // it is at 0x3FF400 the sprite samples the wrong base; if it is absent the upload really is dropped.
        void scanVramForPalette()
        {
            static const bool s_enabled = std::getenv("PS2X_VRAM_SCAN") != nullptr;
            if (!s_enabled)
                return;
            const auto *vram = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize));
            if (vram == nullptr)
                return;
            uint32_t hits = 0u, firstOffsets[8] = {};
            for (uint32_t off = 0u; off + 4u <= VramSize; off += 4u)
            {
                uint32_t w = 0u;
                std::memcpy(&w, vram + off, sizeof(w));
                if (w == 0x8079a9d7u)
                {
                    if (hits < 8u)
                        firstOffsets[hits] = off;
                    ++hits;
                }
            }
            std::fprintf(stderr, "[GS-vramscan] paletteEntry0 hits=%u at", hits);
            for (uint32_t i = 0u; i < hits && i < 8u; ++i)
                std::fprintf(stderr, " 0x%x", firstOffsets[i]);
            std::fprintf(stderr, "  (cbp*256=0x3a700 expected, sprite tbp=0x3ff000)\n");
        }

        void printUploadStats(const char *when)
        {
            static const bool s_enabled = std::getenv("PS2X_UPLOAD_STATS") != nullptr;
            if (!s_enabled)
                return;
            std::fprintf(stderr, "[GS-upload] %s: %zu destination(s), outstanding dbp=0x%x got=%zu/%zu\n", when,
                         uploads.size(), uploadDbp, uploadGot, uploadNeed);
            for (const auto &kv : uploads)
            {
                std::fprintf(stderr, "[GS-upload]   dbp=0x%x started=%u completed=%u bytes=%llu/%llu%s\n", kv.first,
                             kv.second.started, kv.second.completed,
                             static_cast<unsigned long long>(kv.second.bytesReceived),
                             static_cast<unsigned long long>(kv.second.bytesStarted),
                             (kv.second.completed < kv.second.started) ? "   <= STARTED, NEVER COMPLETED" : "");
            }
        }

        // PS2X_DBP_STATS=1 (session 58m): how many payload bytes the guest actually puts on the wire for each
        // BITBLTBUF destination.
        //
        // Every earlier counter was wrong for a structural reason, not because of a typo:
        //   - PS2X_UPLOAD_STATS sizes a transfer from TRXREG *at TRXDIR time* and defaults `direction` to 0, so a
        //     TRXDIR written before TRXREG leaves need=0 while payload keeps piling up ("904 started, 1 MB, 2 done");
        //   - PS2X_IMAGE_DUMP and bitbltMirror() are fed from the PACKED A+D branch only.
        // The decode itself is NOT reimplemented here: this counts on the same walk observeGIF already performs, which
        // the [GS-census] line proves correct every 256 KiB (its ourFLG/ourNLOOP/ourLoop track parallel-gs's own
        // upFLG/upNLOOP/upLoop). A second, independent tag walker was tried first and desynced - do not add one back.
        //
        // Three mutually exclusive answers per destination:
        //   delivered == declared, VRAM still empty -> parallel-gs drops what we forwarded
        //   delivered <  declared                   -> our forwarding truncates the packet
        //   set up but no IMAGE tag at all           -> the stall is upstream of the GS
        // A 32x16 T4 sprite needs 512 px * 4 bits = 256 B (16 qw); a 64x64 one needs 2048 B (128 qw).
        struct RawDbp
        {
            uint64_t bitbltWrites = 0u;                  // BITBLTBUF writes naming this destination
            uint64_t dir0 = 0u, dir1 = 0u, dir2 = 0u;    // TRXDIR writes while this destination was current
            uint64_t imageTags = 0u;                     // IMAGE tags attributed to this destination
            uint64_t declaredQw = 0u;                    // sum of those tags' NLOOP
            uint64_t deliveredQw = 0u;                   // qwords actually present in the stream
            uint64_t hwregQw = 0u;                       // qwords delivered as HWREG (0x51) writes
            uint64_t maxTagQw = 0u;                      // largest single tag
            uint64_t nzBytes = 0u;                       // non-zero payload bytes (bounded scan)
            uint64_t scannedBytes = 0u;
            uint32_t lastW = 0u, lastH = 0u;             // last TRXREG rectangle
        };
        std::map<uint32_t, RawDbp> m_rawDbp;
        uint32_t m_rawDbpCurrent = 0xFFFFFFFFu;          // DBP named by the last BITBLTBUF
        bool m_rawHaveDbp = false;
        uint64_t m_rawBytes = 0u;
        uint32_t m_rawPrints = 0u;
        uint32_t m_rawBltPrints = 0u;
        uint32_t m_traceLeft = 0u;                       // bounded transfer trace, armed by the first 0x3FF0 setup
        bool m_traceArmed = false;
        uint32_t m_imgPrints = 0u;
        uint32_t m_rawRectW = 0u, m_rawRectH = 0u, m_rawDir = 0xFFFFFFFFu;
        uint32_t m_hwregPrints = 0u;
        uint32_t m_hwregDataPrints = 0u;
        uint32_t m_hwregSinceBlt = 0u;
        uint32_t m_tagCtxPrints = 0u;
        bool m_dumpPkt = false;
        bool m_dumpPktDone = false;
        bool m_dumpBad = false;
        bool m_dumpBadDone = false;
        size_t m_dumpBadAt = 0u;
        uint32_t m_rawDpsm = 0u;
        uint32_t m_xferPrints = 0u;
        uint32_t m_rawXferDbp = 0xFFFFFFFFu;
        uint32_t m_rawXferW = 0u, m_rawXferH = 0u, m_rawXferPsm = 0u;
        uint32_t m_rawXferRequired = 0u, m_rawXferGot = 0u;
        bool m_rawXferActive = false;
        uint32_t m_rawLatchedDbp = 0xFFFFFFFFu;
        bool m_rawLatchedActive = false;
        const bool m_rawEnabled = (std::getenv("PS2X_DBP_STATS") != nullptr);

        // Fed from the same points observeGIF decodes, so the destination is never inferred from a stale mirror.
        void rawDbpRegister(uint8_t reg, uint64_t value)
        {
            switch (reg)
            {
            case 0x50u: // BITBLTBUF
            {
                const uint32_t dbp = uint32_t((value >> 32) & 0x3FFFu);
                m_rawDpsm = uint32_t((value >> 56) & 0x3Fu);
                m_rawDbpCurrent = dbp;
                m_rawHaveDbp = true;
                ++m_rawDbp[dbp].bitbltWrites;
                if (m_rawBltPrints < 32u)
                {
                    ++m_rawBltPrints;
                    std::fprintf(stderr,
                                 "[GS-blt] #%u dbp=0x%04x (0x%06x) dbw=%u dpsm=0x%02x | sbp=0x%04x sbw=%u spsm=0x%02x\n",
                                 m_rawBltPrints, dbp, dbp * 256u, unsigned((value >> 48) & 0x3Fu),
                                 unsigned((value >> 56) & 0x3Fu), unsigned(value & 0x3FFFu),
                                 unsigned((value >> 16) & 0x3Fu), unsigned((value >> 24) & 0x3Fu));
                }
                // Arm the bounded trace on the first T4 setup for the page the logo sprites sample, so the log shows
                // the exact register/payload order for one real texture transfer instead of 500 retries.
                if (m_traceLeft == 0u && dbp == 0x3FF0u && !m_traceArmed)
                {
                    m_traceArmed = true;
                    m_traceLeft = 40u;
                    std::fprintf(stderr, "[GS-xtr] armed at the first DBP=0x3FF0 (T4) setup\n");
                }
                if (m_traceLeft != 0u)
                {
                    --m_traceLeft;
                    std::fprintf(stderr, "[GS-xtr] BITBLTBUF dbp=0x%04x dbw=%u dpsm=0x%02x\n", dbp,
                                 unsigned((value >> 48) & 0x3Fu), unsigned((value >> 56) & 0x3Fu));
                }
                // Session 58q: PCSX2 shows the guest writes a *complete* 1248-byte transfer here
                // ([tag NLOOP=4][BITBLTBUF 0x3ff0 T4][TRXREG 48x48][HWREG][TRXDIR 0][tag IMAGE NLOOP=72]).
                // We read NLOOP=0x7fff instead, and parallel-gs agrees, so the bytes we forward are already
                // wrong. Ask for a hex dump of the whole packet that carried the first 0x3ff0 setup.
                if (dbp == 0x3FF0u && !m_dumpPktDone)
                    m_dumpPkt = true;
                break;
            }
            case 0x51u: // HWREG - the A word IS 8 bytes of image data. This is the *other* legal way to feed
                        // a host->local transfer (a PACKED A+D 0x51 write), and a ledger that only counts
                        // IMAGE tags reports "no payload" for a texture that arrived entirely this way.
                        // Attribution is to the destination LATCHED AT TRXDIR, not the last BITBLTBUF: the guest
                        // writes BITBLTBUF between individual qwords inside one long packed tag, and on the GS a
                        // BITBLTBUF write does not retarget an already-started transfer.
                if (m_rawLatchedActive)
                {
                    RawDbp &d = m_rawDbp[m_rawLatchedDbp];
                    ++d.hwregQw;
                    ++d.deliveredQw;
                    ++m_hwregSinceBlt;
                    ++m_rawXferGot;
                    if (d.scannedBytes < 1024u)
                    {
                        const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
                        for (uint32_t i = 0u; i < 8u; ++i)
                        {
                            if (bytes[i] != 0u)
                                ++d.nzBytes;
                        }
                        d.scannedBytes += 8u;
                    }
                    if (m_hwregDataPrints < 8u)
                    {
                        ++m_hwregDataPrints;
                        std::fprintf(stderr, "[GS-hwreg] #%u dbp=0x%04x data=0x%016llx\n", m_hwregDataPrints,
                                     m_rawDbpCurrent, static_cast<unsigned long long>(value));
                    }
                }
                break;
            case 0x52u: // TRXREG
                if (m_rawHaveDbp)
                {
                    RawDbp &d = m_rawDbp[m_rawDbpCurrent];
                    d.lastW = uint32_t(value & 0xFFFu);
                    d.lastH = uint32_t((value >> 32) & 0xFFFu);
                    m_rawRectW = d.lastW;
                    m_rawRectH = d.lastH;
                    if (m_traceLeft != 0u)
                    {
                        --m_traceLeft;
                        std::fprintf(stderr, "[GS-xtr] TRXREG dbp=0x%04x rect=%ux%u\n", m_rawDbpCurrent, d.lastW,
                                     d.lastH);
                    }
                }
                break;
            case 0x53u: // TRXDIR - this is what latches the transfer in the GS
                if (m_rawHaveDbp)
                {
                    // Session 58p, the decisive measurement: a transfer is latched at TRXDIR, so group by TRXDIR,
                    // NOT by BITBLTBUF. Report what the previous latched transfer received against the same
                    // required_qwords parallel-gs computes (RRW*RRH*bpp/64) before it will flush anything.
                    if (m_rawXferActive && m_rawXferDbp == 0x3FF0u && m_xferPrints < 24u)
                    {
                        ++m_xferPrints;
                        const bool enough = m_rawXferGot >= m_rawXferRequired;
                        std::fprintf(stderr,
                                     "[GS-xferq] dbp=0x3ff0 latched rect=%ux%u psm=0x%02x required=%u qw "
                                     "received=%u qw (%u B)%s\n",
                                     m_rawXferW, m_rawXferH, m_rawXferPsm, m_rawXferRequired, m_rawXferGot,
                                     m_rawXferGot * 16u,
                                     enough ? "  <= ENOUGH: backend had the data and did not write it"
                                            : "  <= SHORT: guest starved the transfer");
                    }
                    m_rawXferDbp = m_rawDbpCurrent;
                    m_rawXferW = m_rawRectW;
                    m_rawXferH = m_rawRectH;
                    m_rawXferPsm = m_rawDpsm;
                    m_rawXferRequired =
                        (m_rawRectW * m_rawRectH * bitsPerPixel(m_rawDpsm)) / 64u;
                    m_rawXferGot = 0u;
                    m_rawXferActive = true;

                    m_rawLatchedDbp = m_rawDbpCurrent;
                    m_rawLatchedActive = true;
                    RawDbp &d = m_rawDbp[m_rawDbpCurrent];
                    const uint32_t dir = uint32_t(value & 3u);
                    m_rawDir = dir;
                    if (dir == 0u)
                        ++d.dir0;
                    else if (dir == 1u)
                        ++d.dir1;
                    else if (dir == 2u)
                        ++d.dir2;
                    if (m_traceLeft != 0u)
                    {
                        --m_traceLeft;
                        std::fprintf(stderr, "[GS-xtr] TRXDIR dbp=0x%04x dir=%u\n", m_rawDbpCurrent, dir);
                    }
                }
                break;
            default: break;
            }
        }

        // One IMAGE tag chunk: NLOOP qwords of payload, attributed to the destination in force. `tagStart` is true for
        // the first chunk only, so the tag itself is counted once even when it spans many DMA packets.
        void rawDbpImage(uint64_t nloop, size_t deliveredBytes, const uint8_t *payload, bool tagStart)
        {
            if (!m_rawHaveDbp)
                return;
            RawDbp &d = m_rawDbp[m_rawDbpCurrent];
            if (tagStart)
            {
                ++d.imageTags;
                d.declaredQw += nloop;
                if (nloop > d.maxTagQw)
                    d.maxTagQw = nloop;
                // First 24 IMAGE tags: the true BITBLTBUF/IMAGE pairing, so "which destination got the big payload"
                // never rests on a stale mirror.
                if (m_imgPrints < 24u)
                {
                    ++m_imgPrints;
                    std::fprintf(stderr, "[GS-img] #%u dbp=0x%04x nloop=%llu qw (%llu B) rect=%ux%u dir=%u\n",
                                 m_imgPrints, m_rawDbpCurrent, static_cast<unsigned long long>(nloop),
                                 static_cast<unsigned long long>(nloop * 16u), m_rawRectW, m_rawRectH,
                                 m_rawDir);
                }
            }
            d.deliveredQw += deliveredBytes / 16u;
            if (m_traceLeft != 0u)
            {
                --m_traceLeft;
                std::fprintf(stderr, "[GS-xtr] IMAGE chunk dbp=0x%04x nloop=%llu this=%zu bytes (tagStart=%u)\n",
                             m_rawDbpCurrent, static_cast<unsigned long long>(nloop), deliveredBytes,
                             tagStart ? 1u : 0u);
            }
            // Content check, bounded: the first 1 KiB of payload per destination, once.
            if (payload != nullptr && d.scannedBytes < 1024u)
            {
                const size_t scan = std::min<size_t>(deliveredBytes, 1024u - size_t(d.scannedBytes));
                for (size_t i = 0u; i < scan; ++i)
                {
                    if (payload[i] != 0u)
                        ++d.nzBytes;
                }
                d.scannedBytes += scan;
            }
        }

        // Session 58s: packet-boundary history. The bogus NLOOP=0x7FFF tag is read because the backend ENTERS a
        // packet with an open IMAGE tag one qword short, so the boundary that went wrong is the PREVIOUS packet.
        // Keep the last 32 packet boundaries and dump them alongside the bad tag.
        struct PktHist
        {
            uint32_t path = 0u, size = 0u;
            uint32_t entryLoop = 0u, entryNloop = 0u, entryFlg = 0u, entryReg = 0u;
            uint32_t exitLoop = 0u, exitNloop = 0u, exitFlg = 0u, exitReg = 0u;
        };
        PktHist m_pktHist[32];
        uint32_t m_pktHistCount = 0u;
        bool m_pktHistDumped = false;
        // Bytes of the two packets before the current one, so the bad tag can be shown in its true context.
        uint8_t m_prevPkt[2][192] = {};
        uint32_t m_prevPktSize[2] = {0u, 0u};
        uint32_t m_prevPktPath[2] = {0u, 0u};
        uint32_t m_prevPktSlot = 0u;

        void recordPacket(uint32_t path, uint32_t size, bool entry)
        {
            const auto &gifPath = gs.get_gif_path(path & 3u);
            if (entry)
            {
                m_pktHist[m_pktHistCount % 32u] = PktHist{};
                PktHist &h = m_pktHist[m_pktHistCount % 32u];
                h.path = path;
                h.size = size;
                h.entryLoop = gifPath.loop;
                h.entryNloop = gifPath.tag.NLOOP;
                h.entryFlg = gifPath.tag.FLG;
                h.entryReg = gifPath.reg;
            }
            else
            {
                PktHist &h = m_pktHist[m_pktHistCount % 32u];
                h.exitLoop = gifPath.loop;
                h.exitNloop = gifPath.tag.NLOOP;
                h.exitFlg = gifPath.tag.FLG;
                h.exitReg = gifPath.reg;
                ++m_pktHistCount;
            }
        }

        void dumpPacketHistory()
        {
            if (m_pktHistDumped || m_pktHistCount == 0u)
                return;
            m_pktHistDumped = true;
            const uint32_t first = m_pktHistCount > 32u ? m_pktHistCount - 32u : 0u;
            std::fprintf(stderr, "[GS-hist] last %u packet boundaries before the bad tag:\n",
                         m_pktHistCount - first);
            for (uint32_t n = first; n < m_pktHistCount; ++n)
            {
                const PktHist &h = m_pktHist[n % 32u];
                std::fprintf(stderr,
                             "[GS-hist] p%u size=%u  entry(flg=%u nloop=%u loop=%u reg=%u) -> "
                             "exit(flg=%u nloop=%u loop=%u reg=%u)%s\n",
                             h.path, h.size, h.entryFlg, h.entryNloop, h.entryLoop, h.entryReg, h.exitFlg,
                             h.exitNloop, h.exitLoop, h.exitReg,
                             (h.exitFlg >= 2u && h.exitLoop < h.exitNloop) ? "   <= exited MID-IMAGE-TAG" : "");
            }
        }

        void printRawDbpStats(const char *when)
        {
            if (!m_rawEnabled)
                return;
            std::fprintf(stderr, "[GS-dbp] %s gifBytes=%llu destinations=%zu current=0x%04x\n", when,
                         static_cast<unsigned long long>(m_rawBytes), m_rawDbp.size(),
                         m_rawHaveDbp ? m_rawDbpCurrent : 0xFFFFFFFFu);
            // The page watch's 8 KiB window at 0x3FF000 also contains the CLUT at 0x3FF400, so "0x3FF000 has 44
            // non-zero bytes" has never distinguished the texture page from its palette. Break the window into
            // 256-byte pages: 0x3FF000..0x3FF3FF is the texture, 0x3FF400.. is the CLUT.
            if (const auto *vram = static_cast<const uint8_t *>(gs.map_vram_read(0, VramSize)))
            {
                // Session 58w: also dump the page-aligned 0x3FE000 destination, so PS2X_FORCE_TBP can be judged.
                std::fprintf(stderr, "[GS-dbp]   pages 0x3fe000 (32 x 256B, page-aligned target):");
                for (uint32_t pg = 0u; pg < 32u; ++pg)
                {
                    uint32_t nz = 0u;
                    const uint32_t base = 0x3FE000u + pg * 256u;
                    if (base + 256u <= VramSize)
                    {
                        for (uint32_t i = 0u; i < 256u; ++i)
                        {
                            if (vram[base + i] != 0u)
                                ++nz;
                        }
                    }
                    std::fprintf(stderr, " %u", nz);
                }
                std::fputc('\n', stderr);
                std::fprintf(stderr, "[GS-dbp]   pages 0x3ff000:");
                for (uint32_t pg = 0u; pg < 8u; ++pg)
                {
                    uint32_t nz = 0u;
                    const uint32_t base = 0x3FF000u + pg * 256u;
                    if (base + 256u <= VramSize)
                    {
                        for (uint32_t i = 0u; i < 256u; ++i)
                        {
                            if (vram[base + i] != 0u)
                                ++nz;
                        }
                    }
                    std::fprintf(stderr, " %u", nz);
                }
                std::fputc('\n', stderr);
            }
            for (const auto &kv : m_rawDbp)
            {
                const RawDbp &d = kv.second;
                const uint64_t bytes = d.deliveredQw * 16u;
                // "set up but no payload" is the discriminator: the guest names the destination and starts a
                // host->local transfer, then nothing arrives. "never set up" puts the stall upstream of the GS.
                const char *verdict = "";
                if (d.bitbltWrites != 0u && d.deliveredQw == 0u)
                    verdict = "   <= SET UP, NO PAYLOAD AT ALL (no IMAGE tag, no HWREG)";
                else if (d.imageTags == 0u && d.hwregQw != 0u)
                    verdict = "   <= fed entirely by HWREG writes";
                else if (bytes < 256u)
                    verdict = "   <= below one 32x16 T4 (256 B)";
                std::fprintf(stderr,
                             "[GS-dbp]   dbp=0x%04x (0x%06x) blt=%llu dir=%llu/%llu/%llu rect=%ux%u tags=%llu "
                             "declared=%llu qw delivered=%llu qw max=%llu qw hwreg=%llu qw bytes=%llu nz=%llu/%llu%s\n",
                             kv.first, kv.first * 256u, static_cast<unsigned long long>(d.bitbltWrites),
                             static_cast<unsigned long long>(d.dir0), static_cast<unsigned long long>(d.dir1),
                             static_cast<unsigned long long>(d.dir2), d.lastW, d.lastH,
                             static_cast<unsigned long long>(d.imageTags),
                             static_cast<unsigned long long>(d.declaredQw),
                             static_cast<unsigned long long>(d.deliveredQw),
                             static_cast<unsigned long long>(d.maxTagQw),
                             static_cast<unsigned long long>(d.hwregQw),
                             static_cast<unsigned long long>(bytes),
                             static_cast<unsigned long long>(d.nzBytes),
                             static_cast<unsigned long long>(d.scannedBytes), verdict);
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
            static uint64_t s_zeroNloop[4] = {0u, 0u, 0u, 0u}; // tags seen with NLOOP == 0, by FLG
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
    static uint64_t s_adTexFlush = 0u; // TEXFLUSH (0x3F) - required after a texture upload
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
                case 0x3F: ++s_adTexFlush; break;                            // TEXFLUSH
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
                    // Session 58r: arm the byte dump at the moment this tag HEADER is read. PCSX2 contains no
                    // NLOOP=0x7FFF tag in either upper-word variant, so whichever packet reads one is where the
                    // stream first goes wrong - and it is not the packet where the tag later becomes "active".
                    if (state.tag.NLOOP == 0x7FFFu && state.tag.FLG >= 2u && !m_dumpBadDone && m_rawEnabled)
                    {
                        m_dumpBad = true;
                        m_dumpBadAt = offset; // offset still points at the 16-byte tag header here
                    }
                    // Session 58n: a texture upload the guest sets up and then never feeds would look exactly like
                    // this if the IMAGE tag it emits has NLOOP == 0 - the tag is consumed as a header with no payload,
                    // so every payload counter reports nothing. Count zero-length tags by FLG to tell "the guest sends
                    // an empty image packet" from "the guest sends no image packet at all".
                    if (state.tag.NLOOP == 0u)
                        ++s_zeroNloop[state.tag.FLG & 3u];
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
                    // Session 58r: PCSX2 contains NO NLOOP=0x7FFF tag anywhere, so the first one we see is the
                    // desync point. Capture the bytes around its tag header for a byte-level diff.
                    if (state.loop == 0u && state.tag.NLOOP == 0x7FFFu && !m_dumpBadDone && m_rawEnabled)
                    {
                        m_dumpBad = true;
                        m_dumpBadAt = offset; // payload starts here; the 16-byte tag header is at offset-16
                    }
                    observeImage(count * 16);
                    if (m_rawEnabled)
                        rawDbpImage(state.tag.NLOOP, count * 16u, data + offset, state.loop == 0u);
                    s_gifImageBytes += count * 16u;
                    state.loop += uint32_t(count);
                    offset += count * 16;
                }
                else if (state.tag.FLG == 1)
                {
                    // Packed-reglist: a loop is nreg A words then nreg D words, NOT interleaved, and each
                    // register word is 8 bytes (gs_frontend.cpp's own REGLIST path uses `offset += 8`; the GIF
                    // spec makes a loop nreg*8 + nreg*8 = 16*nreg bytes). Advancing 16 here consumed twice the
                    // real stride, so every REGLIST-sourced register value after the first loop was read from
                    // the wrong offset - that is what produced impossible rects like 0x12 in [GS-xferq].
                    // state.reg counts 0..2*nreg-1 across both phases so a loop straddling a DMA call stays correct.
                    ++s_gifDrawTags;
                    while (offset + 8u <= size && state.loop < state.tag.NLOOP)
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
                            // PS2X_DBP_STATS=1: in REGLIST the register address is the tag's REGS nibble, not the
                            // D word's low byte, so decode it from the descriptor the same way the frontend does.
                            if (m_rawEnabled)
                            {
                                const uint8_t rlReg = uint8_t((word(reinterpret_cast<const uint8_t *>(&state.tag) + 8) >>
                                                               (k * 4u)) &
                                                              0xFu);
                                if (rlReg >= 0x50u && rlReg <= 0x53u)
                                    rawDbpRegister(rlReg, aWord);
                            }
                            if ((dWord & 0xFFu) == 0x00u && wantPrimRaw())
                            {
                                takePrimRaw();
                                primRawDump("reglist", offset, s_reglistAOff[path & 3u][k], aWord);
                            }
                            ++state.reg;
                        }
                        offset += 8;
                        if (state.reg >= nreg * 2u)
                        {
                            state.reg = 0;
                            ++state.loop;
                        }
                    }
                    // A REGLIST tag whose loop is an odd number of register words carries one 8-byte pad.
                    if (state.loop >= state.tag.NLOOP && ((state.tag.NLOOP * nreg) & 1u) != 0u)
                        offset += 8u;
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
                        // PS2X_DBP_STATS=1: BITBLTBUF/TRXREG/TRXDIR from the same decode the census trusts.
                        if (m_rawEnabled && adReg >= 0x50u && adReg <= 0x53u)
                            rawDbpRegister(adReg, adValue);
                        // Session 58o: the tag that carries HWREG image data. If NLOOP is 12 and we log 12 of these,
                        // the guest really sends 12 qwords per transfer and the shortfall is downstream. If NLOOP is
                        // 1-4, the guest's own packet is short.
                        if (m_rawEnabled && adReg == 0x51u && m_tagCtxPrints < 24u)
                        {
                            ++m_tagCtxPrints;
                            std::fprintf(stderr,
                                         "[GS-hwtag] #%u dbp=0x%04x NLOOP=%u NREG=%u FLG=%u loop=%u reg=%u EOP=%u\n",
                                         m_tagCtxPrints, m_rawDbpCurrent, state.tag.NLOOP, state.tag.NREG,
                                         unsigned(state.tag.FLG), state.loop, state.reg, state.tag.EOP);
                        }
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
                    // Session 54: does a black-phase primitive rasterise at all?
                    if (adReg == 0x00u && forceNoTexEnabled())
                        m_scisPatch.push_back({offset, adValue & ~0x10ull}); // clear PRIM.TME
                    if (adReg == 0x01u && forceRgbaqEnabled())
                        m_scisPatch.push_back({offset, kBrightRgbaq});
                    // PS2X_FORCE_TBP=1: move the 4-bit texture to a page-aligned destination, in the upload
                    // (BITBLTBUF.DBP, bits 32-45) and in the sampler (TEX0.TBP0, bits 0-13).
                    if (forceTbpEnabled())
                    {
                        if (adReg == 0x50u && ((adValue >> 32) & 0x3FFFu) == kForceTbpFrom)
                        {
                            m_scisPatch.push_back(
                                {offset, (adValue & ~(0x3FFFull << 32)) | (static_cast<uint64_t>(kForceTbpTo) << 32)});
                            static uint32_t s_tbpBlts = 0u;
                            if (s_tbpBlts < 4u)
                            {
                                ++s_tbpBlts;
                                std::cerr << "[GS-tbp] BITBLTBUF dbp 0x" << std::hex << kForceTbpFrom << " -> 0x"
                                          << kForceTbpTo << std::dec << " (page-aligned)" << std::endl;
                            }
                        }
                        else if ((adReg == 0x06u || adReg == 0x07u) && (adValue & 0x3FFFu) == kForceTbpFrom)
                        {
                            m_scisPatch.push_back({offset, (adValue & ~0x3FFFull) | kForceTbpTo});
                            static uint32_t s_tbpTex = 0u;
                            if (s_tbpTex < 4u)
                            {
                                ++s_tbpTex;
                                std::cerr << "[GS-tbp] TEX0_" << (adReg == 0x06u ? 1 : 2) << " tbp0 0x" << std::hex
                                          << kForceTbpFrom << " -> 0x" << kForceTbpTo << std::dec << std::endl;
                            }
                        }
                    }
                    // Session 54: does the black-phase sprite sample a real texel?
                    if (uvDumpEnabled() && (adReg == 0x02u || adReg == 0x03u) &&
                        s_scissor1 == 0x100000001ull)
                    {
                        static uint32_t s_uvPrints = 0u;
                        static uint64_t s_uvSample[24] = {};
                        static uint32_t s_uvReg[24] = {};
                        if (s_uvPrints < 24u)
                        {
                            s_uvSample[s_uvPrints] = adValue;
                            s_uvReg[s_uvPrints] = adReg;
                            ++s_uvPrints;
                            if (s_uvPrints == 24u)
                            {
                                std::cerr << "[UV] black-phase samples (reg 0x02=ST, 0x03=UV):";
                                for (uint32_t i = 0u; i < 24u; ++i)
                                {
                                    const uint32_t u = uint32_t(s_uvSample[i]) & 0x3FFFu;
                                    const uint32_t v = uint32_t(s_uvSample[i] >> 16) & 0x3FFFu;
                                    std::cerr << " [" << std::hex << s_uvReg[i] << ":"
                                              << u << "," << v << std::dec << "]";
                                }
                                std::cerr << std::endl;
                            }
                        }
                    }
                    // Session 54: force a known non-zero sampled alpha.
                    if (forceAlphaEnabled())
                    {
                        if (adReg == 0x06u || adReg == 0x07u) // TEX0_1 / TEX0_2
                            m_scisPatch.push_back({offset, adValue & ~kTex0TccBit});
                        else if (adReg == 0x3Bu) // TEXA
                            m_scisPatch.push_back({offset, kTexaFixedAlpha});
                    }
                    // PS2X_FORCE_TEXA_ONLY=1: pin TEXA only, TCC untouched - the discriminator between "TEXA never
                    // reaches the sampler" and "the sampled alpha really is 0".
                    else if (forceTexaOnlyEnabled() && adReg == 0x3Bu)
                    {
                        m_scisPatch.push_back({offset, kTexaFixedAlpha});
                    }
                    // A 24-bit texture (PSMCT24, psm 0x01) stores no alpha channel: the GS takes the sampled alpha
                    // from TEXA.TA0 whatever TCC says. Sampling "texture alpha" instead yields 0, and with the
                    // alpha test the game uses (TEST=0x5120D: ATE=1, ATST=GREATER, AREF=0x20) every pixel is then
                    // rejected - that is the black phase: Tales of Legendia's logo sprites are prim=0x15E
                    // (sprite, tme, abe, ctxt=0) over a CT24 texture with TCC=1 and TEXA=0x80. The correction is
                    // applied after the packet is dispatched (see fixCt24AfterPacket) because TEX0 does not always
                    // arrive as a packed A+D pair.
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
                printUploadStats("census");
                scanVramForPalette();
                watchPage();
                std::cerr << "[GS-census] bytes=" << s_gifBytes << " tags=" << s_gifTags
                          << " imageBytes=" << s_gifImageBytes << " drawTags=" << s_gifDrawTags
                          << " dir=" << unsigned(transfer.direction) << " need=" << transferBytes
                          << " up=" << uploadedBytes << " px=" << transfer.copiedPixels << "/"
                          << transfer.totalPixels << " prim=" << s_adPrim << " xyz=" << s_adXyz
                          << " in=" << s_xyzIn << " out=" << s_xyzOut << " X=" << s_minX << ".."
                           << s_maxX << " Y=" << s_minY << ".." << s_maxY << " tex=" << s_adTex << " texflush=" << s_adTexFlush
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
                          << s_flg[3] << " zeroNLOOP=" << s_zeroNloop[0] << "/" << s_zeroNloop[1] << "/"
                           << s_zeroNloop[2] << "/" << s_zeroNloop[3] << " ourFLG=" << unsigned(state.tag.FLG) << " ourNLOOP="
                          << state.tag.NLOOP << " ourLoop=" << state.loop << " upFLG="
                          << unsigned(gs.get_gif_path(path).tag.FLG) << " upNLOOP="
                          << gs.get_gif_path(path).tag.NLOOP << " upLoop="
                          << gs.get_gif_path(path).loop << std::dec << std::endl;
            }

            // Session 58s: keep this packet's bytes so the next packet's diagnostics can show the boundary that
            // actually caused the inherited tag state.
            m_prevPktPath[m_prevPktSlot] = path;
            m_prevPktSize[m_prevPktSlot] = std::min<uint32_t>(size, static_cast<uint32_t>(sizeof(m_prevPkt[0])));
            if (data != nullptr && m_prevPktSize[m_prevPktSlot] != 0u)
                std::memcpy(m_prevPkt[m_prevPktSlot], data, m_prevPktSize[m_prevPktSlot]);
            m_prevPktSlot = (m_prevPktSlot + 1u) % 2u;
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
            ps2x::gifhop::count(5, path, size);
            ps2x::gifhop::reportIfDue();
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
            // If TCC is cleared but the guest never writes TEXA, the alpha would
            // default to 0 and the blend would still collapse to Cd.  Pin TEXA once.
            if (forceAlphaEnabled() || forceTexaOnlyEnabled())
            {
                static bool s_texaPinned = false;
                if (!s_texaPinned)
                {
                    s_texaPinned = true;
                    gs.write_register(static_cast<ParallelGS::RegisterAddr>(0x3Bu),
                                      kTexaFixedAlpha);
                }
            }
            observeGIF(path, data, size);
            auto dumpPacket = [&](const char *why, size_t from, size_t wantBytes) {
                const size_t begin = from & ~size_t(15u);
                const size_t limit = std::min<size_t>(size, begin + wantBytes);
                std::fprintf(stderr, "[GS-pkt] %s: path=%u size=%u from=%zu\n", why, path, size, begin);
                for (size_t off = begin; off < limit; off += 16u)
                {
                    std::fprintf(stderr, "[GS-pkt] %04zx:", off);
                    for (size_t i = 0u; i < 16u && off + i < limit; ++i)
                        std::fprintf(stderr, " %02x", data[off + i]);
                    std::fputc('\n', stderr);
                }
            };
            if (m_dumpPkt && !m_dumpPktDone)
            {
                m_dumpPktDone = true;
                m_dumpPkt = false;
                dumpPacket("setup carrying the first 0x3ff0 BITBLTBUF", 0u, 320u);
            }
            if (m_dumpBad && !m_dumpBadDone)
            {
                m_dumpBadDone = true;
                m_dumpBad = false;
                // The tag header sits 16 bytes before the payload; give 80 bytes of lead-in so the preceding
                // tag/payload boundary is visible too.
                dumpPacket("IMAGE tag with NLOOP=0x7FFF (PCSX2 has none)", m_dumpBadAt >= 80u ? m_dumpBadAt - 80u : 0u,
                           400u);
                // And the two preceding packets in full: the bad state was inherited from them.
                for (uint32_t back = 1u; back <= 2u; ++back)
                {
                    const uint32_t slot = (m_prevPktSlot + 2u - back) % 2u;
                    std::fprintf(stderr, "[GS-pkt] packet -%u: path=%u size=%u\n", back, m_prevPktPath[slot],
                                 m_prevPktSize[slot]);
                    const size_t prevLimit = std::min<size_t>(m_prevPktSize[slot], sizeof(m_prevPkt[0]));
                    for (size_t off = 0u; off < prevLimit; off += 16u)
                    {
                        std::fprintf(stderr, "[GS-pkt] -%u %04zx:", back, off);
                        for (size_t i = 0u; i < 16u && off + i < prevLimit; ++i)
                            std::fprintf(stderr, " %02x", m_prevPkt[slot][off + i]);
                        std::fputc('\n', stderr);
                    }
                }
                dumpPacketHistory();
            }
            // PS2X_DBP_STATS=1: the per-destination accounting observeGIF fed, reported on an exponential byte
            // threshold (16 KiB, 32 KiB, 64 KiB, 128 KiB, 256 KiB, 512 KiB, 1 MiB ...) rather than per packet, so the
            // probe cannot decide the boot race (session 58l) and still reports in a run that stalls after a few
            // hundred KiB.
            if (m_rawEnabled)
            {
                m_rawBytes += size;
                if (m_rawPrints < 12u && m_rawBytes >= (static_cast<uint64_t>(16384) << m_rawPrints))
                {
                    ++m_rawPrints;
                    printRawDbpStats("threshold");
                }
            }
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
            // Patches are only queued when one of the PS2X_FORCE_* experiment flags is
            // set, so an empty vector means "no experiment active".
            if (!m_scisPatch.empty())
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
                recordPacket(path, size, true);
                gs.gif_transfer(path, buf.data(), size);
                recordPacket(path, size, false);
                fixCt24Alpha();
                logSamplerState();
                dumpT4TextureOnce();
                followContextFrame();
                watchPage();
            }
            else
            {
                m_scisPatch.clear();
                recordPacket(path, size, true);
                gs.gif_transfer(path, data, size);
                recordPacket(path, size, false);
                fixCt24Alpha();
                logSamplerState();
                dumpT4TextureOnce();
                followContextFrame();
                watchPage();
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
                        // Session 54: what is actually stored at the T8 texture and its
                        // CLUT, and what colour+alpha does index -> CLUT yield?  The
                        // sprites use TBP0 0x3FF000 / TBW=1 / PSM=T8 / CBP 0x3FF400.
                        if (texDumpEnabled() && s0 == 0x100000001ull)
                        {
                            static uint32_t s_texPrints = 0u;
                            if (s_texPrints < 4u)
                            {
                                gs.flush();
                                const uint8_t *mapped =
                                    static_cast<const uint8_t *>(gs.map_vram_read(0x3FF000u, 0x800u));
                                if (mapped != nullptr)
                                {
                                    ++s_texPrints;
                                    uint8_t buf[0x800];
                                    std::memcpy(buf, mapped, sizeof(buf));
                                    constexpr uint32_t kClutOff = 0x400u; // 0x3FF400 - 0x3FF000
                                    auto wordAt = [&](uint32_t off) {
                                        uint32_t v = 0u;
                                        std::memcpy(&v, buf + off, sizeof(v));
                                        return v;
                                    };
                                    auto alphaOf = [&](uint32_t idx) {
                                        return (wordAt(kClutOff + (idx & 0xFFu) * 4u) >> 24) & 0xFFu;
                                    };
                                    // CLUT extent: how many of the 256 entries are populated?
                                    uint32_t clutNz = 0u, clutLo = 256u, clutHi = 0u;
                                    for (uint32_t i = 0u; i < 256u; ++i)
                                    {
                                        if (wordAt(kClutOff + i * 4u) != 0u)
                                        {
                                            ++clutNz;
                                            if (i < clutLo)
                                                clutLo = i;
                                            if (i > clutHi)
                                                clutHi = i;
                                        }
                                    }
                                    std::cerr << "[TEX] clutNz=" << clutNz
                                              << " range=" << (clutNz ? clutLo : 0u) << ".."
                                              << clutHi << " tex32=";
                                    for (uint32_t i = 0u; i < 32u; ++i)
                                        std::cerr << std::hex << (i ? ":" : "") << unsigned(buf[i]);
                                    // 8-bit indices: byte i
                                    std::cerr << std::dec << " | as8 idx/alpha:";
                                    for (uint32_t i = 0u; i < 10u; ++i)
                                        std::cerr << " " << std::hex << unsigned(buf[i]) << "/"
                                                  << alphaOf(buf[i]) << std::dec;
                                    // 16-bit indices: little-endian halfword at byte i*2
                                    std::cerr << " | as16 idx/alpha(lo,hi):";
                                    for (uint32_t i = 0u; i < 5u; ++i)
                                    {
                                        const uint32_t lo = buf[i * 2u];
                                        const uint32_t hi = buf[i * 2u + 1u];
                                        std::cerr << " " << std::hex << (lo | (hi << 8)) << "["
                                                  << alphaOf(lo) << "," << alphaOf(hi) << "]"
                                                  << std::dec;
                                    }
                                    std::cerr << std::endl;
                                }
                            }
                        }
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
                                  << " FRAME=0x" << fb << " fbp=" << (fb & 0x1FFull)
                                  << " fbw=" << ((fb >> 16) & 0x3Full) << " psm=" << ((fb >> 24) & 0x3Full)
                                  << " FMASK=0x" << (fb >> 32)
                                  << " ZBUF=0x" << zb << " zbp=" << (zb & 0x1FFull)
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
                        // PS2X_CLUT_DUMP=1: the palette and the first texels this context samples, read straight out of
                        // VRAM. Decides whether an indexed draw's alpha is genuinely 0 in the palette (the guest
                        // expects TEXA) or we are addressing the wrong entry.
                        if (std::getenv("PS2X_CLUT_DUMP") != nullptr)
                        {                            const uint64_t t0 = cc.tex0.bits;
                            const unsigned cbps = unsigned((t0 >> 37) & 0x3FFFu), cpsm = unsigned((t0 >> 51) & 0xFu);
                            const unsigned csm = unsigned((t0 >> 55) & 1u), csa = unsigned((t0 >> 56) & 0x1Fu);
                            const unsigned psms = unsigned((t0 >> 20) & 0x3Fu), tbps = unsigned(t0 & 0x3FFFu);
                            const unsigned tbws = unsigned((t0 >> 14) & 0x3Fu);
                            std::cerr << "\n  ctx" << ci << " CLUT cpsm=0x" << std::hex << cpsm << " base=0x" << cbps
                                      << " csm=" << std::dec << csm << " csa=" << csa << ":";
                            for (unsigned i = 0; i < 16u; ++i)
                                std::cerr << " " << std::hex << ReadVram(cpsm, cbps, 1u, i, 0u);
                            std::cerr << std::dec << "\n  ctx" << ci << " TEXELS psm=0x" << std::hex << psms
                                      << " tbp=0x" << tbps << std::dec << ":";
                            for (unsigned i = 0; i < 8u; ++i)
                                std::cerr << " " << std::hex << ReadVram(psms, tbps, tbws, i, 0u);
                            std::cerr << std::dec;
                        }
                        // PS2X_T4_DUMP=1: the 4-bit texture decoded with our own page tables, beside the palette's
                        // alpha per index - see dumpT4Texture.
                        if (std::getenv("PS2X_T4_DUMP") != nullptr)
                            dumpT4Texture(ci, cc.tex0.bits, rs.texa.bits);
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
