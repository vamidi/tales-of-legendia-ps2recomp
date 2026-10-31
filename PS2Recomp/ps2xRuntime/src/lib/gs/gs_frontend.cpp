#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include "runtime/ps2_memory.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

// Defined below, next to GS::ReadVram it samples through; called from vertexKick for every submitted draw.
static void ps2xProbe2dDraw(const GS &gs, const GSPrimitiveBatch &batch);

// ---------------------------------------------------------------------------
// TEMP diagnostic (session 25, first-frame triage): framebuffer BMP dump.
// The entry runner window has no GS image path, so the frames the guest
// presents are otherwise invisible. When the guest first presents (libgs swap
// stubs in Kernel/Stubs/GS.cpp, a DISPFB flip through the GIF path below, or
// a forced vblank tick in EeScheduler), snapshot the live VRAM through the
// raster backend and write the framebuffer described by DISPFB as a 24-bit
// BMP. Budget: 3 dumps per run; PS2_FB_DUMP=N overrides (0 disables).
// ---------------------------------------------------------------------------
namespace
{
    // TEMP diagnostic (session 30): which GIF A+D register ids the guest really
    // writes. Dumped alongside the framebuffer census.
    uint32_t g_gsAdHistogram[256] = {};

    void DumpGsAdHistogram(const char *tag)
    {
        uint32_t order[256];
        for (uint32_t i = 0u; i < 256u; ++i)
            order[i] = i;
        auto greater = [](uint32_t a, uint32_t b) { return g_gsAdHistogram[a] > g_gsAdHistogram[b]; };
        std::partial_sort(order, order + 16u, order + 256u, greater);
        std::fprintf(stderr, "[GS-ad] %s top:", tag);
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            if (g_gsAdHistogram[order[i]] == 0u)
                break;
            std::fprintf(stderr, " 0x%02X=%u", order[i], g_gsAdHistogram[order[i]]);
        }
        std::fprintf(stderr, "\n");
    }
}

namespace
{
    bool WriteBmp24(const std::filesystem::path &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &bgrBottomUp)
    {
        const uint32_t rowBytes = (w * 3u + 3u) & ~3u;
        const uint32_t pixelBytes = rowBytes * h;
        std::vector<uint8_t> header(54u, 0u);
        auto put32 = [](std::vector<uint8_t> &v, size_t at, uint32_t value) {
            v[at] = static_cast<uint8_t>(value);
            v[at + 1] = static_cast<uint8_t>(value >> 8);
            v[at + 2] = static_cast<uint8_t>(value >> 16);
            v[at + 3] = static_cast<uint8_t>(value >> 24);
        };
        auto put16 = [](std::vector<uint8_t> &v, size_t at, uint16_t value) {
            v[at] = static_cast<uint8_t>(value);
            v[at + 1] = static_cast<uint8_t>(value >> 8);
        };
        header[0] = 'B';
        header[1] = 'M';
        put32(header, 2, 14u + 40u + pixelBytes);
        put32(header, 10, 54u);
        put32(header, 14, 40u);
        put32(header, 18, w);
        put32(header, 22, h);
        put16(header, 26, 1u);
        put16(header, 28, 24u);
        put32(header, 34, pixelBytes);
        std::ofstream out(path, std::ios::binary);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char *>(header.data()), 54);
        std::vector<uint8_t> padRow(rowBytes, 0u);
        for (uint32_t y = 0u; y < h; ++y)
        {
            std::memcpy(padRow.data(), bgrBottomUp.data() + static_cast<size_t>(y) * w * 3u, static_cast<size_t>(w) * 3u);
            out.write(reinterpret_cast<const char *>(padRow.data()), rowBytes);
        }
        return out.good();
    }
}

bool Ps2DumpFramebufferBmp(GS &frontend, const GSRegisters &regs, const char *tag)
{
    static uint32_t s_budget = [] {
        const char *env = std::getenv("PS2_FB_DUMP");
        return env ? static_cast<uint32_t>(std::atoi(env)) : 8u;
    }();
    static uint32_t s_index = 0u;
    if (s_budget == 0u)
        return false;
    --s_budget;
    const uint32_t frameIndex = s_index++;

    // A scanout pass records the true display geometry (hostPresentation*)
    // even though the parallel backend's frame is GPU-only (no CPU pixels).
    frontend.latchHostPresentationFrame();
    const GSDebugSnapshot snap = frontend.getDebugSnapshot();

    frontend.refreshDisplaySnapshot();
    uint32_t vramSize = 0u;
    const uint8_t *vram = frontend.lockDisplaySnapshot(vramSize);
    if (!vram || vramSize < 0x40000u)
    {
        if (vram)
            frontend.unlockDisplaySnapshot();
        std::fprintf(stderr, "[FB-dump] %s: no VRAM snapshot (size=%u)\n", tag, vramSize);
        return false;
    }

    // PMODE.EN1 (bit 0) / EN2 (bit 1) enable read circuits 1 and 2. Prefer an
    // enabled circuit; fall back to circuit 1 when neither is enabled.
    const bool en1 = (regs.pmode & 0x1u) != 0u;
    const bool en2 = (regs.pmode & 0x2u) != 0u;
    const unsigned circuit = en1 ? 0u : (en2 ? 1u : 0u);
    const uint64_t dispfb = circuit != 0u ? regs.dispfb2 : regs.dispfb1;

    // VRAM census: is anything actually drawn? Scan the snapshot for non-zero
    // 32-bit words; extent + densest 64 KiB blocks tell whether the guest has
    // written primitives and where, independent of the DISPFB guess.
    uint64_t nzCount = 0u;
    uint64_t nzFirst = ~0ull, nzLast = 0u;
    uint32_t blockDense[64] = {};
    for (uint32_t off = 0u; off + 4u <= vramSize; off += 4u)
    {
        if (reinterpret_cast<const uint32_t *>(vram)[off >> 2] != 0u)
        {
            ++nzCount;
            if (nzFirst == ~0ull)
                nzFirst = off;
            nzLast = off;
            ++blockDense[off >> 16];
        }
    }
    uint32_t denseIdx[3] = {0u, 0u, 0u};
    // Session 41: the dense-block summary cannot answer the one question that
    // matters once parallel-gs is known to rasterise (14k prims, 23k copies):
    // is the *displayed* buffer non-empty, and is the *drawn* one?  Count each
    // buffer window explicitly.  FBP is in 8192-byte pages.
    auto nzWindow = [&](uint32_t fbp) -> uint64_t {
        const uint64_t base = static_cast<uint64_t>(fbp) * 8192ull;
        uint64_t n = 0u;
        for (uint64_t off = base; off + 4u <= vramSize && off < base + 0xB8000ull; off += 4u)
        {
            if (reinterpret_cast<const uint32_t *>(vram)[off >> 2] != 0u)
                ++n;
        }
        return n;
    };
    const uint64_t nzDisp2 = nzWindow(static_cast<uint32_t>(regs.dispfb2 & 0x1FFu));
    const uint64_t nzCtx0 = nzWindow(snap.ctx[0].frame.fbp);
    const uint64_t nzCtx1 = nzWindow(snap.ctx[1].frame.fbp);
    bool denseTaken[64] = {};
    for (uint32_t k = 0u; k < 3u; ++k)
    {
        uint32_t best = 0xFFFFFFFFu;
        for (uint32_t i = 0u; i < 64u; ++i)
        {
            if (!denseTaken[i] && (best == 0xFFFFFFFFu || blockDense[i] > blockDense[best]))
                best = i;
        }
        if (best == 0xFFFFFFFFu)
            break;
        denseIdx[k] = best;
        denseTaken[best] = true;
    }
    std::fprintf(stderr,
                 "[FB-dump] %s census: nzWords=%llu range=0x%llX..0x%llX dense=0x%X:%u 0x%X:%u 0x%X:%u "
                 "nzDisp2=%llu nzCtx0=%llu nzCtx1=%llu "
                 "pmode=0x%llX dispfb1=0x%llX dispfb2=0x%llX "
                 "ctx0frame=%u/%u/0x%02X ctx1frame=%u/%u/0x%02X "
                 "c0xy=0x%X/0x%X c0sc=%u,%u..%u,%u c0fbmsk=0x%X c0zmask=%d c0test=0x%llX "
                 "c1xy=0x%X/0x%X c1sc=%u,%u..%u,%u c1fbmsk=0x%X c1zmask=%d c1test=0x%llX\n",
                 tag,
                 static_cast<unsigned long long>(nzCount),
                 nzCount != 0u ? static_cast<unsigned long long>(nzFirst) : 0ull,
                 static_cast<unsigned long long>(nzLast),
                 denseIdx[0] * 0x10000u, blockDense[denseIdx[0]],
                 denseIdx[1] * 0x10000u, blockDense[denseIdx[1]],
                 denseIdx[2] * 0x10000u, blockDense[denseIdx[2]],
                 static_cast<unsigned long long>(nzDisp2),
                 static_cast<unsigned long long>(nzCtx0),
                 static_cast<unsigned long long>(nzCtx1),
                 static_cast<unsigned long long>(regs.pmode),
                 static_cast<unsigned long long>(regs.dispfb1),
                 static_cast<unsigned long long>(regs.dispfb2),
                 snap.ctx[0].frame.fbp, snap.ctx[0].frame.fbw, static_cast<unsigned>(snap.ctx[0].frame.psm),
                 snap.ctx[1].frame.fbp, snap.ctx[1].frame.fbw, static_cast<unsigned>(snap.ctx[1].frame.psm),
                 static_cast<unsigned>(snap.ctx[0].xyoffset.ofx), static_cast<unsigned>(snap.ctx[0].xyoffset.ofy),
                 static_cast<unsigned>(snap.ctx[0].scissor.x0), static_cast<unsigned>(snap.ctx[0].scissor.y0),
                 static_cast<unsigned>(snap.ctx[0].scissor.x1), static_cast<unsigned>(snap.ctx[0].scissor.y1),
                 snap.ctx[0].frame.fbmsk, static_cast<int>(snap.ctx[0].zbuf.zmask),
                 static_cast<unsigned long long>(snap.ctx[0].test),
                 static_cast<unsigned>(snap.ctx[1].xyoffset.ofx), static_cast<unsigned>(snap.ctx[1].xyoffset.ofy),
                 static_cast<unsigned>(snap.ctx[1].scissor.x0), static_cast<unsigned>(snap.ctx[1].scissor.y0),
                 static_cast<unsigned>(snap.ctx[1].scissor.x1), static_cast<unsigned>(snap.ctx[1].scissor.y1),
                 snap.ctx[1].frame.fbmsk, static_cast<int>(snap.ctx[1].zbuf.zmask),
                 static_cast<unsigned long long>(snap.ctx[1].test));
    DumpGsAdHistogram(tag);

    // DISPFB shares FRAME_1's packed layout: FBP[0:8] (2048-byte pages),
    // FBW[9:14] (64-pixel units), PSM[15:19], DBX[32:42], DBY[43:53].
    const uint32_t bp = static_cast<uint32_t>(dispfb & 0x1FFu);
    uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
    const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);

    uint32_t w = snap.hostPresentationWidth;
    uint32_t h = snap.hostPresentationHeight;
    if (w == 0u)
        w = fbw != 0u ? fbw * 64u : 640u;
    if (h == 0u)
        h = 448u;
    if (w > 1024u)
        w = 1024u;
    if (h > 1024u)
        h = 1024u;
    // renderRegion() addresses with the FBW stride below, so reading more
    // columns than that stride wraps each row into the next.  hostPresentationWidth
    // says 640 for this title while DISPFB FBW=8 gives a 512-pixel stride, and the
    // guest's own scissor (0,0..511,447 from 0x1bf000001ff0000) confirms 512 is the
    // real width - so a 640-wide read smeared every row by 128 columns.  That is
    // why dumped "frames" looked black/noise.  Never read past the stride.
    if (fbw != 0u && w > fbw * 64u)
        w = fbw * 64u;
    bool fbwGuessed = false;
    if (fbw == 0u)
    {
        fbw = (w + 63u) / 64u;
        fbwGuessed = true;
    }

    // GSMem readers take the raw packed register fields exactly as DISPFB
    // stores them (bp = FBP, bw = FBW) and apply the GS VRAM block
    // addressing. Pixel words are R[0:7] G[8:15] B[16:23] A[24:31]
    // (same convention as gs_cpu_backend texel reads).
    uint8_t *vramMut = const_cast<uint8_t *>(vram);
    auto renderRegion = [&](uint32_t regionBp, uint32_t regionFbw, uint32_t regionPsm,
                            uint32_t regionW, uint32_t regionH) {
        std::vector<uint8_t> pixels(static_cast<size_t>(regionW) * regionH * 3u, 0u);
        for (uint32_t y = 0u; y < regionH; ++y)
        {
            // BMP rows are bottom-up.
            uint8_t *row = pixels.data() + static_cast<size_t>(regionH - 1u - y) * regionW * 3u;
            for (uint32_t x = 0u; x < regionW; ++x)
            {
                uint32_t c = 0u;
                switch (regionPsm & 0x3Fu)
                {
                case GS_PSM_CT24:
                    c = GSMem::ReadCT24(vramMut, regionBp, regionFbw, x, y);
                    break;
                case GS_PSM_CT16:
                    c = GSMem::ReadCT16(vramMut, regionBp, regionFbw, x, y);
                    break;
                case GS_PSM_CT16S:
                    c = GSMem::ReadCT16S(vramMut, regionBp, regionFbw, x, y);
                    break;
                case GS_PSM_CT32:
                default:
                    c = GSMem::ReadCT32(vramMut, regionBp, regionFbw, x, y);
                    break;
                }
                uint8_t r = 0u, g = 0u, b = 0u;
                if ((regionPsm & 0x3Fu) == GS_PSM_CT16 || (regionPsm & 0x3Fu) == GS_PSM_CT16S)
                {
                    r = static_cast<uint8_t>((c & 0x1Fu) << 3);
                    g = static_cast<uint8_t>(((c >> 5) & 0x1Fu) << 3);
                    b = static_cast<uint8_t>(((c >> 10) & 0x1Fu) << 3);
                }
                else
                {
                    r = static_cast<uint8_t>(c & 0xFFu);
                    g = static_cast<uint8_t>((c >> 8) & 0xFFu);
                    b = static_cast<uint8_t>((c >> 16) & 0xFFu);
                }
                row[x * 3u + 0u] = b;
                row[x * 3u + 1u] = g;
                row[x * 3u + 2u] = r;
            }
        }
        return pixels;
    };
    const std::vector<uint8_t> bgr = renderRegion(bp, fbw, psm, w, h);
    // Raw physical slab of the densest 3x64 KiB region, read as linear CT32
    // words: shows what actually lives in VRAM (uploaded textures vs a drawn
    // framebuffer) regardless of DISPFB/FRAME programming.
    const uint32_t slabW = 640u;
    const uint32_t slabWords = 3u * 0x4000u;
    const uint32_t slabH = slabWords / slabW;
    const uint64_t slabBaseWord = static_cast<uint64_t>(denseIdx[0]) * 0x4000u;
    const uint64_t vramWords = vramSize / 4u;
    const uint32_t *vram32 = reinterpret_cast<const uint32_t *>(vram);
    std::vector<uint8_t> slab(static_cast<size_t>(slabW) * slabH * 3u, 0u);
    for (uint32_t y = 0u; y < slabH; ++y)
    {
        uint8_t *row = slab.data() + static_cast<size_t>(slabH - 1u - y) * slabW * 3u;
        for (uint32_t x = 0u; x < slabW; ++x)
        {
            const uint64_t word = slabBaseWord + static_cast<uint64_t>(y) * slabW + x;
            const uint32_t c = word < vramWords ? vram32[word] : 0u;
            row[x * 3u + 0u] = static_cast<uint8_t>(c >> 16);
            row[x * 3u + 1u] = static_cast<uint8_t>(c >> 8);
            row[x * 3u + 2u] = static_cast<uint8_t>(c);
        }
    }
    frontend.unlockDisplaySnapshot();

    std::error_code ec;
    std::filesystem::path outDir;
    if (const char *dumpDir = std::getenv("PS2_DUMP_DIR"))
        outDir = dumpDir;
    else
    {
        std::error_code tempEc;
        outDir = std::filesystem::temp_directory_path(tempEc);
        if (tempEc)
            outDir = std::filesystem::current_path(tempEc);
        outDir /= "ps2_diag";
    }
    std::filesystem::create_directories(outDir, ec);
    const auto framePath = outDir / ("gs_frame" + std::to_string(frameIndex) + "_" + tag + ".bmp");
    if (!WriteBmp24(framePath, w, h, bgr))
    {
        std::fprintf(stderr, "[FB-dump] %s: cannot write %s\n", tag, framePath.string().c_str());
        return false;
    }
    // Also render the context-1 draw framebuffer (FRAME_1) so the displayed
    // buffer and the buffer the guest actually renders into can be compared.
    {
        const auto &drawFrame = snap.ctx[0].frame;
        if (drawFrame.fbw != 0u && drawFrame.fbp != bp)
        {
            const uint32_t drawW = std::min<uint32_t>(drawFrame.fbw * 64u, 1024u);
            const std::vector<uint8_t> drawBgr =
                renderRegion(drawFrame.fbp, drawFrame.fbw, drawFrame.psm, drawW, h);
            const auto drawPath = outDir / ("gs_draw" + std::to_string(frameIndex) + "_" + tag + ".bmp");
            if (WriteBmp24(drawPath, drawW, h, drawBgr))
                std::fprintf(stderr, "[FB-dump] drew %s (ctx1 fbp=%u fbw=%u psm=%u)\n",
                             drawPath.string().c_str(), drawFrame.fbp, drawFrame.fbw,
                             static_cast<unsigned>(drawFrame.psm));
        }
    }
    const auto slabPath = outDir / ("gs_slab" + std::to_string(frameIndex) + "_" + tag + ".bmp");
    if (!WriteBmp24(slabPath, slabW, slabH, slab))
        std::fprintf(stderr, "[FB-dump] %s: cannot write %s\n", tag, slabPath.string().c_str());

    std::fprintf(stderr,
                 "[FB-dump] wrote %s + %s (%ux%u psm=%u fbw=%u%s bp=0x%X circuit=%u dispfb=0x%llX slab@0x%X tag=%s idx=%u)\n",
                 framePath.string().c_str(), slabPath.string().c_str(), w, h, psm, fbw, fbwGuessed ? "(guessed)" : "", bp, circuit,
                 static_cast<unsigned long long>(dispfb), denseIdx[0] * 0x10000u, tag, frameIndex);
    return true;
}

namespace
{
    static constexpr uint32_t kHostFrameWidth = 640u;

    GSPrimReg decodePrimRegister(uint64_t value)
    {
        GSPrimReg prim{};
        prim.type = static_cast<GSPrimType>(value & 0x7u);
        prim.iip = ((value >> 3) & 1u) != 0u;
        prim.tme = ((value >> 4) & 1u) != 0u;
        prim.fge = ((value >> 5) & 1u) != 0u;
        prim.abe = ((value >> 6) & 1u) != 0u;
        prim.aa1 = ((value >> 7) & 1u) != 0u;
        prim.fst = ((value >> 8) & 1u) != 0u;
        prim.ctxt = ((value >> 9) & 1u) != 0u;
        prim.fix = ((value >> 10) & 1u) != 0u;
        return prim;
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &)
                                    { return true; });
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
}


GS::GS()
    : m_backend(std::make_unique<GSCpuBackend>())
{
    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_localMemoryStorage = vram;
    m_localMemorySize = vramSize;
    m_privRegs = privRegs;
    if (!m_backend)
        m_backend = std::make_unique<GSCpuBackend>();
    m_backend->Initialize(vram, vramSize);
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_primRegister = {};
    m_prmodeRegister = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_fogR = 0;
    m_fogG = 0;
    m_fogB = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_scanmsk = 0u;
    m_dimx = 0u;
    m_dthe = 0u;
    m_colclamp = 0u;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);
        m_backend->Reset();
    }
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationGpuFrame.reset();
        m_hostPresentationTexture = 0u;
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }

    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    // Presentation/debug snapshots run outside m_stateMutex so the EE can keep
    // feeding the GS while a backend performs host-side conversion. Keep the
    // selected backend alive and unswappable for the duration of the call.
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (!m_backend)
        return;
    std::vector<uint8_t> snapshot;
    m_backend->Sync(GSSyncReason::DebugReadback);
    m_backend->SnapshotVram(snapshot);
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.swap(snapshot);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_ctx[0];
    snapshot.ctx[1] = m_ctx[1];
    snapshot.prim = m_prim;
    snapshot.texa = m_texa;
    snapshot.texclut = m_texclut;
    snapshot.scanmsk = m_scanmsk;
    snapshot.dimx = m_dimx;
    snapshot.dthe = m_dthe;
    snapshot.colclamp = m_colclamp;
    snapshot.bitbltbuf = m_bitbltbuf;
    snapshot.trxpos = m_trxpos;
    snapshot.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    snapshot.trxdir = transfer.direction;
    snapshot.transferX = transfer.x;
    snapshot.transferY = transfer.y;
    snapshot.transferTotalPixels = transfer.totalPixels;
    snapshot.transferCopiedPixels = transfer.copiedPixels;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        snapshot.hostPresentationWidth = m_hostPresentationWidth;
        snapshot.hostPresentationHeight = m_hostPresentationHeight;
        snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
        snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
        snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
        snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    }
    snapshot.localToHostPendingBytes = transfer.localToHostPendingBytes;
    if (m_backend && m_backend->UsesRawCommands())
        m_backend->ReadRegisterState(snapshot);
    return snapshot;
}

std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_prim;
    const uint32_t ci = m_prim.ctxt ? 1u : 0u;
    entry.frame = m_ctx[ci].frame;
    entry.zbuf = m_ctx[ci].zbuf;
    entry.tex0 = m_ctx[ci].tex0;
    entry.scissor = m_ctx[ci].scissor;
    entry.test = m_ctx[ci].test;
    entry.alpha = m_ctx[ci].alpha;
    entry.bitbltbuf = m_bitbltbuf;
    entry.trxpos = m_trxpos;
    entry.trxreg = m_trxreg;
    const GSTransferSnapshot transfer = m_backend ? m_backend->GetTransferSnapshot() : GSTransferSnapshot{};
    entry.trxdir = transfer.direction;
    entry.transferPixels = transfer.totalPixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_privRegs ? m_privRegs->vsyncTick.load(std::memory_order_acquire) : 0u;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_backend ? m_backend->GetTransferSnapshot().totalPixels : 0u;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

GSPresentationRequest GS::buildPresentationRequestUnlocked() const
{
    GSPresentationRequest request{};
    if (!m_privRegs)
        return request;
    request.pmode = m_privRegs->pmode;
    request.smode1 = m_privRegs->smode1;
    request.syncv = m_privRegs->syncv;
    request.field = (m_privRegs->csr.load() >> 13u) & 1u;
    request.smode2 = m_privRegs->smode2;
    request.dispfb1 = m_privRegs->dispfb1;
    request.display1 = m_privRegs->display1;
    request.dispfb2 = m_privRegs->dispfb2;
    request.display2 = m_privRegs->display2;
    request.bgcolor = m_privRegs->bgcolor;
    request.vsyncTick = m_privRegs->vsyncTick.load(std::memory_order_acquire);
    request.contextFrames[0] = m_ctx[0].frame;
    request.contextFrames[1] = m_ctx[1].frame;
    request.preferredSource = m_preferredDisplaySourceFrame;
    request.preferredDestFbp = m_preferredDisplayDestFbp;
    request.hasPreferredSource = m_hasPreferredDisplaySource;
    return request;
}

void GS::latchHostPresentationFrame()
{
    GSPresentationRequest request{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        if (!m_backend || !m_privRegs)
        {
            std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
            m_hostPresentationFrame.clear();
            m_hostPresentationGpuFrame.reset();
            m_hostPresentationTexture = 0u;
            m_hasHostPresentationFrame = false;
            m_hostPresentationWidth = m_hostPresentationHeight = 0u;
            return;
        }
        request = buildPresentationRequestUnlocked();
    }

    PresentationFrame frame{};
    {
        std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Presentation);
            frame = m_backend->Present(request);
        }
    }

    const bool hasFrame = static_cast<bool>(frame);
    const uint32_t displayFbp = frame.displayFbp;
    const uint32_t sourceFbp = frame.sourceFbp;
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const bool usedPreferred = frame.usedPreferred;
    {
        std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
        m_hostPresentationFrame = std::move(frame.pixels);
        m_hostPresentationGpuFrame = std::move(frame.gpu);
        m_hostPresentationTexture = frame.glTexture;
        m_hostPresentationAspectRatio = frame.aspectRatio;
        m_hostPresentationWidth = width;
        m_hostPresentationHeight = height;
        m_hostPresentationDisplayFbp = displayFbp;
        m_hostPresentationSourceFbp = sourceFbp;
        m_hostPresentationUsedPreferred = usedPreferred;
        m_hasHostPresentationFrame = hasFrame;
    }

    if (hasFrame)
    {
        std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
        recordPresentDebugEventUnlocked(displayFbp, sourceFbp, width, height, usedPreferred);
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

namespace
{
    // TEMP DIAGNOSTIC (native backend): how far do guest GIF packets get through the
    // frontend parser when a non-raw backend is installed?
    uint64_t g_glDbgPkts = 0u, g_glDbgKicks = 0u, g_glDbgDraws = 0u, g_glDbgEarly = 0u;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes, uint32_t path)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend && m_backend->UsesRawCommands())
    {
        if (data && sizeBytes && (sizeBytes & 15u) == 0u)
            m_backend->ProcessGIF(path, data, sizeBytes);
        return;
    }

    if (!data || sizeBytes < 16 || !m_backend)
        return;

    if ((++g_glDbgPkts & 0x3FFu) == 0u)
    {
        std::cerr << "[GS-parse] pkts=" << g_glDbgPkts << " kicks=" << g_glDbgKicks
                  << " kicksDrawing=" << g_glDbgDraws << " imageEarly=" << g_glDbgEarly
                  << std::endl;
    }

    if (tryProcessNativeImageUploadPacket(data, sizeBytes))
    {
        ++g_glDbgEarly;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;
    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegisterUnlocked(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
                imageBytes = sizeBytes - offset;
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_backend)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    if (m_backend->UsesRawCommands())
    {
        m_backend->ProcessGIF(3, data, sizeBytes);
        ++m_nativePackedGIFPacketCount;
        return true;
    }

    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
                                                {
        m_curQ = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegisterUnlocked(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true; });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    uploadImageNativeUnlocked(bitbltbuf, trxpos, trxreg, trxdir, data, sizeBytes);
}

void GS::uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes)
{
    if (!data || sizeBytes == 0 || !m_backend)
        return;

    writeRegisterUnlocked(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegisterUnlocked(GS_REG_TRXPOS, trxpos);
    writeRegisterUnlocked(GS_REG_TRXREG, trxreg);
    writeRegisterUnlocked(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNativeUnlocked(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0x3FFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0x3FFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        // Packed descriptor 0xF (A+D auto-data pair) is not modeled yet; the pair
        // words are consumed by the caller's data stream and dropped here.
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    writeRegisterUnlocked(regAddr, value);
}

void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    ++g_gsAdHistogram[regAddr];
    if (m_backend && m_backend->UsesRawCommands())
    {
        m_backend->WriteRegisterRaw(regAddr, value);
        return;
    }
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_primRegister = decodePrimRegister(value);
        if (m_prmodecont)
        {
            m_prim = m_primRegister;
        }
        else
        {
            // PRIM always selects the primitive topology. With AC=0, all
            // rendering attributes remain sourced from PRMODE.
            m_prim.type = m_primRegister.type;
        }
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        m_backend->LoadClut(t, m_texclut);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
    {
        m_prmodecont = (value & 1) != 0;
        const GSPrimType type = m_primRegister.type;
        m_prim = m_prmodecont ? m_primRegister : m_prmodeRegister;
        m_prim.type = type;
        break;
    }
    case GS_REG_PRMODE:
    {
        m_prmodeRegister = decodePrimRegister(value);
        if (!m_prmodecont)
        {
            const GSPrimType type = m_primRegister.type;
            m_prim = m_prmodeRegister;
            m_prim.type = type;
        }
        break;
    }
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);

        if (m_backend)
        {
            GSTransferCommand command{};
            command.bitbltbuf = m_bitbltbuf;
            command.trxpos = m_trxpos;
            command.trxreg = m_trxreg;
            command.direction = m_trxdir;
            m_backend->BeginTransfer(command);
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_FOGCOL:
        m_fogR = static_cast<uint8_t>(value & 0xFFu);
        m_fogG = static_cast<uint8_t>((value >> 8) & 0xFFu);
        m_fogB = static_cast<uint8_t>((value >> 16) & 0xFFu);
        break;
    case GS_REG_TEXFLUSH:
        if (m_backend)
            m_backend->TextureFlush();
        break;
    case GS_REG_SCANMSK:
        m_scanmsk = value;
        break;
    case GS_REG_DIMX:
        m_dimx = value;
        break;
    case GS_REG_DTHE:
        m_dthe = value;
        break;
    case GS_REG_COLCLAMP:
        m_colclamp = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        const int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_backend)
        {
            m_backend->Flush();
            m_backend->Sync(GSSyncReason::Finish);
        }
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
        {
            // TEMP diagnostic (session 30): who programs the display buffer?
            static uint32_t s_dispfb1Writes = 0u;
            const uint64_t flipsBefore = m_privRegs->displayFlipCount[0].load(std::memory_order_relaxed);
            m_privRegs->writeDisplayFramebuffer(0, value);
            if (s_dispfb1Writes < 12u)
            {
                ++s_dispfb1Writes;
                std::fprintf(stderr,
                             "[GS-w] DISPFB1 val=0x%llX fbp=%llu fbw=%llu psm=%llu pmode=0x%llX "
                             "flips=%llu\n",
                             static_cast<unsigned long long>(value),
                             static_cast<unsigned long long>(value & 0x1FFu),
                             static_cast<unsigned long long>((value >> 9) & 0x3Fu),
                             static_cast<unsigned long long>((value >> 15) & 0x1Fu),
                             static_cast<unsigned long long>(m_privRegs->pmode),
                             static_cast<unsigned long long>(
                                 m_privRegs->displayFlipCount[0].load(std::memory_order_relaxed)));
            }
            if (flipsBefore == 0u && m_privRegs->displayFlipCount[0].load(std::memory_order_relaxed) != 0u)
                Ps2DumpFramebufferBmp(*this, *m_privRegs, "flip");
        }
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
        {
            // TEMP diagnostic (session 30): who programs the display buffer?
            static uint32_t s_dispfb2Writes = 0u;
            const uint64_t flipsBefore = m_privRegs->displayFlipCount[1].load(std::memory_order_relaxed);
            m_privRegs->writeDisplayFramebuffer(1, value);
            if (s_dispfb2Writes < 12u)
            {
                ++s_dispfb2Writes;
                std::fprintf(stderr,
                             "[GS-w] DISPFB2 val=0x%llX fbp=%llu fbw=%llu psm=%llu pmode=0x%llX "
                             "flips=%llu\n",
                             static_cast<unsigned long long>(value),
                             static_cast<unsigned long long>(value & 0x1FFu),
                             static_cast<unsigned long long>((value >> 9) & 0x3Fu),
                             static_cast<unsigned long long>((value >> 15) & 0x1Fu),
                             static_cast<unsigned long long>(m_privRegs->pmode),
                             static_cast<unsigned long long>(
                                 m_privRegs->displayFlipCount[1].load(std::memory_order_relaxed)));
            }
            if (flipsBefore == 0u && m_privRegs->displayFlipCount[1].load(std::memory_order_relaxed) != 0u)
                Ps2DumpFramebufferBmp(*this, *m_privRegs, "flip");
        }
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::vertexKick(bool drawing)
{
    ++g_glDbgKicks;
    if (drawing)
        ++g_glDbgDraws;
    if ((g_glDbgKicks & 0xFFFu) == 0u)
    {
        std::cerr << "[GS-kick] kicks=" << g_glDbgKicks << " kicksDrawing=" << g_glDbgDraws
                  << " vtxCount=" << m_vtxCount << std::endl;
    }
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (drawing && m_backend)
    {
        GSPrimitiveBatch batch = buildDrawBatch(needed);
        updatePreferredDisplaySourceForDraw(batch);
        ps2xProbe2dDraw(*this, batch);
        m_backend->Submit(batch);
        recordDrawDebugEventUnlocked(needed);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_backend)
        m_backend->UploadImage(data, sizeBytes);
}


bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend && m_backend->ClearFramebuffer(getDebugSnapshot().ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    const auto state = getDebugSnapshot();
    return m_backend && m_backend->ClearFramebuffer(state.ctx[state.prim.ctxt ? 1 : 0], rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ConsumeLocalToHostBytes(dst, maxBytes) : 0u;
}

void GS::setRasterBackend(std::unique_ptr<GSRasterBackend> backend)
{
    if (!backend)
        backend = std::make_unique<GSCpuBackend>();

    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    if (m_backend)
    {
        m_backend->Flush();
        m_backend->Sync(GSSyncReason::Reset);

        // The external 4 MiB GS allocation is the backend hand-off format.
        // This keeps hot backend replacement deterministic even when a future
        // GPU backend keeps a private/mirrored local-memory representation.
        if (m_localMemoryStorage && m_localMemorySize != 0u)
        {
            std::vector<uint8_t> localMemory;
            m_backend->SnapshotVram(localMemory);
            const size_t bytes = std::min<size_t>(localMemory.size(), m_localMemorySize);
            if (bytes != 0u)
                std::memcpy(m_localMemoryStorage, localMemory.data(), bytes);
        }
    }

    m_backend = std::move(backend);
    m_backend->Initialize(m_localMemoryStorage, m_localMemorySize);
}

// PS2X_2D_PROBE=1: the state of the first 2D draws and then every 512th, to see why one phase rasterises and
// another stays black. Prints exactly what a draw samples - texture, CLUT entry, vertex alpha, blend/test, scissor -
// so a working "NOW LOADING" glyph can be diffed against a logo screen that draws nothing. Every sprite at the black
// phase is prim=0x15E: textured (tme), alpha-blended (abe), fst - and with ABE an alpha of 0 makes a sprite
// invisible rather than wrong, which is the first hypothesis this has to confirm or kill.
static void ps2xProbe2dDraw(const GS &gs, const GSPrimitiveBatch &batch)
{
    static const bool enabled = std::getenv("PS2X_2D_PROBE") != nullptr;
    if (!enabled)
        return;
    static uint64_t s_draws = 0u;
    const uint64_t index = s_draws++;
    // PS2X_2D_PROBE=1 summary: per 512-draw *window*, so the loading phase and the black phase separate instead of
    // averaging into one cumulative ratio. A fade that stops advancing shows up as whole windows at blackColour=100%.
    static uint64_t s_black = 0u, s_textured = 0u;
    const GSDrawState &st0 = batch.state;
    const GSVertex &v00 = batch.vertices[0];
    const bool colourZero = (v00.r | v00.g | v00.b | v00.a) == 0u;
    if (colourZero)
        ++s_black;
    if (st0.prim.tme)
        ++s_textured;
    if (index != 0u && (index % 512u) == 0u)
    {
        std::fprintf(stderr,
                     "[2Dsum] draws %llu-%llu: blackColour=%llu (%.1f%%) textured=%llu last=(prim=%u tme=%u rgba=%02x%02x%02x%02x)\n",
                     static_cast<unsigned long long>(index - 511u), static_cast<unsigned long long>(index),
                     static_cast<unsigned long long>(s_black), 100.0 * static_cast<double>(s_black) / 512.0,
                     static_cast<unsigned long long>(s_textured), static_cast<unsigned>(st0.prim.type),
                     st0.prim.tme ? 1u : 0u, v00.r, v00.g, v00.b, v00.a);
        s_black = 0u;
        s_textured = 0u;
    }
    if (index >= 12u && (index % 512u) != 0u)
        return;

    const GSDrawState &st = batch.state;
    const GSContext &ctx = st.context;
    const GSTex0Reg &t = ctx.tex0;
    const GSVertex &v0 = batch.vertices[0];
    const uint32_t texel = gs.ReadVram(t.psm, t.tbp0, t.tbw, v0.u, v0.v);
    const bool indexed = t.psm == GS_PSM_T8 || t.psm == GS_PSM_T8H || t.psm == GS_PSM_T4 ||
                         t.psm == GS_PSM_T4HL || t.psm == GS_PSM_T4HH;
    const uint32_t clutX = indexed
                               ? ((t.csm == 0u) ? (texel & 0xFFu)
                                                : (static_cast<uint32_t>(t.csa) * 16u + (texel & 0xFFu)))
                               : 0u;
    const uint32_t clut = indexed ? gs.ReadVram(t.cpsm, t.cbp, 1u, clutX, 0u) : 0u;
    std::fprintf(stderr,
                 "[2D] #%llu prim=%u verts=%u tme=%u abe=%u iip=%u fst=%u ct=%u aa1=%u "
                 "v0=(%.2f,%.2f) rgba=%02x%02x%02x%02x uv=(%u,%u) "
                 "tex0{tbp=%x tbw=%u psm=%x tw=%u th=%u tcc=%u tfx=%u cbp=%x cpsm=%x csm=%u csa=%u} "
                 "texa{ta0=%02x aem=%u ta1=%02x} alpha=%08x test=%08x sc=%u,%u-%u,%u ofs=%u,%u texel=%08x clut=%08x\n",
                 static_cast<unsigned long long>(index), static_cast<unsigned>(st.prim.type), batch.vertexCount,
                 st.prim.tme ? 1u : 0u, st.prim.abe ? 1u : 0u, st.prim.iip ? 1u : 0u, st.prim.fst ? 1u : 0u,
                 st.prim.ctxt ? 1u : 0u, st.prim.aa1 ? 1u : 0u, v0.x, v0.y, v0.r, v0.g, v0.b, v0.a,
                 static_cast<unsigned>(v0.u), static_cast<unsigned>(v0.v), t.tbp0, static_cast<unsigned>(t.tbw),
                 static_cast<unsigned>(t.psm), static_cast<unsigned>(t.tw), static_cast<unsigned>(t.th),
                 static_cast<unsigned>(t.tcc), static_cast<unsigned>(t.tfx), t.cbp, static_cast<unsigned>(t.cpsm),
                 static_cast<unsigned>(t.csm), static_cast<unsigned>(t.csa), static_cast<unsigned>(st.texa.ta0),
                 st.texa.aem ? 1u : 0u, static_cast<unsigned>(st.texa.ta1),
                 static_cast<unsigned>(ctx.alpha & 0xFFFFFFFFull), static_cast<unsigned>(ctx.test & 0xFFFFFFFFull),
                 ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1, ctx.xyoffset.ofx, ctx.xyoffset.ofy,
                 texel, clut);
}

uint32_t GS::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->ReadVram(psm, base, bw, x, y) : 0u;
}

void GS::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_backend)
        m_backend->WriteVram(psm, base, bw, x, y, value);
}

GSPrimitiveBatch GS::buildDrawBatch(int vertexCount) const
{
    GSPrimitiveBatch batch{};
    batch.vertexCount = static_cast<uint8_t>(std::min(vertexCount, 3));
    for (int i = 0; i < batch.vertexCount; ++i)
        batch.vertices[static_cast<size_t>(i)] = m_vtxQueue[i];
    batch.state = currentDrawState(m_prim.ctxt ? 1 : 0);
    return batch;
}

GSDrawState GS::currentDrawState(int context) const
{
    const GSContext &ctx = m_ctx[(context != 0) ? 1 : 0];
    GSDrawState state{};
    state.context = ctx;
    state.prim = m_prim;
    state.texa = m_texa;
    // The raw TEXA word a native draw hands to the hook (GsDrawTarget::texa), rebuilt from the parsed fields:
    // TEXA defines only TA0, AEM and TA1, so this is the register bit for bit.
    state.texaRaw = static_cast<uint64_t>(m_texa.ta0) | (m_texa.aem ? (1ull << 15) : 0ull) |
                    (static_cast<uint64_t>(m_texa.ta1) << 32);
    state.texclut = m_texclut;
    state.pabe = m_pabe;
    state.scanmsk = m_scanmsk;
    state.dimx = m_dimx;
    state.dthe = m_dthe;
    state.colclamp = m_colclamp;
    state.fogR = m_fogR;
    state.fogG = m_fogG;
    state.fogB = m_fogB;
    state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(ctx.tex0.tw, 10u));
    state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(ctx.tex0.th, 10u));
    const uint8_t mmag = static_cast<uint8_t>((ctx.tex1 >> 5u) & 0x1u);
    const uint8_t mmin = static_cast<uint8_t>((ctx.tex1 >> 6u) & 0x7u);
    state.linearFilter = mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    return state;
}

void GS::queueNativeDraw(uint64_t group, int context)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_backend)
        return;
    // PS2X_GS_MARKER_LOG=1: the first markers that reach the rasteriser, with the registers they carry. The only
    // way to see the marker path fire once a producer exists - print the first ones, then stop.
    static const bool s_logMarkers = std::getenv("PS2X_GS_MARKER_LOG") != nullptr;
    if (s_logMarkers)
    {
        static uint32_t s_logged = 0u;
        if (s_logged < 8u)
        {
            const GSDrawState state = currentDrawState(context);
            ++s_logged;
            std::fprintf(stderr,
                         "[GS-marker] group=%llu ctx=%d tex0=%#llx alpha=%#llx test=%#llx scissor=%u,%u..%u,%u\n",
                         static_cast<unsigned long long>(group), (context != 0) ? 1 : 0,
                         static_cast<unsigned long long>(state.context.tex0.tbp0) |
                             (static_cast<unsigned long long>(state.context.tex0.psm) << 20),
                         static_cast<unsigned long long>(state.context.alpha),
                         static_cast<unsigned long long>(state.context.test), state.context.scissor.x0,
                         state.context.scissor.y0, state.context.scissor.x1, state.context.scissor.y1);
        }
    }
    // The rasteriser keeps the marker where it is: a backend that draws native groups itself (GSNativeBackend)
    // flushes the batch collected so far and calls the hook with this state; one that does not (CPU, parallel-gs)
    // inherits the empty default and draws nothing.
    m_backend->NativeDraw(group, currentDrawState(context));
}

void GS::updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSContext &ctx = state.context;
    if (m_hasPreferredDisplaySource && ctx.frame.fbp == m_preferredDisplayDestFbp)
        m_hasPreferredDisplaySource = false;
    if (state.prim.type != GS_PRIM_SPRITE || batch.vertexCount < 2u)
        return;

    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    const uint8_t alphaMode = static_cast<uint8_t>(ctx.alpha & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((ctx.alpha >> 32u) & 0xFFu);
    const bool displayCopy = state.prim.tme && state.prim.abe && state.prim.fst && state.prim.ctxt &&
                             ctx.frame.fbp != ctx.tex0.tbp0 && alphaMode == 0x64u &&
                             (alphaFix == 0x60u || alphaFix == 0x80u) &&
                             x0 <= 0 && y0 <= 0 && xEnd >= 639 && yEnd >= 447;
    if (displayCopy)
    {
        m_preferredDisplaySourceFrame = {ctx.tex0.tbp0, ctx.tex0.tbw, ctx.tex0.psm, 0u};
        m_preferredDisplayDestFbp = ctx.frame.fbp;
        m_hasPreferredDisplaySource = true;
    }
}

std::shared_ptr<GSGpuFrame> GS::getLatchedGpuFrame(uint32_t &width, uint32_t &height, float &aspectRatio) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hostPresentationGpuFrame)
        return {};
    width = m_hostPresentationWidth;
    height = m_hostPresentationHeight;
    aspectRatio = m_hostPresentationAspectRatio;
    return m_hostPresentationGpuFrame;
}

bool GS::latchedHostPresentationTexture(uint32_t &outTexture, uint32_t &outWidth, uint32_t &outHeight) const
{
    std::lock_guard<std::mutex> lock(m_presentationMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationTexture == 0u)
        return false;
    outTexture = m_hostPresentationTexture;
    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    return true;
}

void GS::shutdownBackend()
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    std::lock_guard<std::mutex> backendLock(m_backendLifetimeMutex);
    std::lock_guard<std::mutex> presentationLock(m_presentationMutex);
    m_hostPresentationGpuFrame.reset();
    m_hostPresentationFrame.clear();
    m_hostPresentationTexture = 0u;
    m_hasHostPresentationFrame = false;
    m_backend.reset();
}

uint64_t GS::getReadbackCount() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_backend ? m_backend->GetReadbackCount() : 0;
}
