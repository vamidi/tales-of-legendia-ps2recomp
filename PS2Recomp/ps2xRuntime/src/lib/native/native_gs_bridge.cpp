// GSRasterBackend implemented on top of ps2x::native::Backend.
//
// This is the seam: gs_frontend.cpp keeps decoding the guest's GIF packets into
// GSPrimitiveBatch exactly as it does for GSCpuBackend, and this class turns
// those batches into host draws. Nothing above it changes; what is replaced is
// only the rasteriser and the texture sampler.
//
// Installed by the runtime only when PS2X_GS_NATIVE=1, so the working
// parallel-gs and software paths are untouched.

#include "native_gl_backend.h"

#include "native_backend.h"

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_texture_page_cache.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace ps2x::native;

    constexpr uint32_t kTextureSizeLimit = 1024u;

    bool psmIsIndexed(uint32_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T4 || psm == GS_PSM_T8H ||
               psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool psmIs16Bit(uint32_t psm)
    {
        return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    }

    class GsNativeBridge final : public GSRasterBackend
    {
    public:
        explicit GsNativeBridge(std::unique_ptr<Backend> backend) : m_backend(std::move(backend)) {}
        ~GsNativeBridge() override = default;

        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            m_vram = vram;
            m_vramSize = vramSize;
            m_frameOpen = false;
        }

        void Reset() override
        {
            for (auto &t : m_textures)
                m_backend->destroyTexture(t.second);
            m_textures.clear();
            m_frameOpen = false;
        }

        void Submit(const GSPrimitiveBatch &batch) override
        {
            if (!m_backend || batch.vertexCount == 0u)
                return;
            ++m_submits;
            m_verticesFed += batch.vertexCount;
            if (!m_frameOpen)
            {
                m_backend->beginFrame(kFrameWidth, kFrameHeight);
                m_frameOpen = true;
            }

            const auto &st = batch.state;
            const auto &ctx = st.context;

            DrawState ds;
            ds.textured = st.prim.tme && st.textureWidth > 1u && st.textureHeight > 1u;
            ds.blending = st.prim.abe;
            ds.smoothShading = st.prim.iip;
            ds.linearFilter = st.linearFilter;
            ds.textureWidth = float(st.textureWidth ? st.textureWidth : 1u);
            ds.textureHeight = float(st.textureHeight ? st.textureHeight : 1u);
            if (ds.textured)
            {
                ds.texture = textureFor(ctx.tex0);
                if (!ds.texture)
                    ds.textured = false; // decode failed: fall back to vertex colour
            }

            const float ofx = float(ctx.xyoffset.ofx >> 4);
            const float ofy = float(ctx.xyoffset.ofy >> 4);
            const float invW = ds.textureWidth > 0.0f ? 1.0f / ds.textureWidth : 0.0f;
            const float invH = ds.textureHeight > 0.0f ? 1.0f / ds.textureHeight : 0.0f;
            const float invQ = st.prim.fst ? 1.0f : 1.0f / (std::fabs(verticesQ(batch)) < 1e-6f
                                                               ? 1.0f
                                                               : std::fabs(verticesQ(batch)));

            const int32_t sx0 = int32_t(ctx.scissor.x0);
            const int32_t sy0 = int32_t(ctx.scissor.y0);
            const int32_t sx1 = int32_t(ctx.scissor.x1);
            const int32_t sy1 = int32_t(ctx.scissor.y1);
            m_backend->setScissor(sx0, sy0, sx1, sy1);

            Vertex out[6];
            for (uint8_t i = 0u; i < batch.vertexCount; ++i)
            {
                const GSVertex &v = batch.vertices[i];
                Vertex &o = out[i];
                o.x = v.x - ofx;
                o.y = v.y - ofy;
                o.z = float(v.z);
                o.w = 1.0f;
                if (st.prim.fst)
                {
                    o.u = (float(v.u) / 16.0f) * invW;
                    o.v = (float(v.v) / 16.0f) * invH;
                }
                else
                {
                    o.u = (v.s * invQ) * invW;
                    o.v = (v.t * invQ) * invH;
                }
                o.rgba = uint32_t(v.r) | (uint32_t(v.g) << 8) | (uint32_t(v.b) << 16) |
                         (uint32_t(v.a) << 24);
                o.fog = 0u;
            }

            if (batch.vertexCount == 2u)
            {
                // GS sprite: two opposite corners -> a quad (two triangles).
                const Vertex &a = out[0];
                const Vertex &c = out[1];
                Vertex tl = a, br = c;
                tl.x = a.x < c.x ? a.x : c.x;
                br.x = a.x < c.x ? c.x : a.x;
                tl.u = a.x < c.x ? a.u : c.u;
                br.u = a.x < c.x ? c.u : a.u;
                tl.y = a.y < c.y ? a.y : c.y;
                br.y = a.y < c.y ? c.y : a.y;
                tl.v = a.y < c.y ? a.v : c.v;
                br.v = a.y < c.y ? c.v : a.v;
                Vertex tr = tl, bl = br;
                tr.x = br.x; tr.u = br.u;
                bl.x = tl.x; bl.u = tl.u;
                const Vertex quad[6] = {tl, bl, br, tl, br, tr};
                m_backend->drawVertices(PrimitiveType::Triangles, quad, 6u, ds);
            }
            else if (batch.vertexCount == 3u)
            {
                m_backend->drawVertices(PrimitiveType::Triangles, out, 3u, ds);
            }
            else
            {
                m_backend->drawVertices(PrimitiveType::Triangles, out, batch.vertexCount, ds);
            }
        }

        bool UsesRawCommands() const override { return true; }

        // The guest's GIF traffic never reaches gs_frontend.cpp in this title (see
        // the [GS-parse]/[GS-kick] counters: both zero), so the backend parses the
        // raw stream itself, as parallel-gs does. Registers mirrored here are the
        // ones that affect rasterisation; image transfers are applied to VRAM so
        // textureFor() can read them back.
        void ProcessGIF(uint32_t path, const uint8_t *data, uint32_t size) override
        {
            static uint64_t s_calls = 0u;
            ++s_calls;
            if (!m_backend || data == nullptr || size < 16u || (size & 15u) != 0u)
            {
                if ((s_calls & 0x3FFu) == 0u)
                    std::fprintf(stderr, "[GSB] calls=%llu rejected (size=%u)\n",
                                 static_cast<unsigned long long>(s_calls), size);
                return;
            }
            if ((s_calls & 0x3FFu) == 0u)
                std::fprintf(stderr, "[GSB] calls=%llu regs=%llu subs=%llu verts=%llu\n",
                             static_cast<unsigned long long>(s_calls),
                             static_cast<unsigned long long>(m_regCount),
                             static_cast<unsigned long long>(m_submits),
                             static_cast<unsigned long long>(m_verticesFed));
            size_t offset = 0u;
            while (offset + 16u <= size)
            {
                uint64_t tagLo = 0u, tagHi = 0u;
                std::memcpy(&tagLo, data + offset, sizeof(tagLo));
                std::memcpy(&tagHi, data + offset + 8u, sizeof(tagHi));
                offset += 16u;

                const uint32_t nloop = uint32_t(tagLo & 0x7FFFu);
                const bool eop = ((tagLo >> 15) & 1u) != 0u;
                const bool pre = ((tagLo >> 46) & 1u) != 0u;
                const uint32_t primWord = uint32_t((tagLo >> 47) & 0x7FFu);
                const uint8_t flg = uint8_t((tagLo >> 58) & 0x3u);
                uint32_t nreg = uint32_t((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;

                if (pre)
                    applyPrim(primWord);

                if (flg == 0u)
                {
                    // Packed: one register per QW, A and D in the same 128 bits.
                    for (uint32_t i = 0u; i < nloop && offset + 16u <= size; ++i)
                    {
                        uint64_t a = 0u, d = 0u;
                        std::memcpy(&a, data + offset, sizeof(a));
                        std::memcpy(&d, data + offset + 8u, sizeof(d));
                        offset += 16u;
                        applyRegister(uint8_t(a & 0x7Fu), d);
                    }
                }
                else if (flg == 1u)
                {
                    // Reglist: nreg descriptors in the tag, then nloop data words.
                    for (uint32_t loop = 0u; loop < nloop; ++loop)
                    {
                        for (uint32_t r = 0u; r < nreg && offset + 8u <= size; ++r)
                        {
                            const uint8_t reg = uint8_t((tagHi >> (r * 4u)) & 0xFu);
                            if (reg == 0xEu) // A+D: next two words are the descriptor
                            {
                                if (offset + 16u > size)
                                    return;
                                uint64_t a = 0u, d = 0u;
                                std::memcpy(&a, data + offset, sizeof(a));
                                std::memcpy(&d, data + offset + 8u, sizeof(d));
                                offset += 16u;
                                applyRegister(uint8_t(a & 0x7Fu), d);
                            }
                            else
                            {
                                uint64_t d = 0u;
                                std::memcpy(&d, data + offset, sizeof(d));
                                offset += 8u;
                                applyRegister(reg, d);
                            }
                        }
                    }
                }
                else if (flg == 2u)
                {
                    // Image: payload follows, in QWs.
                    const uint32_t bytes = nloop * 16u;
                    if (offset + bytes > size)
                        return;
                    pushImage(data + offset, bytes);
                    offset += bytes;
                }
                (void)eop;
                (void)path;
            }
            flushImageIfComplete();
        }
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override
        {
            // Guest GIF traffic carries its own image data; the frontend already
            // applied it to VRAM, which textureFor() reads.
        }

        void Flush() override {}
        void TextureFlush() override
        {
            for (auto &t : m_textures)
                m_backend->destroyTexture(t.second);
            m_textures.clear();
        }
        void Sync(GSSyncReason) override {}

        PresentationFrame Present(const GSPresentationRequest &) override
        {
            PresentationFrame frame;
            if (!m_backend)
                return frame;
            {
                static uint32_t s_logged = 0u;
                if (s_logged < 6u)
                {
                    ++s_logged;
                    std::fprintf(stderr, "[GS-gl] presents=%u submits=%llu vertices=%llu\n",
                                 s_logged, static_cast<unsigned long long>(m_submits),
                                 static_cast<unsigned long long>(m_verticesFed));
                }
            }
            if (!m_frameOpen)
            {
                m_backend->beginFrame(kFrameWidth, kFrameHeight);
                m_frameOpen = true;
            }
            m_backend->endFrame();
            m_frameOpen = false;
            if (m_backend->readback(frame.pixels, frame.width, frame.height))
            {
                frame.aspectRatio = float(frame.width) / float(frame.height ? frame.height : 1u);
                dumpFrameIfRequested(frame);
            }
            return frame;
        }

        bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }

        // ---- raw GIF register handling -------------------------------------
        void applyPrim(uint32_t word)
        {
            m_primWord = word;
            m_prim.type = static_cast<GSPrimType>(word & 0x7u);
            m_prim.iip = ((word >> 3) & 1u) != 0u;
            m_prim.tme = ((word >> 4) & 1u) != 0u;
            m_prim.abe = ((word >> 6) & 1u) != 0u;
            m_prim.fst = ((word >> 8) & 1u) != 0u;
            m_prim.ctxt = uint8_t((word >> 9) & 1u);
        }

        void applyRegister(uint8_t reg, uint64_t d)
        {
            ++m_regCount;
            switch (reg)
            {
            case 0x00u: applyPrim(uint32_t(d & 0x7FFu)); break;
            case 0x01u: m_rgba = uint32_t(d & 0xFFFFFFFFu); m_q = 0.0f; break;
            case 0x02u:
                m_s = float(int32_t(d & 0xFFFFFFFFu)) / 4096.0f;
                m_t = float(int32_t((d >> 32) & 0xFFFFFFFFu)) / 4096.0f;
                break;
            case 0x03u:
                m_u = uint32_t(d & 0x3FFFu);
                m_v = uint32_t((d >> 16) & 0x3FFFu);
                break;
            case 0x04u: // XYZF2: same layout, fog in the high bits
            case 0x05u: // XYZ2
                m_x = float(d & 0xFFFFu) / 16.0f;
                m_y = float((d >> 16) & 0xFFFFu) / 16.0f;
                m_z = float((d >> 32) & 0xFFFFFFFFu);
                kick(m_prim.type == GS_PRIM_SPRITE ? 2u : 3u);
                break;
            case 0x06u: applyTex0(0u, d); break;
            case 0x07u: applyTex0(1u, d); break;
            case 0x18u: m_ctx[0].xyoffset.ofx = uint16_t(d & 0xFFFFu);
                        m_ctx[0].xyoffset.ofy = uint16_t((d >> 32) & 0xFFFFu); break;
            case 0x19u: m_ctx[1].xyoffset.ofx = uint16_t(d & 0xFFFFu);
                        m_ctx[1].xyoffset.ofy = uint16_t((d >> 32) & 0xFFFFu); break;
            case 0x40u: m_ctx[0].scissor.x0 = uint16_t(d & 0x7FFu);
                        m_ctx[0].scissor.x1 = uint16_t((d >> 16) & 0x7FFu);
                        m_ctx[0].scissor.y0 = uint16_t((d >> 32) & 0x7FFu);
                        m_ctx[0].scissor.y1 = uint16_t((d >> 48) & 0x7FFu); break;
            case 0x41u: m_ctx[1].scissor.x0 = uint16_t(d & 0x7FFu);
                        m_ctx[1].scissor.x1 = uint16_t((d >> 16) & 0x7FFu);
                        m_ctx[1].scissor.y0 = uint16_t((d >> 32) & 0x7FFu);
                        m_ctx[1].scissor.y1 = uint16_t((d >> 48) & 0x7FFu); break;
            case 0x42u: m_alpha = uint32_t(d & 0xFFFFFFFFu); break;
            case 0x47u: m_test = uint32_t(d & 0xFFFFFFFFu); break;
            case 0x50u: m_bitblt = d; m_imgBuffer.clear(); m_imgWritten = 0u; break;
            case 0x52u:
                m_trxreg = uint32_t(d & 0xFFFFFFFFu);
                m_trxregH = uint32_t((d >> 32) & 0xFFFFFFFFu);
                break;
            case 0x53u: m_trxdir = uint32_t(d & 0x3u); break;
            case 0x54u: pushImage(reinterpret_cast<const uint8_t *>(&d), 8u); break;
            default: break;
            }
        }

        void applyTex0(uint32_t ctxt, uint64_t d)
        {
            GSTex0Reg &t = m_tex0[ctxt & 1u];
            t.tbp0 = uint32_t(d & 0x3FFFu);
            t.tbw = uint8_t((d >> 14) & 0x3Fu);
            t.psm = uint8_t((d >> 20) & 0x3Fu);
            t.tw = uint8_t((d >> 26) & 0xFu);
            t.th = uint8_t((d >> 30) & 0xFu);
            t.tcc = uint8_t((d >> 34) & 0x1u);
            t.cbp = uint32_t((d >> 37) & 0x3FFFu);
            t.cpsm = uint8_t((d >> 51) & 0x3Fu);
            t.csm = uint8_t((d >> 55) & 0x1u);
            t.csa = uint8_t((d >> 56) & 0x1Fu);
        }

        void pushImage(const uint8_t *p, uint32_t n)
        {
            m_imgBuffer.insert(m_imgBuffer.end(), p, p + n);
        }

        void flushImageIfComplete()
        {
            // Host->local transfer: copy linearly to DBP*256. Crude, but enough for
            // textureFor() to read back what the guest uploaded.
            if (m_trxdir != 0u || m_trxreg == 0u || m_imgBuffer.empty() || m_vram == nullptr)
                return;
            const uint32_t w = m_trxreg & 0xFFFu;
            const uint32_t h = m_trxregH & 0xFFFu;
            const uint32_t want = w * h * 4u;
            if (want == 0u || m_imgBuffer.size() < want)
                return;
            const uint64_t dbp = (m_bitblt >> 32) & 0x3FFFu;
            const uint64_t base = dbp * 256ull;
            const uint32_t n = uint32_t(std::min<uint64_t>(want, m_vramSize > base ? m_vramSize - base : 0u));
            if (n)
                std::memcpy(m_vram + base, m_imgBuffer.data(), n);
            m_imgBuffer.clear();
            m_trxreg = 0u;
            m_trxregH = 0u;
        }

        void kick(uint32_t needed)
        {
            if (!m_backend)
                return;
            if (!m_frameOpen)
            {
                m_backend->beginFrame(kFrameWidth, kFrameHeight);
                m_frameOpen = true;
            }
            if (m_vtxCount < 3u)
            {
                GSVertex &v = m_vtx[m_vtxCount];
                v = GSVertex{};
                v.x = m_x;
                v.y = m_y;
                v.z = double(m_z);
                v.r = uint8_t(m_rgba & 0xFFu);
                v.g = uint8_t((m_rgba >> 8) & 0xFFu);
                v.b = uint8_t((m_rgba >> 16) & 0xFFu);
                v.a = uint8_t((m_rgba >> 24) & 0xFFu);
                v.u = uint16_t(m_u);
                v.v = uint16_t(m_v);
                v.s = m_s;
                v.t = m_t;
                v.q = 1.0f;
            }
            ++m_vtxCount;
            if (m_vtxCount < needed)
                return;

            GSPrimitiveBatch batch;
            batch.vertexCount = uint8_t(needed);
            for (uint32_t i = 0u; i < needed; ++i)
                batch.vertices[i] = m_vtx[i];
            batch.state.context = m_ctx[m_prim.ctxt & 1u];
            batch.state.prim = m_prim;
            batch.state.textureWidth = uint16_t(1u << (m_tex0[m_prim.ctxt & 1u].tw & 0xFu));
            batch.state.textureHeight = uint16_t(1u << (m_tex0[m_prim.ctxt & 1u].th & 0xFu));
            batch.state.linearFilter = false;
            Submit(batch);

            // Strip/fan continue from the previous edge; sprite and triangle reset.
            if (m_prim.type == GS_PRIM_TRISTRIP && m_vtxCount >= 3u)
            {
                m_vtx[0] = m_vtx[1];
                m_vtx[1] = m_vtx[2];
                m_vtxCount = 2u;
            }
            else
            {
                m_vtxCount = 0u;
            }
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0u; }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x,
                          uint32_t y) const override
        {
            if (m_vram == nullptr)
                return 0u;
            return GSMem::ReadTexture(m_pageCache, m_vram, psm, base, bw, x, y);
        }

        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override
        {
            // Not needed by the draw path: guest GIF traffic reaches VRAM through
            // the frontend, and this backend only reads it.
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            out.assign(m_vram, m_vram + m_vramSize);
        }

        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }

    private:
        static constexpr uint32_t kFrameWidth = 640u;
        static constexpr uint32_t kFrameHeight = 448u;

        static float verticesQ(const GSPrimitiveBatch &batch)
        {
            return batch.vertices[0].q != 0.0f ? batch.vertices[0].q : 1.0f;
        }

        // PS2X_GL_DUMP=1 writes the first few presented frames as binary PPM, so
        // what this backend actually produced can be inspected without a screen.
        void dumpFrameIfRequested(const PresentationFrame &frame)
        {
            static const bool enabled = std::getenv("PS2X_GL_DUMP") != nullptr;
            static uint32_t s_presented = 0u, s_written = 0u;
            if (!enabled || frame.pixels.empty() || frame.width == 0u || frame.height == 0u)
                return;
            ++s_presented;
            if (s_written >= 6u || (s_presented % 120u) != 1u)
                return;
            char path[512];
            std::snprintf(path, sizeof(path), "%s/ps2_gl_f%u.ppm",
                          std::getenv("TEMP") ? std::getenv("TEMP") : ".", s_written);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);
                for (uint32_t y = 0u; y < frame.height; ++y)
                {
                    const uint8_t *row =
                        frame.pixels.data() + static_cast<size_t>(y) * frame.width * 4u;
                    for (uint32_t x = 0u; x < frame.width; ++x)
                        std::fwrite(row + x * 4u, 1u, 3u, f); // RGB, drop alpha
                }
                std::fclose(f);
                std::fprintf(stderr, "[GS-gl] wrote %s (%ux%u)\n", path, frame.width,
                             frame.height);
                ++s_written;
            }
        }

        // GSTex0Reg is a plain struct (no union), so the cache key is packed from
        // the fields that actually change what gets sampled.
        static uint64_t textureKey(const GSTex0Reg &t)
        {
            return uint64_t(t.tbp0 & 0x3FFFu) |
                   (uint64_t(t.tbw & 0x3Fu) << 14) |
                   (uint64_t(t.psm & 0x3Fu) << 20) |
                   (uint64_t(t.tw & 0xFu) << 26) |
                   (uint64_t(t.th & 0xFu) << 30) |
                   (uint64_t(t.cbp & 0x3FFFu) << 34) |
                   (uint64_t(t.cpsm & 0x3Fu) << 48) |
                   (uint64_t(t.tcc & 0x1u) << 54) |
                   (uint64_t(t.csa & 0x1Fu) << 55);
        }

        // Decodes this TEX0's texture out of GS local memory into RGBA8, cached by
        // the register value. A decode failure returns an invalid id and the draw
        // falls back to the vertex colour rather than sampling nothing.
        TextureId textureFor(const GSTex0Reg &tex0)
        {
            const uint64_t key = textureKey(tex0);
            auto it = m_textures.find(key);
            if (it != m_textures.end())
                return it->second;

            const uint32_t w = uint32_t(1u) << (tex0.tw & 0xFu);
            const uint32_t h = uint32_t(1u) << (tex0.th & 0xFu);
            if (w == 0u || h == 0u || w > kTextureSizeLimit || h > kTextureSizeLimit ||
                m_vram == nullptr)
                return TextureId{};

            const uint32_t psm = tex0.psm & 0x3Fu;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u, 0u);
            for (uint32_t y = 0u; y < h; ++y)
            {
                for (uint32_t x = 0u; x < w; ++x)
                {
                    uint32_t texel = GSMem::ReadTexture(m_pageCache, m_vram, psm, tex0.tbp0,
                                                        tex0.tbw, x, y);
                    uint32_t r = 0u, g = 0u, b = 0u, a = 0xFFu;
                    if (psmIsIndexed(psm))
                    {
                        // T8 indices come back as-is; T4 is expanded by ReadTexture.
                        const uint32_t entry = clutEntry(tex0, texel & 0xFFu);
                        r = entry & 0xFFu;
                        g = (entry >> 8) & 0xFFu;
                        b = (entry >> 16) & 0xFFu;
                        a = (entry >> 24) & 0xFFu;
                    }
                    else if (psmIs16Bit(psm))
                    {
                        r = uint32_t((texel & 0x1Fu) << 3);
                        g = uint32_t(((texel >> 5) & 0x1Fu) << 3);
                        b = uint32_t(((texel >> 10) & 0x1Fu) << 3);
                        a = uint32_t(((texel >> 15) & 1u) * 0xFFu);
                    }
                    else
                    {
                        r = texel & 0xFFu;
                        g = (texel >> 8) & 0xFFu;
                        b = (texel >> 16) & 0xFFu;
                        a = (texel >> 24) & 0xFFu;
                    }
                    uint8_t *p = rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
                    p[0] = uint8_t(r);
                    p[1] = uint8_t(g);
                    p[2] = uint8_t(b);
                    p[3] = uint8_t(a);
                }
            }

            const TextureId id = m_backend->createTexture(rgba.data(), w, h);
            if (id)
                m_textures.emplace(key, id);
            return id;
        }

        // CT32 CLUT only for now: entry = the 32-bit word at cbp*256 + index*4.
        uint32_t clutEntry(const GSTex0Reg &tex0, uint32_t index) const
        {
            const uint64_t base = static_cast<uint64_t>(tex0.cbp) * 256ull +
                                  static_cast<uint64_t>(index) * 4ull;
            if (m_vram == nullptr || base + 4ull > m_vramSize)
                return 0u;
            uint32_t v = 0u;
            std::memcpy(&v, m_vram + base, sizeof(v));
            return v;
        }

        std::unique_ptr<Backend> m_backend;
        uint8_t *m_vram = nullptr;
        uint32_t m_vramSize = 0u;
        bool m_frameOpen = false;
        uint64_t m_submits = 0u, m_verticesFed = 0u, m_regCount = 0u;

        // Raw GIF parser state.
        uint32_t m_primWord = 0u;
        GSPrimReg m_prim{};
        GSContext m_ctx[2]{};
        GSTex0Reg m_tex0[2]{};
        uint32_t m_rgba = 0u;
        float m_q = 0.0f;
        float m_s = 0.0f, m_t = 0.0f;
        uint32_t m_u = 0u, m_v = 0u;
        float m_x = 0.0f, m_y = 0.0f, m_z = 0.0f;
        GSVertex m_vtx[3]{};
        uint32_t m_vtxCount = 0u;
        uint32_t m_alpha = 0u, m_test = 0u;
        uint64_t m_bitblt = 0u;
        uint32_t m_trxreg = 0u, m_trxregH = 0u, m_trxdir = 3u;
        std::vector<uint8_t> m_imgBuffer;
        uint32_t m_imgWritten = 0u;
        mutable GSMem::TexturePageCache m_pageCache;
        std::unordered_map<uint64_t, TextureId> m_textures;
    };
}

namespace ps2x::native
{
    std::unique_ptr<GSRasterBackend> createGlGsBackend(uint8_t *vram, uint32_t vramSize)
    {
        if (!glAvailable())
            return nullptr;
        auto backend = createGlBackend();
        if (!backend)
            return nullptr;
        auto bridge = std::make_unique<GsNativeBridge>(std::move(backend));
        bridge->Initialize(vram, vramSize);
        std::fprintf(stderr, "[GS-gl] native backend active (%s)\n", "gl");
        return bridge;
    }
}
