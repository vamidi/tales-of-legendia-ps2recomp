#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

// TEMP DIAGNOSTIC (native backend receive path).
static uint64_t g_gifDbgSubmit = 0u, g_gifDbgDrain = 0u, g_gifDbgFwd = 0u;

// Session 58u: cumulative path-2 GIF handoff counters. Six hops, in order:
//   0 VIF1 -> submitGifPacket      (ps2_vif1_interpreter.cpp)
//   1 PS2Memory::submitGifPacket   (the funnel)
//   2 GifArbiter::submit
//   3 GifArbiter::drain -> processFn
//   4 GS::processGIFPacket raw forward (gs_frontend.cpp)
//   5 ParallelBackend::ProcessGIF  (what the renderer actually parses)
// A LOSS shows up as a hop with fewer bytes than the one before it. INFLATION (hop N larger
// than N-1) is the signature of a synthesised tag: the VIF1 pending-image path wraps the
// continuation of a still-open guest tag in a fresh 16-byte GIFtag, which the backend then
// consumes as payload. Reported from the backend on a byte threshold, never per packet.
namespace ps2x::gifhop
{
    namespace
    {
        constexpr int kHopCount = 6;
        uint64_t g_hopPkts[kHopCount][4] = {};
        uint64_t g_hopBytes[kHopCount][4] = {};
        uint64_t g_synthTags = 0u;
        uint32_t g_hopPrints = 0u;
    }

    void count(int hop, uint32_t path, uint64_t bytes) noexcept
    {
        if (hop < 0 || hop >= kHopCount)
            return;
        const uint32_t p = path & 3u;
        ++g_hopPkts[hop][p];
        g_hopBytes[hop][p] += bytes;
    }

    void countSynthTag() noexcept
    {
        ++g_synthTags;
    }

    void reportIfDue() noexcept
    {
        // Drive the threshold off the backend's own byte count, every 8 MiB, at most 16 times.
        const uint64_t backendBytes = g_hopBytes[5][2];
        if (g_hopPrints >= 16u || backendBytes < (static_cast<uint64_t>(g_hopPrints + 1u) << 23))
            return;
        ++g_hopPrints;
        std::fprintf(stderr, "[GIF-hop] path2 backendBytes=%llu synthTags=%llu\n",
                     static_cast<unsigned long long>(backendBytes),
                     static_cast<unsigned long long>(g_synthTags));
        static const char *const kNames[kHopCount] = {
            "vif1->submit", "funnel", "arbiter.in", "arbiter.out", "frontend", "backend"};
        for (int h = 0; h < kHopCount; ++h)
        {
            std::fprintf(stderr, "[GIF-hop] %-13s pkts=%llu bytes=%llu\n", kNames[h],
                         static_cast<unsigned long long>(g_hopPkts[h][2]),
                         static_cast<unsigned long long>(g_hopBytes[h][2]));
        }
        const long long inflate = static_cast<long long>(g_hopBytes[5][2]) -
                                  static_cast<long long>(g_hopBytes[0][2]);
        std::fprintf(stderr,
                     "[GIF-hop] backend - vif1 = %+lld bytes (synthesis is counted inside hop 0, not after it)\n",
                     inflate);
        std::fprintf(stderr, "[GIF-hop] %s\n",
                     (g_synthTags == 0u)
                         ? "no synthesised tags: the stream reaches the renderer unmodified, so look for a real loss"
                         : "SYNTHETIC TAGS PRESENT: each prepends 16 B to a continuation whose guest tag is still"
                           " OPEN, so the renderer eats that header as payload");
    }
}

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if ((++g_gifDbgSubmit & 0x3FFu) == 0u)
        std::fprintf(stderr, "[GIF-arb] submit=%llu drain=%llu fwd=%llu (last size=%u)\n",
                     static_cast<unsigned long long>(g_gifDbgSubmit),
                     static_cast<unsigned long long>(g_gifDbgDrain),
                     static_cast<unsigned long long>(g_gifDbgFwd), sizeBytes);
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    ps2x::gifhop::count(2, static_cast<uint32_t>(pathId), sizeBytes);

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            ++g_gifDbgFwd;
            ps2x::gifhop::count(3, static_cast<uint32_t>(pkt.pathId), pkt.data.size());
            m_processFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    ++g_gifDbgDrain;
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
