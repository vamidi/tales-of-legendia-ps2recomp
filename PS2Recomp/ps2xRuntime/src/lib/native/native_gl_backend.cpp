// OpenGL raster backend for the GS seam (see native_gl_backend.h).
//
// Reads the GSPrimitiveBatch stream that gs_frontend.cpp already produces - the
// same stream GSCpuBackend rasterises - and draws it with GL 3.3 instead of
// emulating the GS. Fixed-function PS2 behaviour that the GS would apply
// (the blend equation, the CLUT expand) is reproduced in the shader or on the
// CPU when a texture is decoded, so what is bypassed is the *GS rasteriser*,
// not the game's data.
//
// GL entry points come from glfwGetProcAddress with a macro-generated table,
// mirroring gs_gl_interop.cpp. No glad, so the same table can be filled from
// EGL/libnx on a platform without GLFW.

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "native_backend.h"
#include "native_gl_backend.h"

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace
{
    // ---------------------------------------------------------------- GL loader
#define PS2X_GL_FN(ret, name, args)                     \
    using name##Proc = ret(*) args;                     \
    name##Proc gl_##name = nullptr
    struct GlTable
    {
        PS2X_GL_FN(void, GetIntegerv, (unsigned, int *));
        PS2X_GL_FN(const unsigned char *, GetString, (unsigned));
        PS2X_GL_FN(unsigned, CreateShader, (unsigned));
        PS2X_GL_FN(void, ShaderSource, (unsigned, int, const char *const *, const int *));
        PS2X_GL_FN(void, CompileShader, (unsigned));
        PS2X_GL_FN(void, GetShaderiv, (unsigned, unsigned, int *));
        PS2X_GL_FN(void, GetShaderInfoLog, (unsigned, int, int *, char *));
        PS2X_GL_FN(unsigned, CreateProgram, ());
        PS2X_GL_FN(void, AttachShader, (unsigned, unsigned));
        PS2X_GL_FN(void, LinkProgram, (unsigned));
        PS2X_GL_FN(void, GetProgramiv, (unsigned, unsigned, int *));
        PS2X_GL_FN(void, GetProgramInfoLog, (unsigned, int, int *, char *));
        PS2X_GL_FN(void, DeleteShader, (unsigned));
        PS2X_GL_FN(void, UseProgram, (unsigned));
        PS2X_GL_FN(int, GetUniformLocation, (unsigned, const char *));
        PS2X_GL_FN(void, Uniform1i, (int, int));
        PS2X_GL_FN(void, Uniform2f, (int, float, float));
        PS2X_GL_FN(void, GenBuffers, (int, unsigned *));
        PS2X_GL_FN(void, DeleteBuffers, (int, const unsigned *));
        PS2X_GL_FN(void, BindBuffer, (unsigned, unsigned));
        PS2X_GL_FN(void, BufferData, (unsigned, long long, const void *, unsigned));
        PS2X_GL_FN(void, GenVertexArrays, (int, unsigned *));
        PS2X_GL_FN(void, DeleteVertexArrays, (int, const unsigned *));
        PS2X_GL_FN(void, BindVertexArray, (unsigned));
        PS2X_GL_FN(void, EnableVertexAttribArray, (unsigned));
        PS2X_GL_FN(void, VertexAttribPointer, (unsigned, int, unsigned, unsigned char, int, const void *));
        PS2X_GL_FN(void, GenTextures, (int, unsigned *));
        PS2X_GL_FN(void, DeleteTextures, (int, const unsigned *));
        PS2X_GL_FN(void, BindTexture, (unsigned, unsigned));
        PS2X_GL_FN(void, ActiveTexture, (unsigned));
        PS2X_GL_FN(void, TexImage2D, (unsigned, int, int, int, int, int, unsigned, unsigned, const void *));
        PS2X_GL_FN(void, TexParameteri, (unsigned, unsigned, int));
        PS2X_GL_FN(void, PixelStorei, (unsigned, int));
        PS2X_GL_FN(void, Viewport, (int, int, int, int));
        PS2X_GL_FN(void, Scissor, (int, int, int, int));
        PS2X_GL_FN(void, Enable, (unsigned));
        PS2X_GL_FN(void, Disable, (unsigned));
        PS2X_GL_FN(void, BlendFunc, (unsigned, unsigned));
        PS2X_GL_FN(void, ClearColor, (float, float, float, float));
        PS2X_GL_FN(void, Clear, (unsigned));
        PS2X_GL_FN(void, DrawArrays, (unsigned, int, int));
        PS2X_GL_FN(void, ReadPixels, (int, int, int, int, unsigned, unsigned, void *));

        bool load()
        {
#define LOAD(name)                                                      \
    gl_##name = reinterpret_cast<name##Proc>(glfwGetProcAddress("gl" #name)); \
    if (!gl_##name)                                                     \
        return false
            LOAD(GetIntegerv); LOAD(GetString); LOAD(CreateShader); LOAD(ShaderSource);
            LOAD(CompileShader); LOAD(GetShaderiv); LOAD(GetShaderInfoLog); LOAD(CreateProgram);
            LOAD(AttachShader); LOAD(LinkProgram); LOAD(GetProgramiv); LOAD(GetProgramInfoLog);
            LOAD(DeleteShader); LOAD(UseProgram); LOAD(GetUniformLocation); LOAD(Uniform1i);
            LOAD(Uniform2f); LOAD(GenBuffers); LOAD(DeleteBuffers); LOAD(BindBuffer);
            LOAD(BufferData); LOAD(GenVertexArrays); LOAD(DeleteVertexArrays); LOAD(BindVertexArray);
            LOAD(EnableVertexAttribArray); LOAD(VertexAttribPointer); LOAD(GenTextures);
            LOAD(DeleteTextures); LOAD(BindTexture); LOAD(ActiveTexture); LOAD(TexImage2D);
            LOAD(TexParameteri); LOAD(PixelStorei); LOAD(Viewport); LOAD(Scissor); LOAD(Enable);
            LOAD(Disable); LOAD(BlendFunc); LOAD(ClearColor); LOAD(Clear); LOAD(DrawArrays);
            LOAD(ReadPixels);
#undef LOAD
            return true;
        }
    };
#undef PS2X_GL_FN

    GlTable &gl()
    {
        static GlTable table;
        return table;
    }

    // GL enums (avoid depending on a GL header being present).
    constexpr unsigned kGLArrayBuffer = 0x8892;
    constexpr unsigned kGLDynamicDraw = 0x88E8;
    constexpr unsigned kGLVertexShader = 0x8B31;
    constexpr unsigned kGLFragmentShader = 0x8B30;
    constexpr unsigned kGLCompileStatus = 0x8B81;
    constexpr unsigned kGLLinkStatus = 0x8B82;
    constexpr unsigned kGLTexture2D = 0x0DE1;
    constexpr unsigned kGLRGBA8 = 0x8058;
    constexpr unsigned kGLRGBA = 0x1908;
    constexpr unsigned kGLUnsignedByte = 0x1401;
    constexpr unsigned kGLFloat = 0x1406;
    constexpr unsigned kGLNearest = 0x2600;
    constexpr unsigned kGLLinear = 0x2601;
    constexpr unsigned kGLTextureMinFilter = 0x2801;
    constexpr unsigned kGLTextureMagFilter = 0x2800;
    constexpr unsigned kGLTexture0 = 0x84C0;
    constexpr unsigned kGLBlend = 0x0BE2;
    constexpr unsigned kGLSrcAlpha = 0x0302;
    constexpr unsigned kGLOneMinusSrcAlpha = 0x0303;
    constexpr unsigned kGLColorBufferBit = 0x4000;
    constexpr unsigned kGLTriangles = 0x0004;
    constexpr unsigned kGLLines = 0x0001;
    constexpr unsigned kGLPoints = 0x0000;
    constexpr unsigned kGLUnpackAlignment = 0x0CF5;
    constexpr unsigned kGLScissorTest = 0x0C11;

    const char *kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
uniform vec2 uViewport;
out vec2 vUV;
out vec4 vCol;
void main()
{
    // GS raster space is top-left, y down; NDC is bottom-left, y up.
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
    vCol = aCol;
}
)";

    const char *kFragmentSrc = R"(#version 330 core
in vec2 vUV;
in vec4 vCol;
uniform sampler2D uTex;
uniform int uTextured;
out vec4 oColor;
void main()
{
    vec4 c = vCol;
    if (uTextured == 1)
        c *= texture(uTex, vUV);
    oColor = c;
}
)";
}

namespace ps2x::native
{
    namespace
    {
        struct GlVertex
        {
            float x, y;
            float u, v;
            float r, g, b, a;
        };

        struct FrameDraw
        {
            PrimitiveType type = PrimitiveType::Triangles;
            uint32_t first = 0;
            uint32_t count = 0;
            TextureId texture{};
            bool textured = false;
            bool blending = false;
            bool linear = false;
            int32_t scissor[4] = {0, 0, -1, -1};
        };

        class GlBackend final : public Backend
        {
        public:
            const char *name() const override { return "gl"; }
            Caps caps() const override
            {
                Caps c;
                c.clipDepthZeroToOne = false; // GL: clip -1..1, depth -1..1
                c.renderTargets = false;      // not needed for the first cut
                c.maxTextureSize = 1024u;
                return c;
            }

            bool initialize() override
            {
                if (!gl().load())
                    return false;
                m_program = buildProgram();
                if (m_program == 0u)
                    return false;
                m_uViewport = gl().gl_GetUniformLocation(m_program, "uViewport");
                m_uTextured = gl().gl_GetUniformLocation(m_program, "uTextured");
                m_uTex = gl().gl_GetUniformLocation(m_program, "uTex");
                gl().gl_GenVertexArrays(1, &m_vao);
                gl().gl_GenBuffers(1, &m_vbo);
                gl().gl_BindVertexArray(m_vao);
                gl().gl_BindBuffer(kGLArrayBuffer, m_vbo);
                const int stride = sizeof(GlVertex);
                gl().gl_EnableVertexAttribArray(0);
                gl().gl_VertexAttribPointer(0, 2, kGLFloat, 0, stride, reinterpret_cast<const void *>(0));
                gl().gl_EnableVertexAttribArray(1);
                gl().gl_VertexAttribPointer(1, 2, kGLFloat, 0, stride, reinterpret_cast<const void *>(8));
                gl().gl_EnableVertexAttribArray(2);
                gl().gl_VertexAttribPointer(2, 4, kGLFloat, 0, stride, reinterpret_cast<const void *>(16));
                gl().gl_BindVertexArray(0);
                return true;
            }

            void shutdown() override
            {
                for (auto &t : m_textures)
                    gl().gl_DeleteTextures(1, &t.second);
                m_textures.clear();
                if (m_vbo) gl().gl_DeleteBuffers(1, &m_vbo);
                if (m_vao) gl().gl_DeleteVertexArrays(1, &m_vao);
                m_vbo = m_vao = 0u;
            }

            TextureId createTexture(const uint8_t *rgba, uint32_t w, uint32_t h) override
            {
                unsigned id = 0u;
                gl().gl_GenTextures(1, &id);
                gl().gl_BindTexture(kGLTexture2D, id);
                gl().gl_PixelStorei(kGLUnpackAlignment, 1);
                gl().gl_TexImage2D(kGLTexture2D, 0, int(kGLRGBA8), int(w), int(h), 0, kGLRGBA,
                                   kGLUnsignedByte, rgba);
                gl().gl_TexParameteri(kGLTexture2D, kGLTextureMinFilter, int(kGLNearest));
                gl().gl_TexParameteri(kGLTexture2D, kGLTextureMagFilter, int(kGLNearest));
                return TextureId{id};
            }

            void updateTexture(TextureId id, const uint8_t *rgba, uint32_t w, uint32_t h) override
            {
                if (!id) return;
                gl().gl_BindTexture(kGLTexture2D, id.value);
                gl().gl_PixelStorei(kGLUnpackAlignment, 1);
                gl().gl_TexImage2D(kGLTexture2D, 0, int(kGLRGBA8), int(w), int(h), 0, kGLRGBA,
                                   kGLUnsignedByte, rgba);
            }

            void destroyTexture(TextureId id) override
            {
                if (!id) return;
                gl().gl_DeleteTextures(1, &id.value);
            }

            MeshId createMesh(const Vertex *, uint32_t) override { return MeshId{}; }
            void destroyMesh(MeshId) override {}

            void beginFrame(uint32_t width, uint32_t height) override
            {
                m_width = width ? width : 1u;
                m_height = height ? height : 1u;
                m_vertices.clear();
                m_draws.clear();
                gl().gl_Viewport(0, 0, int(m_width), int(m_height));
                gl().gl_Disable(kGLScissorTest);
                gl().gl_ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                gl().gl_Clear(kGLColorBufferBit);
            }

            void setScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) override
            {
                // Recorded per draw; applied at replay because the frontend sets it
                // between submissions rather than as a state.
                m_pendingScissor[0] = x0;
                m_pendingScissor[1] = y0;
                m_pendingScissor[2] = x1;
                m_pendingScissor[3] = y1;
            }

            void draw(PrimitiveType, MeshId, const DrawState &) override
            {
                // Cached meshes are not used by the GS seam yet.
            }

            void drawVertices(PrimitiveType type, const Vertex *vertices, uint32_t count,
                              const DrawState &state) override
            {
                if (vertices == nullptr || count == 0u)
                    return;
                const uint32_t first = uint32_t(m_vertices.size());
                m_vertices.reserve(m_vertices.size() + count);
                for (uint32_t i = 0u; i < count; ++i)
                {
                    const Vertex &v = vertices[i];
                    GlVertex g;
                    g.x = v.x;
                    g.y = v.y;
                    g.u = v.u;
                    g.v = v.v;
                    g.r = float(v.rgba & 0xFFu) / 255.0f;
                    g.g = float((v.rgba >> 8) & 0xFFu) / 255.0f;
                    g.b = float((v.rgba >> 16) & 0xFFu) / 255.0f;
                    g.a = float((v.rgba >> 24) & 0xFFu) / 255.0f;
                    m_vertices.push_back(g);
                }
                FrameDraw d;
                d.type = type;
                d.first = first;
                d.count = count;
                d.texture = state.texture;
                d.textured = state.textured;
                d.blending = state.blending;
                d.linear = state.linearFilter;
                std::memcpy(d.scissor, m_pendingScissor, sizeof(d.scissor));
                m_draws.push_back(d);
            }

            // The bridge appends here; draw() then records the range.
            void pushVertex(const GlVertex &v) { m_vertices.push_back(v); }
            void resetRecorded() { m_recorded = 0u; }

            void endFrame() override
            {
                if (m_vertices.empty() || m_draws.empty())
                    return;
                gl().gl_UseProgram(m_program);
                gl().gl_Uniform2f(m_uViewport, float(m_width), float(m_height));
                gl().gl_Uniform1i(m_uTex, 0);
                gl().gl_ActiveTexture(kGLTexture0);
                gl().gl_BindVertexArray(m_vao);
                gl().gl_BindBuffer(kGLArrayBuffer, m_vbo);
                gl().gl_BufferData(kGLArrayBuffer,
                                   static_cast<long long>(m_vertices.size() * sizeof(GlVertex)),
                                   m_vertices.data(), kGLDynamicDraw);
                bool blendOn = false;
                bool scissorOn = false;
                for (const FrameDraw &d : m_draws)
                {
                    if (d.scissor[2] >= d.scissor[0] && d.scissor[3] >= d.scissor[1])
                    {
                        gl().gl_Enable(kGLScissorTest);
                        const int sy = int(m_height) - d.scissor[3] - 1;
                        gl().gl_Scissor(d.scissor[0], sy < 0 ? 0 : sy,
                                        d.scissor[2] - d.scissor[0] + 1,
                                        d.scissor[3] - d.scissor[1] + 1);
                        scissorOn = true;
                    }
                    else if (scissorOn)
                    {
                        gl().gl_Disable(kGLScissorTest);
                        scissorOn = false;
                    }
                    if (d.blending != blendOn)
                    {
                        if (d.blending)
                        {
                            gl().gl_Enable(kGLBlend);
                            gl().gl_BlendFunc(kGLSrcAlpha, kGLOneMinusSrcAlpha);
                        }
                        else
                        {
                            gl().gl_Disable(kGLBlend);
                        }
                        blendOn = d.blending;
                    }
                    gl().gl_Uniform1i(m_uTextured, d.textured ? 1 : 0);
                    if (d.textured && d.texture)
                        gl().gl_BindTexture(kGLTexture2D, d.texture.value);
                    gl().gl_DrawArrays(toGlPrimitive(d.type), int(d.first), int(d.count));
                }
                gl().gl_BindVertexArray(0);
            }

            bool readback(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override
            {
                width = m_width;
                height = m_height;
                rgba.assign(static_cast<size_t>(m_width) * m_height * 4u, 0u);
                gl().gl_PixelStorei(0x0D05 /*GL_PACK_ALIGNMENT*/, 1);
                gl().gl_ReadPixels(0, 0, int(m_width), int(m_height), kGLRGBA, kGLUnsignedByte,
                                   rgba.data());
                // GL origin is bottom-left; PresentationFrame wants row 0 at the top.
                const size_t rowBytes = static_cast<size_t>(m_width) * 4u;
                std::vector<uint8_t> tmp(rowBytes);
                for (uint32_t y = 0u; y < m_height / 2u; ++y)
                {
                    uint8_t *a = rgba.data() + static_cast<size_t>(y) * rowBytes;
                    uint8_t *b = rgba.data() + static_cast<size_t>(m_height - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), a, rowBytes);
                    std::memcpy(a, b, rowBytes);
                    std::memcpy(b, tmp.data(), rowBytes);
                }
                // GL gives RGBA; PresentationFrame expects RGBA too.
                return true;
            }

            // Exposed for the bridge's texture decode.
            TextureId cachedTexture(uint64_t key) const
            {
                auto it = m_textures.find(key);
                return it == m_textures.end() ? TextureId{} : TextureId{it->second};
            }
            void storeTexture(uint64_t key, unsigned id) { m_textures[key] = id; }
            void clearTextures()
            {
                for (auto &t : m_textures)
                    gl().gl_DeleteTextures(1, &t.second);
                m_textures.clear();
            }
            uint32_t width() const { return m_width; }
            uint32_t height() const { return m_height; }

        private:
            static unsigned toGlPrimitive(PrimitiveType t)
            {
                switch (t)
                {
                case PrimitiveType::Points: return kGLPoints;
                case PrimitiveType::Lines:
                case PrimitiveType::LineStrip: return kGLLines;
                default: return kGLTriangles;
                }
            }

            unsigned buildProgram()
            {
                const unsigned vs = compile(kGLVertexShader, kVertexSrc);
                const unsigned fs = compile(kGLFragmentShader, kFragmentSrc);
                if (!vs || !fs)
                    return 0u;
                const unsigned p = gl().gl_CreateProgram();
                gl().gl_AttachShader(p, vs);
                gl().gl_AttachShader(p, fs);
                gl().gl_LinkProgram(p);
                int ok = 0;
                gl().gl_GetProgramiv(p, kGLLinkStatus, &ok);
                gl().gl_DeleteShader(vs);
                gl().gl_DeleteShader(fs);
                return ok ? p : 0u;
            }

            unsigned compile(unsigned type, const char *src)
            {
                const unsigned s = gl().gl_CreateShader(type);
                gl().gl_ShaderSource(s, 1, &src, nullptr);
                gl().gl_CompileShader(s);
                int ok = 0;
                gl().gl_GetShaderiv(s, kGLCompileStatus, &ok);
                if (!ok)
                {
                    char log[512] = {};
                    gl().gl_GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
                    std::fprintf(stderr, "[GS-gl] shader compile failed: %s\n", log);
                    gl().gl_DeleteShader(s);
                    return 0u;
                }
                return s;
            }

            unsigned m_program = 0u, m_vao = 0u, m_vbo = 0u;
            int m_uViewport = -1, m_uTextured = -1, m_uTex = -1;
            uint32_t m_width = 1u, m_height = 1u;
            uint32_t m_recorded = 0u;
            std::vector<GlVertex> m_vertices;
            std::vector<FrameDraw> m_draws;
            int32_t m_pendingScissor[4] = {0, 0, -1, -1};
            std::unordered_map<uint64_t, unsigned> m_textures;
        };
    }

    bool glAvailable()
    {
        return glfwGetCurrentContext() != nullptr;
    }

    std::unique_ptr<Backend> createGlBackend()
    {
        auto b = std::make_unique<GlBackend>();
        if (!b->initialize())
            return nullptr;
        return b;
    }
}
