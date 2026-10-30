#pragma once

// Native raster backend: a PS2-GS-shaped draw sink that is NOT the GS.
//
// The recompiled game builds GIF packets as usual; gs_frontend.cpp decodes them
// into GSPrimitiveBatch (vertices + GSDrawState) exactly as it does today for
// GSCpuBackend. This interface is where those batches go instead of into
// parallel-gs's Vulkan rasteriser.
//
// Design constraints (Nintendo Switch is a planned target):
//
//   * Nothing here names an API. No GL/Vulkan/deko3d types, handles are opaque
//     integers. A backend per API implements it: `gl` for desktop, `deko3d` for
//     Switch, both created through the factory functions at the bottom.
//   * Vertex and fragment stages only. No compute, no geometry, no tessellation,
//     no storage buffers the target might lack. Skinning is therefore either
//     done on the CPU when meshes are built, or in the vertex shader.
//   * Uniforms are passed as a plain struct (DrawState) that each backend packs
//     into whatever it needs; no shared buffer layouts across APIs.
//   * Textures are RGBA8 only - no compressed or float formats are required of a
//     backend, so a Switch texture pool and a desktop one behave the same.
//   * Clip space and depth range differ between APIs and are handled *inside*
//     each backend, never by callers. `backendsClipDepthZeroToOne()` exists only
//     so the shared vertex transform can be validated in tests.
//   * The core renderer owns no graphics thread. A backend may run one, but the
//     interface must be callable from a single thread and must not block on the
//     caller beyond bounded queueing.

#include <cstdint>
#include <memory>

namespace ps2x::native
{
    // Opaque, backend-owned ids. 0 is always invalid.
    struct TextureId
    {
        uint32_t value = 0u;
        explicit operator bool() const { return value != 0u; }
    };

    struct MeshId
    {
        uint32_t value = 0u;
        explicit operator bool() const { return value != 0u; }
    };

    // One vertex as the GS would have consumed it. Positions are already in
    // raster space (the frontend applies XYOFFSET and the scissor) and `w` is 1
    // for every primitive the GS emits. `u`/`v` are normalised texture
    // coordinates; they are only meaningful when DrawState::textured is set.
    //
    // Kept to 8 floats/ints so both GL (glVertexAttribPointer) and deko3d
    // (DkVertexAttrib) can describe it with one interleaved stream.
    struct Vertex
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
        uint32_t rgba = 0u; // R in bits 0-7, G 8-15, B 16-23, A 24-31
        uint32_t fog = 0u;  // reserved; zero today
    };
    static_assert(sizeof(Vertex) == 32u, "Vertex must stay 32 bytes (8 dwords)");

    enum class PrimitiveType : uint8_t
    {
        Points,
        Lines,
        LineStrip,
        Triangles,
        TriangleStrip,
        TriangleFan,
    };

    // Everything a draw needs that is not per-vertex. This mirrors the fields of
    // GSDrawState that actually affect rasterisation; it deliberately does not
    // contain GS registers the backend cannot act on.
    struct DrawState
    {
        bool textured = false;   // PRIM.TME
        bool blending = false;   // PRIM.ABE
        bool smoothShading = true; // PRIM.IIP
        bool colorClamp = true;  // COLCLAMP
        bool alphaTest = false;  // TEST.ATE
        uint8_t alphaRef = 0u;   // TEST.AREF
        uint8_t alphaFix = 0x80u; // ALPHA.FIX (0..128)
        uint8_t alphaA = 0u;     // ALPHA A/B/C/D selectors, 2 bits each
        uint8_t alphaB = 1u;
        uint8_t alphaC = 0u;
        uint8_t alphaD = 1u;
        float textureWidth = 1.0f;
        float textureHeight = 1.0f;
        bool linearFilter = false;
        TextureId texture{};
    };

    struct Caps
    {
        // Clip space: -1..1 (GL) or 0..1 (Vulkan/deko3d); and depth 0..1 vs
        // -1..1. Recorded so the shared vertex transform can be checked.
        bool clipDepthZeroToOne = false;
        // Whether the backend can render to an offscreen target (needed for the
        // bloom/glow passes; a backend that cannot simply returns false).
        bool renderTargets = false;
        uint32_t maxTextureSize = 1024u;
    };

    class Backend
    {
    public:
        virtual ~Backend() = default;

        [[nodiscard]] virtual const char *name() const = 0;
        [[nodiscard]] virtual Caps caps() const = 0;

        // Returns false when the API/device is unavailable; the caller then keeps
        // whatever backend it had (usually the software GS) rather than failing.
        virtual bool initialize() = 0;
        virtual void shutdown() = 0;

        // Textures. `rgba` is width*height*4 bytes, row 0 first. Re-uploading the
        // same id is expected and must not leak.
        virtual TextureId createTexture(const uint8_t *rgba, uint32_t width, uint32_t height) = 0;
        virtual void updateTexture(TextureId id, const uint8_t *rgba, uint32_t width,
                                   uint32_t height) = 0;
        virtual void destroyTexture(TextureId id) = 0;

        // Meshes. Uploaded once, drawn many times; `Vertex` layout above.
        virtual MeshId createMesh(const Vertex *vertices, uint32_t count) = 0;
        virtual void destroyMesh(MeshId id) = 0;

        // Frame. `beginFrame` clears/sets up; `endFrame` presents. A backend that
        // draws on its own thread must have queued everything by endFrame.
        virtual void beginFrame(uint32_t width, uint32_t height) = 0;
        virtual void setScissor(int32_t x0, int32_t y0, int32_t x1, int32_t y1) = 0;
        virtual void draw(PrimitiveType type, MeshId mesh, const DrawState &state) = 0;
        virtual void endFrame() = 0;
    };

    // Backends. Return nullptr when the platform/API is not built in.
    std::unique_ptr<Backend> createGlBackend();
    // std::unique_ptr<Backend> createDeko3dBackend(); // Switch, later
}
